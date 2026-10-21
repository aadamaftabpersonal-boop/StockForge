#include "metrics.h"

#include <stdatomic.h>
#include <stdio.h>

static const uint64_t bounds_us[METRICS_BUCKETS - 1] = {
    1000, 2000, 5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000};

static atomic_ullong c_accepted, c_rejected, c_requests, c_2xx, c_3xx, c_4xx, c_5xx;
static atomic_ullong lat_sum_us, qwait_sum_us;
static atomic_ullong lat_hist[METRICS_BUCKETS];
static atomic_int    busy;
static atomic_ullong c_events[MC_COUNT];

void metrics_checkout_event(checkout_event e) {
    if (e >= 0 && e < MC_COUNT) atomic_fetch_add_explicit(&c_events[e], 1, memory_order_relaxed);
}

#define INC(x) atomic_fetch_add_explicit(&(x), 1, memory_order_relaxed)
#define ADD(x, v) atomic_fetch_add_explicit(&(x), (v), memory_order_relaxed)
#define GET(x) atomic_load_explicit(&(x), memory_order_relaxed)

void metrics_conn_accepted(void) { INC(c_accepted); }
void metrics_conn_rejected_overload(void) { INC(c_rejected); }
void metrics_worker_busy(int d) { atomic_fetch_add_explicit(&busy, d, memory_order_relaxed); }

void metrics_observe(int status, uint64_t lat_us, uint64_t qw_us) {
    INC(c_requests);
    if (status >= 500) INC(c_5xx);
    else if (status >= 400) INC(c_4xx);
    else if (status >= 300) INC(c_3xx);
    else INC(c_2xx);
    ADD(lat_sum_us, lat_us);
    ADD(qwait_sum_us, qw_us);
    int b = 0;
    while (b < METRICS_BUCKETS - 1 && lat_us > bounds_us[b]) b++;
    INC(lat_hist[b]);
}

size_t metrics_render(char *buf, size_t n, size_t qd, size_t hwm, size_t qcap, int workers) {
    int o = snprintf(buf, n,
        "{\"connections_accepted\":%llu,\"connections_rejected_overload\":%llu,"
        "\"requests_total\":%llu,\"responses\":{\"2xx\":%llu,\"3xx\":%llu,\"4xx\":%llu,\"5xx\":%llu},"
        "\"workers\":{\"total\":%d,\"busy\":%d},"
        "\"queue\":{\"depth\":%zu,\"high_water\":%zu,\"capacity\":%zu},"
        "\"orders\":{\"created\":%llu,\"replayed\":%llu,\"idempotency_conflicts\":%llu,"
        "\"insufficient_stock\":%llu,\"deadlock_retries\":%llu,\"retries_exhausted\":%llu,"
        "\"paid\":%llu,\"payment_failed\":%llu,\"cancelled\":%llu,\"expired\":%llu},"
        "\"latency_us_sum\":%llu,\"queue_wait_us_sum\":%llu,"
        "\"latency_ms_hist\":{\"le\":[1,2,5,10,25,50,100,250,500,1000,\"+Inf\"],\"count\":[",
        GET(c_accepted), GET(c_rejected), GET(c_requests), GET(c_2xx), GET(c_3xx),
        GET(c_4xx), GET(c_5xx), workers, atomic_load_explicit(&busy, memory_order_relaxed),
        qd, hwm, qcap,
        GET(c_events[MC_CREATED]), GET(c_events[MC_REPLAYED]), GET(c_events[MC_IDEM_CONFLICT]),
        GET(c_events[MC_INSUFFICIENT]), GET(c_events[MC_DEADLOCK_RETRY]), GET(c_events[MC_RETRY_EXHAUSTED]),
        GET(c_events[MC_PAID]), GET(c_events[MC_PAY_FAILED]), GET(c_events[MC_CANCELLED]),
        GET(c_events[MC_EXPIRED]), GET(lat_sum_us), GET(qwait_sum_us));
    for (int i = 0; i < METRICS_BUCKETS && o > 0 && (size_t)o < n; i++)
        o += snprintf(buf + o, n - (size_t)o, "%s%llu", i ? "," : "", GET(lat_hist[i]));
    if (o > 0 && (size_t)o < n) o += snprintf(buf + o, n - (size_t)o, "]}}");
    if (o < 0) return 0;
    return (size_t)o < n ? (size_t)o : n - 1;
}
