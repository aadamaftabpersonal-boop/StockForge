#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "auth.h"
#include "db.h"
#include "http.h"
#include "log.h"
#include "metrics.h"
#include "queue.h"
#include "router.h"
#include "util.h"

typedef struct {
    const config *cfg;
    fdqueue       queue;
    router_ctx    rctx;
    int           wake_pipe[2];   /* signaller -> acceptor */
} server;

static void set_timeouts(int fd, int send_ms) {
    struct timeval tv = {.tv_sec = send_ms / 1000, .tv_usec = (send_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/* ------------------------------------------------------------ worker side */

static void serve_connection(server *s, const fdq_item *it) {
    int fd = it->fd;
    uint64_t t0 = now_ns();
    uint64_t qwait_us = (t0 - it->enq_ns) / 1000;
    char rid[17];
    http_new_request_id(rid);
    set_timeouts(fd, s->cfg->write_timeout_ms);

    http_limits lim = {.max_header = s->cfg->max_header,
                       .max_body = s->cfg->max_body,
                       .deadline_ms = s->cfg->read_timeout_ms};
    http_req req;
    http_rc rc = http_read_request(fd, &req, &lim);
    int status;
    if (rc == HTTP_OK) {
        char peer[48] = "unknown";
        struct sockaddr_in pa;
        socklen_t pl = sizeof pa;
        if (getpeername(fd, (struct sockaddr *)&pa, &pl) == 0 && pa.sin_family == AF_INET)
            inet_ntop(AF_INET, &pa.sin_addr, peer, sizeof peer);
        status = router_handle(&s->rctx, fd, &req, rid, peer);
        log_access(rid, req.method, req.method_len, req.path, req.path_len, status,
                   (now_ns() - t0) / 1000, qwait_us);
        http_req_free(&req);
    } else {
        status = http_status_for(rc);
        if (status) {
            const char *code = status == 408 ? "REQUEST_TIMEOUT"
                             : status == 413 ? "BODY_TOO_LARGE"
                             : status == 431 ? "HEADERS_TOO_LARGE"
                             : status == 501 ? "NOT_IMPLEMENTED" : "BAD_REQUEST";
            http_send_error(fd, status, code, "The request could not be processed.", rid);
            log_access(rid, "-", 1, "-", 1, status, (now_ns() - t0) / 1000, qwait_us);
        } else {
            log_msg("info", "client_gone", rc == HTTP_ERR_CLOSED ? "closed before full request"
                                                                 : "socket error while reading");
            status = 499;   /* nginx convention: client closed request; metrics only */
        }
    }
    metrics_observe(status, (now_ns() - t0) / 1000, qwait_us);
    /* Error paths may leave unread request bytes behind; linger so our error
     * response is not destroyed by an RST. Bounded at 200 ms per connection. */
    if (rc == HTTP_OK) close(fd);
    else http_lingering_close(fd, 200);
}

static void *worker_main(void *arg) {
    server *s = arg;
    fdq_item it;
    db_thread_init();                       /* this worker's MySQL connection is created lazily */
    while (fdq_pop(&s->queue, &it) == 0) {
        metrics_worker_busy(+1);
        serve_connection(s, &it);
        metrics_worker_busy(-1);
    }
    db_thread_end();
    return NULL;
}

/* ----------------------------------------------------------- signal thread */

static void *signal_main(void *arg) {
    server *s = arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int sig = 0;
    sigwait(&set, &sig);
    char c = (char)sig;
    ssize_t r = write(s->wake_pipe[1], &c, 1);   /* wakes poll() in the acceptor */
    (void)r;
    return NULL;
}

/* ----------------------------------------------------------------- acceptor */

static int open_listener(const config *cfg) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)cfg->port)};
    if (inet_pton(AF_INET, cfg->bind_addr, &a.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(fd, 128) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

static void reject_overloaded(int fd, const config *cfg) {
    char rid[17];
    http_new_request_id(rid);
    /* Acceptor must not block on a slow client: tiny send timeout, then drop. */
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    http_send_error(fd, 503, "OVERLOADED", "Server is busy. Retry shortly.", rid);
    (void)cfg;
    http_lingering_close(fd, 0);   /* non-blocking: the acceptor must never wait */
    metrics_conn_rejected_overload();
    log_msg("warn", "overload_reject", "queue full, connection rejected with 503");
}

int server_run(const config *cfg) {
    server s = {.cfg = cfg};
    char msg[160];

    /* Block SIGINT/SIGTERM in every thread; only the signaller consumes them.
     * SIGPIPE is ignored so a vanished client yields EPIPE, not process death. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (auth_global_init() || db_global_init(cfg)) {
        log_msg("error", "startup", "crypto/database library init failed");
        return 1;
    }
    if (fdq_init(&s.queue, (size_t)cfg->queue_cap)) { log_msg("error", "startup", "queue init failed"); return 1; }
    if (pipe2(s.wake_pipe, O_CLOEXEC)) { log_msg("error", "startup", "pipe2 failed"); return 1; }
    int lfd = open_listener(cfg);
    if (lfd < 0) {
        snprintf(msg, sizeof msg, "cannot listen on %s:%d: %s", cfg->bind_addr, cfg->port, strerror(errno));
        log_msg("error", "startup", msg);
        return 1;
    }
    s.rctx = (router_ctx){.cfg = cfg, .queue = &s.queue, .start_ns = now_ns()};

    pthread_t sigt;
    pthread_t *workers = calloc((size_t)cfg->workers, sizeof *workers);
    int started = 0;
    if (!workers) return 1;
    for (; started < cfg->workers; started++)
        if (pthread_create(&workers[started], NULL, worker_main, &s)) break;
    if (started != cfg->workers || pthread_create(&sigt, NULL, signal_main, &s)) {
        log_msg("error", "startup", "thread creation failed");
        fdq_close(&s.queue);
        for (int i = 0; i < started; i++) pthread_join(workers[i], NULL);
        return 1;
    }

    snprintf(msg, sizeof msg, "listening on %s:%d workers=%d queue=%d",
             cfg->bind_addr, cfg->port, cfg->workers, cfg->queue_cap);
    log_msg("info", "started", msg);

    struct pollfd pf[2] = {{.fd = lfd, .events = POLLIN}, {.fd = s.wake_pipe[0], .events = POLLIN}};
    for (;;) {
        int r = poll(pf, 2, -1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (pf[1].revents & POLLIN) break;               /* shutdown requested */
        if (!(pf[0].revents & POLLIN)) continue;
        int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EMFILE || errno == ENFILE) {    /* out of fds: back off, don't spin */
                log_msg("error", "accept", "file descriptor limit reached");
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
            }
            continue;                                    /* EINTR, ECONNABORTED, ... */
        }
        metrics_conn_accepted();
        if (fdq_try_push(&s.queue, cfd) != FDQ_OK) reject_overloaded(cfd, cfg);
    }

    log_msg("info", "shutdown", "signal received: draining");
    close(lfd);                    /* new connections are refused from here on */
    fdq_close(&s.queue);           /* workers finish queued + in-flight work, then exit */
    for (int i = 0; i < started; i++) pthread_join(workers[i], NULL);
    pthread_join(sigt, NULL);
    auth_global_free();
    fdq_destroy(&s.queue);
    close(s.wake_pipe[0]);
    close(s.wake_pipe[1]);
    free(workers);
    log_msg("info", "stopped", "clean shutdown");
    return 0;
}
