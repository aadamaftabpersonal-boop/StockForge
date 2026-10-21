#include "router.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "api.h"
#include "validate.h"

enum { A_NONE, A_USER, A_ADMIN };

typedef struct {
    const char *method;
    const char *pattern;     /* ":x" matches any one non-empty segment */
    handler_fn  fn;
    int         auth;
    int         debug_only;
} route;

static const route ROUTES[] = {
    {"GET",    "/healthz",                            h_health,               A_NONE,  0},
    {"GET",    "/metrics",                            h_metrics,              A_NONE,  0},
    {"GET",    "/debug/sleep",                        h_debug_sleep,          A_NONE,  1},

    {"POST",   "/api/v1/auth/register",               h_register,             A_NONE,  0},
    {"POST",   "/api/v1/auth/login",                  h_login,                A_NONE,  0},
    {"POST",   "/api/v1/auth/logout",                 h_logout,               A_USER,  0},
    {"GET",    "/api/v1/auth/me",                     h_me,                   A_USER,  0},

    {"GET",    "/api/v1/categories",                  h_categories,           A_NONE,  0},
    {"GET",    "/api/v1/products",                    h_products,             A_NONE,  0},
    {"GET",    "/api/v1/products/:id",                h_product,              A_NONE,  0},

    {"GET",    "/api/v1/cart",                        h_cart_get,             A_USER,  0},
    {"DELETE", "/api/v1/cart",                        h_cart_clear,           A_USER,  0},
    {"PUT",    "/api/v1/cart/items/:id",              h_cart_put,             A_USER,  0},
    {"DELETE", "/api/v1/cart/items/:id",              h_cart_delete_item,     A_USER,  0},

    {"GET",    "/api/v1/orders",                      h_orders,               A_USER,  0},
    {"POST",   "/api/v1/orders",                      h_checkout,             A_USER,  0},
    {"GET",    "/api/v1/orders/:id",                  h_order,                A_USER,  0},
    {"POST",   "/api/v1/orders/:id/pay",              h_pay,                  A_USER,  0},
    {"POST",   "/api/v1/orders/:id/cancel",           h_cancel,               A_USER,  0},

    {"POST",   "/api/v1/admin/products",              h_admin_product_create, A_ADMIN, 0},
    {"POST",   "/api/v1/admin/inventory/:id/adjust",  h_admin_stock_adjust,   A_ADMIN, 0},
    {"GET",    "/api/v1/admin/audit",                 h_admin_audit,          A_ADMIN, 0},
    {"GET",    "/api/v1/admin/reconcile",             h_admin_reconcile,      A_ADMIN, 0},
};

static int split(const char *p, size_t n, const char **seg, size_t *len, int max) {
    int k = 0;
    size_t i = 1;                                  /* skip leading '/' */
    if (n == 0 || p[0] != '/') return -1;
    for (;;) {
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        if (j == i) return -1;                     /* empty segment: "//" or trailing "/" */
        if (k == max) return -1;
        seg[k] = p + i;
        len[k++] = j - i;
        if (j >= n) return k;
        i = j + 1;
    }
}

static int pattern_match(const char *pat, const char **seg, const size_t *len, int n) {
    const char *ps[API_MAX_SEGS];
    size_t pl[API_MAX_SEGS];
    int pn = split(pat, strlen(pat), ps, pl, API_MAX_SEGS);
    if (pn != n) return 0;
    for (int i = 0; i < n; i++) {
        if (ps[i][0] == ':') continue;
        if (pl[i] != len[i] || memcmp(ps[i], seg[i], len[i]) != 0) return 0;
    }
    return 1;
}

static void client_ip(rctx *c, const char *peer) {
    snprintf(c->client_ip, sizeof c->client_ip, "%s", peer);
    if (!c->cfg->trust_proxy) return;
    /* Apache APPENDS the connecting address to X-Forwarded-For, so the LAST
     * entry is the one our own proxy observed; earlier entries are client-supplied. */
    size_t l;
    const char *x = http_header(c->req, "x-forwarded-for", &l);
    if (!x || !l) return;
    size_t s = l;
    while (s > 0 && x[s - 1] != ',') s--;
    while (s < l && x[s] == ' ') s++;
    size_t n = l - s;
    if (n == 0 || n >= sizeof c->client_ip) return;
    for (size_t i = 0; i < n; i++)
        if (!(isxdigit((unsigned char)x[s + i]) || x[s + i] == '.' || x[s + i] == ':')) return;
    memcpy(c->client_ip, x + s, n);
    c->client_ip[n] = 0;
}

static int is_unsafe(const http_req *r) {
    return !(r->method_len == 3 && memcmp(r->method, "GET", 3) == 0);
}

int router_handle(const router_ctx *rc, int fd, const http_req *req, const char *rid,
                  const char *peer) {
    rctx c;
    memset(&c, 0, sizeof c);
    c.fd = fd; c.req = req; c.rid = rid; c.cfg = rc->cfg; c.queue = rc->queue; c.start_ns = rc->start_ns;
    client_ip(&c, peer);

    c.nseg = split(req->path, req->path_len, c.seg, c.seglen, API_MAX_SEGS);
    if (c.nseg < 0) return api_err(&c, 404, "NOT_FOUND", "No such route.", NULL);

    const route *hit = NULL;
    int path_seen = 0;
    for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
        const route *r = &ROUTES[i];
        if (r->debug_only && !rc->cfg->debug) continue;
        if (!pattern_match(r->pattern, c.seg, c.seglen, c.nseg)) continue;
        path_seen = 1;
        if (strlen(r->method) == req->method_len && memcmp(r->method, req->method, req->method_len) == 0) {
            hit = r;
            break;
        }
    }
    if (!hit) {
        if (path_seen) return api_err(&c, 405, "METHOD_NOT_ALLOWED", "Method not allowed for this route.", NULL);
        return api_err(&c, 404, "NOT_FOUND", "No such route.", NULL);
    }

    int unsafe = is_unsafe(req);
    if (unsafe) {
        /* Browsers always send Origin on cross-site unsafe requests. If it is
         * present it must be our own host; absent (curl, same-origin forms) is allowed. */
        size_t ol, hl;
        const char *o = http_header(req, "origin", &ol), *h = http_header(req, "host", &hl);
        if (o && h && !origin_matches_host(o, ol, h, hl))
            return api_err(&c, 403, "ORIGIN_MISMATCH", "Cross-origin request rejected.", NULL);
        /* Requiring application/json forces a CORS preflight for cross-site pages. */
        if (req->body_len > 0) {
            size_t cl;
            const char *ct = http_header(req, "content-type", &cl);
            if (!ct || cl < 16 || strncasecmp(ct, "application/json", 16) != 0)
                return api_err(&c, 415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json.", NULL);
        }
    }

    c.db = db_get();
    if (!c.db && hit->fn != h_health)
        return api_err(&c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);

    if (hit->auth != A_NONE) {
        int s = api_session(&c);
        if (s < 0) return api_err(&c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
        if (s == 0) return api_err(&c, 401, "UNAUTHENTICATED", "Sign in required.", NULL);
        if (hit->auth == A_ADMIN && !c.user.is_admin)
            return api_err(&c, 403, "FORBIDDEN", "Administrator access required.", NULL);
        if (unsafe && !auth_csrf_ok(&c.user, req))
            return api_err(&c, 403, "CSRF_INVALID", "Missing or invalid CSRF token.", NULL);
    }
    return hit->fn(&c);
}
