/* Identity endpoints: register, login, logout, me. */
#include <sodium.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "audit.h"
#include "validate.h"

static cJSON *user_json(uint64_t id, const char *email, const char *name, const char *role) {
    cJSON *u = cJSON_CreateObject();
    cJSON_AddNumberToObject(u, "id", (double)id);
    cJSON_AddStringToObject(u, "email", email);
    cJSON_AddStringToObject(u, "display_name", name);
    cJSON_AddStringToObject(u, "role", role);
    return u;
}

static int too_many(rctx *c, int retry) {
    char h[40];
    snprintf(h, sizeof h, "Retry-After: %d\r\n", retry);
    c->sent = resp_error_ex(c->fd, 429, c->rid, "RATE_LIMITED", "Too many attempts. Try again later.",
                            NULL, h);
    return c->sent;
}

int h_register(rctx *c) {
    char key[96];
    int retry = 0;
    snprintf(key, sizeof key, "reg:%s", c->client_ip);
    if (!auth_rate(key, c->cfg->rl_register_ip, 60, &retry)) return too_many(c, retry);

    cJSON *b = api_body(c);
    if (!b) return c->sent;
    const char *email = NULL, *pw = NULL, *name = NULL;
    char em[256], hash[crypto_pwhash_STRBYTES] = "";
    if (api_str(c, b, "email", &email, 1) || api_str(c, b, "password", &pw, 1) ||
        api_str(c, b, "display_name", &name, 1))
        goto done;
    if (strlen(email) >= sizeof em) { api_err(c, 422, "VALIDATION_FAILED", "Invalid email address.", "email"); goto done; }
    snprintf(em, sizeof em, "%s", email);
    email_normalize(em);
    if (!valid_email(em)) { api_err(c, 422, "VALIDATION_FAILED", "Invalid email address.", "email"); goto done; }
    if (!valid_password(pw)) { api_err(c, 422, "VALIDATION_FAILED", "Password must be 10 to 128 characters.", "password"); goto done; }
    if (!valid_display_name(name)) { api_err(c, 422, "VALIDATION_FAILED", "Display name must be 1 to 100 characters.", "display_name"); goto done; }
    if (auth_hash_password(pw, hash, sizeof hash)) { api_err(c, 500, "INTERNAL", "Internal error.", NULL); goto done; }

    int64_t n = db_exec(c->db,
        "INSERT INTO users (email, password_hash, display_name, role) VALUES (?, ?, ?, 'customer')",
        "sss", em, hash, name);
    if (n != 1) {
        if (db_is_dup(c->db)) api_err(c, 409, "EMAIL_TAKEN", "An account with this email already exists.", "email");
        else api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
        goto done;
    }
    uint64_t uid = db_insert_id(c->db);
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)uid);
    audit_log(c->db, c->rid, uid, "user.registered", "user", eid, NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "user", user_json(uid, em, name, "customer"));
    api_ok(c, 201, r);
done:
    sodium_memzero((void *)c->req->body, c->req->body_len);   /* body held the password */
    if (pw) sodium_memzero((void *)pw, strlen(pw));           /* so did cJSON's copy */
    sodium_memzero(hash, sizeof hash);
    cJSON_Delete(b);
    return c->sent;
}

static void set_cookie(char *buf, size_t n, const char *token, int ttl, int secure) {
    snprintf(buf, n, "Set-Cookie: " AUTH_COOKIE "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d%s\r\n",
             token, ttl, secure ? "; Secure" : "");
}

int h_login(rctx *c) {
    char key[300];
    int retry = 0;
    snprintf(key, sizeof key, "login-ip:%s", c->client_ip);
    if (!auth_rate(key, c->cfg->rl_login_ip, 60, &retry)) return too_many(c, retry);

    cJSON *b = api_body(c);
    if (!b) return c->sent;
    const char *email = NULL, *pw = NULL;
    char em[256];
    if (api_str(c, b, "email", &email, 1) || api_str(c, b, "password", &pw, 1)) goto done;
    if (strlen(email) >= sizeof em || strlen(pw) > 1024) { api_err(c, 401, "INVALID_CREDENTIALS", "Invalid email or password.", NULL); goto done; }
    snprintf(em, sizeof em, "%s", email);
    email_normalize(em);
    snprintf(key, sizeof key, "login-acct:%s", em);
    if (!auth_rate(key, c->cfg->rl_login_acct, 300, &retry)) { too_many(c, retry); goto done; }

    uint64_t uid = 0, active = 0;
    char hash[crypto_pwhash_STRBYTES] = "", name[128] = "", role[16] = "";
    db_rs *rs = db_query(c->db,
        "SELECT id, password_hash, display_name, role, is_active FROM users WHERE email = ?", "s", em);
    if (!rs) { api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL); goto done; }
    int found = db_rs_next(rs, "usssu", &uid, hash, sizeof hash, name, sizeof name, role, sizeof role, &active);
    db_rs_free(rs);
    if (found < 0) { api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL); goto done; }

    /* Unknown account still burns one Argon2 verification, so response time
     * does not reveal whether the email exists. Same error for every failure. */
    int ok = 0;
    if (found == 1) ok = auth_verify_password(hash, pw) && active;
    else auth_verify_dummy(pw);
    if (!ok) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "reason", found == 1 ? (active ? "bad_password" : "inactive") : "unknown_account");
        char eid[24] = "0";
        if (found == 1) snprintf(eid, sizeof eid, "%llu", (unsigned long long)uid);
        audit_log(c->db, c->rid, found == 1 ? uid : 0, "auth.login_failed", "user", eid, d);
        cJSON_Delete(d);
        api_err(c, 401, "INVALID_CREDENTIALS", "Invalid email or password.", NULL);
        goto done;
    }

    char token[AUTH_TOKEN_LEN + 1], csrf[AUTH_TOKEN_LEN + 1];
    if (auth_new_session(c->db, uid, c->cfg->session_ttl_s, token, sizeof token, csrf, sizeof csrf)) {
        api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
        goto done;
    }
    char eid[24];
    snprintf(eid, sizeof eid, "%llu", (unsigned long long)uid);
    audit_log(c->db, c->rid, uid, "auth.login", "user", eid, NULL);

    char ck[256];
    set_cookie(ck, sizeof ck, token, c->cfg->session_ttl_s, c->cfg->cookie_secure);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "user", user_json(uid, em, name, role));
    cJSON_AddStringToObject(r, "csrf_token", csrf);
    c->sent = resp_json(c->fd, 200, c->rid, r, ck);
    sodium_memzero(token, sizeof token);
done:
    sodium_memzero((void *)c->req->body, c->req->body_len);
    if (pw) sodium_memzero((void *)pw, strlen(pw));
    cJSON_Delete(b);
    return c->sent;
}

int h_logout(rctx *c) {
    auth_delete_session(c->db, c->req);
    char ck[160];
    snprintf(ck, sizeof ck, "Set-Cookie: " AUTH_COOKIE "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0%s\r\n",
             c->cfg->cookie_secure ? "; Secure" : "");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    c->sent = resp_json(c->fd, 200, c->rid, r, ck);
    return c->sent;
}

int h_me(rctx *c) {
    char email[256] = "", name[128] = "", role[16] = "";
    db_rs *rs = db_query(c->db, "SELECT email, display_name, role FROM users WHERE id = ?", "u", c->user.user_id);
    if (!rs) return api_err(c, 503, "DB_UNAVAILABLE", "Service temporarily unavailable.", NULL);
    int f = db_rs_next(rs, "sss", email, sizeof email, name, sizeof name, role, sizeof role);
    db_rs_free(rs);
    if (f != 1) return api_err(c, 401, "UNAUTHENTICATED", "Sign in required.", NULL);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "user", user_json(c->user.user_id, email, name, role));
    cJSON_AddStringToObject(r, "csrf_token", c->user.csrf);
    return api_ok(c, 200, r);
}
