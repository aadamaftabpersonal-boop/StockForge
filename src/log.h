#ifndef SF_LOG_H
#define SF_LOG_H

#include <stddef.h>
#include <stdint.h>

/* One JSON object per line on stderr, emitted with a single write(2) so lines
 * from different threads never interleave. User-controlled strings are
 * JSON-escaped and truncated (log-injection defence). Query strings, cookies,
 * headers and bodies are never logged. */
void log_msg(const char *level, const char *event, const char *msg);

void log_access(const char *request_id, const char *method, size_t method_len,
                const char *path, size_t path_len, int status,
                uint64_t latency_us, uint64_t queue_wait_us);

#endif
