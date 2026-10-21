#ifndef SF_TESTUTIL_H
#define SF_TESTUTIL_H

#include <stdio.h>
#include <stdlib.h>

static int t_failed, t_passed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) t_passed++;                                                    \
        else { t_failed++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define TEST_MAIN_END()                                                          \
    do {                                                                         \
        printf("  %d checks passed, %d failed\n", t_passed, t_failed);            \
        return t_failed ? 1 : 0;                                                 \
    } while (0)

#endif
