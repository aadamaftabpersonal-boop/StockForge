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

int config_load(config *cfg, char *err, size_t errn) {
    long v;
    const char *b = getenv("SF_BIND");
    snprintf(cfg->bind_addr, sizeof cfg->bind_addr, "%s", (b && *b) ? b : "127.0.0.1");

    if (env_long("SF_PORT", 8080, 1, 65535, &v, err, errn)) return -1;
    cfg->port = (int)v;
    if (env_long("SF_WORKERS", 8, 1, 256, &v, err, errn)) return -1;
    cfg->workers = (int)v;
    if (env_long("SF_QUEUE_CAP", 64, 1, 65536, &v, err, errn)) return -1;
    cfg->queue_cap = (int)v;
    if (env_long("SF_READ_TIMEOUT_MS", 5000, 50, 60000, &v, err, errn)) return -1;
    cfg->read_timeout_ms = (int)v;
    if (env_long("SF_WRITE_TIMEOUT_MS", 5000, 50, 60000, &v, err, errn)) return -1;
    cfg->write_timeout_ms = (int)v;
    if (env_long("SF_MAX_HEADER", 8192, 512, 65536, &v, err, errn)) return -1;
    cfg->max_header = (size_t)v;
    if (env_long("SF_MAX_BODY", 65536, 0, 1048576, &v, err, errn)) return -1;
    cfg->max_body = (size_t)v;
    if (env_long("SF_DEBUG", 0, 0, 1, &v, err, errn)) return -1;
    cfg->debug = (int)v;
    return 0;
}
