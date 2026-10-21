#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "testutil.h"

/* Feeds `raw` through a socketpair into http_read_request. If close_after is
 * set the writer end is closed (models a client that disconnects). */
static http_rc run(const char *raw, size_t n, int close_after, http_req *r, int deadline_ms) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (n) { ssize_t w = write(sv[0], raw, n); (void)w; }
    if (close_after) close(sv[0]);
    http_limits lim = {.max_header = 1024, .max_body = 100, .deadline_ms = deadline_ms};
    http_rc rc = http_read_request(sv[1], r, &lim);
    close(sv[1]);
    if (!close_after) close(sv[0]);
    return rc;
}
#define RUN(s, close_after, r) run(s, strlen(s), close_after, r, 300)

static void test_valid(void) {
    http_req r;
    CHECK(RUN("GET /healthz?x=1 HTTP/1.1\r\nHost: a\r\nX-Foo: bar\r\n\r\n", 0, &r) == HTTP_OK);
    CHECK(r.method_len == 3 && memcmp(r.method, "GET", 3) == 0);
    CHECK(r.path_len == 8 && memcmp(r.path, "/healthz", 8) == 0);
    CHECK(r.query_len == 3 && memcmp(r.query, "x=1", 3) == 0);
    size_t l;
    const char *v = http_header(&r, "x-FOO", &l);       /* case-insensitive */
    CHECK(v && l == 3 && memcmp(v, "bar", 3) == 0);
    CHECK(http_header(&r, "missing", &l) == NULL);
    http_req_free(&r);

    CHECK(RUN("POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello", 0, &r) == HTTP_OK);
    CHECK(r.body_len == 5 && memcmp(r.body, "hello", 5) == 0 && r.body[5] == 0);
    CHECK(r.path_len == 2 && r.query_len == 0);
    http_req_free(&r);
}

static void test_rejections(void) {
    http_req r;
    CHECK(RUN("GARBAGE\r\n\r\n", 0, &r) == HTTP_ERR_BAD);
    CHECK(RUN("GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", 0, &r) == HTTP_ERR_BAD);   /* smuggling */
    CHECK(RUN("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", 0, &r) == HTTP_ERR_BAD);
    CHECK(RUN("POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", 0, &r) == HTTP_ERR_BAD);
    CHECK(RUN("POST / HTTP/1.1\r\nContent-Length: 101\r\n\r\n", 0, &r) == HTTP_ERR_BODY_TOO_BIG);
    CHECK(RUN("POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n", 0, &r) == HTTP_ERR_BODY_TOO_BIG);
    CHECK(RUN("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 0, &r) == HTTP_ERR_UNSUPPORTED);

    char big[2000];
    memset(big, 'a', sizeof big);
    memcpy(big, "GET /", 5);                            /* never terminates headers */
    CHECK(run(big, sizeof big, 0, &r, 300) == HTTP_ERR_HEADER_TOO_BIG);
}

static void test_disconnect_and_timeout(void) {
    http_req r;
    CHECK(RUN("GET /heal", 1, &r) == HTTP_ERR_CLOSED);                       /* hung up mid-headers */
    CHECK(RUN("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", 1, &r) == HTTP_ERR_CLOSED); /* mid-body */
    CHECK(RUN("", 1, &r) == HTTP_ERR_CLOSED);                                /* connected, sent nothing */
    CHECK(RUN("GET / HTTP/1.1\r\nHost: slow", 0, &r) == HTTP_ERR_TIMEOUT);   /* slowloris */
    CHECK(RUN("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", 0, &r) == HTTP_ERR_TIMEOUT);
    CHECK(http_status_for(HTTP_ERR_TIMEOUT) == 408);
    CHECK(http_status_for(HTTP_ERR_CLOSED) == 0);                            /* nobody to answer */
}

int main(void) {
    test_valid();
    test_rejections();
    test_disconnect_and_timeout();
    TEST_MAIN_END();
}
