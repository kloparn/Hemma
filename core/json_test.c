#include "../tests/test.h"
#include "json.h"

static bool parse(Json *j, const char *s, JsonTok *t, int max) { return json_parse(j, s, strlen(s), t, max); }

TEST(json_parses_nested_documents) {
    JsonTok t[64];
    Json j;
    const char *s = " {\"t\":\"act\",\"id\":17,\"f\":[[\"res\",\"2\"],[\"note\",\"ikea \\\"trip\\\"\"]],\"ok\":true,"
                    "\"n\":null,\"x\":-1.5e3} ";
    CHECK(parse(&j, s, t, 64));
    CHECK(json_type(&j, 0) == JSON_OBJECT);
    CHECK_INT(j.tok[0].count, 6);
    char buf[64];
    CHECK(json_string(&j, json_get(&j, 0, "t"), buf, sizeof buf));
    CHECK_STR(buf, "act");
    long long id = 0;
    CHECK(json_integer(&j, json_get(&j, 0, "id"), &id) && id == 17);
    int f = json_get(&j, 0, "f");
    CHECK(json_type(&j, f) == JSON_ARRAY);
    CHECK_INT(j.tok[f].count, 2);
    int pair = json_index(&j, f, 1);
    CHECK(json_string(&j, json_index(&j, pair, 1), buf, sizeof buf));
    CHECK_STR(buf, "ikea \"trip\"");
    bool ok = false;
    CHECK(json_bool(&j, json_get(&j, 0, "ok"), &ok) && ok);
    CHECK(json_type(&j, json_get(&j, 0, "n")) == JSON_NULL);
    CHECK(!json_integer(&j, json_get(&j, 0, "x"), &id)); /* not an integer */
    CHECK(json_get(&j, 0, "missing") == -1);
    CHECK(json_index(&j, f, 2) == -1);
}

TEST(json_iterates_objects_and_arrays) {
    JsonTok t[32];
    Json j;
    CHECK(parse(&j, "{\"a\":[1,[2,3],{\"b\":4}],\"c\":5}", t, 32));
    int keys = 0;
    for (int k = json_child(&j, 0); k >= 0; k = json_next(&j, 0, k)) keys++;
    CHECK_INT(keys, 2);
    int a = json_get(&j, 0, "a"), items = 0;
    for (int k = json_child(&j, a); k >= 0; k = json_next(&j, a, k)) items++;
    CHECK_INT(items, 3);
    long long c = 0;
    CHECK(json_integer(&j, json_get(&j, 0, "c"), &c) && c == 5);
}

TEST(json_unescapes_unicode) {
    JsonTok t[4];
    Json j;
    char buf[32];
    CHECK(parse(&j, "\"\\u00e5\\u00E4\\ud83c\\udfe1 \\n\\t\\/\"", t, 4));
    CHECK(json_string(&j, 0, buf, sizeof buf));
    CHECK_STR(buf, "åä🏡 \n\t/");
    CHECK(parse(&j, "\"\\udc00x\"", t, 4)); /* lone low surrogate -> U+FFFD */
    CHECK(json_string(&j, 0, buf, sizeof buf));
    CHECK_STR(buf, "\xEF\xBF\xBDx");
    char small[4];
    CHECK(parse(&j, "\"åäö\"", t, 4)); /* truncation keeps whole characters */
    CHECK(json_string(&j, 0, small, sizeof small));
    CHECK_STR(small, "å");
    CHECK(parse(&j, "{\"k\\u0065y\":1}", t, 4));
    CHECK(json_get(&j, 0, "key") == 2);
}

TEST(json_rejects_invalid_input) {
    JsonTok t[64];
    Json j;
    const char *bad[] = {"",        "{",          "{\"a\"}",   "{\"a\":}",     "[1,]",    "[1 2]",   "01",
                         "1.",      "-",          "\"\\x\"",   "\"\\u12\"",    "tru",     "nul",     "{} x",
                         "\"a\nb\"", "{\"a\":1,}", "[\"\\uZZZZ\"]", "{1:2}", "1e", "--1"};
    for (int i = 0; i < countof(bad); i++) {
        if (parse(&j, bad[i], t, 64)) {
            test_fail(__FILE__, __LINE__, "accepted invalid JSON: %s", bad[i]);
            return;
        }
    }
    /* depth limit */
    char deep[200];
    memset(deep, '[', 100);
    memset(deep + 100, ']', 100);
    CHECK(!json_parse(&j, deep, 200, t, 64));
    /* token limit */
    CHECK(!parse(&j, "[1,2,3,4,5]", t, 3));
}

TEST(json_writer_escapes_and_scrubs) {
    Buf b = {0};
    json_str(&b, "say \"hej\"\n\\ \x01 åäö 🏡");
    CHECK_STR(buf_cstr(&b), "\"say \\\"hej\\\"\\n\\\\ \\u0001 åäö 🏡\"");
    buf_reset(&b);
    json_strn(&b, "bad\xff\xc3", 5); /* invalid bytes become U+FFFD */
    CHECK_STR(buf_cstr(&b), "\"bad\xEF\xBF\xBD\xEF\xBF\xBD\"");
    buf_reset(&b);
    json_str(&b, NULL);
    CHECK_STR(buf_cstr(&b), "\"\"");
    /* round trip */
    buf_reset(&b);
    json_str(&b, "tab\there \"q\" \xE2\x80\xA8");
    JsonTok t[2];
    Json j;
    char back[64];
    CHECK(json_parse(&j, b.p, b.len, t, 2));
    CHECK(json_string(&j, 0, back, sizeof back));
    CHECK_STR(back, "tab\there \"q\" \xE2\x80\xA8");
    buf_free(&b);
}

TEST(json_survives_random_garbage) {
    JsonTok t[64];
    Json j;
    uint32_t x = 2463534242u;
    char s[64];
    const char alphabet[] = "{}[]\",:0123456789-+.eEtrufalsn \\u\n";
    for (int round = 0; round < 20000; round++) {
        int n = (int)(x % 40);
        for (int i = 0; i < n; i++) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            s[i] = alphabet[x % (sizeof alphabet - 1)];
        }
        json_parse(&j, s, (size_t)n, t, 64); /* must not crash or hang */
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    }
    CHECK(true);
}
