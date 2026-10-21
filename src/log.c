#include "log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Escapes into dst (always NUL-terminated); truncates at most_src bytes of input. */
static void jesc(char *dst, size_t dn, const char *s, size_t sn) {
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    if (sn > 200) sn = 200;
    for (size_t i = 0; i < sn && o + 7 < dn; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
        else if (c < 0x20 || c == 0x7f || c >= 0x80) {
            dst[o++] = '\\'; dst[o++] = 'u'; dst[o++] = '0'; dst[o++] = '0';
            dst[o++] = hex[c >> 4]; dst[o++] = hex[c & 15];
        } else dst[o++] = (char)c;
    }
    dst[o] = 0;
}

static void ts(char *out, size_t n) {
    struct timespec t;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &t);
    gmtime_r(&t.tv_sec, &tm);
    size_t k = strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + k, n - k, ".%03ldZ", t.tv_nsec / 1000000);
}

void log_msg(const char *level, const char *event, const char *msg) {
    char t[40], m[1100], line[1400];
    ts(t, sizeof t);
    jesc(m, sizeof m, msg, strlen(msg));
    int n = snprintf(line, sizeof line,
                     "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\"}\n",
                     t, level, event, m);
    if (n > 0) { ssize_t r = write(STDERR_FILENO, line, (size_t)n); (void)r; }
}

void log_access(const char *rid, const char *method, size_t ml, const char *path,
                size_t pl, int status, uint64_t lat_us, uint64_t qw_us) {
    char t[40], m[64], p[1100], line[1500];
    ts(t, sizeof t);
    jesc(m, sizeof m, method, ml > 16 ? 16 : ml);
    jesc(p, sizeof p, path, pl);
    int n = snprintf(line, sizeof line,
        "{\"ts\":\"%s\",\"level\":\"info\",\"event\":\"access\",\"request_id\":\"%s\","
        "\"method\":\"%s\",\"path\":\"%s\",\"status\":%d,\"latency_us\":%llu,"
        "\"queue_wait_us\":%llu}\n",
        t, rid, m, p, status, (unsigned long long)lat_us, (unsigned long long)qw_us);
    if (n > 0) { ssize_t r = write(STDERR_FILENO, line, (size_t)n); (void)r; }
}
