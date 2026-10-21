"""Shared helpers: spawn stockforged on a free port and speak HTTP to it."""
import http.client
import json
import os
import signal
import socket
import subprocess
import tempfile
import time
from collections import namedtuple

import dbtools

BIN = os.environ.get("SF_BIN", "build/rel/stockforged")
METRICS_TOKEN = "test-metrics-token-0123456789"
ADMIN = ("admin@stockforge.test", "Admin-Demo-2026!")
AVA = ("ava@stockforge.test", "Customer-Demo-2026!")
BEN = ("ben@stockforge.test", "Customer-Demo-2026!")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Server:
    def __init__(self, **env):
        dbtools.ensure_ready()
        self.port = free_port()
        e = dict(
            os.environ,
            SF_PORT=str(self.port), SF_BIND="127.0.0.1",
            SF_DB_NAME=dbtools.TEST_DB, SF_COOKIE_SECURE="0", SF_METRICS_TOKEN=METRICS_TOKEN,
            SF_RL_LOGIN_IP="100000", SF_RL_LOGIN_ACCT="100000", SF_RL_REGISTER_IP="100000",
        )
        e.update({k: str(v) for k, v in env.items()})
        # stderr goes to a file: a full pipe would block a chatty server, and a
        # readiness probe *connection* would consume a queue slot and skew
        # overload tests, so readiness = the server's own "started" log line.
        self.logf = tempfile.TemporaryFile("w+")
        self.proc = subprocess.Popen([BIN], env=e, stderr=self.logf, text=True)
        deadline = time.time() + 10
        while time.time() < deadline:
            if '"event":"started"' in self.log():
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.02)
        self.stop()
        raise RuntimeError("server did not start: " + self.log())

    def stop(self, sig=signal.SIGTERM, timeout=10):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        log = self.log()
        for marker in ("Sanitizer", "runtime error:", "ERROR: AddressSanitizer", "SUMMARY:"):
            if marker in log:     # a sanitizer build found a bug: never let it pass silently
                raise AssertionError(f"sanitizer report in server log:\n{log}")
        return self.proc.returncode

    def log(self):
        self.logf.flush()
        self.logf.seek(0)
        return self.logf.read()

    def connect(self, timeout=5):
        return socket.create_connection(("127.0.0.1", self.port), timeout)


# ------------------------------------------------------------- raw socket API
def read_response(sock):
    """Reads until the server closes (we always send Connection: close)."""
    data = b""
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    status = int(lines[0].split()[1]) if lines and lines[0] else 0
    headers = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(":")
        headers[k.strip().lower()] = v.strip()
    return status, headers, body


def request(server, raw: bytes, timeout=5):
    s = server.connect(timeout)
    s.sendall(raw)
    try:
        return read_response(s)
    finally:
        s.close()


def get(server, path):
    st, h, b = request(server, f"GET {path} HTTP/1.1\r\nHost: t\r\n\r\n".encode())
    return st, h, (json.loads(b) if b else None)


def metrics(server):
    st, h, b = request(server, (f"GET /metrics HTTP/1.1\r\nHost: t\r\n"
                                f"Authorization: Bearer {METRICS_TOKEN}\r\n\r\n").encode())
    assert st == 200, (st, b)
    return json.loads(b)


# ------------------------------------------------------------------ API client
Resp = namedtuple("Resp", "status headers json text")


class Client:
    """A browser-like client: keeps the session cookie and CSRF token."""

    def __init__(self, server):
        self.srv = server
        self.cookie = None
        self.csrf = None

    def call(self, method, path, body=None, headers=None, csrf=True, raw=None, ctype="application/json"):
        h = {"Host": f"127.0.0.1:{self.srv.port}"}
        if self.cookie:
            h["Cookie"] = f"sf_sid={self.cookie}"
        if csrf and self.csrf:
            h["X-CSRF-Token"] = self.csrf
        payload = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        if payload is not None:
            h["Content-Type"] = ctype
        h.update(headers or {})
        conn = http.client.HTTPConnection("127.0.0.1", self.srv.port, timeout=15)
        try:
            conn.request(method, path, body=payload, headers=h)
            r = conn.getresponse()
            data = r.read()
        finally:
            conn.close()
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        try:
            js = json.loads(data) if data else None
        except ValueError:
            js = None
        return Resp(r.status, hdrs, js, data.decode("utf-8", "replace"))

    def get(self, path, **kw):
        return self.call("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.call("POST", path, body=body, **kw)

    def put(self, path, body=None, **kw):
        return self.call("PUT", path, body=body, **kw)

    def delete(self, path, **kw):
        return self.call("DELETE", path, **kw)

    def login(self, creds):
        r = self.post("/api/v1/auth/login", {"email": creds[0], "password": creds[1]}, csrf=False)
        assert r.status == 200, (r.status, r.text)
        self.cookie = r.headers["set-cookie"].split(";")[0].split("=", 1)[1]
        self.csrf = r.json["csrf_token"]
        return r


def logged_in(server, creds):
    c = Client(server)
    c.login(creds)
    return c
