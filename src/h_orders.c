/* Order READ APIs (customers see only their own orders). Creation, payment and
 * cancellation arrive with the checkout engine in phase 5. */
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "validate.h"

#define TS(col) "DATE_FORMAT(" col ", '%Y-%m-%dT%H:%i:%sZ')"

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
    if (c->nseg < 4 || c->seglen[3] >= sizeof pub) return api_err(c, 404, "NOT_FOUND", "No such order.", NULL);
    memcpy(pub, c->seg[3], c->seglen[3]);
    pub[c->seglen[3]] = 0;
    if (!valid_public_id(pub)) return api_err(c, 404, "NOT_FOUND", "No such order.", NULL);

    /* user_id is part of the WHERE clause: someone else's order is "not found",
     * never "forbidden", so order ids cannot be probed for existence. */
    db_rs *rs = db_query(c->db,
        "SELECT o.id, o.status, o.total_cents, o.currency, " TS("o.created_at") ", "
        TS("o.expires_at") ", o.version FROM orders o WHERE o.public_id = ? AND o.user_id = ?",
        "su", pub, c->user.user_id);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    uint64_t oid, total, version;
    char status[24], cur3[8], created[32], expires[32];
    int f = db_rs_next(rs, "ususssu", &oid, status, sizeof status, &total, cur3, sizeof cur3,
                       created, sizeof created, expires, sizeof expires, &version);
    db_rs_free(rs);
    if (f < 0) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    if (f == 0) return api_err(c, 404, "NOT_FOUND", "No such order.", NULL);

    rs = db_query(c->db,
        "SELECT product_id, sku, product_name, unit_price_cents, quantity "
        "FROM order_items WHERE order_id = ? ORDER BY id", "u", oid);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    cJSON *root = cJSON_CreateObject(), *o = cJSON_AddObjectToObject(root, "order");
    cJSON_AddStringToObject(o, "id", pub);
    cJSON_AddStringToObject(o, "status", status);
    cJSON_AddNumberToObject(o, "total_cents", (double)total);
    cJSON_AddStringToObject(o, "currency", cur3);
    cJSON_AddStringToObject(o, "created_at", created);
    if (expires[0]) cJSON_AddStringToObject(o, "expires_at", expires);
    else cJSON_AddNullToObject(o, "expires_at");
    cJSON *arr = cJSON_AddArrayToObject(o, "items");
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
        cJSON_AddItemToArray(arr, it);
    }
    db_rs_free(rs);
    return api_ok(c, 200, root);
}
