#include "validate.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

int utf8_valid(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s, *e = p + n;
    while (p < e) {
        if (*p < 0x80) { p++; continue; }
        size_t need;
        uint32_t cp;
        if ((*p & 0xE0) == 0xC0) { need = 1; cp = *p & 0x1F; if (*p < 0xC2) return 0; }
        else if ((*p & 0xF0) == 0xE0) { need = 2; cp = *p & 0x0F; }
        else if ((*p & 0xF8) == 0xF0) { need = 3; cp = *p & 0x07; if (*p > 0xF4) return 0; }
        else return 0;
        if ((size_t)(e - p) <= need) return 0;
        for (size_t i = 1; i <= need; i++) {
            if ((p[i] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
        if (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
        p += need + 1;
    }
    return 1;
}

void email_normalize(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

int valid_email(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > 254) return 0;
    const char *at = strchr(s, '@');
    if (!at || strchr(at + 1, '@') || at == s || at - s > 64) return 0;
    for (const char *c = s; c < at; c++)
        if (!(isalnum((unsigned char)*c) || strchr("._%+-", *c))) return 0;
    if (s[0] == '.' || at[-1] == '.') return 0;
    const char *d = at + 1;
    if (!*d || !strchr(d, '.') || *d == '.' || *d == '-' || d[strlen(d) - 1] == '.' || d[strlen(d) - 1] == '-')
        return 0;
    char prev = 0;
    for (const char *c = d; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '.')) return 0;
        if (*c == '.' && prev == '.') return 0;
        prev = *c;
    }
    return 1;
}

int valid_password(const char *s) {
    size_t n = strlen(s);
    return n >= 10 && n <= 128 && utf8_valid(s, n);
}

int valid_display_name(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 100 || !utf8_valid(s, n)) return 0;
    int visible = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
        if (c != ' ') visible = 1;
    }
    return visible;
}

int valid_sku(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 40) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '-' || *s == '_')) return 0;
    return 1;
}

int valid_public_id(const char *s) {
    if (strlen(s) != 26) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z'))) return 0;
    return 1;
}

int parse_u64(const char *s, size_t n, uint64_t *out) {
    if (n < 1 || n > 18) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int qs_get(const char *qs, size_t qlen, const char *key, char *out, size_t outn) {
    size_t kl = strlen(key), i = 0;
    while (i <= qlen) {
        size_t j = i;
        while (j < qlen && qs[j] != '&') j++;
        const char *eq = memchr(qs + i, '=', j - i);
        size_t nl = eq ? (size_t)(eq - (qs + i)) : j - i;
        if (nl == kl && memcmp(qs + i, key, kl) == 0) {
            const char *v = eq ? eq + 1 : qs + j;
            size_t vl = eq ? j - (size_t)(eq + 1 - qs) : 0, o = 0;
            for (size_t k = 0; k < vl; k++) {
                char c = v[k];
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (k + 2 >= vl) return -1;          /* truncated %XX */
                    int h = hexv(v[k + 1]), l = hexv(v[k + 2]);
                    if (h < 0 || l < 0) return -1;
                    c = (char)(h * 16 + l);
                    k += 2;
                    if (c == 0) return -1;
                }
                if (o + 1 >= outn) return -1;
                out[o++] = c;
            }
            out[o] = 0;
            return 1;
        }
        i = j + 1;
    }
    return 0;
}

/* Length of the VALID UTF-8 sequence starting at p (1-4), or 0 if p does not start one.
 * Strict: rejects overlong forms, UTF-16 surrogates and code points above U+10FFFF. */
static size_t utf8_len_at(const unsigned char *p, size_t n) {
    if (p[0] < 0x80) return 1;
    size_t need;
    uint32_t cp;
    if ((p[0] & 0xE0) == 0xC0) { need = 1; cp = p[0] & 0x1F; if (p[0] < 0xC2) return 0; }
    else if ((p[0] & 0xF0) == 0xE0) { need = 2; cp = p[0] & 0x0F; }
    else if ((p[0] & 0xF8) == 0xF0) { need = 3; cp = p[0] & 0x07; if (p[0] > 0xF4) return 0; }
    else return 0;
    if (n <= need) return 0;
    for (size_t i = 1; i <= need; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (need == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return need + 1;
}

/* A word character is an ASCII letter/digit or any character of a VALID multi-byte sequence.
 * Invalid bytes are separators: MySQL's FULLTEXT parser rejects `+<invalid bytes>*` with a
 * syntax error (found by the HTTP fuzzer: it surfaced as a 503). Token length is counted in
 * characters, matching InnoDB's minimum token size of 3 characters. */
static size_t word_char_len(const unsigned char *p, size_t n) {
    size_t l = utf8_len_at(p, n);
    if (l == 0) return 0;
    if (l == 1 && !isalnum(p[0])) return 0;
    return l;
}

int search_to_boolean(const char *q, char *out, size_t outn) {
    int tokens = 0;
    size_t o = 0, n = strlen(q), i = 0;
    const unsigned char *p = (const unsigned char *)q;
    out[0] = 0;
    while (i < n && tokens < 6) {
        size_t l;
        while (i < n && !(l = word_char_len(p + i, n - i))) i++;                 /* skip separators */
        size_t start = i, cap_end = i;
        int chars = 0;
        while (i < n && (l = word_char_len(p + i, n - i))) {
            i += l;
            if (++chars <= 32) cap_end = i;                                      /* at most 32 characters per token */
        }
        if (chars < 3) continue;
        size_t tl = cap_end - start;
        if (o + tl + 5 >= outn) break;
        if (o) out[o++] = ' ';
        out[o++] = '+';
        memcpy(out + o, p + start, tl);
        o += tl;
        out[o++] = '*';
        out[o] = 0;
        tokens++;
    }
    return tokens;
}

int cursor_parse(const char *s, uint64_t *a, uint64_t *b) {
    const char *dot = strchr(s, '.');
    if (!dot) return parse_u64(s, strlen(s), a) == 0 ? 1 : 0;
    if (parse_u64(s, (size_t)(dot - s), a) || parse_u64(dot + 1, strlen(dot + 1), b)) return 0;
    return 2;
}

int cookie_get(const char *hdr, size_t hlen, const char *name, char *out, size_t outn) {
    size_t nl = strlen(name), i = 0;
    while (i < hlen) {
        while (i < hlen && (hdr[i] == ' ' || hdr[i] == ';')) i++;
        size_t j = i;
        while (j < hlen && hdr[j] != ';') j++;
        if (j - i > nl && memcmp(hdr + i, name, nl) == 0 && hdr[i + nl] == '=') {
            size_t vl = j - (i + nl + 1);
            if (vl + 1 > outn) return 0;
            memcpy(out, hdr + i + nl + 1, vl);
            out[vl] = 0;
            return 1;
        }
        i = j + 1;
    }
    return 0;
}

int origin_matches_host(const char *o, size_t ol, const char *h, size_t hl) {
    size_t skip = 0;
    if (ol > 8 && strncasecmp(o, "https://", 8) == 0) skip = 8;
    else if (ol > 7 && strncasecmp(o, "http://", 7) == 0) skip = 7;
    else return 0;
    return ol - skip == hl && hl > 0 && strncasecmp(o + skip, h, hl) == 0;
}
