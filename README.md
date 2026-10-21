# StockForge

Online retail store management system built on Linux systems programming:
C (pthreads, sockets, POSIX), MySQL, Apache, HTML/CSS/JS. The centerpiece is a
concurrency-safe checkout engine that cannot oversell stock.

Status: **Phase 5 complete** (concurrent checkout engine: idempotent order
creation, atomic stock reservation, simulated payment, cancel, expiry,
reconciliation). The frontend is Phase 7. Start with
[docs/CHECKOUT.md](docs/CHECKOUT.md), then [docs/PLAN.md](docs/PLAN.md),
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [docs/PHASE4_NOTES.md](docs/PHASE4_NOTES.md).

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
scripts/dev.sh make itest                # integration + stress tests (needs MySQL up; rebuilds a throwaway test DB)
scripts/dev.sh sh scripts/check.sh       # build + unit + integration
SAN=address,undefined scripts/dev.sh sh scripts/check.sh   # ASan + UBSan
scripts/dev.sh python3 tests/mutation/mutate.py            # mutation check: do the tests catch broken locking?
STRESS_SCALE=3 scripts/dev.sh make itest                   # triple the concurrency in the stress tests
```

TSan needs ASLR off and the DB network inside the container:

```bash
docker run --rm --security-opt seccomp=unconfined --env-file .env --network stockforge_default \
  -v "$PWD":/work -w /work stockforge-dev setarch x86_64 -R sh -c 'SAN=thread sh scripts/check.sh'
```

## Verify a database offline

```bash
scripts/dev.sh build/rel/reconcile        # prints the invariant report; exit 0 = every invariant holds
```

## Run the server

```bash
scripts/dev.sh sh -c 'SF_COOKIE_SECURE=0 build/rel/stockforged & sleep 1; curl -s localhost:8080/healthz; echo; curl -s "localhost:8080/api/v1/products?limit=2"'
```

Configuration is via `SF_*` environment variables (`src/config.c`, `.env.example`).
`SF_DB_PASSWORD` is required; there is no default.
