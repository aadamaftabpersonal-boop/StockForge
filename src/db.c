#include "db.h"

#include <mysql.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

#define MAX_PARAMS 24
#define MAX_COLS 24

struct db {
    MYSQL   *my;
    int      in_tx;
    unsigned last_errno;
    uint64_t insert_id;
    int64_t  affected;
};

struct db_rs {
    MYSQL_STMT *st;
    unsigned    ncols;
    db         *owner;
};

typedef db_param param;

void dbp_i(db_params *p, int64_t v) { if (p->n < 24) { p->p[p->n].c = 'i'; p->p[p->n++].v.i = v; } }
void dbp_u(db_params *p, uint64_t v) { if (p->n < 24) { p->p[p->n].c = 'u'; p->p[p->n++].v.u = v; } }
void dbp_s(db_params *p, const char *v) { if (p->n < 24) { p->p[p->n].c = 's'; p->p[p->n++].v.s = v; } }

static const config *g_cfg;
static __thread db tl_db;

int db_global_init(const config *cfg) {
    g_cfg = cfg;
    return mysql_library_init(0, NULL, NULL) ? -1 : 0;
}

void db_thread_init(void) { mysql_thread_init(); }

static void drop_conn(db *d) {
    if (d->my) { mysql_close(d->my); d->my = NULL; }
    d->in_tx = 0;     /* a dropped connection implicitly rolled back */
}

void db_thread_end(void) {
    drop_conn(&tl_db);
    mysql_thread_end();
}

static void note_error(db *d, const char *where) {
    d->last_errno = d->my ? mysql_errno(d->my) : 0;
    char m[256];
    snprintf(m, sizeof m, "%s: errno=%u %s", where, d->last_errno, d->my ? mysql_error(d->my) : "no connection");
    /* deadlock (1213) and lock-wait timeout (1205) are expected under contention and are retried */
    log_msg(d->last_errno == 1213 || d->last_errno == 1205 ? "warn" : "error", "db", m);
}

static int is_conn_error(unsigned e) {
    return e == 2002 || e == 2003 || e == 2006 || e == 2013 || e == 2055 || e == 1053 || e == 1317;
}

static int connect_db(db *d) {
    d->my = mysql_init(NULL);
    if (!d->my) return -1;
    unsigned ct = 3, rt = 15, wt = 15;
    mysql_options(d->my, MYSQL_OPT_CONNECT_TIMEOUT, &ct);
    mysql_options(d->my, MYSQL_OPT_READ_TIMEOUT, &rt);
    mysql_options(d->my, MYSQL_OPT_WRITE_TIMEOUT, &wt);
    if (!mysql_real_connect(d->my, g_cfg->db_host, g_cfg->db_user, g_cfg->db_password,
                            g_cfg->db_name, (unsigned)g_cfg->db_port, NULL, 0)) {
        note_error(d, "connect");
        mysql_close(d->my);
        d->my = NULL;
        return -1;
    }
    mysql_set_character_set(d->my, "utf8mb4");
    /* A bounded lock wait: better to fail fast and retry than to park a worker for the 50 s default. */
    if (mysql_query(d->my, "SET SESSION time_zone='+00:00', "
                           "SESSION transaction_isolation='READ-COMMITTED', "
                           "SESSION innodb_lock_wait_timeout=10")) {
        note_error(d, "session setup");
        drop_conn(d);
        return -1;
    }
    return 0;
}

db *db_get(void) {
    if (!tl_db.my && connect_db(&tl_db) != 0) return NULL;
    return &tl_db;
}

int db_ping(db *d) {
    if (!d->my && connect_db(d) != 0) return -1;
    if (mysql_query(d->my, "SELECT 1")) {
        note_error(d, "ping");
        if (is_conn_error(d->last_errno)) drop_conn(d);
        return -1;
    }
    MYSQL_RES *r = mysql_store_result(d->my);
    if (r) mysql_free_result(r);
    return 0;
}

/* ------------------------------------------------------------ statements */

static int collect(const char *pfmt, va_list ap, param *out) {
    int n = 0;
    for (const char *f = pfmt ? pfmt : ""; *f; f++) {
        if (n >= MAX_PARAMS) return -1;
        out[n].c = *f;
        switch (*f) {
        case 'i': out[n].v.i = va_arg(ap, int64_t); break;
        case 'u': out[n].v.u = va_arg(ap, uint64_t); break;
        case 's': out[n].v.s = va_arg(ap, const char *); break;
        case 'U': out[n].v.pu = va_arg(ap, const uint64_t *); break;
        case 'B': out[n].v.b.p = va_arg(ap, const void *); out[n].v.b.n = va_arg(ap, size_t); break;
        default: return -1;
        }
        n++;
    }
    return n;
}

/* Prepares and executes. On success returns the live statement. */
static MYSQL_STMT *run(db *d, const char *sql, const param *p, int np) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!d->my && connect_db(d) != 0) return NULL;
        MYSQL_STMT *st = mysql_stmt_init(d->my);
        if (!st) return NULL;
        if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql))) {
            note_error(d, "prepare");
            mysql_stmt_close(st);
            if (is_conn_error(d->last_errno)) {
                int was_tx = d->in_tx;
                drop_conn(d);
                if (!was_tx && attempt == 0) continue;   /* nothing executed: safe to retry */
            }
            return NULL;
        }
        if ((int)mysql_stmt_param_count(st) != np) {
            log_msg("error", "db", "placeholder/parameter count mismatch");
            mysql_stmt_close(st);
            return NULL;
        }
        MYSQL_BIND b[MAX_PARAMS];
        unsigned long len[MAX_PARAMS];
        my_bool isnull[MAX_PARAMS];
        memset(b, 0, sizeof b);
        for (int i = 0; i < np; i++) {
            isnull[i] = 0;
            b[i].is_null = &isnull[i];
            switch (p[i].c) {
            case 'i': b[i].buffer_type = MYSQL_TYPE_LONGLONG; b[i].buffer = (void *)&p[i].v.i; break;
            case 'u': b[i].buffer_type = MYSQL_TYPE_LONGLONG; b[i].is_unsigned = 1;
                      b[i].buffer = (void *)&p[i].v.u; break;
            case 'U': b[i].buffer_type = MYSQL_TYPE_LONGLONG; b[i].is_unsigned = 1;
                      if (p[i].v.pu) b[i].buffer = (void *)p[i].v.pu; else isnull[i] = 1;
                      break;
            case 's': b[i].buffer_type = MYSQL_TYPE_STRING;
                      if (p[i].v.s) { len[i] = (unsigned long)strlen(p[i].v.s);
                                      b[i].buffer = (void *)p[i].v.s; b[i].length = &len[i]; }
                      else isnull[i] = 1;
                      break;
            case 'B': b[i].buffer_type = MYSQL_TYPE_BLOB; len[i] = (unsigned long)p[i].v.b.n;
                      b[i].buffer = (void *)p[i].v.b.p; b[i].length = &len[i]; break;
            }
        }
        if ((np && mysql_stmt_bind_param(st, b)) || mysql_stmt_execute(st)) {
            note_error(d, "execute");
            if (is_conn_error(d->last_errno)) drop_conn(d);   /* never retried: may have run */
            mysql_stmt_close(st);
            return NULL;
        }
        d->affected = (int64_t)mysql_stmt_affected_rows(st);
        d->insert_id = mysql_stmt_insert_id(st);
        return st;
    }
    return NULL;
}

int64_t db_exec(db *d, const char *sql, const char *pfmt, ...) {
    param p[MAX_PARAMS];
    va_list ap;
    va_start(ap, pfmt);
    int np = collect(pfmt, ap, p);
    va_end(ap);
    if (np < 0) return -1;
    MYSQL_STMT *st = run(d, sql, p, np);
    if (!st) return -1;
    mysql_stmt_close(st);
    return d->affected;
}

static db_rs *finish_query(db *d, MYSQL_STMT *st);

db_rs *db_query(db *d, const char *sql, const char *pfmt, ...) {
    param p[MAX_PARAMS];
    va_list ap;
    va_start(ap, pfmt);
    int np = collect(pfmt, ap, p);
    va_end(ap);
    if (np < 0) return NULL;
    return finish_query(d, run(d, sql, p, np));
}

db_rs *db_query_p(db *d, const char *sql, const db_params *p) {
    return finish_query(d, run(d, sql, p->p, p->n));
}

static db_rs *finish_query(db *d, MYSQL_STMT *st) {
    if (!st) return NULL;
    if (mysql_stmt_store_result(st)) {
        note_error(d, "store_result");
        if (is_conn_error(d->last_errno)) drop_conn(d);
        mysql_stmt_close(st);
        return NULL;
    }
    db_rs *rs = malloc(sizeof *rs);
    if (!rs) { mysql_stmt_close(st); return NULL; }
    rs->st = st;
    rs->ncols = mysql_stmt_field_count(st);
    rs->owner = d;
    return rs;
}

int db_rs_next(db_rs *rs, const char *rfmt, ...) {
    MYSQL_BIND b[MAX_COLS];
    unsigned long len[MAX_COLS];
    my_bool isnull[MAX_COLS];
    char *sbuf[MAX_COLS];
    size_t ssize[MAX_COLS];
    memset(b, 0, sizeof b);
    memset(sbuf, 0, sizeof sbuf);
    size_t n = strlen(rfmt);
    if (n != rs->ncols || n > MAX_COLS) {
        log_msg("error", "db", "result column count mismatch");
        return -1;
    }
    va_list ap;
    va_start(ap, rfmt);
    for (size_t i = 0; i < n; i++) {
        b[i].is_null = &isnull[i];
        b[i].length = &len[i];
        switch (rfmt[i]) {
        case 'i': b[i].buffer_type = MYSQL_TYPE_LONGLONG; b[i].buffer = va_arg(ap, int64_t *); break;
        case 'u': b[i].buffer_type = MYSQL_TYPE_LONGLONG; b[i].is_unsigned = 1;
                  b[i].buffer = va_arg(ap, uint64_t *); break;
        case 's': { char *s = va_arg(ap, char *); size_t sz = va_arg(ap, size_t);
                    b[i].buffer_type = MYSQL_TYPE_STRING; b[i].buffer = s;
                    b[i].buffer_length = sz ? sz - 1 : 0; sbuf[i] = s; ssize[i] = sz; break; }
        case 'B': { void *s = va_arg(ap, void *); size_t sz = va_arg(ap, size_t);
                    b[i].buffer_type = MYSQL_TYPE_BLOB; b[i].buffer = s; b[i].buffer_length = sz; break; }
        default: va_end(ap); return -1;
        }
    }
    va_end(ap);
    if (mysql_stmt_bind_result(rs->st, b)) { note_error(rs->owner, "bind_result"); return -1; }
    int rc = mysql_stmt_fetch(rs->st);
    if (rc == MYSQL_NO_DATA) return 0;
    if (rc == 1) { note_error(rs->owner, "fetch"); return -1; }
    for (size_t i = 0; i < n; i++) {                       /* rc==MYSQL_DATA_TRUNCATED is fine for strings */
        if (!sbuf[i]) { if (isnull[i] && rfmt[i] != 'B') *(int64_t *)b[i].buffer = 0; continue; }
        size_t l = isnull[i] ? 0 : len[i];
        if (l > ssize[i] - 1) l = ssize[i] - 1;
        sbuf[i][l] = 0;
    }
    return 1;
}

void db_rs_free(db_rs *rs) {
    if (!rs) return;
    mysql_stmt_free_result(rs->st);
    mysql_stmt_close(rs->st);
    free(rs);
}

uint64_t db_insert_id(db *d) { return d->insert_id; }
unsigned db_errno(db *d) { return d->last_errno; }
int db_is_dup(db *d) { return d->last_errno == 1062; }

int db_begin(db *d) {
    if (!d->my && connect_db(d) != 0) return -1;
    if (mysql_query(d->my, "START TRANSACTION")) {
        note_error(d, "begin");
        if (is_conn_error(d->last_errno)) drop_conn(d);
        return -1;
    }
    d->in_tx = 1;
    return 0;
}

int db_commit(db *d) {
    int rc = (d->my && mysql_commit(d->my)) ? -1 : 0;
    if (rc) {
        note_error(d, "commit");
        if (is_conn_error(d->last_errno)) drop_conn(d);   /* outcome unknown: caller must treat as such */
    }
    d->in_tx = 0;
    return rc;
}

void db_rollback(db *d) {
    if (d->my && d->in_tx) mysql_rollback(d->my);
    d->in_tx = 0;
}
