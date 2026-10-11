/* Making videos play everywhere: which files need what, and (opt-in, with a real
 * file) a conversion checked by probing what came out. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "convert.h"

static MediaFile video(const char *container, const char *vcodec, const char *acodec) {
    MediaFile f;
    memset(&f, 0, sizeof f);
    f.type = MF_VIDEO;
    str_copy(f.rel, sizeof f.rel, "Show/Season 1/x.mkv");
    str_copy(f.probe.container, sizeof f.probe.container, container);
    str_copy(f.probe.vcodec, sizeof f.probe.vcodec, vcodec);
    str_copy(f.probe.acodec, sizeof f.probe.acodec, acodec);
    return f;
}

TEST(convert_knows_what_each_file_needs) {
    MediaFile f = video("mkv", "mpeg2", "ac3"); /* the owner's DVD rips */
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "h264", "aac"); /* season 7: only the box is wrong for iPhones */
    CHECK_INT(convert_need(&f), CONV_REMUX);
    f = video("mkv", "h264", "ac3"); /* picture fine, Dolby sound isn't */
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "mpeg2", "");
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mp4", "h264", "aac"); /* plays everywhere already */
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "hevc", "aac"); /* HEVC is copied into an MP4: Safari plays that, never MKV */
    CHECK_INT(convert_need(&f), CONV_REMUX);
    f = video("mkv", "hevc", "ac3");
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "hevc", "dts");
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "av1", "aac");
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "h264", "dts");
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "mpeg2", "ac3");
    f.web = 42; /* converted already */
    CHECK_INT(convert_need(&f), CONV_NONE);

    char out[256];
    convert_web_path("D:/lib/Show/Disc - 1/B1_t01.mkv", out, sizeof out);
    CHECK_STR(out, "D:/lib/Show/Disc - 1/B1_t01.web.mp4");
    convert_web_path("/mnt/media/a.b/movie", out, sizeof out); /* a dot in a folder, none in the name */
    CHECK_STR(out, "/mnt/media/a.b/movie.web.mp4");
    CHECK(media_is_web_copy("x/B1_t01.web.mp4"));
    CHECK(!media_is_web_copy("x/web.mp4.mkv"));
}

/* HEMMA_TEST_CONVERT=<an .mkv>: convert its first 30 s and check what came out. */
TEST(convert_real_file_when_given) {
    const char *src = plat_getenv("HEMMA_TEST_CONVERT");
    if (!src || !*src) return;
    ConvertJob j;
    memset(&j, 0, sizeof j);
    str_copy(j.src, sizeof j.src, src);
    snprintf(j.dst, sizeof j.dst, "%s/out.web.mp4", test_tmpdir());
    j.limit_ns = 30LL * 1000000000LL;
    int64_t t0 = now_ms();
    bool ok = convert_file(&j);
    if (!ok) test_fail(__FILE__, __LINE__, "convert failed: %s", j.err);
    CHECK(ok);
    MediaProbe p;
    CHECK(media_probe_file(j.dst, &p));
    CHECK_STR(p.container, "mp4");
    CHECK_STR(p.vcodec, "h264");
    CHECK_STR(p.acodec, "aac");
    CHECK_INT(p.playable, PLAY_YES);
    CHECK(p.duration_ms > 28000 && p.duration_ms < 32000);
    fprintf(stderr, "    converted 30 s in %lld ms: %s, %lld KB, %dx%d -> %s\n", (long long)(now_ms() - t0), j.what,
            (long long)(j.out_bytes >> 10), p.width, p.height, j.dst);
}
