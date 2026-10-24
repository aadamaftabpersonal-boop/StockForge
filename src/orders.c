/* Order engine. See docs/CHECKOUT.md for the full design; the short version:
 *
 *  LOCK ORDER (the deadlock-avoidance rule, never violated):
 *      1. idempotency row (UNIQUE insert)       -- checkout only
 *      2. the user's row                         -- checkout only (serialises one user's checkouts)
 *      3. the order row                          -- pay / cancel / expire
 *      4. inventory rows in ASCENDING product_id
 *  Checkout creates a brand-new order, so it never waits on an existing order
 *  row; every path that touches several inventory rows takes them in the same
 *  ascending order. A wait-for cycle would need two transactions acquiring the
 *  same two locks in opposite orders, and no path does.
 *
 *  Anything MySQL still decides to kill (1213/1205) is re-run by tx_run().
 */
#include "orders.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audit.h"
#include "log.h"
#include "metrics.h"
#include "order_state.h"
#include "tx.h"

#define MAX_LINES 50
#define TS(col) "DATE_FORMAT(" col ", '%Y-%m-%dT%H:%i:%sZ')"

/* ---------------------------------------------------------------- helpers */

void order_out_free(order_out *o) {
    cJSON_Delete(o->json);
    o->json = NULL;
}

static void out_reset(order_out *o) {
    order_out_free(o);
    o->http = 0;
    o->replay = 0;
}

/* Builds {"error":{code,message,request_id}} and returns the inner object so
 * callers can attach extra fields (e.g. the list of short items). */
static cJSON *out_err(order_out *o, const char *rid, int http, const char *code, const char *msg) {
    out_reset(o);
    cJSON *root = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    cJSON_AddStringToObject(e, "request_id", rid);
    o->http = http;
    o->json = root;
    return e;
}

static void out_order(order_out *o, int http, cJSON *order) {
    out_reset(o);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "order", order);
    o->http = http;
    o->json = root;
}

void order_new_id(char out[27]) {
    static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    for (int i = 9; i >= 0; i--) { out[i] = B32[ms & 31]; ms >>= 5; }   /* sortable time prefix */
    unsigned char r[16];
    randombytes_buf(r, sizeof r);
    for (int i = 0; i < 16; i++) out[10 + i] = B32[r[i] & 31];           /* 80 random bits */
    out[26] = 0;
}

static void audit_order(db *d, const char *rid, uint64_t actor, const char *action, const char *pub,
                        cJSON *details) {
    audit_log(d, rid, actor, action, "order", pub, details);
    cJSON_Delete(details);
}

/* ------------------------------------------------------------- order JSON */

cJSON *orders_json_by_id(db *d, uint64_t oid) {
    db_rs *rs = db_query(d,
        "SELECT public_id, status, total_cents, currency, " TS("created_at") ", " TS("expires_at")
        " FROM orders WHERE id = ?", "u", oid);
    if (!rs) return NULL;
    char pub[32], status[24], cur[8], created[32], expires[32];
    uint64_t total;
    int f = db_rs_next(rs, "ssusss", pub, sizeof pub, status, sizeof status, &total, cur, sizeof cur,
                       created, sizeof created, expires, sizeof expires);
    db_rs_free(rs);
    if (f != 1) return NULL;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", pub);
    cJSON_AddStringToObject(o, "status", status);
    cJSON_AddNumberToObject(o, "total_cents", (double)total);
    cJSON_AddStringToObject(o, "currency", cur);
    cJSON_AddStringToObject(o, "created_at", created);
    if (expires[0]) cJSON_AddStringToObject(o, "expires_at", expires);
    else cJSON_AddNullToObject(o, "expires_at");

    rs = db_query(d, "SELECT product_id, sku, product_name, unit_price_cents, quantity "
                     "FROM order_items WHERE order_id = ? ORDER BY id", "u", oid);
    if (!rs) { cJSON_Delete(o); return NULL; }
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    uint64_t pid, price, qty;
    char sku[48], name[208];
    while (db_rs_next(rs, "ussuu", &pid, sku, sizeof sku, name, sizeof name, &price, &qty) == 1) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "product_id", (double)pid);
        cJSON_AddStringToObject(it, "sku", sku);
        cJSON_AddStringToObject(it, "name", name);
        cJSON_AddNumberToObject(it, "unit_price_cents", (double)price);
        cJSON_AddNumberToObject(it, "quantity", (double)qty);
        cJSON_AddNumberToObject(it, "line_total_cents", (double)(price * qty));
        cJSON_AddItemToArray(items, it);
    }
    db_rs_free(rs);

    rs = db_query(d, "SELECT attempt_no, status, scenario, amount_cents FROM payment_attempts "
                     "WHERE order_id = ? ORDER BY attempt_no", "u", oid);
    if (!rs) { cJSON_Delete(o); return NULL; }
    cJSON *pays = cJSON_AddArrayToObject(o, "payments");
    uint64_t no, amt;
    char pst[16], scen[16];
    while (db_rs_next(rs, "ussu", &no, pst, sizeof pst, scen, sizeof scen, &amt) == 1) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "attempt", (double)no);
        cJSON_AddStringToObject(p, "status", pst);
        cJSON_AddStringToObject(p, "scenario", scen);
        cJSON_AddNumberToObject(p, "amount_cents", (double)amt);
        cJSON_AddItemToArray(pays, p);
    }
    db_rs_free(rs);
    return o;
}

/* -------------------------------------------- reservation accounting core */

typedef struct { uint64_t pid, qty; } rline;
enum apply_mode { AM_COMMIT, AM_RELEASE, AM_RESTOCK };
enum { APPLY_OK = 0, APPLY_DBERR = -1, APPLY_INVARIANT = -2 };

/* Moves an order's reservations to their next state and updates inventory +
 * ledger in the same transaction, locking inventory rows in ascending
 * product_id order. APPLY_INVARIANT means the data contradicts the state
 * machine (e.g. releasing more than is reserved): the caller must roll back.
 *
 *   AM_COMMIT   held      -> committed  on_hand -= q, reserved -= q   (payment succeeded)
 *   AM_RELEASE  held      -> released   reserved -= q                 (cancel/expire unpaid)
 *   AM_RESTOCK  committed -> released   on_hand  += q                 (cancel after payment)  */
static int apply_reservations(db *d, uint64_t oid, enum apply_mode mode) {
    rline L[MAX_LINES];
    int n = 0;
    db_rs *rs = db_query(d, "SELECT product_id, quantity FROM stock_reservations "
                            "WHERE order_id = ? ORDER BY product_id", "u", oid);
    if (!rs) return APPLY_DBERR;
    while (n < MAX_LINES && db_rs_next(rs, "uu", &L[n].pid, &L[n].qty) == 1) n++;
    db_rs_free(rs);
    if (n == 0) return APPLY_INVARIANT;     /* every order has at least one reservation */

    for (int i = 0; i < n; i++) {
        uint64_t pid = L[i].pid, q = L[i].qty;
        int64_t rows;
        /* Each statement is checked BEFORE the next one runs: after a database error nothing
         * else may execute (see the `poisoned` guard in db.c for the second line of defence). */
#define STEP(call) do { rows = (call); if (rows < 0) return APPLY_DBERR; if (rows != 1) return APPLY_INVARIANT; } while (0)
        switch (mode) {
        case AM_COMMIT:
            STEP(db_exec(d, "UPDATE inventory SET on_hand = on_hand - ?, reserved = reserved - ? "
                            "WHERE product_id = ? AND on_hand >= ? AND reserved >= ?", "uuuuu", q, q, pid, q, q));
            STEP(db_exec(d, "UPDATE stock_reservations SET status = 'committed' "
                            "WHERE order_id = ? AND product_id = ? AND status = 'held'", "uu", oid, pid));
            STEP(db_exec(d, "INSERT INTO inventory_ledger (product_id, delta_on_hand, delta_reserved, reason, order_id) "
                            "VALUES (?, ?, ?, 'commit', ?)", "uiiu", pid, -(int64_t)q, -(int64_t)q, oid));
            break;
        case AM_RELEASE:
            STEP(db_exec(d, "UPDATE inventory SET reserved = reserved - ? "
                            "WHERE product_id = ? AND reserved >= ?", "uuu", q, pid, q));
            STEP(db_exec(d, "UPDATE stock_reservations SET status = 'released' "
                            "WHERE order_id = ? AND product_id = ? AND status = 'held'", "uu", oid, pid));
            STEP(db_exec(d, "INSERT INTO inventory_ledger (product_id, delta_on_hand, delta_reserved, reason, order_id) "
                            "VALUES (?, 0, ?, 'release', ?)", "uiu", pid, -(int64_t)q, oid));
            break;
        default: /* AM_RESTOCK */
            STEP(db_exec(d, "UPDATE inventory SET on_hand = on_hand + ? WHERE product_id = ?", "uu", q, pid));
            STEP(db_exec(d, "UPDATE stock_reservations SET status = 'released' "
                            "WHERE order_id = ? AND product_id = ? AND status = 'committed'", "uu", oid, pid));
            STEP(db_exec(d, "INSERT INTO inventory_ledger (product_id, delta_on_hand, delta_reserved, reason, order_id) "
                            "VALUES (?, ?, 0, 'restock', ?)", "uiu", pid, (int64_t)q, oid));
            break;
        }
#undef STEP
    }
    return APPLY_OK;
}

/* Compare-and-set status change guarded by the state machine and the row version. */
static int cas_status(db *d, uint64_t oid, order_status from, order_status to, uint64_t version) {
    if (!os_can_transition(from, to)) return -2;
    int64_t n = db_exec(d, "UPDATE orders SET status = ?, version = version + 1, expires_at = NULL "
                           "WHERE id = ? AND status = ? AND version = ?",
                        "susu", os_name(to), oid, os_name(from), version);
    if (n < 0) return -1;
    return n == 1 ? 0 : -2;
}

typedef struct {
    uint64_t     id, total, version;
    order_status st;
    int          expired;      /* past its reservation deadline */
    char         pub[28];
} olock;

/* SELECT ... FOR UPDATE on one order. 1 found, 0 absent, -1 db error.
 * With `pub`, uid restricts to that owner; uid == 0 means ANY owner (administrator actions). */
static int lock_order(db *d, const char *pub, uint64_t uid, uint64_t oid, olock *o) {
    db_rs *rs = pub
        ? db_query(d, "SELECT id, status, total_cents, (expires_at IS NOT NULL AND expires_at <= NOW(3)), version, "
                      "public_id FROM orders WHERE public_id = ? AND (? = 0 OR user_id = ?) FOR UPDATE", "suu", pub, uid, uid)
        : db_query(d, "SELECT id, status, total_cents, (expires_at IS NOT NULL AND expires_at <= NOW(3)), version, "
                      "public_id FROM orders WHERE id = ? FOR UPDATE", "u", oid);
    if (!rs) return -1;
    char st[24];
    uint64_t ex;
    int f = db_rs_next(rs, "usuuus", &o->id, st, sizeof st, &o->total, &ex, &o->version, o->pub, sizeof o->pub);
    db_rs_free(rs);
    if (f != 1) return f;
    o->st = os_parse(st);
    o->expired = ex != 0;
    return 1;
}

static int invariant_fail(db *d, order_out *out, const char *rid, const char *what) {
    db_rollback(d);
    log_msg("error", "invariant", what);
    out_err(out, rid, 500, "INVARIANT_VIOLATION", "Internal consistency check failed; nothing was changed.");
    return TX_DONE;
}

/* ================================================================ CHECKOUT */

typedef struct {
    uint64_t pid, qty, price, active;
    char     sku[48], name[208];
} cline;

typedef struct {
    const char          *rid, *key;
    uint64_t             uid;
    const unsigned char *hash;
    int                  has_expected, ttl_s;
    int64_t              expected;
    order_out           *out;
} co_arg;

/* A duplicate key: the original transaction has committed (our INSERT waited
 * for it), so its record is visible. Replay it, or reject a mismatched request. */
static int co_replay(db *d, co_arg *a) {
    unsigned char stored[32];
    uint64_t oid = 0, status = 0;
    static __thread char body[24576];
    db_rs *rs = db_query(d,
        "SELECT request_hash, COALESCE(order_id, 0), COALESCE(response_status, 0), "
        "COALESCE(CAST(response_body AS CHAR), '') FROM idempotency_keys WHERE user_id = ? AND idem_key = ?",
        "us", a->uid, a->key);
    if (!rs) return tx_classify(d);
    int f = db_rs_next(rs, "Buus", stored, sizeof stored, &oid, &status, body, sizeof body);
    db_rs_free(rs);
    if (f < 0) return tx_classify(d);
    if (f == 0) return TX_RETRY;                       /* record vanished: start over */
    if (memcmp(stored, a->hash, 32) != 0) {
        metrics_checkout_event(MC_IDEM_CONFLICT);
        out_err(a->out, a->rid, 422, "IDEMPOTENCY_KEY_REUSED",
                "This Idempotency-Key was already used with a different request.");
        return TX_DONE;
    }
    if (status == 0 || !oid) {
        out_err(a->out, a->rid, 409, "IDEMPOTENCY_IN_PROGRESS", "The original request is still being processed.");
        return TX_DONE;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) return TX_FAIL;
    out_reset(a->out);
    a->out->http = (int)status;
    a->out->json = j;
    a->out->replay = 1;
    metrics_checkout_event(MC_REPLAYED);
    return TX_DONE;
}

static int co_body(db *d, void *vp) {
    co_arg *a = vp;
    order_out *out = a->out;
    out_reset(out);
    cline L[MAX_LINES];
    int nl = 0;

    if (db_begin(d)) return TX_FAIL;

    /* 1. Claim the idempotency key. The UNIQUE index makes two concurrent
     *    requests with the same key serialise here, in the database, no matter
     *    how many server processes exist. */
    int64_t n = db_exec(d, "INSERT INTO idempotency_keys (user_id, idem_key, request_hash) VALUES (?, ?, ?)",
                        "usB", a->uid, a->key, a->hash, (size_t)32);
    if (n != 1) {
        if (db_is_dup(d)) { db_rollback(d); return co_replay(d, a); }
        return tx_classify(d);
    }
    uint64_t idem_id = db_insert_id(d);

    /* 2. One checkout per user at a time: two different keys racing on the
     *    same cart must not both turn it into an order. */
    db_rs *rs = db_query(d, "SELECT id FROM users WHERE id = ? FOR UPDATE", "u", a->uid);
    if (!rs) return tx_classify(d);
    uint64_t dummy;
    db_rs_next(rs, "u", &dummy);
    db_rs_free(rs);

    /* 3. Read the cart; FOR SHARE keeps price/is_active stable until we commit. */
    rs = db_query(d,
        "SELECT ci.product_id, ci.quantity, p.price_cents, p.is_active, p.sku, p.name "
        "FROM cart_items ci JOIN products p ON p.id = ci.product_id "
        "WHERE ci.user_id = ? ORDER BY ci.product_id FOR SHARE OF p", "u", a->uid);
    if (!rs) return tx_classify(d);
    while (nl < MAX_LINES && db_rs_next(rs, "uuuuss", &L[nl].pid, &L[nl].qty, &L[nl].price, &L[nl].active,
                                         L[nl].sku, sizeof L[nl].sku, L[nl].name, sizeof L[nl].name) == 1)
        nl++;
    db_rs_free(rs);
    if (nl == 0) {
        db_rollback(d);
        out_err(out, a->rid, 409, "EMPTY_CART", "Your cart is empty.");
        return TX_DONE;
    }
    for (int i = 0; i < nl; i++) {
        if (!L[i].active) {
            db_rollback(d);
            cJSON *e = out_err(out, a->rid, 409, "PRODUCT_UNAVAILABLE", "A product in your cart is no longer available.");
            cJSON_AddStringToObject(e, "sku", L[i].sku);
            return TX_DONE;
        }
    }

    /* 4. Lock inventory rows in ascending product_id order and verify stock. */
    cJSON *shorts = cJSON_CreateArray();
    uint64_t total = 0;
    for (int i = 0; i < nl; i++) {
        rs = db_query(d, "SELECT on_hand, reserved FROM inventory WHERE product_id = ? FOR UPDATE", "u", L[i].pid);
        if (!rs) { cJSON_Delete(shorts); return tx_classify(d); }
        uint64_t on_hand = 0, reserved = 0;
        int f = db_rs_next(rs, "uu", &on_hand, &reserved);
        db_rs_free(rs);
        if (f < 0) { cJSON_Delete(shorts); return tx_classify(d); }
        uint64_t avail = (f == 1 && on_hand > reserved) ? on_hand - reserved : 0;
        if (avail < L[i].qty) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddStringToObject(s, "sku", L[i].sku);
            cJSON_AddNumberToObject(s, "requested", (double)L[i].qty);
            cJSON_AddNumberToObject(s, "available", (double)avail);
            cJSON_AddItemToArray(shorts, s);
        }
        total += L[i].price * L[i].qty;
    }
    if (cJSON_GetArraySize(shorts) > 0) {
        db_rollback(d);                    /* releases locks AND un-claims the idempotency key */
        cJSON *e = out_err(out, a->rid, 409, "INSUFFICIENT_STOCK", "Not enough stock for one or more items.");
        cJSON_AddItemToObject(e, "items", shorts);
        metrics_checkout_event(MC_INSUFFICIENT);
        return TX_DONE;
    }
    cJSON_Delete(shorts);

    if (a->has_expected && (uint64_t)a->expected != total) {
        db_rollback(d);
        cJSON *e = out_err(out, a->rid, 409, "PRICE_CHANGED", "The cart total changed. Review it and try again.");
        cJSON_AddNumberToObject(e, "current_total_cents", (double)total);
        return TX_DONE;
    }

    /* 5. Reserve, create the order, snapshot prices, write ledger + audit. */
    char pub[27];
    order_new_id(pub);
    if (db_exec(d, "INSERT INTO orders (public_id, user_id, status, total_cents, currency, expires_at) "
                   "VALUES (?, ?, 'PENDING_PAYMENT', ?, 'USD', NOW(3) + INTERVAL ? SECOND)",
                "suui", pub, a->uid, total, (int64_t)a->ttl_s) != 1)
        return tx_classify(d);
    uint64_t oid = db_insert_id(d);

    for (int i = 0; i < nl; i++) {
        if (db_exec(d, "UPDATE inventory SET reserved = reserved + ? WHERE product_id = ?", "uu", L[i].qty, L[i].pid) != 1 ||
            db_exec(d, "INSERT INTO order_items (order_id, product_id, sku, product_name, unit_price_cents, quantity) "
                       "VALUES (?, ?, ?, ?, ?, ?)", "uussuu", oid, L[i].pid, L[i].sku, L[i].name, L[i].price, L[i].qty) != 1 ||
            db_exec(d, "INSERT INTO stock_reservations (order_id, product_id, quantity, status) VALUES (?, ?, ?, 'held')",
                    "uuu", oid, L[i].pid, L[i].qty) != 1 ||
            db_exec(d, "INSERT INTO inventory_ledger (product_id, delta_on_hand, delta_reserved, reason, order_id) "
                       "VALUES (?, 0, ?, 'reserve', ?)", "uiu", L[i].pid, (int64_t)L[i].qty, oid) != 1)
            return tx_classify(d);
    }
    if (db_exec(d, "DELETE FROM cart_items WHERE user_id = ?", "u", a->uid) < 0) return tx_classify(d);

    cJSON *det = cJSON_CreateObject();
    cJSON_AddNumberToObject(det, "total_cents", (double)total);
    cJSON_AddNumberToObject(det, "lines", nl);
    audit_order(d, a->rid, a->uid, "order.created", pub, det);

    /* 6. Store the response in the idempotency record, in the SAME transaction,
     *    so "key exists" and "order exists" become visible atomically. */
    cJSON *oj = orders_json_by_id(d, oid);
    if (!oj) return tx_classify(d);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "order", oj);
    char *bs = cJSON_PrintUnformatted(body);
    if (!bs) { cJSON_Delete(body); db_rollback(d); return TX_FAIL; }
    int64_t u = db_exec(d, "UPDATE idempotency_keys SET order_id = ?, response_status = 201, response_body = ? "
                           "WHERE id = ?", "usu", oid, bs, idem_id);
    cJSON_free(bs);
    if (u != 1) { cJSON_Delete(body); return tx_classify(d); }

    if (db_commit(d)) { cJSON_Delete(body); return TX_FAIL; }    /* outcome unknown: client retries with the same key */
    out->http = 201;
    out->json = body;
    metrics_checkout_event(MC_CREATED);
    return TX_DONE;
}

int orders_checkout(db *d, const char *rid, uint64_t uid, const char *key, const unsigned char hash[32],
                    int has_expected, int64_t expected, int ttl_s, order_out *out) {
    co_arg a = {.rid = rid, .key = key, .uid = uid, .hash = hash, .has_expected = has_expected,
                .expected = expected, .ttl_s = ttl_s, .out = out};
    return tx_run(d, co_body, &a);
}

/* ===================================================================== PAY */

typedef struct {
    const char  *rid, *pub;
    uint64_t     uid;
    pay_scenario sc;
    order_out   *out;
} pay_arg;

static int pay_body(db *d, void *vp) {
    pay_arg *a = vp;
    order_out *out = a->out;
    out_reset(out);
    if (db_begin(d)) return TX_FAIL;

    olock o;
    int f = lock_order(d, a->pub, a->uid, 0, &o);
    if (f < 0) return tx_classify(d);
    if (f == 0) { db_rollback(d); out_err(out, a->rid, 404, "NOT_FOUND", "No such order."); return TX_DONE; }

    if (o.st == OS_PAID || o.st == OS_SHIPPED || o.st == OS_DELIVERED) {   /* idempotent: already paid */
        cJSON *oj = orders_json_by_id(d, o.id);
        db_rollback(d);
        if (!oj) return TX_FAIL;
        out_order(out, 200, oj);
        out->replay = 1;
        return TX_DONE;
    }
    if (o.st != OS_PENDING_PAYMENT) {
        db_rollback(d);
        out_err(out, a->rid, 409, "ILLEGAL_TRANSITION", "This order can no longer be paid.");
        return TX_DONE;
    }
    if (o.expired) {                       /* lazy expiry: do not take money for stock we no longer hold */
        int r = apply_reservations(d, o.id, AM_RELEASE);
        if (r == APPLY_DBERR) return tx_classify(d);
        if (r == APPLY_INVARIANT) return invariant_fail(d, out, a->rid, "expire-on-pay");
        int c = cas_status(d, o.id, OS_PENDING_PAYMENT, OS_EXPIRED, o.version);
        if (c == -1) return tx_classify(d);
        if (c != 0) return invariant_fail(d, out, a->rid, "expire-on-pay cas");
        audit_order(d, a->rid, 0, "order.expired", a->pub, NULL);
        if (db_commit(d)) return TX_FAIL;
        metrics_checkout_event(MC_EXPIRED);
        out_err(out, a->rid, 409, "ORDER_EXPIRED", "The reservation expired; the items were released.");
        return TX_DONE;
    }

    /* The order row is locked, so attempt numbers cannot collide. */
    db_rs *rs = db_query(d, "SELECT COALESCE(MAX(attempt_no), 0) + 1 FROM payment_attempts WHERE order_id = ?", "u", o.id);
    if (!rs) return tx_classify(d);
    uint64_t attempt = 1;
    db_rs_next(rs, "u", &attempt);
    db_rs_free(rs);

    pay_result pr = payment_sim_charge(a->sc, (unsigned)attempt);
    char ref[27];
    order_new_id(ref);
    if (db_exec(d, "INSERT INTO payment_attempts (order_id, attempt_no, scenario, status, amount_cents, provider_ref) "
                   "VALUES (?, ?, ?, ?, ?, ?)", "uussus", o.id, attempt, payment_sim_name(a->sc),
                payment_result_sql(pr), o.total, ref) != 1)
        return tx_classify(d);

    cJSON *det = cJSON_CreateObject();
    cJSON_AddNumberToObject(det, "attempt", (double)attempt);
    cJSON_AddStringToObject(det, "scenario", payment_sim_name(a->sc));

    if (pr != PAY_SUCCEEDED) {             /* the failed attempt is recorded; the reservation stays held */
        cJSON_AddStringToObject(det, "result", payment_result_sql(pr));
        audit_order(d, a->rid, a->uid, "order.payment_failed", a->pub, det);
        if (db_commit(d)) return TX_FAIL;
        metrics_checkout_event(MC_PAY_FAILED);
        if (pr == PAY_DECLINED) out_err(out, a->rid, 402, "PAYMENT_DECLINED", "The payment was declined. You can try again.");
        else out_err(out, a->rid, 504, "PAYMENT_TIMEOUT", "The payment provider timed out. You can try again.");
        return TX_DONE;
    }

    int r = apply_reservations(d, o.id, AM_COMMIT);
    if (r == APPLY_DBERR) { cJSON_Delete(det); return tx_classify(d); }
    if (r == APPLY_INVARIANT) { cJSON_Delete(det); return invariant_fail(d, out, a->rid, "pay"); }
    int c = cas_status(d, o.id, OS_PENDING_PAYMENT, OS_PAID, o.version);
    if (c == -1) { cJSON_Delete(det); return tx_classify(d); }
    if (c != 0) { cJSON_Delete(det); return invariant_fail(d, out, a->rid, "pay cas"); }
    audit_order(d, a->rid, a->uid, "order.paid", a->pub, det);
    cJSON *oj = orders_json_by_id(d, o.id);
    if (!oj) return tx_classify(d);
    if (db_commit(d)) { cJSON_Delete(oj); return TX_FAIL; }
    metrics_checkout_event(MC_PAID);
    out_order(out, 200, oj);
    return TX_DONE;
}

int orders_pay(db *d, const char *rid, uint64_t uid, const char *pub, pay_scenario sc, order_out *out) {
    pay_arg a = {.rid = rid, .pub = pub, .uid = uid, .sc = sc, .out = out};
    return tx_run(d, pay_body, &a);
}

/* ================================================================== CANCEL */

typedef struct {
    const char *rid, *pub;
    uint64_t    uid;          /* owner filter; 0 = any owner (administrator) */
    uint64_t    actor;        /* who is recorded in the audit trail */
    order_out  *out;
} cancel_arg;

static int cancel_body(db *d, void *vp) {
    cancel_arg *a = vp;
    order_out *out = a->out;
    out_reset(out);
    if (db_begin(d)) return TX_FAIL;

    olock o;
    int f = lock_order(d, a->pub, a->uid, 0, &o);
    if (f < 0) return tx_classify(d);
    if (f == 0) { db_rollback(d); out_err(out, a->rid, 404, "NOT_FOUND", "No such order."); return TX_DONE; }

    if (o.st == OS_CANCELLED) {            /* repeated cancel: same answer, no second release */
        cJSON *oj = orders_json_by_id(d, o.id);
        db_rollback(d);
        if (!oj) return TX_FAIL;
        out_order(out, 200, oj);
        out->replay = 1;
        return TX_DONE;
    }
    if (!os_can_transition(o.st, OS_CANCELLED)) {
        db_rollback(d);
        out_err(out, a->rid, 409, "ILLEGAL_TRANSITION", "This order can no longer be cancelled.");
        return TX_DONE;
    }
    int r = apply_reservations(d, o.id, o.st == OS_PAID ? AM_RESTOCK : AM_RELEASE);
    if (r == APPLY_DBERR) return tx_classify(d);
    if (r == APPLY_INVARIANT) return invariant_fail(d, out, a->rid, "cancel");
    int c = cas_status(d, o.id, o.st, OS_CANCELLED, o.version);
    if (c == -1) return tx_classify(d);
    if (c != 0) return invariant_fail(d, out, a->rid, "cancel cas");
    cJSON *det = cJSON_CreateObject();
    cJSON_AddStringToObject(det, "from", os_name(o.st));
    audit_order(d, a->rid, a->actor, "order.cancelled", a->pub, det);
    cJSON *oj = orders_json_by_id(d, o.id);
    if (!oj) return tx_classify(d);
    if (db_commit(d)) { cJSON_Delete(oj); return TX_FAIL; }
    metrics_checkout_event(MC_CANCELLED);
    out_order(out, 200, oj);
    return TX_DONE;
}

int orders_cancel(db *d, const char *rid, uint64_t uid, const char *pub, order_out *out) {
    cancel_arg a = {.rid = rid, .pub = pub, .uid = uid, .actor = uid, .out = out};
    return tx_run(d, cancel_body, &a);
}

int orders_admin_cancel(db *d, const char *rid, uint64_t admin_id, const char *pub, order_out *out) {
    cancel_arg a = {.rid = rid, .pub = pub, .uid = 0, .actor = admin_id, .out = out};
    return tx_run(d, cancel_body, &a);
}

/* ====================================================== ADMIN FULFILMENT */

typedef struct {
    const char  *rid, *pub;
    uint64_t     admin;
    order_status to;
    order_out   *out;
} ship_arg;

/* PAID -> SHIPPED -> DELIVERED, for any owner. Repeating the transition that already happened is a no-op 200. */
static int ship_body(db *d, void *vp) {
    ship_arg *a = vp;
    order_out *out = a->out;
    out_reset(out);
    if (db_begin(d)) return TX_FAIL;
    olock o;
    int f = lock_order(d, a->pub, 0, 0, &o);
    if (f < 0) return tx_classify(d);
    if (f == 0) { db_rollback(d); out_err(out, a->rid, 404, "NOT_FOUND", "No such order."); return TX_DONE; }
    if (o.st == a->to) {                                   /* idempotent repeat */
        cJSON *oj = orders_json_by_id(d, o.id);
        db_rollback(d);
        if (!oj) return TX_FAIL;
        out_order(out, 200, oj);
        out->replay = 1;
        return TX_DONE;
    }
    if (!os_can_transition(o.st, a->to)) {
        db_rollback(d);
        out_err(out, a->rid, 409, "ILLEGAL_TRANSITION", "That status change is not allowed from the order's current status.");
        return TX_DONE;
    }
    int c = cas_status(d, o.id, o.st, a->to, o.version);
    if (c == -1) return tx_classify(d);
    if (c != 0) return invariant_fail(d, out, a->rid, "ship cas");
    cJSON *det = cJSON_CreateObject();
    cJSON_AddStringToObject(det, "from", os_name(o.st));
    cJSON_AddStringToObject(det, "to", os_name(a->to));
    audit_order(d, a->rid, a->admin, a->to == OS_SHIPPED ? "order.shipped" : "order.delivered", a->pub, det);
    cJSON *oj = orders_json_by_id(d, o.id);
    if (!oj) return tx_classify(d);
    if (db_commit(d)) { cJSON_Delete(oj); return TX_FAIL; }
    out_order(out, 200, oj);
    return TX_DONE;
}

int orders_admin_fulfil(db *d, const char *rid, uint64_t admin_id, const char *pub, order_status to, order_out *out) {
    ship_arg a = {.rid = rid, .pub = pub, .admin = admin_id, .to = to, .out = out};
    return tx_run(d, ship_body, &a);
}

/* ================================================================== EXPIRY */

typedef struct {
    const char *rid;
    uint64_t    oid;
    int         expired;
} expire_arg;

static int expire_body(db *d, void *vp) {
    expire_arg *a = vp;
    a->expired = 0;
    if (db_begin(d)) return TX_FAIL;
    olock o;
    int f = lock_order(d, NULL, 0, a->oid, &o);
    if (f < 0) return tx_classify(d);
    /* Re-check under the lock: the customer may have paid or cancelled since the sweep query ran. */
    if (f == 0 || o.st != OS_PENDING_PAYMENT || !o.expired) { db_rollback(d); return TX_DONE; }
    int r = apply_reservations(d, o.id, AM_RELEASE);
    if (r == APPLY_DBERR) return tx_classify(d);
    if (r == APPLY_INVARIANT) { db_rollback(d); return TX_FAIL; }
    int c = cas_status(d, o.id, OS_PENDING_PAYMENT, OS_EXPIRED, o.version);
    if (c == -1) return tx_classify(d);
    if (c != 0) { db_rollback(d); return TX_FAIL; }
    audit_order(d, a->rid, 0, "order.expired", o.pub, NULL);
    if (db_commit(d)) return TX_FAIL;
    a->expired = 1;
    metrics_checkout_event(MC_EXPIRED);
    return TX_DONE;
}

int orders_expire_due(db *d, const char *rid, int max) {
    uint64_t ids[64];
    int n = 0;
    if (max > 64) max = 64;
    db_rs *rs = db_query(d, "SELECT id FROM orders WHERE status = 'PENDING_PAYMENT' AND expires_at <= NOW(3) "
                            "ORDER BY expires_at, id LIMIT ?", "i", (int64_t)max);
    if (!rs) return -1;
    while (n < max && db_rs_next(rs, "u", &ids[n]) == 1) n++;
    db_rs_free(rs);
    int done = 0;
    for (int i = 0; i < n; i++) {
        expire_arg a = {.rid = rid, .oid = ids[i]};
        if (tx_run(d, expire_body, &a) != TX_DONE) return done ? done : -1;
        done += a.expired;
    }
    return done;
}
