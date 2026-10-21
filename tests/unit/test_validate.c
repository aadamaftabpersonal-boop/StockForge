#include <string.h>

#include "testutil.h"
#include "validate.h"

static void test_email(void) {
    const char *good[] = {"a@b.co", "john.doe+tag@example.com", "x_y%z@sub.domain.org", "ava@stockforge.test"};
    const char *bad[] = {"a@b", "@b.com", "a@@b.com", "a b@c.com", ".a@b.com", "a.@b.com", "a@b..com",
                         "a@-b.com", "a@b.com-", "a@b.com\n", "\xc3\xbc@b.com", "plain", "a@.com", ""};
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) CHECK(valid_email(good[i]));
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!valid_email(bad[i]));
    char local65[100];
    memset(local65, 'a', 65);
    strcpy(local65 + 65, "@b.com");
    CHECK(!valid_email(local65));
    char e[] = "Ava.Martin@StockForge.TEST";
    email_normalize(e);
    CHECK(strcmp(e, "ava.martin@stockforge.test") == 0);
}

static void test_utf8_and_strings(void) {
    CHECK(utf8_valid("caf\xc3\xa9", 5));
    CHECK(utf8_valid("\xf0\x9f\x98\x80", 4));
    CHECK(!utf8_valid("\xc0\x80", 2));              /* overlong NUL */
    CHECK(!utf8_valid("\xed\xa0\x80", 3));          /* surrogate */
    CHECK(!utf8_valid("\xf4\x90\x80\x80", 4));      /* > U+10FFFF */
    CHECK(!utf8_valid("\xe2\x82", 2));              /* truncated */
    CHECK(!utf8_valid("\xff", 1));

    CHECK(!valid_password("123456789"));
    CHECK(valid_password("1234567890"));
    char pw[200];
    memset(pw, 'x', 128); pw[128] = 0;
    CHECK(valid_password(pw));
    memset(pw, 'x', 129); pw[129] = 0;
    CHECK(!valid_password(pw));
    CHECK(!valid_password("\xff\xfe\xfd\xfc\xfb\xfa\xf9\xf8\xf7\xf6"));

    CHECK(valid_display_name("Ava"));
    CHECK(valid_display_name("Jos\xc3\xa9 Garc\xc3\xad" "a"));
    CHECK(!valid_display_name(""));
    CHECK(!valid_display_name("    "));
    CHECK(!valid_display_name("A\tB"));
    memset(pw, 'n', 101); pw[101] = 0;
    CHECK(!valid_display_name(pw));

    CHECK(valid_sku("SF-KB-001") && valid_sku("a_b-9"));
    CHECK(!valid_sku("") && !valid_sku("has space") && !valid_sku("semi;colon") && !valid_sku("q'uote"));
    CHECK(valid_public_id("01ARZ3NDEKTSV4RRFFQ69G5FAV"));
    CHECK(!valid_public_id("01arz3ndektsv4rrffq69g5fav") && !valid_public_id("SHORT"));
}

static void test_parse_u64(void) {
    uint64_t v = 99;
    CHECK(parse_u64("0", 1, &v) == 0 && v == 0);
    CHECK(parse_u64("123", 3, &v) == 0 && v == 123);
    CHECK(parse_u64("999999999999999999", 18, &v) == 0 && v == 999999999999999999ull);
    CHECK(parse_u64("", 0, &v) != 0);
    CHECK(parse_u64("12a", 3, &v) != 0);
    CHECK(parse_u64("-1", 2, &v) != 0);
    CHECK(parse_u64(" 1", 2, &v) != 0);
    CHECK(parse_u64("1000000000000000000", 19, &v) != 0);   /* 19 digits: could overflow int64 */
}

static void test_qs(void) {
    const char *qs = "a=1&b=two%20words&c=x+y&flag&e=%E2%82%AC";
    size_t n = strlen(qs);
    char o[64];
    CHECK(qs_get(qs, n, "a", o, sizeof o) == 1 && strcmp(o, "1") == 0);
    CHECK(qs_get(qs, n, "b", o, sizeof o) == 1 && strcmp(o, "two words") == 0);
    CHECK(qs_get(qs, n, "c", o, sizeof o) == 1 && strcmp(o, "x y") == 0);
    CHECK(qs_get(qs, n, "flag", o, sizeof o) == 1 && o[0] == 0);
    CHECK(qs_get(qs, n, "e", o, sizeof o) == 1 && strcmp(o, "\xe2\x82\xac") == 0);
    CHECK(qs_get(qs, n, "missing", o, sizeof o) == 0);
    CHECK(qs_get("ab=1", 4, "a", o, sizeof o) == 0);               /* prefix must not match */
    CHECK(qs_get("", 0, "a", o, sizeof o) == 0);
    CHECK(qs_get("a=%zz", 5, "a", o, sizeof o) == -1);             /* bad escape */
    CHECK(qs_get("a=%2", 4, "a", o, sizeof o) == -1);              /* truncated escape */
    CHECK(qs_get("a=%00", 5, "a", o, sizeof o) == -1);             /* NUL injection */
    char small[4];
    CHECK(qs_get("a=abcdef", 8, "a", small, sizeof small) == -1);  /* no silent truncation */
}

static void test_search(void) {
    char o[128];
    CHECK(search_to_boolean("red leather bag", o, sizeof o) == 3 && strcmp(o, "+red* +leather* +bag*") == 0);
    CHECK(search_to_boolean("a b tv", o, sizeof o) == 0 && o[0] == 0);          /* all < 3 bytes */
    CHECK(search_to_boolean("-foo +bar \"baz\" ~qux* (zip)", o, sizeof o) == 5);
    CHECK(strcmp(o, "+foo* +bar* +baz* +qux* +zip*") == 0);                    /* operators stripped */
    CHECK(search_to_boolean("foo*bar", o, sizeof o) == 2 && strcmp(o, "+foo* +bar*") == 0);
    CHECK(search_to_boolean("aaa bbb ccc ddd eee fff ggg", o, sizeof o) == 6);  /* capped */
    CHECK(search_to_boolean("caf\xc3\xa9 latte", o, sizeof o) == 2);
    CHECK(search_to_boolean("' OR 1=1 --", o, sizeof o) == 0);                 /* nothing survives */
    CHECK(search_to_boolean("", o, sizeof o) == 0);

    /* Strict UTF-8 (regression: invalid bytes used to become `+<garbage>*`, which MySQL rejects with errno 1064) */
    CHECK(search_to_boolean("\xed\xa0\x80", o, sizeof o) == 0 && o[0] == 0);                 /* UTF-16 surrogate */
    CHECK(search_to_boolean("\xf8\x88\x80\x80\x80", o, sizeof o) == 0);                       /* impossible 5-byte form */
    CHECK(search_to_boolean("\xc0\xaf\xc0\xaf\xc0\xaf", o, sizeof o) == 0);                   /* overlong '/' */
    CHECK(search_to_boolean("abc\xed\xa0\x80" "def", o, sizeof o) == 2 && strcmp(o, "+abc* +def*") == 0);
    CHECK(search_to_boolean("abc\xff" "def\x80" "ghi", o, sizeof o) == 3);                    /* stray bytes split tokens */
    CHECK(search_to_boolean("lamp\xe2\x82", o, sizeof o) == 1 && strcmp(o, "+lamp*") == 0);   /* truncated sequence dropped */
    /* Tokens are counted in CHARACTERS, as InnoDB does (3 minimum), not bytes */
    CHECK(search_to_boolean("\xe6\x97\xa5\xe6\x9c\xac", o, sizeof o) == 0);                   /* 2 characters (6 bytes) */
    CHECK(search_to_boolean("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", o, sizeof o) == 1);       /* 3 characters */
    CHECK(search_to_boolean("\xe2\x98\x85\xe2\x98\x85\xe2\x98\x85 lamp", o, sizeof o) == 2);  /* valid symbols are kept */
    CHECK(search_to_boolean("\xd0\xba\xd0\xbb\xd0\xb0\xd0\xb2", o, sizeof o) == 1);           /* Cyrillic */
}

static void test_cursor_cookie_origin(void) {
    uint64_t a = 0, b = 0;
    CHECK(cursor_parse("123", &a, &b) == 1 && a == 123);
    CHECK(cursor_parse("10.20", &a, &b) == 2 && a == 10 && b == 20);
    CHECK(cursor_parse("abc", &a, &b) == 0);
    CHECK(cursor_parse("1.2.3", &a, &b) == 0);
    CHECK(cursor_parse("", &a, &b) == 0 && cursor_parse("-5", &a, &b) == 0 && cursor_parse(".5", &a, &b) == 0);

    char o[64];
    const char *ck = "a=1; sf_sid=TOKEN123; b=2";
    CHECK(cookie_get(ck, strlen(ck), "sf_sid", o, sizeof o) == 1 && strcmp(o, "TOKEN123") == 0);
    CHECK(cookie_get("sf_sid=X", 8, "sf_sid", o, sizeof o) == 1 && strcmp(o, "X") == 0);
    CHECK(cookie_get("xsf_sid=Y", 9, "sf_sid", o, sizeof o) == 0);              /* name must match fully */
    CHECK(cookie_get("a=1", 3, "sf_sid", o, sizeof o) == 0);
    char small[3];
    CHECK(cookie_get("sf_sid=LONGVALUE", 16, "sf_sid", small, sizeof small) == 0);

    CHECK(origin_matches_host("https://shop.example", 20, "shop.example", 12));
    CHECK(origin_matches_host("http://localhost:8080", 21, "localhost:8080", 14));
    CHECK(origin_matches_host("HTTPS://Shop.Example", 20, "shop.example", 12));
    CHECK(!origin_matches_host("https://evil.com", 16, "shop.example", 12));
    CHECK(!origin_matches_host("https://shop.example.evil.com", 29, "shop.example", 12));
    CHECK(!origin_matches_host("null", 4, "shop.example", 12));
    CHECK(!origin_matches_host("https://shop.example", 20, "", 0));
}

int main(void) {
    test_email();
    test_utf8_and_strings();
    test_parse_u64();
    test_qs();
    test_search();
    test_cursor_cookie_origin();
    TEST_MAIN_END();
}
