# The checkout engine: design, guarantees, and how each is tested

Code: [src/orders.c](../src/orders.c) (engine), [src/order_state.c](../src/order_state.c) (state machine),
[src/tx.c](../src/tx.c) (deadlock retry), [src/reconcile.c](../src/reconcile.c) (invariant checks),
[src/sweeper.c](../src/sweeper.c) (expiry thread).

## 1. Model

```
 cart (advisory) --POST /orders--> PENDING_PAYMENT --pay ok--> PAID --> SHIPPED --> DELIVERED
                  reserves stock        |  \                    |
                  (Idempotency-Key)     |   `-- ttl elapsed --> EXPIRED   (stock released)
                                        `-- cancel -----------> CANCELLED (stock released)
                                  PAID --cancel---------------> CANCELLED (stock restocked)
```

Stock has two numbers per product: `on_hand` (physical) and `reserved` (promised
to unpaid orders). **available = on_hand - reserved.**

| Event | on_hand | reserved | reservation row | ledger reason |
|---|---|---|---|---|
| checkout | - | +q | `held` | `reserve` |
| pay succeeds | -q | -q | `committed` | `commit` |
| cancel / expire an UNPAID order | - | -q | `released` | `release` |
| cancel a PAID order | +q | - | `released` | `restock` |

Every row of this table is one SQL transaction that also writes the ledger row
and an audit row, so the books can never disagree with the stock columns.

## 2. The checkout transaction (`POST /orders`, header `Idempotency-Key`)

All inside **one** transaction at `READ COMMITTED`:

1. `INSERT idempotency_keys (user_id, idem_key, request_hash)`. `UNIQUE(user_id, idem_key)`.
   * duplicate key => the original has committed (our insert waited for it) =>
     **replay** its stored response, or `422` if the request hash differs.
2. `SELECT ... FROM users WHERE id=? FOR UPDATE`. One checkout per user at a time.
3. Read the cart joined to products `FOR SHARE` (price and `is_active` cannot change under us).
4. For each cart line **in ascending `product_id`**: `SELECT ... FROM inventory FOR UPDATE`; collect every shortage.
5. Any shortage => `ROLLBACK` => `409 INSUFFICIENT_STOCK` listing all short items. The rollback also un-claims the key, so the client can retry with the **same** key after stock returns.
6. `UPDATE inventory SET reserved = reserved + q`, insert order, items (price/name snapshots), reservations (`held`), ledger rows, delete cart, audit row.
7. Store the response JSON in the idempotency row, then `COMMIT`.

If MySQL kills the transaction (deadlock 1213 / lock-wait timeout 1205), `tx_run`
re-runs it from step 1, up to 6 times with jittered backoff; if that fails the
client gets `503 DB_BUSY` and may retry with the same key.

### Why this is correct

**No overselling.** Two transactions cannot both pass step 4 for the same
product: the second blocks on the `FOR UPDATE` row lock until the first commits,
then reads the already-incremented `reserved`. This does not rely on isolation
level (we lock explicitly) and does not rely on any process-local mutex, so it
holds with any number of server threads or processes.
**Defence in depth:** `CHECK (reserved <= on_hand)` on `inventory` makes the
database itself refuse an oversell even if the application check were wrong.
(Verified: with the application check deliberately disabled, 300 concurrent
buyers of 1 unit produced exactly 1 order; the other 299 were stopped by the
constraint.)

**No deadlocks from our own code.** Lock order is fixed everywhere:

```
idempotency row -> user row -> order row -> inventory rows (ascending product_id)
```

A deadlock needs two transactions taking the same two locks in opposite
orders; no path does. Measured: 150 concurrent carts of 2-4 random products
across 5 products produced **0** deadlock retries; deliberately reading the
cart in random order produced **97** (all absorbed by the retry loop, results
still correct). Retry is the safety net, ordering is the design.

**Idempotency across crashes and timeouts.** The key row and the order become
visible in the same commit. If the response is lost (client disconnect,
timeout, server killed after commit), retrying with the same key returns the
original order; if the transaction never committed, no trace of it exists and
the retry simply does the work. The request hash is over a canonical form
(not raw bytes), so reformatted JSON is still "the same request".

**One order per cart.** Concurrent checkouts with *different* keys by the same
user serialise on the user row; the second finds an empty cart.

## 3. Payment (`POST /orders/{id}/pay`) - simulated

The provider is `src/payment_sim.c`: a pure function of (scenario, attempt number).
Scenarios `success | decline | timeout | flaky` (flaky: odd attempts decline,
even succeed). No card data exists anywhere. Boundary with a real provider: a
real integration must record a `pending` attempt, **commit**, call the provider
**outside** any DB transaction using an idempotency key, then record the result;
here the call is an in-memory function so the attempt and its effect share one transaction.

* Locks the order row (`FOR UPDATE`), so concurrent pays serialise: exactly one
  attempt can succeed, later calls see `PAID` and return 200 without charging.
* A failed/timed-out attempt is recorded and committed; the reservation stays held and the user may retry.
* Re-checks the deadline **under the lock**: an overdue order is expired and the
  response is `409 ORDER_EXPIRED`; no money is taken for stock we no longer hold.

## 4. Cancel and expiry

* Cancel is idempotent (`CANCELLED` stays `CANCELLED`, 200, nothing released twice).
  `PENDING_PAYMENT -> CANCELLED` releases; `PAID -> CANCELLED` restocks. Anything
  later (`SHIPPED`, `DELIVERED`, `EXPIRED`) is `409 ILLEGAL_TRANSITION`.
* A sweeper thread expires overdue orders every `SF_EXPIRY_SWEEP_MS`. It
  re-checks status and deadline under the order lock, so it cannot clobber a
  payment or cancel that won the race. Correctness never depends on the sweeper
  running (payment checks the deadline itself).
* Status changes are compare-and-set (`WHERE status=? AND version=?`) and gated by
  `os_can_transition()`, the only definition of legal transitions.

## 5. Invariants and the tests that guard them

`docs/ARCHITECTURE.md` section 5 lists I1-I10. Cross-table ones are checked by
`src/reconcile.c` (admin `GET /admin/reconcile`, CLI `build/rel/reconcile`) **and** by an
independently written Python restatement in `tests/integration/dbtools.py`.

| Guarantee | Test(s) |
|---|---|
| Never oversell, 1 unit / 300 buyers | `test_last_unit_300_buyers_exactly_one_wins` |
| Exactly N winners for N units | `test_ten_units_two_hundred_buyers...`, `test_multi_quantity_never_oversells...` |
| Duplicate delivery creates one order | `test_sixty_identical_requests_create_one_order`, `test_replay_returns_the_same_order...` |
| Same key + different request rejected | `test_same_key_different_request_is_rejected` |
| One cart -> one order | `test_one_cart_many_keys_becomes_one_order` |
| Failed checkout leaves no partial state, key reusable | `test_insufficient_stock_is_all_or_nothing_and_retryable` |
| Abandoned request + retry never duplicates | `test_client_disconnect_then_retry_never_duplicates` |
| No deadlock failures under cross-product contention | `test_cross_product_contention_has_no_failed_requests` |
| Checkout vs admin stock changes | `test_checkout_racing_admin_stock_removal` |
| Charged exactly once | `test_thirty_concurrent_payments_charge_once` |
| Pay vs cancel / pay vs deadline races | `test_pay_and_cancel_race...`, `test_payment_racing_the_deadline...` |
| Cancel releases / restocks exactly once | `test_repeated_and_concurrent_cancels...`, `test_cancel_after_payment_restocks` |
| Overdue order cannot be paid even if unswept | `LazyExpiry.test_overdue_order_cannot_be_paid_even_if_unswept` |
| Legal transitions only | `tests/unit/test_order_state.c` (full 6x6 matrix) |
| Killed mid-checkout leaves no half-order; retries converge | `CrashRecovery.test_sigkill_mid_checkout...` |
| Graceful shutdown under load loses nothing acknowledged | `CrashRecovery.test_sigterm_under_load...` |
| Random op mixes keep every invariant | `Chaos.test_random_operations_preserve_every_invariant` (seeded, reproducible) |
| The reconcile checker catches corruption | `test_reconcile_detects_corruption` |

**The tests are themselves tested.** `tests/mutation/mutate.py` breaks one line
of the engine at a time (remove a row lock, skip the stock check, ignore the
idempotency key, remove the deadline re-check, ...) in a temp copy, and requires
the guarding tests to fail. Result: 12/12 mutants killed.

## 6. Known limitations (stated, not hidden)

* **Crash recovery and durability are InnoDB's job.** We test the *application's* transaction boundaries and the retry protocol, not MySQL's redo log.
* **Commit with unknown outcome** (connection dies during `COMMIT`): the client sees 503 and must retry with the same key; that is correct for checkout (idempotency key) but pay/cancel rely on their state-based idempotency instead. A dropped DB connection mid-checkout is exercised in Phase 6 (needs controlling the DB container).
* **Single MySQL primary.** Row locks serialise correctly there; a multi-primary or replica-read setup would need different reasoning.
* **Carts are advisory**; stock is only claimed at checkout. A user can hold a cart for items that later sell out, and is told at checkout.
* **The 15-minute reservation (`SF_RESERVATION_TTL_S`) is a product decision:** shorter frees stock faster but frustrates slow payers; longer lets abandoned carts-in-checkout starve real buyers.
* **Hot-row throughput:** every buyer of one product serialises on its inventory row. Fine for retail volumes; a flash sale on a single SKU is bounded by lock hand-off time (Phase 6 measures this).
* The `Idempotent-Replay` response header and stored response mean a replay returns the order *as it was at creation*, not its current status. Poll `GET /orders/{id}` for current state.
