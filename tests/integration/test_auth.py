"""Identity: registration, login, sessions, CSRF, origin checks, rate limits."""
import hashlib
import time
import unittest
import uuid

import dbtools
from harness import ADMIN, AVA, BEN, Client, Server, logged_in, metrics

PW = "correct-horse-battery-staple"


def new_email():
    return f"user-{uuid.uuid4().hex[:10]}@example.test"


def register(c, email=None, pw=PW, name="Test User"):
    email = email or new_email()
    r = c.post("/api/v1/auth/register", {"email": email, "password": pw, "display_name": name}, csrf=False)
    return email, r


class Auth(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=4)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def client(self):
        return Client(self.srv)

    # ------------------------------------------------------------ register
    def test_register_then_login_then_me(self):
        c = self.client()
        email, r = register(c)
        self.assertEqual(r.status, 201)
        self.assertEqual(r.json["user"]["email"], email)
        self.assertEqual(r.json["user"]["role"], "customer")
        self.assertNotIn("password", r.text.lower().replace("password_", ""))   # no hash/echo in response
        self.assertNotIn("$argon2", r.text)
        l = c.login((email, PW))
        self.assertEqual(l.json["user"]["email"], email)
        me = c.get("/api/v1/auth/me")
        self.assertEqual(me.status, 200)
        self.assertEqual(me.json["csrf_token"], c.csrf)

    def test_password_stored_as_argon2id_never_plaintext(self):
        email, _ = register(self.client(), pw="plaintext-must-not-appear-1")
        h = dbtools.scalar(f"SELECT password_hash FROM users WHERE email='{email}'")
        self.assertTrue(h.startswith("$argon2id$"))
        self.assertNotIn("plaintext-must-not-appear-1", h)

    def test_email_is_case_insensitive_and_unique(self):
        c = self.client()
        email, r = register(c, "MiXeD.Case@Example.test")
        self.assertEqual(r.status, 201)
        self.assertEqual(r.json["user"]["email"], "mixed.case@example.test")
        _, r2 = register(c, "mixed.case@example.test")
        self.assertEqual(r2.status, 409)
        self.assertEqual(r2.json["error"]["code"], "EMAIL_TAKEN")
        self.assertEqual(c.post("/api/v1/auth/login", {"email": "MIXED.CASE@EXAMPLE.TEST", "password": PW},
                                csrf=False).status, 200)

    def test_register_validation(self):
        c = self.client()
        cases = [
            ({"email": "nope", "password": PW, "display_name": "x"}, "email"),
            ({"email": new_email(), "password": "short", "display_name": "x"}, "password"),
            ({"email": new_email(), "password": PW, "display_name": ""}, "display_name"),
            ({"email": new_email(), "password": PW, "display_name": "a\tb"}, "display_name"),
            ({"password": PW, "display_name": "x"}, "email"),
            ({"email": 123, "password": PW, "display_name": "x"}, "email"),
            ({"email": new_email(), "password": ["a"], "display_name": "x"}, "password"),
        ]
        for body, field in cases:
            r = c.post("/api/v1/auth/register", body, csrf=False)
            self.assertEqual(r.status, 422, body)
            self.assertEqual(r.json["error"]["field"], field, body)

    def test_malformed_bodies(self):
        c = self.client()
        self.assertEqual(c.post("/api/v1/auth/register", raw=b"{not json", csrf=False).json["error"]["code"], "INVALID_JSON")
        self.assertEqual(c.post("/api/v1/auth/register", raw=b"[1,2]", csrf=False).status, 400)
        self.assertEqual(c.post("/api/v1/auth/register", raw=b"null", csrf=False).status, 400)
        self.assertEqual(c.call("POST", "/api/v1/auth/register", csrf=False).json["error"]["code"], "EMPTY_BODY")
        deep = b"[" * 5000 + b"]" * 5000
        self.assertEqual(c.post("/api/v1/auth/register", raw=deep, csrf=False).status, 400)

    def test_content_type_must_be_json(self):
        c = self.client()
        r = c.post("/api/v1/auth/login", raw=b"email=a&password=b", ctype="application/x-www-form-urlencoded", csrf=False)
        self.assertEqual(r.status, 415)
        r = c.post("/api/v1/auth/login", raw=b"{}", ctype="text/plain", csrf=False)
        self.assertEqual(r.status, 415)

    # --------------------------------------------------------------- login
    def test_login_failures_are_indistinguishable(self):
        c = self.client()
        email, _ = register(c)
        wrong = c.post("/api/v1/auth/login", {"email": email, "password": "wrong-password-123"}, csrf=False)
        ghost = c.post("/api/v1/auth/login", {"email": new_email(), "password": "wrong-password-123"}, csrf=False)
        self.assertEqual((wrong.status, ghost.status), (401, 401))
        self.assertEqual(wrong.json["error"]["code"], ghost.json["error"]["code"])
        self.assertEqual(wrong.json["error"]["message"], ghost.json["error"]["message"])
        self.assertNotIn("set-cookie", wrong.headers)

    def test_unknown_account_still_pays_for_argon2(self):
        """Timing side channel: a missing account must not return instantly."""
        c = self.client()
        t = []
        for _ in range(3):
            t0 = time.perf_counter()
            c.post("/api/v1/auth/login", {"email": new_email(), "password": "wrong-password-123"}, csrf=False)
            t.append(time.perf_counter() - t0)
        self.assertGreater(min(t), 0.008, f"unknown-account login too fast: {t}")

    def test_inactive_account_cannot_login_or_use_session(self):
        c = self.client()
        email, _ = register(c)
        c.login((email, PW))
        self.assertEqual(c.get("/api/v1/auth/me").status, 200)
        dbtools.q(f"UPDATE users SET is_active=0 WHERE email='{email}'")
        self.assertEqual(c.get("/api/v1/auth/me").status, 401)                 # live session dies
        r = self.client().post("/api/v1/auth/login", {"email": email, "password": PW}, csrf=False)
        self.assertEqual(r.status, 401)

    def test_sql_injection_attempts_are_inert(self):
        c = self.client()
        for evil in ["' OR '1'='1", "admin@stockforge.test'--", "x'; DROP TABLE users; --"]:
            r = c.post("/api/v1/auth/login", {"email": evil, "password": "whatever-pass-1"}, csrf=False)
            self.assertEqual(r.status, 401, evil)
        r = c.post("/api/v1/auth/register", {"email": "a'; DROP TABLE users;--@x.com", "password": PW,
                                             "display_name": "x"}, csrf=False)
        self.assertEqual(r.status, 422)
        self.assertGreaterEqual(int(dbtools.scalar("SELECT COUNT(*) FROM users")), 3)

    # ------------------------------------------------------------ sessions
    def test_cookie_attributes(self):
        r = Client(self.srv).post("/api/v1/auth/login", {"email": AVA[0], "password": AVA[1]}, csrf=False)
        ck = r.headers["set-cookie"]
        self.assertIn("HttpOnly", ck)
        self.assertIn("SameSite=Lax", ck)
        self.assertIn("Path=/", ck)
        self.assertIn("Max-Age=", ck)

    def test_defensive_headers_on_every_kind_of_response(self):
        c = self.client()
        for r in (c.get("/api/v1/products?limit=1"), c.get("/api/v1/nope"), c.get("/api/v1/auth/me"),
                  c.post("/api/v1/auth/login", raw=b"{bad", csrf=False)):                # success, 404, 401, 400
            self.assertEqual(r.headers["x-frame-options"], "DENY")
            self.assertEqual(r.headers["referrer-policy"], "no-referrer")
            self.assertEqual(r.headers["content-security-policy"], "default-src 'none'; frame-ancestors 'none'")
            self.assertEqual(r.headers["x-content-type-options"], "nosniff")
            self.assertEqual(r.headers["cache-control"], "no-store")
            self.assertNotIn("server", r.headers)                                       # no version banner

    def test_sessions_per_user_are_capped(self):
        email, _ = register(self.client(), pw=PW)
        uid = dbtools.scalar(f"SELECT id FROM users WHERE email='{email}'")
        cookies = []
        for _ in range(13):
            c = self.client()
            c.login((email, PW))
            cookies.append(c)
            time.sleep(0.005)                                                           # distinct created_at ticks
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM sessions WHERE user_id={uid}"), "10")
        self.assertEqual(cookies[0].get("/api/v1/auth/me").status, 401)                 # oldest were evicted
        self.assertEqual(cookies[-1].get("/api/v1/auth/me").status, 200)                # newest still valid

    def test_session_token_is_hashed_at_rest_and_unique_per_login(self):
        c1, c2 = logged_in(self.srv, AVA), logged_in(self.srv, AVA)
        self.assertNotEqual(c1.cookie, c2.cookie)                              # fresh token each login
        self.assertEqual(len(c1.cookie), 43)
        digest = hashlib.sha256(c1.cookie.encode()).hexdigest()
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM sessions WHERE token_hash=UNHEX('{digest}')"), "1")
        self.assertEqual(dbtools.scalar(f"SELECT COUNT(*) FROM sessions WHERE csrf_token='{c1.cookie}'"), "0")
        self.assertNotEqual(c1.csrf, c1.cookie)                                # csrf != session token

    def test_garbage_and_missing_cookies_are_rejected(self):
        c = self.client()
        self.assertEqual(c.get("/api/v1/auth/me").status, 401)
        for bad in ["x", "A" * 43, "A" * 200, "'; DROP TABLE sessions;--", ""]:
            c.cookie = bad
            self.assertEqual(c.get("/api/v1/auth/me").status, 401, bad)

    def test_expired_session_rejected(self):
        c = logged_in(self.srv, BEN)
        self.assertEqual(c.get("/api/v1/auth/me").status, 200)
        digest = hashlib.sha256(c.cookie.encode()).hexdigest()
        dbtools.q(f"UPDATE sessions SET expires_at = NOW() - INTERVAL 1 SECOND WHERE token_hash=UNHEX('{digest}')")
        self.assertEqual(c.get("/api/v1/auth/me").status, 401)

    def test_logout_revokes_server_side(self):
        c = logged_in(self.srv, AVA)
        stolen = Client(self.srv)
        stolen.cookie, stolen.csrf = c.cookie, c.csrf
        self.assertEqual(c.post("/api/v1/auth/logout").status, 200)
        self.assertEqual(stolen.get("/api/v1/auth/me").status, 401)            # replaying the old cookie fails

    # ----------------------------------------------------------- CSRF/origin
    def test_csrf_required_on_unsafe_authenticated_requests(self):
        c = logged_in(self.srv, AVA)
        r = c.post("/api/v1/auth/logout", csrf=False)
        self.assertEqual((r.status, r.json["error"]["code"]), (403, "CSRF_INVALID"))
        r = c.post("/api/v1/auth/logout", headers={"X-CSRF-Token": "A" * 43})
        self.assertEqual(r.status, 403)
        r = c.post("/api/v1/auth/logout", headers={"X-CSRF-Token": c.csrf[:-1]})
        self.assertEqual(r.status, 403)
        self.assertEqual(c.get("/api/v1/auth/me").status, 200)                 # session survived the attacks
        self.assertEqual(c.post("/api/v1/auth/logout").status, 200)

    def test_csrf_token_of_another_session_is_rejected(self):
        a, b = logged_in(self.srv, AVA), logged_in(self.srv, BEN)
        r = a.post("/api/v1/auth/logout", headers={"X-CSRF-Token": b.csrf})
        self.assertEqual(r.status, 403)

    def test_cross_origin_requests_rejected(self):
        c = self.client()
        r = c.post("/api/v1/auth/login", {"email": AVA[0], "password": AVA[1]}, csrf=False,
                   headers={"Origin": "https://evil.example"})
        self.assertEqual((r.status, r.json["error"]["code"]), (403, "ORIGIN_MISMATCH"))
        r = c.post("/api/v1/auth/login", {"email": AVA[0], "password": AVA[1]}, csrf=False,
                   headers={"Origin": f"http://127.0.0.1:{self.srv.port}"})
        self.assertEqual(r.status, 200)
        r = c.post("/api/v1/auth/login", {"email": AVA[0], "password": AVA[1]}, csrf=False,
                   headers={"Origin": "null"})
        self.assertEqual(r.status, 403)

    # ----------------------------------------------------- secrets hygiene
    def test_no_secrets_in_server_log_or_audit(self):
        c = self.client()
        pw = "S3cret-Canary-Password-xyz"
        email, _ = register(c, pw=pw)
        c.login((email, pw))
        c.post("/api/v1/auth/login", {"email": email, "password": pw + "wrong"}, csrf=False)
        token, csrf = c.cookie, c.csrf
        c.get("/api/v1/auth/me")
        log = self.srv.log()
        for secret in (pw, token, csrf):
            self.assertNotIn(secret, log)
        audit = " ".join(r[0] for r in dbtools.q("SELECT COALESCE(CAST(details AS CHAR),'') FROM audit_events"))
        for secret in (pw, token, csrf):
            self.assertNotIn(secret, audit)
        self.assertEqual(dbtools.scalar("SELECT COUNT(*) FROM audit_events WHERE action='auth.login_failed'") != "0", True)


class RateLimits(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()

    def test_per_account_login_limit(self):
        srv = Server(SF_RL_LOGIN_ACCT=3)
        try:
            c = Client(srv)
            codes = [c.post("/api/v1/auth/login", {"email": AVA[0], "password": "bad-password-1"}, csrf=False).status
                     for _ in range(4)]
            self.assertEqual(codes, [401, 401, 401, 429])
            r = c.post("/api/v1/auth/login", {"email": AVA[0], "password": AVA[1]}, csrf=False)
            self.assertEqual(r.status, 429)                                   # even the right password is throttled
            self.assertGreaterEqual(int(r.headers["retry-after"]), 1)
            self.assertEqual(r.json["error"]["code"], "RATE_LIMITED")
            other = c.post("/api/v1/auth/login", {"email": BEN[0], "password": BEN[1]}, csrf=False)
            self.assertEqual(other.status, 200)                               # other accounts unaffected
        finally:
            self.assertEqual(srv.stop(), 0)

    def test_per_ip_login_and_register_limits(self):
        srv = Server(SF_RL_LOGIN_IP=2, SF_RL_REGISTER_IP=2)
        try:
            c = Client(srv)
            self.assertEqual([c.post("/api/v1/auth/login", {"email": new_email(), "password": "x" * 12}, csrf=False).status
                              for _ in range(3)], [401, 401, 429])
            self.assertEqual([register(c)[1].status for _ in range(3)], [201, 201, 429])
        finally:
            self.assertEqual(srv.stop(), 0)

    def test_secure_cookie_flag_when_enabled(self):
        srv = Server(SF_COOKIE_SECURE=1)
        try:
            r = Client(srv).post("/api/v1/auth/login", {"email": AVA[0], "password": AVA[1]}, csrf=False)
            self.assertIn("Secure", r.headers["set-cookie"])
        finally:
            self.assertEqual(srv.stop(), 0)

    def test_forwarded_for_is_ignored_unless_proxy_trusted(self):
        srv = Server(SF_RL_LOGIN_IP=2)                                        # SF_TRUST_PROXY defaults to 0
        try:
            c = Client(srv)
            codes = [c.post("/api/v1/auth/login", {"email": new_email(), "password": "x" * 12}, csrf=False,
                            headers={"X-Forwarded-For": f"203.0.113.{i}"}).status for i in range(3)]
            self.assertEqual(codes, [401, 401, 429])                          # spoofed XFF cannot evade the limit
        finally:
            self.assertEqual(srv.stop(), 0)

    def test_trusted_proxy_uses_last_xff_entry(self):
        srv = Server(SF_RL_LOGIN_IP=2, SF_TRUST_PROXY=1)
        try:
            c = Client(srv)
            # client-supplied leading entries vary, the proxy-appended LAST entry is constant => still limited
            codes = [c.post("/api/v1/auth/login", {"email": new_email(), "password": "x" * 12}, csrf=False,
                            headers={"X-Forwarded-For": f"198.51.100.{i}, 203.0.113.9"}).status for i in range(3)]
            self.assertEqual(codes, [401, 401, 429])
            r = c.post("/api/v1/auth/login", {"email": new_email(), "password": "x" * 12}, csrf=False,
                       headers={"X-Forwarded-For": "198.51.100.1, 203.0.113.77"})
            self.assertEqual(r.status, 401)                                   # a different real client IP has its own budget
        finally:
            self.assertEqual(srv.stop(), 0)


class MetricsAccess(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server()

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def test_metrics_authorization(self):
        self.assertEqual(Client(self.srv).get("/metrics").status, 401)
        self.assertEqual(Client(self.srv).get("/metrics", headers={"Authorization": "Bearer wrong-token-0123456789"}).status, 401)
        self.assertEqual(metrics(self.srv)["workers"]["total"], 8)
        self.assertEqual(logged_in(self.srv, AVA).get("/metrics").status, 401)       # customers: no
        self.assertEqual(logged_in(self.srv, ADMIN).get("/metrics").status, 200)     # admins: yes


if __name__ == "__main__":
    unittest.main()
