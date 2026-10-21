#!/usr/bin/env python3
"""Builds docs/BENCHMARKS.md from bench/results/*.json. Every number in the tables is read
from a result file; the only hand-written text is bench/commentary.md, which is appended
verbatim and must only interpret what the tables show.
    python3 bench/report.py"""
import glob
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
RES = os.path.join(ROOT, "bench", "results")
OUT = os.path.join(ROOT, "docs", "BENCHMARKS.md")


def load(exp):
    return [json.load(open(p)) for p in sorted(glob.glob(os.path.join(RES, f"{exp}__*.json")))
            if not p.endswith((".loadgen.json", "invariants.json"))]


def errs(r):
    lg = r["loadgen"]
    five = sum(v for k, v in lg["status_counts"].items() if k.startswith("5") and k != "503")
    return f"{lg['connect_failures']}/{lg['timeouts']}/{five}"


def table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    out += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(out)


def ok_per_s(r):
    lg = r["loadgen"]
    return sum(v for k, v in lg["status_counts"].items() if k.startswith("2")) / lg["duration_s"]


def spread(r, key="ok_per_s"):
    """'median (min-max)' over the repeated runs, or just the value if the case ran once."""
    reps = r.get("repeats") or []
    v = [x[key] for x in reps]
    if len(v) < 2:
        return None
    return f"{min(v):,.0f}-{max(v):,.0f}"


def f(x, d=1):
    return f"{x:,.{d}f}"


def main():
    meta = json.load(open(os.path.join(RES, "_meta.json")))
    hw, ds, me = meta["hardware"], meta["dataset"], meta["method"]
    L = ["# Benchmarks (measured)", "",
         "Every number below was produced by `bench/run_bench.py` and read from `bench/results/*.json` by "
         "`bench/report.py`; nothing is estimated. Treat them as **this laptop**: a shared machine where the generator, server and "
         f"database compete for CPUs. {me.get('note', '')}. The *range over runs* columns show how much the same case varied between runs "
         "(in an earlier run of the same configuration the hot-SKU case differed by about 25%), so only differences larger than that range mean anything. "
         "Compare configurations against each other, not against other systems.", "",
         "## Environment", "",
         table(["item", "value"], [
             ["CPU", f"{hw['cpu_model']} ({hw['logical_cpus']} logical CPUs)"], ["Memory", f"{hw['memory_gb_total']} GB"],
             ["OS / kernel", f"{hw['os']} / {hw['kernel']}"], ["Where it ran", hw["environment"]], ["Compiler", hw["compiler"]],
             ["MySQL", f"{hw['mysql_version']}, InnoDB buffer pool {hw['innodb_buffer_pool_bytes'] / 2**20:.0f} MiB, "
                       f"`innodb_flush_log_at_trx_commit={hw['innodb_flush_log_at_trx_commit']}` (full durability, not relaxed)"],
             ["Dataset", f"{ds['products']:,} products, {ds['categories']} categories, {ds['units_per_bench_product']:,} units per bench product"],
             ["Per case", f"{me['warmup_s']} s warm-up (discarded) + {me['duration_s']} s measured"],
             ["Load generator", me["loadgen"]]]), "",
         "The load generator, server and MySQL all share the same CPUs, so server throughput is understated by the "
         "generator's own CPU use. Each request opens a new TCP connection (the server answers `Connection: close`), "
         "so the figures include connection setup.", ""]

    w = load("workers")
    if w:
        L += ["## 1. Worker-pool size (read-heavy browse mix, 64 client threads)", "",
              "`SF_WORKERS=1` is the single-threaded baseline: the same code with one worker thread (there is no separate non-threaded build).", "",
              table(["workers", "successful req/s (median run)", "range over runs", "p50 ms", "p95 ms", "p99 ms", "server CPU (cores)", "RSS MB", "conn fail / timeouts / other 5xx", "DB ms per request"],
                    [[r["server_env"]["SF_WORKERS"], f(ok_per_s(r), 0), spread(r) or "-", f(r["loadgen"]["latency_ms"]["p50"], 2),
                      f(r["loadgen"]["latency_ms"]["p95"], 2), f(r["loadgen"]["latency_ms"]["p99"], 2), r["server"]["avg_cpu_cores"],
                      r["server"]["max_rss_mb"], errs(r),
                      f(r["server_metrics_delta"]["db"]["time_us"] / 1000 / max(1, r["server_metrics_delta"]["requests_total"]), 2)] for r in w]), "",
              "![throughput](../bench/charts/workers_throughput.svg)", "![latency](../bench/charts/workers_latency.svg)", ""]
        mid = [r for r in w if r["server_env"]["SF_WORKERS"] == 8]
        if mid:
            ops = mid[0]["loadgen"]["by_operation"]
            L += ["Per-operation latency in the 8-worker run (this is what shows which request type costs the most):", "",
                  table(["operation", "requests", "p50 ms", "p95 ms", "p99 ms"],
                        [[k, v["count"], f(v["p50"], 2), f(v["p95"], 2), f(v["p99"], 2)] for k, v in ops.items()]), ""]
    c = load("concurrency")
    if c:
        L += ["## 2. Client concurrency (browse mix, 8 workers, queue capacity 64)", "",
              "`successful req/s` counts only 2xx responses. Requests shed with an instant `503` (queue full) are shown separately and are NOT throughput. "
              "Latency percentiles below include those fast rejections, so read them together with the rejected column.", "",
              table(["clients", "successful req/s (median run)", "range over runs", "503 shed/s", "p50 ms", "p99 ms", "max ms", "queue high-water", "rejected by overload (503)", "conn fail / timeouts / other 5xx"],
                    [[r["threads"], f(ok_per_s(r), 0), spread(r) or "-", f(r["loadgen"]["status_counts"].get("503", 0) / r["loadgen"]["duration_s"], 0), f(r["loadgen"]["latency_ms"]["p50"], 2),
                      f(r["loadgen"]["latency_ms"]["p99"], 2), f(r["loadgen"]["latency_ms"]["max"], 1), r["queue_high_water"],
                      r["server_metrics_delta"]["connections_rejected_overload"], errs(r)] for r in c]), "",
              "![throughput](../bench/charts/concurrency_throughput.svg)", "![latency](../bench/charts/concurrency_latency.svg)", ""]
    h = load("hot_row")
    if h:
        inv = os.path.join(RES, "hot_row__invariants.json")
        verdict = ("**all database invariants held after the run**" if os.path.exists(inv) and not json.load(open(inv))["violations"]
                   else "invariant check missing or FAILED (see bench/results/hot_row__invariants.json)")
        L += ["## 3. Hot-row contention: purchase cycles (add to cart, checkout, pay, cancel), 32 buyers, 16 workers", "",
              "Each cycle is four requests. Buyers pick a product at random from the first K products; K = 1 means every buyer wants the same SKU, "
              "so every checkout serialises on one `inventory` row. Stock is never exhausted (1,000,000 units), so this measures lock hand-off, not sell-out. "
              + verdict + ".", "",
              table(["distinct SKUs", "cycles/s (median run)", "cycles/s range over runs", "checkout p50 ms", "checkout p99 ms", "InnoDB row-lock waits", "avg lock wait ms", "deadlock retries", "server CPU (cores)"],
                    [[r["label"].replace("skus", "").lstrip("0"), f(r["loadgen"]["purchase_cycles"]["per_s"], 1),
                      spread(r, "cycles_per_s") or "-",
                      f(r["loadgen"]["by_operation"]["checkout"]["p50"], 1), f(r["loadgen"]["by_operation"]["checkout"]["p99"], 1),
                      r["innodb_delta"]["Innodb_row_lock_waits"],
                      f(r["innodb_delta"]["Innodb_row_lock_time"] / max(1, r["innodb_delta"]["Innodb_row_lock_waits"]), 1),
                      r["server_metrics_delta"]["orders"]["deadlock_retries"], r["server"]["avg_cpu_cores"]] for r in h]), "",
              "![cycles](../bench/charts/hot_row_throughput.svg)", "![latency](../bench/charts/hot_row_checkout_latency.svg)", ""]
    m = load("mixed")
    if m:
        r = m[0]
        lg = r["loadgen"]
        L += ["## 4. Mixed workload (80% browse, 20% purchase cycles; 64 clients, 16 workers)", "",
              table(["operation", "count", "p50 ms", "p95 ms", "p99 ms"],
                    [[k, v["count"], f(v["p50"], 2), f(v["p95"], 2), f(v["p99"], 2)] for k, v in lg["by_operation"].items()]), "",
              f"Total {f(lg['requests_per_s'], 0)} req/s; purchase cycles completed: {lg['purchase_cycles']['completed']} of {lg['purchase_cycles']['attempted']}.", ""]
    for p in sorted(glob.glob(os.path.join(RES, "pmp__*.json"))):
        d = json.load(open(p))
        L += [f"## 5. Where the server's threads spend their time (GDB sampling, {d['scenario']}, {d['hot_skus']} hot SKU(s))", "",
              f"{d['samples_taken']} stack samples of all threads while {d['client_threads']} clients ran against {d['server_workers']} workers. "
              "Each sample pauses the process, so use the proportions, not absolute throughput.", "",
              table(["thread state", "share of observations"], [[k, f"{v['percent_of_all_thread_observations']}%"] for k, v in d["thread_states"].items()]), ""]
    qp = os.path.join(RES, "query_plans.json")
    if os.path.exists(qp):
        q = json.load(open(qp))
        L += [f"## 6. Query plans on {q['dataset_products']:,} products ({q['runs_per_variant']} executions per variant after warm-up)", "",
              "Index A/B uses MySQL 8 *invisible indexes*: identical data and statement, the optimizer just cannot see the index. "
              "Full `EXPLAIN ANALYZE` text is in [DB_QUERY_PLANS.md](DB_QUERY_PLANS.md).", ""]
        for case in q["cases"]:
            base = case["variants"][0]["avg_ms"]
            L += [f"**{case['question']}**", "",
                  table(["variant", "avg ms", "p50 ms", "p99 ms", "vs first row"],
                        [[v["name"], f(v["avg_ms"], 3), f(v["p50_ms"], 3), f(v["p99_ms"], 3), f"{v['avg_ms'] / base:.1f}x" if base else "-"]
                         for v in case["variants"]]), "", f"![{case['id']}](../bench/charts/query_{case['id']}.svg)", ""]
        plans = ["# Query plans (EXPLAIN ANALYZE, verbatim)", "",
                 f"Dataset: {q['dataset_products']:,} products. Generated by `bench/query_plans.py`; see BENCHMARKS.md for timings.", ""]
        for case in q["cases"]:
            plans += [f"## {case['question']}", ""]
            for v in case["variants"]:
                plans += [f"### {v['name']}  (avg {v['avg_ms']} ms)", "", "```sql", v["sql"], "```", "```", v["plan"], "```", ""]
        open(os.path.join(ROOT, "docs", "DB_QUERY_PLANS.md"), "w", encoding="utf-8").write("\n".join(plans))
    com = os.path.join(ROOT, "bench", "commentary.md")
    if os.path.exists(com):
        L += ["---", "", open(com, encoding="utf-8").read()]
    L += ["", "## Reproduce", "", "```bash", "scripts/dev.sh python3 bench/run_bench.py        # builds the dataset, runs the experiments",
          "scripts/dev.sh python3 bench/query_plans.py", "scripts/dev.sh python3 bench/pmp.py --scenario buy --hot 1",
          "python3 bench/charts.py && python3 bench/report.py", "```", ""]
    open(OUT, "w", encoding="utf-8").write("\n".join(L))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
