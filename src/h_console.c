/* Administrator console APIs: dashboard statistics, order operations, catalogue and
 * customer management. Every route is role-gated by the router; every mutation is
 * transactional and writes an audit row with the acting administrator's id. */
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "audit.h"
#include "orders.h"
#include "validate.h"

#define DB_DOWN "Service temporarily unavailable."
#define TS(col) "DATE_FORMAT(" col ", '%Y-%m-%dT%H:%i:%sZ')"
#define PAID_STATES "('PAID','SHIPPED','DELIVERED')"

static int db_down(rctx *c) { return api_err(c, 503, "DB_UNAVAILABLE", DB_DOWN, NULL); }

static int one_u64(db *d, const char *sql, uint64_t *out) {
    db_rs *rs = db_query(d, sql, "");
    if (!rs) return -1;
    *out = 0;
    int f = db_rs_next(rs, "u", out);
    db_rs_free(rs);
    return f < 0 ? -1 : 0;
}

/* ================================================================ stats */

int h_admin_stats(rctx *c) {
    db *d = c->db;
    uint64_t v, v2;
    cJSON *root = cJSON_CreateObject();
    db_rs *rs;
#define FAIL do { cJSON_Delete(root); return db_down(c); } while (0)

    cJSON *rev = cJSON_AddObjectToObject(root, "revenue");
    if (one_u64(d, "SELECT COALESCE(SUM(total_cents),0) FROM orders WHERE status IN " PAID_STATES, &v)) FAIL;
    cJSON_AddNumberToObject(rev, "total_cents", (double)v);
    if (one_u64(d, "SELECT COALESCE(SUM(total_cents),0) FROM orders WHERE status IN " PAID_STATES
                   " AND created_at >= NOW(3) - INTERVAL 30 DAY", &v)) FAIL;
    if (one_u64(d, "SELECT COALESCE(SUM(total_cents),0) FROM orders WHERE status IN " PAID_STATES
                   " AND created_at >= NOW(3) - INTERVAL 60 DAY AND created_at < NOW(3) - INTERVAL 30 DAY", &v2)) FAIL;
    cJSON_AddNumberToObject(rev, "last_30d_cents", (double)v);
    cJSON_AddNumberToObject(rev, "previous_30d_cents", (double)v2);
    if (one_u64(d, "SELECT COUNT(*) FROM orders WHERE status IN " PAID_STATES, &v)) FAIL;
    cJSON_AddNumberToObject(rev, "paid_orders", (double)v);

    cJSON *by = cJSON_AddObjectToObject(root, "orders_by_status");
    rs = db_query(d, "SELECT status, COUNT(*) FROM orders GROUP BY status", "");
    if (!rs) FAIL;
    char st[24];
    while (db_rs_next(rs, "su", st, sizeof st, &v) == 1) cJSON_AddNumberToObject(by, st, (double)v);
    db_rs_free(rs);

    char today[16] = "";
    rs = db_query(d, "SELECT DATE_FORMAT(UTC_DATE(), '%Y-%m-%d')", "");
    if (!rs) FAIL;
    db_rs_next(rs, "s", today, sizeof today);
    db_rs_free(rs);
    cJSON_AddStringToObject(root, "today", today);
    cJSON *days = cJSON_AddArrayToObject(root, "daily");           /* days with no orders are absent; the client fills zeros */
    rs = db_query(d, "SELECT DATE_FORMAT(created_at, '%Y-%m-%d') d, COUNT(*), COALESCE(SUM(total_cents),0) "
                     "FROM orders WHERE status IN " PAID_STATES " AND created_at >= UTC_DATE() - INTERVAL 13 DAY "
                     "GROUP BY d ORDER BY d", "");
    if (!rs) FAIL;
    char day[16];
    uint64_t n, sum;
    while (db_rs_next(rs, "suu", day, sizeof day, &n, &sum) == 1) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "date", day);
        cJSON_AddNumberToObject(o, "orders", (double)n);
        cJSON_AddNumberToObject(o, "revenue_cents", (double)sum);
        cJSON_AddItemToArray(days, o);
    }
    db_rs_free(rs);

    cJSON *inv = cJSON_AddObjectToObject(root, "inventory");
    uint64_t total, low, out;
    rs = db_query(d, "SELECT COUNT(*), "
                     "COALESCE(SUM(i.on_hand - i.reserved = 0), 0), "
                     "COALESCE(SUM(i.on_hand - i.reserved > 0 AND i.on_hand - i.reserved <= i.low_stock_threshold), 0) "
                     "FROM inventory i JOIN products p ON p.id = i.product_id WHERE p.is_active = 1", "");
    if (!rs) FAIL;
    db_rs_next(rs, "uuu", &total, &out, &low);
    db_rs_free(rs);
    cJSON_AddNumberToObject(inv, "active_products", (double)total);
    cJSON_AddNumberToObject(inv, "out_of_stock", (double)out);
    cJSON_AddNumberToObject(inv, "low_stock", (double)low);
    if (one_u64(d, "SELECT COALESCE(SUM(reserved),0) FROM inventory", &v)) FAIL;
    cJSON_AddNumberToObject(inv, "units_reserved", (double)v);

    cJSON *cust = cJSON_AddObjectToObject(root, "customers");
    if (one_u64(d, "SELECT COUNT(*) FROM users WHERE role = 'customer'", &v)) FAIL;
    cJSON_AddNumberToObject(cust, "total", (double)v);
    if (one_u64(d, "SELECT COUNT(*) FROM users WHERE role = 'customer' AND created_at >= NOW(3) - INTERVAL 7 DAY", &v)) FAIL;
    cJSON_AddNumberToObject(cust, "new_7d", (double)v);

    cJSON *top = cJSON_AddArrayToObject(root, "top_products_30d");
    rs = db_query(d, "SELECT oi.product_id, oi.product_name, SUM(oi.quantity) q, SUM(oi.quantity * oi.unit_price_cents) "
                     "FROM order_items oi JOIN orders o ON o.id = oi.order_id "
                     "WHERE o.status IN " PAID_STATES " AND o.created_at >= NOW(3) - INTERVAL 30 DAY "
                     "GROUP BY oi.product_id, oi.product_name ORDER BY q DESC, oi.product_id LIMIT 5", "");
    if (!rs) FAIL;
    uint64_t pid, qty, amt;
    char name[208];
    while (db_rs_next(rs, "usuu", &pid, name, sizeof name, &qty, &amt) == 1) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "product_id", (double)pid);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "units", (double)qty);
        cJSON_AddNumberToObject(o, "revenue_cents", (double)amt);
        cJSON_AddItemToArray(top, o);
    }
    db_rs_free(rs);
#undef FAIL
    return api_ok(c, 200, root);
}

/* ============================================================ orders */

static int parse_status_filter(rctx *c, order_status *out, int *has) {
    char s[24];
    int r = api_qs(c, "status", s, sizeof s);
    *has = 0;
    if (r < 0) return -1;
    if (r == 1 && s[0]) {
        *out = os_parse(s);
        if (*out == OS_INVALID) return -1;
        *has = 1;
    }
    return 0;
}

int h_admin_orders(rctx *c) {
    int limit, has_status, has_q = 0, r;
    order_status st = OS_INVALID;
    char q[64], q_up[64], q_lo[64], cur[24];
    uint64_t after = 0, dummy = 0;
    if (api_limit(c, 25, 100, &limit)) return c->sent;
    if (parse_status_filter(c, &st, &has_status))
        return api_err(c, 422, "VALIDATION_FAILED", "Unknown status.", "status");
    r = api_qs(c, "q", q, sizeof q);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid search text.", "q");
    if (r == 1 && q[0]) {
        for (const char *p = q; *p; p++)                      /* no LIKE wildcards can reach the pattern */
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || strchr("@.+-", *p)))
                return api_err(c, 422, "VALIDATION_FAILED", "Search may contain letters, digits and @ . + - only.", "q");
        size_t n = strlen(q);
        for (size_t i = 0; i <= n; i++) {
            q_up[i] = (q[i] >= 'a' && q[i] <= 'z') ? (char)(q[i] - 32) : q[i];
            q_lo[i] = (q[i] >= 'A' && q[i] <= 'Z') ? (char)(q[i] + 32) : q[i];
        }
        has_q = 1;
    }
    r = api_qs(c, "after", cur, sizeof cur);
    if (r < 0 || (r == 1 && cursor_parse(cur, &after, &dummy) != 1))
        return api_err(c, 422, "VALIDATION_FAILED", "Invalid cursor.", "after");

    char sql[1100];
    size_t o = 0;
    db_params p = {.n = 0};
#define ADD(s) (o += (size_t)snprintf(sql + o, sizeof sql - o, "%s", (s)))
    ADD("SELECT o.id, o.public_id, o.status, o.total_cents, o.currency, " TS("o.created_at") ", "
        "(SELECT COALESCE(SUM(oi.quantity), 0) FROM order_items oi WHERE oi.order_id = o.id), "
        "u.id, u.email, u.display_name FROM orders o JOIN users u ON u.id = o.user_id WHERE 1 = 1 ");
    if (has_status) { ADD("AND o.status = ? "); dbp_s(&p, os_name(st)); }
    if (has_q) { ADD("AND (o.public_id LIKE CONCAT(?, '%') OR u.email LIKE CONCAT(?, '%')) "); dbp_s(&p, q_up); dbp_s(&p, q_lo); }
    if (after) { ADD("AND o.id < ? "); dbp_u(&p, after); }
    ADD("ORDER BY o.id DESC LIMIT ?");
    dbp_i(&p, limit + 1);
#undef ADD
    db_rs *rs = db_query_p(c->db, sql, &p);
    if (!rs) return db_down(c);

    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "items");
    uint64_t id, total, items, uid, last = 0;
    char pub[32], status[24], cur3[8], ts[32], email[256], name[128];
    int n = 0, more = 0;
    while (db_rs_next(rs, "ussussuuss", &id, pub, sizeof pub, status, sizeof status, &total, cur3, sizeof cur3,
                      ts, sizeof ts, &items, &uid, email, sizeof email, name, sizeof name) == 1) {
        if (n == limit) { more = 1; break; }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", pub);
        cJSON_AddStringToObject(e, "status", status);
        cJSON_AddNumberToObject(e, "total_cents", (double)total);
        cJSON_AddStringToObject(e, "currency", cur3);
        cJSON_AddStringToObject(e, "created_at", ts);
        cJSON_AddNumberToObject(e, "item_count", (double)items);
        cJSON *cu = cJSON_AddObjectToObject(e, "customer");
        cJSON_AddNumberToObject(cu, "id", (double)uid);
        cJSON_AddStringToObject(cu, "email", email);
        cJSON_AddStringToObject(cu, "display_name", name);
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

static int admin_pub_id(rctx *c, char *out, size_t n) {
    if (c->nseg < 5 || c->seglen[4] >= n) { api_err(c, 404, "NOT_FOUND", "No such order.", NULL); return -1; }
    memcpy(out, c->seg[4], c->seglen[4]);
    out[c->seglen[4]] = 0;
    if (!valid_public_id(out)) { api_err(c, 404, "NOT_FOUND", "No such order.", NULL); return -1; }
    return 0;
}

int h_admin_order(rctx *c) {
    char pub[32];
    if (admin_pub_id(c, pub, sizeof pub)) return c->sent;
    db_rs *rs = db_query(c->db, "SELECT o.id, u.id, u.email, u.display_name FROM orders o "
                                "JOIN users u ON u.id = o.user_id WHERE o.public_id = ?", "s", pub);
    if (!rs) return db_down(c);
    uint64_t oid = 0, uid = 0;
    char email[256], name[128];
    int f = db_rs_next(rs, "uuss", &oid, &uid, email, sizeof email, name, sizeof name);
    db_rs_free(rs);
    if (f < 0) return db_down(c);
    if (f == 0) return api_err(c, 404, "NOT_FOUND", "No such order.", NULL);
    cJSON *oj = orders_json_by_id(c->db, oid);
    if (!oj) return db_down(c);
    cJSON *cu = cJSON_AddObjectToObject(oj, "customer");
    cJSON_AddNumberToObject(cu, "id", (double)uid);
    cJSON_AddStringToObject(cu, "email", email);
    cJSON_AddStringToObject(cu, "display_name", name);

    cJSON *hist = cJSON_AddArrayToObject(oj, "history");           /* this order's audit trail, oldest first */
    rs = db_query(c->db, "SELECT action, COALESCE(actor_user_id, 0), " TS("created_at") " FROM audit_events "
                         "WHERE entity_type = 'order' AND entity_id = ? ORDER BY id LIMIT 50", "s", pub);
    if (rs) {
        char act[64], ts[32];
        uint64_t actor;
        while (db_rs_next(rs, "sus", act, sizeof act, &actor, ts, sizeof ts) == 1) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "action", act);
            if (actor) cJSON_AddNumberToObject(e, "actor_user_id", (double)actor);
            else cJSON_AddNullToObject(e, "actor_user_id");
            cJSON_AddStringToObject(e, "at", ts);
            cJSON_AddItemToArray(hist, e);
        }
        db_rs_free(rs);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "order", oj);
    return api_ok(c, 200, root);
}

int h_admin_order_transition(rctx *c) {
    char pub[32];
    if (admin_pub_id(c, pub, sizeof pub)) return c->sent;
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    const char *to = NULL;
    if (api_str(c, b, "to", &to, 1)) { cJSON_Delete(b); return c->sent; }
    order_status target = os_parse(to);
    cJSON_Delete(b);                                              /* `to` pointed into b: parsed above */
    if (target != OS_SHIPPED && target != OS_DELIVERED && target != OS_CANCELLED)
        return api_err(c, 422, "VALIDATION_FAILED", "to must be SHIPPED, DELIVERED or CANCELLED.", "to");
    order_out out = {0};
    int rc = target == OS_CANCELLED ? orders_admin_cancel(c->db, c->rid, c->user.user_id, pub, &out)
                                    : orders_admin_fulfil(c->db, c->rid, c->user.user_id, pub, target, &out);
    if (rc != 0) {
        order_out_free(&out);
        return api_err(c, 503, rc == 1 ? "DB_BUSY" : "DB_UNAVAILABLE", DB_DOWN, NULL);
    }
    c->sent = resp_json(c->fd, out.http, c->rid, out.json, NULL);
    return c->sent;
}

/* ========================================================= products */

int h_admin_products(rctx *c) {
    int limit, r;
    char q[64], cat[96], stock[8], cur[24];
    uint64_t after = 0, dummy = 0;
    int has_q = 0, has_cat = 0, mode = 0;                         /* mode: 1 low, 2 out */
    if (api_limit(c, 25, 100, &limit)) return c->sent;
    r = api_qs(c, "q", q, sizeof q);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid search text.", "q");
    if (r == 1 && q[0]) {
        for (const char *p = q; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || strchr("-_", *p)))
                return api_err(c, 422, "VALIDATION_FAILED", "Search by SKU: letters, digits, - and _ only.", "q");
        has_q = 1;
    }
    r = api_qs(c, "category", cat, sizeof cat);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid category.", "category");
    if (r == 1 && cat[0]) {
        for (const char *p = cat; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
                return api_err(c, 422, "VALIDATION_FAILED", "Invalid category.", "category");
        has_cat = 1;
    }
    r = api_qs(c, "stock", stock, sizeof stock);
    if (r < 0 || (r == 1 && stock[0] && strcmp(stock, "low") && strcmp(stock, "out")))
        return api_err(c, 422, "VALIDATION_FAILED", "stock must be low or out.", "stock");
    if (r == 1 && stock[0]) mode = strcmp(stock, "low") == 0 ? 1 : 2;
    r = api_qs(c, "after", cur, sizeof cur);
    if (r < 0 || (r == 1 && cursor_parse(cur, &after, &dummy) != 1))
        return api_err(c, 422, "VALIDATION_FAILED", "Invalid cursor.", "after");

    char sql[1100];
    size_t o = 0;
    db_params p = {.n = 0};
#define ADD(s) (o += (size_t)snprintf(sql + o, sizeof sql - o, "%s", (s)))
    ADD("SELECT p.id, p.sku, p.name, p.price_cents, p.is_active, c.id, c.slug, c.name, "
        "i.on_hand, i.reserved, i.low_stock_threshold, COALESCE(p.image_url, '') "
        "FROM products p JOIN categories c ON c.id = p.category_id JOIN inventory i ON i.product_id = p.id WHERE 1 = 1 ");
    if (has_q) { ADD("AND (p.sku LIKE CONCAT(?, '%') OR p.name LIKE CONCAT('%', ?, '%')) "); dbp_s(&p, q); dbp_s(&p, q); }
    if (has_cat) { ADD("AND c.slug = ? "); dbp_s(&p, cat); }
    if (mode == 1) ADD("AND i.on_hand - i.reserved > 0 AND i.on_hand - i.reserved <= i.low_stock_threshold ");
    if (mode == 2) ADD("AND i.on_hand - i.reserved = 0 ");
    if (after) { ADD("AND p.id < ? "); dbp_u(&p, after); }
    ADD("ORDER BY p.id DESC LIMIT ?");
    dbp_i(&p, limit + 1);
#undef ADD
    db_rs *rs = db_query_p(c->db, sql, &p);
    if (!rs) return db_down(c);
    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "items");
    uint64_t id, price, active, cid, onh, res, thr, last = 0;
    char sku[48], name[208], slug[88], cname[128], img[512];
    int n = 0, more = 0;
    while (db_rs_next(rs, "ussuuussuuus", &id, sku, sizeof sku, name, sizeof name, &price, &active, &cid,
                      slug, sizeof slug, cname, sizeof cname, &onh, &res, &thr, img, sizeof img) == 1) {
        if (n == limit) { more = 1; break; }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", (double)id);
        cJSON_AddStringToObject(e, "sku", sku);
        cJSON_AddStringToObject(e, "name", name);
        cJSON_AddNumberToObject(e, "price_cents", (double)price);
        cJSON_AddBoolToObject(e, "is_active", active != 0);
        cJSON *ca = cJSON_AddObjectToObject(e, "category");
        cJSON_AddNumberToObject(ca, "id", (double)cid);
        cJSON_AddStringToObject(ca, "slug", slug);
        cJSON_AddStringToObject(ca, "name", cname);
        cJSON_AddNumberToObject(e, "on_hand", (double)onh);
        cJSON_AddNumberToObject(e, "reserved", (double)res);
        cJSON_AddNumberToObject(e, "available", (double)(onh - res));
        cJSON_AddNumberToObject(e, "low_stock_threshold", (double)thr);
        if (img[0]) cJSON_AddStringToObject(e, "image_url", img);
        else cJSON_AddNullToObject(e, "image_url");
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

static int get_bool(const cJSON *f, int *out) {
    if (cJSON_IsBool(f)) { *out = cJSON_IsTrue(f); return 0; }
    return -1;
}

/* PATCH: any subset of name, description, price_cents, category_id, is_active, image_url, low_stock_threshold. */
int h_admin_product_patch(rctx *c) {
    uint64_t pid;
    if (api_seg_id(c, 4, &pid)) return c->sent;
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    const char *name = NULL, *desc = NULL, *image = NULL;
    int64_t price = 0, cat = 0, thr = 0;
    int active = 0, has_name = 0, has_desc = 0, has_price = 0, has_cat = 0, has_active = 0, has_img = 0, has_thr = 0, clear_img = 0;
    const cJSON *f;

    if ((f = cJSON_GetObjectItemCaseSensitive(b, "name"))) {
        if (api_str(c, b, "name", &name, 1)) goto out;
        size_t l = strlen(name);
        if (l < 1 || l > 200 || !utf8_valid(name, l)) { api_err(c, 422, "VALIDATION_FAILED", "Name must be 1-200 characters.", "name"); goto out; }
        has_name = 1;
    }
    if ((f = cJSON_GetObjectItemCaseSensitive(b, "description"))) {
        if (api_str(c, b, "description", &desc, 1)) goto out;
        size_t l = strlen(desc);
        if (l > 4000 || !utf8_valid(desc, l)) { api_err(c, 422, "VALIDATION_FAILED", "Description must be at most 4000 characters.", "description"); goto out; }
        has_desc = 1;
    }
    if (cJSON_GetObjectItemCaseSensitive(b, "price_cents")) {
        if (api_int(c, b, "price_cents", 1, 100000000, &price)) goto out;
        has_price = 1;
    }
    if (cJSON_GetObjectItemCaseSensitive(b, "category_id")) {
        if (api_int(c, b, "category_id", 1, 4294967295LL, &cat)) goto out;
        has_cat = 1;
    }
    if (cJSON_GetObjectItemCaseSensitive(b, "low_stock_threshold")) {
        if (api_int(c, b, "low_stock_threshold", 0, 1000000, &thr)) goto out;
        has_thr = 1;
    }
    if ((f = cJSON_GetObjectItemCaseSensitive(b, "is_active"))) {
        if (get_bool(f, &active)) { api_err(c, 422, "VALIDATION_FAILED", "Must be true or false.", "is_active"); goto out; }
        has_active = 1;
    }
    if ((f = cJSON_GetObjectItemCaseSensitive(b, "image_url"))) {
        if (cJSON_IsNull(f)) clear_img = 1;
        else {
            if (api_str(c, b, "image_url", &image, 1)) goto out;
            if (strlen(image) > 500 || (strncmp(image, "https://", 8) && (image[0] != '/' || image[1] == '/'))) {
                api_err(c, 422, "VALIDATION_FAILED", "image_url must be an https URL or an absolute path.", "image_url");
                goto out;
            }
            has_img = 1;
        }
    }
    if (!(has_name || has_desc || has_price || has_cat || has_active || has_img || clear_img || has_thr)) {
        api_err(c, 422, "VALIDATION_FAILED", "Nothing to update.", NULL);
        goto out;
    }

    if (db_begin(c->db)) { db_down(c); goto out; }
    db_rs *rs = db_query(c->db, "SELECT id FROM products WHERE id = ? FOR UPDATE", "u", pid);
    if (!rs) { db_rollback(c->db); db_down(c); goto out; }
    uint64_t found = 0;
    int fr = db_rs_next(rs, "u", &found);
    db_rs_free(rs);
    if (fr != 1) {
        db_rollback(c->db);
        if (fr == 0) api_err(c, 404, "NOT_FOUND", "No such product.", NULL); else db_down(c);
        goto out;
    }
    char sql[400];
    size_t o = 0;
    db_params p = {.n = 0};
    int first = 1;
#define SET(frag) (o += (size_t)snprintf(sql + o, sizeof sql - o, "%s%s", first ? "" : ", ", frag), first = 0)
    o += (size_t)snprintf(sql, sizeof sql, "UPDATE products SET ");
    if (has_name) { SET("name = ?"); dbp_s(&p, name); }
    if (has_desc) { SET("description = ?"); dbp_s(&p, desc); }
    if (has_price) { SET("price_cents = ?"); dbp_i(&p, price); }
    if (has_cat) { SET("category_id = ?"); dbp_i(&p, cat); }
    if (has_active) { SET("is_active = ?"); dbp_i(&p, active); }
    if (has_img) { SET("image_url = ?"); dbp_s(&p, image); }
    if (clear_img) SET("image_url = NULL");
#undef SET
    if (!first) {                                                 /* any products column to change */
        snprintf(sql + o, sizeof sql - o, " WHERE id = ?");
        dbp_u(&p, pid);
        if (db_exec_p(c->db, sql, &p) < 0) {
            unsigned en = db_errno(c->db);
            db_rollback(c->db);
            if (en == 1452) api_err(c, 422, "VALIDATION_FAILED", "Unknown category.", "category_id");
            else db_down(c);
            goto out;
        }
    }
    if (has_thr && db_exec(c->db, "UPDATE inventory SET low_stock_threshold = ? WHERE product_id = ?", "iu", thr, pid) < 0) {
        db_rollback(c->db);
        db_down(c);
        goto out;
    }
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)pid);
    cJSON *det = cJSON_CreateObject(), *fields = cJSON_AddArrayToObject(det, "fields");
    if (has_name) cJSON_AddItemToArray(fields, cJSON_CreateString("name"));
    if (has_desc) cJSON_AddItemToArray(fields, cJSON_CreateString("description"));
    if (has_price) { cJSON_AddItemToArray(fields, cJSON_CreateString("price_cents")); cJSON_AddNumberToObject(det, "price_cents", (double)price); }
    if (has_cat) { cJSON_AddItemToArray(fields, cJSON_CreateString("category_id")); cJSON_AddNumberToObject(det, "category_id", (double)cat); }
    if (has_active) { cJSON_AddItemToArray(fields, cJSON_CreateString("is_active")); cJSON_AddBoolToObject(det, "is_active", active); }
    if (has_img || clear_img) cJSON_AddItemToArray(fields, cJSON_CreateString("image_url"));
    if (has_thr) { cJSON_AddItemToArray(fields, cJSON_CreateString("low_stock_threshold")); cJSON_AddNumberToObject(det, "low_stock_threshold", (double)thr); }
    int ar = audit_log(c->db, c->rid, c->user.user_id, "product.updated", "product", eid, det);
    cJSON_Delete(det);
    if (ar || db_commit(c->db)) { db_rollback(c->db); db_down(c); goto out; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", (double)pid);
    cJSON_AddBoolToObject(r, "updated", 1);
    api_ok(c, 200, r);
out:
    cJSON_Delete(b);
    return c->sent;
}

/* ======================================================= categories */

static int valid_slug(const char *s) {
    size_t n = strlen(s);
    if (n < 2 || n > 80) return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-')) return 0;
    return 1;
}

int h_admin_category_create(rctx *c) {
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    const char *slug, *name;
    if (api_str(c, b, "slug", &slug, 1) || api_str(c, b, "name", &name, 1)) goto out;
    size_t nl = strlen(name);
    if (!valid_slug(slug)) { api_err(c, 422, "VALIDATION_FAILED", "Slug must be 2-80 characters of a-z, 0-9 and -.", "slug"); goto out; }
    if (nl < 1 || nl > 120 || !utf8_valid(name, nl)) { api_err(c, 422, "VALIDATION_FAILED", "Name must be 1-120 characters.", "name"); goto out; }
    if (db_exec(c->db, "INSERT INTO categories (slug, name) VALUES (?, ?)", "ss", slug, name) != 1) {
        if (db_is_dup(c->db)) api_err(c, 409, "SLUG_TAKEN", "A category with this slug already exists.", "slug");
        else db_down(c);
        goto out;
    }
    uint64_t id = db_insert_id(c->db);
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)id);
    cJSON *det = cJSON_CreateObject();
    cJSON_AddStringToObject(det, "slug", slug);
    audit_log(c->db, c->rid, c->user.user_id, "category.created", "category", eid, det);
    cJSON_Delete(det);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", (double)id);
    cJSON_AddStringToObject(r, "slug", slug);
    api_ok(c, 201, r);
out:
    cJSON_Delete(b);
    return c->sent;
}

int h_admin_category_patch(rctx *c) {
    uint64_t id;
    if (api_seg_id(c, 4, &id)) return c->sent;
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    const char *name;
    if (api_str(c, b, "name", &name, 1)) goto out;
    size_t nl = strlen(name);
    if (nl < 1 || nl > 120 || !utf8_valid(name, nl)) { api_err(c, 422, "VALIDATION_FAILED", "Name must be 1-120 characters.", "name"); goto out; }
    db_rs *rs = db_query(c->db, "SELECT id FROM categories WHERE id = ?", "u", id);
    if (!rs) { db_down(c); goto out; }
    uint64_t found = 0;
    int f = db_rs_next(rs, "u", &found);
    db_rs_free(rs);
    if (f != 1) { if (f == 0) api_err(c, 404, "NOT_FOUND", "No such category.", NULL); else db_down(c); goto out; }
    if (db_exec(c->db, "UPDATE categories SET name = ? WHERE id = ?", "su", name, id) < 0) { db_down(c); goto out; }
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)id);
    audit_log(c->db, c->rid, c->user.user_id, "category.renamed", "category", eid, NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", (double)id);
    cJSON_AddBoolToObject(r, "updated", 1);
    api_ok(c, 200, r);
out:
    cJSON_Delete(b);
    return c->sent;
}

/* ========================================================= customers */

int h_admin_users(rctx *c) {
    int limit, r, has_q = 0;
    char q[64], cur[24];
    uint64_t after = 0, dummy = 0;
    if (api_limit(c, 25, 100, &limit)) return c->sent;
    r = api_qs(c, "q", q, sizeof q);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid search text.", "q");
    if (r == 1 && q[0]) {
        for (const char *p = q; *p; p++)
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || strchr("@.+-", *p)))
                return api_err(c, 422, "VALIDATION_FAILED", "Search by email: letters, digits and @ . + - only.", "q");
        email_normalize(q);
        has_q = 1;
    }
    r = api_qs(c, "after", cur, sizeof cur);
    if (r < 0 || (r == 1 && cursor_parse(cur, &after, &dummy) != 1))
        return api_err(c, 422, "VALIDATION_FAILED", "Invalid cursor.", "after");

    char sql[900];
    size_t o = 0;
    db_params p = {.n = 0};
#define ADD(s) (o += (size_t)snprintf(sql + o, sizeof sql - o, "%s", (s)))
    ADD("SELECT u.id, u.email, u.display_name, u.role, u.is_active, " TS("u.created_at") ", "
        "(SELECT COUNT(*) FROM orders o WHERE o.user_id = u.id), "
        "(SELECT COALESCE(SUM(o.total_cents), 0) FROM orders o WHERE o.user_id = u.id AND o.status IN " PAID_STATES ") "
        "FROM users u WHERE 1 = 1 ");
    if (has_q) { ADD("AND u.email LIKE CONCAT(?, '%') "); dbp_s(&p, q); }
    if (after) { ADD("AND u.id < ? "); dbp_u(&p, after); }
    ADD("ORDER BY u.id DESC LIMIT ?");
    dbp_i(&p, limit + 1);
#undef ADD
    db_rs *rs = db_query_p(c->db, sql, &p);
    if (!rs) return db_down(c);
    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "items");
    uint64_t id, active, orders, spent, last = 0;
    char email[256], name[128], role[16], ts[32];
    int n = 0, more = 0;
    while (db_rs_next(rs, "usssusuu", &id, email, sizeof email, name, sizeof name, role, sizeof role, &active,
                      ts, sizeof ts, &orders, &spent) == 1) {
        if (n == limit) { more = 1; break; }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", (double)id);
        cJSON_AddStringToObject(e, "email", email);
        cJSON_AddStringToObject(e, "display_name", name);
        cJSON_AddStringToObject(e, "role", role);
        cJSON_AddBoolToObject(e, "is_active", active != 0);
        cJSON_AddStringToObject(e, "created_at", ts);
        cJSON_AddNumberToObject(e, "order_count", (double)orders);
        cJSON_AddNumberToObject(e, "total_spent_cents", (double)spent);
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

/* Enable/disable an account. Disabling also ends all of that user's sessions at once. An
 * administrator cannot change their own account (no accidental lock-out). */
int h_admin_user_patch(rctx *c) {
    uint64_t id;
    if (api_seg_id(c, 4, &id)) return c->sent;
    cJSON *b = api_body(c);
    if (!b) return c->sent;
    int active;
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(b, "is_active");
    if (!f || get_bool(f, &active)) { api_err(c, 422, "VALIDATION_FAILED", "is_active must be true or false.", "is_active"); goto out; }
    if (id == c->user.user_id) { api_err(c, 409, "SELF_MODIFY", "You cannot change your own account here.", NULL); goto out; }
    if (db_begin(c->db)) { db_down(c); goto out; }
    int64_t n = db_exec(c->db, "UPDATE users SET is_active = ? WHERE id = ?", "iu", (int64_t)active, id);
    if (n < 0) { db_rollback(c->db); db_down(c); goto out; }
    db_rs *rs = db_query(c->db, "SELECT COUNT(*) FROM users WHERE id = ?", "u", id);
    uint64_t exists = 0;
    if (!rs) { db_rollback(c->db); db_down(c); goto out; }
    db_rs_next(rs, "u", &exists);
    db_rs_free(rs);
    if (!exists) { db_rollback(c->db); api_err(c, 404, "NOT_FOUND", "No such user.", NULL); goto out; }
    if (!active && db_exec(c->db, "DELETE FROM sessions WHERE user_id = ?", "u", id) < 0) { db_rollback(c->db); db_down(c); goto out; }
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)id);
    cJSON *det = cJSON_CreateObject();
    cJSON_AddBoolToObject(det, "is_active", active);
    int ar = audit_log(c->db, c->rid, c->user.user_id, active ? "user.enabled" : "user.disabled", "user", eid, det);
    cJSON_Delete(det);
    if (ar || db_commit(c->db)) { db_rollback(c->db); db_down(c); goto out; }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", (double)id);
    cJSON_AddBoolToObject(r, "is_active", active);
    api_ok(c, 200, r);
out:
    cJSON_Delete(b);
    return c->sent;
}
