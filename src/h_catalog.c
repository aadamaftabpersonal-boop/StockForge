/* Public catalog: categories, product list (search/filter/sort/keyset), product detail. */
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "validate.h"

#define PRODUCT_COLS                                                               \
    "p.id, p.sku, p.name, p.description, p.price_cents, p.currency, p.image_url, " \
    "c.id, c.slug, c.name, CAST(i.on_hand AS SIGNED) - CAST(i.reserved AS SIGNED), " \
    "i.low_stock_threshold "
#define PRODUCT_FROM                                                               \
    "FROM products p JOIN categories c ON c.id = p.category_id "                   \
    "JOIN inventory i ON i.product_id = p.id "

typedef struct {
    uint64_t id, price, cat_id, threshold;
    int64_t  available;
    char     sku[48], name[208], desc[4100], currency[8], image[512], slug[88], cat_name[128];
} prow;

static int fetch_product(db_rs *rs, prow *r) {
    return db_rs_next(rs, "usssussussiu", &r->id, r->sku, sizeof r->sku, r->name, sizeof r->name,
                      r->desc, sizeof r->desc, &r->price, r->currency, sizeof r->currency,
                      r->image, sizeof r->image, &r->cat_id, r->slug, sizeof r->slug,
                      r->cat_name, sizeof r->cat_name, &r->available, &r->threshold);
}

static cJSON *product_json(const prow *r) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double)r->id);
    cJSON_AddStringToObject(o, "sku", r->sku);
    cJSON_AddStringToObject(o, "name", r->name);
    cJSON_AddStringToObject(o, "description", r->desc);
    cJSON_AddNumberToObject(o, "price_cents", (double)r->price);
    cJSON_AddStringToObject(o, "currency", r->currency);
    if (r->image[0]) cJSON_AddStringToObject(o, "image_url", r->image);
    else cJSON_AddNullToObject(o, "image_url");
    cJSON *cat = cJSON_AddObjectToObject(o, "category");
    cJSON_AddNumberToObject(cat, "id", (double)r->cat_id);
    cJSON_AddStringToObject(cat, "slug", r->slug);
    cJSON_AddStringToObject(cat, "name", r->cat_name);
    int64_t av = r->available < 0 ? 0 : r->available;
    cJSON_AddNumberToObject(o, "available", (double)av);
    cJSON_AddStringToObject(o, "stock_status",
                            av == 0 ? "out_of_stock" : (uint64_t)av <= r->threshold ? "low_stock" : "in_stock");
    return o;
}

int h_categories(rctx *c) {
    db_rs *rs = db_query(c->db,
        "SELECT c.id, c.slug, c.name, "
        "(SELECT COUNT(*) FROM products p WHERE p.category_id = c.id AND p.is_active = 1) "
        "FROM categories c ORDER BY c.name", "");
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "categories");
    uint64_t id, n;
    char slug[88], name[128];
    while (db_rs_next(rs, "ussu", &id, slug, sizeof slug, name, sizeof name, &n) == 1) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)id);
        cJSON_AddStringToObject(o, "slug", slug);
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddNumberToObject(o, "product_count", (double)n);
        cJSON_AddItemToArray(arr, o);
    }
    db_rs_free(rs);
    return api_ok(c, 200, root);
}

enum { SORT_NEWEST, SORT_PRICE_ASC, SORT_PRICE_DESC };

int h_products(rctx *c) {
    char sort_s[16], cat[96], q[256], cur[48], ft[320];
    int limit, sort = SORT_NEWEST, has_cat = 0, has_q = 0;
    uint64_t ca = 0, cb = 0;
    int ncur = 0, r;

    if (api_limit(c, 20, 50, &limit)) return c->sent;

    r = api_qs(c, "sort", sort_s, sizeof sort_s);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid sort.", "sort");
    if (r == 1) {
        if (!strcmp(sort_s, "newest")) sort = SORT_NEWEST;
        else if (!strcmp(sort_s, "price_asc")) sort = SORT_PRICE_ASC;
        else if (!strcmp(sort_s, "price_desc")) sort = SORT_PRICE_DESC;
        else return api_err(c, 422, "VALIDATION_FAILED", "sort must be newest, price_asc or price_desc.", "sort");
    }

    r = api_qs(c, "category", cat, sizeof cat);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid category.", "category");
    if (r == 1 && cat[0]) {
        for (const char *p = cat; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
                return api_err(c, 422, "VALIDATION_FAILED", "Invalid category.", "category");
        has_cat = 1;
    }

    r = api_qs(c, "q", q, sizeof q);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid search text.", "q");
    if (r == 1 && q[0]) {
        if (!search_to_boolean(q, ft, sizeof ft))
            return api_err(c, 422, "VALIDATION_FAILED", "Search needs at least one word of 3 or more letters.", "q");
        has_q = 1;
    }

    r = api_qs(c, "after", cur, sizeof cur);
    if (r < 0) return api_err(c, 422, "VALIDATION_FAILED", "Invalid cursor.", "after");
    if (r == 1 && cur[0]) {
        ncur = cursor_parse(cur, &ca, &cb);
        if (ncur != (sort == SORT_NEWEST ? 1 : 2))
            return api_err(c, 422, "VALIDATION_FAILED", "Invalid cursor.", "after");
    }

    /* SQL is assembled ONLY from constant fragments; every value is a `?`. */
    char sql[1400];
    size_t o = 0;
    db_params p = {.n = 0};
#define ADD(s) (o += (size_t)snprintf(sql + o, sizeof sql - o, "%s", (s)))
    ADD("SELECT " PRODUCT_COLS PRODUCT_FROM "WHERE p.is_active = 1 ");
    if (has_cat) { ADD("AND c.slug = ? "); dbp_s(&p, cat); }
    if (has_q) { ADD("AND MATCH(p.name, p.description) AGAINST (? IN BOOLEAN MODE) "); dbp_s(&p, ft); }
    if (ncur) {
        if (sort == SORT_NEWEST) { ADD("AND p.id < ? "); dbp_u(&p, ca); }
        else if (sort == SORT_PRICE_ASC) {
            ADD("AND (p.price_cents > ? OR (p.price_cents = ? AND p.id > ?)) ");
            dbp_u(&p, ca); dbp_u(&p, ca); dbp_u(&p, cb);
        } else {
            ADD("AND (p.price_cents < ? OR (p.price_cents = ? AND p.id < ?)) ");
            dbp_u(&p, ca); dbp_u(&p, ca); dbp_u(&p, cb);
        }
    }
    ADD(sort == SORT_NEWEST ? "ORDER BY p.id DESC "
        : sort == SORT_PRICE_ASC ? "ORDER BY p.price_cents ASC, p.id ASC "
                                 : "ORDER BY p.price_cents DESC, p.id DESC ");
    ADD("LIMIT ?");
    dbp_i(&p, limit + 1);                 /* one extra row tells us whether another page exists */
#undef ADD

    db_rs *rs = db_query_p(c->db, sql, &p);
    if (!rs) {
        /* 1064: MySQL's full-text parser rejected the search expression. That is a problem with the
         * client's text, not an outage, so say so (the tokenizer should make this unreachable). */
        if (has_q && db_errno(c->db) == 1064)
            return api_err(c, 422, "VALIDATION_FAILED", "Search text contains unsupported characters.", "q");
        return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    }

    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "items");
    prow row, last = {0};
    int n = 0, more = 0;
    while (db_rs_next(rs, "usssussussiu", &row.id, row.sku, sizeof row.sku, row.name, sizeof row.name,
                      row.desc, sizeof row.desc, &row.price, row.currency, sizeof row.currency,
                      row.image, sizeof row.image, &row.cat_id, row.slug, sizeof row.slug,
                      row.cat_name, sizeof row.cat_name, &row.available, &row.threshold) == 1) {
        if (n == limit) { more = 1; break; }
        cJSON_AddItemToArray(arr, product_json(&row));
        last.id = row.id;
        last.price = row.price;
        n++;
    }
    db_rs_free(rs);
    if (more) {
        char nc[48];
        if (sort == SORT_NEWEST) snprintf(nc, sizeof nc, "%llu", (unsigned long long)last.id);
        else snprintf(nc, sizeof nc, "%llu.%llu", (unsigned long long)last.price, (unsigned long long)last.id);
        cJSON_AddStringToObject(root, "next_cursor", nc);
    } else cJSON_AddNullToObject(root, "next_cursor");
    return api_ok(c, 200, root);
}

int h_product(rctx *c) {
    uint64_t id;
    if (api_seg_id(c, 3, &id)) return c->sent;     /* /api/v1/products/{id} */
    db_rs *rs = db_query(c->db,
        "SELECT " PRODUCT_COLS PRODUCT_FROM "WHERE p.id = ? AND p.is_active = 1", "u", id);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    prow row;
    int f = fetch_product(rs, &row);
    db_rs_free(rs);
    if (f < 0) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    if (f == 0) return api_err(c, 404, "NOT_FOUND", "No such product.", NULL);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "product", product_json(&row));
    return api_ok(c, 200, root);
}
