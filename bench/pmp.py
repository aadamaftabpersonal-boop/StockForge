#!/usr/bin/env python3
"""Poor man's profiler: while a load test runs, repeatedly attach GDB to the live server,
dump every thread's stack, and classify what each worker is doing. Statistical, coarse
and intrusive (the process pauses for ~100 ms per sample), but it answers "is the server
waiting on MySQL, on a lock, on the client, or burning CPU?" without `perf`.

    scripts/dev.sh python3 bench/pmp.py --scenario buy --hot 1 --samples 25
Needs SYS_PTRACE (scripts/dev.sh adds it) and the benchmark database from run_bench.py.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "bench"))
import run_bench  # noqa: E402  (also points dbtools at the benchmark database)
from run_bench import dbtools, harness  # noqa: E402

RULES = [  # first match wins; patterns are searched in the thread's whole stack text
    ("idle: waiting for a connection (queue empty)", r"fdq_pop"),
    ("sweeper (sleeping between sweeps)", r"sweep_main"),
    ("signal thread", r"sigwait|signal_main"),
    ("acceptor", r"server_run|accept4"),
    ("waiting on MySQL (network round trip)", r"mysql_stmt_(execute|prepare|store_result|fetch)|mysql_query|mysql_commit|ma_net_read|ma_pvio|mthd_stmt"),
    ("Argon2 password hashing (CPU)", r"crypto_pwhash|argon2|fill_memory"),
    ("reading the client's request", r"http_read_request|wait_readable"),
    ("writing the response", r"http_send|writev|write_all"),
    ("retry backoff sleep", r"nanosleep"),
]


def classify(stack):
    for name, pat in RULES:
        if re.search(pat, stack):
            return name
    top = re.search(r"#0\s+(?:0x[0-9a-f]+ in )?(\w+)", stack)
    return f"other (top frame: {top.group(1) if top else '?'})"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", default="buy")
    ap.add_argument("--hot", type=int, default=1)
    ap.add_argument("--threads", type=int, default=32)
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--samples", type=int, default=25)
    a = ap.parse_args()
    dbtools._ready = True
    buyers = run_bench.make_buyers(256)
    srv = harness.Server(SF_WORKERS=a.workers, SF_QUEUE_CAP=256, SF_EXPIRY_SWEEP_MS=5000, SF_RL_CHECKOUT_USER=1000000)
    out = os.path.join(run_bench.RESULTS, "pmp_loadgen.json")
    cmd = [run_bench.LOADGEN, "--port", str(srv.port), "--threads", str(a.threads), "--duration", "40", "--warmup", "2",
           "--scenario", a.scenario, "--users", buyers, "--products", ",".join(str(i) for i in range(49, 49 + 256)),
           "--hot", str(a.hot), "--out", out]
    lg = subprocess.Popen(cmd)
    time.sleep(4)
    counts, per_sample, errors = collections.Counter(), [], 0
    for _ in range(a.samples):
        r = subprocess.run(["gdb", "-p", str(srv.proc.pid), "-batch", "-ex", "set pagination off",
                            "-ex", "thread apply all bt 14"], capture_output=True, text=True, timeout=60)
        text = r.stdout
        if "Thread" not in text:
            errors += 1
            continue
        threads = re.split(r"\n(?=Thread \d+ )", text)
        sample = collections.Counter()
        for t in threads:
            if not t.startswith("Thread "):
                continue
            c = classify(t)
            sample[c] += 1
            counts[c] += 1
        per_sample.append(dict(sample))
        time.sleep(0.4)
    lg.terminate()
    lg.wait()
    srv.stop()
    n = sum(counts.values())
    res = {"scenario": a.scenario, "hot_skus": a.hot, "client_threads": a.threads, "server_workers": a.workers,
           "samples_taken": len(per_sample), "gdb_failures": errors,
           "thread_states": {k: {"samples": v, "percent_of_all_thread_observations": round(100 * v / n, 1)}
                             for k, v in counts.most_common()} if n else {}}
    json.dump(res, open(os.path.join(run_bench.RESULTS, f"pmp__{a.scenario}_hot{a.hot}.json"), "w"), indent=1)
    print(json.dumps(res, indent=1))
    if not n:
        print("GDB could not attach (needs SYS_PTRACE); no profile recorded.")


if __name__ == "__main__":
    main()
