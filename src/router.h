#ifndef SF_ROUTER_H
#define SF_ROUTER_H

#include "config.h"
#include "http.h"
#include "queue.h"

typedef struct {
    const config *cfg;
    fdqueue      *queue;
    uint64_t      start_ns;
} router_ctx;

/* Dispatches one parsed request and writes exactly one response to fd.
 * Returns the HTTP status that was sent. */
int router_handle(const router_ctx *ctx, int fd, const http_req *req, const char *request_id);

#endif
