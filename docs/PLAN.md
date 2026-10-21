# StockForge — Delivery Plan

A Linux retail transaction engine in C: pthread worker pool, MySQL-backed
concurrency-safe checkout, Apache at the edge, storefront + admin console.

## Environment findings (Phase 1)

| Item | Finding | Consequence |
|---|---|---|
| Host | Windows 11 + WSL2 Ubuntu 26.04, gcc 15.2, make, gdb, valgrind | Real Linux for sockets/pthreads/ASan/Valgrind/perf |
| Docker | Docker Desktop 29.x working, also reachable from WSL | MySQL, Apache and the dev image run in Compose |
| `sudo` in WSL | needs password | Dev libraries are installed **inside the Docker build image**, not on the host |
| MySQL / Apache on host | not installed | Provided by containers |
| Repo | `D:\InterviewProjects\oslabproject`, no commits, **git root detected as `D:\`** | Must `git init` inside the project folder (see commit guide) |
| Remote | `StockForge` on GitHub, empty | First push creates `main` |

## Decisions (flag any you disagree with)

1. **Integration:** Apache (`mod_proxy_http`, TLS termination, static files) → standalone C HTTP/1.1 server on `127.0.0.1:8080`. Chosen over CGI because CGI forks per request, which makes a worker pool, queue and shared metrics meaningless.
2. **HTTP parsing:** vendored `picohttpparser` (MIT, ~400 lines). Everything else in the server loop (accept, bounded queue, timeouts, limits, keep-alive) is ours and visible. Only the request-line/header tokeniser is borrowed.
3. **Libraries:** `libmariadb` (MySQL protocol client, prepared statements), `libsodium` (Argon2id, CSPRNG), vendored `cJSON`. No frameworks.
4. **Server I/O model:** one acceptor thread → bounded MPMC queue (mutex + 2 condvars) → N workers, each with its own DB connection. Full queue ⇒ immediate `503 Retry-After`. Blocking sockets with `SO_RCVTIMEO`/`SO_SNDTIMEO`.
5. **Language of tooling:** backend is 100 % C. Stress/failure/load test drivers are Python 3 stdlib (tooling, not product); a C load generator is built in Phase 6 if Python becomes the measurement bottleneck.
6. **Isolation:** `READ COMMITTED` for checkout; correctness comes from `SELECT … FOR UPDATE` in a deterministic lock order plus CHECK constraints, not from isolation level alone.
7. **Reservation model:** checkout *reserves* stock (`PENDING_PAYMENT`, 15-min TTL); simulated payment success *commits* it; cancel/expiry *releases* it.

## Phases

| # | Phase | Exit criteria (all must be demonstrated, not claimed) |
|---|---|---|
| 1 | Inspect & constraints | This document. ✅ |
| 2 | Architecture, schema, API contracts, state machine, invariants | Schema applies cleanly to MySQL 8.4 and CHECK constraints reject bad states ✅; `ARCHITECTURE.md` written ✅ |
| 3 | Build + test harness ✅ | Dev Docker image; `make` builds with `-Wall -Wextra -Werror`; ASan/UBSan/TSan targets; unit + integration tests; HTTP server answers `/healthz` through the thread pool (no DB yet) |
| 4 | Catalog, identity, inventory, orders ✅ | Register/login (Argon2id, sessions, CSRF), product list/search/pagination, cart, order read APIs, admin stock adjust; integration tests green. Details: `docs/PHASE4_NOTES.md` |
| 5 | **Concurrent checkout** ✅ | Idempotent `POST /orders`; 300 concurrent buyers vs stock = 1 ⇒ exactly 1 order, `on_hand ≥ 0`, reconcile clean; deadlock-retry covered; pay/cancel/expiry; mutation-tested. Design + guarantee→test matrix: `docs/CHECKOUT.md` |
| 6 | Failure, security, observability, perf ✅ | Fault-proxy sweeps at 322 points of checkout/pay/cancel + outage/hang/latency tests; authz matrix with meta-test; 4,000-request HTTP/JSON fuzzing; DB metrics and slow-query log; security headers and session cap; ASan/UBSan/TSan/Valgrind runs; C load generator, 3-run benchmarks, query-plan study, GDB profile. Docs: `FAILURE_TESTING.md`, `SECURITY.md`, `OPERATIONS.md`, `BENCHMARKS.md`, `DB_QUERY_PLANS.md` |
| 7 | Frontend + deploy + docs | Storefront + admin console on real APIs; Compose with Apache+TLS; CI; threat model; demo script |
| 8 | Stretch (in order) | Ledger+reconcile CLI (ledger table already in schema), expiry worker, outbox, fault-injection flag, 2nd server process |

Each phase ends with: build → run tests → report exact pass/fail → trade-offs → risks.

## Open questions for you

1. Does your coursework **mandate** Apache-as-proxy vs CGI, or a specific grading rubric/deliverable list? (Drop the brief/rubric into `docs/coursework/` if you have it.)
2. Is the demo machine Linux native, or WSL/Docker acceptable?
3. Team size / deadline? (Affects how much of Phase 7/8 is realistic.)
