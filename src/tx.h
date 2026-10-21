#ifndef SF_TX_H
#define SF_TX_H

#include "db.h"

/* Transaction runner with bounded deadlock retry.
 *
 * A transaction body does `db_begin`, its work, and `db_commit`. It returns:
 *   TX_DONE   finished (committed OR deliberately rolled back for a business
 *             reason such as insufficient stock); the outcome is in its argument
 *   TX_RETRY  MySQL chose it as a deadlock victim / lock wait timed out; it has
 *             ALREADY rolled back. The runner re-runs it from scratch
 *   TX_FAIL   any other database failure; already rolled back
 *
 * The body must be safe to re-run from the top: it may not keep side effects
 * outside the database (it can reset its own output at entry).
 *
 * tx_run() returns TX_DONE, TX_FAIL, or TX_RETRY when retries were exhausted. */
enum { TX_DONE = 0, TX_RETRY = 1, TX_FAIL = 2 };

#define TX_MAX_ATTEMPTS 6

typedef int (*tx_body)(db *d, void *arg);

int tx_run(db *d, tx_body fn, void *arg);

/* Call after a failed statement inside a body: rolls back and classifies
 * 1213 (deadlock) / 1205 (lock wait timeout) as TX_RETRY, everything else TX_FAIL. */
int tx_classify(db *d);

#endif
