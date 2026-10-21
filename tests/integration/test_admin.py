"""Admin authorization, product creation, and transactional stock adjustment."""
import threading
import unittest
import uuid

import dbtools
from harness import ADMIN, AVA, Client, Server, logged_in

LEDGER_INVARIANT = """
    SELECT COUNT(*) FROM inventory i
    LEFT JOIN (SELECT product_id, SUM(delta_on_hand) s FROM inventory_ledger GROUP BY product_id) l
           ON l.product_id = i.product_id
    WHERE i.on_hand <> COALESCE(l.s, 0)"""


def sku():
    return "T-" + uuid.uuid4().hex[:10].upper()


def new_product(**kw):
    p = {"sku": sku(), "name": "Test Widget", "description": "A test widget.", "price_cents": 1234,
         "category_id": 1, "initial_stock": 10}
    p.update(kw)
    return p


class AdminAccess(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def test_anonymous_is_401_customer_is_403_on_every_admin_route(self):
        routes = [("POST", "/api/v1/admin/products", new_product()),
                  ("POST", "/api/v1/admin/inventory/1/adjust", {"delta": 1, "reason": "restock"}),
                  ("GET", "/api/v1/admin/audit", None)]
        anon, ava = Client(self.srv), logged_in(self.srv, AVA)
        for method, path, body in routes:
            self.assertEqual(anon.call(method, path, body=body, csrf=False).status, 401, path)
            r = ava.call(method, path, body=body)
            self.assertEqual((r.status, r.json["error"]["code"]), (403, "FORBIDDEN"), path)
        self.assertEqual(int(dbtools.scalar("SELECT COUNT(*) FROM products")), 48)    # nothing leaked through

    def test_admin_still_needs_csrf(self):
        adm = logged_in(self.srv, ADMIN)
        r = adm.post("/api/v1/admin/products", new_product(), csrf=False)
        self.assertEqual((r.status, r.json["error"]["code"]), (403, "CSRF_INVALID"))


class AdminProducts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4)
        cls.adm = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def test_create_product_end_to_end(self):
        body = new_product(initial_stock=7, name="Café Brûlée Lamp")
        r = self.adm.post("/api/v1/admin/products", body)
        self.assertEqual(r.status, 201, r.text)
        pid = r.json["id"]
        pub = Client(self.srv).get(f"/api/v1/products/{pid}").json["product"]
        self.assertEqual((pub["sku"], pub["available"], pub["name"]), (body["sku"], 7, "Café Brûlée Lamp"))
        self.assertEqual(dbtools.q(f"SELECT on_hand,reserved FROM inventory WHERE product_id={pid}"), [["7", "0"]])
        self.assertEqual(dbtools.q(f"SELECT delta_on_hand,reason FROM inventory_ledger WHERE product_id={pid}"),
                         [["7", "seed"]])
        audit = dbtools.q(f"SELECT actor_user_id,action,entity_id FROM audit_events WHERE action='product.created' "
                          f"AND entity_id='{pid}'")
        self.assertEqual(audit, [["1", "product.created", str(pid)]])

    def test_duplicate_sku_and_bad_category(self):
        p = new_product()
        self.assertEqual(self.adm.post("/api/v1/admin/products", p).status, 201)
        r = self.adm.post("/api/v1/admin/products", p)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "SKU_TAKEN"))
        r = self.adm.post("/api/v1/admin/products", new_product(category_id=9999))
        self.assertEqual((r.status, r.json["error"]["field"]), (422, "category_id"))
        # a failed create must leave no half-written rows
        self.assertEqual(dbtools.scalar("SELECT COUNT(*) FROM products p LEFT JOIN inventory i "
                                        "ON i.product_id=p.id WHERE i.product_id IS NULL"), "0")

    def test_validation(self):
        bad = [
            (new_product(price_cents=0), "price_cents"), (new_product(price_cents=-5), "price_cents"),
            (new_product(price_cents=1.5), "price_cents"), (new_product(price_cents="12"), "price_cents"),
            (new_product(sku="has space"), "sku"), (new_product(sku="x;drop"), "sku"), (new_product(sku=""), "sku"),
            (new_product(sku="S" * 41), "sku"), (new_product(name=""), "name"), (new_product(name="n" * 201), "name"),
            (new_product(description="d" * 4001), "description"), (new_product(initial_stock=-1), "initial_stock"),
            (new_product(initial_stock=1000001), "initial_stock"),
            (new_product(image_url="javascript:alert(1)"), "image_url"),
            (new_product(image_url="http://insecure.example/x.png"), "image_url"),
            (new_product(image_url="//evil.example/x.png"), "image_url"),     # protocol-relative = external
            (new_product(image_url="data:text/html,x"), "image_url"),
        ]
        for body, field in bad:
            r = self.adm.post("/api/v1/admin/products", body)
            self.assertEqual((r.status, r.json["error"]["field"]), (422, field), body)
        missing = new_product()
        del missing["name"]
        self.assertEqual(self.adm.post("/api/v1/admin/products", missing).json["error"]["field"], "name")

    def test_valid_image_urls_accepted(self):
        for url in ("https://cdn.example/x.png", "/img/x.png", None):
            self.assertEqual(self.adm.post("/api/v1/admin/products", new_product(image_url=url)).status, 201, url)

    def test_html_in_text_fields_is_data_not_markup(self):
        nasty = "<script>alert(1)</script><img src=x onerror=alert(2)>"
        pid = self.adm.post("/api/v1/admin/products", new_product(name=nasty, description=nasty)).json["id"]
        r = Client(self.srv).get(f"/api/v1/products/{pid}")
        self.assertEqual(r.headers["content-type"], "application/json")      # never rendered as HTML
        self.assertEqual(r.headers["x-content-type-options"], "nosniff")
        self.assertEqual(r.json["product"]["name"], nasty)                    # stored verbatim; frontend uses textContent

    def test_ledger_matches_stock_after_everything(self):
        self.assertEqual(dbtools.scalar(LEDGER_INVARIANT), "0")


class StockAdjust(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=8)
        cls.adm = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def make(self, stock):
        return self.adm.post("/api/v1/admin/products", new_product(initial_stock=stock)).json["id"]

    def adjust(self, pid, delta, reason="restock", client=None):
        return (client or self.adm).post(f"/api/v1/admin/inventory/{pid}/adjust", {"delta": delta, "reason": reason})

    def test_adjust_writes_ledger_and_audit_atomically(self):
        pid = self.make(10)
        r = self.adjust(pid, 15)
        self.assertEqual((r.status, r.json["on_hand"], r.json["available"]), (200, 25, 25))
        r = self.adjust(pid, -5, "adjust")
        self.assertEqual(r.json["on_hand"], 20)
        self.assertEqual(dbtools.q(f"SELECT delta_on_hand,reason FROM inventory_ledger WHERE product_id={pid} ORDER BY id"),
                         [["10", "seed"], ["15", "restock"], ["-5", "adjust"]])
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM audit_events WHERE action='inventory.adjusted' "
                                        f"AND entity_id='{pid}'"), "2")

    def test_cannot_go_below_zero_or_below_reserved(self):
        pid = self.make(5)
        r = self.adjust(pid, -6)
        self.assertEqual((r.status, r.json["error"]["code"]), (409, "BELOW_RESERVED"))
        dbtools.q(f"UPDATE inventory SET reserved=3 WHERE product_id={pid}")          # simulate open orders
        self.assertEqual(self.adjust(pid, -3).status, 409)                          # 5-3=2 < reserved 3
        r = self.adjust(pid, -2)                                                     # 5-2=3 == reserved: allowed
        self.assertEqual((r.status, r.json["available"]), (200, 0))
        self.assertEqual(dbtools.q(f"SELECT on_hand,reserved FROM inventory WHERE product_id={pid}"), [["3", "3"]])
        dbtools.q(f"UPDATE inventory SET reserved=0 WHERE product_id={pid}")

    def test_rejected_adjustments_change_nothing(self):
        pid = self.make(5)
        ledger_before = dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE product_id={pid}")
        for body in ({"delta": 0, "reason": "adjust"}, {"delta": 1, "reason": "theft"}, {"delta": 1},
                     {"delta": 1.5, "reason": "adjust"}, {"delta": 10**7, "reason": "adjust"},
                     {"delta": "3", "reason": "adjust"}, {"reason": "adjust"}):
            self.assertEqual(self.adm.post(f"/api/v1/admin/inventory/{pid}/adjust", body).status, 422, body)
        self.assertEqual(self.adjust(pid, -50).status, 409)
        self.assertEqual(self.adjust(999999, 1).status, 404)
        self.assertEqual(dbtools.scalar(f"SELECT on_hand FROM inventory WHERE product_id={pid}"), "5")
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM inventory_ledger WHERE product_id={pid}"), ledger_before)

    def _parallel(self, n, fn):
        results = [None] * n
        gate = threading.Barrier(n)

        def run(i):
            c = Client(self.srv)
            c.cookie, c.csrf = self.adm.cookie, self.adm.csrf
            gate.wait()
            results[i] = fn(c)

        ts = [threading.Thread(target=run, args=(i,)) for i in range(n)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        return results

    def test_concurrent_adjustments_never_lose_updates(self):
        """40 racing +1s must all land. Without SELECT ... FOR UPDATE the
        read-modify-write would silently drop increments."""
        pid = self.make(100)
        res = self._parallel(40, lambda c: self.adjust(pid, 1, client=c).status)
        self.assertEqual(res.count(200), 40, res)
        self.assertEqual(dbtools.scalar(f"SELECT on_hand FROM inventory WHERE product_id={pid}"), "140")
        self.assertEqual(dbtools.scalar(f"SELECT SUM(delta_on_hand) FROM inventory_ledger WHERE product_id={pid}"), "140")

    def test_concurrent_decrements_cannot_overdraw(self):
        pid = self.make(5)
        res = self._parallel(20, lambda c: self.adjust(pid, -1, "adjust", client=c).status)
        self.assertEqual((res.count(200), res.count(409)), (5, 15), res)            # exactly 5 succeed
        self.assertEqual(dbtools.scalar(f"SELECT on_hand FROM inventory WHERE product_id={pid}"), "0")

    def test_global_ledger_invariant_after_chaos(self):
        self.assertEqual(dbtools.scalar(LEDGER_INVARIANT), "0")


class AuditApi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server()
        cls.adm = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def test_audit_explorer(self):
        for _ in range(3):
            self.adm.post("/api/v1/admin/products", new_product())
        r = self.adm.get("/api/v1/admin/audit?limit=2")
        self.assertEqual(r.status, 200)
        self.assertEqual(len(r.json["items"]), 2)
        ids = [e["id"] for e in r.json["items"]]
        self.assertEqual(ids, sorted(ids, reverse=True))
        self.assertIsNotNone(r.json["next_cursor"])
        only = self.adm.get("/api/v1/admin/audit?action=product.created&limit=50").json["items"]
        self.assertEqual(len(only), 3)
        self.assertTrue(all(e["action"] == "product.created" and e["actor_user_id"] == 1 for e in only))
        self.assertEqual(only[0]["details"]["initial_stock"], 10)
        self.assertEqual(len(only[0]["request_id"]), 16)
        self.assertEqual(self.adm.get("/api/v1/admin/audit?after=abc").status, 422)


if __name__ == "__main__":
    unittest.main()
