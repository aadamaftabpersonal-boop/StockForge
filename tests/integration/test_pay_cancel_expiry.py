"""Payment (simulated), cancellation and reservation expiry: state transitions,
stock accounting, repeat/concurrent calls, and races between them."""
import random
import time
import unittest
import uuid
from collections import Counter

import dbtools
from harness import ADMIN, Client, Server, checkout, logged_in, metrics, run_parallel, session_client


def pay(c, oid, scenario=None):
    return c.post(f"/api/v1/orders/{oid}/pay", {"scenario": scenario} if scenario else None)


def cancel(c, oid):
    return c.post(f"/api/v1/orders/{oid}/cancel")


def counts(oid, what):
    return int(dbtools.scalar(what.replace("{oid}", f"(SELECT id FROM orders WHERE public_id='{oid}')")))


class Base(unittest.TestCase):
    SERVER_ENV = {}

    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(**cls.SERVER_ENV)
        cls.admin = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def tearDown(self):
        self.assertEqual(dbtools.invariant_violations(), {})

    def place(self, stock=10, qty=2, price=1000):
        """A buyer with one PENDING_PAYMENT order. Returns (client, order_id, product_id, user_id)."""
        p = dbtools.new_product(stock, price)
        u = dbtools.make_users(1)[0]
        dbtools.put_cart(u[0], [(p, qty)])
        c = session_client(self.srv, u)
        r = checkout(c)
        self.assertEqual(r.status, 201, r.text)
        return c, r.json["order"]["id"], p, u[0]


class PayAndCancel(Base):
    SERVER_ENV = {"SF_WORKERS": 8}

    # ------------------------------------------------------------------ pay
    def test_successful_payment_commits_stock(self):
        c, oid, p, _ = self.place(10, 2)
        self.assertEqual(dbtools.stock(p), (10, 2))
        r = pay(c, oid)
        self.assertEqual(r.status, 200, r.text)
        o = r.json["order"]
        self.assertEqual((o["status"], o["expires_at"]), ("PAID", None))
        self.assertEqual([(x["attempt"], x["status"]) for x in o["payments"]], [(1, "succeeded")])
        self.assertEqual(dbtools.stock(p), (8, 0))                        # on_hand really decreases now
        self.assertEqual(dbtools.q(f"SELECT status FROM stock_reservations WHERE product_id={p}"), [["committed"]])
        self.assertEqual(dbtools.q(f"SELECT delta_on_hand,delta_reserved FROM inventory_ledger "
                                   f"WHERE product_id={p} AND reason='commit'"), [["-2", "-2"]])
        self.assertEqual(counts(oid, "SELECT COUNT(*) FROM audit_events WHERE entity_id='%s' AND action='order.paid'" % oid), 1)
        self.assertEqual(Client(self.srv).get(f"/api/v1/products/{p}").json["product"]["available"], 8)

    def test_repeated_payment_is_idempotent(self):
        c, oid, p, _ = self.place(10, 2)
        self.assertEqual(pay(c, oid).status, 200)
        for _ in range(3):
            r = pay(c, oid)
            self.assertEqual((r.status, r.json["order"]["status"]), (200, "PAID"))
        self.assertEqual(dbtools.stock(p), (8, 0))                         # deducted ONCE
        self.assertEqual(counts(oid, "SELECT COUNT(*) FROM payment_attempts WHERE order_id={oid}"), 1)
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE product_id={p} AND reason='commit'"), "1")

    def test_thirty_concurrent_payments_charge_once(self):
        c, oid, p, _ = self.place(10, 3)
        res = run_parallel(30, lambda i: pay(c, oid))
        self.assertEqual(Counter(r.status for r in res), Counter({200: 30}))
        self.assertEqual(dbtools.stock(p), (7, 0))
        self.assertEqual(counts(oid, "SELECT COUNT(*) FROM payment_attempts WHERE order_id={oid} AND status='succeeded'"), 1)

    def test_decline_keeps_the_reservation_and_allows_retry(self):
        c, oid, p, _ = self.place(10, 2)
        r = pay(c, oid, "decline")
        self.assertEqual((r.status, r.json["error"]["code"]), (402, "PAYMENT_DECLINED"))
        self.assertEqual(dbtools.stock(p), (10, 2))                        # still held, nothing deducted
        self.assertEqual(c.get(f"/api/v1/orders/{oid}").json["order"]["status"], "PENDING_PAYMENT")
        r = pay(c, oid, "success")
        self.assertEqual((r.status, [x["status"] for x in r.json["order"]["payments"]]), (200, ["failed", "succeeded"]))
        self.assertEqual(dbtools.stock(p), (8, 0))

    def test_timeout_scenario(self):
        c, oid, p, _ = self.place(10, 1)
        r = pay(c, oid, "timeout")
        self.assertEqual((r.status, r.json["error"]["code"]), (504, "PAYMENT_TIMEOUT"))
        self.assertEqual(counts(oid, "SELECT COUNT(*) FROM payment_attempts WHERE order_id={oid} AND status='timed_out'"), 1)
        self.assertEqual(dbtools.stock(p), (10, 1))

    def test_flaky_fails_then_succeeds_deterministically(self):
        c, oid, p, _ = self.place(10, 1)
        self.assertEqual(pay(c, oid, "flaky").status, 402)
        self.assertEqual(pay(c, oid, "flaky").status, 200)
        self.assertEqual(dbtools.stock(p), (9, 0))

    def test_bad_scenario_and_ids(self):
        c, oid, p, _ = self.place(10, 1)
        self.assertEqual(pay(c, oid, "magic").status, 422)
        self.assertEqual(c.post(f"/api/v1/orders/{oid}/pay", {"scenario": 5}).status, 422)
        for bad in ("short", "0" * 26, oid.lower(), "01HZ'; DROP TABLE orders;--"):
            self.assertEqual(c.post(f"/api/v1/orders/{bad.replace(' ', '%20')}/pay").status, 404, bad)
        self.assertEqual(dbtools.stock(p), (10, 1))

    def test_other_users_cannot_pay_or_cancel(self):
        c, oid, p, _ = self.place(10, 1)
        other = session_client(self.srv, dbtools.make_users(1)[0])
        self.assertEqual(pay(other, oid).status, 404)
        self.assertEqual(cancel(other, oid).status, 404)
        self.assertEqual(Client(self.srv).post(f"/api/v1/orders/{oid}/pay").status, 401)
        self.assertEqual(c.post(f"/api/v1/orders/{oid}/pay", csrf=False).status, 403)
        self.assertEqual(dbtools.stock(p), (10, 1))

    # --------------------------------------------------------------- cancel
    def test_cancel_pending_releases_the_reservation(self):
        c, oid, p, _ = self.place(10, 4)
        r = cancel(c, oid)
        self.assertEqual((r.status, r.json["order"]["status"]), (200, "CANCELLED"))
        self.assertEqual(dbtools.stock(p), (10, 0))
        self.assertEqual(dbtools.q(f"SELECT status FROM stock_reservations WHERE product_id={p}"), [["released"]])
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE product_id={p} AND reason='release'"), "1")
        self.assertEqual(Client(self.srv).get(f"/api/v1/products/{p}").json["product"]["available"], 10)

    def test_repeated_and_concurrent_cancels_release_once(self):
        c, oid, p, _ = self.place(10, 4)
        res = run_parallel(20, lambda i: cancel(c, oid))
        self.assertEqual(Counter(r.status for r in res), Counter({200: 20}))
        self.assertEqual(dbtools.stock(p), (10, 0))
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE product_id={p} AND reason='release'"), "1")
        self.assertEqual(cancel(c, oid).status, 200)

    def test_cancel_after_payment_restocks(self):
        c, oid, p, _ = self.place(10, 3)
        pay(c, oid)
        self.assertEqual(dbtools.stock(p), (7, 0))
        res = run_parallel(15, lambda i: cancel(c, oid))
        self.assertEqual(Counter(r.status for r in res), Counter({200: 15}))
        self.assertEqual(dbtools.stock(p), (10, 0))                        # restored exactly once
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE product_id={p} AND reason='restock'"), "1")
        self.assertEqual(c.get(f"/api/v1/orders/{oid}").json["order"]["status"], "CANCELLED")

    def test_cancelled_order_cannot_be_paid(self):
        c, oid, p, _ = self.place(10, 2)
        cancel(c, oid)
        r = pay(c, oid)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "ILLEGAL_TRANSITION"))
        self.assertEqual(dbtools.stock(p), (10, 0))
        self.assertEqual(counts(oid, "SELECT COUNT(*) FROM payment_attempts WHERE order_id={oid}"), 0)

    def test_shipped_orders_cannot_be_cancelled(self):
        c, oid, p, _ = self.place(10, 2)
        pay(c, oid)
        dbtools.q(f"UPDATE orders SET status='SHIPPED', version=version+1 WHERE public_id='{oid}'")
        r = cancel(c, oid)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "ILLEGAL_TRANSITION"))
        self.assertEqual(dbtools.stock(p), (8, 0))
        self.assertEqual(pay(c, oid).status, 200)                          # already paid: idempotent success

    def test_full_lifecycle_returns_all_stock(self):
        c, oid, p, _ = self.place(10, 5)
        pay(c, oid)
        cancel(c, oid)
        self.assertEqual(dbtools.stock(p), (10, 0))
        self.assertEqual(dbtools.q(f"SELECT reason,delta_on_hand,delta_reserved FROM inventory_ledger "
                                   f"WHERE product_id={p} ORDER BY id"),
                         [["seed", "10", "0"], ["reserve", "0", "5"], ["commit", "-5", "-5"], ["restock", "5", "0"]])

    def test_pay_and_cancel_race_ends_in_exactly_one_state(self):
        orders = [self.place(10, 2) for _ in range(30)]

        def act(i):
            c, oid, p, _ = orders[i // 2]
            return pay(c, oid) if i % 2 == 0 else cancel(c, oid)

        res = run_parallel(60, act)
        self.assertFalse([r for r in res if isinstance(r, Exception)])
        # Cancelling a PAID order is legal, so the final state is CANCELLED either way.
        # What differs is the path, and the ledger must show exactly one of two valid histories:
        #   cancel first: reserve, release                (the late payment gets 409)
        #   pay first:    reserve, commit, restock        (payment 200, then the cancel)
        paths = Counter()
        for k, (c, oid, p, _) in enumerate(orders):
            self.assertEqual(c.get(f"/api/v1/orders/{oid}").json["order"]["status"], "CANCELLED")
            self.assertEqual(dbtools.stock(p), (10, 0))
            hist = tuple(r[0] for r in dbtools.q(f"SELECT reason FROM inventory_ledger WHERE product_id={p} ORDER BY id"))
            self.assertIn(hist, [("seed", "reserve", "release"), ("seed", "reserve", "commit", "restock")], hist)
            pay_code, cancel_code = res[2 * k].status, res[2 * k + 1].status
            self.assertEqual(cancel_code, 200)
            self.assertEqual(pay_code, 200 if len(hist) == 4 else 409, (hist, pay_code))
            paths[hist[-1]] += 1
        print(f"\n    [info] pay-vs-cancel interleavings: {dict(paths)}")

    def test_metrics_and_audit_trail(self):
        before = metrics(self.srv)["orders"]
        c, oid, p, _ = self.place(10, 1)
        pay(c, oid, "decline")
        pay(c, oid)
        c2, oid2, _, _ = self.place(10, 1)
        cancel(c2, oid2)
        m = metrics(self.srv)["orders"]
        self.assertEqual((m["paid"] - before["paid"], m["payment_failed"] - before["payment_failed"],
                          m["cancelled"] - before["cancelled"]), (1, 1, 1))
        actions = {r[0] for r in dbtools.q(f"SELECT action FROM audit_events WHERE entity_id IN ('{oid}','{oid2}')")}
        self.assertEqual(actions, {"order.created", "order.payment_failed", "order.paid", "order.cancelled"})


class Expiry(Base):
    SERVER_ENV = {"SF_WORKERS": 8, "SF_EXPIRY_SWEEP_MS": 200}

    def wait_status(self, c, oid, want, timeout=10):
        end = time.time() + timeout
        st = None
        while time.time() < end:
            st = c.get(f"/api/v1/orders/{oid}").json["order"]["status"]
            if st == want:
                return st
            time.sleep(0.1)
        return st

    def test_sweeper_expires_overdue_orders_and_frees_stock(self):
        c, oid, p, _ = self.place(10, 3)
        self.assertEqual(dbtools.stock(p), (10, 3))
        dbtools.q(f"UPDATE orders SET expires_at = NOW(3) - INTERVAL 1 SECOND WHERE public_id='{oid}'")
        self.assertEqual(self.wait_status(c, oid, "EXPIRED"), "EXPIRED")
        self.assertEqual(dbtools.stock(p), (10, 0))
        self.assertEqual(dbtools.q(f"SELECT status FROM stock_reservations WHERE product_id={p}"), [["released"]])
        self.assertEqual(dbtools.q(f"SELECT actor_user_id FROM audit_events WHERE entity_id='{oid}' AND action='order.expired'"),
                         [["NULL"]])                                       # system actor
        self.assertGreaterEqual(metrics(self.srv)["orders"]["expired"], 1)
        r = pay(c, oid)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "ILLEGAL_TRANSITION"))

    def test_expired_stock_can_be_bought_by_someone_else(self):
        c1, oid, p, _ = self.place(1, 1)                                   # the last unit, held by buyer 1
        c2 = session_client(self.srv, dbtools.make_users(1)[0])
        dbtools.put_cart(int(dbtools.scalar("SELECT MAX(id) FROM users")), [(p, 1)])
        self.assertEqual(checkout(c2).status, 409)                         # blocked while held
        dbtools.q(f"UPDATE orders SET expires_at = NOW(3) - INTERVAL 1 SECOND WHERE public_id='{oid}'")
        self.wait_status(c1, oid, "EXPIRED")
        self.assertEqual(checkout(c2).status, 201)                         # now it is free
        self.assertEqual(dbtools.stock(p), (1, 1))

    def test_paid_orders_never_expire(self):
        c, oid, p, _ = self.place(10, 2)
        pay(c, oid)
        time.sleep(1.0)                                                    # several sweeps
        self.assertEqual(c.get(f"/api/v1/orders/{oid}").json["order"]["status"], "PAID")
        self.assertEqual(dbtools.stock(p), (8, 0))

    def test_sweeper_drains_a_large_backlog(self):
        orders = [self.place(5, 1) for _ in range(120)]                    # > one 50-order batch
        ids = ",".join(f"'{o[1]}'" for o in orders)
        dbtools.q(f"UPDATE orders SET expires_at = NOW(3) - INTERVAL 1 SECOND WHERE public_id IN ({ids})")
        end = time.time() + 30
        left = 120
        while time.time() < end and left:
            left = int(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE public_id IN ({ids}) AND status='PENDING_PAYMENT'"))
            time.sleep(0.2)
        self.assertEqual(left, 0)
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE public_id IN ({ids}) AND status='EXPIRED'"), "120")

    def test_payment_racing_the_deadline_is_all_or_nothing(self):
        orders = [self.place(5, 1) for _ in range(40)]
        ids = ",".join(f"'{o[1]}'" for o in orders)
        dbtools.q(f"UPDATE orders SET expires_at = NOW(3) + INTERVAL 1200000 MICROSECOND WHERE public_id IN ({ids})")
        rnd = random.Random(3)
        delays = [rnd.uniform(0.0, 2.4) for _ in orders]

        def act(i):
            time.sleep(delays[i])
            return pay(orders[i][0], orders[i][1])

        res = run_parallel(len(orders), act)
        self.assertFalse([r for r in res if isinstance(r, Exception)])
        time.sleep(1.0)                                                    # let the sweeper finish stragglers
        final = Counter()
        for (c, oid, p, _), r in zip(orders, res):
            st = c.get(f"/api/v1/orders/{oid}").json["order"]["status"]
            final[st] += 1
            self.assertIn(st, ("PAID", "EXPIRED"))
            self.assertEqual(dbtools.stock(p), (4, 0) if st == "PAID" else (5, 0))
            self.assertEqual(r.status, 200 if st == "PAID" else 409, (st, r.status, r.text))
        print(f"\n    [info] pay-vs-deadline race outcomes: {dict(final)}")


class LazyExpiry(Base):
    """Sweeper effectively disabled: correctness must not depend on it."""
    SERVER_ENV = {"SF_WORKERS": 4, "SF_EXPIRY_SWEEP_MS": 3600000}

    def test_overdue_order_cannot_be_paid_even_if_unswept(self):
        c, oid, p, _ = self.place(10, 2)
        dbtools.q(f"UPDATE orders SET expires_at = NOW(3) - INTERVAL 1 SECOND WHERE public_id='{oid}'")
        r = pay(c, oid)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "ORDER_EXPIRED"))
        self.assertEqual(c.get(f"/api/v1/orders/{oid}").json["order"]["status"], "EXPIRED")
        self.assertEqual(dbtools.stock(p), (10, 0))                        # stock released, no money taken
        self.assertEqual(counts(oid, "SELECT COUNT(*) FROM payment_attempts WHERE order_id={oid}"), 0)


if __name__ == "__main__":
    unittest.main()
