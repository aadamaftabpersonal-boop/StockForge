"""Phase 3 integration tests: the thread-pool server behaves under bad input,
overload, disconnects and shutdown. Run: make itest"""
import json
import socket
import threading
import time
import unittest

from harness import Server, get, read_response, request


class Basics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = Server(SF_WORKERS=4, SF_QUEUE_CAP=512, SF_MAX_HEADER=2048,
                         SF_MAX_BODY=1024, SF_READ_TIMEOUT_MS=600)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0, "server must exit 0 after a clean drain"

    def test_healthz(self):
        st, h, body = get(self.srv, "/healthz")
        self.assertEqual(st, 200)
        self.assertEqual(body["status"], "ok")
        self.assertEqual(body["workers"], 4)
        self.assertEqual(len(h["x-request-id"]), 16)
        self.assertEqual(h["x-content-type-options"], "nosniff")

    def test_request_ids_are_unique(self):
        ids = {get(self.srv, "/healthz")[1]["x-request-id"] for _ in range(20)}
        self.assertEqual(len(ids), 20)

    def test_unknown_route_and_error_envelope(self):
        st, _, body = get(self.srv, "/nope")
        self.assertEqual(st, 404)
        self.assertEqual(body["error"]["code"], "NOT_FOUND")
        self.assertIn("request_id", body["error"])

    def test_wrong_method(self):
        st, _, _ = request(self.srv, b"POST /healthz HTTP/1.1\r\nContent-Length: 0\r\n\r\n")
        self.assertEqual(st, 405)

    def test_debug_routes_disabled_by_default(self):
        st, _, _ = get(self.srv, "/debug/sleep?ms=1")
        self.assertEqual(st, 404)

    def test_malformed_request_line(self):
        st, _, _ = request(self.srv, b"THIS IS NOT HTTP\r\n\r\n")
        self.assertEqual(st, 400)

    def test_oversized_headers(self):
        raw = b"GET / HTTP/1.1\r\nX: " + b"a" * 5000 + b"\r\n\r\n"
        st, _, _ = request(self.srv, raw)
        self.assertEqual(st, 431)

    def test_oversized_body_rejected_before_reading_it(self):
        st, _, body = request(self.srv, b"POST /x HTTP/1.1\r\nContent-Length: 999999\r\n\r\n")
        self.assertEqual(st, 413)

    def test_chunked_rejected(self):
        st, _, _ = request(self.srv, b"POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
        self.assertEqual(st, 501)

    def test_duplicate_content_length_rejected(self):
        st, _, _ = request(self.srv, b"POST /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na")
        self.assertEqual(st, 400)

    def test_slowloris_times_out_with_408(self):
        s = self.srv.connect()
        s.sendall(b"GET /healthz HTTP/1.1\r\nHost: slow")     # never finishes
        t0 = time.time()
        st, _, _ = read_response(s)
        s.close()
        self.assertEqual(st, 408)
        self.assertLess(time.time() - t0, 2.0)                 # deadline is 600 ms total

    def test_client_disconnects_do_not_hurt_server(self):
        for _ in range(50):
            s = self.srv.connect()
            s.sendall(b"GET /heal")
            s.close()
            s = self.srv.connect()                              # connect and vanish
            s.close()
            s = self.srv.connect()
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\0\0\0\0\0\0\0")  # RST on close
            s.sendall(b"GET /healthz HTTP/1.1\r\n\r\n")
            s.close()
        time.sleep(0.3)
        st, _, body = get(self.srv, "/healthz")
        self.assertEqual(st, 200)
        self.assertEqual(self.srv.proc.poll(), None)            # still alive (SIGPIPE ignored)

    def test_many_concurrent_requests_all_succeed(self):
        results = []
        def one():
            results.append(get(self.srv, "/healthz")[0])
        threads = [threading.Thread(target=one) for _ in range(200)]
        [t.start() for t in threads]
        [t.join() for t in threads]
        self.assertEqual(results.count(200), 200)

    def test_metrics_count_requests(self):
        before = get(self.srv, "/metrics")[2]["requests_total"]
        for _ in range(5):
            get(self.srv, "/healthz")
        after = get(self.srv, "/metrics")[2]
        self.assertGreaterEqual(after["requests_total"] - before, 5)
        self.assertEqual(sum(after["latency_ms_hist"]["count"]), after["requests_total"])


class Overload(unittest.TestCase):
    def test_full_queue_rejects_with_503_and_recovers(self):
        srv = Server(SF_WORKERS=1, SF_QUEUE_CAP=1, SF_DEBUG=1)
        try:
            busy = srv.connect()                                # occupies the only worker
            busy.sendall(b"GET /debug/sleep?ms=1200 HTTP/1.1\r\n\r\n")
            time.sleep(0.25)
            queued = srv.connect()                              # fills the 1-slot queue
            queued.sendall(b"GET /healthz HTTP/1.1\r\n\r\n")
            time.sleep(0.25)
            st, h, body = request(srv, b"GET /healthz HTTP/1.1\r\n\r\n")   # no room
            self.assertEqual(st, 503)
            self.assertEqual(h.get("retry-after"), "1")
            self.assertEqual(json.loads(body)["error"]["code"], "OVERLOADED")
            self.assertEqual(read_response(busy)[0], 200)       # admitted work still completes
            self.assertEqual(read_response(queued)[0], 200)
            st, _, _ = request(srv, b"GET /healthz HTTP/1.1\r\n\r\n")      # recovered
            self.assertEqual(st, 200)
            m = get(srv, "/metrics")[2]
            self.assertGreaterEqual(m["connections_rejected_overload"], 1)
            self.assertEqual(m["queue"]["high_water"], 1)
        finally:
            self.assertEqual(srv.stop(), 0)


class Shutdown(unittest.TestCase):
    def test_sigterm_drains_inflight_and_exits_zero(self):
        srv = Server(SF_WORKERS=2, SF_QUEUE_CAP=8, SF_DEBUG=1)
        inflight = srv.connect()
        inflight.sendall(b"GET /debug/sleep?ms=800 HTTP/1.1\r\n\r\n")
        time.sleep(0.2)
        import signal
        srv.proc.send_signal(signal.SIGTERM)
        time.sleep(0.2)
        with self.assertRaises(OSError):                        # listener closed => refused
            socket.create_connection(("127.0.0.1", srv.port), 0.5)
        self.assertEqual(read_response(inflight)[0], 200)       # in-flight request finished
        self.assertEqual(srv.stop(), 0)
        self.assertIn("clean shutdown", srv.log())

    def test_sigint_also_shuts_down(self):
        import signal
        srv = Server()
        self.assertEqual(srv.stop(signal.SIGINT), 0)

    def test_idle_shutdown_is_prompt(self):
        srv = Server(SF_WORKERS=16)
        t0 = time.time()
        self.assertEqual(srv.stop(), 0)
        self.assertLess(time.time() - t0, 1.0)                  # all 16 idle workers woke


class Config(unittest.TestCase):
    def test_bad_config_fails_fast(self):
        import os, subprocess
        from harness import BIN
        r = subprocess.run([BIN], env=dict(os.environ, SF_WORKERS="0"), capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("SF_WORKERS", r.stderr)


if __name__ == "__main__":
    unittest.main()
