#include "resp.h"

#include <string.h>

#include "http.h"

int resp_json(int fd, int status, const char *rid, cJSON *obj, const char *extra) {
    char *s = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (!s) {
        http_send_error(fd, 500, "INTERNAL", "Internal error.", rid);
        return 500;
    }
    http_send(fd, status, "application/json", s, strlen(s), rid, extra);
    cJSON_free(s);
    return status;
}

int resp_error(int fd, int status, const char *rid, const char *code, const char *message,
               const char *field) {
    return resp_error_ex(fd, status, rid, code, message, field,
                         status == 503 ? "Retry-After: 1\r\n" : NULL);
}

int resp_error_ex(int fd, int status, const char *rid, const char *code, const char *message,
                  const char *field, const char *extra) {
    cJSON *root = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(root, "error");
    if (!e) { cJSON_Delete(root); http_send_error(fd, 500, "INTERNAL", "Internal error.", rid); return 500; }
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    if (field) cJSON_AddStringToObject(e, "field", field);
    cJSON_AddStringToObject(e, "request_id", rid);
    return resp_json(fd, status, rid, root, extra);
}
