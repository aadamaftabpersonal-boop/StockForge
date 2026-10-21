# Phase 4 design notes and findings

Appended to the architecture as "section 8"; kept in its own file so the
architecture document stays a stable reference.

## 8.1 Database layer (`src/db.c`)

- **Prepared statements only.** Every statement uses `?` placeholders. The one
  place SQL text varies (`h_products`, `h_admin_audit`) assembles it from
  constant fragments; values go through `db_params`.
- **One connection per worker thread** (thread-local), created lazily, so there
  is no pool lock and no cross-thread connection sharing.
- **Retry policy (the part to be able to explain):** if a *prepare* fails
  because the connection died, nothing has run, so we reconnect and retry once
  (only outside a transaction). If an *execute* fails, we **never** retry: the
  statement may have been applied before the connection dropped, and replaying
  it could double-apply. Checkout will rely on idempotency keys for that case.
- **Session settings:** `READ COMMITTED`, UTC.
- **Least privilege:** the server connects as `sf_app` (SELECT/INSERT/UPDATE/DELETE
  only). `test_db_privileges.py` proves it cannot run DDL, `GRANT`, or read
  `mysql.user`, so a SQL-injection or RCE bug cannot alter the schema.

## 8.2 Authentication and sessions

| Concern | Decision |
|---|---|
| Password hashing | Argon2id (libsodium `crypto_pwhash_str`, interactive limits: 2 passes, 64 MiB). Per-request memory is therefore ~64 MiB x workers in the worst case. |
| Session token | 32 random bytes, base64url, in an `HttpOnly; SameSite=Lax` cookie (`Secure` unless `SF_COOKIE_SECURE=0`). **Only SHA-256(token) is stored**, so a database leak does not yield usable cookies. New token on every login (no fixation). |
| CSRF | Per-session random token returned at login/`/auth/me`, required in `X-CSRF-Token` on every unsafe authenticated request, compared in constant time. Layered with: `Origin` must match `Host` when present; unsafe requests with a body must be `application/json` (forces a CORS preflight cross-site); `SameSite=Lax`. |
| Login errors | Identical response for wrong password, unknown account and inactive account. Unknown accounts still burn one Argon2 verification so timing does not leak existence (tested). |
| Enumeration | **Known trade-off:** `register` returns `409 EMAIL_TAKEN`, which reveals that an email is registered. Accepted for UX; mitigated by the per-IP register limit. |
| Rate limits | Fixed-window, in-process (`ratelimit.c`): login per IP and per account, register per IP. Client IP is the socket peer, or the *last* `X-Forwarded-For` entry only when `SF_TRUST_PROXY=1` (Apache appends, so the last entry is the one it observed). **Limitations:** per-process, not cluster-wide; an attacker spraying many distinct keys can evict counters. |
| Secrets | Password and session token are zeroed after use where we control the memory and are never logged (tested by a canary-password test over the server log and audit table). |

## 8.3 Catalog design

- **Keyset pagination**, not OFFSET: cursor = `id` (newest) or `price.id` (price
  sorts). Stable under concurrent inserts (tested) and O(limit) per page.
  `limit` is clamped to 50 server-side.
- **Search** uses InnoDB `FULLTEXT` in BOOLEAN MODE with `+token*` terms built
  by `search_to_boolean()`: operators are stripped (no query-syntax injection),
  tokens under 3 bytes are dropped (InnoDB's minimum), max 6 tokens. Searching
  one- or two-letter words returns `422`, by design.
- **Availability** is `on_hand - reserved`, computed in SQL; `stock_status` is
  derived from the per-product low-stock threshold.
- Migration `002` adds `ix_products_active_price` for price-sorted browsing
  without a category. **Not yet proven with EXPLAIN**: that comparison belongs
  to the Phase 6 query-plan study, and until then it is a hypothesis.

## 8.4 Findings from testing (things the tests caught, not inspection)

1. **cJSON data race** (found by ThreadSanitizer in the concurrent stock test):
   cJSON writes a process-wide error struct on every parse. Fixed by making it
   `_Thread_local`; recorded in `vendor/cJSON/PATCHES.md`.
2. **Missing `FOR UPDATE` is caught by the suite** (mutation test, done once by
   hand): removing the row lock from stock adjustment makes both concurrency
   tests fail. Without the lock, concurrent transactions do not just lose
   updates, they deadlock (MySQL 1213), surfacing as 503s. **Consequence for
   Phase 5:** checkout needs a bounded deadlock-retry loop and a fixed lock
   order; the DB layer exposes `db_errno()` for that.
3. **Protocol-relative image URLs** (`//evil.example/x.png`) passed the first
   draft of `image_url` validation (it accepted anything starting with `/`).
   Caught while writing the validation test; now rejected.
4. Phase 3 recap: lingering close (RST destroys error responses) and the
   readiness-probe-consumes-a-queue-slot test artifact.

## 8.5 Known gaps (deliberate, tracked)

- No `PATCH /admin/products` or product deactivation endpoint yet.
- `/metrics` token comparison is constant-time, but the token length is not hidden.
- Session cleanup only deletes the *logging-in user's* expired sessions; a
  global sweeper arrives with the background worker (stretch goal 3).
- Cart lines are advisory: they do not reserve stock. Only checkout (Phase 5) does.
- TLS between the server and MySQL is not enabled (same Docker network); required for any non-local deployment.
