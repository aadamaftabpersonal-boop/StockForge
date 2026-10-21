/* Server-side cart. Advisory only: stock is NOT reserved here. The checkout
 * transaction (phase 5) is the only place stock is claimed. */
#include <string.h>

#include "api.h"

#define MAX_CART_LINES 50

int h_cart_get(rctx *c) {
    db_rs *rs = db_query(c->db,
        "SELECT ci.product_id, p.sku, p.name, p.price_cents, p.currency, ci.quantity, "
        "CAST(i.on_hand AS SIGNED) - CAST(i.reserved AS SIGNED), p.is_active "
        "FROM cart_items ci JOIN products p ON p.id = ci.product_id "
        "JOIN inventory i ON i.product_id = p.id "
        "WHERE ci.user_id = ? ORDER BY ci.updated_at, ci.product_id LIMIT 50", "u", c->user.user_id);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "items");
    uint64_t pid, price, qty, active, subtotal = 0;
    int64_t avail;
    char sku[48], name[208], cur[8] = "USD";
    int ok_all = 1;
    while (db_rs_next(rs, "ussusuiu", &pid, sku, sizeof sku, name, sizeof name, &price, cur, sizeof cur,
                      &qty, &avail, &active) == 1) {
        if (avail < 0) avail = 0;
        int ok = active && qty <= (uint64_t)avail;
        if (!ok) ok_all = 0;
        subtotal += price * qty;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "product_id", (double)pid);
        cJSON_AddStringToObject(o, "sku", sku);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "unit_price_cents", (double)price);
        cJSON_AddNumberToObject(o, "quantity", (double)qty);
        cJSON_AddNumberToObject(o, "line_total_cents", (double)(price * qty));
        cJSON_AddNumberToObject(o, "available", (double)avail);
        cJSON_AddBoolToObject(o, "purchasable", ok);
        cJSON_AddItemToArray(arr, o);
    }
    db_rs_free(rs);
    cJSON_AddNumberToObject(root, "subtotal_cents", (double)subtotal);
    cJSON_AddStringToObject(root, "currency", cur);
    cJSON_AddBoolToObject(root, "checkout_ready", ok_all && cJSON_GetArraySize(arr) > 0);
    return api_ok(c, 200, root);
}

int h_cart_put(rctx *c) {
    uint64_t pid;
    if (api_seg_id(c, 4, &pid)) return c->sent;    /* /api/v1/cart/items/{product_id} */
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    int64_t qty;
    if (api_int(c, b, "quantity", 1, 99, &qty)) { cJSON_Delete(b); return c->sent; }
    cJSON_Delete(b);

    db_rs *rs = db_query(c->db, "SELECT is_active FROM products WHERE id = ?", "u", pid);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    uint64_t active = 0;
    int f = db_rs_next(rs, "u", &active);
    db_rs_free(rs);
    if (f < 0) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    if (f == 0 || !active) return api_err(c, 404, "NOT_FOUND", "No such product.", NULL);

    rs = db_query(c->db, "SELECT COUNT(*) FROM cart_items WHERE user_id = ? AND product_id <> ?",
                  "uu", c->user.user_id, pid);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    uint64_t others = 0;
    db_rs_next(rs, "u", &others);
    db_rs_free(rs);
    if (others >= MAX_CART_LINES)
        return api_err(c, 409, "CART_FULL", "Cart cannot hold more than 50 different products.", NULL);

    int64_t n = db_exec(c->db,
        "INSERT INTO cart_items (user_id, product_id, quantity) VALUES (?, ?, ?) AS new "
        "ON DUPLICATE KEY UPDATE quantity = new.quantity",
        "uui", c->user.user_id, pid, qty);
    if (n < 0) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "product_id", (double)pid);
    cJSON_AddNumberToObject(r, "quantity", (double)qty);
    return api_ok(c, 200, r);
}

int h_cart_delete_item(rctx *c) {
    uint64_t pid;
    if (api_seg_id(c, 4, &pid)) return c->sent;
    if (db_exec(c->db, "DELETE FROM cart_items WHERE user_id = ? AND product_id = ?", "uu",
                c->user.user_id, pid) < 0)
        return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    return api_ok(c, 200, r);            /* idempotent: removing an absent line is fine */
}

int h_cart_clear(rctx *c) {
    if (db_exec(c->db, "DELETE FROM cart_items WHERE user_id = ?", "u", c->user.user_id) < 0)
        return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    return api_ok(c, 200, r);
}
