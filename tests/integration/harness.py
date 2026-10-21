"""Shared helpers: spawn stockforged on a free port and speak raw HTTP to it."""
import json
import os
import signal
import socket
import subprocess
import tempfile
import time

BIN = os.environ.get("SF_BIN", "build/rel/stockforged")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Server:
    def __init__(self, **env):
        self.port = free_port()
        e = dict(os.environ, SF_PORT=str(self.port), SF_BIND="127.0.0.1")
        e.update({k: str(v) for k, v in env.items()})
        # stderr goes to a file: a full pipe would block a chatty server, and a
        # readiness probe *connection* would consume a queue slot and skew
        # overload tests, so readiness = the server's own "started" log line.
        self.logf = tempfile.TemporaryFile("w+")
        self.proc = subprocess.Popen([BIN], env=e, stderr=self.logf, text=True)
        deadline = time.time() + 5
        while time.time() < deadline:
            if '"event":"started"' in self.log():
                return
            if self.proc.poll() is not None:
                break
            time.sleep(0.02)
        self.stop()
        raise RuntimeError("server did not start: " + self.log())

    def stop(self, sig=signal.SIGTERM, timeout=5):
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
        s = socket.create_connection(("127.0.0.1", self.port), timeout)
        return s


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
