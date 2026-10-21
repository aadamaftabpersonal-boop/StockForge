#!/usr/bin/env python3
"""Reproducible benchmark driver. Every number in docs/BENCHMARKS.md comes from a
JSON file this script wrote to bench/results/ (nothing is estimated or typed in).

    scripts/dev.sh python3 bench/run_bench.py            # all experiments (~10 min)
    scripts/dev.sh python3 bench/run_bench.py --quick    # short runs, for a smoke test
    scripts/dev.sh python3 bench/run_bench.py workers hot_row   # chosen experiments

Experiments
  workers      worker-pool size sweep, read-heavy browse load, fixed client concurrency
  concurrency  client concurrency sweep at a fixed pool size (saturation, overload shedding)
  hot_row      purchase cycles (cart -> checkout -> pay -> cancel) vs how many distinct
               products the buyers spread over: 1 hot SKU ... 256 SKUs
  mixed        80% browse / 20% purchase cycles
Query-plan study (indexed vs not, keyset vs OFFSET, FULLTEXT vs LIKE): bench/query_plans.py
"""
import argparse
import json
import os
import platform
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "integration"))
import dbtools  # noqa: E402

dbtools.TEST_DB = "stockforge_bench"                    # never touch the test or dev databases
import harness  # noqa: E402

BUILD = os.environ.get("BENCH_BUILD", "build/rel")
LOADGEN = os.path.join(ROOT, BUILD, "loadgen")
RESULTS = os.path.join(ROOT, "bench", "results")
N_PRODUCTS = int(os.environ.get("BENCH_PRODUCTS", "100000"))
CLK_TCK = os.sysconf("SC_CLK_TCK")

# Dataset: 6 categories, N_PRODUCTS products (names drawn from a small vocabulary so the
# full-text search has realistic selectivity), 1,000,000 units each so a long run never sells out.
BULK_SQL = f"""
SET SESSION cte_max_recursion_depth = {N_PRODUCTS + 10};
INSERT INTO products (sku, category_id, name, description, price_cents)
WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < {N_PRODUCTS})
SELECT CONCAT('BN-', LPAD(n, 6, '0')), 1 + (n % 6),
       CONCAT(ELT(1 + n % 8, 'Wireless','Compact','Premium','Ergonomic','Portable','Studio','Smart','Classic'), ' ',
              ELT(1 + (n DIV 8) % 8, 'Keyboard','Lamp','Charger','Headphones','Desk','Bag','Cable','Speaker'), ' ', n),
       CONCAT('Benchmark product ', n, ' with a reasonably long description to give the text index real work.'),
       500 + (n * 7919) % 90000
FROM seq;
INSERT INTO inventory (product_id, on_hand, reserved, low_stock_threshold)
  SELECT id, 1000000, 0, 5 FROM products WHERE sku LIKE 'BN-%';
INSERT INTO inventory_ledger (product_id, delta_on_hand, delta_reserved, reason)
  SELECT product_id, on_hand, 0, 'seed' FROM inventory WHERE on_hand = 1000000;
"""


def hardware():
    def sh(cmd):
        return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
    mysql = dbtools._mysql("SELECT VERSION(), @@innodb_buffer_pool_size, @@innodb_flush_log_at_trx_commit, "
                           "@@max_connections, @@innodb_lock_wait_timeout", None)[0]
    return {
        "cpu_model": sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2").strip(),
        "logical_cpus": os.cpu_count(),
        "memory_gb_total": round(int(sh("awk '/MemTotal/ {print $2}' /proc/meminfo")) / 1048576, 1),
        "kernel": platform.release(),
        "os": sh("grep PRETTY_NAME /etc/os-release | cut -d= -f2 | tr -d '\"'"),
        "environment": "Docker Desktop (WSL2) on Windows 11; app, load generator and MySQL share this machine",
        "mysql_version": mysql[0], "innodb_buffer_pool_bytes": int(mysql[1]),
        "innodb_flush_log_at_trx_commit": mysql[2], "mysql_max_connections": mysql[3],
        "innodb_lock_wait_timeout_s": mysql[4], "compiler": sh("gcc --version | head -1"),
    }


def setup_dataset():
    print(f"[setup] building benchmark database '{dbtools.TEST_DB}' with {N_PRODUCTS:,} products ...", flush=True)
    t = time.time()
    dbtools.reset_db()                                  # schema + the 48-product seed + app user grants
    dbtools._mysql("SET GLOBAL innodb_buffer_pool_size = 1073741824", None)     # 1 GiB: dataset fits in memory
    dbtools._mysql(BULK_SQL, dbtools.TEST_DB)
    dbtools._mysql("ANALYZE TABLE products, inventory, categories, orders", dbtools.TEST_DB)
    dbtools._ready = True
    n = dbtools.scalar("SELECT COUNT(*) FROM products")
    print(f"[setup] {n} products ready in {time.time() - t:.0f}s", flush=True)
    return int(n)


class Sampler(threading.Thread):
    """Samples the server's CPU time and resident memory while the load runs."""

    def __init__(self, pid):
        super().__init__(daemon=True)
        self.pid, self.stop_, self.samples = pid, threading.Event(), []

    def run(self):
        while not self.stop_.is_set():
            try:
                with open(f"/proc/{self.pid}/stat") as f:
                    parts = f.read().rsplit(")", 1)[1].split()
                ticks = int(parts[11]) + int(parts[12])                   # utime + stime
                rss = 0
                with open(f"/proc/{self.pid}/status") as f:
                    for ln in f:
                        if ln.startswith("VmRSS:"):
                            rss = int(ln.split()[1])
                self.samples.append((time.time(), ticks, rss))
            except OSError:
                return
            time.sleep(0.5)

    def summary(self):
        if len(self.samples) < 2:
            return {"avg_cpu_cores": None, "max_rss_mb": None}
        (t0, c0, _), (t1, c1, _) = self.samples[0], self.samples[-1]
        return {"avg_cpu_cores": round((c1 - c0) / CLK_TCK / (t1 - t0), 2),
                "max_rss_mb": round(max(s[2] for s in self.samples) / 1024, 1)}


def innodb_lock_stats():
    rows = dbtools._mysql("SHOW GLOBAL STATUS WHERE Variable_name IN ('Innodb_row_lock_waits','Innodb_row_lock_time',"
                          "'Innodb_buffer_pool_reads','Innodb_buffer_pool_read_requests')", None)
    return {k: int(v) for k, v in rows}


def numeric_delta(a, b):
    out = {}
    for k, v in b.items():
        if isinstance(v, dict):
            out[k] = numeric_delta(a.get(k, {}), v)
        elif isinstance(v, (int, float)):
            out[k] = v - a.get(k, 0)
    return out


def make_buyers(n):
    users = dbtools.make_users(n, tag="bench")
    path = os.path.join(RESULTS, "_users.txt")
    with open(path, "w") as f:
        for _, cookie, csrf in users:
            f.write(f"{cookie} {csrf}\n")
    return path


def ok_per_s(rec):
    lg = rec["loadgen"]
    return sum(v for k, v in lg["status_counts"].items() if k.startswith("2")) / lg["duration_s"]


def run_median(experiment, label, *args, repeats=1, **kw):
    """Runs a case `repeats` times (fresh server each time) and stores the MEDIAN run (by successful
    req/s) plus the spread of all runs, so run-to-run noise is visible instead of assumed away."""
    recs = [run_case(experiment, label, *args, rep=i, **kw) for i in range(repeats)]
    med = dict(sorted(recs, key=ok_per_s)[len(recs) // 2])
    med["repeats"] = [{"ok_per_s": round(ok_per_s(r), 1), "p99_ms": r["loadgen"]["latency_ms"]["p99"],
                       "cycles_per_s": r["loadgen"]["purchase_cycles"]["per_s"]} for r in recs]
    json.dump(med, open(os.path.join(RESULTS, f"{experiment}__{label}.json"), "w"), indent=1)
    spread = [r["ok_per_s"] for r in med["repeats"]]
    print(f"  -> median of {repeats}: {ok_per_s(med):,.0f} ok/s (range {min(spread):,.0f}-{max(spread):,.0f})", flush=True)
    return med


def run_case(experiment, label, server_env, threads, scenario, duration, warmup, extra=(), buyers=None, products=None, rep=0):
    srv = harness.Server(SF_EXPIRY_SWEEP_MS=5000, SF_SLOW_QUERY_MS=100, SF_RL_CHECKOUT_USER=1000000, **server_env)
    try:
        # touch every worker so connections exist before measurement
        for _ in range(max(8, int(server_env.get("SF_WORKERS", 8)) * 2)):
            harness.get(srv, "/api/v1/categories")
        before_m, before_i = harness.metrics(srv), innodb_lock_stats()
        os.makedirs(os.path.join(RESULTS, "runs"), exist_ok=True)
        out = os.path.join(RESULTS, "runs", f"{experiment}__{label}__r{rep}.loadgen.json")
        cmd = [LOADGEN, "--port", str(srv.port), "--threads", str(threads), "--duration", str(duration),
               "--warmup", str(warmup), "--scenario", scenario, "--max-product", str(48 + N_PRODUCTS), "--out", out] + list(extra)
        if buyers:
            cmd += ["--users", buyers, "--products", ",".join(map(str, products))]
        sampler = Sampler(srv.proc.pid)
        proc = subprocess.Popen(cmd)
        time.sleep(warmup)
        sampler.start()
        proc.wait()
        sampler.stop_.set()
        after_m, after_i = harness.metrics(srv), innodb_lock_stats()
        lg = json.load(open(out))
        rec = {
            "experiment": experiment, "label": label, "server_env": server_env, "threads": threads, "scenario": scenario,
            "loadgen": lg, "server": sampler.summary(),
            "server_metrics_delta": numeric_delta(before_m, after_m),
            "queue_high_water": after_m["queue"]["high_water"],
            "innodb_delta": {k: after_i[k] - before_i[k] for k in after_i},
        }
        json.dump(rec, open(os.path.join(RESULTS, "runs", f"{experiment}__{label}__r{rep}.json"), "w"), indent=1)
        l = lg["latency_ms"]
        print(f"  {experiment:<12} {label:<22} {lg['requests_per_s']:>9.0f} req/s   p50 {l['p50']:>7.2f}  p95 {l['p95']:>7.2f}  "
              f"p99 {l['p99']:>7.2f} ms   cpu {rec['server']['avg_cpu_cores']} cores  "
              f"errors: conn={lg['connect_failures']} timeout={lg['timeouts']} 5xx={sum(v for k, v in lg['status_counts'].items() if k.startswith('5'))}",
              flush=True)
        return rec
    finally:
        rc = srv.stop()
        if rc != 0:
            print(f"  !! server exit code {rc}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("experiments", nargs="*", default=["workers", "concurrency", "hot_row", "mixed"])
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--skip-setup", action="store_true", help="reuse the existing benchmark database")
    ap.add_argument("--repeats", type=int, default=3, help="runs per case; the median is reported (default 3, 1 with --quick)")
    a = ap.parse_args()
    dur, warm = (3, 1) if a.quick else (8, 2)
    reps = 1 if a.quick else a.repeats
    os.makedirs(RESULTS, exist_ok=True)

    if not a.skip_setup:
        setup_dataset()
    dbtools._ready = True
    meta = {"hardware": hardware(), "dataset": {"products": int(dbtools.scalar("SELECT COUNT(*) FROM products")),
            "categories": 6, "units_per_bench_product": 1000000},
            "method": {"duration_s": dur, "warmup_s": warm, "repeats": reps,
                       "loadgen": "closed-loop, one connection per request (server uses Connection: close)",
                       "note": f"each case run {reps}x with a fresh server; the median run is reported and the min-max range shown"}}
    json.dump(meta, open(os.path.join(RESULTS, "_meta.json"), "w"), indent=1)
    print(json.dumps(meta["hardware"], indent=1), flush=True)

    n_buyers = 256
    buyers = make_buyers(n_buyers)
    first_bench = 49
    products = list(range(first_bench, first_bench + 256))

    if "workers" in a.experiments:
        print("\n== workers: browse, 64 client threads ==", flush=True)
        for w in (1, 2, 4, 8, 16, 32):
            run_median("workers", f"w{w:02d}", {"SF_WORKERS": w, "SF_QUEUE_CAP": 256}, 64, "browse", dur, warm, repeats=reps)
    if "concurrency" in a.experiments:
        print("\n== concurrency: browse, 8 workers, queue cap 64 ==", flush=True)
        for t in (4, 8, 16, 32, 64, 128, 256):
            run_median("concurrency", f"c{t:03d}", {"SF_WORKERS": 8, "SF_QUEUE_CAP": 64}, t, "browse", dur, warm, repeats=reps)
    if "hot_row" in a.experiments:
        print("\n== hot_row: purchase cycles, 32 buyers, 16 workers ==", flush=True)
        for k in (1, 4, 16, 64, 256):
            run_median("hot_row", f"skus{k:03d}", {"SF_WORKERS": 16, "SF_QUEUE_CAP": 256}, 32, "buy", dur, warm,
                       extra=["--hot", str(k)], buyers=buyers, products=products, repeats=reps)
        bad = dbtools.invariant_violations()
        print(f"  invariants after hot_row: {'ALL HOLD' if not bad else bad}", flush=True)
        json.dump({"violations": bad}, open(os.path.join(RESULTS, "hot_row__invariants.json"), "w"))
    if "mixed" in a.experiments:
        print("\n== mixed: 80% browse / 20% purchase cycles, 64 threads, 16 workers ==", flush=True)
        run_median("mixed", "mixed", {"SF_WORKERS": 16, "SF_QUEUE_CAP": 256}, 64, "mixed", dur, warm,
                   extra=["--hot", "256"], buyers=buyers, products=products, repeats=reps)
        bad = dbtools.invariant_violations()
        print(f"  invariants after mixed: {'ALL HOLD' if not bad else bad}", flush=True)
        json.dump({"violations": bad}, open(os.path.join(RESULTS, "mixed__invariants.json"), "w"))


if __name__ == "__main__":
    main()
