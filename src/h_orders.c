/* Order endpoints: read APIs, checkout, pay, cancel. The transactional logic
 * lives in orders.c; these handlers only validate input and shape responses. */
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "orders.h"
#include "tx.h"
#include "validate.h"

#define TS(col) "DATE_FORMAT(" col ", '%Y-%m-%dT%H:%i:%sZ')"

/* Sends the engine's result, or a 503 if the transaction could not complete.
 * `idem_header` adds Idempotent-Replay for the checkout endpoint. */
static int send_out(rctx *c, int rc, order_out *out, int idem_header) {
    if (rc != TX_DONE) {
        order_out_free(out);
        if (rc == TX_RETRY)
            return api_err(c, 503, "DB_BUSY", "The system is busy; please retry with the same request.", NULL);
        return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    }
    const char *hdr = !idem_header ? NULL : out->replay ? "Idempotent-Replay: true\r\n" : "Idempotent-Replay: false\r\n";
    c->sent = resp_json(c->fd, out->http, c->rid, out->json, hdr);
    out->json = NULL;                       /* resp_json consumed it */
    return c->sent;
}

static int path_public_id(rctx *c, char *out, size_t n) {
    if (c->nseg < 4 || c->seglen[3] >= n) { api_err(c, 404, "NOT_FOUND", "No such order.", NULL); return -1; }
    memcpy(out, c->seg[3], c->seglen[3]);
    out[c->seglen[3]] = 0;
    if (!valid_public_id(out)) { api_err(c, 404, "NOT_FOUND", "No such order.", NULL); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ reads */

int h_orders(rctx *c) {
    int limit, r;
    char cur[24];
    uint64_t after = 0, dummy = 0;
    if (api_limit(c, 20, 50, &limit)) return c->sent;
    r = api_qs(c, "after", cur, sizeof cur);
    if (r < 0 || (r == 1 && cursor_parse(cur, &after, &dummy) != 1))
        return api_err(c, 422, "VALIDATION_FAILED", "Invalid cursor.", "after");

    db_rs *rs = db_query(c->db,
        "SELECT o.id, o.public_id, o.status, o.total_cents, o.currency, " TS("o.created_at") ", "
        "(SELECT COALESCE(SUM(oi.quantity), 0) FROM order_items oi WHERE oi.order_id = o.id) "
        "FROM orders o WHERE o.user_id = ? AND (? = 0 OR o.id < ?) ORDER BY o.id DESC LIMIT ?",
        "uuui", c->user.user_id, after, after, (int64_t)limit + 1);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);

    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "items");
    uint64_t id, total, items, last = 0;
    char pub[32], status[24], cur3[8], ts[32];
    int n = 0, more = 0;
    while (db_rs_next(rs, "ussussu", &id, pub, sizeof pub, status, sizeof status, &total, cur3,
                      sizeof cur3, ts, sizeof ts, &items) == 1) {
        if (n == limit) { more = 1; break; }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", pub);
        cJSON_AddStringToObject(o, "status", status);
        cJSON_AddNumberToObject(o, "total_cents", (double)total);
        cJSON_AddStringToObject(o, "currency", cur3);
        cJSON_AddStringToObject(o, "created_at", ts);
        cJSON_AddNumberToObject(o, "item_count", (double)items);
        cJSON_AddItemToArray(arr, o);
        last = id;
        n++;
    }
    db_rs_free(rs);
    if (more) {
        char nc[24];
        snprintf(nc, sizeof nc, "%llu", (unsigned long long)last);
        cJSON_AddStringToObject(root, "next_cursor", nc);
    } else cJSON_AddNullToObject(root, "next_cursor");
    return api_ok(c, 200, root);
}

int h_order(rctx *c) {
    char pub[32];
    if (path_public_id(c, pub, sizeof pub)) return c->sent;
    /* user_id is part of the WHERE clause: someone else's order is "not found",
     * never "forbidden", so order ids cannot be probed for existence. */
    db_rs *rs = db_query(c->db, "SELECT id FROM orders WHERE public_id = ? AND user_id = ?", "su", pub, c->user.user_id);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    uint64_t oid = 0;
    int f = db_rs_next(rs, "u", &oid);
    db_rs_free(rs);
    if (f < 0) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    if (f == 0) return api_err(c, 404, "NOT_FOUND", "No such order.", NULL);
    cJSON *oj = orders_json_by_id(c->db, oid);
    if (!oj) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "order", oj);
    return api_ok(c, 200, root);
}

/* --------------------------------------------------------------- checkout */

int h_checkout(rctx *c) {
    size_t kl;
    const char *k = http_header(c->req, "idempotency-key", &kl);
    if (!k) return api_err(c, 400, "IDEMPOTENCY_KEY_REQUIRED", "An Idempotency-Key header is required.", NULL);
    int valid = kl >= 8 && kl <= 64;
    for (size_t i = 0; valid && i < kl; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= 'A' && k[i] <= 'Z') || (k[i] >= '0' && k[i] <= '9') ||
              k[i] == '-' || k[i] == '_'))
            valid = 0;
    if (!valid)
        return api_err(c, 400, "IDEMPOTENCY_KEY_INVALID", "Idempotency-Key must be 8-64 characters of A-Z a-z 0-9 - _.", NULL);
    char key[72];
    memcpy(key, k, kl);
    key[kl] = 0;

    char rk[48];
    int retry = 0;
    snprintf(rk, sizeof rk, "checkout:%llu", (unsigned long long)c->user.user_id);
    if (!auth_rate(rk, c->cfg->rl_checkout_user, 60, &retry)) {
        char h[40];
        snprintf(h, sizeof h, "Retry-After: %d\r\n", retry);
        c->sent = resp_error_ex(c->fd, 429, c->rid, "RATE_LIMITED", "Too many checkout attempts. Try again shortly.", NULL, h);
        return c->sent;
    }

    int has_expected = 0;
    int64_t expected = 0;
    if (c->req->body_len > 0) {
        cJSON *b = api_body(c);
        if (!b) return c->sent;
        if (cJSON_GetObjectItemCaseSensitive(b, "expected_total_cents")) {
            if (api_int(c, b, "expected_total_cents", 0, 1000000000000LL, &expected)) { cJSON_Delete(b); return c->sent; }
            has_expected = 1;
        }
        cJSON_Delete(b);
    }
    /* Hash a canonical form, not raw bytes: whitespace or key order must not
     * make an honest retry look like a different request. */
    char canon[64];
    unsigned char hash[crypto_hash_sha256_BYTES];
    int cl = snprintf(canon, sizeof canon, "v1|%d|%lld", has_expected, (long long)expected);
    crypto_hash_sha256(hash, (const unsigned char *)canon, (unsigned long long)cl);

    order_out out = {0};
    int rc = orders_checkout(c->db, c->rid, c->user.user_id, key, hash, has_expected, expected,
                             c->cfg->reservation_ttl_s, &out);
    return send_out(c, rc, &out, 1);
}

/* ------------------------------------------------------------ pay / cancel */

int h_pay(rctx *c) {
    char pub[32];
    if (path_public_id(c, pub, sizeof pub)) return c->sent;
    pay_scenario sc = SCEN_SUCCESS;
    if (c->req->body_len > 0) {
        cJSON *b = api_body(c);
        if (!b) return c->sent;
        const char *s = NULL;
        if (api_str(c, b, "scenario", &s, 0)) { cJSON_Delete(b); return c->sent; }
        if (s && (sc = payment_sim_parse(s)) == SCEN_INVALID) {
            cJSON_Delete(b);
            return api_err(c, 422, "VALIDATION_FAILED", "scenario must be success, decline, timeout or flaky.", "scenario");
        }
        cJSON_Delete(b);
    }
    order_out out = {0};
    int rc = orders_pay(c->db, c->rid, c->user.user_id, pub, sc, &out);
    return send_out(c, rc, &out, 0);
}

int h_cancel(rctx *c) {
    char pub[32];
    if (path_public_id(c, pub, sizeof pub)) return c->sent;
    order_out out = {0};
    int rc = orders_cancel(c->db, c->rid, c->user.user_id, pub, &out);
    return send_out(c, rc, &out, 0);
}
