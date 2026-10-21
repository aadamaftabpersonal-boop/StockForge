# StockForge

Online retail store management system built on Linux systems programming:
C (pthreads, sockets, POSIX), MySQL, Apache, HTML/CSS/JS. The centerpiece is a
concurrency-safe checkout engine that cannot oversell stock.

Status: **Phase 3 complete** (thread-pool HTTP server core, build, tests).
No database access, auth or store features yet. See [docs/PLAN.md](docs/PLAN.md)
and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build and test (Linux, via Docker — nothing to install on the host)

```bash
scripts/dev.sh make test              # unit tests            (PowerShell: .\scripts\dev.ps1 make test)
scripts/dev.sh make itest             # integration tests against the live server
scripts/dev.sh sh scripts/check.sh    # build + unit + integration
SAN=address,undefined scripts/dev.sh sh scripts/check.sh   # ASan + UBSan
```

TSan needs ASLR off inside the container:

```bash
docker run --rm --security-opt seccomp=unconfined -v "$PWD":/work -w /work stockforge-dev \
  setarch x86_64 -R sh -c 'SAN=thread sh scripts/check.sh'
```

## Run the server

```bash
scripts/dev.sh sh -c 'SF_DEBUG=1 build/rel/stockforged & sleep 1; curl -s localhost:8080/healthz; echo; curl -s localhost:8080/metrics'
```

Configuration is via `SF_*` environment variables (see `src/config.c`).

## Database schema

```bash
cp .env.example .env            # edit the password
docker compose -f deploy/docker-compose.yml --env-file .env up -d --wait
docker compose -f deploy/docker-compose.yml exec -T db sh -c 'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" stockforge' < db/migrations/001_init.sql
```
