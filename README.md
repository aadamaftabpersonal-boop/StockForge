# StockForge

Online retail store management system built on Linux systems programming:
C (pthreads, sockets, POSIX), MySQL, Apache, HTML/CSS/JS. The centerpiece is a
concurrency-safe checkout engine that cannot oversell stock.

Status: **Phase 6 complete**: checkout engine, failure injection, security hardening,
observability and a measured performance study. The frontend, Apache/TLS deployment, CI and
the demo are Phase 7. Where to start reading:

| document | what it is |
|---|---|
| [docs/CHECKOUT.md](docs/CHECKOUT.md) | the checkout design, lock order, and which test guards each guarantee |
| [docs/FAILURE_TESTING.md](docs/FAILURE_TESTING.md) | what was broken on purpose (DB cut at every point of every workflow) and what it found |
| [docs/SECURITY.md](docs/SECURITY.md) | threat model, controls with the test proving each, residual risks |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | measured results: worker pool, overload, hot-row contention, query plans |
| [docs/OPERATIONS.md](docs/OPERATIONS.md) | health, metrics, log events, every config variable, troubleshooting |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/PLAN.md](docs/PLAN.md), [docs/PHASE4_NOTES.md](docs/PHASE4_NOTES.md) | design and plan |

## Quick start (Linux via Docker; nothing to install on the host)

```bash
cp .env.example .env                  # then edit: set real passwords
docker compose -f deploy/docker-compose.yml --env-file .env up -d --wait   # MySQL 8.4
scripts/dev.sh sh scripts/db-init.sh --seed      # schema + DEV demo data (PowerShell: .\scripts\dev.ps1 ...)
```

Demo accounts (**dev seed only**, passwords are public): `admin@stockforge.test`,
`ava@stockforge.test`, `ben@stockforge.test`. Passwords are in `db/seed/gen_seed.py`.
The seed includes a numbered keyboard with exactly **1 unit in stock** for checkout demos.

## Build and test

```bash
scripts/dev.sh make test                 # unit tests (no DB needed)
scripts/dev.sh make itest                # all integration, stress, failure and fuzz tests (needs MySQL up; ~2 min)
scripts/dev.sh sh scripts/it.sh test_db_faults            # one module or class (add `env SF_DUMP_LOG=1` to see the server log)
scripts/dev.sh sh scripts/check.sh       # build + unit + integration
SAN=address,undefined scripts/dev.sh sh scripts/check.sh   # ASan + UBSan
STRESS_SCALE=3 scripts/dev.sh make itest                   # triple the concurrency in the stress tests
FUZZ_SEED=123 scripts/dev.sh sh scripts/it.sh test_fuzz_http   # replay a fuzz run
scripts/dev.sh python3 tests/mutation/mutate.py            # mutation check: do the tests catch broken locking/authz?
```

TSan needs ASLR off and the DB network inside the container:

```bash
docker run --rm --security-opt seccomp=unconfined --env-file .env --network stockforge_default \
  -v "$PWD":/work -w /work stockforge-dev setarch x86_64 -R sh -c 'SAN=thread sh scripts/check.sh'
```

Valgrind (server ~20x slower, so run a subset):

```bash
scripts/dev.sh env SF_BIN=/work/scripts/valgrind-sf.sh SF_START_TIMEOUT=120 SF_STOP_TIMEOUT=180 \
    sh scripts/it.sh test_checkout.CheckoutSemantics
```

## Benchmarks

```bash
scripts/dev.sh python3 bench/run_bench.py        # ~15 min: builds a 100k-product DB, runs every experiment 3x
scripts/dev.sh python3 bench/query_plans.py      # indexed vs not, keyset vs OFFSET, FULLTEXT vs LIKE
scripts/dev.sh python3 bench/pmp.py --scenario buy --hot 1   # where threads spend their time (GDB sampling)
python3 bench/charts.py && python3 bench/report.py           # regenerates docs/BENCHMARKS.md from the JSON results
```

## Operate

```bash
scripts/dev.sh build/rel/reconcile        # offline invariant check; exit 0 = every invariant holds
scripts/dev.sh sh -c 'SF_COOKIE_SECURE=0 build/rel/stockforged & sleep 1; curl -s localhost:8080/healthz; echo; curl -s "localhost:8080/api/v1/products?limit=2"'
```

Configuration is via `SF_*` environment variables; see [docs/OPERATIONS.md](docs/OPERATIONS.md).
`SF_DB_PASSWORD` is required; there is no default.
