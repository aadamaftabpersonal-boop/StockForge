"""The demo order history (db/seed/002_demo_history.sql) must satisfy every invariant the engine does,
and give the admin dashboard something real to show."""
import json
import os
import subprocess
import unittest

import dbtools
from harness import ADMIN, Server, logged_in


class DemoHistory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        dbtools._mysql(open(os.path.join(dbtools.REPO, "db", "seed", "002_demo_history.sql"), encoding="utf-8").read(), dbtools.TEST_DB)

    @classmethod
    def tearDownClass(cls):
        dbtools.reset_db()                                       # leave the shared test DB in its normal state

    def test_every_invariant_holds_on_the_generated_history(self):
        self.assertEqual(dbtools.invariant_violations(), {})
        cli = os.path.join(os.path.dirname(os.environ.get("SF_BIN", "build/rel/stockforged")), "reconcile")
        r = subprocess.run([cli], env=dict(os.environ, SF_DB_NAME=dbtools.TEST_DB), capture_output=True, text=True, timeout=60)
        rep = json.loads(r.stdout)
        self.assertEqual((r.returncode, rep["ok"]), (0, True), [c for c in rep["checks"] if not c["ok"]])

    def test_base_stock_levels_and_the_last_unit_are_untouched(self):
        self.assertEqual(dbtools.q("SELECT on_hand, reserved FROM inventory i JOIN products p ON p.id=i.product_id "
                                   "WHERE p.sku='SF-KB-FOUNDER'"), [["1", "0"]])
        self.assertEqual(dbtools.scalar("SELECT COUNT(*) FROM order_items WHERE sku='SF-KB-FOUNDER'"), "0")

    def test_history_is_rich_enough_to_chart(self):
        n = int(dbtools.scalar("SELECT COUNT(*) FROM orders"))
        self.assertGreaterEqual(n, 120)
        statuses = {r[0] for r in dbtools.q("SELECT DISTINCT status FROM orders")}
        self.assertEqual(statuses, {"PENDING_PAYMENT", "PAID", "SHIPPED", "DELIVERED", "CANCELLED", "EXPIRED"})
        self.assertGreaterEqual(int(dbtools.scalar("SELECT COUNT(DISTINCT DATE(created_at)) FROM orders")), 25)   # many distinct days
        self.assertGreaterEqual(int(dbtools.scalar("SELECT COUNT(*) FROM audit_events")), 300)

    def test_dashboard_and_console_work_on_it(self):
        srv = Server(SF_WORKERS=4)
        try:
            adm = logged_in(srv, ADMIN)
            s = adm.get("/api/v1/admin/stats").json
            self.assertGreater(s["revenue"]["total_cents"], 100000)
            self.assertGreaterEqual(len(s["daily"]), 10)
            self.assertEqual(len(s["top_products_30d"]), 5)
            self.assertTrue(adm.get("/api/v1/admin/reconcile").json["ok"])
            first = adm.get("/api/v1/admin/orders?limit=5").json["items"]
            self.assertEqual(len(first), 5)
            self.assertEqual(adm.get(f"/api/v1/admin/orders/{first[0]['id']}").status, 200)
            self.assertGreater(adm.get("/api/v1/admin/users?limit=100").json["items"][0]["id"], 3)
        finally:
            self.assertEqual(srv.stop(), 0)


if __name__ == "__main__":
    unittest.main()
