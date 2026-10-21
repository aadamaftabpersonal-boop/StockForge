#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "queue.h"
#include "testutil.h"

static void test_fifo_and_bounds(void) {
    fdqueue q;
    CHECK(fdq_init(&q, 3) == 0);
    CHECK(fdq_try_push(&q, 10) == FDQ_OK);
    CHECK(fdq_try_push(&q, 11) == FDQ_OK);
    CHECK(fdq_try_push(&q, 12) == FDQ_OK);
    CHECK(fdq_try_push(&q, 13) == FDQ_FULL);          /* bounded: rejects, never blocks */
    CHECK(fdq_depth(&q) == 3);
    CHECK(fdq_high_water(&q) == 3);
    fdq_item it;
    CHECK(fdq_pop(&q, &it) == 0 && it.fd == 10);       /* FIFO */
    CHECK(fdq_try_push(&q, 13) == FDQ_OK);             /* ring buffer wraps */
    CHECK(fdq_pop(&q, &it) == 0 && it.fd == 11);
    CHECK(fdq_pop(&q, &it) == 0 && it.fd == 12);
    CHECK(fdq_pop(&q, &it) == 0 && it.fd == 13);
    CHECK(fdq_depth(&q) == 0);
    fdq_destroy(&q);
}

static void test_close_drains_then_ends(void) {
    fdqueue q;
    fdq_init(&q, 4);
    fdq_try_push(&q, 1);
    fdq_try_push(&q, 2);
    fdq_close(&q);
    CHECK(fdq_try_push(&q, 3) == FDQ_CLOSED);          /* no new work after close */
    fdq_item it;
    CHECK(fdq_pop(&q, &it) == 0 && it.fd == 1);        /* queued work still drains */
    CHECK(fdq_pop(&q, &it) == 0 && it.fd == 2);
    CHECK(fdq_pop(&q, &it) == -1);                     /* then workers are told to exit */
    fdq_destroy(&q);
}

static void *blocked_popper(void *arg) {
    fdqueue *q = arg;
    fdq_item it;
    return (void *)(long)fdq_pop(q, &it);
}

static void test_close_wakes_all_blocked_poppers(void) {
    fdqueue q;
    fdq_init(&q, 4);
    pthread_t t[6];
    for (int i = 0; i < 6; i++) pthread_create(&t[i], NULL, blocked_popper, &q);
    usleep(100 * 1000);                                /* let them block in cond_wait */
    fdq_close(&q);
    int all_ended = 1;
    for (int i = 0; i < 6; i++) {
        void *r;
        pthread_join(t[i], &r);                        /* hangs forever if a wakeup is lost */
        if ((long)r != -1) all_ended = 0;
    }
    CHECK(all_ended);
    fdq_destroy(&q);
}

enum { PRODUCERS = 4, CONSUMERS = 4, PER_PRODUCER = 25000 };
static fdqueue stress_q;
static unsigned char *seen;                            /* seen[v] counts deliveries of v */
static pthread_mutex_t seen_mu = PTHREAD_MUTEX_INITIALIZER;

static void *producer(void *arg) {
    long base = (long)arg * PER_PRODUCER;
    for (long i = 0; i < PER_PRODUCER; i++)
        while (fdq_try_push(&stress_q, (int)(base + i)) == FDQ_FULL) sched_yield();
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    fdq_item it;
    while (fdq_pop(&stress_q, &it) == 0) {
        pthread_mutex_lock(&seen_mu);
        seen[it.fd]++;
        pthread_mutex_unlock(&seen_mu);
    }
    return NULL;
}

static void test_mpmc_no_loss_no_duplicates(void) {
    size_t total = (size_t)PRODUCERS * PER_PRODUCER;
    seen = calloc(total, 1);
    fdq_init(&stress_q, 16);                           /* tiny queue => constant contention */
    pthread_t p[PRODUCERS], c[CONSUMERS];
    for (long i = 0; i < CONSUMERS; i++) pthread_create(&c[i], NULL, consumer, NULL);
    for (long i = 0; i < PRODUCERS; i++) pthread_create(&p[i], NULL, producer, (void *)i);
    for (int i = 0; i < PRODUCERS; i++) pthread_join(p[i], NULL);
    fdq_close(&stress_q);
    for (int i = 0; i < CONSUMERS; i++) pthread_join(c[i], NULL);
    size_t lost = 0, dup = 0;
    for (size_t i = 0; i < total; i++) { if (seen[i] == 0) lost++; else if (seen[i] > 1) dup++; }
    CHECK(lost == 0);
    CHECK(dup == 0);
    CHECK(fdq_depth(&stress_q) == 0);
    CHECK(fdq_high_water(&stress_q) <= 16);            /* never exceeded capacity */
    fdq_destroy(&stress_q);
    free(seen);
}

int main(void) {
    test_fifo_and_bounds();
    test_close_drains_then_ends();
    test_close_wakes_all_blocked_poppers();
    test_mpmc_no_loss_no_duplicates();
    TEST_MAIN_END();
}
