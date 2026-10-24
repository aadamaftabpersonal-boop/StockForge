#ifndef SF_DB_H
#define SF_DB_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Thin layer over libmariadb prepared statements.
 *
 *  - Every statement is prepared with `?` placeholders; user data is never
 *    concatenated into SQL (the only dynamic SQL is built from constant
 *    fragments in catalog.c).
 *  - One connection per worker thread (thread-local); nothing is shared.
 *  - Retry policy: a dead connection is re-established and the call retried
 *    ONLY if the failure happened at PREPARE time (nothing ran yet) and we are
 *    not inside a transaction. A failure during EXECUTE is never retried: the
 *    statement may have run, so replaying it could double-apply it. Callers
 *    see an error and (for checkout) rely on idempotency keys.
 *
 * Parameter format string, one char per `?`:
 *   i  int64_t            u  uint64_t           s  const char* (NULL => SQL NULL)
 *   U  const uint64_t* (NULL => SQL NULL)      B  const void*, size_t  (binary)
 * Result format string, one char per selected column (must match column count):
 *   i  int64_t*           u  uint64_t*
 *   s  char*, size_t      (NUL-terminated, truncated to size-1; NULL => "")
 *   B  void*, size_t      (raw bytes, up to size)
 *   NULL integers read as 0.                                                  */

typedef struct db db;
typedef struct db_rs db_rs;

/* Parameter list for statements whose placeholder count is decided at runtime
 * (e.g. optional filters). The SQL text may only ever be assembled from constant
 * fragments; all values go through here. */
typedef struct {
    char c;
    union { int64_t i; uint64_t u; const char *s; const uint64_t *pu;
            struct { const void *p; size_t n; } b; } v;
} db_param;
typedef struct { db_param p[24]; int n; } db_params;

void dbp_i(db_params *p, int64_t v);
void dbp_u(db_params *p, uint64_t v);
void dbp_s(db_params *p, const char *v);     /* pointer must outlive the call */

int  db_global_init(const config *cfg);   /* once, from main, before threads */
void db_thread_init(void);                /* once per worker thread */
void db_thread_end(void);                 /* closes that thread's connection */

db  *db_get(void);                        /* this thread's connection; NULL if DB unreachable */
int  db_ping(db *d);                      /* 0 ok */

int64_t  db_exec(db *d, const char *sql, const char *pfmt, ...);   /* affected rows, or -1 */
db_rs   *db_query(db *d, const char *sql, const char *pfmt, ...);  /* NULL on error */
db_rs   *db_query_p(db *d, const char *sql, const db_params *p);   /* NULL on error */
int64_t  db_exec_p(db *d, const char *sql, const db_params *p);    /* affected rows, or -1 */
int      db_rs_next(db_rs *rs, const char *rfmt, ...);             /* 1 row, 0 done, -1 error */
void     db_rs_free(db_rs *rs);

uint64_t db_insert_id(db *d);             /* of the last db_exec */
unsigned db_errno(db *d);                 /* of the last failure */
int      db_is_dup(db *d);                /* last failure was a unique-key violation (1062) */

int  db_begin(db *d);                     /* 0 ok */
int  db_commit(db *d);
void db_rollback(db *d);

#endif
