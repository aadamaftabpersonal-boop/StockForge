#ifndef SF_HTTP_H
#define SF_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "picohttpparser.h"

#define HTTP_MAX_HEADERS 64

typedef struct {
    size_t max_header;
    size_t max_body;
    int    deadline_ms;   /* total time allowed to receive the whole request */
} http_limits;

typedef struct {
    const char *method;  size_t method_len;
    const char *path;    size_t path_len;     /* excludes query */
    const char *query;   size_t query_len;    /* excludes '?'; may be empty */
    struct phr_header headers[HTTP_MAX_HEADERS];
    size_t num_headers;
    const char *body;    size_t body_len;     /* NUL-terminated copy lives in buf */
    char *buf;                                /* owns all the pointers above */
} http_req;

typedef enum {
    HTTP_OK = 0,
    HTTP_ERR_CLOSED,          /* peer closed before a full request: no response */
    HTTP_ERR_IO,              /* socket error: no response */
    HTTP_ERR_TIMEOUT,         /* -> 408 */
    HTTP_ERR_BAD,             /* -> 400 */
    HTTP_ERR_HEADER_TOO_BIG,  /* -> 431 */
    HTTP_ERR_BODY_TOO_BIG,    /* -> 413 */
    HTTP_ERR_UNSUPPORTED      /* -> 501 (Transfer-Encoding etc.) */
} http_rc;

/* Reads exactly one request from fd. On HTTP_OK the caller must http_req_free(). */
http_rc http_read_request(int fd, http_req *r, const http_limits *lim);
void    http_req_free(http_req *r);

/* Case-insensitive header lookup; NULL if absent. Value is NOT NUL-terminated:
 * *len receives its length. */
const char *http_header(const http_req *r, const char *name, size_t *len);

int http_status_for(http_rc rc);   /* error rc -> status; 0 if no response should be sent */

/* Responses always carry Connection: close, nosniff and no-store.
 * Return 0 on success, -1 if the peer went away. */
int http_send(int fd, int status, const char *ctype, const char *body, size_t len,
              const char *request_id, const char *extra_headers);
int http_send_error(int fd, int status, const char *code, const char *message,
                    const char *request_id);

/* Close a socket we may not have fully read. close() with unread bytes in the
 * receive queue makes Linux send RST, which can destroy the client's copy of
 * the response we just wrote. So: send FIN, then drain what the client sent.
 * linger_ms == 0 => non-blocking best-effort drain (used by the acceptor).
 * Otherwise bounded by linger_ms and 1 MiB. Always closes fd. */
void http_lingering_close(int fd, int linger_ms);

void http_new_request_id(char out[17]);

#endif
