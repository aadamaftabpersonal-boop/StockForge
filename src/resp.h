#ifndef SF_RESP_H
#define SF_RESP_H

#include "cJSON.h"

/* JSON responses. All user-influenced text is escaped by cJSON; error
 * messages are compile-time literals only. */

/* Serialises and sends `obj`, then frees it. Returns `status`. */
int resp_json(int fd, int status, const char *rid, cJSON *obj, const char *extra_headers);

/* {"error":{"code","message","field"?,"request_id"}} */
int resp_error(int fd, int status, const char *rid, const char *code, const char *message,
               const char *field);

int resp_error_ex(int fd, int status, const char *rid, const char *code, const char *message,
                  const char *field, const char *extra_headers);

#endif
