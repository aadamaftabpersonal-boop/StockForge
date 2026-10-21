#ifndef SF_VALIDATE_H
#define SF_VALIDATE_H

#include <stddef.h>
#include <stdint.h>

/* Pure functions (no I/O, no globals) so they can be unit-tested exhaustively.
 * Everything here treats input as hostile. */

int  utf8_valid(const char *s, size_t n);               /* strict: no overlongs/surrogates */
int  valid_email(const char *s);                        /* after email_normalize() */
void email_normalize(char *s);                          /* ASCII lowercase in place */
int  valid_password(const char *s);                     /* 10..128 bytes, valid UTF-8, no NUL */
int  valid_display_name(const char *s);                 /* 1..100 bytes, valid UTF-8, no control chars */
int  valid_sku(const char *s);                          /* [A-Za-z0-9_-]{1,40} */
int  valid_public_id(const char *s);                    /* 26 chars of [0-9A-Z] */

/* Decimal digits only, 1..18 digits (always fits in int64). 0 on success. */
int  parse_u64(const char *s, size_t n, uint64_t *out);

/* Query-string lookup with percent-decoding ('+' => space). Returns 1 found,
 * 0 absent, -1 malformed escape / embedded NUL / does not fit outn. */
int  qs_get(const char *qs, size_t qlen, const char *key, char *out, size_t outn);

/* Turns free text into a MySQL BOOLEAN MODE expression of `+token*` terms.
 * Operator characters are stripped, tokens < 3 bytes dropped (InnoDB FULLTEXT
 * minimum), at most 6 tokens of at most 32 bytes. Returns token count. */
int  search_to_boolean(const char *q, char *out, size_t outn);

/* Keyset cursors: "<a>" or "<a>.<b>". Returns number of fields parsed (1 or 2), 0 if invalid. */
int  cursor_parse(const char *s, uint64_t *a, uint64_t *b);

/* Extracts cookie `name` from a Cookie header value. 1 found, 0 absent. */
int  cookie_get(const char *hdr, size_t hlen, const char *name, char *out, size_t outn);

/* Origin ("https://shop.example:8443") vs Host header ("shop.example:8443"). */
int  origin_matches_host(const char *origin, size_t olen, const char *host, size_t hlen);

#endif
