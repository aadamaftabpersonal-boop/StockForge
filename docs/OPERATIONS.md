# Operating StockForge

## Health, metrics, logs

### `GET /healthz` (no auth)
Readiness, not just liveness: it pings MySQL. `200 {"status":"ok","database":"up",...}` or
`503 {"status":"degraded","database":"down"}` with `Retry-After`. Includes uptime, worker count and queue depth.
The server **self-heals**: when MySQL returns, the next request reconnects (tested without restarting the server).

### `GET /metrics` (admin session, or `Authorization: Bearer $SF_METRICS_TOKEN`)
JSON counters (all monotonic since process start except `workers.busy` and `queue.depth`):

| group | fields |
|---|---|
| connections | `connections_accepted`, `connections_rejected_overload` (queue full => 503) |
| requests | `requests_total`, `responses.{2xx,3xx,4xx,5xx}` |
| worker pool | `workers.{total,busy}`, `queue.{depth,high_water,capacity}` |
| orders | `orders.{created,replayed,idempotency_conflicts,insufficient_stock,deadlock_retries,retries_exhausted,paid,payment_failed,cancelled,expired}` |
| database | `db.{statements,time_us,slow,connects,errors}` |
| latency | `latency_us_sum`, `queue_wait_us_sum`, `latency_ms_hist` (buckets le 1,2,5,10,25,50,100,250,500,1000,+Inf ms) |

Worker utilisation = `workers.busy / workers.total`; queue saturation = `queue.high_water / queue.capacity`.
Mean DB time per statement = `db.time_us / db.statements`. A rising `orders.deadlock_retries` means lock-order
discipline has been broken; `retries_exhausted > 0` means clients are seeing `503 DB_BUSY`.

Behind Apache, `/metrics` is **denied** (`403`) so it is never public. Administrators read the
same document at `GET /api/v1/admin/metrics`, which is what the console's System page shows
(refreshed every 5 seconds; "Run consistency check" calls `GET /api/v1/admin/reconcile`).
Scrapers inside the private network can reach the app container's port 8080 directly with the bearer token.

### Logs (stderr, one JSON object per line)
Fields: `ts` (UTC, ms), `level`, `event`, then event fields. User-controlled text is JSON-escaped and truncated.
**Never logged:** passwords, session tokens, CSRF tokens, request bodies, query strings, cookies, card data.

| event | meaning |
|---|---|
| `access` | one per request: `request_id`, `method`, `path` (no query), `status`, `latency_us`, `queue_wait_us` |
| `started` / `shutdown` / `stopped` | lifecycle; `stopped` means a clean drain |
| `overload_reject` | queue full, a connection was answered with 503 |
| `client_gone` | peer closed before sending a full request |
| `db` | a MySQL error (`errno=` and text); deadlocks/lock waits (1213/1205) are `warn`, expected and retried |
| `slow_query` | a statement took >= `SF_SLOW_QUERY_MS`; logs the SQL **template** (placeholders), never values |
| `expiry` | the sweeper expired N overdue orders |
| `invariant` | an internal consistency check failed and the operation was rolled back: treat as a bug report |

Every response carries `X-Request-Id`; error bodies repeat it as `error.request_id`, and the audit table stores it,
so a user report can be traced from the browser to the log line to the audit row.

### Error codes
Stable strings in `error.code`: `VALIDATION_FAILED`, `INVALID_JSON`, `UNAUTHENTICATED`, `FORBIDDEN`, `CSRF_INVALID`,
`ORIGIN_MISMATCH`, `NOT_FOUND`, `METHOD_NOT_ALLOWED`, `UNSUPPORTED_MEDIA_TYPE`, `RATE_LIMITED`, `EMAIL_TAKEN`, `SKU_TAKEN`,
`INSUFFICIENT_STOCK`, `EMPTY_CART`, `PRICE_CHANGED`, `PRODUCT_UNAVAILABLE`, `IDEMPOTENCY_KEY_REQUIRED/INVALID/REUSED`,
`ILLEGAL_TRANSITION`, `ORDER_EXPIRED`, `PAYMENT_DECLINED`, `PAYMENT_TIMEOUT`, `BELOW_RESERVED`, `DB_UNAVAILABLE`, `DB_BUSY`,
`OVERLOADED`, `INVARIANT_VIOLATION`. Messages are constant strings; SQL text, paths and stack traces never reach a client.

## Configuration (environment variables)

| variable | default | meaning |
|---|---|---|
| `SF_BIND` / `SF_PORT` | `127.0.0.1` / `8080` | listen address (keep it on loopback behind Apache) |
| `SF_WORKERS` | `8` | worker threads (1-256). Benchmarks: throughput knee between 8 and 16 on the dev machine |
| `SF_QUEUE_CAP` | `64` | accepted-but-unserved connections before shedding with 503 |
| `SF_READ_TIMEOUT_MS` / `SF_WRITE_TIMEOUT_MS` | `5000` / `5000` | total deadline to receive a request / per-send timeout |
| `SF_MAX_HEADER` / `SF_MAX_BODY` | `8192` / `65536` | byte limits (431 / 413 beyond them) |
| `SF_DB_HOST` / `SF_DB_PORT` / `SF_DB_USER` / `SF_DB_NAME` | `127.0.0.1` / `3306` / `sf_app` / `stockforge` | connection |
| **`SF_DB_PASSWORD`** | **required** | no default; startup fails without it |
| `SF_DB_TIMEOUT_S` | `15` | MySQL socket read/write timeout (a hung DB fails a request after this) |
| `SF_SLOW_QUERY_MS` | `250` | slow-statement threshold |
| `SF_COOKIE_SECURE` | `1` | `Secure` flag on the session cookie; set `0` only for plain-HTTP development |
| `SF_SESSION_TTL_S` | `86400` | absolute session lifetime |
| `SF_TRUST_PROXY` | `0` | take the client IP from the **last** `X-Forwarded-For` entry (only behind Apache) |
| `SF_RL_LOGIN_IP` / `SF_RL_LOGIN_ACCT` / `SF_RL_REGISTER_IP` / `SF_RL_CHECKOUT_USER` | `30/min` / `8 per 5 min` / `10/min` / `30/min` | rate limits |
| `SF_RESERVATION_TTL_S` | `900` | how long an unpaid order holds stock |
| `SF_EXPIRY_SWEEP_MS` | `5000` | expiry sweeper interval |
| `SF_METRICS_TOKEN` | empty (disabled) | bearer token for `/metrics`, at least 16 characters |
| `SF_DEBUG` | `0` | enables `/debug/*` routes. **Never in production.** |

## Deployment notes (Apache stack)

* Compose profile `stack` adds the `app` container (non-root, read-only root filesystem) and `web` (Apache 2.4).
  The app is **not** published to the host; only Apache is (`8443` TLS, `8081` HTTP redirect).
* Apache appends the connecting address to `X-Forwarded-For`; the app is started with `SF_TRUST_PROXY=1`
  so rate limits are per real client, not per proxy.
* Rebuild the app image after changing C code: `docker compose --env-file .env -f deploy/docker-compose.yml --profile stack up -d --build app`.
  Changes under `web/` need no rebuild (mounted read-only); hard-reload the browser.
* Demo data: `scripts/dev.sh sh scripts/reset-demo.sh` (destroys and rebuilds the demo database, then runs the
  consistency checker). Never point it at real data.
* Full install and troubleshooting: [INSTALL.md](INSTALL.md).

## Troubleshooting

| symptom | likely cause | check |
|---|---|---|
| `503 DB_UNAVAILABLE`, `/healthz` degraded | MySQL unreachable or credentials wrong | `db` log events (`errno=2003` refused, `1045` access denied) |
| `503 DB_BUSY` | lock waits exhausted 6 retries | `orders.deadlock_retries`, `slow_query` logs; one very hot SKU |
| `503 OVERLOADED` with `Retry-After` | queue full | `queue.high_water == capacity`; raise `SF_WORKERS` only if the DB has headroom |
| requests slow but CPU idle | the server is waiting on MySQL | `db.time_us / db.statements`; `slow_query` logs; `EXPLAIN` the template |
| everyone gets `401` after a deploy | `SF_COOKIE_SECURE=1` over plain HTTP (browsers drop the cookie) | use HTTPS or set `0` in dev |
| rate limit hits behind Apache | all clients share 127.0.0.1 | set `SF_TRUST_PROXY=1` and configure Apache to append `X-Forwarded-For` |
| `build/rel/reconcile` exits 1 | an invariant is violated | read which check failed; it lists the offending ids |
| TSan: `unexpected memory mapping` | kernel ASLR entropy | run under `setarch x86_64 -R` (README) |
