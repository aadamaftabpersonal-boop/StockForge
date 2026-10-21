#ifndef SF_METRICS_H
#define SF_METRICS_H

#include <stddef.h>
#include <stdint.h>

/* Process-wide counters. All fields are C11 atomics updated with relaxed
 * ordering: they are statistics, not synchronisation. Readers get a
 * slightly-torn-in-time but never corrupt snapshot. */
#define METRICS_BUCKETS 11   /* le 1,2,5,10,25,50,100,250,500,1000 ms, +Inf */

void metrics_conn_accepted(void);
void metrics_conn_rejected_overload(void);
void metrics_worker_busy(int delta);
void metrics_observe(int status, uint64_t latency_us, uint64_t queue_wait_us);

/* Renders JSON into buf; returns bytes written (<= n-1). */
size_t metrics_render(char *buf, size_t n, size_t queue_depth, size_t queue_hwm,
                      size_t queue_cap, int workers);

#endif
