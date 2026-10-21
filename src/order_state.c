#include "order_state.h"

#include <string.h>

static const char *const NAMES[OS_COUNT] = {
    "PENDING_PAYMENT", "PAID", "SHIPPED", "DELIVERED", "CANCELLED", "EXPIRED"};

/* ALLOWED[from][to]: the single source of truth for legal transitions. */
static const unsigned char ALLOWED[OS_COUNT][OS_COUNT] = {
    /*                  PEND PAID SHIP DELV CANC EXPR */
    /* PENDING_PAYMENT */ {0, 1, 0, 0, 1, 1},
    /* PAID            */ {0, 0, 1, 0, 1, 0},
    /* SHIPPED         */ {0, 0, 0, 1, 0, 0},
    /* DELIVERED       */ {0, 0, 0, 0, 0, 0},
    /* CANCELLED       */ {0, 0, 0, 0, 0, 0},
    /* EXPIRED         */ {0, 0, 0, 0, 0, 0},
};

const char *os_name(order_status s) { return (s >= 0 && s < OS_COUNT) ? NAMES[s] : "INVALID"; }

order_status os_parse(const char *name) {
    for (int i = 0; i < OS_COUNT; i++)
        if (strcmp(name, NAMES[i]) == 0) return (order_status)i;
    return OS_INVALID;
}

int os_can_transition(order_status from, order_status to) {
    if (from < 0 || from >= OS_COUNT || to < 0 || to >= OS_COUNT) return 0;
    return ALLOWED[from][to];
}

int os_is_terminal(order_status s) {
    if (s < 0 || s >= OS_COUNT) return 0;
    for (int t = 0; t < OS_COUNT; t++)
        if (ALLOWED[s][t]) return 0;
    return 1;
}
