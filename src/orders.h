#ifndef SF_ORDERS_H
#define SF_ORDERS_H

#include <stdint.h>

#include "cJSON.h"
#include "db.h"
#include "payment_sim.h"

/* The order engine: checkout, payment, cancellation, expiry.
 * Design, lock order and invariants are documented in docs/CHECKOUT.md.
 *
 * Every entry point returns a tx.h code (TX_DONE / TX_FAIL / TX_RETRY=exhausted).
 * On TX_DONE `out` holds the complete HTTP status and JSON body to send; the
 * caller owns out->json and must cJSON_Delete it (or hand it to resp_json). */
typedef struct {
    int    http;
    cJSON *json;
    int    replay;      /* 1 when served from the idempotency record */
} order_out;

void order_out_free(order_out *o);

/* 26-char time-sortable public identifier, alphabet [0-9A-HJKMNP-TV-Z]. */
void order_new_id(char out[27]);

/* Full order document (items + payment attempts) or NULL on database error. */
cJSON *orders_json_by_id(db *d, uint64_t order_id);

/* POST /orders: turns the user's cart into a PENDING_PAYMENT order, atomically
 * reserving stock. `req_hash` identifies the request body for idempotency. */
int orders_checkout(db *d, const char *rid, uint64_t user_id, const char *idem_key,
                    const unsigned char req_hash[32], int has_expected, int64_t expected_total,
                    int ttl_s, order_out *out);

/* Pays (simulated) an order owned by user_id. Safe to call repeatedly. */
int orders_pay(db *d, const char *rid, uint64_t user_id, const char *public_id,
               pay_scenario scenario, order_out *out);

/* Cancels an order owned by user_id (PENDING_PAYMENT or PAID). Idempotent. */
int orders_cancel(db *d, const char *rid, uint64_t user_id, const char *public_id, order_out *out);

/* Expires up to `max` overdue PENDING_PAYMENT orders, releasing their stock.
 * Returns how many were expired, or -1 on database failure. */
int orders_expire_due(db *d, const char *rid, int max);

#endif
