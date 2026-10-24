"""Authorization matrix: EVERY route x (anonymous, customer, admin).

A meta-test parses src/router.c and fails if a route exists that is not in this
matrix, so a new endpoint can never ship without a deliberate authorization decision."""
import os
import re
import unittest
import uuid

import dbtools
from harness import Client, Server, session_client

SOME_ORDER = "0" * 26                       # well-formed public id that does not exist
KEY = lambda: {"Idempotency-Key": "authz-" + uuid.uuid4().hex}      # noqa: E731

# (method, path, body, headers_fn, (anonymous, customer, admin))
MATRIX = [
    ("GET",    "/healthz",                               None, None, (200, 200, 200)),
    ("GET",    "/metrics",                               None, None, (401, 401, 200)),
    ("POST",   "/api/v1/auth/register",                  {},   None, (422, 422, 422)),
    ("POST",   "/api/v1/auth/login",                     {},   None, (422, 422, 422)),
    ("POST",   "/api/v1/auth/logout",                    None, None, (401, 200, 200)),
    ("GET",    "/api/v1/auth/me",                        None, None, (401, 200, 200)),
    ("GET",    "/api/v1/categories",                     None, None, (200, 200, 200)),
    ("GET",    "/api/v1/products",                       None, None, (200, 200, 200)),
    ("GET",    "/api/v1/products/1",                     None, None, (200, 200, 200)),
    ("GET",    "/api/v1/cart",                           None, None, (401, 200, 200)),
    ("DELETE", "/api/v1/cart",                           None, None, (401, 200, 200)),
    ("PUT",    "/api/v1/cart/items/1",                   {"quantity": 1}, None, (401, 200, 200)),
    ("DELETE", "/api/v1/cart/items/1",                   None, None, (401, 200, 200)),
    ("GET",    "/api/v1/orders",                         None, None, (401, 200, 200)),
    ("POST",   "/api/v1/orders",                         {},   KEY,  (401, 409, 409)),
    ("GET",    f"/api/v1/orders/{SOME_ORDER}",           None, None, (401, 404, 404)),
    ("POST",   f"/api/v1/orders/{SOME_ORDER}/pay",       None, None, (401, 404, 404)),
    ("POST",   f"/api/v1/orders/{SOME_ORDER}/cancel",    None, None, (401, 404, 404)),
    ("POST",   "/api/v1/admin/products",                 {},   None, (401, 403, 422)),
    ("POST",   "/api/v1/admin/inventory/99999/adjust",   {"delta": 1, "reason": "restock"}, None, (401, 403, 404)),
    ("GET",    "/api/v1/admin/audit",                    None, None, (401, 403, 200)),
    ("GET",    "/api/v1/admin/reconcile",                None, None, (401, 403, 200)),
    ("GET",    "/api/v1/admin/stats",                    None, None, (401, 403, 200)),
    ("GET",    "/api/v1/admin/metrics",                  None, None, (401, 403, 200)),
    ("GET",    "/api/v1/admin/orders",                   None, None, (401, 403, 200)),
    ("GET",    f"/api/v1/admin/orders/{SOME_ORDER}",     None, None, (401, 403, 404)),
    ("POST",   f"/api/v1/admin/orders/{SOME_ORDER}/transition", {"to": "SHIPPED"}, None, (401, 403, 404)),
    ("GET",    "/api/v1/admin/products",                 None, None, (401, 403, 200)),
    ("PATCH",  "/api/v1/admin/products/1",               {},   None, (401, 403, 422)),
    ("POST",   "/api/v1/admin/categories",               {},   None, (401, 403, 422)),
    ("PATCH",  "/api/v1/admin/categories/1",             {},   None, (401, 403, 422)),
    ("GET",    "/api/v1/admin/users",                    None, None, (401, 403, 200)),
    ("PATCH",  "/api/v1/admin/users/99999",              {"is_active": True}, None, (401, 403, 404)),
]


class AuthzMatrix(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4, SF_METRICS_TOKEN="")       # no scraper token: sessions only

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def client(self, role):
        if role == "anonymous":
            return Client(self.srv)
        u = dbtools.make_users(1)[0]
        if role == "admin":
            dbtools.q(f"UPDATE users SET role='admin' WHERE id={u[0]}")
        return session_client(self.srv, u)

    def test_every_route_for_every_role(self):
        for method, path, body, hdr, expected in MATRIX:
            for role, want in zip(("anonymous", "customer", "admin"), expected):
                c = self.client(role)                              # fresh session: logout must not poison later rows
                r = c.call(method, path, body=body, headers=hdr() if hdr else None)
                self.assertEqual(r.status, want, f"{role} {method} {path} -> {r.status} {r.text[:150]}")

    def test_matrix_covers_every_route_in_the_router(self):
        src = open(os.path.join(dbtools.REPO, "src", "router.c"), encoding="utf-8").read()
        in_router = set(re.findall(r'\{"(GET|POST|PUT|PATCH|DELETE)",\s*"([^"]+)"', src))    # PATCH included: a verb the check cannot see is a hole
        in_router = {(m, p) for m, p in in_router if not p.startswith("/debug/")}
        norm = lambda p: re.sub(r":\w+", "*", p)                    # noqa: E731
        router_set = {(m, norm(p)) for m, p in in_router}
        matrix_set = {(m, norm(re.sub(r"/(\d+|0{26})(?=/|$)", "/*", p))) for m, p, *_ in MATRIX}
        self.assertGreater(len(router_set), 15)
        self.assertEqual(router_set - matrix_set, set(), "route(s) with no authorization test")
        self.assertEqual(matrix_set - router_set, set(), "matrix lists route(s) that no longer exist")

    def test_every_authenticated_unsafe_route_demands_csrf(self):
        for method, path, body, hdr, expected in MATRIX:
            if method == "GET" or expected[0] != 401:
                continue                                           # safe, or not session-protected
            admin = self.client("admin")
            r = admin.call(method, path, body=body, headers=hdr() if hdr else None, csrf=False)
            self.assertEqual((r.status, r.json["error"]["code"]), (403, "CSRF_INVALID"), f"{method} {path}")

    def test_session_of_deleted_or_demoted_user_loses_power_immediately(self):
        adm = self.client("admin")
        self.assertEqual(adm.get("/api/v1/admin/audit").status, 200)
        uid = adm.get("/api/v1/auth/me").json["user"]["id"]
        dbtools.q(f"UPDATE users SET role='customer' WHERE id={uid}")
        self.assertEqual(adm.get("/api/v1/admin/audit").status, 403)           # role is read per request, not cached in the session
        dbtools.q(f"UPDATE users SET is_active=0 WHERE id={uid}")
        self.assertEqual(adm.get("/api/v1/auth/me").status, 401)

    def test_customers_cannot_read_each_others_data_by_id_guessing(self):
        a, b = self.client("customer"), self.client("customer")
        p = dbtools.new_product(5, 1000)
        uid_a = a.get("/api/v1/auth/me").json["user"]["id"]
        dbtools.put_cart(uid_a, [(p, 1)])
        from harness import checkout
        oid = checkout(a).json["order"]["id"]
        self.assertEqual(a.get(f"/api/v1/orders/{oid}").status, 200)
        for call in (lambda: b.get(f"/api/v1/orders/{oid}"), lambda: b.post(f"/api/v1/orders/{oid}/pay"),
                     lambda: b.post(f"/api/v1/orders/{oid}/cancel")):
            self.assertEqual(call().status, 404)                   # indistinguishable from "does not exist"
        self.assertEqual(a.get(f"/api/v1/orders/{oid}").json["order"]["status"], "PENDING_PAYMENT")


if __name__ == "__main__":
    unittest.main()
