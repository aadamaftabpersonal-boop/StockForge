"""Public catalog: pagination, sorting, filtering, search, availability."""
import unittest
from urllib.parse import quote

import dbtools
from harness import Client, Server


class Catalog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4)
        cls.c = Client(cls.srv)
        cls.total = int(dbtools.scalar("SELECT COUNT(*) FROM products WHERE is_active=1"))

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def walk(self, query="", limit=7):
        """Follow next_cursor to the end; returns all items in order."""
        items, cursor, pages = [], None, 0
        while True:
            q = f"limit={limit}" + (f"&{query}" if query else "") + (f"&after={cursor}" if cursor else "")
            r = self.c.get(f"/api/v1/products?{q}")
            self.assertEqual(r.status, 200, r.text)
            items += r.json["items"]
            cursor = r.json["next_cursor"]
            pages += 1
            self.assertLess(pages, 100, "pagination did not terminate")
            if cursor is None:
                return items

    def test_seed_is_realistic(self):
        self.assertEqual(self.total, 48)
        r = self.c.get("/api/v1/products?limit=1")
        it = r.json["items"][0]
        for k in ("id", "sku", "name", "description", "price_cents", "currency", "category", "available", "stock_status"):
            self.assertIn(k, it)
        self.assertEqual(it["currency"], "USD")

    def test_keyset_pagination_newest_complete_and_stable(self):
        items = self.walk("sort=newest")
        ids = [i["id"] for i in items]
        self.assertEqual(len(ids), self.total)
        self.assertEqual(len(set(ids)), self.total)                 # no duplicates
        self.assertEqual(ids, sorted(ids, reverse=True))            # strict newest-first

    def test_price_sorts_paginate_without_gaps_or_dupes(self):
        for sort, rev in (("price_asc", False), ("price_desc", True)):
            items = self.walk(f"sort={sort}", limit=5)
            keys = [(i["price_cents"], i["id"]) for i in items]
            self.assertEqual(len(keys), self.total, sort)
            self.assertEqual(len(set(keys)), self.total, sort)
            self.assertEqual(keys, sorted(keys, reverse=rev), sort)  # ties broken by id => deterministic

    def test_pagination_survives_inserts_between_pages(self):
        r = self.c.get("/api/v1/products?limit=10&sort=newest")
        cursor = r.json["next_cursor"]
        seen = {i["id"] for i in r.json["items"]}
        dbtools.q("INSERT INTO products (sku,category_id,name,description,price_cents) "
                  "VALUES ('SF-TMP-INS',1,'Inserted mid-walk','x',100); "
                  "INSERT INTO inventory (product_id,on_hand) SELECT id,5 FROM products WHERE sku='SF-TMP-INS'")
        try:
            r2 = self.c.get(f"/api/v1/products?limit=10&sort=newest&after={cursor}")
            self.assertFalse(seen & {i["id"] for i in r2.json["items"]})   # keyset: no shifted duplicates
        finally:
            dbtools.q("DELETE FROM inventory WHERE product_id IN (SELECT id FROM products WHERE sku='SF-TMP-INS');"
                      "DELETE FROM products WHERE sku='SF-TMP-INS'")

    def test_category_filter(self):
        items = self.walk("category=audio")
        self.assertEqual(len(items), 8)
        self.assertTrue(all(i["category"]["slug"] == "audio" for i in items))
        self.assertEqual(self.walk("category=no-such-category"), [])
        cats = self.c.get("/api/v1/categories").json["categories"]
        self.assertEqual(len(cats), 6)
        self.assertEqual(sum(c["product_count"] for c in cats), self.total)

    def test_category_and_price_sort_combine(self):
        items = self.walk("category=keyboards-mice&sort=price_asc", limit=3)
        prices = [i["price_cents"] for i in items]
        self.assertEqual(len(prices), 8)
        self.assertEqual(prices, sorted(prices))

    def test_search(self):
        items = self.walk("q=keyboard")
        self.assertGreaterEqual(len(items), 3)
        for i in items:
            self.assertIn("keyboard", (i["name"] + " " + i["description"]).lower())
        prefix = self.walk("q=wirel")                                 # prefix match via token*
        self.assertTrue(any("Wireless" in i["name"] for i in prefix))
        both = self.walk("q=wireless%20keyboard")                     # all words required
        self.assertTrue(all("keyboard" in i["name"].lower() and "wireless" in i["name"].lower() for i in both))
        self.assertEqual(self.walk("q=zzzzqqqq"), [])

    def test_search_input_is_hostile_safe(self):
        for evil in ['"; DROP TABLE products; --', "' OR 1=1 --", "-keyboard +(", "*", "%00", "ke\\yboard"]:
            r = self.c.get(f"/api/v1/products?q={quote(evil)}")
            self.assertIn(r.status, (200, 422), evil)
        self.assertEqual(int(dbtools.scalar("SELECT COUNT(*) FROM products")), self.total)
        self.assertEqual(self.c.get("/api/v1/products?q=a").status, 422)              # too short to search
        self.assertEqual(self.c.get("/api/v1/products?q=%ZZ").status, 422)            # bad escape
        self.assertEqual(self.c.get("/api/v1/products?q=" + "a" * 500).status, 422)   # over-long

    def test_parameter_validation(self):
        bad = ["limit=0", "limit=-1", "limit=abc", "limit=1.5", "limit=", "sort=hacked", "sort=price_asc;drop",
               "after=abc", "after=-1", "after=1.2&sort=newest", "after=5&sort=price_asc", "category=UPPER",
               "category=a%20b", "category=a;b"]
        for q in bad:
            self.assertEqual(self.c.get(f"/api/v1/products?{q}").status, 422, q)

    def test_limit_is_clamped_not_trusted(self):
        r = self.c.get("/api/v1/products?limit=1000000")
        self.assertEqual(r.status, 200)
        self.assertEqual(len(r.json["items"]), min(50, self.total))
        self.assertEqual(self.c.get("/api/v1/products?limit=50").json["next_cursor"], None)

    def test_product_detail(self):
        r = self.c.get("/api/v1/products/1")
        self.assertEqual(r.status, 200)
        self.assertEqual(r.json["product"]["id"], 1)
        for bad in ["99999", "0", "abc", "-1", "1.5", "9" * 30, "1%20OR%201=1", "1;DROP"]:
            self.assertEqual(self.c.get(f"/api/v1/products/{bad}").status, 404, bad)

    def test_inactive_products_are_hidden(self):
        dbtools.q("UPDATE products SET is_active=0 WHERE id=5")
        try:
            self.assertEqual(self.c.get("/api/v1/products/5").status, 404)
            self.assertNotIn(5, [i["id"] for i in self.walk()])
            self.assertEqual(len(self.walk()), self.total - 1)
        finally:
            dbtools.q("UPDATE products SET is_active=1 WHERE id=5")

    def test_availability_is_on_hand_minus_reserved(self):
        pid, on_hand = [int(x) for x in dbtools.q("SELECT product_id,on_hand FROM inventory WHERE on_hand >= 20 LIMIT 1")[0]]
        try:
            for reserved, status, avail in ((0, "in_stock", on_hand), (on_hand - 3, "low_stock", 3), (on_hand, "out_of_stock", 0)):
                dbtools.q(f"UPDATE inventory SET reserved={reserved} WHERE product_id={pid}")
                p = self.c.get(f"/api/v1/products/{pid}").json["product"]
                self.assertEqual((p["available"], p["stock_status"]), (avail, status))
        finally:
            dbtools.q(f"UPDATE inventory SET reserved=0 WHERE product_id={pid}")

    def test_seeded_edge_products(self):
        founder = [i for i in self.walk() if i["sku"] == "SF-KB-FOUNDER"][0]
        self.assertEqual((founder["available"], founder["stock_status"]), (1, "low_stock"))
        out = [i for i in self.walk() if i["available"] == 0]
        self.assertTrue(out and all(i["stock_status"] == "out_of_stock" for i in out))

    def test_method_and_route_errors(self):
        self.assertEqual(self.c.post("/api/v1/products", {}, csrf=False).status, 405)
        self.assertEqual(self.c.get("/api/v1/nope").status, 404)
        self.assertEqual(self.c.get("/api/v1/products/").status, 404)
        self.assertEqual(self.c.get("/api/v1//products").status, 404)

    def test_responses_are_json_and_not_sniffable(self):
        r = self.c.get("/api/v1/products?limit=1")
        self.assertEqual(r.headers["content-type"], "application/json")
        self.assertEqual(r.headers["x-content-type-options"], "nosniff")
        self.assertEqual(r.headers["cache-control"], "no-store")


if __name__ == "__main__":
    unittest.main()
