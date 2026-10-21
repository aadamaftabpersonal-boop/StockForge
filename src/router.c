#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "metrics.h"
#include "util.h"

#define VERSION "0.1.0"

static int path_is(const http_req *r, const char *p) {
    size_t n = strlen(p);
    return r->path_len == n && memcmp(r->path, p, n) == 0;
}

static int method_is(const http_req *r, const char *m) {
    size_t n = strlen(m);
    return r->method_len == n && memcmp(r->method, m, n) == 0;
}

static int send_json(int fd, int status, const char *rid, const char *body, size_t n) {
    http_send(fd, status, "application/json", body, n, rid, NULL);
    return status;
}

static int h_health(const router_ctx *c, int fd, const char *rid) {
    char b[256];
    uint64_t up = (now_ns() - c->start_ns) / 1000000000ull;
    int n = snprintf(b, sizeof b,
        "{\"status\":\"ok\",\"version\":\"" VERSION "\",\"uptime_s\":%llu,"
        "\"workers\":%d,\"queue_depth\":%zu,\"queue_capacity\":%d}",
        (unsigned long long)up, c->cfg->workers, fdq_depth(c->queue), c->cfg->queue_cap);
    return send_json(fd, 200, rid, b, (size_t)n);
}

static int h_metrics(const router_ctx *c, int fd, const char *rid) {
    char b[2048];
    size_t n = metrics_render(b, sizeof b, fdq_depth(c->queue), fdq_high_water(c->queue),
                              (size_t)c->cfg->queue_cap, c->cfg->workers);
    return send_json(fd, 200, rid, b, n);
}

/* Debug only (SF_DEBUG=1): GET /debug/sleep?ms=N holds a worker for N ms.
 * Used by tests to saturate the pool and to prove graceful shutdown. */
static int h_debug_sleep(int fd, const http_req *r, const char *rid) {
    long ms = 0;
    char q[32] = {0};
    memcpy(q, r->query, r->query_len < sizeof q - 1 ? r->query_len : sizeof q - 1);
    if (strncmp(q, "ms=", 3) == 0) ms = strtol(q + 3, NULL, 10);
    if (ms < 0) ms = 0;
    if (ms > 10000) ms = 10000;
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {}
    return send_json(fd, 200, rid, "{\"slept\":true}", 14);
}

int router_handle(const router_ctx *c, int fd, const http_req *r, const char *rid) {
    int get = method_is(r, "GET");
    if (path_is(r, "/healthz")) {
        if (!get) goto bad_method;
        return h_health(c, fd, rid);
    }
    if (path_is(r, "/metrics")) {          /* TODO(phase 4): admin-only once auth exists */
        if (!get) goto bad_method;
        return h_metrics(c, fd, rid);
    }
    if (c->cfg->debug && path_is(r, "/debug/sleep")) {
        if (!get) goto bad_method;
        return h_debug_sleep(fd, r, rid);
    }
    http_send_error(fd, 404, "NOT_FOUND", "No such route.", rid);
    return 404;
bad_method:
    http_send_error(fd, 405, "METHOD_NOT_ALLOWED", "Method not allowed for this route.", rid);
    return 405;
}
