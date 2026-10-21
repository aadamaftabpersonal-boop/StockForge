#ifndef SF_CONFIG_H
#define SF_CONFIG_H

#include <stddef.h>

typedef struct {
    char   bind_addr[64];
    int    port;
    int    workers;           /* worker threads */
    int    queue_cap;         /* max accepted-but-unserved connections */
    int    read_timeout_ms;   /* total deadline to receive one full request */
    int    write_timeout_ms;  /* per-send timeout */
    size_t max_header;        /* bytes, request line + headers */
    size_t max_body;          /* bytes */
    int    debug;             /* enables the /debug routes; never in production */
} config;

/* Fills cfg from SF_* environment variables. Returns 0, or -1 with a message in err. */
int config_load(config *cfg, char *err, size_t errn);

#endif
