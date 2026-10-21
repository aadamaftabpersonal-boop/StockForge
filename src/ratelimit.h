#ifndef SF_RATELIMIT_H
#define SF_RATELIMIT_H

#include <stddef.h>
#include <stdint.h>

/* Fixed-window counters in a fixed-size hash table, keyed by a SipHash of the
 * key string (random per-process key, so attackers cannot aim collisions).
 * Memory is bounded: when the probe window is full the entry with the oldest
 * window is evicted, which means an attacker spraying many distinct keys can
 * reset other counters. Known limitation; also per-process, not cluster-wide.
 * Time is a parameter so tests are deterministic. */
typedef struct rl rl;

rl  *rl_new(size_t slots);
void rl_free(rl *r);

/* Counts one event for `key`. Returns 1 if within `limit` per `window_s`,
 * 0 if over (and sets *retry_after_s to the seconds until the window resets). */
int  rl_allow(rl *r, const char *key, int limit, int window_s, uint64_t now_s, int *retry_after_s);

#endif
