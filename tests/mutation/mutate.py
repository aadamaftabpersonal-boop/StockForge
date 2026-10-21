#!/usr/bin/env python3
"""Mutation check for the checkout engine.

For each mutant: copy the tree to a temp dir, break ONE line of the engine,
rebuild, run the tests that are supposed to guard that line, and demand that
they FAIL ("killed"). A mutant that survives means the suite cannot see that
bug. The working tree is never modified.

Run inside the dev container with the database up:
    scripts/dev.sh python3 tests/mutation/mutate.py [name-substring]
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CC = "test_checkout_concurrency.ConcurrentCheckout"
CS = "test_checkout.CheckoutSemantics"
PC = "test_pay_cancel_expiry"

MUTANTS = [
    dict(name="checkout: stock rows read WITHOUT row lock", file="src/orders.c",
         old='"SELECT on_hand, reserved FROM inventory WHERE product_id = ? FOR UPDATE", "u", L[i].pid',
         new='"SELECT on_hand, reserved FROM inventory WHERE product_id = ?", "u", L[i].pid',
         tests=[f"{CC}.test_last_unit_300_buyers_exactly_one_wins", f"{CC}.test_ten_units_two_hundred_buyers_exactly_ten_win"]),
    dict(name="checkout: availability check skipped (DB CHECK is the only guard)", file="src/orders.c",
         old="if (avail < L[i].qty) {", new="if (0 && avail < L[i].qty) {",
         tests=[f"{CC}.test_last_unit_300_buyers_exactly_one_wins", f"{CS}.test_insufficient_stock_is_all_or_nothing_and_retryable"]),
    dict(name="checkout: per-user serialisation lock removed", file="src/orders.c",
         old='"SELECT id FROM users WHERE id = ? FOR UPDATE"', new='"SELECT id FROM users WHERE id = ?"',
         tests=[f"{CC}.test_one_cart_many_keys_becomes_one_order"]),
    dict(name="checkout: idempotency key not actually used", file="src/orders.c",
         old='"usB", a->uid, a->key, a->hash', new='"usB", a->uid, a->rid, a->hash',
         tests=[f"{CC}.test_sixty_identical_requests_create_one_order", f"{CS}.test_replay_returns_the_same_order_and_changes_nothing"]),
    dict(name="checkout: request-hash comparison removed", file="src/orders.c",
         old="if (memcmp(stored, a->hash, 32) != 0) {", new="if (0) {",
         tests=[f"{CS}.test_same_key_different_request_is_rejected"]),
    dict(name="checkout: failed checkout still keeps the idempotency key", file="src/orders.c",
         old="        db_rollback(d);                    /* releases locks AND un-claims the idempotency key */",
         new="        db_commit(d);",
         tests=[f"{CS}.test_insufficient_stock_is_all_or_nothing_and_retryable"]),
    dict(name="pay: order row not locked", file="src/orders.c",
         old='"public_id FROM orders WHERE public_id = ? AND user_id = ? FOR UPDATE"',
         new='"public_id FROM orders WHERE public_id = ? AND user_id = ?"',
         tests=[f"{PC}.PayAndCancel.test_thirty_concurrent_payments_charge_once"]),
    dict(name="pay: deadline not re-checked (overdue order can be paid)", file="src/orders.c",
         old="    if (o.expired) {                       /* lazy expiry", new="    if (0) {                       /* lazy expiry",
         tests=[f"{PC}.LazyExpiry.test_overdue_order_cannot_be_paid_even_if_unswept"]),
    dict(name="cancel: paid order released instead of restocked", file="src/orders.c",
         old="o.st == OS_PAID ? AM_RESTOCK : AM_RELEASE", new="AM_RELEASE",
         tests=[f"{PC}.PayAndCancel.test_cancel_after_payment_restocks"]),
    dict(name="cancel: repeated cancel releases again", file="src/orders.c",
         old="    if (o.st == OS_CANCELLED) {            /* repeated cancel", new="    if (0) {            /* repeated cancel",
         tests=[f"{PC}.PayAndCancel.test_repeated_and_concurrent_cancels_release_once"]),
    dict(name="state machine: EXPIRED -> PAID allowed", file="src/order_state.c",
         old="/* EXPIRED         */ {0, 0, 0, 0, 0, 0},", new="/* EXPIRED         */ {0, 1, 0, 0, 0, 0},",
         run="unit"),
    dict(name="reconcile: on-hand-vs-ledger check made vacuous", file="src/reconcile.c",
         old='ON l.product_id = i.product_id WHERE i.on_hand <> COALESCE(l.s, 0) LIMIT 20"',
         new='ON l.product_id = i.product_id WHERE 0 LIMIT 20"',
         tests=[f"{CS}.test_reconcile_detects_corruption"]),
    dict(name="INFORMATIONAL: cart read in random lock order (deadlocks expected, retries must absorb them)",
         file="src/orders.c", old="ORDER BY ci.product_id FOR SHARE OF p", new="ORDER BY RAND() FOR SHARE OF p",
         tests=[f"{CC}.test_cross_product_contention_has_no_failed_requests"], expect="survive"),
]


def run(cmd, cwd, env=None, timeout=900):
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    results = []
    for m in MUTANTS:
        if m.get("skip") or only not in m["name"]:
            continue
        tmp = tempfile.mkdtemp(prefix="mut-")
        try:
            for d in ("src", "vendor", "tools", "tests", "db"):
                shutil.copytree(os.path.join(ROOT, d), os.path.join(tmp, d), ignore=shutil.ignore_patterns("__pycache__"))
            shutil.copy(os.path.join(ROOT, "Makefile"), tmp)
            path = os.path.join(tmp, m["file"])
            src = open(path, encoding="utf-8").read()
            if src.count(m["old"]) != 1:
                results.append((m["name"], "BAD-MUTANT", f"pattern found {src.count(m['old'])}x"))
                continue
            open(path, "w", encoding="utf-8").write(src.replace(m["old"], m["new"]))
            b = run(["make", "-s", "-j4"], tmp)
            if b.returncode:
                results.append((m["name"], "BUILD-ERROR", b.stderr[-300:]))
                continue
            if m.get("run") == "unit":
                t = run(["make", "-s", "test"], tmp)
            else:
                env = dict(os.environ, SF_BIN=os.path.join(tmp, "build/rel/stockforged"))
                t = run(["python3", "-W", "ignore", "-m", "unittest"] + m["tests"], os.path.join(tmp, "tests/integration"), env)
            killed = t.returncode != 0
            want_killed = m.get("expect", "kill") == "kill"
            verdict = "KILLED" if killed else "SURVIVED"
            extra = ""
            if not want_killed:
                extra = " (expected)" if not killed else " (UNEXPECTED: failed)"
                for ln in (t.stderr + t.stdout).splitlines():
                    if "deadlock retries" in ln:
                        extra += " " + ln.strip()
            if os.environ.get("MUT_VERBOSE"):
                print("---- test output (tail) ----\n" + (t.stdout + t.stderr)[-1500:])
            ok = killed == want_killed
            results.append((m["name"], verdict + extra, "ok" if ok else "PROBLEM"))
            if not ok and want_killed:
                print((t.stdout + t.stderr)[-600:])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        print(f"  {results[-1][1]:<28} {results[-1][0]}", flush=True)

    bad = [r for r in results if r[2] not in ("ok",)]
    print(f"\n{len(results) - len(bad)}/{len(results)} mutants behaved as expected")
    for r in bad:
        print("  PROBLEM:", r)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
