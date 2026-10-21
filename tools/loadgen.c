/* loadgen: closed-loop HTTP load generator for StockForge.
 *
 * N threads each act as one user: send a request, wait for the full response,
 * send the next. (Closed loop: offered load adapts to server speed, which is how
 * real browsers behave; it cannot overload the server beyond N in-flight requests.)
 * Latency is wall-clock from just before connect() to the server closing the
 * connection after its response (the server uses Connection: close, so every
 * request includes TCP setup; that is part of what is being measured).
 *
 *   loadgen --port 8080 --threads 64 --duration 10 --scenario browse --out r.json
 *   scenarios: healthz | browse | buy | mixed
 *   buy / mixed need --users FILE  (lines: "<session cookie> <csrf token>", one per user)
 *             and --products 1,2,3  (--hot K restricts buying to the first K of them)
 *
 * Output: one JSON object (see print_json). Nothing is estimated: percentiles are
 * computed from every recorded sample after warm-up. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { OP_HEALTHZ, OP_LIST, OP_CATEGORY, OP_SEARCH, OP_SEARCH_RARE, OP_DETAIL, OP_SORTED, OP_CART, OP_CHECKOUT, OP_PAY, OP_CANCEL, OP_COUNT };
static const char *OP_NAME[OP_COUNT] = {"healthz", "list_newest", "list_category", "search_common_word", "search_rare_token", "detail",
                                        "list_price_sorted", "cart_put", "checkout", "pay", "cancel"};

typedef struct { uint8_t op; uint16_t status; uint32_t us; } sample;
typedef struct { sample *v; size_t n, cap; } vec;

static struct {
    const char *host; int port, threads, duration, warmup, hot, max_product;
    const char *scenario, *out, *users_file, *products_arg;
    int think_ms, timeout_s;
} A = {"127.0.0.1", 8080, 16, 10, 2, 0, 48, "browse", NULL, NULL, NULL, 0, 10};

static char **cookies, **csrfs;
static int nusers;
static long products[256];
static int nproducts;
static uint64_t t_start_ns, t_warm_ns, t_end_ns;

typedef struct {
    int id;
    vec samples;
    uint64_t conn_fail, timeouts, short_reads, cycles_ok, cycles_total;
    uint64_t rng;
} tstate;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t rnd(tstate *t) {            /* xorshift64* */
    uint64_t x = t->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    t->rng = x;
    return x * 2685821657736338717ull;
}

static void record(tstate *t, int op, int status, uint64_t t0, uint64_t t1) {
    if (t0 < t_warm_ns) return;              /* warm-up samples are discarded */
    vec *v = &t->samples;
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 1 << 14;
        v->v = realloc(v->v, v->cap * sizeof *v->v);
        if (!v->v) { perror("realloc"); exit(2); }
    }
    v->v[v->n++] = (sample){(uint8_t)op, (uint16_t)status, (uint32_t)((t1 - t0) / 1000)};
}

/* One request on a fresh connection. Returns the HTTP status, 0 on connect failure, -1 timeout, -2 bad response. */
static int do_request(tstate *t, int op, const char *method, const char *path, const char *cookie,
                      const char *csrf, const char *idem, const char *body, char *resp, size_t resp_cap) {
    char req[2048];
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s:%d\r\n", method, path, A.host, A.port);
    if (cookie) n += snprintf(req + n, sizeof req - (size_t)n, "Cookie: sf_sid=%s\r\n", cookie);
    if (csrf) n += snprintf(req + n, sizeof req - (size_t)n, "X-CSRF-Token: %s\r\n", csrf);
    if (idem) n += snprintf(req + n, sizeof req - (size_t)n, "Idempotency-Key: %s\r\n", idem);
    if (body) n += snprintf(req + n, sizeof req - (size_t)n, "Content-Type: application/json\r\nContent-Length: %zu\r\n", strlen(body));
    else if (strcmp(method, "GET") != 0) n += snprintf(req + n, sizeof req - (size_t)n, "Content-Length: 0\r\n");
    n += snprintf(req + n, sizeof req - (size_t)n, "\r\n%s", body ? body : "");

    uint64_t t0 = now_ns();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)A.port)};
    inet_pton(AF_INET, A.host, &a.sin_addr);
    struct timeval tv = {.tv_sec = A.timeout_s};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        t->conn_fail++;
        return 0;
    }
    if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) { close(fd); t->short_reads++; return -2; }
    size_t got = 0;
    int status = -2;
    for (;;) {
        ssize_t r = recv(fd, resp + got, resp_cap - 1 - got, 0);
        if (r > 0) { got += (size_t)r; if (got >= resp_cap - 1) break; continue; }
        if (r == 0) break;
        if (errno == EINTR) continue;
        status = -1;
        t->timeouts++;
        got = 0;
        break;
    }
    resp[got] = 0;
    close(fd);
    uint64_t t1 = now_ns();
    if (status != -1 && got > 12 && memcmp(resp, "HTTP/1.1 ", 9) == 0) status = atoi(resp + 9);
    else if (status != -1) t->short_reads++;
    record(t, op, status, t0, t1);
    return status;
}

static const char *body_of(const char *resp) {
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : "";
}

/* ----------------------------------------------------------- scenarios */

static const char *CATS[] = {"audio", "keyboards-mice", "home-office", "bags-carry", "lighting", "cables-power"};
static const char *TERMS[] = {"wireless", "keyboard", "lamp", "charger", "bag", "headphones", "desk", "cable"};

static void op_browse(tstate *t, char *resp, size_t cap) {
    char path[160];
    unsigned r = (unsigned)(rnd(t) % 100);
    if (r < 40) { do_request(t, OP_LIST, "GET", "/api/v1/products?limit=20", NULL, NULL, NULL, NULL, resp, cap); }
    else if (r < 55) {
        snprintf(path, sizeof path, "/api/v1/products?limit=20&category=%s", CATS[rnd(t) % 6]);
        do_request(t, OP_CATEGORY, "GET", path, NULL, NULL, NULL, NULL, resp, cap);
    } else if (r < 62) {
        /* A word that appears in ~1/8 of the benchmark catalogue: the expensive, unselective search. */
        snprintf(path, sizeof path, "/api/v1/products?limit=20&q=%s", TERMS[rnd(t) % 8]);
        do_request(t, OP_SEARCH, "GET", path, NULL, NULL, NULL, NULL, resp, cap);
    } else if (r < 70) {
        /* A product number: matches one or two rows, like a shopper searching for something specific. */
        snprintf(path, sizeof path, "/api/v1/products?limit=20&q=%lu", 100 + (unsigned long)(rnd(t) % (uint64_t)(A.max_product - 100)));
        do_request(t, OP_SEARCH_RARE, "GET", path, NULL, NULL, NULL, NULL, resp, cap);
    } else if (r < 90) {
        snprintf(path, sizeof path, "/api/v1/products/%lu", (unsigned long)(rnd(t) % (uint64_t)A.max_product) + 1);
        do_request(t, OP_DETAIL, "GET", path, NULL, NULL, NULL, NULL, resp, cap);
    } else {
        do_request(t, OP_SORTED, "GET", "/api/v1/products?limit=20&sort=price_asc", NULL, NULL, NULL, NULL, resp, cap);
    }
}

/* One purchase cycle: add to cart -> checkout -> pay -> cancel (cancel recycles the stock so the run can continue). */
static void op_buy(tstate *t, char *resp, size_t cap) {
    int u = t->id % nusers;
    int pool = A.hot > 0 && A.hot < nproducts ? A.hot : nproducts;
    long pid = products[rnd(t) % (uint64_t)pool];
    char path[128], key[64], oid[40];
    int measured = now_ns() >= t_warm_ns;           /* cycles that start during warm-up are not counted */
    uint64_t done_before = t->cycles_ok;
    if (measured) t->cycles_total++;
    snprintf(path, sizeof path, "/api/v1/cart/items/%ld", pid);
    if (do_request(t, OP_CART, "PUT", path, cookies[u], csrfs[u], NULL, "{\"quantity\":1}", resp, cap) != 200) return;
    snprintf(key, sizeof key, "lg-%d-%llu-%llu", t->id, (unsigned long long)now_ns(), (unsigned long long)rnd(t) % 100000);
    int st = do_request(t, OP_CHECKOUT, "POST", "/api/v1/orders", cookies[u], csrfs[u], key, "{}", resp, cap);
    if (st != 201) return;                                       /* 409 = sold out: a valid outcome, not an error */
    const char *p = strstr(body_of(resp), "\"id\":\"");
    if (!p) return;
    memcpy(oid, p + 6, 26);
    oid[26] = 0;
    snprintf(path, sizeof path, "/api/v1/orders/%s/pay", oid);
    if (do_request(t, OP_PAY, "POST", path, cookies[u], csrfs[u], NULL, "{\"scenario\":\"success\"}", resp, cap) != 200) return;
    snprintf(path, sizeof path, "/api/v1/orders/%s/cancel", oid);
    if (do_request(t, OP_CANCEL, "POST", path, cookies[u], csrfs[u], NULL, NULL, resp, cap) == 200 && measured)
        t->cycles_ok = done_before + 1;
}

static void *worker(void *arg) {
    tstate *t = arg;
    char *resp = malloc(65536);
    while (now_ns() < t_end_ns) {
        if (!strcmp(A.scenario, "healthz")) do_request(t, OP_HEALTHZ, "GET", "/healthz", NULL, NULL, NULL, NULL, resp, 65536);
        else if (!strcmp(A.scenario, "browse")) op_browse(t, resp, 65536);
        else if (!strcmp(A.scenario, "buy")) op_buy(t, resp, 65536);
        else if (rnd(t) % 100 < 80) op_browse(t, resp, 65536);          /* mixed */
        else op_buy(t, resp, 65536);
        if (A.think_ms) usleep((useconds_t)A.think_ms * 1000);
    }
    free(resp);
    return NULL;
}

/* ------------------------------------------------------------- reporting */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void stats(uint32_t *v, size_t n, double *p50, double *p95, double *p99, double *max, double *mean) {
    if (!n) { *p50 = *p95 = *p99 = *max = *mean = 0; return; }
    qsort(v, n, sizeof *v, cmp_u32);
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += v[i];
    *p50 = v[(size_t)((double)(n - 1) * 0.50)] / 1000.0;
    *p95 = v[(size_t)((double)(n - 1) * 0.95)] / 1000.0;
    *p99 = v[(size_t)((double)(n - 1) * 0.99)] / 1000.0;
    *max = v[n - 1] / 1000.0;
    *mean = sum / (double)n / 1000.0;
}

static void load_users(void) {
    FILE *f = fopen(A.users_file, "r");
    if (!f) { perror("users file"); exit(2); }
    char line[512];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char c[128], s[128];
        if (sscanf(line, "%127s %127s", c, s) != 2) continue;
        if (nusers == cap) { cap = cap ? cap * 2 : 64; cookies = realloc(cookies, (size_t)cap * sizeof *cookies); csrfs = realloc(csrfs, (size_t)cap * sizeof *csrfs); }
        cookies[nusers] = strdup(c);
        csrfs[nusers++] = strdup(s);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    static struct option opts[] = {{"host", 1, 0, 'H'}, {"port", 1, 0, 'p'}, {"threads", 1, 0, 't'}, {"duration", 1, 0, 'd'},
        {"warmup", 1, 0, 'w'}, {"scenario", 1, 0, 's'}, {"users", 1, 0, 'u'}, {"products", 1, 0, 'P'}, {"hot", 1, 0, 'h'},
        {"max-product", 1, 0, 'm'}, {"think-ms", 1, 0, 'k'}, {"out", 1, 0, 'o'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'H': A.host = optarg; break;           case 'p': A.port = atoi(optarg); break;
        case 't': A.threads = atoi(optarg); break;  case 'd': A.duration = atoi(optarg); break;
        case 'w': A.warmup = atoi(optarg); break;   case 's': A.scenario = optarg; break;
        case 'u': A.users_file = optarg; break;     case 'P': A.products_arg = optarg; break;
        case 'h': A.hot = atoi(optarg); break;      case 'm': A.max_product = atoi(optarg); break;
        case 'k': A.think_ms = atoi(optarg); break; case 'o': A.out = optarg; break;
        default: return 2;
        }
    }
    int needs_users = !strcmp(A.scenario, "buy") || !strcmp(A.scenario, "mixed");
    if (needs_users) {
        if (!A.users_file || !A.products_arg) { fprintf(stderr, "buy/mixed need --users and --products\n"); return 2; }
        load_users();
        for (char *s = strdup(A.products_arg), *tok = strtok(s, ","); tok && nproducts < 256; tok = strtok(NULL, ","))
            products[nproducts++] = atol(tok);
        if (!nusers || !nproducts) { fprintf(stderr, "no users/products\n"); return 2; }
    }
    tstate *ts = calloc((size_t)A.threads, sizeof *ts);
    pthread_t *th = calloc((size_t)A.threads, sizeof *th);
    t_start_ns = now_ns();
    t_warm_ns = t_start_ns + (uint64_t)A.warmup * 1000000000ull;
    t_end_ns = t_warm_ns + (uint64_t)A.duration * 1000000000ull;
    for (int i = 0; i < A.threads; i++) {
        ts[i].id = i;
        ts[i].rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)(i + 1) * 0xD1B54A32D192ED03ull) ^ t_start_ns;
        pthread_create(&th[i], NULL, worker, &ts[i]);
    }
    for (int i = 0; i < A.threads; i++) pthread_join(th[i], NULL);

    /* merge */
    size_t total = 0;
    for (int i = 0; i < A.threads; i++) total += ts[i].samples.n;
    uint32_t *all = malloc((total ? total : 1) * sizeof *all);
    uint32_t *per_op[OP_COUNT] = {0};
    size_t n_op[OP_COUNT] = {0}, st_count[600] = {0};
    for (int o = 0; o < OP_COUNT; o++) per_op[o] = malloc((total ? total : 1) * sizeof(uint32_t));
    size_t k = 0, other_status = 0, cs_fail = 0, to = 0, sr = 0, cyc_ok = 0, cyc = 0;
    for (int i = 0; i < A.threads; i++) {
        for (size_t j = 0; j < ts[i].samples.n; j++) {
            sample *s = &ts[i].samples.v[j];
            all[k++] = s->us;
            per_op[s->op][n_op[s->op]++] = s->us;
            if (s->status < 600) st_count[s->status]++; else other_status++;
        }
        cs_fail += ts[i].conn_fail; to += ts[i].timeouts; sr += ts[i].short_reads; cyc_ok += ts[i].cycles_ok; cyc += ts[i].cycles_total;
    }
    double p50, p95, p99, mx, mean;
    stats(all, total, &p50, &p95, &p99, &mx, &mean);

    FILE *out = A.out ? fopen(A.out, "w") : stdout;
    fprintf(out, "{\n  \"scenario\": \"%s\", \"threads\": %d, \"duration_s\": %d, \"warmup_s\": %d,\n", A.scenario, A.threads, A.duration, A.warmup);
    fprintf(out, "  \"requests\": %zu, \"requests_per_s\": %.1f,\n", total, (double)total / (double)A.duration);
    fprintf(out, "  \"latency_ms\": {\"mean\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f},\n", mean, p50, p95, p99, mx);
    fprintf(out, "  \"connect_failures\": %zu, \"timeouts\": %zu, \"bad_responses\": %zu,\n", cs_fail, to, sr);
    fprintf(out, "  \"purchase_cycles\": {\"attempted\": %zu, \"completed\": %zu, \"per_s\": %.1f},\n", cyc, cyc_ok, (double)cyc_ok / (double)A.duration);
    fprintf(out, "  \"status_counts\": {");
    int first = 1;
    for (int s = 0; s < 600; s++) if (st_count[s]) { fprintf(out, "%s\"%d\": %zu", first ? "" : ", ", s, st_count[s]); first = 0; }
    fprintf(out, "},\n  \"by_operation\": {");
    first = 1;
    for (int o = 0; o < OP_COUNT; o++) {
        if (!n_op[o]) continue;
        double a, b, c2, d, m;
        stats(per_op[o], n_op[o], &a, &b, &c2, &d, &m);
        fprintf(out, "%s\n    \"%s\": {\"count\": %zu, \"mean\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f}",
                first ? "" : ",", OP_NAME[o], n_op[o], m, a, b, c2, d);
        first = 0;
    }
    fprintf(out, "\n  }\n}\n");
    if (A.out) fclose(out);
    (void)other_status;
    return 0;
}
