#include "../tests/test.h"

TEST(buf_grows_and_formats) {
    Buf b = {0};
    for (int i = 0; i < 1000; i++) buf_printf(&b, "%d,", i);
    CHECK(b.len > 3000);
    CHECK(str_starts(buf_cstr(&b), "0,1,2,"));
    CHECK(str_ends(buf_cstr(&b), "998,999,"));
    buf_consume(&b, 2);
    CHECK(str_starts(buf_cstr(&b), "1,2,"));
    buf_reset(&b);
    buf_puts(&b, "x");
    buf_putc(&b, 'y');
    CHECK_STR(buf_cstr(&b), "xy");
    buf_free(&b);
    CHECK(b.p == NULL && b.len == 0);
}

/* Regression: an un-terminated Buf passed as a C string leaked heap bytes into
 * HTTP headers (seen as garbage between headers and body in the browser). */
TEST(buf_is_always_a_c_string) {
    Buf b = {0};
    char junk[512];
    memset(junk, 'X', sizeof junk); /* make sure reused heap memory isn't zeroed */
    buf_put(&b, junk, sizeof junk);
    buf_reset(&b);
    CHECK_INT(strlen(b.p), 0);
    buf_puts(&b, "Cache-Control: no-store\r\n");
    CHECK_INT(strlen(b.p), b.len);
    buf_putc(&b, '!');
    CHECK_INT(strlen(b.p), b.len);
    buf_consume(&b, 6);
    CHECK_INT(strlen(b.p), b.len);
    buf_put(&b, b.p, 3); /* appending from itself is fine */
    CHECK_INT(strlen(b.p), b.len);
    buf_consume(&b, 1000);
    CHECK_STR(b.p, "");
    buf_free(&b);
}

TEST(buf_printf_long_strings) {
    Buf b = {0};
    char big[5000];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = 0;
    buf_printf(&b, "<%s>", big);
    CHECK_INT(b.len, 5001);
    CHECK(b.p[0] == '<' && b.p[5000] == '>');
    buf_free(&b);
}

TEST(str_copy_never_splits_utf8) {
    char d[6];
    /* "åäö" is 6 bytes; only two characters (4 bytes) fit in 5 + NUL */
    CHECK_INT(str_copy(d, sizeof d, "åäö"), 4);
    CHECK_STR(d, "åä");
    CHECK_INT(str_copy(d, sizeof d, "hello world"), 5);
    CHECK_STR(d, "hello");
    CHECK_INT(str_copy(d, sizeof d, NULL), 0);
    CHECK_STR(d, "");
    char e[4];
    CHECK_INT(str_copy(e, sizeof e, "€"), 3); /* exactly fits */
    CHECK_STR(e, "€");
    CHECK_INT(str_ncopy(e, sizeof e, "abcdef", 2), 2);
    CHECK_STR(e, "ab");
}

TEST(str_compare_helpers) {
    CHECK(str_eq("a", "a") && !str_eq("a", "b") && str_eq(NULL, NULL) && !str_eq("a", NULL));
    CHECK(str_ieq("Hello", "hELLO") && !str_ieq("Hell", "Hello"));
    CHECK(str_starts("/book?x", "/book") && !str_starts("/bo", "/book"));
    CHECK(str_istarts("Upgrade: x", "upgrade:"));
    CHECK(str_ends("movie.MKV", ".MKV") && str_iends("movie.MKV", ".mkv") && !str_ends("a", "abc"));
    CHECK(str_blank(NULL) && str_blank("  \t\n") && !str_blank(" x "));
    char t[] = "  padded \t ";
    CHECK_STR(str_trim(t), "padded");
}

TEST(str_int_parsing) {
    long long v = 0;
    CHECK(str_int("42", &v) && v == 42);
    CHECK(str_int("-7", &v) && v == -7);
    CHECK(str_int("+3", &v) && v == 3);
    CHECK(!str_int("", &v) && !str_int("-", &v) && !str_int("12x", &v) && !str_int(" 1", &v));
    CHECK(str_int("9223372036854775807", &v) && v == 9223372036854775807LL);
    CHECK(!str_int("9223372036854775808", &v));
    CHECK_INT(str_int_or("nope", 5), 5);
    CHECK_INT(str_int_or(NULL, 9), 9);
}

TEST(utf8_roundtrip_and_validation) {
    const uint32_t cps[] = {0x41, 0xE5, 0x20AC, 0x1F3E1};
    for (int i = 0; i < 4; i++) {
        char enc[4];
        size_t n = utf8_encode(cps[i], enc);
        uint32_t back = 0;
        CHECK_INT(utf8_decode(enc, n, &back), (int)n);
        CHECK_INT(back, cps[i]);
    }
    CHECK(utf8_valid("hej på dig 🏡", strlen("hej på dig 🏡")));
    CHECK(!utf8_valid("\xC3", 1));             /* truncated */
    CHECK(!utf8_valid("\xC0\xAF", 2));         /* overlong '/' */
    CHECK(!utf8_valid("\xED\xA0\x80", 3));     /* surrogate */
    CHECK(!utf8_valid("\xF4\x90\x80\x80", 4)); /* > U+10FFFF */
    CHECK_INT(utf8_count("åäö!"), 4);
    CHECK_INT(utf8_clip("åäö", 3), 2);
    CHECK_INT(utf8_clip("abc", 10), 3);
}

TEST(fold_matches_swedish_letters) {
    char out[32];
    fold_copy(out, sizeof out, "ÅÄÖ Tvättstuga ×");
    CHECK_STR(out, "åäö tvättstuga ×");
    const char *hay = "Boka TVÄTTSTUGAN idag";
    CHECK(fold_find(hay, "tvättstugan") == hay + 5);
    CHECK(fold_find(hay, "bastu") == NULL);
    CHECK(fold_find(hay, "") == hay);
}

TEST(dates_normalise_and_roundtrip) {
    int64_t t = dt_make(2026, 10, 10, 14, 30);
    DateTime d = dt_local(t);
    CHECK_INT(d.year, 2026);
    CHECK_INT(d.mon, 10);
    CHECK_INT(d.day, 10);
    CHECK_INT(d.hour, 14);
    CHECK_INT(d.min, 30);
    CHECK_INT(d.wday, 5); /* Saturday (0 = Monday) */
    /* day 32 rolls into the next month */
    DateTime n = dt_local(dt_make(2026, 1, 32, 0, 0));
    CHECK_INT(n.mon, 2);
    CHECK_INT(n.day, 1);
}

TEST(week_start_is_monday_midnight) {
    int64_t sat = dt_make(2026, 10, 10, 18, 5);
    DateTime w = dt_local(week_start(sat));
    CHECK_INT(w.year, 2026);
    CHECK_INT(w.mon, 10);
    CHECK_INT(w.day, 5);
    CHECK_INT(w.hour, 0);
    CHECK_INT(w.wday, 0);
    DateTime m = dt_local(week_start(dt_make(2026, 10, 5, 0, 0)));
    CHECK_INT(m.day, 5);
    DateTime s = dt_local(day_start(sat));
    CHECK_INT(s.hour, 0);
    CHECK_INT(s.min, 0);
    CHECK_INT(s.day, 10);
}

TEST(add_days_keeps_wall_clock_across_dst) {
    /* Europe switches on the last Sunday of March/October; wall time must stay put
       whatever zone the test machine is in. */
    DateTime a = dt_local(add_days(dt_make(2026, 3, 28, 12, 0), 1));
    CHECK_INT(a.day, 29);
    CHECK_INT(a.hour, 12);
    DateTime b = dt_local(add_days(dt_make(2026, 10, 24, 9, 15), 2));
    CHECK_INT(b.day, 26);
    CHECK_INT(b.hour, 9);
    CHECK_INT(b.min, 15);
    DateTime c = dt_local(add_days(dt_make(2026, 3, 1, 8, 0), -1));
    CHECK_INT(c.mon, 2);
    CHECK_INT(c.day, 28);
}

TEST(iso_week_numbers) {
    int y = 0;
    CHECK_INT(iso_week(dt_make(2026, 10, 10, 12, 0), &y), 41);
    CHECK_INT(y, 2026);
    CHECK_INT(iso_week(dt_make(2026, 1, 1, 12, 0), &y), 1);
    CHECK_INT(iso_week(dt_make(2024, 12, 30, 12, 0), &y), 1);
    CHECK_INT(y, 2025);
    CHECK_INT(iso_week(dt_make(2021, 1, 3, 12, 0), &y), 53);
    CHECK_INT(y, 2020);
}

TEST(calendar_helpers) {
    CHECK_INT(days_in_month(2024, 2), 29);
    CHECK_INT(days_in_month(2026, 2), 28);
    CHECK_INT(days_in_month(2000, 2), 29);
    CHECK_INT(days_in_month(1900, 2), 28);
    CHECK_INT(days_in_month(2026, 13), 0);
    CHECK_STR(weekday_name(0, true), "Mon");
    CHECK_STR(weekday_name(6, false), "Sunday");
    CHECK_STR(month_name(10, true), "Oct");
    char ymd[11], hm[6];
    int64_t t = dt_make(2026, 1, 2, 3, 4);
    fmt_ymd(t, ymd);
    fmt_hm(t, hm);
    CHECK_STR(ymd, "2026-01-02");
    CHECK_STR(hm, "03:04");
}

TEST(parse_ymd_is_strict) {
    int y, m, d;
    CHECK(parse_ymd("2026-10-12", &y, &m, &d) && y == 2026 && m == 10 && d == 12);
    CHECK(!parse_ymd("2026-02-30", &y, &m, &d));
    CHECK(!parse_ymd("2026-1-12", &y, &m, &d));
    CHECK(!parse_ymd("2026-10-12x", &y, &m, &d));
    CHECK(!parse_ymd(NULL, &y, &m, &d));
}

TEST(parse_hm_accepts_common_forms) {
    int m = -1;
    CHECK(parse_hm("14:30", &m) && m == 870);
    CHECK(parse_hm("9.05", &m) && m == 545);
    CHECK(parse_hm("7", &m) && m == 420);
    CHECK(parse_hm("1430", &m) && m == 870);
    CHECK(parse_hm("24:00", &m) && m == 1440);
    CHECK(!parse_hm("24:30", &m));
    CHECK(!parse_hm("12:60", &m));
    CHECK(!parse_hm("12:5", &m));
    CHECK(!parse_hm("abc", &m));
    CHECK(!parse_hm("", &m));
}

TEST(hash_and_hex) {
    CHECK(hash64("", 0) == 0xcbf29ce484222325ULL);
    CHECK(hash64_str("a") == 0xaf63dc4c8601ec8cULL);
    char hex[9];
    hex_encode("\x01\xab\xff\x10", 4, hex);
    CHECK_STR(hex, "01abff10");
}

TEST(log_lines_cant_be_forged) {
    /* review: a username with %0A in it produced a second, fake log line */
    char out[256];
    log_clean("failed sign-in for \"x\nHH:MM:SS I adam signed in from 10.0.0.2\" from 10.0.0.9", out, sizeof out);
    CHECK(!strchr(out, '\n'));
    CHECK_HAS(out, "\"x\\nHH:MM:SS I adam");
    log_clean("a\rb\tc\x1b[31md\x7f e\x01", out, sizeof out);
    CHECK_STR(out, "a\\rb\\tc\\x1b[31md\\x7f e\\x01");
    log_clean("csi \xC2\x9B" "31m, but \xC3\xA5 stays", out, sizeof out); /* C1 controls too; real UTF-8 kept */
    CHECK_STR(out, "csi \\u009b31m, but \xC3\xA5 stays");
    log_clean("trailing newline\r\n", out, sizeof out);
    CHECK_STR(out, "trailing newline");
    char tiny[8];
    CHECK_INT(log_clean("abcdef\ng", tiny, sizeof tiny), 6); /* never cuts an escape in half */
    CHECK_STR(tiny, "abcdef");
    CHECK_INT(log_clean("\n\n\n\n", tiny, sizeof tiny), 0); /* only trailing newlines: nothing */
    CHECK_INT(log_clean("x\n\ny", tiny, sizeof tiny), 6);
    CHECK_STR(tiny, "x\\n\\ny");
}
