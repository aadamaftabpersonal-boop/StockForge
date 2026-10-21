#include "auth.h"

#include <sodium.h>
#include <string.h>
#include <time.h>

#include "ratelimit.h"
#include "validate.h"

static char g_dummy_hash[crypto_pwhash_STRBYTES];
static rl  *g_rl;

int auth_global_init(void) {
    if (sodium_init() < 0) return -1;
    char pw[24];
    randombytes_buf(pw, sizeof pw);
    pw[sizeof pw - 1] = 0;
    for (size_t i = 0; i < sizeof pw - 1; i++) pw[i] = (char)('a' + (unsigned char)pw[i] % 26);
    if (auth_hash_password(pw, g_dummy_hash, sizeof g_dummy_hash)) return -1;
    g_rl = rl_new(8192);
    return g_rl ? 0 : -1;
}

void auth_global_free(void) { rl_free(g_rl); g_rl = NULL; }

int auth_hash_password(const char *pw, char *out, size_t outn) {
    if (outn < crypto_pwhash_STRBYTES) return -1;
    return crypto_pwhash_str(out, pw, strlen(pw), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0 ? 0 : -1;
}

int auth_verify_password(const char *hash, const char *pw) {
    return crypto_pwhash_str_verify(hash, pw, strlen(pw)) == 0;
}

void auth_verify_dummy(const char *pw) {
    int r = crypto_pwhash_str_verify(g_dummy_hash, pw, strlen(pw));   /* result intentionally unused */
    (void)r;
}

static void new_token(char *out, size_t n) {
    unsigned char raw[32];
    randombytes_buf(raw, sizeof raw);
    sodium_bin2base64(out, n, raw, sizeof raw, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_memzero(raw, sizeof raw);
}

static void hash_token(const char *tok, unsigned char out[32]) {
    crypto_hash_sha256(out, (const unsigned char *)tok, strlen(tok));
}

int auth_new_session(db *d, uint64_t uid, int ttl, char *token, size_t tn, char *csrf, size_t cn) {
    if (tn < AUTH_TOKEN_LEN + 1 || cn < AUTH_TOKEN_LEN + 1) return -1;
    unsigned char h[32];
    new_token(token, tn);
    new_token(csrf, cn);
    hash_token(token, h);
    /* housekeeping: drop this user's expired sessions */
    db_exec(d, "DELETE FROM sessions WHERE user_id=? AND expires_at < NOW(3)", "u", uid);
    int64_t r = db_exec(d,
        "INSERT INTO sessions (token_hash, user_id, csrf_token, expires_at) "
        "VALUES (?, ?, ?, NOW(3) + INTERVAL ? SECOND)",
        "Bus" "i", h, sizeof h, uid, csrf, (int64_t)ttl);
    if (r != 1) return -1;
    /* Bound the table: keep the 10 newest sessions per user. The 10th-newest
     * created_at is the cut-off; with fewer than 10 sessions the sub-select is
     * NULL and nothing is deleted. */
    db_exec(d,
        "DELETE FROM sessions WHERE user_id = ? AND created_at < "
        "(SELECT c FROM (SELECT created_at c FROM sessions WHERE user_id = ? "
        " ORDER BY created_at DESC LIMIT 1 OFFSET 9) t)", "uu", uid, uid);
    return 0;
}

int auth_load_session(db *d, const http_req *r, auth_user *out) {
    memset(out, 0, sizeof *out);
    size_t hl;
    const char *ch = http_header(r, "cookie", &hl);
    char tok[AUTH_TOKEN_LEN + 8];
    if (!ch || !cookie_get(ch, hl, AUTH_COOKIE, tok, sizeof tok) || strlen(tok) != AUTH_TOKEN_LEN)
        return 0;
    unsigned char h[32];
    hash_token(tok, h);
    db_rs *rs = db_query(d,
        "SELECT s.user_id, (u.role = 'admin'), s.csrf_token FROM sessions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = ? AND s.expires_at > NOW(3) AND u.is_active = 1",
        "B", h, sizeof h);
    if (!rs) return -1;
    uint64_t uid = 0, adm = 0;
    int rc = db_rs_next(rs, "uus", &uid, &adm, out->csrf, sizeof out->csrf);
    db_rs_free(rs);
    if (rc < 0) return -1;
    if (rc == 0) return 0;
    out->authed = 1;
    out->user_id = uid;
    out->is_admin = adm != 0;
    return 1;
}

int auth_delete_session(db *d, const http_req *r) {
    size_t hl;
    const char *ch = http_header(r, "cookie", &hl);
    char tok[AUTH_TOKEN_LEN + 8];
    if (!ch || !cookie_get(ch, hl, AUTH_COOKIE, tok, sizeof tok)) return 0;
    unsigned char h[32];
    hash_token(tok, h);
    return db_exec(d, "DELETE FROM sessions WHERE token_hash = ?", "B", h, sizeof h) < 0 ? -1 : 0;
}

int auth_csrf_ok(const auth_user *u, const http_req *r) {
    size_t vl;
    const char *v = http_header(r, "x-csrf-token", &vl);
    size_t n = strlen(u->csrf);
    if (!v || vl != n || n == 0) return 0;
    return sodium_memcmp(v, u->csrf, n) == 0;
}

int auth_rate(const char *key, int limit, int window_s, int *retry_after) {
    return rl_allow(g_rl, key, limit, window_s, (uint64_t)time(NULL), retry_after);
}
