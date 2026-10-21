# StockForge

Online retail store management system built on Linux systems programming:
C (pthreads, sockets, POSIX), MySQL, Apache, HTML/CSS/JS. The centerpiece is a
concurrency-safe checkout engine that cannot oversell stock.

Status: **Phase 4 complete** (identity, catalog, cart, order read APIs, admin
inventory, on top of the thread-pool server). Checkout/payment is Phase 5; the
frontend is Phase 7. See [docs/PLAN.md](docs/PLAN.md),
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/PHASE4_NOTES.md](docs/PHASE4_NOTES.md).

## Quick start (Linux via Docker; nothing to install on the host)

```bash
cp .env.example .env                  # then edit: set real passwords
docker compose -f deploy/docker-compose.yml --env-file .env up -d --wait   # MySQL 8.4
scripts/dev.sh sh scripts/db-init.sh --seed      # schema + DEV demo data (PowerShell: .\scripts\dev.ps1 ...)
```

Demo accounts (**dev seed only**, passwords are public): `admin@stockforge.test`,
`ava@stockforge.test`, `ben@stockforge.test`. Passwords are in `db/seed/gen_seed.py`.

## Build and test

```bash
scripts/dev.sh make test                 # unit tests (no DB needed)
scripts/dev.sh make itest                # integration tests (needs MySQL up; rebuilds a throwaway test DB)
scripts/dev.sh sh scripts/check.sh       # build + unit + integration
SAN=address,undefined scripts/dev.sh sh scripts/check.sh   # ASan + UBSan
```

TSan needs ASLR off and the DB network inside the container:

```bash
docker run --rm --security-opt seccomp=unconfined --env-file .env --network stockforge_default \
  -v "$PWD":/work -w /work stockforge-dev setarch x86_64 -R sh -c 'SAN=thread sh scripts/check.sh'
```

## Run the server

```bash
scripts/dev.sh sh -c 'SF_COOKIE_SECURE=0 build/rel/stockforged & sleep 1; curl -s localhost:8080/healthz; echo; curl -s "localhost:8080/api/v1/products?limit=2"'
```

Configuration is via `SF_*` environment variables (`src/config.c`, `.env.example`).
`SF_DB_PASSWORD` is required; there is no default.
