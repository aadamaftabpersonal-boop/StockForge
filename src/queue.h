#ifndef SF_QUEUE_H
#define SF_QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded FIFO of accepted connections (MPMC, mutex + condition variable).
 *
 * Ownership: a pushed fd belongs to the queue until popped; the popper then
 * owns it and must close it. If push fails the caller still owns the fd.
 *
 * Overload policy is reject-don't-block: the acceptor must never stall, so
 * there is no blocking push and no not_full condition variable. */
typedef struct {
    int      fd;
    uint64_t enq_ns;    /* CLOCK_MONOTONIC at enqueue, for queue-wait metrics */
} fdq_item;

typedef struct {
    fdq_item       *buf;
    size_t          cap, head, len, high_water;
    int             closed;
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;
} fdqueue;

enum { FDQ_OK = 0, FDQ_FULL = -1, FDQ_CLOSED = -2 };

int    fdq_init(fdqueue *q, size_t cap);
void   fdq_destroy(fdqueue *q);                /* only after all users have stopped */
int    fdq_try_push(fdqueue *q, int fd);       /* FDQ_OK | FDQ_FULL | FDQ_CLOSED */
int    fdq_pop(fdqueue *q, fdq_item *out);     /* blocks; 0, or -1 once closed AND drained */
void   fdq_close(fdqueue *q);                  /* wakes all poppers; remaining items still drain */
size_t fdq_depth(fdqueue *q);
size_t fdq_high_water(fdqueue *q);

#endif
