# Demo script and presentation outline

A 12-minute walkthrough that shows the system working, then breaks it on purpose. Everything
here uses the demo seed; run `scripts/dev.sh sh scripts/reset-demo.sh` first so the numbered
keyboard `SF-KB-FOUNDER` has its single unit back. Setup: [INSTALL.md](INSTALL.md).

Have three things open: the storefront (`/`), the admin console (`/admin/`) in a second
window, and a terminal in the repository root.

## Part 1: the product (3 min)

1. **Storefront.** Point out the stock tally on each card: one notch per unit on the shelf,
   amber when low, a single ink notch and the words "Last one" for the final unit. The state is
   shown in text and shape as well as colour. *Say:* "The number on this page is the same number
   the database enforces."
2. **Sign in** as `ava@stockforge.test`. Open the Founder's Edition keyboard (1 left). Add it to
   the cart.
3. **Check out with a declined card.** In the payment step choose "Card is declined".
   The order exists, the keyboard is held for 15 minutes, and the page says so. Choose "Payment succeeds"
   and pay. The order becomes Paid. *Say:* "A failed payment never loses the reservation, and a
   successful one cannot be charged twice."
4. **Admin console, Overview.** The order appears in "Ready to ship". Mark it shipped. Show the
   low-stock list and the revenue chart (all figures are computed from the database, not
   invented; the test suite cross-checks them with independent SQL).

## Part 2: the claim, proven live (4 min)

*The claim:* two people can never both buy the last unit, and retrying never double-orders.

The integration suite runs this race for real: 300 concurrent buyers, one unit.

```bash
scripts/dev.sh sh scripts/it.sh test_checkout_concurrency.ConcurrentCheckout.test_last_unit_300_buyers_exactly_one_wins
scripts/dev.sh sh scripts/it.sh test_checkout   # idempotent replay, key reuse, all-or-nothing stock
scripts/dev.sh build/rel/reconcile              # every invariant, against the live database
```

*Say:* "Three hundred buyers race for one unit: exactly one gets `201`, the other 299 get
`409 INSUFFICIENT_STOCK`, `reserved` never exceeds `on_hand`, and the database holds exactly one
order line and one reservation for that product." (The test asserts each of those; the output is the
evidence.) The same file also sends sixty identical requests and proves one order is created.

## Part 3: breaking it on purpose (3 min)

```bash
scripts/dev.sh sh scripts/it.sh test_db_faults   # the database is cut at every statement of checkout, pay, cancel
```

*Say:* "A proxy sits between the server and MySQL and kills the connection after statement 1, 2,
3 and so on, in every workflow. After each cut the retry either finds nothing or finds the finished
order, and the invariants hold." Then show the mutation harness: "I also broke the engine's code on
purpose, one line at a time, and demanded that the tests notice."

```bash
scripts/dev.sh python3 tests/mutation/mutate.py   # every mutant must be KILLED
```

## Part 4: operations and security (2 min)

1. Console **System** page: workers busy, queue depth, requests shed with 503, latency
   histogram, "Run consistency check" (every invariant, live).
2. `curl -k https://localhost:8443/metrics` returns `403`: the metrics are not public. The same
   figures are available only to an administrator, in the console.
3. One security sentence per control: Argon2id passwords, hashed session tokens, CSRF token plus
   Origin check, strict CSP with no inline script and no `innerHTML` anywhere, least-privilege
   database user, secrets only from the environment. Full table: [SECURITY.md](SECURITY.md).

## Presentation outline (8 slides)

| # | Slide | One-line point | Evidence in the repo |
|---|---|---|---|
| 1 | The problem | A retail checkout is a distributed-systems problem in miniature: overselling, double charges, half-finished orders. | [CHECKOUT.md](CHECKOUT.md) |
| 2 | Architecture | Apache, a pthreads C server with a bounded queue, MySQL. Overload sheds load explicitly. | [ARCHITECTURE.md](ARCHITECTURE.md) section 0 |
| 3 | The checkout transaction | One transaction, fixed lock order, idempotency key, database `CHECK` as a backstop. | [CHECKOUT.md](CHECKOUT.md) |
| 4 | Proof by attack | 300 buyers for one unit; the database cut at every statement; mutated code caught. | [FAILURE_TESTING.md](FAILURE_TESTING.md) |
| 5 | Bugs the tests found | Real defects found by the suite (list in FAILURE_TESTING.md and PHASE4_NOTES.md), each with its fix. | git history |
| 6 | Security | Threat model, controls, and the test proving each; residual risks stated plainly. | [SECURITY.md](SECURITY.md) |
| 7 | Measured performance | Median and range of repeated runs; where the bottleneck is and what was ruled out. | [BENCHMARKS.md](BENCHMARKS.md) |
| 8 | Honest limits | Single node, simulated payment provider, per-process rate limits, no keep-alive. What I would do next, and what I would measure first. | [SECURITY.md](SECURITY.md), [BENCHMARKS.md](BENCHMARKS.md) |

## Questions to expect

* *Why not `SELECT ... FOR UPDATE` on a single stock counter?* It serialises every checkout;
  locking only the rows of the products in the cart, in ascending id order, serialises only real
  conflicts and makes deadlock impossible between checkouts. Measured in BENCHMARKS.md (hot-row
  contention).
* *What if the server dies after the database commits but before the client hears?* The client
  retries with the same `Idempotency-Key` and receives the stored response (`Idempotent-Replay: true`).
* *What is simulated?* The payment provider. Everything about how its outcomes are handled
  (declines, timeouts, retries, exactly-one-success) is real and tested.
* *What would break first at ten times the load?* See the bottleneck analysis in BENCHMARKS.md;
  the honest answer there is a measurement, not a guess.
