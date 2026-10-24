# Installation and troubleshooting

Everything runs in containers; the host needs only Docker (Docker Desktop with WSL2 on
Windows, or Docker Engine on Linux/macOS) and Git. Commands are run from the repository root.

> **Verification status.** The steps below were run on Windows 11 with Docker Desktop and
> `scripts/dev.ps1`. The Linux/macOS equivalents (`scripts/dev.sh`) use the same commands but
> have not been run on those hosts by the author. The GitHub Actions workflow
> (`.github/workflows/ci.yml`) has not yet run on GitHub.

## 1. Configure

```bash
cp .env.example .env
```

Edit `.env` and replace **every** `change-me-*` value. There is no default database password;
the server refuses to start without `SF_DB_PASSWORD`. `.env` is git-ignored.

## 2. Start the database and load data

```bash
docker compose --env-file .env -f deploy/docker-compose.yml up -d --wait
scripts/dev.sh sh scripts/db-init.sh --seed
```

PowerShell: replace `scripts/dev.sh` with `.\scripts\dev.ps1`. The first call builds the
`stockforge-dev` image (a few minutes, once).

`--seed` loads the demo catalogue (48 products), three demo accounts, and 45 days of order
history. Without it you get an empty schema. To rebuild the demo data from scratch at any time
(this **destroys** the database it points at):

```bash
scripts/dev.sh sh scripts/reset-demo.sh
```

Demo accounts (dev seed only; the passwords are public in this repository):

| account | password | role |
|---|---|---|
| `admin@stockforge.test` | `Admin-Demo-2026!` | admin |
| `ava@stockforge.test`, `ben@stockforge.test` | `Customer-Demo-2026!` | customer |

The product `SF-KB-FOUNDER` has exactly **one** unit in stock; it is the checkout demo item.

## 3. Run the full stack (Apache + TLS + app + database)

```bash
scripts/dev.sh sh scripts/gen-dev-cert.sh          # self-signed certificate, development only
docker compose --env-file .env -f deploy/docker-compose.yml --profile stack up -d --build --wait
```

Open **https://localhost:8443** (the browser warns about the self-signed certificate; that is
expected) and **https://localhost:8443/admin/** for the console. Plain HTTP on port 80 only
redirects to HTTPS.

### Development over plain HTTP

Some embedded browsers refuse self-signed certificates. A development override serves the site on
`http://localhost:8081` as well. Never use it outside your own machine:

```bash
docker compose --env-file .env -f deploy/docker-compose.yml -f deploy/docker-compose.dev-http.yml --profile stack up -d --build --wait
```

### Real certificates

Replace the mounted files named by `SSLCertificateFile` / `SSLCertificateKeyFile` in
`deploy/apache/httpd.conf` with certificates from a CA. Keep the `Secure` cookie, HSTS and the
HTTPS redirect.

## 4. Build and test without Docker Compose's app container

```bash
scripts/dev.sh make                      # server, reconcile CLI, load generator
scripts/dev.sh make test                 # unit tests (no database needed)
scripts/dev.sh make itest                # integration, stress, fault-injection, fuzz (needs the database; about 2 minutes)
```

See [README.md](../README.md) for sanitizer builds, the mutation harness, and benchmarks.

## 5. Stop and clean up

```bash
docker compose --env-file .env -f deploy/docker-compose.yml --profile stack down        # keep data
docker compose --env-file .env -f deploy/docker-compose.yml --profile stack down -v     # also delete the database volume
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `required variable MYSQL_ROOT_PASSWORD is missing a value` | Compose was run without the repository's `.env`. Run from the repository root and pass `--env-file .env`. |
| App container exits immediately; log says `SF_DB_PASSWORD` is required | `.env` lacks the application password. Set it, then recreate the container. |
| App log shows `Access denied for user 'sf_app'` | The `sf_app` user is created by `scripts/db-init.sh`. If you changed `SF_DB_PASSWORD` afterwards, run `db-init.sh` again (it updates the user). |
| `/healthz` returns `503` with `"database":"down"` | MySQL is not reachable or not healthy yet: `docker compose ps`, then `docker compose logs db`. The server keeps running and reconnects; requests fail fast with `503 DB_UNAVAILABLE` meanwhile. |
| Apache exits with `Invalid command '\xef\xbb\xbf#'` | A config file was saved with a UTF-8 byte-order mark (common with Windows editors and PowerShell `Set-Content -Encoding utf8`). Save as UTF-8 **without BOM** and LF line endings. |
| Apache cannot start: certificate file not found | Run `scripts/gen-dev-cert.sh` (step 3), or use the dev-HTTP override. |
| Browser shows a certificate warning | Expected with the self-signed development certificate. |
| `503 Service temporarily unavailable` under heavy load | The bounded queue is full: this is deliberate load shedding (`connections_rejected_overload` in `/metrics`). Raise `SF_WORKERS` / `SF_QUEUE_CAP` only after measuring; see [BENCHMARKS.md](BENCHMARKS.md). |
| Sign-in answers `429` | The rate limiter (per IP and per account). Wait for `Retry-After` seconds. Limits are per process and reset on restart. |
| Checkout shows an item as sold out that you expect to be in stock | Open orders hold stock for 15 minutes (`SF_RESERVATION_TTL_S`). Let them expire, cancel them, or reset the demo data. |
| Integration tests cannot reach the database | Start the database first (step 2); the test container joins the `stockforge_default` network automatically when it exists. |
| Docker commands hang or return 500 | The Docker Desktop engine is unhealthy. Restart Docker Desktop; containers and volumes are kept. |
| Changed a file under `web/` and the browser still shows the old one | Static assets are cached. Hard-reload (Ctrl+Shift+R). |
| `dev.ps1` output looks truncated or shows `$?` oddities | Run it as `cmd /c "powershell -NoProfile -File scripts\dev.ps1 <command>"` so the exit code and output stream are passed through unchanged. |
