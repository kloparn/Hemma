#include "../../tests/test.h"
#include "mediainfo.h"

static const char *vtt(const char *srt, size_t n, Buf *out, bool *ok) {
    buf_reset(out);
    *ok = srt_to_vtt(srt, n, out);
    return buf_cstr(out);
}

TEST(srt_basic_crlf_bom_and_short_millis) {
    const char *srt = "\xEF\xBB\xBF"
                      "1\r\n00:00:01,000 --> 00:00:04,250\r\nHej!\r\n<i>Hur mår du?</i>\r\n\r\n"
                      "2\r\n00:01:02,5 --> 00:01:03,75\r\nBra.\r\n";
    Buf out = {0};
    bool ok;
    CHECK_STR(vtt(srt, strlen(srt), &out, &ok), "WEBVTT\n\n"
                                               "00:00:01.000 --> 00:00:04.250\nHej!\n<i>Hur mår du?</i>\n\n"
                                               "00:01:02.500 --> 00:01:03.750\nBra.\n\n");
    CHECK(ok);
    buf_free(&out);
}

TEST(srt_windows_1252_is_converted) {
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nR\xE4ksm\xF6rg\xE5s \x96 \x93" "citat\x94 \x80" "5\n";
    Buf out = {0};
    bool ok;
    CHECK_STR(vtt(srt, strlen(srt), &out, &ok),
              "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nRäksmörgås – \xE2\x80\x9C" "citat\xE2\x80\x9D €5\n\n");
    CHECK(ok);
    CHECK(utf8_valid(out.p, out.len));
    buf_free(&out);
}

TEST(srt_tags_entities_and_ass_overrides) {
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\n"
                      "{\\an8}<font color=\"#ffff00\">Gul</font> & <B>fet</B> 5 < 6 > 4 &amp; ok\n"
                      "<u>under</u> &#228; <br>slut\n"
                      "{\\i1}\n";
    Buf out = {0};
    bool ok;
    CHECK_STR(vtt(srt, strlen(srt), &out, &ok), "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\n"
                                               "Gul &amp; <b>fet</b> 5 &lt; 6 &gt; 4 &amp; ok\n"
                                               "<u>under</u> &#228; slut\n\n");
    CHECK(ok);
    buf_free(&out);
}

TEST(srt_malformed_blocks_are_tolerated) {
    const char *srt = "garbage line\nanother\n\n"
                      "1\n00:00:01,000 --> 00:00:02,000\nFirst\n"
                      "2\n00:00:03,000 --> 00:00:04,000\nSecond\n"
                      "00:00:05,000-->00:00:06,000\nThird, no blank line\n\n"
                      "bad --> timing\nskip me\n\n"
                      "   \n"
                      "00:07,000 --> 00:08,000 X1:100 X2:200\n   Short form  \n\n"
                      "9\n00:00:10,000 --> 00:00:11,000\n\n"   /* empty cue: dropped */
                      "10\n01:59:59,999 --> 02:00:00,000\nLast";
    Buf out = {0};
    bool ok;
    CHECK_STR(vtt(srt, strlen(srt), &out, &ok), "WEBVTT\n\n"
                                               "00:00:01.000 --> 00:00:02.000\nFirst\n\n"
                                               "00:00:03.000 --> 00:00:04.000\nSecond\n\n"
                                               "00:00:05.000 --> 00:00:06.000\nThird, no blank line\n\n"
                                               "00:00:07.000 --> 00:00:08.000\nShort form\n\n"
                                               "01:59:59.999 --> 02:00:00.000\nLast\n\n");
    CHECK(ok);
    buf_free(&out);
}

TEST(srt_utf16_with_bom) {
    const char *ascii = "1\r\n00:00:01,000 --> 00:00:02,000\r\nG? bra\r\n";
    Buf in = {0};
    buf_put(&in, "\xFF\xFE", 2);
    for (const char *p = ascii; *p; p++) {
        if (*p == '?') buf_put(&in, "\xF6\x00", 2); /* U+00F6 ö */
        else {
            buf_putc(&in, *p);
            buf_putc(&in, 0);
        }
    }
    buf_put(&in, "\x3C\xD8\xE1\xDF", 4); /* U+1F3E1 🏡 as a surrogate pair (D83C DFE1) */
    Buf out = {0};
    bool ok;
    CHECK_STR(vtt(in.p, in.len, &out, &ok), "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nGö bra\n\xF0\x9F\x8F\xA1\n\n");
    CHECK(ok);
    buf_free(&in);
    buf_free(&out);
}

TEST(srt_empty_or_garbage_returns_false) {
    Buf out = {0};
    bool ok = true;
    CHECK_STR(vtt("", 0, &out, &ok), "WEBVTT\n\n");
    CHECK(!ok);
    CHECK_STR(vtt(NULL, 5, &out, &ok), "WEBVTT\n\n");
    CHECK(!ok);
    vtt("hello\nworld\n", 12, &out, &ok);
    CHECK(!ok);
    vtt("\xEF\xBB\xBF", 3, &out, &ok);
    CHECK(!ok);
    buf_free(&out);
}

TEST(srt_random_bytes_never_crash) {
    uint64_t s = 12345;
    char buf[1024];
    Buf out = {0};
    for (int iter = 0; iter < 2000; iter++) {
        size_t n = 0;
        for (; n < (size_t)(iter % 1000); n++) {
            s ^= s << 13, s ^= s >> 7, s ^= s << 17;
            /* bias towards SRT-ish bytes so the parser gets exercised */
            static const char alphabet[] = "0123456789:,.-> \n\r<>/{}\\ib&;#xaZ\xC3\xA5\x96\xFF\xFE";
            buf[n] = alphabet[s % (sizeof alphabet - 1)];
        }
        buf_reset(&out);
        srt_to_vtt(buf, n, &out);
        CHECK(str_starts(buf_cstr(&out), "WEBVTT\n\n"));
        CHECK(utf8_valid(out.p, out.len));
    }
    buf_free(&out);
}

TEST(subtitle_matching_by_file_name) {
    char lang[16];
    CHECK(media_sub_for("Movie.mkv", "Movie.srt", lang, sizeof lang));
    CHECK_STR(lang, "");
    CHECK(media_sub_for("Movie.mkv", "Movie.sv.srt", lang, sizeof lang));
    CHECK_STR(lang, "sv");
    CHECK(media_sub_for("Movie.mkv", "movie.SWE.forced.srt", lang, sizeof lang));
    CHECK_STR(lang, "sv");
    CHECK(media_sub_for("Movie.mkv", "Movie.English.SDH.vtt", lang, sizeof lang));
    CHECK_STR(lang, "en");
    CHECK(media_sub_for("Movie.2010.1080p.mkv", "Movie.2010.1080p.en.srt", lang, sizeof lang));
    CHECK_STR(lang, "en");
    CHECK(media_sub_for("Räksmörgås.mkv", "RÄKSMÖRGÅS.da.srt", lang, sizeof lang));
    CHECK_STR(lang, "da");
    CHECK(media_sub_for("Show S01E01.mp4", "Show S01E01_forced.srt", lang, sizeof lang));
    CHECK_STR(lang, "");
    CHECK(!media_sub_for("Movie.mkv", "Movie 2.srt", lang, sizeof lang));
    CHECK(!media_sub_for("Movie.mkv", "Movie Extended.srt", lang, sizeof lang));
    CHECK(!media_sub_for("Movie.mkv", "Movie.sv.txt", lang, sizeof lang));
    CHECK(!media_sub_for("Movie.mkv", "Other.srt", lang, sizeof lang));
    CHECK(!media_sub_for("Movie.mkv", "Mov.srt", lang, sizeof lang));
    CHECK(media_sub_for("Movie.mkv", "Movie.sv.srt", NULL, 0));
}
