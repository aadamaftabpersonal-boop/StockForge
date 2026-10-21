#ifndef SF_API_H
#define SF_API_H

#include <stdint.h>

#include "auth.h"
#include "cJSON.h"
#include "config.h"
#include "db.h"
#include "http.h"
#include "queue.h"
#include "resp.h"

#define API_MAX_SEGS 8

/* Per-request context. Lives on the worker's stack; nothing here is shared. */
typedef struct rctx {
    int           fd;
    const http_req *req;
    const char   *rid;
    const config *cfg;
    fdqueue      *queue;
    uint64_t      start_ns;
    char          client_ip[48];
    db           *db;
    int           session_state;      /* 0 not loaded, 1 loaded */
    int           session_rc;         /* auth_load_session result */
    auth_user     user;
    const char   *seg[API_MAX_SEGS];  /* path segments, "/a/b/c" => a,b,c */
    size_t        seglen[API_MAX_SEGS];
    int           nseg;
    int           sent;               /* status of the response already written */
} rctx;

typedef int (*handler_fn)(rctx *c);

/* Loads the session once per request. 1 authenticated, 0 anonymous, -1 db error. */
int  api_session(rctx *c);

int  api_err(rctx *c, int status, const char *code, const char *msg, const char *field);
int  api_ok(rctx *c, int status, cJSON *obj);

/* Parses the body as a JSON object; on failure sends the error and returns NULL. */
cJSON *api_body(rctx *c);
/* Field extractors: 0 on success, -1 after sending a 422 naming the field. */
int  api_str(rctx *c, const cJSON *o, const char *name, const char **out, int required);
int  api_int(rctx *c, const cJSON *o, const char *name, int64_t lo, int64_t hi, int64_t *out);
/* Path segment as an id; -1 after sending 404-style error. */
int  api_seg_id(rctx *c, int idx, uint64_t *out);
/* ?limit=: default def, clamped to max; invalid => -1 after sending 422. */
int  api_limit(rctx *c, int def, int max, int *out);
/* Query parameter into out; same return convention as qs_get. */
int  api_qs(rctx *c, const char *key, char *out, size_t outn);

/* Handlers */
int h_health(rctx *c);
int h_metrics(rctx *c);
int h_debug_sleep(rctx *c);
int h_register(rctx *c);
int h_login(rctx *c);
int h_logout(rctx *c);
int h_me(rctx *c);
int h_categories(rctx *c);
int h_products(rctx *c);
int h_product(rctx *c);
int h_cart_get(rctx *c);
int h_cart_put(rctx *c);
int h_cart_delete_item(rctx *c);
int h_cart_clear(rctx *c);
int h_orders(rctx *c);
int h_order(rctx *c);
int h_admin_product_create(rctx *c);
int h_admin_stock_adjust(rctx *c);
int h_admin_audit(rctx *c);

#endif
