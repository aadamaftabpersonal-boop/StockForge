#include "reconcile.h"

#include <stddef.h>

typedef struct { const char *name, *desc, *sql; } spec;

/* Every query returns offending ids in column 1. */
static const spec CHECKS[] = {
    {"reserved_not_above_on_hand",
     "Inventory never reserves more than it holds (I2)",
     "SELECT product_id FROM inventory WHERE reserved > on_hand LIMIT 20"},

    {"reserved_equals_held_reservations",
     "inventory.reserved equals the sum of HELD reservations per product (I3)",
     "SELECT i.product_id FROM inventory i "
     "LEFT JOIN (SELECT product_id, SUM(quantity) q FROM stock_reservations WHERE status = 'held' "
     "           GROUP BY product_id) r ON r.product_id = i.product_id "
     "WHERE i.reserved <> COALESCE(r.q, 0) LIMIT 20"},

    {"on_hand_equals_ledger",
     "inventory.on_hand equals the sum of ledger on-hand deltas (I4)",
     "SELECT i.product_id FROM inventory i "
     "LEFT JOIN (SELECT product_id, SUM(delta_on_hand) s FROM inventory_ledger GROUP BY product_id) l "
     "ON l.product_id = i.product_id WHERE i.on_hand <> COALESCE(l.s, 0) LIMIT 20"},

    {"reserved_equals_ledger",
     "inventory.reserved equals the sum of ledger reserved deltas (I4)",
     "SELECT i.product_id FROM inventory i "
     "LEFT JOIN (SELECT product_id, SUM(delta_reserved) s FROM inventory_ledger GROUP BY product_id) l "
     "ON l.product_id = i.product_id WHERE i.reserved <> COALESCE(l.s, 0) LIMIT 20"},

    {"order_total_equals_items",
     "orders.total_cents equals the sum of its line items (I9)",
     "SELECT o.id FROM orders o "
     "LEFT JOIN (SELECT order_id, SUM(unit_price_cents * quantity) t FROM order_items GROUP BY order_id) x "
     "ON x.order_id = o.id WHERE o.total_cents <> COALESCE(x.t, 0) LIMIT 20"},

    {"reservation_state_matches_order_state",
     "Reservation status agrees with order status: pending=held, paid..=committed, cancelled/expired=released (I7)",
     "SELECT DISTINCT o.id FROM orders o JOIN stock_reservations r ON r.order_id = o.id "
     "WHERE (o.status = 'PENDING_PAYMENT' AND r.status <> 'held') "
     "   OR (o.status IN ('PAID','SHIPPED','DELIVERED') AND r.status <> 'committed') "
     "   OR (o.status IN ('CANCELLED','EXPIRED') AND r.status <> 'released') LIMIT 20"},

    {"reservations_match_order_items",
     "Every order line has a reservation of exactly the same quantity (I6)",
     "SELECT DISTINCT oi.order_id FROM order_items oi "
     "LEFT JOIN stock_reservations r ON r.order_id = oi.order_id AND r.product_id = oi.product_id "
     "WHERE r.id IS NULL OR r.quantity <> oi.quantity LIMIT 20"},

    {"paid_orders_charged_exactly_once",
     "Paid orders have exactly one successful payment; unpaid ones have none (no double charge)",
     "SELECT o.id FROM orders o "
     "LEFT JOIN (SELECT order_id, COUNT(*) n FROM payment_attempts WHERE status = 'succeeded' "
     "           GROUP BY order_id) p ON p.order_id = o.id "
     "WHERE (o.status IN ('PAID','SHIPPED','DELIVERED') AND COALESCE(p.n, 0) <> 1) "
     "   OR (o.status IN ('PENDING_PAYMENT','EXPIRED') AND COALESCE(p.n, 0) > 0) LIMIT 20"},

    {"no_overdue_pending_orders",
     "No PENDING_PAYMENT order is more than 60 s past its deadline (the sweeper is keeping up) (I10)",
     "SELECT id FROM orders WHERE status = 'PENDING_PAYMENT' AND expires_at < NOW(3) - INTERVAL 60 SECOND LIMIT 20"},
};

int reconcile_run(db *d, rc_check *out, int max) {
    int n = (int)(sizeof CHECKS / sizeof CHECKS[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        out[i].name = CHECKS[i].name;
        out[i].description = CHECKS[i].desc;
        out[i].violations = 0;
        db_rs *rs = db_query(d, CHECKS[i].sql, "");
        if (!rs) return -1;
        uint64_t id;
        while (out[i].violations < RC_MAX_IDS && db_rs_next(rs, "u", &id) == 1)
            out[i].ids[out[i].violations++] = id;
        db_rs_free(rs);
    }
    return n;
}

int reconcile_ok(const rc_check *c, int n) {
    for (int i = 0; i < n; i++)
        if (c[i].violations) return 0;
    return 1;
}

cJSON *reconcile_json(const rc_check *c, int n) {
    cJSON *root = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(root, "checks");
    cJSON_AddBoolToObject(root, "ok", reconcile_ok(c, n));
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", c[i].name);
        cJSON_AddStringToObject(o, "description", c[i].description);
        cJSON_AddBoolToObject(o, "ok", c[i].violations == 0);
        cJSON_AddNumberToObject(o, "violations", c[i].violations);
        cJSON *ids = cJSON_AddArrayToObject(o, "ids");
        for (int k = 0; k < c[i].violations; k++) cJSON_AddItemToArray(ids, cJSON_CreateNumber((double)c[i].ids[k]));
        cJSON_AddItemToArray(arr, o);
    }
    return root;
}
