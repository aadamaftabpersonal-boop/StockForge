/* Administrator endpoints. Every route here is gated to role=admin by the router. */
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "audit.h"
#include "reconcile.h"
#include "validate.h"

#define DB_DOWN "Service temporarily unavailable."

int h_admin_product_create(rctx *c) {
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    const char *sku, *name, *desc, *image;
    int64_t price, cat, stock, thr;
    if (api_str(c, b, "sku", &sku, 1) || api_str(c, b, "name", &name, 1) ||
        api_str(c, b, "description", &desc, 1) || api_str(c, b, "image_url", &image, 0) ||
        api_int(c, b, "price_cents", 1, 100000000, &price) ||
        api_int(c, b, "category_id", 1, 4294967295LL, &cat) ||
        api_int(c, b, "initial_stock", 0, 1000000, &stock))
        goto out;
    thr = 5;
    if (cJSON_GetObjectItemCaseSensitive(b, "low_stock_threshold") &&
        api_int(c, b, "low_stock_threshold", 0, 1000000, &thr))
        goto out;
    size_t nl = strlen(name), dl = strlen(desc);
    if (!valid_sku(sku)) { api_err(c, 422, "VALIDATION_FAILED", "SKU must be 1-40 letters, digits, - or _.", "sku"); goto out; }
    if (nl < 1 || nl > 200 || !utf8_valid(name, nl)) { api_err(c, 422, "VALIDATION_FAILED", "Name must be 1-200 characters.", "name"); goto out; }
    if (dl > 4000 || !utf8_valid(desc, dl)) { api_err(c, 422, "VALIDATION_FAILED", "Description must be at most 4000 characters.", "description"); goto out; }
    /* https URL or a same-site absolute path; "//host/x" (protocol-relative) is an external URL in disguise */
    if (image && (strlen(image) > 500 ||
                  (strncmp(image, "https://", 8) && (image[0] != '/' || image[1] == '/')))) {
        api_err(c, 422, "VALIDATION_FAILED", "image_url must be an https URL or an absolute path.", "image_url");
        goto out;
    }

    if (db_begin(c->db)) { api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL); goto out; }
    if (db_exec(c->db,
            "INSERT INTO products (sku, category_id, name, description, price_cents, image_url) "
            "VALUES (?, ?, ?, ?, ?, ?)", "sissis",
            sku, cat, name, desc, price, image) != 1) {
        int dup = db_is_dup(c->db);
        unsigned en = db_errno(c->db);
        db_rollback(c->db);
        if (dup) api_err(c, 409, "SKU_TAKEN", "A product with this SKU already exists.", "sku");
        else if (en == 1452) api_err(c, 422, "VALIDATION_FAILED", "Unknown category.", "category_id");
        else api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL);
        goto out;
    }
    uint64_t pid = db_insert_id(c->db);
    if (db_exec(c->db, "INSERT INTO inventory (product_id, on_hand, reserved, low_stock_threshold) VALUES (?, ?, 0, ?)",
                "uii", pid, stock, thr) != 1 ||
        db_exec(c->db, "INSERT INTO inventory_ledger (product_id, delta_on_hand, delta_reserved, reason) "
                       "VALUES (?, ?, 0, 'seed')", "ui", pid, stock) != 1) {
        db_rollback(c->db);
        api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL);
        goto out;
    }
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)pid);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "sku", sku);
    cJSON_AddNumberToObject(d, "initial_stock", (double)stock);
    int ar = audit_log(c->db, c->rid, c->user.user_id, "product.created", "product", eid, d);
    cJSON_Delete(d);
    if (ar || db_commit(c->db)) { db_rollback(c->db); api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL); goto out; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", (double)pid);
    cJSON_AddStringToObject(r, "sku", sku);
    api_ok(c, 201, r);
out:
    cJSON_Delete(b);
    return c->sent;
}

/* Adjusts on-hand stock by a signed delta under a row lock, with ledger + audit
 * rows in the SAME transaction. Cannot drop on_hand below what is reserved. */
int h_admin_stock_adjust(rctx *c) {
    uint64_t pid;
    if (api_seg_id(c, 4, &pid)) return c->sent;    /* /api/v1/admin/inventory/{id}/adjust */
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    int64_t delta;
    const char *reason;
    if (api_int(c, b, "delta", -1000000, 1000000, &delta)) goto out;
    if (api_str(c, b, "reason", &reason, 1)) goto out;
    if (delta == 0) { api_err(c, 422, "VALIDATION_FAILED", "Delta must not be zero.", "delta"); goto out; }
    if (strcmp(reason, "restock") && strcmp(reason, "adjust")) {
        api_err(c, 422, "VALIDATION_FAILED", "reason must be restock or adjust.", "reason");
        goto out;
    }

    if (db_begin(c->db)) { api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL); goto out; }
    db_rs *rs = db_query(c->db, "SELECT on_hand, reserved FROM inventory WHERE product_id = ? FOR UPDATE", "u", pid);
    if (!rs) { db_rollback(c->db); api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL); goto out; }
    uint64_t on_hand = 0, reserved = 0;
    int f = db_rs_next(rs, "uu", &on_hand, &reserved);
    db_rs_free(rs);
    if (f != 1) {
        db_rollback(c->db);
        if (f == 0) api_err(c, 404, "NOT_FOUND", "No such product.", NULL);
        else api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL);
        goto out;
    }
    int64_t next = (int64_t)on_hand + delta;
    if (next < (int64_t)reserved) {          /* also covers next < 0 */
        db_rollback(c->db);
        api_err(c, 409, "BELOW_RESERVED", "Stock cannot drop below the quantity already reserved by open orders.", "delta");
        goto out;
    }
    if (db_exec(c->db, "UPDATE inventory SET on_hand = ? WHERE product_id = ?", "iu", next, pid) != 1 ||
        db_exec(c->db, "INSERT INTO inventory_ledger (product_id, delta_on_hand, delta_reserved, reason) "
                       "VALUES (?, ?, 0, ?)", "uis", pid, delta, reason) != 1) {
        db_rollback(c->db);
        api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL);
        goto out;
    }
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)pid);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "delta", (double)delta);
    cJSON_AddStringToObject(d, "reason", reason);
    cJSON_AddNumberToObject(d, "on_hand_after", (double)next);
    int ar = audit_log(c->db, c->rid, c->user.user_id, "inventory.adjusted", "product", eid, d);
    cJSON_Delete(d);
    if (ar || db_commit(c->db)) { db_rollback(c->db); api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL); goto out; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "product_id", (double)pid);
    cJSON_AddNumberToObject(r, "on_hand", (double)next);
    cJSON_AddNumberToObject(r, "reserved", (double)reserved);
    cJSON_AddNumberToObject(r, "available", (double)(next - (int64_t)reserved));
    api_ok(c, 200, r);
out:
    cJSON_Delete(b);
    return c->sent;
}

int h_admin_reconcile(rctx *c) {
    rc_check checks[RC_MAX_CHECKS];
    int n = reconcile_run(c->db, checks, RC_MAX_CHECKS);
    if (n < 0) return api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL);
    return api_ok(c, 200, reconcile_json(checks, n));
}

int h_admin_audit(rctx *c) {
    int limit, r;
    char cur[24], action[64];
    uint64_t after = 0, dummy = 0;
    if (api_limit(c, 50, 200, &limit)) return c->sent;
    r = api_qs(c, "after", cur, sizeof cur);
    if (r < 0 || (r == 1 && cursor_parse(cur, &after, &dummy) != 1))
        return api_err(c, 422, "VALIDATION_FAILED", "Invalid cursor.", "after");
    r = api_qs(c, "action", action, sizeof action);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid action filter.", "action");
    int has_action = r == 1 && action[0];

    db_params p = {.n = 0};
    char sql[600];
    size_t o = 0;
#define ADD(s) (o += (size_t)snprintf(sql + o, sizeof sql - o, "%s", (s)))
    ADD("SELECT id, COALESCE(request_id, ''), COALESCE(actor_user_id, 0), action, entity_type, entity_id, "
        "COALESCE(CAST(details AS CHAR), ''), DATE_FORMAT(created_at, '%Y-%m-%dT%H:%i:%sZ') "
        "FROM audit_events WHERE 1=1 ");
    if (after) { ADD("AND id < ? "); dbp_u(&p, after); }
    if (has_action) { ADD("AND action = ? "); dbp_s(&p, action); }
    ADD("ORDER BY id DESC LIMIT ?");
    dbp_i(&p, limit + 1);
#undef ADD
    db_rs *rs = db_query_p(c->db, sql, &p);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL);

    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "items");
    uint64_t id, actor, last = 0;
    char rid[24], act[64], et[48], eid[48], det[2048], ts[32];
    int n = 0, more = 0;
    while (db_rs_next(rs, "ususssss", &id, rid, sizeof rid, &actor, act, sizeof act, et, sizeof et,
                      eid, sizeof eid, det, sizeof det, ts, sizeof ts) == 1) {
        if (n == limit) { more = 1; break; }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", (double)id);
        cJSON_AddStringToObject(e, "request_id", rid);
        if (actor) cJSON_AddNumberToObject(e, "actor_user_id", (double)actor);
        else cJSON_AddNullToObject(e, "actor_user_id");
        cJSON_AddStringToObject(e, "action", act);
        cJSON_AddStringToObject(e, "entity_type", et);
        cJSON_AddStringToObject(e, "entity_id", eid);
        cJSON *dj = det[0] ? cJSON_Parse(det) : NULL;
        if (dj) cJSON_AddItemToObject(e, "details", dj);
        else cJSON_AddNullToObject(e, "details");
        cJSON_AddStringToObject(e, "created_at", ts);
        cJSON_AddItemToArray(arr, e);
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
