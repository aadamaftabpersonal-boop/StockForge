# StockForge

Online retail store management system built on Linux systems programming:
C (pthreads, sockets, POSIX), MySQL, Apache, HTML/CSS/JS. The centerpiece is a
concurrency-safe checkout engine that cannot oversell stock.

Status: **Phase 2 complete** (plan, architecture, schema). See
[docs/PLAN.md](docs/PLAN.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Try the schema

```bash
cp .env.example .env            # edit the password
docker compose -f deploy/docker-compose.yml --env-file .env up -d --wait
docker compose -f deploy/docker-compose.yml exec -T db sh -c 'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" stockforge' < db/migrations/001_init.sql
```
