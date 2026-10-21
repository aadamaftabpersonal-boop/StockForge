"""Failure injection: the server is killed mid-workflow, and random operation
mixes are thrown at it. After each, the DATABASE (not the HTTP layer) is
checked for invariant violations.

What this does and does not prove: MySQL/InnoDB owns crash recovery and
durability. These tests check the APPLICATION's transaction boundaries (no
half-finished checkout is ever visible) and the idempotent-retry protocol."""
import os
import random
import threading
import time
import unittest
import uuid
from collections import Counter

import dbtools
from harness import ADMIN, Server, checkout, logged_in, run_parallel, session_client

SCALE = float(os.environ.get("STRESS_SCALE", "1"))


def pay(c, oid, scenario="success"):
    return c.post(f"/api/v1/orders/{oid}/pay", {"scenario": scenario})


def cancel(c, oid):
    return c.post(f"/api/v1/orders/{oid}/cancel")


class CrashRecovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()

    def load_then_interrupt(self, interrupt, delay):
        """Fire N checkouts at a server, interrupt it after `delay` s, return (keys, clients, product)."""
        count = max(20, int(150 * SCALE))
        p = dbtools.new_product(100000, 1000)
        users = dbtools.make_users(count)
        for u in users:
            dbtools.put_cart(u[0], [(p, 1)])
        keys = ["crash-" + uuid.uuid4().hex for _ in users]
        srv = Server(SF_WORKERS=16, SF_QUEUE_CAP=2048)
        clients = [session_client(srv, u) for u in users]
        threading.Timer(delay, lambda: interrupt(srv)).start()
        res = run_parallel(count, lambda i: checkout(clients[i], keys[i]))
        srv.proc.wait(30)
        return srv, users, keys, p, res

    def assert_atomic_after_interruption(self, users, p):
        self.assertEqual(dbtools.invariant_violations(), {})                    # no half-applied checkout
        self.assertEqual(dbtools.scalar("SELECT COUNT(*) FROM idempotency_keys WHERE order_id IS NULL OR response_status IS NULL"), "0")
        orders = int(dbtools.scalar(f"SELECT COUNT(*) FROM order_items WHERE product_id={p}"))
        on_hand, reserved = dbtools.stock(p)
        self.assertEqual((on_hand, reserved), (100000, orders))                 # reserved == committed orders, exactly
        return orders

    def retry_everything_on_fresh_server(self, users, keys, p, expect_committed):
        srv = Server(SF_WORKERS=16, SF_QUEUE_CAP=2048)
        try:
            clients = [session_client(srv, u) for u in users]
            res = run_parallel(len(users), lambda i: checkout(clients[i], keys[i]))
            bad = [r for r in res if isinstance(r, Exception) or r.status != 201]
            self.assertEqual(bad, [], bad[:3])
            replays = sum(r.headers["idempotent-replay"] == "true" for r in res)
            self.assertEqual(replays, expect_committed)                          # those committed before the crash replay
            self.assertEqual(dbtools.stock(p), (100000, len(users)))             # everyone has exactly one order
            self.assertEqual(dbtools.scalar(f"SELECT COUNT(DISTINCT user_id) FROM orders WHERE user_id IN "
                                            f"({','.join(str(u[0]) for u in users)})"), str(len(users)))
            self.assertEqual(dbtools.invariant_violations(), {})
            self.assertTrue(logged_in(srv, ADMIN).get("/api/v1/admin/reconcile").json["ok"])
        finally:
            self.assertEqual(srv.stop(), 0)

    def test_sigkill_mid_checkout_leaves_no_partial_state(self):
        outcomes = []
        for delay in (0.05, 0.12, 0.25):                                       # interrupt at different workflow points
            srv, users, keys, p, res = self.load_then_interrupt(lambda s: s.proc.kill(), delay)
            self.assertEqual(srv.proc.returncode, -9)
            committed = self.assert_atomic_after_interruption(users, p)
            outcomes.append(committed)
            self.retry_everything_on_fresh_server(users, keys, p, committed)
        print(f"\n    [info] orders already committed when SIGKILL landed (of {len(users)}): {outcomes}")

    def test_sigterm_under_load_drains_cleanly(self):
        import signal
        srv, users, keys, p, res = self.load_then_interrupt(lambda s: s.proc.send_signal(signal.SIGTERM), 0.12)
        self.assertEqual(srv.proc.returncode, 0)                               # graceful: exit 0
        self.assertIn("clean shutdown", srv.log())
        committed = self.assert_atomic_after_interruption(users, p)
        done = sum(1 for r in res if not isinstance(r, Exception) and r.status == 201)
        self.assertGreaterEqual(committed, done)                               # nothing acknowledged was lost
        self.retry_everything_on_fresh_server(users, keys, p, committed)
        print(f"\n    [info] SIGTERM: {done} acknowledged, {committed} committed of {len(users)}")


class Chaos(unittest.TestCase):
    """Random mixes of checkout / duplicate checkout / pay / cancel / restock /
    forced expiry from many users at once, against scarce stock."""

    @classmethod
    def setUpClass(cls):
        dbtools.reset_db()
        cls.srv = Server(SF_WORKERS=16, SF_QUEUE_CAP=2048, SF_EXPIRY_SWEEP_MS=150)
        cls.admin = logged_in(cls.srv, ADMIN)

    @classmethod
    def tearDownClass(cls):
        assert cls.srv.stop() == 0

    def test_random_operations_preserve_every_invariant(self):
        seed = int(os.environ.get("FUZZ_SEED", random.randrange(1 << 30)))
        print(f"\n    [info] chaos seed = {seed} (rerun with FUZZ_SEED={seed})")
        pids = [dbtools.new_product(random.Random(seed + i).randint(3, 12), 500 + 50 * i) for i in range(6)]
        users = dbtools.make_users(max(6, int(24 * SCALE)))
        clients = [session_client(self.srv, u) for u in users]
        ops = max(15, int(40 * SCALE))
        seen = {"checkout": Counter(), "pay": Counter(), "cancel": Counter(), "admin": Counter(), "cart": Counter()}
        lock = threading.Lock()

        def note(kind, status):
            with lock:
                seen[kind][status] += 1

        stop_expirer = threading.Event()
        expirer_errors, expirer_runs = [], [0]

        def expirer():                                  # forces overdue deadlines under the workload's feet
            rnd = random.Random(seed)
            try:
                while not stop_expirer.is_set():
                    dbtools.q("UPDATE orders SET expires_at = NOW(3) - INTERVAL 1 SECOND WHERE status='PENDING_PAYMENT' "
                              f"AND id % 7 = {rnd.randint(0, 6)}")
                    expirer_runs[0] += 1
                    time.sleep(0.4)
            except Exception as e:                      # noqa: BLE001 - a dead helper must fail the test, not hide
                expirer_errors.append(e)

        def user_loop(i):
            rnd = random.Random(seed * 31 + i)
            c, mine, keys = clients[i], [], []
            for _ in range(ops):
                roll = rnd.random()
                if roll < 0.30 or not mine:
                    for p in rnd.sample(pids, rnd.randint(1, 3)):
                        note("cart", c.put(f"/api/v1/cart/items/{p}", {"quantity": rnd.randint(1, 3)}).status)
                    k = "chaos-" + uuid.uuid4().hex
                    r = checkout(c, k)
                    note("checkout", r.status)
                    if r.status == 201:
                        mine.append(r.json["order"]["id"])
                        keys.append(k)
                elif roll < 0.38 and keys:
                    note("checkout", checkout(c, rnd.choice(keys)).status)             # duplicate delivery
                elif roll < 0.80:
                    note("pay", pay(c, rnd.choice(mine), rnd.choice(["success"] * 5 + ["decline", "flaky", "timeout"])).status)
                elif roll < 0.90:
                    note("cancel", cancel(c, rnd.choice(mine)).status)
                else:
                    note("admin", self.admin.post(f"/api/v1/admin/inventory/{rnd.choice(pids)}/adjust",
                                                  {"delta": rnd.choice([3, 5, -1, -2]), "reason": "adjust"}).status)

        t = threading.Thread(target=expirer)
        t.start()
        try:
            res = run_parallel(len(users), user_loop)
        finally:
            stop_expirer.set()
            t.join()
        self.assertEqual([r for r in res if isinstance(r, Exception)], [])
        self.assertEqual(expirer_errors, [])
        self.assertGreater(expirer_runs[0], 2)                                    # forced expiry really ran during the load
        time.sleep(1.0)                                                           # let the sweeper settle

        allowed = {"cart": {200}, "checkout": {201, 409}, "pay": {200, 402, 504, 409}, "cancel": {200, 409},
                   "admin": {200, 409}}
        for kind, counter in seen.items():
            self.assertTrue(set(counter) <= allowed[kind], (kind, counter))        # in particular: no 5xx, ever
        print(f"    [info] {sum(sum(c.values()) for c in seen.values())} requests: " +
              ", ".join(f"{k}={dict(v)}" for k, v in seen.items()))

        self.assertEqual(dbtools.invariant_violations(), {})
        self.assertTrue(self.admin.get("/api/v1/admin/reconcile").json["ok"])
        for p in pids:
            on_hand, reserved = dbtools.stock(p)
            self.assertGreaterEqual(on_hand, reserved)
        states = Counter(r[0] for r in dbtools.q("SELECT status FROM orders"))
        print(f"    [info] final order states: {dict(states)}")
        self.assertGreater(len(states), 2)                                         # the run really exercised several paths


if __name__ == "__main__":
    unittest.main()
