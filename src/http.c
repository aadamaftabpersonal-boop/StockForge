#include "http.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "util.h"

/* ---------------------------------------------------------------- reading */

/* Waits for readability until deadline; 0 ready, 1 timeout, -1 error. */
static int wait_readable(int fd, uint64_t deadline_ns) {
    for (;;) {
        uint64_t now = now_ns();
        if (now >= deadline_ns) return 1;
        int ms = (int)((deadline_ns - now) / 1000000ull) + 1;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, ms);
        if (r > 0) return 0;
        if (r == 0) return 1;
        if (errno != EINTR) return -1;
    }
}

const char *http_header(const http_req *r, const char *name, size_t *len) {
    size_t nl = strlen(name);
    for (size_t i = 0; i < r->num_headers; i++) {
        if (r->headers[i].name_len == nl && strncasecmp(r->headers[i].name, name, nl) == 0) {
            if (len) *len = r->headers[i].value_len;
            return r->headers[i].value;
        }
    }
    return NULL;
}

/* Strict Content-Length: digits only, no sign/space, single header, bounded.
 * Anything ambiguous is rejected (request-smuggling defence). */
static http_rc parse_content_length(const http_req *r, size_t max_body, size_t *out) {
    int seen = 0;
    size_t cl = 0;
    for (size_t i = 0; i < r->num_headers; i++) {
        const struct phr_header *h = &r->headers[i];
        if (h->name_len == 14 && strncasecmp(h->name, "content-length", 14) == 0) {
            if (seen++) return HTTP_ERR_BAD;
            if (h->value_len == 0 || h->value_len > 9) return h->value_len > 9 ? HTTP_ERR_BODY_TOO_BIG : HTTP_ERR_BAD;
            cl = 0;
            for (size_t k = 0; k < h->value_len; k++) {
                if (h->value[k] < '0' || h->value[k] > '9') return HTTP_ERR_BAD;
                cl = cl * 10 + (size_t)(h->value[k] - '0');
            }
        } else if (h->name_len == 17 && strncasecmp(h->name, "transfer-encoding", 17) == 0) {
            return HTTP_ERR_UNSUPPORTED;
        }
    }
    if (cl > max_body) return HTTP_ERR_BODY_TOO_BIG;
    *out = cl;
    return HTTP_OK;
}

static int parse(http_req *r, size_t len, size_t prev, int *minor) {
    const char *m, *p;
    size_t ml, pl;
    r->num_headers = HTTP_MAX_HEADERS;
    int ret = phr_parse_request(r->buf, len, &m, &ml, &p, &pl, minor, r->headers,
                                &r->num_headers, prev);
    if (ret > 0) {
        r->method = m; r->method_len = ml;
        const char *q = memchr(p, '?', pl);
        r->path = p;
        r->path_len = q ? (size_t)(q - p) : pl;
        r->query = q ? q + 1 : p + pl;
        r->query_len = q ? pl - r->path_len - 1 : 0;
    }
    return ret;
}

http_rc http_read_request(int fd, http_req *r, const http_limits *lim) {
    memset(r, 0, sizeof *r);
    uint64_t deadline = now_ns() + (uint64_t)lim->deadline_ms * 1000000ull;
    size_t cap = lim->max_header, len = 0, prev = 0;
    int hdr_len = 0, minor = 0;

    r->buf = malloc(cap + 1);
    if (!r->buf) return HTTP_ERR_IO;

    /* Phase 1: read until the header block is complete or a limit trips. */
    for (;;) {
        if (len == cap) { http_req_free(r); return HTTP_ERR_HEADER_TOO_BIG; }
        int w = wait_readable(fd, deadline);
        if (w) { http_req_free(r); return w > 0 ? HTTP_ERR_TIMEOUT : HTTP_ERR_IO; }
        ssize_t n = recv(fd, r->buf + len, cap - len, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            http_req_free(r);
            return HTTP_ERR_IO;
        }
        if (n == 0) { http_req_free(r); return HTTP_ERR_CLOSED; }
        prev = len;
        len += (size_t)n;
        hdr_len = parse(r, len, prev, &minor);
        if (hdr_len > 0) break;
        if (hdr_len == -1) { http_req_free(r); return HTTP_ERR_BAD; }
        /* -2: incomplete, keep reading */
    }

    size_t cl = 0;
    http_rc rc = parse_content_length(r, lim->max_body, &cl);
    if (rc != HTTP_OK) { http_req_free(r); return rc; }

    /* Phase 2: body. Buffer is reallocated, so pointers are rebuilt by re-parsing. */
    size_t total = (size_t)hdr_len + cl;
    if (total + 1 > cap + 1) {
        char *nb = realloc(r->buf, total + 1);
        if (!nb) { http_req_free(r); return HTTP_ERR_IO; }
        r->buf = nb;
        cap = total;
    }
    while (len < total) {
        int w = wait_readable(fd, deadline);
        if (w) { http_req_free(r); return w > 0 ? HTTP_ERR_TIMEOUT : HTTP_ERR_IO; }
        ssize_t n = recv(fd, r->buf + len, total - len, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            http_req_free(r);
            return HTTP_ERR_IO;
        }
        if (n == 0) { http_req_free(r); return HTTP_ERR_CLOSED; }
        len += (size_t)n;
    }
    if (parse(r, (size_t)hdr_len, 0, &minor) <= 0) { http_req_free(r); return HTTP_ERR_BAD; }
    r->body = r->buf + hdr_len;
    r->body_len = cl;
    r->buf[total] = '\0';   /* bytes past `total` (pipelined data) are discarded */
    return HTTP_OK;
}

void http_req_free(http_req *r) {
    free(r->buf);
    r->buf = NULL;
}

int http_status_for(http_rc rc) {
    switch (rc) {
    case HTTP_ERR_TIMEOUT: return 408;
    case HTTP_ERR_BAD: return 400;
    case HTTP_ERR_HEADER_TOO_BIG: return 431;
    case HTTP_ERR_BODY_TOO_BIG: return 413;
    case HTTP_ERR_UNSUPPORTED: return 501;
    default: return 0;
    }
}

/* ---------------------------------------------------------------- writing */

static const char *reason(int s) {
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 415: return "Unsupported Media Type";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

static int write_all(int fd, struct iovec *iov, int cnt) {
    while (cnt > 0) {
        ssize_t n = writev(fd, iov, cnt);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;      /* EPIPE/ECONNRESET/EAGAIN(send timeout): give up */
        }
        while (cnt > 0 && (size_t)n >= iov->iov_len) {
            n -= (ssize_t)iov->iov_len; iov++; cnt--;
        }
        if (cnt > 0) { iov->iov_base = (char *)iov->iov_base + n; iov->iov_len -= (size_t)n; }
    }
    return 0;
}

int http_send(int fd, int status, const char *ctype, const char *body, size_t len,
              const char *rid, const char *extra) {
    char head[1536];
    /* API responses are data, never documents: forbid framing, sniffing, referrer
     * leaks and any active content if a response were ever rendered by a browser.
     * (HSTS is set by Apache, the TLS boundary.) */
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "X-Request-Id: %s\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n"
        "X-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n"
        "Connection: close\r\n%s\r\n",
        status, reason(status), ctype, len, rid, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof head) return -1;
    struct iovec iov[2] = {{head, (size_t)n}, {(void *)body, len}};
    return write_all(fd, iov, len ? 2 : 1);
}

int http_send_error(int fd, int status, const char *code, const char *message, const char *rid) {
    char body[400];
    /* code/message are compile-time literals: no escaping needed, and no user
     * data (SQL text, paths, stack traces) can ever leak through this path. */
    int n = snprintf(body, sizeof body,
        "{\"error\":{\"code\":\"%s\",\"message\":\"%s\",\"request_id\":\"%s\"}}", code, message, rid);
    return http_send(fd, status, "application/json", body, (size_t)n, rid,
                     status == 503 ? "Retry-After: 1\r\n" : NULL);
}

void http_lingering_close(int fd, int linger_ms) {
    char sink[4096];
    size_t drained = 0;
    uint64_t deadline = now_ns() + (uint64_t)linger_ms * 1000000ull;
    shutdown(fd, SHUT_WR);
    for (;;) {
        ssize_t n = recv(fd, sink, sizeof sink, MSG_DONTWAIT);
        if (n > 0) { drained += (size_t)n; if (drained > (1u << 20)) break; continue; }
        if (n == 0) break;                                   /* client saw our FIN and closed */
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        if (linger_ms == 0 || wait_readable(fd, deadline) != 0) break;
    }
    close(fd);
}

void http_new_request_id(char out[17]) {
    unsigned char b[8];
    static const char hex[] = "0123456789abcdef";
    if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b) memset(b, 0, sizeof b);
    for (int i = 0; i < 8; i++) { out[2 * i] = hex[b[i] >> 4]; out[2 * i + 1] = hex[b[i] & 15]; }
    out[16] = 0;
}
