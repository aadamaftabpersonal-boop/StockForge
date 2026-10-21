#ifndef SF_RECONCILE_H
#define SF_RECONCILE_H

#include <stdint.h>

#include "cJSON.h"
#include "db.h"

/* Cross-table invariant checks. Each check is one SQL query that returns the
 * ids of rows VIOLATING an invariant (empty result = invariant holds).
 * These are the same invariants docs/CHECKOUT.md promises; the database
 * constraints prevent some of them outright, and the rest (those spanning
 * tables) can only be verified here. */
#define RC_MAX_IDS 20
#define RC_MAX_CHECKS 16

typedef struct {
    const char *name;
    const char *description;
    int         violations;          /* number of ids found, capped at RC_MAX_IDS */
    uint64_t    ids[RC_MAX_IDS];
} rc_check;

/* Runs every check; returns the number of checks run, or -1 on database error. */
int    reconcile_run(db *d, rc_check *out, int max);
int    reconcile_ok(const rc_check *c, int n);
cJSON *reconcile_json(const rc_check *c, int n);

#endif
