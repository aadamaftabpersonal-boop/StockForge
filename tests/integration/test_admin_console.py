"""Administrator console APIs: statistics, order operations, catalogue and customer management."""
import unittest
import uuid
from collections import Counter

import dbtools
from harness import ADMIN, Client, Server, checkout, logged_in, run_parallel, session_client


def pay(c, oid, scenario="success"):
    return c.post(f"/api/v1/orders/{oid}/pay", {"scenario": scenario})


def place(srv, lines, paid=False):
    """A buyer with an order. lines = [(stock, price, qty)]. Returns (client, order_id, [product ids], user)."""
    user, pids = dbtools.buyer_with_cart(lines)
    c = session_client(srv, user)
    r = checkout(c)
    assert r.status == 201, r.text
    oid = r.json["order"]["id"]
    if paid:
        assert pay(c, oid).status == 200
    return c, oid, pids, user


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=8)
        cls.adm = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def tearDown(self):
        self.assertEqual(dbtools.invariant_violations(), {})


class Stats(Base):
    def test_dashboard_numbers_match_independent_sql(self):
        c1, o1, (p,), _ = place(self.srv, [(50, 1000, 2)], paid=True)                       # 2000
        c2, o2, (p2, q2), _ = place(self.srv, [(50, 1000, 1), (50, 500, 3)], paid=True)       # 1000 + 1500
        c3, o3, _, _ = place(self.srv, [(50, 700, 1)])
        c4, o4, _, _ = place(self.srv, [(50, 900, 1)], paid=True)
        self.assertEqual(c4.post(f"/api/v1/orders/{o4}/cancel").status, 200)                    # cancelled: not revenue
        s = self.adm.get("/api/v1/admin/stats")
        self.assertEqual(s.status, 200, s.text)
        d = s.json
        self.assertEqual(d["revenue"]["total_cents"], 2000 + 2500)
        self.assertEqual(d["revenue"]["paid_orders"], 2)
        self.assertEqual(d["revenue"]["last_30d_cents"], 4500)
        self.assertEqual(d["orders_by_status"], {"PAID": 2, "PENDING_PAYMENT": 1, "CANCELLED": 1})
        today = d["today"]
        self.assertEqual(dbtools.scalar("SELECT DATE_FORMAT(UTC_DATE(),'%Y-%m-%d')"), today)
        self.assertEqual([x for x in d["daily"] if x["date"] == today], [{"date": today, "orders": 2, "revenue_cents": 4500}])
        top = {t["product_id"]: t["units"] for t in d["top_products_30d"]}
        self.assertEqual((top[p], top[q2]), (2, 3))
        # inventory health recomputed independently
        row = dbtools.q("SELECT COUNT(*), SUM(i.on_hand-i.reserved=0), "
                        "SUM(i.on_hand-i.reserved>0 AND i.on_hand-i.reserved<=i.low_stock_threshold), SUM(i.reserved) "
                        "FROM inventory i JOIN products p ON p.id=i.product_id WHERE p.is_active=1")[0]
        inv = d["inventory"]
        self.assertEqual((inv["active_products"], inv["out_of_stock"], inv["low_stock"], inv["units_reserved"]),
                         (int(row[0]), int(row[1]), int(row[2]), int(dbtools.scalar("SELECT SUM(reserved) FROM inventory"))))
        self.assertEqual(d["customers"]["total"], int(dbtools.scalar("SELECT COUNT(*) FROM users WHERE role='customer'")))

    def test_empty_database_renders_zeroes_not_errors(self):
        dbtools.reset_db()
        d = self.adm.get("/api/v1/admin/stats")
        self.assertEqual(d.status, 401)                          # the reset wiped the admin's session
        adm = logged_in(self.srv, ADMIN)
        d = adm.get("/api/v1/admin/stats").json
        self.assertEqual((d["revenue"]["total_cents"], d["daily"], d["top_products_30d"], d["orders_by_status"]), (0, [], [], {}))


class OrderOps(Base):
    def test_list_filters_search_and_keyset_paging(self):
        made = [place(self.srv, [(20, 1000, 1)], paid=(i % 2 == 0)) for i in range(5)]
        ids = [m[1] for m in made]
        everything, cursor = [], None
        for _ in range(10):
            r = self.adm.get("/api/v1/admin/orders?limit=2" + (f"&after={cursor}" if cursor else "")).json
            everything += [o["id"] for o in r["items"]]
            cursor = r["next_cursor"]
            if not cursor:
                break
        self.assertTrue(set(ids) <= set(everything))
        self.assertEqual(len(everything), len(set(everything)))                               # keyset: no dupes
        paid = {o["id"] for o in self.adm.get("/api/v1/admin/orders?status=PAID&limit=100").json["items"]}
        self.assertEqual(paid & set(ids), {ids[0], ids[2], ids[4]})
        one = self.adm.get(f"/api/v1/admin/orders?q={ids[1][:12].lower()}").json["items"]     # id prefix, case-insensitive
        self.assertEqual([o["id"] for o in one], [ids[1]])
        email = one[0]["customer"]["email"]
        by_mail = self.adm.get(f"/api/v1/admin/orders?q={email[:14]}").json["items"]
        self.assertIn(ids[1], [o["id"] for o in by_mail])
        self.assertEqual(one[0]["customer"]["display_name"], "Buyer")
        for bad in ("status=NOPE", "q=%25", "q=a_b", "q=a%27b", "after=x", "limit=0"):
            self.assertEqual(self.adm.get(f"/api/v1/admin/orders?{bad}").status, 422, bad)

    def test_detail_has_customer_items_payments_and_audit_history(self):
        c, oid, (p,), user = place(self.srv, [(10, 1500, 2)], paid=True)
        o = self.adm.get(f"/api/v1/admin/orders/{oid}").json["order"]
        self.assertEqual((o["status"], o["total_cents"], o["items"][0]["quantity"]), ("PAID", 3000, 2))
        self.assertEqual(o["customer"]["id"], user[0])
        self.assertEqual([p_["status"] for p_ in o["payments"]], ["succeeded"])
        self.assertEqual([h["action"] for h in o["history"]], ["order.created", "order.paid"])
        self.assertEqual(o["history"][0]["actor_user_id"], user[0])
        self.assertEqual(self.adm.get("/api/v1/admin/orders/" + "0" * 26).status, 404)
        self.assertEqual(self.adm.get("/api/v1/admin/orders/short").status, 404)

    def test_ship_then_deliver_and_repeat_is_idempotent(self):
        c, oid, (p,), _ = place(self.srv, [(10, 1000, 2)], paid=True)
        t = lambda to: self.adm.post(f"/api/v1/admin/orders/{oid}/transition", {"to": to})    # noqa: E731
        r = t("SHIPPED")
        self.assertEqual((r.status, r.json["order"]["status"]), (200, "SHIPPED"))
        self.assertEqual(t("SHIPPED").status, 200)                                            # repeat: same answer
        self.assertEqual(c.get(f"/api/v1/orders/{oid}").json["order"]["status"], "SHIPPED")   # the customer sees it
        self.assertEqual(t("DELIVERED").json["order"]["status"], "DELIVERED")
        self.assertEqual(dbtools.stock(p), (8, 0))                                            # stock untouched by fulfilment
        acts = dbtools.q(f"SELECT action, actor_user_id FROM audit_events WHERE entity_id='{oid}' ORDER BY id")
        self.assertEqual([a[0] for a in acts], ["order.created", "order.paid", "order.shipped", "order.delivered"])
        self.assertEqual(acts[2][1], acts[3][1])                                              # both by the admin
        self.assertNotEqual(acts[2][1], acts[0][1])

    def test_illegal_transitions_are_refused(self):
        c, oid, _, _ = place(self.srv, [(10, 1000, 1)])                                       # pending
        for to in ("SHIPPED", "DELIVERED"):
            r = self.adm.post(f"/api/v1/admin/orders/{oid}/transition", {"to": to})
            self.assertEqual((r.status, r.json["error"]["code"]), (409, "ILLEGAL_TRANSITION"), to)
        for bad in ({"to": "PAID"}, {"to": "EXPIRED"}, {"to": "nonsense"}, {"to": 5}, {}):
            self.assertEqual(self.adm.post(f"/api/v1/admin/orders/{oid}/transition", bad).status, 422, bad)
        self.assertEqual(self.adm.post("/api/v1/admin/orders/" + "0" * 26 + "/transition", {"to": "SHIPPED"}).status, 404)
        c2, oid2, _, _ = place(self.srv, [(10, 1000, 1)], paid=True)
        self.adm.post(f"/api/v1/admin/orders/{oid2}/transition", {"to": "SHIPPED"})
        self.adm.post(f"/api/v1/admin/orders/{oid2}/transition", {"to": "DELIVERED"})
        r = self.adm.post(f"/api/v1/admin/orders/{oid2}/transition", {"to": "CANCELLED"})
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "ILLEGAL_TRANSITION"))     # delivered is terminal

    def test_admin_cancel_releases_or_restocks_exactly_like_the_customer_path(self):
        c, oid, (p,), _ = place(self.srv, [(10, 1000, 3)])
        self.assertEqual(self.adm.post(f"/api/v1/admin/orders/{oid}/transition", {"to": "CANCELLED"}).status, 200)
        self.assertEqual(dbtools.stock(p), (10, 0))
        c2, oid2, (p2,), _ = place(self.srv, [(10, 1000, 3)], paid=True)
        self.assertEqual(dbtools.stock(p2), (7, 0))
        self.assertEqual(self.adm.post(f"/api/v1/admin/orders/{oid2}/transition", {"to": "CANCELLED"}).status, 200)
        self.assertEqual(dbtools.stock(p2), (10, 0))
        self.assertEqual(self.adm.post(f"/api/v1/admin/orders/{oid2}/transition", {"to": "CANCELLED"}).status, 200)   # repeat
        self.assertEqual(dbtools.stock(p2), (10, 0))
        actor = dbtools.scalar(f"SELECT actor_user_id FROM audit_events WHERE entity_id='{oid2}' AND action='order.cancelled'")
        self.assertEqual(actor, "1")                                                          # recorded as the admin, not the customer

    def test_concurrent_fulfilment_is_applied_once(self):
        c, oid, _, _ = place(self.srv, [(10, 1000, 1)], paid=True)

        def ship(i):
            cl = Client(self.srv)
            cl.cookie, cl.csrf = self.adm.cookie, self.adm.csrf
            return cl.post(f"/api/v1/admin/orders/{oid}/transition", {"to": "SHIPPED"}).status

        self.assertEqual(Counter(run_parallel(15, ship)), Counter({200: 15}))
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM audit_events WHERE entity_id='{oid}' AND action='order.shipped'"), "1")


class Catalogue(Base):
    def test_product_list_includes_inactive_and_filters(self):
        pid = dbtools.new_product(1, 1000)
        dbtools.q(f"UPDATE products SET is_active=0 WHERE id={pid}")
        allp = self.adm.get("/api/v1/admin/products?limit=100").json["items"]
        self.assertIn(pid, [p["id"] for p in allp])                                           # admins see inactive products
        self.assertNotIn(pid, [p["id"] for p in Client(self.srv).get("/api/v1/products?limit=50").json["items"]])
        row = [p for p in allp if p["id"] == pid][0]
        self.assertEqual((row["is_active"], row["on_hand"], row["reserved"], row["available"]), (False, 1, 0, 1))
        out = self.adm.get("/api/v1/admin/products?stock=out&limit=100").json["items"]
        self.assertTrue(out and all(p["available"] == 0 for p in out))
        low = self.adm.get("/api/v1/admin/products?stock=low&limit=100").json["items"]
        self.assertTrue(low and all(0 < p["available"] <= p["low_stock_threshold"] for p in low))
        sku = row["sku"]
        self.assertEqual([p["id"] for p in self.adm.get(f"/api/v1/admin/products?q={sku}").json["items"]], [pid])
        self.assertTrue(all(p["category"]["slug"] == "audio" for p in self.adm.get("/api/v1/admin/products?category=audio").json["items"]))
        for bad in ("stock=weird", "q=a%25b", "category=UP", "after=x"):
            self.assertEqual(self.adm.get(f"/api/v1/admin/products?{bad}").status, 422, bad)

    def test_patch_changes_what_shoppers_see_but_not_existing_orders(self):
        c, oid, (p,), _ = place(self.srv, [(10, 1000, 2)])
        r = self.adm.call("PATCH", f"/api/v1/admin/products/{p}", {"price_cents": 4999, "name": "Renamed Widget", "low_stock_threshold": 9})
        self.assertEqual(r.status, 200, r.text)
        pub = Client(self.srv).get(f"/api/v1/products/{p}").json["product"]
        self.assertEqual((pub["price_cents"], pub["name"], pub["stock_status"]), (4999, "Renamed Widget", "low_stock"))   # 8 <= 9
        order = c.get(f"/api/v1/orders/{oid}").json["order"]
        self.assertEqual((order["items"][0]["unit_price_cents"], order["total_cents"]), (1000, 2000))                    # snapshot kept
        self.assertEqual(dbtools.scalar("SELECT COUNT(*) FROM audit_events WHERE action='product.updated' AND entity_id='%d'" % p), "1")

    def test_deactivate_hides_from_shoppers_and_blocks_new_checkouts(self):
        p = dbtools.new_product(5, 1000)
        user = dbtools.make_users(1)[0]
        buyer = session_client(self.srv, user)
        dbtools.put_cart(user[0], [(p, 1)])
        self.assertEqual(self.adm.call("PATCH", f"/api/v1/admin/products/{p}", {"is_active": False}).status, 200)
        self.assertEqual(Client(self.srv).get(f"/api/v1/products/{p}").status, 404)
        r = checkout(buyer)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "PRODUCT_UNAVAILABLE"))
        self.assertEqual(self.adm.call("PATCH", f"/api/v1/admin/products/{p}", {"is_active": True}).status, 200)
        self.assertEqual(checkout(buyer).status, 201)

    def test_patch_validation(self):
        p = dbtools.new_product(5, 1000)
        url = f"/api/v1/admin/products/{p}"
        cases = [({"price_cents": 0}, "price_cents"), ({"price_cents": "9"}, "price_cents"), ({"name": ""}, "name"),
                 ({"name": "n" * 201}, "name"), ({"description": "d" * 4001}, "description"), ({"is_active": "yes"}, "is_active"),
                 ({"low_stock_threshold": -1}, "low_stock_threshold"), ({"image_url": "http://x/y.png"}, "image_url"),
                 ({"image_url": "//evil.example/x.png"}, "image_url"), ({"category_id": 99999}, "category_id")]
        for body, field in cases:
            r = self.adm.call("PATCH", url, body)
            self.assertEqual((r.status, r.json["error"]["field"]), (422, field), body)
        self.assertEqual(self.adm.call("PATCH", url, {}).status, 422)                          # nothing to update
        self.assertEqual(self.adm.call("PATCH", "/api/v1/admin/products/99999", {"name": "x"}).status, 404)
        self.assertEqual(self.adm.call("PATCH", url, {"image_url": "https://cdn.example/a.png"}).status, 200)
        self.assertEqual(self.adm.call("PATCH", url, {"image_url": None}).status, 200)
        self.assertEqual(dbtools.scalar(f"SELECT name FROM products WHERE id={p}").startswith("Fixture"), True)   # failed patches changed nothing

    def test_categories_create_rename_and_conflicts(self):
        slug = "gear-" + uuid.uuid4().hex[:6]
        r = self.adm.post("/api/v1/admin/categories", {"slug": slug, "name": "Test gear"})
        self.assertEqual(r.status, 201, r.text)
        cid = r.json["id"]
        self.assertEqual(self.adm.post("/api/v1/admin/categories", {"slug": slug, "name": "Dup"}).json["error"]["code"], "SLUG_TAKEN")
        for body in ({"slug": "A", "name": "x"}, {"slug": "Has Space", "name": "x"}, {"slug": "ok-slug", "name": ""}, {"slug": 5, "name": "x"}):
            self.assertEqual(self.adm.post("/api/v1/admin/categories", body).status, 422, body)
        self.assertEqual(self.adm.call("PATCH", f"/api/v1/admin/categories/{cid}", {"name": "Renamed gear"}).status, 200)
        cats = {c["slug"]: c for c in Client(self.srv).get("/api/v1/categories").json["categories"]}
        self.assertEqual((cats[slug]["name"], cats[slug]["product_count"]), ("Renamed gear", 0))
        self.assertEqual(self.adm.call("PATCH", "/api/v1/admin/categories/99999", {"name": "x"}).status, 404)
        p = dbtools.new_product(3, 1000, category=cid)                                          # a product can live in it
        self.assertEqual(Client(self.srv).get(f"/api/v1/products/{p}").json["product"]["category"]["slug"], slug)


class Customers(Base):
    def test_list_search_paging_and_figures(self):
        c, oid, _, user = place(self.srv, [(10, 2500, 2)], paid=True)
        users = self.adm.get("/api/v1/admin/users?limit=100").json["items"]
        me = [u for u in users if u["id"] == user[0]][0]
        self.assertEqual((me["order_count"], me["total_spent_cents"], me["role"], me["is_active"]), (1, 5000, "customer", True))
        found = self.adm.get(f"/api/v1/admin/users?q={me['email'][:12].upper()}").json["items"]
        self.assertIn(user[0], [u["id"] for u in found])                                        # case-insensitive email prefix
        seen, cur = [], None
        for _ in range(50):
            r = self.adm.get("/api/v1/admin/users?limit=3" + (f"&after={cur}" if cur else "")).json
            seen += [u["id"] for u in r["items"]]
            cur = r["next_cursor"]
            if not cur:
                break
        self.assertEqual(len(seen), len(set(seen)))
        self.assertNotIn("password_hash", self.adm.get("/api/v1/admin/users").text)             # never exposed
        for bad in ("q=a%25", "q=a_b", "after=zz"):
            self.assertEqual(self.adm.get(f"/api/v1/admin/users?{bad}").status, 422, bad)

    def test_disabling_a_customer_ends_their_sessions_and_blocks_login(self):
        email, pw = f"dis-{uuid.uuid4().hex[:8]}@example.test", "a-long-enough-password"
        cl = Client(self.srv)
        self.assertEqual(cl.post("/api/v1/auth/register", {"email": email, "password": pw, "display_name": "Dis Able"}, csrf=False).status, 201)
        cl.login((email, pw))
        uid = cl.get("/api/v1/auth/me").json["user"]["id"]
        self.assertEqual(cl.get("/api/v1/auth/me").status, 200)
        r = self.adm.call("PATCH", f"/api/v1/admin/users/{uid}", {"is_active": False})
        self.assertEqual((r.status, r.json["is_active"]), (200, False))
        self.assertEqual(cl.get("/api/v1/auth/me").status, 401)                                  # live session died at once
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM sessions WHERE user_id={uid}"), "0")
        self.assertEqual(Client(self.srv).post("/api/v1/auth/login", {"email": email, "password": pw}, csrf=False).status, 401)
        self.assertEqual(self.adm.call("PATCH", f"/api/v1/admin/users/{uid}", {"is_active": True}).status, 200)
        Client(self.srv).login((email, pw))                                                      # works again
        acts = [a[0] for a in dbtools.q(f"SELECT action FROM audit_events WHERE entity_id='{uid}' AND entity_type='user' ORDER BY id")]
        self.assertIn("user.disabled", acts)
        self.assertIn("user.enabled", acts)

    def test_guards(self):
        me = self.adm.get("/api/v1/auth/me").json["user"]["id"]
        r = self.adm.call("PATCH", f"/api/v1/admin/users/{me}", {"is_active": False})
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "SELF_MODIFY"))              # no accidental lock-out
        self.assertEqual(self.adm.get("/api/v1/auth/me").status, 200)
        self.assertEqual(self.adm.call("PATCH", "/api/v1/admin/users/99999", {"is_active": False}).status, 404)
        for body in ({}, {"is_active": "no"}, {"is_active": 0}, {"role": "admin"}):
            self.assertEqual(self.adm.call("PATCH", "/api/v1/admin/users/2", body).status, 422, body)    # no privilege escalation field
        self.assertEqual(dbtools.scalar("SELECT role FROM users WHERE id=2"), "customer")


if __name__ == "__main__":
    unittest.main()
