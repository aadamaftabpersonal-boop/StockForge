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

    /* database: secrets come from the environment only */
    char   db_host[128];
    int    db_port;
    char   db_user[64];
    char   db_password[128];
    char   db_name[64];

    /* web security */
    int    cookie_secure;         /* Secure flag on session cookie (needs HTTPS in front) */
    int    session_ttl_s;
    int    trust_proxy;           /* take client IP from the last X-Forwarded-For entry */
    char   metrics_token[128];    /* optional bearer token for /metrics */
    int    rl_login_ip;           /* login attempts per IP per minute */
    int    rl_login_acct;         /* login attempts per account per 5 minutes */
    int    rl_register_ip;        /* registrations per IP per minute */
} config;

/* Fills cfg from SF_* environment variables. Returns 0, or -1 with a message in err. */
int config_load(config *cfg, char *err, size_t errn);

#endif
