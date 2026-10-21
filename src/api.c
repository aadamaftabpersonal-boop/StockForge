#include "api.h"

#include <math.h>
#include <string.h>

#include "validate.h"

int api_session(rctx *c) {
    if (!c->session_state) {
        c->session_rc = auth_load_session(c->db, c->req, &c->user);
        c->session_state = 1;
    }
    return c->session_rc;
}

int api_err(rctx *c, int status, const char *code, const char *msg, const char *field) {
    c->sent = resp_error(c->fd, status, c->rid, code, msg, field);
    return c->sent;
}

int api_ok(rctx *c, int status, cJSON *obj) {
    c->sent = resp_json(c->fd, status, c->rid, obj, NULL);
    return c->sent;
}

cJSON *api_body(rctx *c) {
    if (c->req->body_len == 0) { api_err(c, 400, "EMPTY_BODY", "A JSON body is required.", NULL); return NULL; }
    cJSON *o = cJSON_ParseWithLength(c->req->body, c->req->body_len);
    if (!o || !cJSON_IsObject(o)) {
        cJSON_Delete(o);
        api_err(c, 400, "INVALID_JSON", "Body must be a JSON object.", NULL);
        return NULL;
    }
    return o;
}

int api_str(rctx *c, const cJSON *o, const char *name, const char **out, int required) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(o, name);
    if (!f || cJSON_IsNull(f)) {
        if (!required) { *out = NULL; return 0; }
        api_err(c, 422, "VALIDATION_FAILED", "This field is required.", name);
        return -1;
    }
    if (!cJSON_IsString(f) || !f->valuestring) {
        api_err(c, 422, "VALIDATION_FAILED", "Must be a string.", name);
        return -1;
    }
    *out = f->valuestring;
    return 0;
}

int api_int(rctx *c, const cJSON *o, const char *name, int64_t lo, int64_t hi, int64_t *out) {
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(o, name);
    if (!f || !cJSON_IsNumber(f) || f->valuedouble != floor(f->valuedouble) ||
        fabs(f->valuedouble) > 9007199254740991.0) {
        api_err(c, 422, "VALIDATION_FAILED", "Must be an integer.", name);
        return -1;
    }
    int64_t v = (int64_t)f->valuedouble;
    if (v < lo || v > hi) { api_err(c, 422, "VALIDATION_FAILED", "Out of allowed range.", name); return -1; }
    *out = v;
    return 0;
}

int api_seg_id(rctx *c, int idx, uint64_t *out) {
    if (idx >= c->nseg || parse_u64(c->seg[idx], c->seglen[idx], out) != 0 || *out == 0) {
        api_err(c, 404, "NOT_FOUND", "No such resource.", NULL);
        return -1;
    }
    return 0;
}

int api_qs(rctx *c, const char *key, char *out, size_t outn) {
    return qs_get(c->req->query, c->req->query_len, key, out, outn);
}

int api_limit(rctx *c, int def, int max, int *out) {
    char v[16];
    int r = api_qs(c, "limit", v, sizeof v);
    if (r < 0) { api_err(c, 422, "VALIDATION_FAILED", "Invalid limit.", "limit"); return -1; }
    *out = def;
    if (r == 1) {
        uint64_t n;
        if (parse_u64(v, strlen(v), &n) != 0 || n < 1) {
            api_err(c, 422, "VALIDATION_FAILED", "Invalid limit.", "limit");
            return -1;
        }
        *out = n > (uint64_t)max ? max : (int)n;     /* clamp, never trust the client's size */
    }
    return 0;
}
