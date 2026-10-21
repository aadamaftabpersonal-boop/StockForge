"""The signature tests: concurrent checkouts against deliberately scarce stock.
After every scenario the final DATABASE state is verified (not just HTTP codes)
by two independent checkers: tests' own SQL and the server's reconcile endpoint."""
import random
import time
import unittest
import uuid
from collections import Counter

import dbtools
from harness import ADMIN, Server, checkout, logged_in, metrics, run_parallel, session_client

SCALE = float(__import__("os").environ.get("STRESS_SCALE", "1"))


def n(x):
    return max(2, int(x * SCALE))


class ConcurrentCheckout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=16, SF_QUEUE_CAP=2048)
        cls.admin = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def buyers(self, count, lines_for):
        users = dbtools.make_users(count)
        for i, u in enumerate(users):
            dbtools.put_cart(u[0], lines_for(i))
        return users, [session_client(self.srv, u) for u in users]

    def assert_books_balance(self):
        self.assertEqual(dbtools.invariant_violations(), {})
        r = self.admin.get("/api/v1/admin/reconcile").json
        self.assertTrue(r["ok"], [c for c in r["checks"] if not c["ok"]])

    def statuses(self, results):
        errs = [r for r in results if isinstance(r, Exception)]
        self.assertEqual(errs, [], errs[:3])
        return Counter(r.status for r in results)

    # --------------------------------------------------------------- the headline
    def test_last_unit_300_buyers_exactly_one_wins(self):
        p = dbtools.new_product(1, 5000)
        count = n(300)
        _, clients = self.buyers(count, lambda i: [(p, 1)])
        res = run_parallel(count, lambda i: checkout(clients[i]))
        st = self.statuses(res)
        self.assertEqual(st[201], 1, st)                          # THE invariant: one unit, one order
        self.assertEqual(st[409], count - 1, st)
        self.assertEqual(dbtools.stock(p), (1, 1))                # on_hand never negative, reserved never above it
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM order_items WHERE product_id={p}"), "1")
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM stock_reservations WHERE product_id={p}"), "1")
        winner = [r for r in res if r.status == 201][0].json["order"]
        self.assertEqual(winner["status"], "PENDING_PAYMENT")
        for r in res:
            if r.status == 409:
                self.assertEqual(r.json["error"]["code"], "INSUFFICIENT_STOCK")
        self.assert_books_balance()

    def test_ten_units_two_hundred_buyers_exactly_ten_win(self):
        p = dbtools.new_product(10, 1000)
        count = n(200)
        _, clients = self.buyers(count, lambda i: [(p, 1)])
        st = self.statuses(run_parallel(count, lambda i: checkout(clients[i])))
        self.assertEqual((st[201], st[409]), (10, count - 10), st)
        self.assertEqual(dbtools.stock(p), (10, 10))
        self.assert_books_balance()

    def test_multi_quantity_never_oversells_and_leaves_remainder(self):
        p = dbtools.new_product(20, 1000)
        count = n(100)
        _, clients = self.buyers(count, lambda i: [(p, 3)])
        st = self.statuses(run_parallel(count, lambda i: checkout(clients[i])))
        self.assertEqual(st[201], 6, st)                          # 6 x 3 = 18 <= 20 < 21
        self.assertEqual(dbtools.stock(p), (20, 18))              # 2 units correctly left unsold
        self.assert_books_balance()

    # ----------------------------------------------------- duplicate delivery
    def test_sixty_identical_requests_create_one_order(self):
        p = dbtools.new_product(100, 1000)
        (u,) = dbtools.make_users(1)
        dbtools.put_cart(u[0], [(p, 2)])
        c = session_client(self.srv, u)
        k = "dup-" + uuid.uuid4().hex
        count = n(60)
        res = run_parallel(count, lambda i: checkout(c, k))
        st = self.statuses(res)
        self.assertEqual(st[201], count, st)                      # every duplicate gets the same success
        self.assertEqual(len({r.json["order"]["id"] for r in res}), 1)
        flags = Counter(r.headers["idempotent-replay"] for r in res)
        self.assertEqual(flags["false"], 1, flags)                # exactly one request did the work
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={u[0]}"), "1")
        self.assertEqual(dbtools.stock(p), (100, 2))              # reserved once, not 60 times
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE reason='reserve' AND product_id={p}"), "1")
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM idempotency_keys WHERE idem_key='{k}'"), "1")
        self.assert_books_balance()

    def test_one_cart_many_keys_becomes_one_order(self):
        """Different keys, same user, same cart: the per-user lock must stop a double order."""
        p = dbtools.new_product(100, 1000)
        (u,) = dbtools.make_users(1)
        dbtools.put_cart(u[0], [(p, 4)])
        c = session_client(self.srv, u)
        count = n(30)
        st = self.statuses(run_parallel(count, lambda i: checkout(c)))
        self.assertEqual((st[201], st[409]), (1, count - 1), st)
        self.assertEqual(dbtools.stock(p), (100, 4))
        self.assert_books_balance()

    def test_client_disconnect_then_retry_never_duplicates(self):
        p = dbtools.new_product(1000, 1000)
        count = n(20)
        users, clients = self.buyers(count, lambda i: [(p, 1)])
        keys = ["disc-" + uuid.uuid4().hex for _ in users]

        def fire_and_vanish(i):
            u = users[i]
            body = b"{}"
            req = (f"POST /api/v1/orders HTTP/1.1\r\nHost: 127.0.0.1:{self.srv.port}\r\n"
                   f"Cookie: sf_sid={u[1]}\r\nX-CSRF-Token: {u[2]}\r\nIdempotency-Key: {keys[i]}\r\n"
                   f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n").encode() + body
            s = self.srv.connect()
            s.sendall(req)
            s.close()                                             # client gives up before reading the answer
            return None

        run_parallel(count, fire_and_vanish)
        time.sleep(2.0)
        retried = [checkout(clients[i], keys[i]) for i in range(count)]
        self.assertEqual({r.status for r in retried}, {201})
        replays = sum(r.headers["idempotent-replay"] == "true" for r in retried)
        self.assertGreaterEqual(replays, 1, "server never completed an abandoned request")
        self.assertEqual(dbtools.scalar("SELECT COUNT(*) FROM orders o JOIN order_items i ON i.order_id=o.id "
                                        f"WHERE i.product_id={p}"), str(count))      # exactly one order per user
        self.assertEqual(dbtools.stock(p), (1000, count))
        self.assert_books_balance()

    # ----------------------------------------------- lock-order / deadlock proof
    def test_cross_product_contention_has_no_failed_requests(self):
        pids = [dbtools.new_product(25, 1000 + 100 * i) for i in range(5)]
        rnd = random.Random(7)
        plans = []
        for _ in range(n(150)):
            chosen = rnd.sample(pids, rnd.randint(2, 4))
            rnd.shuffle(chosen)                                   # cart insertion order must not matter
            plans.append([(p, rnd.randint(1, 3)) for p in chosen])
        users, clients = self.buyers(len(plans), lambda i: plans[i])
        before = metrics(self.srv)["orders"]
        res = run_parallel(len(plans), lambda i: checkout(clients[i]))
        st = self.statuses(res)
        self.assertEqual(set(st), {201, 409}, st)                 # no 5xx: deadlocks are retried, not surfaced
        after = metrics(self.srv)["orders"]
        self.assertEqual(after["retries_exhausted"] - before["retries_exhausted"], 0)

        want = Counter()
        for i, r in enumerate(res):
            if r.status == 201:
                for p, q in plans[i]:
                    want[p] += q
        for p in pids:
            on_hand, reserved = dbtools.stock(p)
            self.assertEqual((on_hand, reserved), (25, want[p]), p)
            self.assertLessEqual(reserved, 25)
        self.assert_books_balance()
        print(f"\n    [info] {st[201]} orders / {st[409]} rejected; deadlock retries during run: "
              f"{after['deadlock_retries'] - before['deadlock_retries']}")

    def test_checkout_racing_admin_stock_removal(self):
        p = dbtools.new_product(50, 1000)
        count = n(60)
        _, clients = self.buyers(count, lambda i: [(p, 1)])
        adm = self.admin

        def work(i):
            if i % 2 == 0 and i // 2 < 30:                       # half the workers shrink stock by 1
                return ("adj", adm.post(f"/api/v1/admin/inventory/{p}/adjust", {"delta": -1, "reason": "adjust"}))
            return ("buy", checkout(clients[i]))

        res = run_parallel(count, work)
        errs = [r for r in res if isinstance(r, Exception)]
        self.assertEqual(errs, [], errs[:2])
        bought = sum(1 for k, r in res if k == "buy" and r.status == 201)
        adjusted = sum(1 for k, r in res if k == "adj" and r.status == 200)
        self.assertTrue(all(r.status in (200, 201, 409) for _, r in res), Counter(r.status for _, r in res))
        on_hand, reserved = dbtools.stock(p)
        self.assertEqual((on_hand, reserved), (50 - adjusted, bought))
        self.assertLessEqual(reserved, on_hand)                  # never reserved beyond what exists
        self.assert_books_balance()


if __name__ == "__main__":
    unittest.main()
