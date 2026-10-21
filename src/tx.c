#include "tx.h"

#include <stdlib.h>
#include <time.h>

#include "metrics.h"
#include "util.h"

int tx_classify(db *d) {
    unsigned e = db_errno(d);
    db_rollback(d);
    return (e == 1213 || e == 1205) ? TX_RETRY : TX_FAIL;
}

int tx_run(db *d, tx_body fn, void *arg) {
    static __thread unsigned seed;
    if (!seed) seed = (unsigned)now_ns() | 1u;
    int rc = TX_RETRY;
    for (int attempt = 1; attempt <= TX_MAX_ATTEMPTS; attempt++) {
        rc = fn(d, arg);
        if (rc != TX_RETRY) return rc;
        metrics_checkout_event(MC_DEADLOCK_RETRY);
        /* Jittered exponential backoff so the victims do not collide again in lockstep. */
        long ms = (long)(rand_r(&seed) % (unsigned)(4 << (attempt < 5 ? attempt : 5))) + 1;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = ms * 1000000L};
        nanosleep(&ts, NULL);
    }
    metrics_checkout_event(MC_RETRY_EXHAUSTED);
    return TX_RETRY;
}
