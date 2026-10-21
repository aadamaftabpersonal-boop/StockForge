#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int env_long(const char *name, long def, long lo, long hi, long *out,
                    char *err, size_t errn) {
    const char *v = getenv(name);
    if (!v || !*v) { *out = def; return 0; }
    char *end;
    errno = 0;
    long x = strtol(v, &end, 10);
    if (errno || *end || x < lo || x > hi) {
        snprintf(err, errn, "%s must be an integer in [%ld, %ld]", name, lo, hi);
        return -1;
    }
    *out = x;
    return 0;
}

static int env_str(const char *name, const char *def, char *dst, size_t n, int required,
                   char *err, size_t errn) {
    const char *v = getenv(name);
    if (!v || !*v) {
        if (required) { snprintf(err, errn, "%s is required", name); return -1; }
        v = def;
    }
    if (strlen(v) >= n) { snprintf(err, errn, "%s is too long", name); return -1; }
    snprintf(dst, n, "%s", v);
    return 0;
}

#define LONG(field, name, def, lo, hi) \
    do { if (env_long(name, def, lo, hi, &v, err, errn)) return -1; field = (__typeof__(field))v; } while (0)

int config_load(config *cfg, char *err, size_t errn) {
    long v;
    if (env_str("SF_BIND", "127.0.0.1", cfg->bind_addr, sizeof cfg->bind_addr, 0, err, errn)) return -1;
    LONG(cfg->port, "SF_PORT", 8080, 1, 65535);
    LONG(cfg->workers, "SF_WORKERS", 8, 1, 256);
    LONG(cfg->queue_cap, "SF_QUEUE_CAP", 64, 1, 65536);
    LONG(cfg->read_timeout_ms, "SF_READ_TIMEOUT_MS", 5000, 50, 60000);
    LONG(cfg->write_timeout_ms, "SF_WRITE_TIMEOUT_MS", 5000, 50, 60000);
    LONG(cfg->max_header, "SF_MAX_HEADER", 8192, 512, 65536);
    LONG(cfg->max_body, "SF_MAX_BODY", 65536, 0, 1048576);
    LONG(cfg->debug, "SF_DEBUG", 0, 0, 1);

    LONG(cfg->db_port, "SF_DB_PORT", 3306, 1, 65535);
    LONG(cfg->cookie_secure, "SF_COOKIE_SECURE", 1, 0, 1);
    LONG(cfg->session_ttl_s, "SF_SESSION_TTL_S", 86400, 60, 2592000);
    LONG(cfg->trust_proxy, "SF_TRUST_PROXY", 0, 0, 1);
    LONG(cfg->rl_login_ip, "SF_RL_LOGIN_IP", 30, 1, 1000000);
    LONG(cfg->rl_login_acct, "SF_RL_LOGIN_ACCT", 8, 1, 1000000);
    LONG(cfg->rl_register_ip, "SF_RL_REGISTER_IP", 10, 1, 1000000);
    LONG(cfg->rl_checkout_user, "SF_RL_CHECKOUT_USER", 30, 1, 1000000);
    LONG(cfg->reservation_ttl_s, "SF_RESERVATION_TTL_S", 900, 5, 86400);
    LONG(cfg->expiry_sweep_ms, "SF_EXPIRY_SWEEP_MS", 5000, 50, 3600000);

    if (env_str("SF_DB_HOST", "127.0.0.1", cfg->db_host, sizeof cfg->db_host, 0, err, errn)) return -1;
    if (env_str("SF_DB_USER", "sf_app", cfg->db_user, sizeof cfg->db_user, 0, err, errn)) return -1;
    if (env_str("SF_DB_NAME", "stockforge", cfg->db_name, sizeof cfg->db_name, 0, err, errn)) return -1;
    if (env_str("SF_DB_PASSWORD", "", cfg->db_password, sizeof cfg->db_password, 1, err, errn)) return -1;
    if (env_str("SF_METRICS_TOKEN", "", cfg->metrics_token, sizeof cfg->metrics_token, 0, err, errn)) return -1;
    if (cfg->metrics_token[0] && strlen(cfg->metrics_token) < 16) {
        snprintf(err, errn, "SF_METRICS_TOKEN must be at least 16 characters");
        return -1;
    }
    return 0;
}
