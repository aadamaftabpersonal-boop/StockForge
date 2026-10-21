#ifndef SF_SWEEPER_H
#define SF_SWEEPER_H

#include "config.h"

/* Background thread that expires overdue PENDING_PAYMENT orders and releases
 * their stock. It owns its own database connection. Correctness does not
 * depend on it running promptly: payment re-checks the deadline under the
 * order lock, so an overdue-but-unswept order can never be paid. */
int  sweeper_start(const config *cfg);
void sweeper_stop(void);       /* wakes the thread, waits for it to exit */

#endif
