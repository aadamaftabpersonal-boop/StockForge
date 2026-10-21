#ifndef SF_AUDIT_H
#define SF_AUDIT_H

#include <stdint.h>

#include "cJSON.h"
#include "db.h"

/* Appends one audit row. Call inside the caller's transaction when the audited
 * change must commit atomically with it. `actor` 0 => NULL (anonymous/system).
 * `details` (not consumed) must never contain secrets or personal data beyond ids. */
int audit_log(db *d, const char *request_id, uint64_t actor, const char *action,
              const char *entity_type, const char *entity_id, const cJSON *details);

#endif
