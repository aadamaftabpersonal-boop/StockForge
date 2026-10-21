"""Hostile-input fuzzing of the HTTP layer and the JSON handlers.

Thousands of mutated requests (reproducible via FUZZ_SEED). The server must
never crash, never answer 500, never hang a worker, and every error must use the
standard JSON envelope. Run under ASan/UBSan (scripts/check.sh with SAN=...) to
also catch memory errors the responses would not reveal; the harness fails the
test if a sanitizer report appears in the server log."""
import json
import os
import random
import socket
import threading
import unittest
from collections import Counter

import dbtools
from harness import Server, logged_in

SCALE = float(os.environ.get("STRESS_SCALE", "1"))
VALID_STATUS = {200, 201, 400, 401, 402, 403, 404, 405, 408, 409, 413, 415, 422, 429, 431, 501}


KEYS = ["email", "password", "display_name", "quantity", "scenario", "expected_total_cents", "sku", "name",
        "description", "price_cents", "category_id", "initial_stock", "delta", "reason", "image_url", "x"]


def rand_body(rnd):
    """Mostly JSON objects with the field names the handlers actually read (so validation
    code is reached), sometimes arbitrary JSON values."""
    if rnd.random() < 0.15:
        return rand_json(rnd)
    return {k: rand_json(rnd, 1) for k in rnd.sample(KEYS, rnd.randint(1, 5))}


def build(method, path, headers, body=b""):
    h = [f"{method} {path} HTTP/1.1", "Host: 127.0.0.1"] + [f"{k}: {v}" for k, v in headers.items()]
    if body:
        h.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(h) + "\r\n\r\n").encode("latin1", "replace") + body


def rand_json(rnd, depth=0):
    kinds = ["str", "int", "float", "bool", "null"] + (["obj", "arr"] if depth < 6 else [])
    k = rnd.choice(kinds)
    if k == "str":
        return rnd.choice(["", "a" * rnd.choice([1, 50, 2000]), "\u0000", "\ud800", "é☃😀", "'; DROP TABLE users;--",
                           "<script>", "%00", "\\", "\"", "../../etc/passwd", "1e999", " " * 40])
    if k == "int":
        return rnd.choice([0, -1, 1, 2**31, 2**53, 2**63, -2**63, 10**30, 99])
    if k == "float":
        return rnd.choice([0.5, -0.0, 1e308, 1e-320, 1.5, 3.14159])
    if k == "bool":
        return rnd.random() < 0.5
    if k == "null":
        return None
    if k == "arr":
        return [rand_json(rnd, depth + 1) for _ in range(rnd.randint(0, 4))]
    return {rnd.choice(KEYS): rand_json(rnd, depth + 1) for _ in range(rnd.randint(0, 6))}


def mutate(rnd, raw):
    b = bytearray(raw)
    for _ in range(rnd.randint(1, 4)):
        op = rnd.randrange(9)
        if not b:
            break
        i = rnd.randrange(len(b))
        if op == 0:
            for _ in range(rnd.randint(1, 8)):
                b[rnd.randrange(len(b))] = rnd.randrange(256)
        elif op == 1:
            del b[i:i + rnd.randint(1, 30)]
        elif op == 2:
            b[i:i] = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, 40)))
        elif op == 3:
            b[i:i] = rnd.choice([b"\r\n", b"\x00", b"\xff" * 20, b"A" * 5000, b"%00%zz", b"\r\n\r\n", b"\n", b"\t"])
        elif op == 4:
            lines = bytes(b).split(b"\r\n")
            j = rnd.randrange(len(lines))
            lines.insert(j, lines[j])
            b = bytearray(b"\r\n".join(lines))
        elif op == 5:
            b = b[:i]
        elif op == 6:
            b = bytearray(bytes(b).replace(b"Content-Length: ", b"Content-Length: " + rnd.choice([b"-", b"+", b"0x", b"99999999999", b" ", b"1,"]), 1))
        elif op == 7:
            b = bytearray(bytes(b).replace(b"HTTP/1.1", rnd.choice([b"HTTP/9.9", b"HTTP/1.0", b"HTTP/", b"http/1.1", b""]), 1))
        else:
            b = bytearray(bytes(b).replace(b"Host: 127.0.0.1", rnd.choice([b"Host:", b"host: a\r\nhost: b", b"Transfer-Encoding: chunked"]), 1))
    return bytes(b)


def exchange(port, payload, shutdown_write):
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.settimeout(2.0)
    try:
        s.sendall(payload)
        if shutdown_write:
            s.shutdown(socket.SHUT_WR)
        data = b""
        while len(data) < 200000:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
        return data
    except OSError:
        return b""                                            # server closed on us: legitimate for garbage
    finally:
        s.close()


def parse_response(data):
    if not data:
        return None, None
    head, _, body = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return -1, body
    return status, body


class Fuzz(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=8, SF_QUEUE_CAP=4096, SF_READ_TIMEOUT_MS=300, SF_MAX_BODY=8192)
        cls.user = dbtools.make_users(1)[0]
        cls.admin = logged_in(cls.srv, ("admin@stockforge.test", "Admin-Demo-2026!"))

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def run_fuzz(self, make_requests, n, label):
        seed = int(os.environ.get("FUZZ_SEED", random.randrange(1 << 30)))
        counts, problems, lock = Counter(), [], threading.Lock()
        per_thread = max(1, int(n * SCALE) // 8)

        def worker(t):
            rnd = random.Random(seed * 1000 + t)
            for _ in range(per_thread):
                payload, shut = make_requests(rnd)
                status, body = parse_response(exchange(self.srv.port, payload, shut))
                with lock:
                    counts[status] += 1
                    if status is None:
                        continue
                    if status not in VALID_STATUS:
                        problems.append((status, payload[:200]))
                    elif status >= 400:
                        try:
                            env = json.loads(body)["error"]
                            assert env["code"] and env["message"] and len(env["request_id"]) == 16
                        except Exception:                                      # noqa: BLE001
                            problems.append(("bad error envelope", status, body[:120], payload[:120]))

        ts = [threading.Thread(target=worker, args=(t,)) for t in range(8)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        print(f"\n    [info] {label}: {sum(counts.values())} requests, seed={seed} (FUZZ_SEED to replay), "
              f"statuses={dict(sorted(counts.items(), key=lambda kv: str(kv[0])))}")
        self.assertEqual(problems[:3], [])
        self.assertIsNone(self.srv.proc.poll(), "server died")
        st = exchange(self.srv.port, b"GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", False)
        self.assertTrue(st.startswith(b"HTTP/1.1 200"), st[:100])                # still serving, no stuck workers

    def test_mutated_http_requests(self):
        u = self.user
        auth = {"Cookie": f"sf_sid={u[1]}", "X-CSRF-Token": u[2], "Content-Type": "application/json"}
        seeds = [
            build("GET", "/healthz", {}),
            build("GET", "/api/v1/products?limit=5&sort=price_desc&q=wireless&category=audio", {}),
            build("GET", "/api/v1/products/1", {}),
            build("GET", "/api/v1/cart", auth),
            build("PUT", "/api/v1/cart/items/2", auth, b'{"quantity":2}'),
            build("POST", "/api/v1/orders", dict(auth, **{"Idempotency-Key": "fuzz-key-0001"}), b"{}"),
            build("POST", "/api/v1/auth/login", {"Content-Type": "application/json"}, b'{"email":"a@b.co","password":"x"}'),
            build("POST", "/api/v1/auth/register", {"Content-Type": "application/json"},
                  b'{"email":"fz@b.co","password":"long-enough-pw","display_name":"F"}'),
            build("GET", "/api/v1/orders/" + "0" * 26, auth),
            build("POST", "/api/v1/orders/" + "0" * 26 + "/pay", auth, b'{"scenario":"success"}'),
        ]
        self.run_fuzz(lambda rnd: (mutate(rnd, rnd.choice(seeds)), rnd.random() < 0.5), 2400, "mutated HTTP")

    def test_invalid_utf8_in_any_string_input_is_a_client_error_never_a_5xx(self):
        """Found by the mutation fuzzer (a 503 for hostile client input): bytes that are not valid
        UTF-8 reached MySQL in a text parameter. Every string that can reach SQL is covered here."""
        u, adm = self.user, self.admin
        bad_strings = ["%FF", "%FE%FF", "%C3", "%E2%82", "%ED%A0%80", "%F8%88%80%80%80", "abc%80def", "%C0%AF",
                       # VALID UTF-8 that MySQL's full-text parser does not treat as word characters:
                       "%E2%98%85%E2%98%85%E2%98%85", "%F0%9F%98%80%F0%9F%98%80%F0%9F%98%80", "%E2%80%BB%E2%80%BB%E2%80%BB",
                       "%C2%A7%C2%A7%C2%A7", "%E2%82%AC%E2%82%AC%E2%82%AC"]
        auth = {"Cookie": f"sf_sid={u[1]}", "X-CSRF-Token": u[2], "Content-Type": "application/json"}
        adm_auth = {"Cookie": f"sf_sid={adm.cookie}", "X-CSRF-Token": adm.csrf}
        requests = []
        for s in bad_strings:
            requests += [("GET", f"/api/v1/products?q={s}", {}, b""),
                         ("GET", f"/api/v1/products?q=wireless%20{s}", {}, b""),
                         ("GET", f"/api/v1/products?category={s}", {}, b""),
                         ("GET", f"/api/v1/products?sort={s}", {}, b""),
                         ("GET", f"/api/v1/products?after={s}", {}, b""),
                         ("GET", f"/api/v1/admin/audit?action={s}", adm_auth, b"")]
        raw_bodies = [b'{"email":"\xff\xfe@x.co","password":"long-enough-pw","display_name":"x"}',
                      b'{"email":"a@b.co","password":"\xff\xfe\xfd\xfc\xfb\xfa\xf9\xf8\xf7\xf6\xf5"}',
                      b'{"email":"\xc3","password":"x"}', b'{"email":"a@b.co","password":"\xed\xa0\x80\xed\xa0\x80\xed\xa0\x80"}']
        for body in raw_bodies:
            requests += [("POST", "/api/v1/auth/login", {"Content-Type": "application/json"}, body),
                         ("POST", "/api/v1/auth/register", {"Content-Type": "application/json"}, body)]
        requests += [("POST", "/api/v1/orders/" + "0" * 26 + "/pay", auth, b'{"scenario":"\xff"}'),
                     ("POST", "/api/v1/admin/inventory/1/adjust", dict(adm_auth, **{"Content-Type": "application/json"}),
                      b'{"delta":1,"reason":"\xff\xfe"}'),
                     ("POST", "/api/v1/admin/products", dict(adm_auth, **{"Content-Type": "application/json"}),
                      b'{"sku":"A-1","name":"\xff","description":"d","price_cents":5,"category_id":1,"initial_stock":1}'),
                     ("POST", "/api/v1/admin/products", dict(adm_auth, **{"Content-Type": "application/json"}),
                      b'{"sku":"A-2","name":"n","description":"\xc3","price_cents":5,"category_id":1,"initial_stock":1}')]
        fails = []
        for method, path, hdr, body in requests:
            status, _ = parse_response(exchange(self.srv.port, build(method, path, hdr, body), False))
            if status is None or status >= 500:
                fails.append((status, method, path, body[:60]))
        self.assertEqual(fails, [], "\n".join(map(str, fails)))
        self.assertEqual(self.srv.log().count('"level":"error"'), 0, "hostile input must not produce server-side error logs")

    def test_hostile_json_bodies_to_every_body_endpoint(self):
        u, adm = self.user, self.admin
        targets = [
            ("POST", "/api/v1/auth/login", {}, False),
            ("POST", "/api/v1/auth/register", {}, False),
            ("PUT", "/api/v1/cart/items/1", {"Cookie": f"sf_sid={u[1]}", "X-CSRF-Token": u[2]}, False),
            ("POST", "/api/v1/orders", {"Cookie": f"sf_sid={u[1]}", "X-CSRF-Token": u[2]}, True),
            ("POST", "/api/v1/orders/" + "0" * 26 + "/pay", {"Cookie": f"sf_sid={u[1]}", "X-CSRF-Token": u[2]}, False),
            ("POST", "/api/v1/admin/products", {"Cookie": f"sf_sid={adm.cookie}", "X-CSRF-Token": adm.csrf}, False),
            ("POST", "/api/v1/admin/inventory/1/adjust", {"Cookie": f"sf_sid={adm.cookie}", "X-CSRF-Token": adm.csrf}, False),
        ]

        def make(rnd):
            method, path, hdr, needs_key = rnd.choice(targets)
            h = dict(hdr, **{"Content-Type": "application/json"})
            if needs_key:
                h["Idempotency-Key"] = "fuzz-" + str(rnd.randrange(10**9)).zfill(9)
            v = rand_body(rnd)
            if rnd.random() < 0.15:                                            # syntactically broken JSON
                body = json.dumps(v).encode("utf-8", "replace")[: rnd.randint(0, 40)]
            elif rnd.random() < 0.1:
                body = b"[" * rnd.randint(1, 3000) + b"]" * rnd.randint(0, 3000)   # deep nesting
            else:
                body = json.dumps(v, ensure_ascii=rnd.random() < 0.5).encode("utf-8", "replace")
            return build(method, path, h, body), False

        self.run_fuzz(make, 1600, "hostile JSON")
        self.assertEqual(dbtools.invariant_violations(), {})                  # whatever got through left the books balanced


if __name__ == "__main__":
    unittest.main()
