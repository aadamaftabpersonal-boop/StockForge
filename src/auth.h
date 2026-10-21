#ifndef SF_AUTH_H
#define SF_AUTH_H

#include <stddef.h>
#include <stdint.h>

#include "db.h"
#include "http.h"

typedef struct {
    int      authed;
    uint64_t user_id;
    int      is_admin;
    char     csrf[48];     /* base64url(32 bytes) */
} auth_user;

#define AUTH_TOKEN_LEN 43   /* base64url(32 random bytes), no padding */
#define AUTH_COOKIE "sf_sid"

int  auth_global_init(void);                 /* sodium_init, dummy hash, rate limiter */
void auth_global_free(void);

/* Argon2id via libsodium (interactive limits). out must hold 128 bytes. */
int  auth_hash_password(const char *pw, char *out, size_t outn);
int  auth_verify_password(const char *hash, const char *pw);   /* 1 ok, 0 wrong */
void auth_verify_dummy(const char *pw);      /* equalise timing for unknown accounts */

/* Sessions: the cookie carries a random token; only its SHA-256 is stored. */
int  auth_new_session(db *d, uint64_t user_id, int ttl_s, char *token, size_t tokn,
                      char *csrf, size_t csrfn);                /* 0 ok */
int  auth_load_session(db *d, const http_req *r, auth_user *out); /* 1 yes, 0 no, -1 db error */
int  auth_delete_session(db *d, const http_req *r);

int  auth_csrf_ok(const auth_user *u, const http_req *r);   /* constant-time compare */

/* Fixed-window rate limit, process-wide. 1 allowed, 0 denied (retry_after set). */
int  auth_rate(const char *key, int limit, int window_s, int *retry_after);

#endif
