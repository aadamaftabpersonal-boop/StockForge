"""Checkout semantics: reservation, idempotency, atomicity (single-threaded
behaviour; the concurrent hammering is in test_checkout_concurrency.py)."""
import unittest
import uuid

import dbtools
from harness import ADMIN, Client, Server, checkout, logged_in, session_client


def key():
    return "k-" + uuid.uuid4().hex


class CheckoutSemantics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4, SF_RESERVATION_TTL_S=600)
        cls.admin = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def buyer(self, lines):
        u = dbtools.make_users(1)[0]
        dbtools.put_cart(u[0], lines)
        return u, session_client(self.srv, u)

    def tearDown(self):
        self.assertEqual(dbtools.invariant_violations(), {})     # every test must leave the books balanced

    # --------------------------------------------------------- request shape
    def test_auth_csrf_and_key_are_required(self):
        self.assertEqual(Client(self.srv).post("/api/v1/orders", {}, headers={"Idempotency-Key": key()}).status, 401)
        _, c = self.buyer([])
        r = c.post("/api/v1/orders", {}, csrf=False, headers={"Idempotency-Key": key()})
        self.assertEqual((r.status, r.json["error"]["code"]), (403, "CSRF_INVALID"))
        r = c.post("/api/v1/orders", {})
        self.assertEqual((r.status, r.json["error"]["code"]), (400, "IDEMPOTENCY_KEY_REQUIRED"))
        for bad in ("short", "x" * 65, "has spaces in it", "semi;colon-key1", "quote'key-12345", ""):
            r = c.post("/api/v1/orders", {}, headers={"Idempotency-Key": bad})
            self.assertIn(r.status, (400,), bad)

    def test_empty_cart_is_rejected_and_key_not_consumed(self):
        _, c = self.buyer([])
        k = key()
        r = checkout(c, k)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "EMPTY_CART"))
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM idempotency_keys WHERE idem_key='{k}'"), "0")

    # ----------------------------------------------------------- happy path
    def test_happy_path_reserves_stock_without_deducting_it(self):
        p1, p2 = dbtools.new_product(10, 1500), dbtools.new_product(5, 2500)
        u, c = self.buyer([(p1, 2), (p2, 1)])
        r = checkout(c, key())
        self.assertEqual(r.status, 201, r.text)
        self.assertEqual(r.headers["idempotent-replay"], "false")
        o = r.json["order"]
        self.assertEqual((o["status"], o["total_cents"]), ("PENDING_PAYMENT", 2 * 1500 + 2500))
        self.assertEqual({i["product_id"]: i["quantity"] for i in o["items"]}, {p1: 2, p2: 1})
        self.assertEqual(len(o["id"]), 26)

        self.assertEqual(dbtools.stock(p1), (10, 2))                  # reserved, NOT deducted yet
        self.assertEqual(dbtools.stock(p2), (5, 1))
        self.assertEqual(dbtools.q(f"SELECT product_id,quantity,status FROM stock_reservations "
                                   f"WHERE order_id=(SELECT id FROM orders WHERE public_id='{o['id']}') ORDER BY product_id"),
                         [[str(p1), "2", "held"], [str(p2), "1", "held"]])
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE reason='reserve' "
                                        f"AND product_id IN ({p1},{p2})"), "2")
        self.assertEqual(c.get("/api/v1/cart").json["items"], [])        # cart consumed
        ttl = int(dbtools.scalar(f"SELECT TIMESTAMPDIFF(SECOND, NOW(), expires_at) FROM orders WHERE public_id='{o['id']}'"))
        self.assertTrue(595 <= ttl <= 600, ttl)
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM audit_events WHERE action='order.created' "
                                        f"AND entity_id='{o['id']}' AND actor_user_id={u[0]}"), "1")
        pub = Client(self.srv).get(f"/api/v1/products/{p1}").json["product"]
        self.assertEqual(pub["available"], 8)                           # visible to shoppers immediately
        self.assertEqual(c.get(f"/api/v1/orders/{o['id']}").json["order"]["status"], "PENDING_PAYMENT")

    def test_order_snapshots_price_and_name(self):
        p = dbtools.new_product(5, 1000)
        _, c = self.buyer([(p, 1)])
        o = checkout(c, key()).json["order"]
        dbtools.q(f"UPDATE products SET price_cents=99999, name='Renamed' WHERE id={p}")
        again = c.get(f"/api/v1/orders/{o['id']}").json["order"]
        self.assertEqual((again["items"][0]["unit_price_cents"], again["total_cents"]), (1000, 1000))
        self.assertTrue(again["items"][0]["name"].startswith("Fixture"))

    def test_expected_total_guards_against_price_changes(self):
        p = dbtools.new_product(5, 1000)
        _, c = self.buyer([(p, 2)])
        r = checkout(c, key(), {"expected_total_cents": 1500})
        self.assertEqual((r.status, r.json["error"]["code"], r.json["error"]["current_total_cents"]),
                         (409, "PRICE_CHANGED", 2000))
        self.assertEqual(dbtools.stock(p), (5, 0))
        self.assertEqual(checkout(c, key(), {"expected_total_cents": 2000}).status, 201)
        _, c2 = self.buyer([(p, 1)])
        self.assertEqual(checkout(c2, key(), {"expected_total_cents": "x"}).status, 422)

    def test_inactive_product_blocks_checkout(self):
        p = dbtools.new_product(5, 1000)
        _, c = self.buyer([(p, 1)])
        dbtools.q(f"UPDATE products SET is_active=0 WHERE id={p}")
        r = checkout(c, key())
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "PRODUCT_UNAVAILABLE"))
        self.assertEqual(dbtools.stock(p), (5, 0))

    # ----------------------------------------------------------- idempotency
    def test_replay_returns_the_same_order_and_changes_nothing(self):
        p = dbtools.new_product(10, 1000)
        u, c = self.buyer([(p, 3)])
        k = key()
        first = checkout(c, k)
        self.assertEqual(first.status, 201)
        dbtools.put_cart(u[0], [(p, 1)])                               # cart refilled after the first checkout
        before = (dbtools.stock(p), dbtools.scalar("SELECT COUNT(*) FROM orders"),
                  dbtools.scalar("SELECT COUNT(*) FROM inventory_ledger"))
        again = checkout(c, k)
        self.assertEqual(again.status, 201)
        self.assertEqual(again.headers["idempotent-replay"], "true")
        self.assertEqual(again.json, first.json)                       # byte-for-byte same order
        after = (dbtools.stock(p), dbtools.scalar("SELECT COUNT(*) FROM orders"),
                 dbtools.scalar("SELECT COUNT(*) FROM inventory_ledger"))
        self.assertEqual(before, after)                                # no second reservation, order or ledger row
        self.assertEqual(len(c.get("/api/v1/cart").json["items"]), 1)  # the replay did not eat the new cart

    def test_same_key_different_request_is_rejected(self):
        p = dbtools.new_product(10, 1000)
        _, c = self.buyer([(p, 1)])
        k = key()
        self.assertEqual(checkout(c, k, {"expected_total_cents": 1000}).status, 201)
        r = checkout(c, k, {"expected_total_cents": 2000})
        self.assertEqual((r.status, r.json["error"]["code"]), (422, "IDEMPOTENCY_KEY_REUSED"))
        r = checkout(c, k)                                             # no expectation at all is also "different"
        self.assertEqual(r.status, 422)

    def test_equivalent_json_formatting_is_the_same_request(self):
        p = dbtools.new_product(10, 1000)
        _, c = self.buyer([(p, 1)])
        k = key()
        a = checkout(c, k, {"expected_total_cents": 1000})
        b = c.post("/api/v1/orders", raw=b'{ "expected_total_cents" :   1000 }', headers={"Idempotency-Key": k})
        self.assertEqual((a.status, b.status, b.headers["idempotent-replay"]), (201, 201, "true"))

    def test_keys_are_scoped_per_user(self):
        p = dbtools.new_product(10, 1000)
        (_, c1), (_, c2) = self.buyer([(p, 1)]), self.buyer([(p, 1)])
        k = key()
        r1, r2 = checkout(c1, k), checkout(c2, k)
        self.assertEqual((r1.status, r2.status), (201, 201))
        self.assertNotEqual(r1.json["order"]["id"], r2.json["order"]["id"])
        self.assertEqual(dbtools.stock(p), (10, 2))

    # ------------------------------------------------------------ atomicity
    def test_insufficient_stock_is_all_or_nothing_and_retryable(self):
        p1, p2 = dbtools.new_product(5, 1000), dbtools.new_product(1, 1000)
        u, c = self.buyer([(p1, 3), (p2, 2)])                           # p2 is short by one
        k = key()
        r = checkout(c, k)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "INSUFFICIENT_STOCK"))
        short = r.json["error"]["items"]
        self.assertEqual([(s["requested"], s["available"]) for s in short], [(2, 1)])
        self.assertEqual(dbtools.stock(p1), (5, 0))                     # p1 was NOT reserved: atomic
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM orders WHERE user_id={u[0]}"), "0")
        self.assertEqual(len(c.get("/api/v1/cart").json["items"]), 2)   # cart untouched
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM idempotency_keys WHERE idem_key='{k}'"), "0")
        dbtools.restock(p2, 4)
        r = checkout(c, k)                                              # SAME key now succeeds
        self.assertEqual((r.status, r.headers["idempotent-replay"]), (201, "false"))
        self.assertEqual(dbtools.stock(p1), (5, 3))

    def test_all_short_items_are_reported_together(self):
        p1, p2 = dbtools.new_product(1, 1000), dbtools.new_product(0, 1000)
        _, c = self.buyer([(p1, 5), (p2, 1)])
        short = checkout(c, key()).json["error"]["items"]
        self.assertEqual(sorted((s["requested"], s["available"]) for s in short), [(1, 0), (5, 1)])

    def test_last_unit_goes_to_exactly_one_of_two_sequential_buyers(self):
        p = dbtools.new_product(1, 1000)
        (_, c1), (_, c2) = self.buyer([(p, 1)]), self.buyer([(p, 1)])
        self.assertEqual(checkout(c1, key()).status, 201)
        self.assertEqual(checkout(c2, key()).status, 409)
        self.assertEqual(dbtools.stock(p), (1, 1))

    # ---------------------------------------------------------- operability
    def test_admin_reconcile_endpoint_agrees(self):
        p = dbtools.new_product(3, 1000)
        _, c = self.buyer([(p, 2)])
        checkout(c, key())
        r = self.admin.get("/api/v1/admin/reconcile")
        self.assertEqual(r.status, 200, r.text)
        self.assertTrue(r.json["ok"], r.json)
        self.assertGreaterEqual(len(r.json["checks"]), 8)
        self.assertEqual(self.admin.get("/api/v1/admin/reconcile").json["checks"][0]["ok"], True)
        self.assertEqual(Client(self.srv).get("/api/v1/admin/reconcile").status, 401)
        self.assertEqual(logged_in(self.srv, ("ava@stockforge.test", "Customer-Demo-2026!")).get("/api/v1/admin/reconcile").status, 403)

    def test_reconcile_detects_corruption(self):
        """The checker must actually catch a broken ledger, not just report green."""
        p = dbtools.new_product(3, 1000)
        dbtools.q(f"UPDATE inventory SET on_hand=on_hand+1 WHERE product_id={p}")      # stock appears from nowhere
        try:
            r = self.admin.get("/api/v1/admin/reconcile").json
            self.assertFalse(r["ok"])
            bad = {c["name"]: c["ids"] for c in r["checks"] if not c["ok"]}
            self.assertEqual(bad, {"on_hand_equals_ledger": [p]})
            self.assertIn("on_hand==sum(ledger)", dbtools.invariant_violations())      # independent checker agrees
        finally:
            dbtools.q(f"UPDATE inventory SET on_hand=on_hand-1 WHERE product_id={p}")

    def test_reconcile_cli_exit_codes(self):
        """The offline checker: exit 0 on a consistent DB, 1 (naming the check) on corruption."""
        import json
        import os
        import subprocess
        cli = os.path.join(os.path.dirname(os.environ.get("SF_BIN", "build/rel/stockforged")), "reconcile")
        env = dict(os.environ, SF_DB_NAME=dbtools.TEST_DB)

        def run():
            r = subprocess.run([cli], env=env, capture_output=True, text=True, timeout=60)
            return r.returncode, json.loads(r.stdout) if r.stdout.strip() else None

        p = dbtools.new_product(3, 1000)
        rc, rep = run()
        self.assertEqual((rc, rep["ok"]), (0, True))
        dbtools.q(f"UPDATE inventory SET on_hand=on_hand+1 WHERE product_id={p}")
        try:
            rc, rep = run()
            self.assertEqual(rc, 1)
            self.assertEqual([c["name"] for c in rep["checks"] if not c["ok"]], ["on_hand_equals_ledger"])
        finally:
            dbtools.q(f"UPDATE inventory SET on_hand=on_hand-1 WHERE product_id={p}")
        self.assertEqual(run()[0], 0)

    def test_metrics_count_order_events(self):
        from harness import metrics
        before = metrics(self.srv)["orders"]
        p = dbtools.new_product(3, 1000)
        _, c = self.buyer([(p, 1)])
        k = key()
        checkout(c, k)
        checkout(c, k)
        after = metrics(self.srv)["orders"]
        self.assertEqual((after["created"] - before["created"], after["replayed"] - before["replayed"]), (1, 1))


class CheckoutRateLimit(unittest.TestCase):
    def test_per_user_checkout_limit(self):
        dbtools.reset_db()
        srv = Server(SF_RL_CHECKOUT_USER=2)
        try:
            u = dbtools.make_users(1)[0]
            c = session_client(srv, u)
            codes = [checkout(c, key()).status for _ in range(3)]
            self.assertEqual(codes, [409, 409, 429])                     # empty cart, then throttled
        finally:
            self.assertEqual(srv.stop(), 0)


if __name__ == "__main__":
    unittest.main()
