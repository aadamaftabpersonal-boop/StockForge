/* Operational endpoints: health, metrics, debug. */
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "api.h"
#include "metrics.h"
#include "util.h"

int h_health(rctx *c) {
    /* Readiness, not just liveness: a server that cannot reach MySQL cannot
     * serve the store, so report 503 and let the proxy/orchestrator react. */
    int db_ok = c->db != NULL && db_ping(c->db) == 0;
    uint64_t up = (now_ns() - c->start_ns) / 1000000000ull;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", db_ok ? "ok" : "degraded");
    cJSON_AddStringToObject(r, "database", db_ok ? "up" : "down");
    cJSON_AddStringToObject(r, "version", "0.4.0");
    cJSON_AddNumberToObject(r, "uptime_s", (double)up);
    cJSON_AddNumberToObject(r, "workers", c->cfg->workers);
    cJSON_AddNumberToObject(r, "queue_depth", (double)fdq_depth(c->queue));
    cJSON_AddNumberToObject(r, "queue_capacity", c->cfg->queue_cap);
    c->sent = resp_json(c->fd, db_ok ? 200 : 503, c->rid, r, db_ok ? NULL : "Retry-After: 1\r\n");
    return c->sent;
}

/* /metrics: either `Authorization: Bearer <SF_METRICS_TOKEN>` (for scrapers) or
 * an admin session (for the console). Constant-time token comparison. */
int h_metrics(rctx *c) {
    int allowed = 0;
    size_t vl;
    const char *auth = http_header(c->req, "authorization", &vl);
    const char *tok = c->cfg->metrics_token;
    size_t tl = strlen(tok);
    if (tl && auth && vl == 7 + tl && strncasecmp(auth, "Bearer ", 7) == 0 &&
        sodium_memcmp(auth + 7, tok, tl) == 0)
        allowed = 1;
    if (!allowed && c->db) {
        int s = api_session(c);
        allowed = s == 1 && c->user.is_admin;
    }
    if (!allowed) return api_err(c, 401, "UNAUTHENTICATED", "Credentials required.", NULL);
    char b[3072];
    size_t n = metrics_render(b, sizeof b, fdq_depth(c->queue), fdq_high_water(c->queue),
                              (size_t)c->cfg->queue_cap, c->cfg->workers);
    http_send(c->fd, 200, "application/json", b, n, c->rid, NULL);
    c->sent = 200;
    return 200;
}

/* Debug only (SF_DEBUG=1): holds a worker for N ms. Used by tests to saturate
 * the pool and to prove graceful shutdown. */
int h_debug_sleep(rctx *c) {
    long ms = 0;
    char q[32] = {0};
    size_t n = c->req->query_len < sizeof q - 1 ? c->req->query_len : sizeof q - 1;
    memcpy(q, c->req->query, n);
    if (strncmp(q, "ms=", 3) == 0) ms = strtol(q + 3, NULL, 10);
    if (ms < 0) ms = 0;
    if (ms > 10000) ms = 10000;
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {}
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "slept", 1);
    return api_ok(c, 200, r);
}
