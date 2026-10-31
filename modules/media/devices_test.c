/* 2.5: WHAT YOUR DEVICES PLAY (devices.c; owner, 2026-10-30: "check the issue with the videos that are not playable
   everywhere" - "Play everywhere 1456 of 2776"): Watch pages carry the list a browser answers, the answers are kept per
   device, and Library settings says what plays on every one of them - and why the rest doesn't play everywhere. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../av/mp4.h"
#include "media.h"

static char g_root[512];

static bool f_put(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)ctx) == n; }
static bool f_put_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    fflush(f);
    bool ok = file_seek(f, off) && fwrite(p, 1, n, f) == n;
    fflush(f);
    return fseek(f, 0, SEEK_END) == 0 && ok;
}

/* a tiny MP4 the probe reads: H.264 or HEVC, with AAC or Dolby (AC-3) sound */
static void put_mp4(const char *rel, int video, int audio) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    FILE *f = file_open(path, "wb");
    Mp4Out out = {f, f_put, f_put_at};
    Mp4 *m = mp4_new(&out, 0, 4096);
    static const uint8_t avcc[] = {1, 0x42, 0, 0x1E, 0xFF, 0xE0, 0}, hvcc[23] = {1, 2}, asc[] = {0x11, 0x90},
                         dac3[] = {0x10, 0x3D, 0xC0}, frame[16] = {0, 0, 0, 12};
    Mp4Track v = {.codec = video, .timescale = 1000, .width = 64, .height = 48};
    v.config = video == MP4_HEVC ? hvcc : avcc;
    v.config_len = video == MP4_HEVC ? sizeof hvcc : sizeof avcc;
    Mp4Track a = {.codec = audio, .timescale = 48000, .channels = 2, .sample_rate = 48000};
    a.config = audio == MP4_AC3 ? dac3 : asc;
    a.config_len = audio == MP4_AC3 ? sizeof dac3 : sizeof asc;
    mp4_add_track(m, &v);
    mp4_add_track(m, &a);
    mp4_begin(m);
    mp4_sample(m, 0, frame, sizeof frame, 60000, 0, true);
    mp4_sample(m, 1, frame, sizeof frame, 48000 * 60, 0, true);
    mp4_finish(m);
    mp4_free(m);
    fclose(f);
}

static const char *watch(const char *rel_end) { /* its watch page's address */
    static char url[64];
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_ends(ix->files[i].rel, rel_end)) snprintf(url, sizeof url, "/media/watch/%016llx", (unsigned long long)ix->files[i].id);
    return url;
}

static int with_role(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* an answer: every kind played, but those named ("hevc10", "ac3", "mkv") */
static const char *answer(const char *without) {
    static char caps[64], form[80];
    int n = media_devices_test_ncaps();
    memset(caps, 'y', (size_t)n);
    caps[n] = '\0';
    if (strstr(without, "hevc")) caps[1] = caps[2] = 'n'; /* (CAP_HEVC, CAP_HEVC10) */
    if (strstr(without, "ac3")) caps[9] = caps[10] = 'n'; /* (CAP_AC3, CAP_EAC3) */
    if (strstr(without, "mkv")) caps[n - 1] = 'n';        /* (CAP_MKV: the last) */
    if (strstr(without, "maybe")) caps[0] = 'm';          /* (a "maybe" plays) */
    snprintf(form, sizeof form, "caps=%s", caps);
    return form;
}

TEST(devices_say_what_they_play) {
    char data[512];
    str_copy(data, sizeof data, test_tmpdir());
    th_boot_in(data);
    int adam = with_role("Adam", ROLE_ADMIN), eva = with_role("Eva", ROLE_MEMBER), tv = with_role("Living room TV", ROLE_DISPLAY);
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put_mp4("Movies/Plain (2001)/Plain (2001).mp4", MP4_H264, MP4_AAC);
    put_mp4("Movies/Fresh (2020)/Fresh (2020).mp4", MP4_HEVC, MP4_AAC);
    put_mp4("Movies/Loud (2002)/Loud (2002).mp4", MP4_H264, MP4_AC3);
    setting_set("media.roots", g_root);
    media_scan_sync();
    /* Watch's pages carry the list a browser answers: one type each, | between */
    const char *h = th_page(adam, "/media");
    const char *list = strstr(h, "data-caps=\"");
    CHECK(list != NULL);
    if (list) {
        int bars = 0;
        for (const char *p = list + 11; *p && *p != '"'; p++) bars += *p == '|';
        CHECK_INT(bars, media_devices_test_ncaps() - 1);
        CHECK(str_starts(list, "data-caps=\"video/mp4; codecs=&quot;avc1.640028&quot;|"));
    }
    /* before any device: why the rest doesn't play everywhere, and none listed */
    h = th_page(adam, "/media/settings");
    CHECK_HAS(h, "What your devices play");
    CHECK_HAS(h, "<li><b>1</b> · HEVC needs hardware support");
    CHECK_HAS(h, "<li><b>1</b> · Dolby sound");
    CHECK_HAS(h, "None yet");
    CHECK(!strstr(h, "Play on your devices"));
    /* answers that aren't one: nothing kept */
    CHECK(th_act(adam, "media.caps", "caps=yyy").ok);
    char bad[80];
    snprintf(bad, sizeof bad, "%s", answer(""));
    bad[6] = 'x';
    CHECK(th_act(adam, "media.caps", bad).ok);
    CHECK_HAS(th_page(adam, "/media/settings"), "None yet");
    /* a PC's browser: everything but Dolby in an MP4 */
    CHECK(th_act(adam, "media.caps", answer("ac3 maybe")).ok);
    h = th_page(adam, "/media/settings");
    CHECK_HAS(h, "Play on your devices");
    CHECK_HAS(h, "<b>Adam</b>");
    CHECK_HAS(h, "plays 2 of 3");
    CHECK_HAS(h, "not 1 Dolby sound");
    CHECK_HAS(h, "2 of 3 videos play on this device.");
    CHECK_HAS(h, "Each one that opened Watch in the last 30 days:");
    CHECK_HAS(th_page(adam, watch("Fresh (2020).mp4")), "Plays on the one device of yours"); /* (the watch page) */
    CHECK_HAS(th_page(adam, watch("Loud (2002).mp4")), "Doesn't play on: Adam");
    CHECK(!strstr(th_page(adam, watch("Plain (2001).mp4")), "media-devs")); /* (plays everywhere: nothing to say) */
    /* a TV's (a display account's): no HEVC, but Dolby; the same answer again is no new device */
    CHECK(th_act(tv, "media.caps", answer("hevc")).ok);
    CHECK(th_act(tv, "media.caps", answer("hevc")).ok);
    h = th_page(adam, "/media/settings");
    CHECK_HAS(h, "<b>Living room TV</b>");
    CHECK_HAS(h, "not 1 HEVC (x265)");
    CHECK_HAS(h, "1 of 3 videos play on every one of these 2 devices.");
    CHECK_HAS(h, "<dt>Play on your devices</dt><dd>1 of 3</dd>");
    CHECK_HAS(th_page(adam, watch("Fresh (2020).mp4")), "Doesn't play on: Living room TV");
    /* members see why, not the household's devices */
    h = th_page(eva, "/media/settings");
    CHECK_HAS(h, "What your devices play");
    CHECK(!strstr(h, "The household's devices"));
    /* what plays where: the container too */
    MediaProbe p = {.container = "mkv", .vcodec = "h264", .acodec = "aac", .playable = PLAY_MAYBE};
    char caps[64];
    snprintf(caps, sizeof caps, "%s", answer("") + 5);
    CHECK(media_device_plays(caps, &p));
    snprintf(caps, sizeof caps, "%s", answer("mkv") + 5);
    CHECK(!media_device_plays(caps, &p));
    MediaProbe avi = {.container = "avi", .vcodec = "h264", .acodec = "mp3", .playable = PLAY_NO},
               webm = {.container = "webm", .vcodec = "vp9", .acodec = "opus", .playable = PLAY_YES},
               unread = {.container = "mkv", .playable = PLAY_UNKNOWN}; /* (an empty file named .mkv) */
    CHECK(!media_device_plays(caps, &avi));
    CHECK(media_device_plays(caps, &webm));
    CHECK(!media_device_plays("yyy", &webm)); /* (an answer to another list) */
    CHECK(!media_device_plays(caps, &unread));
    /* someone leaves: their devices go */
    Forget f = {.person = tv, .apply = true};
    CHECK_INT(media_devices_forget(&f), 1);
    CHECK(!strstr(th_page(adam, "/media/settings"), "Living room TV"));
    th_halt();
}
