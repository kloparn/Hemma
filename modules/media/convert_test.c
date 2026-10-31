/* Making videos play everywhere: which files need what, the queue's room on the disk, and
 * (opt-in, with a real file) a conversion checked by probing what came out. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../av/mp4.h"
#include "convert.h"
#include "room.h"

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
    f = video("mkv", "hevc", "eac3"); /* Dolby Digital Plus ("DDP5.1"): silent in Chrome until converted */
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "h264", "eac3");
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "hevc", "dts"); /* (1.3: DTS sound is made AAC too) */
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "av1", "aac"); /* (2.1: AV1 is copied too, as HEVC is) */
    CHECK_INT(convert_need(&f), CONV_REMUX);
    f = video("mkv", "av1", "eac3");
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("webm", "av1", "opus"); /* plays in the browsers as it is: nothing to do */
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "h264", "opus"); /* (2.5: Opus copied into an MP4 - Safari, Firefox and TVs don't take an MKV) */
    CHECK_INT(convert_need(&f), CONV_REMUX);
    f = video("mkv", "av1", "opus");
    CHECK_INT(convert_need(&f), CONV_REMUX);
    f = video("mp4", "h264", "opus"); /* (an MP4 with it plays as it is) */
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "vp9", "aac");
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "h264", "dts");
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "h264", "mp3"); /* (2.5: MP3 copied) */
    CHECK_INT(convert_need(&f), CONV_REMUX);
    f = video("mp4", "h264", "ac3"); /* (2.5: an MP4 with Dolby - DuckTales - gets a copy with AAC) */
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mov", "hevc", "eac3");
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mp4", "h264", "mp3"); /* ... one with MP3 or AAC plays as it is */
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mp4", "hevc", "aac");
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "mpeg4", "mp3"); /* (2.5: Xvid - Solsidan S07 - decoded and made H.264, its MP3 copied) */
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mp4", "mpeg4", "aac"); /* (... in an MP4 too: S06E03/E04) */
    CHECK_INT(convert_need(&f), CONV_TRANSCODE);
    f = video("mkv", "mpeg4", "vorbis"); /* (sound the converter can't take) */
    CHECK_INT(convert_need(&f), CONV_NONE);
    f = video("mkv", "msmpeg4", "mp3"); /* DivX 3: another codec */
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

/* ------------------------------------------------- a DVD rip, as far as the library can tell */

static void eb_id(Buf *b, uint32_t id) {
    for (int s = 24; s >= 0; s -= 8)
        if (id >> s) buf_putc(b, (char)(id >> s));
}
static size_t eb_open(Buf *b, uint32_t id) { /* 8-byte size, filled in by eb_close */
    eb_id(b, id);
    size_t at = b->len;
    buf_put(b, "\x01\0\0\0\0\0\0\0", 8);
    return at;
}
static void eb_close(Buf *b, size_t at) {
    uint64_t n = b->len - at - 8;
    for (int i = 1; i < 8; i++) b->p[at + i] = (char)(n >> (8 * (7 - i)));
}
static void eb_str(Buf *b, uint32_t id, const char *s) {
    eb_id(b, id);
    buf_putc(b, (char)(0x80 | strlen(s)));
    buf_puts(b, s);
}
static void eb_small(Buf *b, uint32_t id, unsigned v) { /* v < 256 */
    eb_id(b, id);
    buf_putc(b, (char)0x81);
    buf_putc(b, (char)v);
}

/* MPEG-2 video and AC-3 sound in Matroska ("webm" doctype: in WebM), a minute long, `size`
   bytes: what the probe makes of a DVD rip - nothing a converter could read. Also used by
   upload_test.c. */
void conv_test_rip(const char *root, const char *rel, const char *doctype, int64_t size) {
    Buf b = {0};
    size_t h = eb_open(&b, 0x1A45DFA3);
    eb_str(&b, 0x4282, doctype);
    eb_close(&b, h);
    size_t seg = eb_open(&b, 0x18538067), info = eb_open(&b, 0x1549A966);
    double ms = 60000.0;
    uint64_t u;
    memcpy(&u, &ms, 8);
    eb_id(&b, 0x4489);
    buf_putc(&b, (char)0x88);
    for (int i = 7; i >= 0; i--) buf_putc(&b, (char)(u >> (8 * i)));
    eb_close(&b, info);
    size_t tracks = eb_open(&b, 0x1654AE6B);
    static const char *const codec[] = {"V_MPEG2", "A_AC3"};
    for (unsigned t = 0; t < 2; t++) {
        size_t e = eb_open(&b, 0xAE);
        eb_small(&b, 0xD7, t + 1); /* track number */
        eb_small(&b, 0x83, t + 1); /* video, audio */
        eb_str(&b, 0x86, codec[t]);
        eb_close(&b, e);
    }
    eb_close(&b, tracks);
    eb_close(&b, seg);
    while ((int64_t)b.len < size) buf_putc(&b, 0);
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, b.p, b.len);
    buf_free(&b);
}

static int make_admin(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

static const char *id_form(uint64_t id) {
    static char form[40];
    char hex[17];
    media_id_hex(id, hex);
    snprintf(form, sizeof form, "id=%s", hex);
    return form;
}

static bool started(const char *want) {
    const char *got = convert_test_start();
    if (!got || !want) return got == want;
    return str_eq(got, want);
}

/* A copy starts only when its original's size fits on the disk next to what is running (and
   1 GB stays free). What doesn't fit waits while others run; with nothing running there is
   nothing to wait for, so it's set aside with one line on the card - not tried, and "Convert
   all" doesn't churn through every file failing. */
TEST(convert_waits_for_room_and_sets_aside_what_cannot_fit) {
    th_boot();
    int adam = make_admin("Adam"), eva = th_person("Eva");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    conv_test_rip(root, "Series/Show/Season 1/Show.S01E01.mkv", "matroska", 3000);
    conv_test_rip(root, "Series/Show/Season 1/Show.S01E02.mkv", "matroska", 3000);
    conv_test_rip(root, "Series/Show/Season 1/Show.S01E03.mkv", "matroska", 20000);
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->nepisodes, 3);
    if (ix->nepisodes != 3) return;
    CHECK_INT(convert_need(&ix->files[ix->episodes[0]]), CONV_TRANSCODE);
    uint64_t e2 = ix->files[ix->episodes[1]].id, e3 = ix->files[ix->episodes[2]].id;

    CHECK(!th_act(eva, "media.convert", "").ok); /* admins only */
    media_test_free_space(MEDIA_RESERVE + 5000); /* room for one small copy at a time */
    ThResult r = th_act(adam, "media.convert", "");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Converting 2 videos");
    CHECK_HAS(r.toast, "1 more doesn't fit on the drive");
    const char *page = th_page(adam, "/media/settings");
    CHECK_HAS(page, "Converting · 0 of 2 done");
    CHECK_HAS(page, "1 video didn't fit on the drive: Show S01E03 needs up to 20 KB, and the drive has 5 KB to spare "
                    "(Hemma keeps 1 GB free). Make room, then convert again.");

    CHECK(started("Show S01E01"));
    CHECK(started(NULL)); /* E01 holds 3000 of the 5000: E02 waits for it ... */
    CHECK(convert_queued(e2));
    CHECK(!convert_queued(e3));
    convert_test_finish(true);
    CHECK(started("Show S01E02")); /* ... and starts once E01 is done */
    convert_test_finish(true);
    page = th_page(adam, "/media/settings");
    CHECK_HAS(page, "2 converted");
    CHECK_HAS(page, "Convert 3 videos"); /* (no copies were really made) */

    /* still no room when its turn comes (the disk filled meanwhile): set aside, not tried */
    media_test_free_space(MEDIA_RESERVE + 50000);
    r = th_act(adam, "media.convert", id_form(e3));
    CHECK_HAS(r.toast, "Converting 1 video, 1 at a time");
    CHECK(convert_queued(e3));
    media_test_free_space(MEDIA_RESERVE + 100);
    CHECK(started(NULL));
    CHECK(!convert_queued(e3));
    CHECK_HAS(th_page(adam, "/media/settings"), "1 video didn't fit on the drive: Show S01E03 needs up to 20 KB");

    /* asked for when it can't fit: said at once */
    r = th_act(adam, "media.convert", id_form(e3));
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Not enough room on the drive: Show S01E03 needs up to 20 KB, and the drive has 0 KB to spare");
    CHECK(!convert_queued(e3));
    r = th_act(adam, "media.convert", "");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Not enough room on the drive for 3 videos: Show S01E01 needs up to 3 KB");
    media_test_free_space(-1);
    th_halt();
}

/* What a finished conversion leaves: a real (tiny) H.264 MP4 lasting a minute, like the rip. */
static bool mp4_put(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)ctx) == n; }
static bool mp4_put_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    fflush(f);
    bool ok = file_seek(f, off) && fwrite(p, 1, n, f) == n;
    fflush(f);
    return fseek(f, 0, SEEK_END) == 0 && ok;
}
static void made_copy(const char *root, const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    FILE *f = file_open(path, "wb");
    Mp4Out out = {f, mp4_put, mp4_put_at};
    Mp4 *m = mp4_new(&out, 0, 4096);
    static const uint8_t cfg[] = {1, 0x42, 0, 0x1E, 0xFF, 0xE0, 0};
    Mp4Track t = {.codec = MP4_H264, .timescale = 1000, .config = cfg, .config_len = sizeof cfg, .width = 64, .height = 48};
    mp4_add_track(m, &t);
    mp4_begin(m);
    static const uint8_t frame[16] = {0, 0, 0, 12};
    mp4_sample(m, 0, frame, sizeof frame, 60000, 0, true);
    mp4_finish(m);
    mp4_free(m);
    fclose(f);
}

static bool there(const char *root, const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    return path_stat(path, NULL, NULL, NULL);
}

/* "Do that by itself after each conversion" on a nearly full disk: the next one waits for the
   finished one's original to go (that's the room it needs) instead of being set aside - and an
   original whose copy just finished goes even when Convert is pressed again right away. */
TEST(convert_waits_for_an_original_about_to_go) {
    th_boot();
    int adam = make_admin("Adam");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    conv_test_rip(root, "Series/Two/Season 1/Two.S01E01.mkv", "matroska", 2000);
    conv_test_rip(root, "Series/Two/Season 1/Two.S01E02.mkv", "matroska", 2000);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK(th_act(adam, "media.drop_auto", "on=1").ok);
    media_test_free_space(MEDIA_RESERVE + 3000); /* room for one original past the reserve */
    ThResult r = th_act(adam, "media.convert", "");
    CHECK_HAS(r.toast, "Converting 2 videos");
    CHECK(started("Two S01E01"));
    convert_test_finish(true);
    made_copy(root, "Series/Two/Season 1/Two.S01E01.web.mp4");
    media_test_free_space(MEDIA_RESERVE + 1000); /* its copy takes room, its original not gone yet */
    CHECK(started(NULL));                        /* E02 waits for that ... */
    CHECK(convert_queued(media_lib()->files[media_lib()->episodes[1]].id)); /* ... rather than being set aside */
    convert_tick(now_ms()); /* the original goes */
    CHECK(!there(root, "Series/Two/Season 1/Two.S01E01.mkv"));
    media_test_free_space(MEDIA_RESERVE + 3000);
    CHECK(started("Two S01E02"));
    convert_test_finish(true);
    made_copy(root, "Series/Two/Season 1/Two.S01E02.web.mp4");
    th_act(adam, "media.convert", ""); /* pressed again before a tick: E02's original still goes */
    CHECK(!there(root, "Series/Two/Season 1/Two.S01E02.mkv"));
    CHECK(there(root, "Series/Two/Season 1/Two.S01E02.web.mp4"));
    media_test_free_space(-1);
    th_halt();
}

/* A long "Convert all": the tick's change signature wraps instead of overflowing (the debug
   build traps on signed overflow). */
TEST(convert_tick_copes_with_a_long_queue) {
    th_boot();
    int adam = make_admin("Adam");
    char root[512], rel[120];
    str_copy(root, sizeof root, test_tmpdir());
    for (int e = 1; e <= 20; e++) {
        snprintf(rel, sizeof rel, "Series/Many/Season 1/Many.S01E%02d.mkv", e);
        conv_test_rip(root, rel, "matroska", 1000);
    }
    setting_set("media.roots", root);
    media_scan_sync();
    media_test_free_space(1LL << 40);
    ThResult r = th_act(adam, "media.convert", "");
    CHECK_HAS(r.toast, "Converting 20 videos");
    CHECK(started("Many S01E01")); /* a running one first, 19 after it */
    convert_tick(now_ms());
    convert_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings"), "Converting · 0 of 20 done");
    media_test_free_space(-1);
    th_halt();
}

/* A half-made copy nobody is making (a crash, a power cut) goes at the next scan; one that a
   queued conversion is making stays. */
TEST(convert_leftovers_go_at_the_next_scan) {
    th_boot();
    int adam = make_admin("Adam");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    conv_test_rip(root, "Films/A (2001)/A (2001).mkv", "matroska", 1000);
    setting_set("media.roots", root);
    media_scan_sync();
    media_test_free_space(1LL << 40);
    ThResult r = th_act(adam, "media.convert", "");
    CHECK_HAS(r.toast, "Converting 1 video");
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/Films/A (2001)/A (2001).web.mp4.conv.part", root);
    CHECK(file_write_atomic(path, "being made", 10));
    snprintf(dir, sizeof dir, "%s/Films/B (2002)", root);
    dir_make(dir);
    snprintf(path, sizeof path, "%s/B (2002).web.mp4.conv.part", dir);
    CHECK(file_write_atomic(path, "left over", 9));
    media_scan_sync();
    CHECK(there(root, "Films/A (2001)/A (2001).web.mp4.conv.part"));
    CHECK(!there(root, "Films/B (2002)/B (2002).web.mp4.conv.part"));
    media_test_free_space(-1);
    th_halt();
}

/* The converter writes its own temp file, and never replaces a copy that is there. */
TEST(convert_never_writes_over_a_copy) {
    char root[512], dst[1100], part[1100];
    str_copy(root, sizeof root, test_tmpdir());
    conv_test_rip(root, "S/x.mkv", "matroska", 4000);
    ConvertJob j;
    memset(&j, 0, sizeof j);
    snprintf(j.src, sizeof j.src, "%s/S/x.mkv", root);
    convert_web_path(j.src, j.dst, sizeof j.dst);
    convert_part_path(j.dst, part, sizeof part);
    CHECK(str_ends(part, "/S/x.web.mp4.conv.part")); /* never an upload's "x.web.mp4.part" */
    CHECK(file_write_atomic(j.dst, "uploaded", 8)); /* an upload brought the copy */
    CHECK(!convert_file(&j));
    CHECK_HAS(j.err, "copy is there already");
    Buf got = {0};
    CHECK(file_read_all(j.dst, &got, 100) && got.len == 8 && memcmp(got.p, "uploaded", 8) == 0);
    buf_free(&got);
    CHECK(!path_stat(part, NULL, NULL, NULL));
    str_copy(dst, sizeof dst, j.dst);
    file_remove(dst);
    CHECK(!convert_file(&j)); /* (nothing a converter can read in there) */
    CHECK(!path_stat(part, NULL, NULL, NULL)); /* a failed one leaves nothing behind */
    CHECK(!path_stat(dst, NULL, NULL, NULL));
}

/* Subtitles taken out of a file go next to it, named so the library pairs them with the video and
   its copy - and never over a file that is there. */
static AvConvSub text_sub(const char *lang, const char *name, bool forced, const char *srt) {
    AvConvSub s = {.forced = forced, .kind = AV_SUB_TEXT, .srt = (char *)srt, .len = strlen(srt)};
    str_copy(s.lang, sizeof s.lang, lang);
    str_copy(s.name, sizeof s.name, name);
    return s;
}

static bool file_is(const char *root, const char *name, const char *want) {
    char path[800];
    snprintf(path, sizeof path, "%s/%s", root, name);
    Buf got = {0};
    bool ok = file_read_all(path, &got, 1000) && got.len == strlen(want) && memcmp(got.p, want, got.len) == 0;
    buf_free(&got);
    return ok;
}

TEST(convert_saves_subtitles_next_to_the_original) {
    char root[512], video[700];
    str_copy(root, sizeof root, test_tmpdir());
    snprintf(video, sizeof video, "%s/x.mkv", root);
    AvConvSub s = text_sub("eng", "English", false, "1\n00:00:01,000 --> 00:00:02,000\nHi\n\n");
    CHECK(convert_save_sub(video, &s, NULL));
    CHECK(file_is(root, "x.en.srt", s.srt));
    s = text_sub("en-GB", "", false, "other");
    CHECK(!convert_save_sub(video, &s, NULL)); /* the same name: the first stays */
    CHECK(file_is(root, "x.en.srt", "1\n00:00:01,000 --> 00:00:02,000\nHi\n\n"));
    s = text_sub("und", "English SDH", true, "x");
    CHECK(convert_save_sub(video, &s, NULL));
    CHECK(there(root, "x.sdh.forced.srt"));
    s = text_sub("swe", "", false, "");
    CHECK(!convert_save_sub(video, &s, NULL)); /* nothing to save */
    char lang[16];
    CHECK(media_sub_for("x.mkv", "x.en.srt", lang, sizeof lang) && str_eq(lang, "en"));
    CHECK(media_sub_for("x.web.mp4", "x.en.srt", lang, sizeof lang) && str_eq(lang, "en"));
    CHECK(media_sub_for("x.mkv", "x.sdh.forced.srt", lang, sizeof lang) && str_eq(lang, ""));
}

/* Contract C1: text -> .srt, Blu-ray PGS -> .sup, DVD VobSub -> .idx + .sub (the pair or nothing); a
   second track in one language and flags is ".2" - counted by the file's tracks, so a run again names
   them alike - and nothing is ever written over. */
TEST(convert_saves_every_kind_of_subtitle_and_numbers_the_second) {
    char root[512], video[700];
    str_copy(root, sizeof root, test_tmpdir());
    snprintf(video, sizeof video, "%s/Film.web.mp4", root); /* (a copy: named for "Film") */
    static uint8_t sup[] = {'P', 'G', 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0, 0};
    static uint8_t vob[] = {0, 0, 1, 0xBA, 0x44};
    static char idx[] = "# VobSub index file, v7\nsize: 720x480\nid: en, index: 0\n";
    AvConvSub subs[7] = {
        text_sub("eng", "English", false, "one"),
        text_sub("eng", "English (Commentary)", false, "two"), /* the same language and flags: .2 */
        text_sub("eng", "English SDH", false, "sdh"),          /* SDH: a variant of its own */
        {.lang = "eng", .kind = AV_SUB_PGS, .data = sup, .size = sizeof sup}, /* a third English: .3 */
        {.lang = "swe", .kind = AV_SUB_VOBSUB, .data = vob, .size = sizeof vob, .idx = idx, .idx_len = strlen(idx)},
        {.lang = "fre", .kind = AV_SUB_VOBSUB, .data = vob, .size = sizeof vob}, /* no .idx: nothing */
        {.lang = "ger", .kind = AV_SUB_PGS},                                      /* no data: nothing */
    };
    CHECK_INT(convert_save_subs(video, subs, 7), 5);
    CHECK(file_is(root, "Film.en.srt", "one"));
    CHECK(file_is(root, "Film.en.2.srt", "two"));
    CHECK(file_is(root, "Film.en.sdh.srt", "sdh"));
    char path[800];
    Buf got = {0};
    snprintf(path, sizeof path, "%s/Film.en.3.sup", root);
    CHECK(file_read_all(path, &got, 1000) && got.len == sizeof sup && memcmp(got.p, sup, sizeof sup) == 0);
    buf_reset(&got);
    snprintf(path, sizeof path, "%s/Film.sv.sub", root);
    CHECK(file_read_all(path, &got, 1000) && got.len == sizeof vob && memcmp(got.p, vob, sizeof vob) == 0);
    buf_free(&got);
    CHECK(file_is(root, "Film.sv.idx", idx));
    CHECK(!there(root, "Film.fr.idx") && !there(root, "Film.fr.sub") && !there(root, "Film.de.sup"));
    /* every one of them goes to the video, as its own choice */
    static const char *const names[] = {"Film.en.srt", "Film.en.2.srt", "Film.en.sdh.srt", "Film.en.3.sup", "Film.sv.idx",
                                        "Film.sv.sub"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) CHECK(media_sub_for("Film.mkv", names[i], NULL, 0));
    /* again (the copy made anew): each track finds its own name there - nothing written over, nothing new */
    subs[0].srt = subs[1].srt = (char *)"changed";
    subs[0].len = subs[1].len = 7;
    CHECK_INT(convert_save_subs(video, subs, 7), 0);
    CHECK(file_is(root, "Film.en.srt", "one"));
    CHECK(file_is(root, "Film.en.2.srt", "two"));
    CHECK(!there(root, "Film.en.4.srt"));
    /* a VobSub pair: a .sub already there (MicroDVD text, the household's) keeps its .idx from being written */
    snprintf(video, sizeof video, "%s/Other.mkv", root);
    snprintf(path, sizeof path, "%s/Other.sv.sub", root);
    CHECK(file_write_atomic(path, "{1}{2}text", 10));
    CHECK(!convert_save_sub(video, &subs[4], NULL));
    CHECK(!there(root, "Other.sv.idx") && file_is(root, "Other.sv.sub", "{1}{2}text"));
}

/* HEMMA_TEST_CONVERT=<an .mkv>: convert its first 30 s (HEMMA_TEST_CONVERT_S: another length; 0 = all of it) and
   check what came out (its text subtitles land next to it as .srt: point it at a copy, not at a household's
   original). HEMMA_TEST_CONVERT_OUT=<file.mp4> keeps the copy there to play (else it goes with the test's folder). */
TEST(convert_real_file_when_given) {
    const char *src = plat_getenv("HEMMA_TEST_CONVERT");
    if (!src || !*src) return;
    const char *len = plat_getenv("HEMMA_TEST_CONVERT_S"), *keep = plat_getenv("HEMMA_TEST_CONVERT_OUT");
    int secs = len && *len ? atoi(len) : 30;
    ConvertJob j;
    memset(&j, 0, sizeof j);
    str_copy(j.src, sizeof j.src, src);
    if (keep && *keep) str_copy(j.dst, sizeof j.dst, keep);
    else snprintf(j.dst, sizeof j.dst, "%s/out.web.mp4", test_tmpdir());
    j.limit_ns = secs * 1000000000LL;
    int64_t t0 = now_ms();
    bool ok = convert_file(&j);
    if (!ok) test_fail(__FILE__, __LINE__, "convert failed: %s", j.err);
    CHECK(ok);
    MediaProbe p, in;
    CHECK(media_probe_file(j.dst, &p));
    CHECK(media_probe_file(src, &in));
    CHECK_STR(p.container, "mp4");
    CHECK(str_eq(p.vcodec, "h264") || str_eq(p.vcodec, "hevc") || str_eq(p.vcodec, "av1")); /* (HEVC and AV1 are copied) */
    CHECK(str_eq(p.acodec, "aac") || ((str_eq(p.acodec, "mp3") || str_eq(p.acodec, "opus")) && str_eq(in.acodec, p.acodec)));
    /* (2.5: MP3 and Opus are copied) */
    CHECK_INT(p.playable, str_eq(p.vcodec, "h264") ? PLAY_YES : PLAY_MAYBE);
    int64_t want = secs > 0 ? secs * 1000LL : in.duration_ms;
    CHECK(p.duration_ms > want - 2000 && p.duration_ms < want + 2000);
    fprintf(stderr, "    converted %lld s in %lld ms: %s, %lld KB, %dx%d, %d subtitle track(s) saved -> %s\n",
            (long long)(p.duration_ms / 1000), (long long)(now_ms() - t0), j.what, (long long)(j.out_bytes >> 10), p.width,
            p.height, j.subs, j.dst);
}
