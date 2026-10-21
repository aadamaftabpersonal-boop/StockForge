# Security model and threat model

Scope: the C backend and its database. TLS termination, HSTS, and the browser-side
rules (no `innerHTML`) belong to Apache and the frontend (Phase 7) and are listed as such.

## 1. Assets and actors

| Asset | Why it matters |
|---|---|
| Customer accounts and sessions | account takeover |
| Order, stock and payment-attempt integrity | money and inventory |
| Admin capability (product, stock, audit, reconcile) | privilege escalation |
| Database credentials | full data access |

| Actor | Capability assumed |
|---|---|
| Anonymous internet client | any HTTP, any rate |
| Malicious customer | valid account; tries other users' data, admin routes, stock/price manipulation, double orders |
| Malicious website visited by a logged-in user | can make the browser send credentialed requests (CSRF) |
| Compromised application process | can run any SQL the app user may run |
| Network observer between browser and Apache | passive/active (mitigated by TLS at Apache) |

Trust boundaries: browser -> Apache (TLS) -> `stockforged` on loopback -> MySQL on a private network.

## 2. Controls and the test that proves each

| Threat | Control | Evidence |
|---|---|---|
| Password theft from a DB leak | Argon2id (libsodium, interactive limits); no plaintext anywhere | `test_password_stored_as_argon2id_never_plaintext` |
| Stolen DB can replay sessions | only SHA-256(token) stored; token is 256 random bits | `test_session_token_is_hashed_at_rest...` |
| Account enumeration by login | identical error for wrong password / unknown / inactive; unknown accounts still pay one Argon2 verification | `test_login_failures_are_indistinguishable`, `test_unknown_account_still_pays_for_argon2` |
| Brute force | per-IP and per-account login limits, per-IP register limit, per-user checkout limit | `RateLimits` tests; unit tests for the limiter incl. 4-thread exactness |
| Spoofed `X-Forwarded-For` evading limits | header ignored unless `SF_TRUST_PROXY=1`, then only the **last** entry (appended by Apache) is used | `test_forwarded_for_is_ignored...`, `test_trusted_proxy_uses_last_xff_entry` |
| Session fixation / unbounded sessions | new token every login; max 10 live sessions per user | `test_sessions_per_user_are_capped` |
| Session theft after logout / role change | server-side revocation; role and `is_active` read from the DB on **every** request | `test_logout_revokes_server_side`, `test_session_of_deleted_or_demoted_user_loses_power_immediately` |
| CSRF | per-session token in `X-CSRF-Token` (constant-time compare) + `Origin` must match `Host` + JSON content type required + `SameSite=Lax` | `test_every_authenticated_unsafe_route_demands_csrf`, `test_cross_origin_requests_rejected` |
| Missing authorization on a route | every route x {anonymous, customer, admin} asserted; **a meta-test fails if a route exists that is not in the matrix** | `test_authz_matrix.py` |
| IDOR on orders | owner is part of the `WHERE`; others get 404 identical to "does not exist" | `test_other_users_orders_look_nonexistent` |
| SQL injection | prepared statements only; the two dynamic queries are built from constant fragments with `?` for values; search input reduced to `+token*` terms | injection tests in auth/catalog/orders; 4,000 fuzz requests |
| Request smuggling / parser abuse | strict `Content-Length` (digits only, one header), any `Transfer-Encoding` rejected, header/body size caps, one total read deadline per request | `test_http.c`, `test_server_core.py` |
| Resource exhaustion | bounded queue sheds load with 503; deadlines; lingering close is time- and byte-bounded | `Overload` test, slowloris test |
| Malformed/deep JSON | cJSON nesting limit; field validators (`validate.c`, 88 unit checks) | `test_malformed_bodies`, `test_fuzz_http` |
| Stored XSS | API only returns `application/json` with `nosniff` and a `default-src 'none'` CSP; text is stored verbatim and **must be rendered with `textContent`** (Phase 7 rule) | `test_html_in_text_fields_is_data_not_markup` |
| Clickjacking, referrer leaks | `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, CSP `frame-ancestors 'none'` | `test_defensive_headers_on_every_kind_of_response` |
| SSRF / external image tracking | product `image_url` must be `https://` or a single-slash absolute path (protocol-relative `//host` rejected) | `test_validation` |
| Secrets in logs | passwords, tokens, CSRF tokens, query strings and request bodies are never logged; log values are JSON-escaped | `test_no_secrets_in_server_log_or_audit` (canary values) |
| Credentials in source | all secrets come from the environment; there is **no default DB password** (startup fails) | `config.c`; `.env` is git-ignored |
| Blast radius of a compromised app | DB user has `SELECT/INSERT/UPDATE/DELETE` only: no DDL, no `GRANT`, no other schemas | `test_db_privileges.py` |
| Over-selling / double charge as a security issue | row locks + `CHECK` + idempotency keys + state machine | `docs/CHECKOUT.md` |
| Memory-safety bugs | ASan, UBSan, TSan runs of the whole suite; fuzzers run under them | CI-equivalent `scripts/check.sh` with `SAN=` |
| Audit | `audit_events` rows for login, failures, order lifecycle, admin changes, with request IDs | `AuditApi` tests |
| Payment data | no card data exists; the provider is a labelled simulation | `docs/CHECKOUT.md` section 3 |

## 3. Residual risks and known limits (honest list)

* **Worker starvation by slow clients.** A blocking-I/O worker is held until the 5 s request deadline. Eight slow connections can occupy eight workers. Apache (which buffers whole requests before proxying) is the intended mitigation; the C server alone is vulnerable. An `epoll` front end is the real fix.
* **Rate limits are per process** and an attacker spraying many distinct keys can evict counters. Not a cluster-wide limiter.
* **Account enumeration via `register`** (`409 EMAIL_TAKEN`) is an accepted UX trade-off, throttled by the register limit.
* **No MFA, email verification, password reset or account lockout** (lockout is deliberately replaced by throttling to avoid lock-out denial of service).
* **No TLS between the app and MySQL** (private network assumed). Required before any non-local deployment.
* **TLS, HSTS, secure cookie transport** depend on the Apache layer (Phase 7). `SF_COOKIE_SECURE` defaults to on; tests turn it off for plain HTTP.
* **Memory cost of Argon2** (64 MiB per concurrent login) means a login flood can use `workers x 64 MiB`; the login limits bound the rate, not the instantaneous memory.
* **The `/metrics` bearer token comparison is constant-time but not length-hiding.**
* **Dependencies:** `picohttpparser` is vendored from its default branch at the date of download (no tagged release exists); cJSON v1.7.18 with one local patch (`vendor/cJSON/PATCHES.md`); libsodium and libmariadb come from the Ubuntu 24.04 image. No automated vulnerability scanning is set up.
* **Simulated payments** accept a client-chosen scenario by design; a real provider integration would not.
