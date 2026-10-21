#include "queue.h"

#include <stdlib.h>

#include "util.h"

int fdq_init(fdqueue *q, size_t cap) {
    q->buf = calloc(cap, sizeof *q->buf);
    if (!q->buf) return -1;
    q->cap = cap; q->head = q->len = q->high_water = 0; q->closed = 0;
    if (pthread_mutex_init(&q->mu, NULL)) { free(q->buf); return -1; }
    if (pthread_cond_init(&q->not_empty, NULL)) {
        pthread_mutex_destroy(&q->mu); free(q->buf); return -1;
    }
    return 0;
}

void fdq_destroy(fdqueue *q) {
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mu);
    free(q->buf);
    q->buf = NULL;
}

int fdq_try_push(fdqueue *q, int fd) {
    pthread_mutex_lock(&q->mu);
    if (q->closed) { pthread_mutex_unlock(&q->mu); return FDQ_CLOSED; }
    if (q->len == q->cap) { pthread_mutex_unlock(&q->mu); return FDQ_FULL; }
    size_t tail = (q->head + q->len) % q->cap;
    q->buf[tail].fd = fd;
    q->buf[tail].enq_ns = now_ns();
    q->len++;
    if (q->len > q->high_water) q->high_water = q->len;
    pthread_cond_signal(&q->not_empty);   /* one item => wake exactly one worker */
    pthread_mutex_unlock(&q->mu);
    return FDQ_OK;
}

int fdq_pop(fdqueue *q, fdq_item *out) {
    pthread_mutex_lock(&q->mu);
    while (q->len == 0 && !q->closed)     /* loop: guards spurious wakeups */
        pthread_cond_wait(&q->not_empty, &q->mu);
    if (q->len == 0) { pthread_mutex_unlock(&q->mu); return -1; }  /* closed + drained */
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void fdq_close(fdqueue *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty); /* every idle worker must see the close */
    pthread_mutex_unlock(&q->mu);
}

size_t fdq_depth(fdqueue *q) {
    pthread_mutex_lock(&q->mu);
    size_t n = q->len;
    pthread_mutex_unlock(&q->mu);
    return n;
}

size_t fdq_high_water(fdqueue *q) {
    pthread_mutex_lock(&q->mu);
    size_t n = q->high_water;
    pthread_mutex_unlock(&q->mu);
    return n;
}
