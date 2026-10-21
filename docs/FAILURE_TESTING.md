# Failure testing: what was broken on purpose, and what happened

Principle: every reliability claim has a test, or is listed under "Not covered".
After each failure the **database** is inspected (cross-table invariants), not just the HTTP status.

## 1. Fault injection between the server and MySQL

`tests/integration/faultproxy.py` is a TCP proxy that speaks enough of the MySQL protocol to
count commands inside a transaction. The server under test connects to it, so the real
driver, real prepared statements and real transactions run, with no mocks.

| Fault | How |
|---|---|
| Connection cut at **every command index** of a transaction | `arm("nth", k)` for k = 1, 2, 3 ... until k passes the end |
| ...before MySQL sees the command ("it did not happen") | action `before` |
| ...after MySQL executed it but the reply is lost ("it happened, the client cannot know") | action `after` |
| `COMMIT` specifically, both ways | `arm("sql", match=b"COMMIT")` |
| Database down / back up | `down()` / `up_()` |
| Hung database (accepts, never answers) | `blackhole = True` |
| Slow database | `latency = 0.005` |
| All connections killed between requests (restart / network blip) | `sever_all()` |

### Results (all from `tests/integration/test_db_faults.py`)

For each point the test asserts: (1) invariants hold immediately after the fault,
(2) a retry with the same idempotency key / same operation converges to exactly the right final state.

| Workflow | Distinct fault points (before + after) | Outcome |
|---|---|---|
| Checkout (2-line cart, 61 commands) | 61 + 61 | all converge; exactly one order; reserved once; replay header correct |
| Pay | 37 + 37 | all converge; charged once; stock deducted once |
| Cancel unpaid | 31 + 31 | released exactly once |
| Cancel paid | 31 + 31 | restocked exactly once |
| Expiry sweeper (7 points) | 7 | sweeper recovers on its next cycle |

Explicit "outcome unknown" demonstration (`OutcomeUnknown`):

* `COMMIT` lost **before** MySQL: no order exists; retry creates it (`Idempotent-Replay: false`).
* `COMMIT` executed, reply lost: the client is told 503 **but the order exists**; the retry returns it (`Idempotent-Replay: true`), and stock is still reserved once.

Outage behaviour (`Outages`): DB down gives `/healthz` 503 `degraded` and API 503 `DB_UNAVAILABLE` with `Retry-After`,
the server stays alive, and **recovers without a restart** once the DB returns; a hung DB makes a request fail
within the configured `SF_DB_TIMEOUT_S` instead of hanging forever; connections cut between requests are
reconnected transparently, including at `START TRANSACTION`; a 5 ms-per-command slowdown never oversells.

## 2. Real bugs this found (not hypothetical)

1. **Statements escaping a transaction after a connection loss (severity: data integrity).**
   `apply_reservations` ran three statements and checked their results afterwards. If the connection
   died on the first, the second and third transparently reconnected and ran on a fresh *autocommit*
   connection, committing half of a transaction outside the transaction (stock released, order still
   pending). Found by the invariant check after a fault at one specific command of the pay sweep.
   Fixes, in depth: each statement is checked before the next runs, **and** the DB layer now marks a
   transaction whose connection died as *poisoned*: no statement may run, and `COMMIT` cannot report
   success, until it is rolled back (`src/db.c`). The mutation harness re-introduces the bug
   (both defences removed) and confirms the sweeps fail.
2. **Hardening (not a failure we observed):** `db_begin` and `db_ping` reconnect once if the connection is
   found dead, because nothing has run yet so that is always safe. In practice every transactional route
   first runs a session lookup that already recovers a stale connection, so only a cut *exactly at*
   `START TRANSACTION` reaches this path. A first version of its test did not reach it either (the
   mutation harness showed the mutant surviving); the test now cuts at precisely that command.
3. **Invalid UTF-8 in search text produced `503 DB_UNAVAILABLE`** (found by the HTTP fuzzer on a release-build run,
   after passing on two other runs: fuzzing needs repeated seeds). The search tokenizer accepted every byte
   >= 0x80 as a word character, so sequences such as a UTF-16 surrogate (`%ED%A0%80`) became the boolean query
   `+<invalid bytes>*`, which MySQL's full-text parser rejects (errno 1064). I first suspected *all* non-ASCII
   symbols and tested valid ones (emoji, `★★★`, `§`, `€`): they are fine; only invalid sequences broke it.
   Fix in two layers: the tokenizer decodes UTF-8 strictly (invalid bytes become separators; tokens are counted in
   characters like InnoDB's minimum), and a remaining 1064 from the search query now returns `422`, not `503`.
   Regression: `test_invalid_utf8_in_any_string_input_is_a_client_error_never_a_5xx` plus unit tests; both layers are mutation-tested.
4. Test-tool bugs found along the way (kept here because the next person will hit them):
   `close()` does not wake a thread blocked in `recv()` on Linux, so a proxy must `shutdown()` first;
   a `COMMIT` rule must match as a *prefix* (it matched `READ-COMMITTED`); a proxy without `TCP_NODELAY`
   adds ~40 ms per round trip (Nagle + delayed ACK) and turned a 36 s suite into 579 s.

## 3. Other failure modes

| Failure | Test | Result |
|---|---|---|
| `kill -9` mid-checkout at 3 different moments | `CrashRecovery.test_sigkill...` | between 1 and 31 of 150 orders were already committed when the kill landed (varies per run); books balanced every time; every retry converged to one order per user |
| SIGTERM under load | `CrashRecovery.test_sigterm...` | exit 0; nothing acknowledged was lost |
| Client disconnects after sending | `test_client_disconnect_then_retry_never_duplicates` | server completes it; retry replays |
| Malformed / oversized / hostile HTTP | `test_server_core`, `test_fuzz_http` | 4,000 mutated requests: 0 x 5xx, server healthy; all errors use the JSON envelope |
| Worker-queue saturation | `Overload.test_full_queue_rejects_with_503_and_recovers` | `503 Retry-After`, admitted work completes, recovers |
| Duplicate request delivery | 60 identical concurrent checkouts | 1 order |
| Repeated cancellations / payments | `test_repeated_and_concurrent_cancels...`, `test_thirty_concurrent_payments...` | effect applied once |
| Failed transitions | `test_order_state` (6x6 matrix), `ILLEGAL_TRANSITION` tests | only the 6 legal transitions succeed |
| Random operation mixes with forced expiry | `Chaos` (seeded) | ~2,000 requests, no 5xx, invariants hold |

## 4. Not covered (stated, not hidden)

* **MySQL server crash / InnoDB recovery.** InnoDB owns redo logging and crash recovery. We test the application's transaction boundaries and retry protocol; we do not kill `mysqld` itself or test its durability.
* Disk-full, out-of-memory kills, clock jumps, file-descriptor exhaustion (the acceptor backs off on `EMFILE` but this is not tested).
* Network partitions between the browser and Apache (Phase 7).
* Primary/replica failover (a single MySQL instance is assumed).
