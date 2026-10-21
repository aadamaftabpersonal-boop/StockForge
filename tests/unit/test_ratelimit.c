#include <pthread.h>
#include <sodium.h>
#include <stdio.h>

#include "ratelimit.h"
#include "testutil.h"

static void test_window(void) {
    rl *r = rl_new(64);
    int retry = 0;
    CHECK(rl_allow(r, "k", 3, 60, 1000, &retry));
    CHECK(rl_allow(r, "k", 3, 60, 1001, &retry));
    CHECK(rl_allow(r, "k", 3, 60, 1002, &retry));
    CHECK(!rl_allow(r, "k", 3, 60, 1010, &retry));      /* 4th in window */
    CHECK(retry == 50);                                  /* window opened at 1000, ends 1060 */
    CHECK(!rl_allow(r, "k", 3, 60, 1059, &retry) && retry == 1);
    CHECK(rl_allow(r, "k", 3, 60, 1060, &retry));        /* new window */
    CHECK(rl_allow(r, "other", 3, 60, 1010, &retry));    /* keys are independent */
    rl_free(r);
}

static void test_eviction_is_safe(void) {
    rl *r = rl_new(16);                                  /* far fewer slots than keys */
    char key[32];
    int first_denied = 0;
    for (int i = 0; i < 20000; i++) {
        snprintf(key, sizeof key, "ip-%d", i);
        if (!rl_allow(r, key, 3, 60, 5000 + (uint64_t)(i % 50), NULL)) first_denied++;
    }
    CHECK(first_denied == 0);                            /* a brand-new key is never denied */
    rl_free(r);
}

static rl *shared;
static int allowed_total;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void *hammer(void *arg) {
    (void)arg;
    int mine = 0;
    for (int i = 0; i < 2500; i++) mine += rl_allow(shared, "same-key", 1000, 3600, 77777, NULL);
    pthread_mutex_lock(&mu);
    allowed_total += mine;
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void test_concurrent_exactness(void) {
    shared = rl_new(64);
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, hammer, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    CHECK(allowed_total == 1000);                        /* 10000 attempts, limit 1000: exactly 1000 pass */
    rl_free(shared);
}

int main(void) {
    if (sodium_init() < 0) return 2;
    test_window();
    test_eviction_is_safe();
    test_concurrent_exactness();
    TEST_MAIN_END();
}
