#include "audit.h"

int audit_log(db *d, const char *rid, uint64_t actor, const char *action,
              const char *etype, const char *eid, const cJSON *details) {
    char *js = details ? cJSON_PrintUnformatted(details) : NULL;
    int64_t r = db_exec(d,
        "INSERT INTO audit_events (request_id, actor_user_id, action, entity_type, entity_id, details) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        "sUssss", rid, actor ? &actor : NULL, action, etype, eid, js);
    cJSON_free(js);
    return r == 1 ? 0 : -1;
}
