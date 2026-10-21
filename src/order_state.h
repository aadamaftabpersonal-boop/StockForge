#ifndef SF_ORDER_STATE_H
#define SF_ORDER_STATE_H

/* The order lifecycle as data. Every status change in the system goes through
 * os_can_transition(); there is no other place that decides what is legal.
 *
 *   PENDING_PAYMENT -> PAID | CANCELLED | EXPIRED
 *   PAID            -> SHIPPED | CANCELLED
 *   SHIPPED         -> DELIVERED
 *   CANCELLED, EXPIRED, DELIVERED are terminal.                                */
typedef enum {
    OS_PENDING_PAYMENT = 0,
    OS_PAID,
    OS_SHIPPED,
    OS_DELIVERED,
    OS_CANCELLED,
    OS_EXPIRED,
    OS_COUNT,
    OS_INVALID = -1
} order_status;

const char  *os_name(order_status s);              /* "PENDING_PAYMENT" ... (matches the SQL ENUM) */
order_status os_parse(const char *name);           /* OS_INVALID if unknown */
int          os_can_transition(order_status from, order_status to);
int          os_is_terminal(order_status s);

#endif
