#include "sweeper.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "db.h"
#include "log.h"
#include "orders.h"

static pthread_t       thr;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv;
static int             stopping;
static int             interval_ms;
static int             running;

static void *sweep_main(void *arg) {
    (void)arg;
    db_thread_init();
    pthread_mutex_lock(&mu);
    while (!stopping) {
        pthread_mutex_unlock(&mu);
        db *d = db_get();
        int n;
        do {                              /* drain a backlog without sleeping between batches */
            n = d ? orders_expire_due(d, "sweeper", 50) : -1;
            if (n > 0) {
                char m[64];
                snprintf(m, sizeof m, "expired %d overdue order(s)", n);
                log_msg("info", "expiry", m);
            }
        } while (n >= 50 && !stopping);
        pthread_mutex_lock(&mu);
        if (stopping) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += interval_ms / 1000;
        ts.tv_nsec += (long)(interval_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (!stopping && pthread_cond_timedwait(&cv, &mu, &ts) == 0) {}   /* loop: spurious wakeups */
    }
    pthread_mutex_unlock(&mu);
    db_thread_end();
    return NULL;
}

int sweeper_start(const config *cfg) {
    pthread_condattr_t at;
    pthread_condattr_init(&at);
    pthread_cond_init(&cv, &at);
    pthread_condattr_destroy(&at);
    interval_ms = cfg->expiry_sweep_ms;
    stopping = 0;
    if (pthread_create(&thr, NULL, sweep_main, NULL)) return -1;
    running = 1;
    return 0;
}

void sweeper_stop(void) {
    if (!running) return;
    pthread_mutex_lock(&mu);
    stopping = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
    pthread_join(thr, NULL);
    running = 0;
}
