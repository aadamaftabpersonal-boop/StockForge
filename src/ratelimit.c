#include "ratelimit.h"

#include <pthread.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

#define PROBE 8

typedef struct {
    uint64_t hash;          /* 0 = empty */
    uint64_t window_start;
    uint32_t window_s;
    uint32_t count;
} slot;

struct rl {
    slot           *slots;
    size_t          n;
    unsigned char   key[crypto_shorthash_KEYBYTES];
    pthread_mutex_t mu;
};

rl *rl_new(size_t slots) {
    rl *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->slots = calloc(slots, sizeof *r->slots);
    if (!r->slots) { free(r); return NULL; }
    r->n = slots;
    randombytes_buf(r->key, sizeof r->key);
    pthread_mutex_init(&r->mu, NULL);
    return r;
}

void rl_free(rl *r) {
    if (!r) return;
    pthread_mutex_destroy(&r->mu);
    free(r->slots);
    free(r);
}

int rl_allow(rl *r, const char *key, int limit, int window_s, uint64_t now, int *retry_after) {
    unsigned char h8[crypto_shorthash_BYTES];
    crypto_shorthash(h8, (const unsigned char *)key, strlen(key), r->key);
    uint64_t h;
    memcpy(&h, h8, sizeof h);
    if (h == 0) h = 1;

    pthread_mutex_lock(&r->mu);
    slot *victim = NULL, *found = NULL;
    for (size_t i = 0; i < PROBE; i++) {
        slot *s = &r->slots[(h + i) % r->n];
        if (s->hash == h) { found = s; break; }
        int dead = s->hash == 0 || now >= s->window_start + s->window_s;
        if (!victim || (dead && victim->hash != 0) ||
            (!dead && victim->hash != 0 && s->window_start < victim->window_start))
            victim = s;
    }
    slot *s = found ? found : victim;
    if (!found || now >= s->window_start + s->window_s) {
        s->hash = h;
        s->window_start = now;
        s->window_s = (uint32_t)window_s;
        s->count = 0;
    }
    s->count++;
    int ok = s->count <= (uint32_t)limit;
    if (!ok && retry_after) {
        uint64_t left = s->window_start + s->window_s - now;
        *retry_after = left < 1 ? 1 : (int)left;
    }
    pthread_mutex_unlock(&r->mu);
    return ok;
}
