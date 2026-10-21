"""Cart behaviour, per-user isolation, and the order READ APIs."""
import unittest
from urllib.parse import quote

import dbtools
from harness import AVA, BEN, Client, Server, logged_in

def oid(prefix, n):
    return ("01HZ" + prefix * 21 + str(n))          # exactly 26 chars of [0-9A-Z]


ORD_A1, ORD_A2, ORD_A3, ORD_B1 = oid("A", 1), oid("A", 2), oid("A", 3), oid("B", 1)
assert all(len(x) == 26 for x in (ORD_A1, ORD_A2, ORD_A3, ORD_B1))


class Cart(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def setUp(self):
        dbtools.q("DELETE FROM cart_items")
        self.ava, self.ben = logged_in(self.srv, AVA), logged_in(self.srv, BEN)

    def test_requires_authentication_and_csrf(self):
        anon = Client(self.srv)
        self.assertEqual(anon.get("/api/v1/cart").status, 401)
        self.assertEqual(anon.put("/api/v1/cart/items/1", {"quantity": 1}).status, 401)
        r = self.ava.put("/api/v1/cart/items/1", {"quantity": 1}, csrf=False)
        self.assertEqual((r.status, r.json["error"]["code"]), (403, "CSRF_INVALID"))
        self.assertEqual(self.ava.delete("/api/v1/cart", csrf=False).status, 403)

    def test_empty_cart(self):
        r = self.ava.get("/api/v1/cart")
        self.assertEqual(r.json["items"], [])
        self.assertEqual((r.json["subtotal_cents"], r.json["checkout_ready"]), (0, False))

    def test_add_update_and_totals(self):
        self.assertEqual(self.ava.put("/api/v1/cart/items/1", {"quantity": 2}).status, 200)
        self.assertEqual(self.ava.put("/api/v1/cart/items/2", {"quantity": 1}).status, 200)
        self.assertEqual(self.ava.put("/api/v1/cart/items/1", {"quantity": 3}).status, 200)   # replaces, not adds
        cart = self.ava.get("/api/v1/cart").json
        by = {i["product_id"]: i for i in cart["items"]}
        self.assertEqual(by[1]["quantity"], 3)
        p1 = int(dbtools.scalar("SELECT price_cents FROM products WHERE id=1"))
        p2 = int(dbtools.scalar("SELECT price_cents FROM products WHERE id=2"))
        self.assertEqual(by[1]["line_total_cents"], 3 * p1)
        self.assertEqual(cart["subtotal_cents"], 3 * p1 + p2)

    def test_quantity_validation(self):
        for body in ({"quantity": 0}, {"quantity": 100}, {"quantity": -1}, {"quantity": 1.5}, {"quantity": "2"},
                     {"quantity": None}, {}, {"qty": 2}, {"quantity": [1]}):
            r = self.ava.put("/api/v1/cart/items/1", body)
            self.assertEqual(r.status, 422, body)
        self.assertEqual(self.ava.get("/api/v1/cart").json["items"], [])

    def test_unknown_and_malformed_product_ids(self):
        for pid in ("99999", "0", "abc", "-1", "1.5", "1;DROP"):
            self.assertEqual(self.ava.put(f"/api/v1/cart/items/{pid}", {"quantity": 1}).status, 404, pid)

    def test_inactive_product_cannot_be_added_and_blocks_checkout(self):
        self.ava.put("/api/v1/cart/items/3", {"quantity": 1})
        dbtools.q("UPDATE products SET is_active=0 WHERE id=3")
        try:
            self.assertEqual(self.ava.put("/api/v1/cart/items/3", {"quantity": 1}).status, 404)
            cart = self.ava.get("/api/v1/cart").json
            self.assertFalse(cart["items"][0]["purchasable"])
            self.assertFalse(cart["checkout_ready"])
        finally:
            dbtools.q("UPDATE products SET is_active=1 WHERE id=3")

    def test_quantity_above_available_is_flagged(self):
        pid = dbtools.scalar("SELECT id FROM products WHERE sku='SF-KB-FOUNDER'")        # exactly 1 in stock
        self.assertEqual(self.ava.put(f"/api/v1/cart/items/{pid}", {"quantity": 5}).status, 200)   # carts are advisory
        item = self.ava.get("/api/v1/cart").json["items"][0]
        self.assertEqual((item["available"], item["purchasable"]), (1, False))
        self.assertFalse(self.ava.get("/api/v1/cart").json["checkout_ready"])
        self.ava.put(f"/api/v1/cart/items/{pid}", {"quantity": 1})
        self.assertTrue(self.ava.get("/api/v1/cart").json["checkout_ready"])

    def test_carts_are_isolated_between_users(self):
        self.ava.put("/api/v1/cart/items/1", {"quantity": 2})
        self.assertEqual(self.ben.get("/api/v1/cart").json["items"], [])
        self.ben.delete("/api/v1/cart/items/1")                       # ben removing "product 1" must not touch ava's
        self.ben.delete("/api/v1/cart")
        self.assertEqual(len(self.ava.get("/api/v1/cart").json["items"]), 1)

    def test_delete_is_idempotent_and_clear_works(self):
        self.ava.put("/api/v1/cart/items/1", {"quantity": 1})
        self.ava.put("/api/v1/cart/items/2", {"quantity": 1})
        self.assertEqual(self.ava.delete("/api/v1/cart/items/1").status, 200)
        self.assertEqual(self.ava.delete("/api/v1/cart/items/1").status, 200)   # already gone
        self.assertEqual(len(self.ava.get("/api/v1/cart").json["items"]), 1)
        self.assertEqual(self.ava.delete("/api/v1/cart").status, 200)
        self.assertEqual(self.ava.get("/api/v1/cart").json["items"], [])

    def test_cart_survives_logout_login(self):
        self.ava.put("/api/v1/cart/items/4", {"quantity": 2})
        self.ava.post("/api/v1/auth/logout")
        again = logged_in(self.srv, AVA)
        self.assertEqual(again.get("/api/v1/cart").json["items"][0]["quantity"], 2)


class OrdersRead(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4)
        # Orders are created by fixtures here; the creation API arrives with checkout (phase 5).
        dbtools.q(f"""
            INSERT INTO orders (public_id,user_id,status,total_cents) VALUES
              ('{ORD_A1}',2,'PAID',3000),('{ORD_A2}',2,'SHIPPED',1500),('{ORD_A3}',2,'PENDING_PAYMENT',500),
              ('{ORD_B1}',3,'PAID',999);
            INSERT INTO order_items (order_id,product_id,sku,product_name,unit_price_cents,quantity)
              SELECT o.id,1,'SNAP-1','Snapshot Name',1000,3 FROM orders o WHERE o.public_id='{ORD_A1}';
            INSERT INTO order_items (order_id,product_id,sku,product_name,unit_price_cents,quantity)
              SELECT o.id,2,'SNAP-2','Other Item',750,2 FROM orders o WHERE o.public_id='{ORD_A2}';
        """)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def test_list_only_own_orders_newest_first_with_paging(self):
        ava = logged_in(self.srv, AVA)
        r = ava.get("/api/v1/orders?limit=2")
        self.assertEqual([o["id"] for o in r.json["items"]], [ORD_A3, ORD_A2])
        self.assertIsNotNone(r.json["next_cursor"])
        r2 = ava.get(f"/api/v1/orders?limit=2&after={r.json['next_cursor']}")
        self.assertEqual([o["id"] for o in r2.json["items"]], [ORD_A1])
        self.assertIsNone(r2.json["next_cursor"])
        self.assertEqual(r.json["items"][1]["item_count"], 2)
        ben = logged_in(self.srv, BEN).get("/api/v1/orders").json["items"]
        self.assertEqual([o["id"] for o in ben], [ORD_B1])

    def test_order_detail_uses_snapshots(self):
        ava = logged_in(self.srv, AVA)
        dbtools.q("UPDATE products SET price_cents=99999, name='Renamed Later' WHERE id=1")
        try:
            o = ava.get(f"/api/v1/orders/{ORD_A1}").json["order"]
            it = o["items"][0]
            self.assertEqual((it["name"], it["unit_price_cents"], it["line_total_cents"]), ("Snapshot Name", 1000, 3000))
            self.assertEqual((o["status"], o["total_cents"]), ("PAID", 3000))
        finally:
            dbtools.q("UPDATE products SET price_cents=14900, name='Aurora Wireless Headphones' WHERE id=1")

    def test_other_users_orders_look_nonexistent(self):
        ben = logged_in(self.srv, BEN)
        other = ben.get(f"/api/v1/orders/{ORD_A1}")
        ghost = ben.get("/api/v1/orders/01HZZZZZZZZZZZZZZZZZZZZZZZ")
        self.assertEqual((other.status, ghost.status), (404, 404))
        self.assertEqual(other.json["error"]["code"], ghost.json["error"]["code"])   # no existence oracle
        self.assertEqual(other.json["error"]["message"], ghost.json["error"]["message"])

    def test_malformed_ids_and_auth(self):
        ava = logged_in(self.srv, AVA)
        for bad in ("short", "01hzaaaaaaaaaaaaaaaaaaaaa1", "01HZ'; DROP TABLE orders;--", "1"):
            self.assertEqual(ava.get(f"/api/v1/orders/{quote(bad)}").status, 404, bad)
        self.assertEqual(Client(self.srv).get("/api/v1/orders").status, 401)
        self.assertEqual(Client(self.srv).get(f"/api/v1/orders/{ORD_A1}").status, 401)
        self.assertEqual(ava.get("/api/v1/orders?after=abc").status, 422)
        self.assertEqual(ava.get("/api/v1/orders?limit=0").status, 422)

    def test_checkout_requires_idempotency_key(self):
        ava = logged_in(self.srv, AVA)
        r = ava.post("/api/v1/orders", {})
        self.assertEqual((r.status, r.json["error"]["code"]), (400, "IDEMPOTENCY_KEY_REQUIRED"))


if __name__ == "__main__":
    unittest.main()
