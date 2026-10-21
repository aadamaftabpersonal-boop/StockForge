"""Database failure injection through a fault-proxy (see faultproxy.py).

The same question is asked at every point of each workflow: if the database
connection dies RIGHT HERE (before the statement ran, or after it ran but
before the client heard back), is the DATABASE still consistent, and does a
retry converge to exactly the right final state?"""
import time
import unittest
import uuid

import dbtools
from faultproxy import FaultProxy
from harness import ADMIN, Client, Server, checkout, get, logged_in, metrics, run_parallel, session_client


def pay(c, oid, scenario="success"):
    return c.post(f"/api/v1/orders/{oid}/pay", {"scenario": scenario})


def cancel(c, oid):
    return c.post(f"/api/v1/orders/{oid}/cancel")


class FaultBase(unittest.TestCase):
    SERVER_ENV = {}

    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.proxy = FaultProxy(dbtools.HOST)
        env = dict(SF_DB_HOST="127.0.0.1", SF_DB_PORT=cls.proxy.port, SF_WORKERS=4,
                   SF_EXPIRY_SWEEP_MS=3600000, SF_DB_TIMEOUT_S=3)
        env.update(cls.SERVER_ENV)
        cls.srv = Server(**env)

    @classmethod
    def tearDownClass(cls):
        cls.proxy.clear()
        rc = cls.srv.stop()
        cls.proxy.close()
        assert rc == 0

    def setUp(self):
        self.proxy.clear()

    def tearDown(self):
        self.proxy.clear()
        self.assertEqual(dbtools.invariant_violations(), {})

    def sweep(self, make, label, kmax=150, min_points=10):
        """make() -> object with call(), snapshot(), retry(), check(first, snap, second).
        Cuts the connection at every command index k of the transaction, in both
        'before' and 'after' flavours, until k exceeds the transaction length."""
        points, spent = {}, {"fixture": 0.0, "faulted call": 0.0, "invariants": 0.0, "retry": 0.0, "check": 0.0}
        clock = time.perf_counter
        for action in ("before", "after"):
            n = 0
            for k in range(1, kmax):
                t = clock()
                sc = make()
                spent["fixture"] += clock() - t
                fired = self.proxy.arm("nth", k=k, action=action)
                t = clock()
                first = sc.call()
                spent["faulted call"] += clock() - t
                self.proxy.clear()
                if not fired.is_set():
                    break                                      # k is past the end of the transaction
                n += 1
                self.assertIn(first.status, (200, 201, 402, 409, 503), f"{label} {action}@{k}: {first.status} {first.text[:200]}")
                self.assertNotEqual(first.status, 500)
                t = clock()
                self.assertEqual(dbtools.invariant_violations(), {}, f"{label} {action}@{k}: books unbalanced right after the fault")
                snap = sc.snapshot()
                spent["invariants"] += clock() - t
                t = clock()
                second = sc.retry()
                spent["retry"] += clock() - t
                t = clock()
                try:
                    sc.check(first, snap, second)
                except AssertionError as e:
                    raise AssertionError(f"{label}: fault '{action}' at command {k}: {e}") from e
                spent["check"] += clock() - t
            points[action] = n
        self.assertGreaterEqual(min(points.values()), min_points, f"{label}: sweep covered too few points {points}")
        total = sum(points.values())
        print(f"\n    [info] {label}: faulted at {points['before']} + {points['after']} distinct points; all converged; "
              f"avg per point: " + ", ".join(f"{k} {v / total * 1000:.0f}ms" for k, v in spent.items()))


class CheckoutFaultSweep(FaultBase):
    def make(self):
        user, (p1, p2) = dbtools.buyer_with_cart([(10, 1000, 2), (5, 2500, 1)])
        c = session_client(self.srv, user)
        key = "fault-" + uuid.uuid4().hex
        test = self

        class S:
            def call(s): return checkout(c, key)
            def snapshot(s): return int(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={user[0]}"))
            def retry(s): return checkout(c, key)

            def check(s, first, committed_before_retry, second):
                test.assertEqual(second.status, 201, second.text[:300])
                test.assertEqual(second.headers["idempotent-replay"], "true" if committed_before_retry else "false")
                test.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={user[0]}"), "1")   # never two
                test.assertEqual((dbtools.stock(p1), dbtools.stock(p2)), ((10, 2), (5, 1)))                   # reserved once
                test.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM cart_items WHERE user_id={user[0]}"), "0")
        return S()

    def test_every_point_of_checkout(self):
        self.sweep(self.make, "checkout", min_points=30)
        print(f"    [info] longest checkout transaction observed: {self.proxy.max_txn_cmds} commands")


class PayFaultSweep(FaultBase):
    def make(self):
        user, (p,) = dbtools.buyer_with_cart([(10, 1000, 2)])
        c = session_client(self.srv, user)
        oid = checkout(c).json["order"]["id"]
        test = self

        class S:
            def call(s): return pay(c, oid)
            def snapshot(s): return dbtools.scalar(f"SELECT status FROM orders WHERE public_id='{oid}'")
            def retry(s): return pay(c, oid)

            def check(s, first, status_before_retry, second):
                test.assertEqual(second.status, 200, second.text[:300])
                test.assertEqual(second.json["order"]["status"], "PAID")
                test.assertEqual(dbtools.stock(p), (8, 0))                                          # deducted exactly once
                test.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM payment_attempts WHERE status='succeeded' AND "
                                                f"order_id=(SELECT id FROM orders WHERE public_id='{oid}')"), "1")
        return S()

    def test_every_point_of_payment(self):
        self.sweep(self.make, "pay", min_points=15)


class CancelFaultSweep(FaultBase):
    def make_cancel(self, paid):
        user, (p,) = dbtools.buyer_with_cart([(10, 1000, 3)])
        c = session_client(self.srv, user)
        oid = checkout(c).json["order"]["id"]
        if paid:
            self.assertEqual(pay(c, oid).status, 200)
        test = self

        class S:
            def call(s): return cancel(c, oid)
            def snapshot(s): return dbtools.scalar(f"SELECT status FROM orders WHERE public_id='{oid}'")
            def retry(s): return cancel(c, oid)

            def check(s, first, status_before_retry, second):
                test.assertEqual(second.status, 200, second.text[:300])
                test.assertEqual(second.json["order"]["status"], "CANCELLED")
                test.assertEqual(dbtools.stock(p), (10, 0))                                         # released/restocked once
                reasons = [r[0] for r in dbtools.q(f"SELECT reason FROM inventory_ledger WHERE product_id={p} ORDER BY id")]
                test.assertIn(reasons, (["seed", "reserve", "release"], ["seed", "reserve", "commit", "restock"]))
        return S()

    def test_every_point_of_cancelling_an_unpaid_order(self):
        self.sweep(lambda: self.make_cancel(False), "cancel(unpaid)", min_points=10)

    def test_every_point_of_cancelling_a_paid_order(self):
        self.sweep(lambda: self.make_cancel(True), "cancel(paid)", min_points=10)


class OutcomeUnknown(FaultBase):
    """The classic distributed-systems trap, shown explicitly: COMMIT was sent
    and the connection died. Did it commit? The client cannot know; the
    idempotency key makes it irrelevant."""

    def setup_buyer(self):
        user, (p,) = dbtools.buyer_with_cart([(10, 1000, 1)])
        return user, p, session_client(self.srv, user), "unknown-" + uuid.uuid4().hex

    def test_connection_lost_BEFORE_commit_reaches_mysql(self):
        user, p, c, key = self.setup_buyer()
        fired = self.proxy.arm("sql", match=b"COMMIT", action="before")
        r = checkout(c, key)
        self.assertTrue(fired.is_set())
        self.assertEqual(r.status, 503)
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={user[0]}"), "0")   # it did NOT happen
        self.assertEqual(dbtools.stock(p), (10, 0))
        r2 = checkout(c, key)
        self.assertEqual((r2.status, r2.headers["idempotent-replay"]), (201, "false"))                 # retry does the work

    def test_connection_lost_AFTER_commit_executed(self):
        user, p, c, key = self.setup_buyer()
        fired = self.proxy.arm("sql", match=b"COMMIT", action="after")
        r = checkout(c, key)
        self.assertTrue(fired.is_set())
        self.assertEqual(r.status, 503)                                                                # client was told: failed
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={user[0]}"), "1")  # ...but it HAPPENED
        self.assertEqual(dbtools.stock(p), (10, 1))
        r2 = checkout(c, key)
        self.assertEqual((r2.status, r2.headers["idempotent-replay"]), (201, "true"))                  # retry returns that order
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={user[0]}"), "1")
        self.assertEqual(dbtools.stock(p), (10, 1))                                                    # still reserved once

    def test_without_the_key_the_same_retry_would_double_order(self):
        """Control experiment: shows WHY the key matters. A retry with a NEW key after an
        unknown-outcome commit creates a second order (the second one is rejected here only
        because the cart was already consumed by the first)."""
        user, p, c, key = self.setup_buyer()
        self.proxy.arm("sql", match=b"COMMIT", action="after")
        self.assertEqual(checkout(c, key).status, 503)
        r = checkout(c, "different-" + uuid.uuid4().hex)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "EMPTY_CART"))                     # per-user lock + consumed cart saves us
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={user[0]}"), "1")


class Outages(FaultBase):
    def test_database_down_then_recovers_without_restarting_the_server(self):
        c = Client(self.srv)
        self.assertEqual(get(self.srv, "/healthz")[2]["database"], "up")
        self.proxy.down()
        st, h, body = get(self.srv, "/healthz")
        self.assertEqual((st, body["database"], body["status"]), (503, "down", "degraded"))
        self.assertIn("retry-after", h)
        r = c.get("/api/v1/products")
        self.assertEqual((r.status, r.json["error"]["code"]), (503, "DB_UNAVAILABLE"))                  # clean error, no crash
        user, _ = dbtools.buyer_with_cart([(5, 1000, 1)])
        self.assertEqual(checkout(session_client(self.srv, user)).status, 503)
        self.assertIsNone(self.srv.proc.poll())
        self.proxy.up_()
        end = time.time() + 10
        while time.time() < end and get(self.srv, "/healthz")[0] != 200:
            time.sleep(0.2)
        self.assertEqual(get(self.srv, "/healthz")[0], 200)                                             # self-healed
        self.assertEqual(c.get("/api/v1/products?limit=1").status, 200)
        self.assertEqual(checkout(session_client(self.srv, user)).status, 201)

    def test_connections_cut_between_requests_are_reconnected_transparently(self):
        adm = logged_in(self.srv, ADMIN)
        c = Client(self.srv)
        for _ in range(5):
            c.get("/api/v1/products?limit=1")                      # make sure every worker holds a connection
            adm.get("/api/v1/admin/audit?limit=1")
        self.proxy.sever_all()
        for _ in range(8):                                         # every worker must cope with a dead connection
            self.assertEqual(c.get("/api/v1/products?limit=1").status, 200)
        self.proxy.sever_all()
        p = dbtools.new_product(5, 1000)
        for _ in range(8):                                         # transactional routes too
            self.assertEqual(adm.post(f"/api/v1/admin/inventory/{p}/adjust", {"delta": 1, "reason": "restock"}).status, 200)
        self.assertEqual(dbtools.stock(p), (13, 0))
        self.assertEqual(get(self.srv, "/healthz")[0], 200)

    def test_connection_lost_exactly_at_START_TRANSACTION_is_retried_transparently(self):
        """Nothing has run yet at that point, so the server may reconnect and begin again without
        telling the client. (Every transactional route runs a session lookup first, which already
        reconnects, so only a cut precisely AT the begin reaches this path.)"""
        for action in ("before", "after"):
            user, (p,) = dbtools.buyer_with_cart([(10, 1000, 2)])
            c = session_client(self.srv, user)
            oid = checkout(c).json["order"]["id"]
            fired = self.proxy.arm("sql", match=b"START TRANSACTION", action=action)
            r = pay(c, oid)
            self.assertTrue(fired.is_set(), action)
            self.assertEqual(r.status, 200, f"{action}: {r.text[:200]}")                  # the client never saw the failure
            self.assertEqual(dbtools.stock(p), (8, 0), action)
            self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM payment_attempts WHERE order_id="
                                            f"(SELECT id FROM orders WHERE public_id='{oid}')"), "1")

    def test_hung_database_times_out_instead_of_hanging_forever(self):
        c = Client(self.srv)
        for _ in range(8):
            c.get("/api/v1/products?limit=1")
        self.proxy.blackhole = True
        t0 = time.time()
        r = c.get("/api/v1/products?limit=1")
        took = time.time() - t0
        self.assertEqual(r.status, 503)
        self.assertLess(took, 3 + 4, f"request hung {took:.1f}s with SF_DB_TIMEOUT_S=3")                # bounded by the DB timeout
        self.assertIsNone(self.srv.proc.poll())
        self.proxy.blackhole = False
        self.proxy.sever_all()
        end = time.time() + 15
        while time.time() < end and c.get("/api/v1/products?limit=1").status != 200:
            time.sleep(0.3)
        self.assertEqual(c.get("/api/v1/products?limit=1").status, 200)                                 # workers recovered

    def test_slow_database_never_breaks_correctness(self):
        """5 ms added to every DB command: lock hold times balloon, so this is a
        lock-wait / retry stress. Outcomes may include 503 (busy); overselling may not."""
        p = dbtools.new_product(1, 1000)
        users = dbtools.make_users(25)
        for u in users:
            dbtools.put_cart(u[0], [(p, 1)])
        clients = [session_client(self.srv, u) for u in users]
        self.proxy.latency = 0.005
        res = run_parallel(len(users), lambda i: checkout(clients[i]))
        self.proxy.latency = 0.0
        self.assertEqual([r for r in res if isinstance(r, Exception)], [])
        codes = [r.status for r in res]
        self.assertLessEqual(codes.count(201), 1, codes)                                                # THE invariant
        self.assertTrue(set(codes) <= {201, 409, 503}, set(codes))
        self.assertEqual(dbtools.stock(p)[1], codes.count(201))


class ExpiryUnderFaults(FaultBase):
    SERVER_ENV = {"SF_EXPIRY_SWEEP_MS": 150}

    def test_sweeper_survives_connection_loss_mid_expiry(self):
        for k in range(1, 8):
            user, (p,) = dbtools.buyer_with_cart([(5, 1000, 2)])
            c = session_client(self.srv, user)
            oid = checkout(c).json["order"]["id"]
            fired = self.proxy.arm("nth", k=k, action="before" if k % 2 else "after")
            dbtools.q(f"UPDATE orders SET expires_at = NOW(3) - INTERVAL 1 SECOND WHERE public_id='{oid}'")
            end = time.time() + 15
            status = None
            while time.time() < end:
                status = dbtools.scalar(f"SELECT status FROM orders WHERE public_id='{oid}'")
                if status == "EXPIRED":
                    break
                time.sleep(0.1)
            self.proxy.clear()
            self.assertEqual(status, "EXPIRED", f"k={k}: sweeper never recovered")
            self.assertEqual(dbtools.stock(p), (5, 0))
            self.assertTrue(fired.is_set())


if __name__ == "__main__":
    unittest.main()
