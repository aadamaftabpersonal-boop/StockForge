#ifndef SF_METRICS_H
#define SF_METRICS_H

#include <stddef.h>
#include <stdint.h>

/* Process-wide counters. All fields are C11 atomics updated with relaxed
 * ordering: they are statistics, not synchronisation. Readers get a
 * slightly-torn-in-time but never corrupt snapshot. */
#define METRICS_BUCKETS 11   /* le 1,2,5,10,25,50,100,250,500,1000 ms, +Inf */

/* Order-engine events: lets operators (and tests) see contention, not guess it. */
typedef enum {
    MC_CREATED = 0,        /* new order committed */
    MC_REPLAYED,           /* idempotent replay served */
    MC_IDEM_CONFLICT,      /* same key, different request */
    MC_INSUFFICIENT,       /* checkout refused: not enough stock */
    MC_DEADLOCK_RETRY,     /* a transaction was re-run after deadlock / lock timeout */
    MC_RETRY_EXHAUSTED,    /* gave up after TX_MAX_ATTEMPTS */
    MC_PAID,
    MC_PAY_FAILED,
    MC_CANCELLED,
    MC_EXPIRED,
    MC_COUNT
} checkout_event;
void metrics_checkout_event(checkout_event e);

/* Database layer: statement latency, slow statements, connects, errors. */
typedef enum { MDB_CONNECT = 0, MDB_ERROR } db_event;
void metrics_db_observe(uint64_t latency_us, int slow);
void metrics_db_event(db_event e);

void metrics_conn_accepted(void);
void metrics_conn_rejected_overload(void);
void metrics_worker_busy(int delta);
void metrics_observe(int status, uint64_t latency_us, uint64_t queue_wait_us);

/* Renders JSON into buf; returns bytes written (<= n-1). */
size_t metrics_render(char *buf, size_t n, size_t queue_depth, size_t queue_hwm,
                      size_t queue_cap, int workers);

#endif
