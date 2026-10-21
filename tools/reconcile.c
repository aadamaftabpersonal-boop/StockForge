/* Offline reconciliation: connects with the SF_DB_* environment and verifies
 * the cross-table invariants. Prints JSON; exit status 0 = all hold, 1 = a
 * violation, 2 = could not run. Safe to run against a live database (read-only).
 *   build/rel/reconcile
 */
#include <stdio.h>

#include "config.h"
#include "db.h"
#include "log.h"
#include "reconcile.h"

int main(void) {
    config cfg;
    char err[160];
    if (config_load(&cfg, err, sizeof err)) { fprintf(stderr, "config: %s\n", err); return 2; }
    if (db_global_init(&cfg)) return 2;
    db_thread_init();
    db *d = db_get();
    if (!d) { fprintf(stderr, "cannot connect to the database\n"); return 2; }
    rc_check checks[RC_MAX_CHECKS];
    int n = reconcile_run(d, checks, RC_MAX_CHECKS);
    if (n < 0) { fprintf(stderr, "reconcile query failed\n"); return 2; }
    cJSON *j = reconcile_json(checks, n);
    char *s = cJSON_Print(j);
    puts(s);
    int ok = reconcile_ok(checks, n);
    cJSON_free(s);
    cJSON_Delete(j);
    db_thread_end();
    return ok ? 0 : 1;
}
