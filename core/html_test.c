#include "../tests/test.h"
#include "html.h"

TEST(html_escapes_by_default) {
    Buf b = {0};
    H(&b, "<a href=\"/x?q=%u\" title=\"%s\">%s %r</a>", "a&b c", "\"quoted\" 'single'", "<script>", "<b>raw</b>");
    CHECK_STR(buf_cstr(&b),
              "<a href=\"/x?q=a%26b%20c\" title=\"&quot;quoted&quot; &#39;single&#39;\">&lt;script&gt; <b>raw</b></a>");
    buf_free(&b);
}

TEST(html_numbers_and_literals) {
    Buf b = {0};
    H(&b, "%d|%L|%X|%c|100%%|%s|%r", -42, (long long)9007199254740993LL, (uint64_t)0xabcdef0123456789ULL, 'z',
      (const char *)NULL, (const char *)NULL);
    CHECK_STR(buf_cstr(&b), "-42|9007199254740993|abcdef0123456789|z|100%||");
    buf_reset(&b);
    H(&b, "%02d:%02d|%3d|%05L|%d.%02d", 7, 5, 42, (long long)-12, 3, 50);
    CHECK_STR(buf_cstr(&b), "07:05| 42|-0012|3.50");
    buf_reset(&b);
    H(&b, "trailing %");
    CHECK_STR(buf_cstr(&b), "trailing %");
    buf_reset(&b);
    H(&b, "%q");
    CHECK_STR(buf_cstr(&b), "%q");
    buf_free(&b);
}

TEST(html_scrubs_invalid_utf8) {
    Buf b = {0};
    html_escn(&b, "ok\xff\xc3 åäö 🏡", 16);
    CHECK_STR(buf_cstr(&b), "ok\xEF\xBF\xBD\xEF\xBF\xBD åäö 🏡");
    CHECK(utf8_valid(b.p, b.len));
    buf_free(&b);
}
