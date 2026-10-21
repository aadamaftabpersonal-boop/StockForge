"""A fault-injecting TCP proxy for the MySQL protocol.

The server under test connects to this proxy instead of MySQL. Everything is
forwarded untouched until a fault is armed, so the real driver, real prepared
statements and real transactions are exercised, with no mocks.

Faults
  arm("nth", k, action)   when a connection has sent its k-th command since
                          START TRANSACTION, cut it. Sweeping k = 1, 2, 3 ...
                          visits EVERY point of a workflow.
  arm("sql", match=b"COMMIT", action)   cut when a COM_QUERY contains the text.
      action "before": the command never reaches MySQL         (it did NOT happen)
      action "after":  MySQL executes it, but the reply never reaches the
                       client                                  (it DID happen, the client cannot know)
  sever_all()             RST every open connection (a restart / network blip)
  down() / up()           refuse connections entirely / accept again
  blackhole = True        accept but stop relaying in both directions (a hung DB)
  latency = seconds       delay every client command

Only the command phase is parsed: client packets with sequence number 0 are
commands (handshake packets carry seq >= 1).
"""
import socket
import struct
import threading
import time

COM_QUERY = 0x03


def _rst(sock):
    """Really close a socket that other threads are blocked on.
    On Linux, close() alone does NOT wake a thread blocked in recv()/accept() on the
    same socket, and the kernel keeps the connection open until that call returns,
    so the peer would never notice. shutdown() first wakes them and sends FIN."""
    for step in (lambda: sock.shutdown(socket.SHUT_RDWR), sock.close):
        try:
            step()
        except OSError:
            pass


class _Rule:
    def __init__(self, kind, k, match, action):
        self.kind, self.k, self.match, self.action = kind, k, match, action
        self.fired = threading.Event()


class _Conn:
    def __init__(self, proxy, client, upstream):
        self.p, self.c, self.s = proxy, client, upstream
        self.in_txn, self.n = False, 0
        self.drop_next_reply = False
        self.dead = False
        threading.Thread(target=self._c2s, daemon=True).start()
        threading.Thread(target=self._s2c, daemon=True).start()

    def kill(self):
        self.dead = True
        _rst(self.c)
        _rst(self.s)

    def _hold(self):
        while self.p.blackhole and not self.dead:
            time.sleep(0.05)

    def _c2s(self):
        buf = b""
        try:
            while not self.dead:
                data = self.c.recv(65536)
                if not data:
                    break
                buf += data
                while len(buf) >= 4:
                    plen = int.from_bytes(buf[0:3], "little")
                    if len(buf) < 4 + plen:
                        break
                    pkt, buf = buf[:4 + plen], buf[4 + plen:]
                    if not self._handle(pkt):
                        return
        except OSError:
            pass
        if not self.dead:
            try:
                self.s.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    def _handle(self, pkt):
        """Returns False if the connection was cut."""
        seq, plen = pkt[3], int.from_bytes(pkt[0:3], "little")
        action = None
        if seq == 0 and plen > 0:
            cmd = pkt[4]
            sql = pkt[5:].upper() if cmd == COM_QUERY else b""
            if cmd == COM_QUERY and sql.startswith(b"START TRANSACTION"):
                self.in_txn, self.n = True, 0
            elif self.in_txn:
                self.n += 1
                self.p.max_txn_cmds = max(self.p.max_txn_cmds, self.n)
            rule = self.p.rule
            if rule and not rule.fired.is_set():
                hit = ((rule.kind == "nth" and self.in_txn and self.n == rule.k) or
                       (rule.kind == "sql" and cmd == COM_QUERY and sql.startswith(rule.match)))   # prefix: "COMMIT" must not match READ-COMMITTED
                if hit:
                    rule.fired.set()
                    self.p.rule = None             # one-shot, so the retry is not also faulted
                    action = rule.action
            if cmd == COM_QUERY and sql.startswith((b"COMMIT", b"ROLLBACK")):
                self.in_txn = False
        self._hold()
        if self.p.latency:
            time.sleep(self.p.latency)
        if action == "before":
            self.kill()
            return False
        try:
            self.s.sendall(pkt)
        except OSError:
            self.kill()
            return False
        if action == "after":
            self.drop_next_reply = True
        return True

    def _s2c(self):
        try:
            while not self.dead:
                data = self.s.recv(65536)
                if not data:
                    break
                self._hold()
                if self.drop_next_reply:           # MySQL answered; the client never hears it
                    self.kill()
                    return
                self.c.sendall(data)
        except OSError:
            pass
        if not self.dead:
            try:
                self.c.shutdown(socket.SHUT_WR)
            except OSError:
                pass


class FaultProxy:
    def __init__(self, upstream_host, upstream_port=3306):
        self.up = (upstream_host, upstream_port)
        self.rule = None
        self.latency = 0.0
        self.blackhole = False
        self.max_txn_cmds = 0
        self.conns = []
        self.port = None
        self._lsock = None
        self._closed = False
        self.up_()

    # ---- lifecycle
    def up_(self):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", self.port or 0))
        s.listen(256)
        self.port = s.getsockname()[1]
        self._lsock = s
        threading.Thread(target=self._accept, args=(s,), daemon=True).start()

    def down(self):
        """Refuse new connections and cut existing ones."""
        if self._lsock:
            _rst(self._lsock)
            self._lsock = None
        self.sever_all()

    def close(self):
        self._closed = True
        self.down()

    def _accept(self, lsock):
        while not self._closed:
            try:
                c, _ = lsock.accept()
            except OSError:
                return
            try:
                s = socket.create_connection(self.up, 5)
            except OSError:
                _rst(c)
                continue
            for sock in (c, s):             # MySQL's protocol is many tiny request/response pairs: without
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # NODELAY, Nagle + delayed ACK add ~40 ms to each
            self.conns.append(_Conn(self, c, s))

    # ---- faults
    def arm(self, kind, k=None, match=None, action="before"):
        self.rule = _Rule(kind, k, match, action)
        return self.rule.fired

    def clear(self):
        self.rule = None
        self.latency = 0.0
        self.blackhole = False

    def sever_all(self):
        for c in list(self.conns):
            c.kill()
        self.conns = []
