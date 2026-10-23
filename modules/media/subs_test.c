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

/* What a subtitle track taken out of a video is saved as - and that its video takes it back. */
TEST(subtitle_sidecar_names) {
    char out[300], lang[16];
    CHECK(media_sub_sidecar("/m/Show/x.mkv", "eng", "", false, "srt", 0, out, sizeof out));
    CHECK_STR(out, "/m/Show/x.en.srt");
    CHECK(media_sub_sidecar("Show/x.web.mp4", "swe", NULL, true, "srt", 0, out, sizeof out)); /* a copy: named for "x" */
    CHECK_STR(out, "Show/x.sv.forced.srt");
    CHECK(media_sub_sidecar("dir.v2/x", "en-US", "English (SDH)", false, "srt", 0, out, sizeof out));
    CHECK_STR(out, "dir.v2/x.en.sdh.srt");
    CHECK(media_sub_sidecar("x.mkv", "und", "", false, "srt", 0, out, sizeof out));
    CHECK_STR(out, "x.srt");
    CHECK(media_sub_sidecar("x.mkv", "hun", "Hungarian", false, "srt", 0, out, sizeof out));
    CHECK_STR(out, "x.hu.srt");
    CHECK(media_sub_sidecar("x.mkv", "qaa", "", false, "srt", 0, out, sizeof out)); /* a code we don't know: none */
    CHECK_STR(out, "x.srt");
    CHECK(media_sub_sidecar("x.mkv", "hi", "", false, "srt", 0, out, sizeof out)); /* reads as "hearing impaired" */
    CHECK_STR(out, "x.srt");
    CHECK(media_sub_sidecar("x.mkv", "ENG", "Forced", false, "srt", 0, out, sizeof out));
    CHECK_STR(out, "x.en.forced.srt");
    CHECK(media_sub_sidecar("x.mkv", "eng", "English, hearing impaired", false, "srt", 0, out, sizeof out));
    CHECK_STR(out, "x.en.sdh.srt");
    CHECK(media_sub_sidecar("x.mkv", "eng", "Accented English", false, "srt", 0, out, sizeof out)); /* "cc" inside a word: not CC */
    CHECK_STR(out, "x.en.srt");
    CHECK(!media_sub_sidecar("a-long-name.mkv", "eng", "", false, "srt", 0, out, 12));
    const char *names[] = {"x.en.srt", "x.sv.forced.srt", "x.en.sdh.srt", "x.hu.srt"};
    const char *langs[] = {"en", "sv", "en", "hu"};
    for (int i = 0; i < 4; i++) {
        CHECK(media_sub_for("x.mkv", names[i], lang, sizeof lang));
        CHECK_STR(lang, langs[i]);
        CHECK(media_sub_for("x.web.mp4", names[i], lang, sizeof lang));
    }
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

/* Contract C1: every kind of subtitle a file holds, and a second track in one language and flags. */
TEST(subtitle_sidecar_names_for_every_kind_and_the_second_of_one_variant) {
    char out[300];
    CHECK(media_sub_sidecar("/m/x.mkv", "eng", "", false, "sup", 0, out, sizeof out));
    CHECK_STR(out, "/m/x.en.sup");
    CHECK(media_sub_sidecar("/m/x.mkv", "eng", "", false, "idx", 0, out, sizeof out));
    CHECK_STR(out, "/m/x.en.idx");
    CHECK(media_sub_sidecar("/m/x.mkv", "eng", "", false, NULL, 0, out, sizeof out)); /* (none: .srt) */
    CHECK_STR(out, "/m/x.en.srt");
    CHECK(media_sub_sidecar("/m/x.mkv", "eng", "", false, "srt", 1, out, sizeof out)); /* (1: the first) */
    CHECK_STR(out, "/m/x.en.srt");
    CHECK(media_sub_sidecar("/m/x.mkv", "eng", "", false, "srt", 2, out, sizeof out));
    CHECK_STR(out, "/m/x.en.2.srt");
    CHECK(media_sub_sidecar("x.web.mp4", "eng", "SDH", true, "sup", 3, out, sizeof out));
    CHECK_STR(out, "x.en.sdh.forced.3.sup");
    CHECK(media_sub_sidecar("x.mkv", "und", "", false, "srt", 2, out, sizeof out)); /* a number needs a language */
    CHECK_STR(out, "x.und.2.srt");
    CHECK(media_sub_sidecar("x.mkv", "", "SDH", false, "idx", 2, out, sizeof out));
    CHECK_STR(out, "x.und.sdh.2.idx");
    /* ... and every one of them goes back to its video, with what its name says */
    static const struct {
        const char *name, *key;
    } back[] = {{"x.en.srt", "en"},          {"x.en.2.srt", "en.2"},        {"x.en.sdh.srt", "en.sdh"},
                {"x.en.forced.srt", "en.forced"}, {"x.en.sdh.forced.3.sup", "en.sdh.forced.3"},
                {"x.und.2.srt", "und.2"},    {"x.und.sdh.2.idx", "und.sdh.2"}, {"x.srt", "und"},
                {"x.en.idx", "en"},          {"x.en.sub", "en"},            {"x.sv.sup", "sv"},
                {"x.English.SDH.srt", "en.sdh"}, {"x.eng.hi.srt", "en.sdh"}, {"x.en.cc.2.srt", "en.sdh.2"},
                {"x.Swedish.foreign.srt", "sv.forced"}, {"x.en.1.srt", "en"}};
    for (size_t i = 0; i < sizeof back / sizeof back[0]; i++) {
        MediaSubTags t;
        char key[40];
        bool ok = media_sub_tags("x.mkv", back[i].name, &t);
        if (!ok) test_fail(__FILE__, __LINE__, "%s isn't x.mkv's", back[i].name);
        CHECK(ok);
        CHECK(media_sub_tags("x.web.mp4", back[i].name, NULL));
        media_sub_key(&t, key, sizeof key);
        CHECK_STR(key, back[i].key);
    }
    MediaSubTags t;
    CHECK(media_sub_tags("x.mkv", "x.en.sdh.2.srt", &t));
    CHECK_STR(t.lang, "en");
    CHECK(t.sdh && !t.forced && t.n == 2);
    /* a number only after a language: "Movie 2" is another video */
    CHECK(!media_sub_tags("Movie.mkv", "Movie 2.srt", &t));
    CHECK(!media_sub_tags("Movie.mkv", "Movie.2.srt", &t));
    CHECK(!media_sub_tags("Movie.mkv", "Movie.sdh.2.srt", &t));
    CHECK(!media_sub_tags("Movie.mkv", "Movie.2.en.srt", &t));
    CHECK(!media_sub_tags("Movie.mkv", "Movie.en.123.srt", &t)); /* (not a track's number) */
    CHECK(media_sub_tags("Movie.mkv", "Movie.en 2.srt", &t) && t.n == 2);
    /* the new kinds belong to their videos; a MicroDVD .sub is still a .sub (the library asks for its .idx) */
    char lang[16];
    CHECK(media_sub_for("Movie.mkv", "Movie.sv.sup", lang, sizeof lang) && str_eq(lang, "sv"));
    CHECK(media_sub_for("Movie.mkv", "Movie.idx", lang, sizeof lang) && str_eq(lang, ""));
    CHECK(!media_sub_for("Movie.mkv", "Movie.sv.txt", lang, sizeof lang));
    CHECK(media_is_sub_ext("a.SUP") && media_is_sub_ext("a.idx") && !media_is_sub_ext("a.sub"));
    CHECK(media_is_sub_pic("a.sup") && media_is_sub_pic("a.IDX") && media_is_sub_pic("a.sub") && !media_is_sub_pic("a.srt"));
    CHECK(media_is_vobsub_data("a.SUB") && !media_is_vobsub_data("a.idx"));
}
