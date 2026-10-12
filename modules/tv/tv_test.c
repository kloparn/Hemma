/* tv: the schedule math on its own, then channels built from a small fake
 * library of synthetic MP4s whose lengths we choose. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../media/library.h"
#include "tv.h"

/* =============================================================== pure math */

static TvItem items3[] = {{1, 100}, {2, 200}, {3, 300}};

TEST(tv_rotate_round_robins_across_shows) {
    TvItem a[] = {{11, 1}, {12, 1}, {13, 1}}, b[] = {{21, 1}}, c[] = {{31, 1}, {32, 1}};
    TvStream s[] = {{a, 3}, {b, 1}, {c, 2}};
    TvItem out[10];
    CHECK_INT(tv_rotate(s, 3, out, 10), 6);
    const uint64_t want[] = {11, 21, 31, 12, 32, 13};
    for (int i = 0; i < 6; i++) CHECK_INT(out[i].file, want[i]);
    CHECK_INT(tv_rotate(s, 3, out, 4), 4); /* capped */
    CHECK_INT(out[3].file, 12);
    CHECK_INT(tv_rotate(s, 0, out, 10), 0);
}

TEST(tv_shuffle_is_a_deterministic_permutation) {
    TvItem a[20], b[20], c[20];
    for (int i = 0; i < 20; i++) a[i] = b[i] = c[i] = (TvItem){(uint64_t)i + 1, 10};
    tv_shuffle(a, 20, 42);
    tv_shuffle(b, 20, 42);
    tv_shuffle(c, 20, 43);
    bool seen[21] = {0};
    int stayed = 0, differs = 0;
    for (int i = 0; i < 20; i++) {
        CHECK(a[i].file == b[i].file); /* same seed, same order */
        CHECK(a[i].file >= 1 && a[i].file <= 20 && !seen[a[i].file]);
        seen[a[i].file] = true;
        stayed += a[i].file == (uint64_t)i + 1;
        differs += c[i].file != a[i].file;
    }
    CHECK(stayed < 20);
    CHECK(differs > 0);
    tv_shuffle(c, 0, 1); /* degenerate sizes are fine */
    tv_shuffle(c, 1, 1);
}

TEST(tv_now_back_to_back_and_wrapping) {
    TvPlan p;
    tv_plan_init(&p, items3, 3, false);
    CHECK_INT(p.cycle, 600);
    int64_t E = 1800000000;
    TvSlot s;
    CHECK(tv_now(&p, E, E, &s));
    CHECK(s.file == 1 && s.offset == 0 && s.start == E && s.end == E + 100 && s.next == E + 100 && !s.gap);
    CHECK(tv_now(&p, E, E + 150, &s));
    CHECK(s.file == 2 && s.offset == 50 && s.start == E + 100);
    CHECK(tv_now(&p, E, E + 599, &s));
    CHECK(s.file == 3 && s.offset == 299);
    CHECK(tv_now(&p, E, E + 600, &s)); /* the loop starts over */
    CHECK(s.file == 1 && s.offset == 0 && s.start == E + 600);
    CHECK(tv_now(&p, E, E + 6350, &s));
    CHECK(s.file == 3 && s.offset == 50 && s.start == E + 6300);
    tv_plan_free(&p);
}

TEST(tv_now_with_the_epoch_in_the_future) {
    TvPlan p;
    tv_plan_init(&p, items3, 3, false);
    int64_t E = 1800000000;
    TvSlot s;
    CHECK(tv_now(&p, E, E - 50, &s)); /* 550 s into the loop before */
    CHECK(s.file == 3);
    CHECK_INT(s.offset, 250);
    CHECK(s.start == E - 300 && s.next == E);
    CHECK(tv_now(&p, E, E - 600, &s));
    CHECK(s.file == 1 && s.offset == 0);
    tv_plan_free(&p);
}

TEST(tv_half_hour_alignment_leaves_intermissions) {
    TvItem it[] = {{1, 1320}, {2, 2700}, {3, 600}}; /* 22, 45 and 10 minutes */
    TvPlan p;
    tv_plan_init(&p, it, 3, true);
    CHECK_INT(p.cycle, 1800 + 3600 + 1800);
    int64_t E = 1800000000 + 1000; /* not on a half hour */
    int64_t e = tv_epoch(&p, E);
    CHECK(e % 1800 == 0 && e <= E && E - e < 1800);
    TvSlot s;
    CHECK(tv_now(&p, E, e + 100, &s));
    CHECK(s.file == 1 && !s.gap && s.offset == 100);
    CHECK(tv_now(&p, E, e + 1400, &s)); /* 22 minutes are over: intermission until :30 */
    CHECK(s.file == 1 && s.gap && s.offset == 0 && s.end == e + 1320 && s.next == e + 1800);
    CHECK(tv_now(&p, E, e + 1800, &s));
    CHECK(s.file == 2 && !s.gap && s.start == e + 1800);
    CHECK(tv_now(&p, E, e + 1800 + 2800, &s));
    CHECK(s.file == 2 && s.gap && s.next == e + 5400);
    CHECK(tv_now(&p, E, e + 7200, &s));
    CHECK(s.file == 1 && s.start == e + 7200);
    for (int64_t t = e - 5000; t < e + 3 * 7200; t += 997) { /* every programme starts on :00 or :30 */
        CHECK(tv_now(&p, E, t, &s));
        CHECK(s.start % 1800 == 0 && s.next % 1800 == 0 && s.start <= t && t < s.next);
    }
    TvPlan q;
    tv_plan_init(&q, it, 3, false);
    CHECK(tv_epoch(&q, E) == E); /* back-to-back plans keep the exact epoch */
    tv_plan_free(&q);
    tv_plan_free(&p);
}

TEST(tv_guide_window_boundaries) {
    TvPlan p;
    tv_plan_init(&p, items3, 3, false);
    int64_t E = 1800000000;
    TvSlot w[16];
    int n = tv_window(&p, E, E + 50, E + 400, w, 16);
    CHECK_INT(n, 3); /* running at the start, plus everything starting before the end */
    CHECK(w[0].file == 1 && w[1].file == 2 && w[2].file == 3);
    n = tv_window(&p, E, E + 100, E + 300, w, 16); /* half-open: 1 ends at 100, 3 starts at 300 */
    CHECK_INT(n, 1);
    CHECK(w[0].file == 2);
    n = tv_window(&p, E, E, E + 1300, w, 16); /* across two wraps */
    CHECK_INT(n, 7);
    CHECK(w[6].file == 1 && w[6].start == E + 1200);
    CHECK_INT(tv_window(&p, E, E, E + 1300, w, 2), 2);
    CHECK_INT(tv_window(&p, E, E + 10, E + 10, w, 16), 0);
    tv_plan_free(&p);
}

TEST(tv_single_programme_channel_loops_as_new_airings) {
    TvItem one[] = {{7, 300}};
    TvPlan p;
    tv_plan_init(&p, one, 1, false);
    int64_t E = 1800000000;
    TvSlot a, b;
    CHECK(tv_now(&p, E, E + 299, &a));
    CHECK(tv_now(&p, E, E + 300, &b));
    CHECK(a.file == 7 && b.file == 7);
    CHECK(a.start != b.start); /* a distinct airing: the player gets a fresh element */
    CHECK_INT(b.offset, 0);
    tv_plan_free(&p);
}

TEST(tv_empty_plan_is_off_air) {
    TvPlan p;
    tv_plan_init(&p, NULL, 0, true);
    TvSlot s, w[4];
    CHECK(!tv_now(&p, 1, 2, &s));
    CHECK(!tv_now(NULL, 1, 2, &s));
    CHECK_INT(tv_window(&p, 1, 2, 100, w, 4), 0);
    tv_plan_free(&p);
    TvItem z[] = {{1, 0}}; /* a zero length still takes a second */
    tv_plan_init(&p, z, 1, false);
    CHECK_INT(p.cycle, 1);
    tv_plan_free(&p);
}

/* ======================================================= synthetic library */

static void p8(Buf *b, unsigned v) { buf_putc(b, (char)(v & 0xFF)); }
static void p16(Buf *b, unsigned v) {
    p8(b, v >> 8);
    p8(b, v);
}
static void p32(Buf *b, uint32_t v) {
    p16(b, v >> 16);
    p16(b, v & 0xFFFF);
}
static void pz(Buf *b, size_t n) {
    while (n--) p8(b, 0);
}
static size_t box(Buf *b, const char *type) {
    size_t at = b->len;
    p32(b, 0);
    buf_put(b, type, 4);
    return at;
}
static void end_box(Buf *b, size_t at) {
    uint32_t v = (uint32_t)(b->len - at);
    for (int i = 0; i < 4; i++) b->p[at + i] = (char)(v >> (24 - 8 * i));
}

static void trak(Buf *b, uint32_t id, bool video) {
    size_t t = box(b, "trak"), th = box(b, "tkhd");
    p32(b, 3), p32(b, 0), p32(b, 0), p32(b, id), p32(b, 0), p32(b, 0), pz(b, 8), pz(b, 8), pz(b, 36);
    p32(b, video ? 1280u << 16 : 0), p32(b, video ? 720u << 16 : 0);
    end_box(b, th);
    size_t md = box(b, "mdia"), mh = box(b, "mdhd");
    p32(b, 0), p32(b, 0), p32(b, 0), p32(b, 1000), p32(b, 0), p16(b, 0x55C4), p16(b, 0);
    end_box(b, mh);
    size_t hd = box(b, "hdlr");
    p32(b, 0), p32(b, 0), buf_put(b, video ? "vide" : "soun", 4), pz(b, 12), buf_put(b, "h", 2);
    end_box(b, hd);
    size_t mi = box(b, "minf"), st = box(b, "stbl"), sd = box(b, "stsd");
    p32(b, 0), p32(b, 1);
    if (video) {
        size_t a = box(b, "avc1");
        pz(b, 6), p16(b, 1), p16(b, 0), p16(b, 0), pz(b, 12), p16(b, 1280), p16(b, 720);
        p32(b, 0x00480000), p32(b, 0x00480000), p32(b, 0), p16(b, 1), pz(b, 32), p16(b, 0x18), p16(b, 0xFFFF);
        size_t c = box(b, "avcC");
        pz(b, 8);
        end_box(b, c);
        end_box(b, a);
    } else {
        size_t a = box(b, "mp4a");
        pz(b, 6), p16(b, 1), pz(b, 8), p16(b, 2), p16(b, 16), p16(b, 0), p16(b, 0), p32(b, 48000u << 16);
        size_t e = box(b, "esds");
        p32(b, 0);
        p8(b, 0x03), p8(b, 0x80), p8(b, 0x80), p8(b, 0x80), p8(b, 25), p16(b, 1), p8(b, 0);
        p8(b, 0x04), p8(b, 17), p8(b, 0x40), p8(b, 0x15), pz(b, 3), p32(b, 128000), p32(b, 128000);
        p8(b, 0x05), p8(b, 2), p8(b, 0x11), p8(b, 0x90), p8(b, 0x06), p8(b, 1), p8(b, 2);
        end_box(b, e);
        end_box(b, a);
    }
    end_box(b, sd), end_box(b, st), end_box(b, mi), end_box(b, md), end_box(b, t);
}

/* An H.264/AAC MP4 header of `ms` milliseconds: probes as browser-playable. */
static void put_mp4(const char *root, const char *rel, uint32_t ms) {
    Buf b = {0};
    size_t f = box(&b, "ftyp");
    buf_put(&b, "isom", 4), p32(&b, 0x200), buf_put(&b, "isomavc1", 8);
    end_box(&b, f);
    size_t m = box(&b, "moov"), mv = box(&b, "mvhd");
    p32(&b, 0), p32(&b, 0), p32(&b, 0), p32(&b, 1000), p32(&b, ms);
    p32(&b, 0x00010000), p16(&b, 0x0100), pz(&b, 10), pz(&b, 36), pz(&b, 24), p32(&b, 3);
    end_box(&b, mv);
    trak(&b, 1, true);
    trak(&b, 2, false);
    end_box(&b, m);
    size_t d = box(&b, "mdat");
    pz(&b, 64);
    end_box(&b, d);
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, b.p, b.len);
    buf_free(&b);
}

static void put_junk(const char *root, const char *rel) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, "not a video", 11);
}

static char g_root[512];
static int adam;

/* Alpha: 2 × 22 min + one unreadable episode; Beta: 45 + 44 min; Gamma: a 90 min movie. */
static void library(void) {
    th_boot();
    g_fake_now = dt_make(2026, 10, 10, 20, 0);
    adam = th_person("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put_mp4(g_root, "Series/Alpha/Season 1/Alpha.S01E01.Pilot.mp4", 1320000);
    put_mp4(g_root, "Series/Alpha/Season 1/Alpha.S01E02.mp4", 1320000);
    put_junk(g_root, "Series/Alpha/Season 1/Alpha.S01E03.mp4");
    put_mp4(g_root, "Series/Beta/Season 1/Beta.S01E01.mp4", 2700000);
    put_mp4(g_root, "Series/Beta/Season 1/Beta.S01E02.mp4", 2640000);
    put_mp4(g_root, "Movies/Gamma (2001)/Gamma.2001.mp4", 5400000);
    setting_set("media.roots", g_root);
    media_scan_sync();
}

static const MediaSeries *series_named(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nseries; i++)
        if (str_eq(ix->series[i].title, title)) return &ix->series[i];
    return NULL;
}

static uint64_t file_id(const char *rel) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_eq(ix->files[i].rel, rel)) return ix->files[i].id;
    return 0;
}

TEST(tv_library_is_what_we_think) {
    library();
    CHECK_INT(media_lib()->nseries, 2);
    CHECK_INT(media_lib()->nmovies, 1);
    const MediaFile *f = media_file(file_id("Series/Alpha/Season 1/Alpha.S01E01.Pilot.mp4"));
    CHECK(f != NULL);
    CHECK_INT(f->probe.duration_ms, 1320000);
    CHECK_INT(f->probe.playable, PLAY_YES);
    th_halt();
}

TEST(tv_channel_plan_rotates_shows_and_skips_unknown_lengths) {
    library();
    char form[256], a[17], b[17];
    media_id_hex(series_named("Alpha")->id, a);
    media_id_hex(series_named("Beta")->id, b);
    snprintf(form, sizeof form, "number=3&name=Sitcoms&show=%s&show=%s&show=%s&order=rotate&align=none", a, b, a);
    ThResult r = th_act(adam, "tv.save", form);
    CHECK(r.ok);
    CHECK_STR(r.redirect, "/tv/3");
    Channel *ch = tv_channel_by_number(3);
    CHECK(ch != NULL);
    CHECK_STR(ch->name, "Sitcoms");
    CHECK(!ch->all_series && !ch->movies);
    CHECK_INT(strlen(ch->shows), 33); /* two ids, the duplicate dropped */
    const TvPlan *p = tv_channel_plan(ch);
    CHECK_INT(p->n, 4);
    CHECK_INT(p->skipped, 1);
    CHECK(p->items[0].file == file_id("Series/Alpha/Season 1/Alpha.S01E01.Pilot.mp4"));
    CHECK(p->items[1].file == file_id("Series/Beta/Season 1/Beta.S01E01.mp4"));
    CHECK(p->items[2].file == file_id("Series/Alpha/Season 1/Alpha.S01E02.mp4"));
    CHECK(p->items[3].file == file_id("Series/Beta/Season 1/Beta.S01E02.mp4"));
    CHECK_INT(p->items[0].dur_s, 1320);
    CHECK_INT(p->cycle, 1320 + 2700 + 1320 + 2640);
    TvSlot s;
    CHECK(tv_now(p, ch->epoch, g_fake_now + 60, &s)); /* it started airing when it was created */
    CHECK(s.index == 0 && s.offset == 60);
    th_halt();
}

TEST(tv_everything_channel_with_movies_shuffles_deterministically) {
    library();
    CHECK(th_act(adam, "tv.save", "number=1&all=1&movies=1&order=shuffle&align=half").ok);
    Channel *ch = tv_channel_by_number(1);
    CHECK_STR(ch->name, "Channel 1");
    const TvPlan *p = tv_channel_plan(ch);
    CHECK_INT(p->n, 5);
    CHECK(p->half_hour);
    CHECK_INT(p->cycle, 1800 + 1800 + 3600 + 3600 + 5400); /* slots rounded up to half hours */
    uint64_t first[5];
    for (int i = 0; i < 5; i++) first[i] = p->items[i].file;
    tv_plans_reset(); /* rebuilt from scratch (a restart): same order */
    p = tv_channel_plan(tv_channel_by_number(1));
    for (int i = 0; i < 5; i++) CHECK(p->items[i].file == first[i]);
    th_halt();
}

TEST(tv_channel_validation_edit_and_delete) {
    library();
    CHECK(!th_act(adam, "tv.save", "number=0&all=1").ok);
    CHECK(!th_act(adam, "tv.save", "number=100&all=1").ok);
    ThResult r = th_act(adam, "tv.save", "number=5");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "at least one show");
    CHECK(!th_act(adam, "tv.save", "number=9&show=zz&show=123").ok); /* garbage ids don't count */
    CHECK(th_act(adam, "tv.save", "number=5&all=1").ok);
    r = th_act(adam, "tv.save", "number=5&movies=1");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "already");
    Channel *ch = tv_channel_by_number(5);
    int id = ch->id;
    int64_t epoch = ch->epoch;
    g_fake_now += 3600;
    char f[200];
    snprintf(f, sizeof f, "id=%d&number=7&name=Late+night&movies=1&align=none", id);
    CHECK(th_act(adam, "tv.save", f).ok);
    ch = tv_channel_by_number(7);
    CHECK(ch && ch->id == id);
    CHECK(ch->epoch == epoch); /* editing doesn't restart the loop */
    CHECK(!ch->all_series && ch->movies);
    CHECK_INT(ch->align, TV_ALIGN_NONE);
    CHECK_STR(ch->name, "Late night");
    CHECK(tv_channel_by_number(5) == NULL);
    snprintf(f, sizeof f, "id=%d", id);
    CHECK(th_act(adam, "tv.delete", f).ok);
    CHECK_INT(tbl_count(&T_tv_channel), 0);
    CHECK(!th_act(adam, "tv.delete", f).ok);
    /* one click: the next free number, everything, on the half hour */
    CHECK(th_act(adam, "tv.save", "number=1&all=1").ok);
    r = th_act(adam, "tv.quick", "");
    CHECK(r.ok);
    CHECK_STR(r.redirect, "/tv/2");
    ch = tv_channel_by_number(2);
    CHECK(ch && ch->all_series && ch->movies && ch->align == TV_ALIGN_HALF_HOUR);
    CHECK_STR(ch->name, "Everything");
    CHECK(tbl_count(&T_activity) >= 2);
    th_halt();
}

TEST(tv_only_members_change_channels) {
    library();
    CHECK(!th_act(0, "tv.save", "number=1&all=1").ok);
    int tablet = th_person("Hallway");
    Person t = *person_get(tablet);
    t.role = ROLE_DISPLAY;
    tbl_put(&T_person, &t);
    ThResult r = th_act(tablet, "tv.quick", "");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "view-only");
    CHECK_INT(tbl_count(&T_tv_channel), 0);
    CHECK(th_act(adam, "tv.quick", "").ok);
    const char *h = th_page(tablet, "/tv/1"); /* ... but the tablet can watch */
    CHECK_HAS(h, "<video");
    CHECK(!strstr(h, "/tv?edit="));
    CHECK(!strstr(th_page(tablet, "/tv"), "New channel"));
    th_halt();
}

TEST(tv_pages_without_library_or_channels) {
    th_boot();
    adam = th_person("Adam");
    const char *h = th_page(adam, "/tv");
    CHECK_HAS(h, "Nothing to broadcast yet");
    CHECK_HAS(h, "/media/settings");
    th_halt();
    library();
    h = th_page(adam, "/tv");
    CHECK_HAS(h, "Start your first channel");
    CHECK_HAS(h, "data-a=\"tv.quick\"");
    h = th_page(adam, "/tv?new=1");
    CHECK_HAS(h, "name=\"show\"");
    CHECK_HAS(h, "Every series");
    CHECK_HAS(th_page(adam, "/tv/42"), "No such channel");
    th_halt();
}

TEST(tv_channel_page_tunes_into_the_live_moment) {
    library();
    CHECK(th_act(adam, "tv.save", "number=1&name=Main&all=1&order=rotate&align=none").ok);
    g_fake_now += 125; /* 2 min 5 s into Alpha S01E01 */
    const char *h = th_page(adam, "/tv/1");
    char want[200];
    snprintf(want, sizeof want, "src=\"/media/file/%016llx\"",
             (unsigned long long)file_id("Series/Alpha/Season 1/Alpha.S01E01.Pilot.mp4"));
    CHECK_HAS(h, want);
    CHECK_HAS(h, "data-resume=\"125\"");
    CHECK_HAS(h, "data-next=\"/tv/1\"");
    CHECK_HAS(h, "data-static");
    CHECK_HAS(h, "autoplay");
    /* live (owner): no controls to pause or wind it - data-onair keeps it at "now"; only the sound */
    CHECK_HAS(h, "data-onair");
    CHECK(!strstr(h, " controls"));
    CHECK_HAS(h, "data-mute data-static aria-pressed=\"false\"><span class=\"tv-mute-on\">Sound on</span>");
    CHECK_HAS(h, "CH 01");
    CHECK_HAS(h, "data-key=\"ArrowUp\"");
    CHECK_HAS(h, "Coming up");
    CHECK_HAS(h, "Beta"); /* next in the rotation */
    CHECK_HAS(h, "1 video left out");
    CHECK_HAS(h, "S01E01 · Pilot");
    th_halt();
}

TEST(tv_half_hour_channel_shows_an_intermission) {
    library();
    CHECK(th_act(adam, "tv.save", "number=2&name=Prime&all=1&align=half").ok);
    Channel *ch = tv_channel_by_number(2);
    int64_t e = tv_epoch(tv_channel_plan(ch), ch->epoch);
    g_fake_now = e + 1320 + 60; /* Alpha's 22 minutes are over; Beta starts on the half hour */
    const char *h = th_page(adam, "/tv/2");
    CHECK_HAS(h, "tv-bars");
    CHECK_HAS(h, "Up next at");
    CHECK_HAS(h, "Beta");
    CHECK(!strstr(h, "<video"));
    CHECK_HAS(th_page(adam, "/tv"), "Intermission");
    th_halt();
}

TEST(tv_guide_lists_channels_and_programmes) {
    library();
    CHECK(th_act(adam, "tv.save", "number=1&name=Main&all=1&align=half").ok);
    CHECK(th_act(adam, "tv.save", "number=4&name=Films&movies=1&align=half").ok);
    const char *h = th_page(adam, "/tv");
    CHECK_HAS(h, "TV guide");
    CHECK_HAS(h, "tv-g-p now");
    CHECK_HAS(h, "Gamma");
    CHECK_HAS(h, "Now: <b>Alpha</b>");
    CHECK_HAS(h, "data-key=\"1\"");
    CHECK_HAS(h, "data-key=\"4\"");
    CHECK_HAS(h, "href=\"/tv?edit=");
    th_halt();
}

TEST(tv_widgets_render_without_a_person) {
    library();
    const Module *m = module_find("tv");
    CHECK(m && m->widget);
    Ctx c;
    Buf b = {0};
    app_ctx(&c, 0, "/wall");
    m->widget(&c, &b, WIDGET_WALL);
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_INT(b.len, 0); /* no channels: nothing, and no hint for strangers */
    app_ctx_free(&c);
    app_ctx(&c, adam, "/");
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "start one");
    app_ctx_free(&c);
    CHECK(th_act(adam, "tv.quick", "").ok);
    buf_reset(&b);
    app_ctx(&c, 0, "/wall");
    m->widget(&c, &b, WIDGET_WALL);
    CHECK_HAS(buf_cstr(&b), "On TV now");
    CHECK_HAS(buf_cstr(&b), "href=\"/tv/1\"");
    app_ctx_free(&c);
    CHECK_HAS(th_page(adam, "/"), "On TV now");
    buf_free(&b);
    th_halt();
}

TEST(tv_plans_follow_library_changes) {
    library();
    CHECK(th_act(adam, "tv.save", "number=1&all=1&align=none").ok);
    CHECK_INT(tv_channel_plan(tv_channel_by_number(1))->n, 4);
    put_mp4(g_root, "Series/Alpha/Season 1/Alpha.S01E03.mp4", 1320000); /* the unreadable one is fixed */
    media_scan_sync();
    const TvPlan *p = tv_channel_plan(tv_channel_by_number(1));
    CHECK_INT(p->n, 5);
    CHECK_INT(p->skipped, 0);
    const Module *m = module_find("tv"); /* the tick is cheap and safe to call */
    m->tick(1000000);
    m->tick(1000500);
    m->tick(1002000);
    th_halt();
}

TEST(tv_plan_cache_holds_every_channel) {
    library();
    for (int n = 1; n <= 40; n++) { /* more channels than the old 32-slot cache */
        Channel ch = {.number = n, .all_series = true, .order = TV_ROTATE, .epoch = g_fake_now};
        snprintf(ch.name, sizeof ch.name, "Channel %d", n);
        tbl_put(&T_tv_channel, &ch);
    }
    const TvPlan *first[41];
    for (int n = 1; n <= 40; n++) first[n] = tv_channel_plan(tv_channel_by_number(n));
    for (int n = 1; n <= 40; n++) CHECK(tv_channel_plan(tv_channel_by_number(n)) == first[n]); /* no rebuilds */
    CHECK_INT(first[40]->n, 4);
    th_halt();
}

/* ===================================================== Live TV under Watch */

TEST(tv_lives_under_watch) {
    library();
    const Module *m = module_find("tv");
    CHECK(m && m->nav == 0 && str_eq(m->parent, "media"));
    CHECK(th_act(adam, "tv.quick", "").ok);
    static const char *const pages[] = {"/tv", "/tv/1", "/tv?new=1", "/tv/42"};
    for (int i = 0; i < countof(pages); i++) {
        const char *h = th_page(adam, pages[i]);
        CHECK_HAS(h, "<a href=\"/media\" class=\"on\" aria-current=\"page\">"); /* Watch lights up */
        CHECK(!strstr(h, "<span>TV</span>") && !strstr(h, "<span>Live TV</span>")); /* not in the navigation */
        CHECK_HAS(h, "<a href=\"/media\">Home</a>");                                 /* Watch's tabs ... */
        CHECK_HAS(h, "<a href=\"/tv\" class=\"on\" aria-current=\"page\">Live TV</a>"); /* ... Live TV on */
        CHECK(!strstr(h, "data-live=\"media.search\"")); /* the search box opens Watch's results */
    }
    CHECK_HAS(th_page(adam, "/tv?new=1"), "name=\"show\"");
    CHECK_HAS(th_page(adam, "/tv"), "New channel");
    const char *h = th_page(adam, "/media");
    CHECK_HAS(h, "<a href=\"/tv\">Live TV</a>");
    CHECK_HAS(h, "<a href=\"/media\" class=\"on\" aria-current=\"page\">Home</a>");
    th_halt();
}

TEST(tv_on_now_row_on_watch_home_and_screens) {
    library();
    const Block *bl = block_find("tv.onnow", NULL);
    CHECK(bl && (bl->kinds & SCREEN_TV) && (bl->kinds & SCREEN_WALL) && !bl->choices);
    Ctx c;
    Buf b = {0};
    app_ctx(&c, adam, "/screens/2");
    CHECK(!bl->render(&c, &b, SCREEN_TV, "")); /* no channels: no row */
    app_ctx_free(&c);
    CHECK(!strstr(th_page(adam, "/media"), "On now"));

    CHECK(th_act(adam, "tv.save", "number=1&name=Main&all=1&order=rotate&align=none").ok);
    g_fake_now += 125; /* 2 min into Alpha S01E01 */
    const char *h = th_page(adam, "/media");
    CHECK_HAS(h, "<h2>On now</h2><a href=\"/tv\">Guide</a></header><div class=\"tiles\"><a class=\"tile wide\" "
                 "href=\"/tv/1\">"); /* Watch's own home: the ordinary channel page */
    CHECK_HAS(h, "<b class=\"tile-t\">Alpha</b><span class=\"tile-s\">Main · S01E01 · Pilot · 20 min left</span>");
    CHECK_HAS(h, "<span class=\"tile-bar\"><i style=\"width:9%\"></i></span>");
    CHECK(strstr(h, "On now") < strstr(h, "Recently added"));
    app_ctx(&c, adam, "/screens/2");
    CHECK(bl->render(&c, &b, SCREEN_TV, ""));
    CHECK_HAS(buf_cstr(&b), "href=\"/tv/1?tv=1&amp;back=%2Fscreens%2F2\""); /* a screen: full screen, and back */
    CHECK_HAS(buf_cstr(&b), "<span class=\"tv-num tv-tile-n\" style=\"--h:150\">01</span>");
    app_ctx_free(&c);

    /* a channel between programmes says what's next */
    CHECK(th_act(adam, "tv.save", "number=2&name=Prime&all=1&align=half").ok);
    Channel *ch = tv_channel_by_number(2);
    g_fake_now = tv_epoch(tv_channel_plan(ch), ch->epoch) + 1320 + 60;
    buf_reset(&b);
    app_ctx(&c, adam, "/screens/2");
    CHECK(bl->render(&c, &b, SCREEN_TV, ""));
    CHECK_HAS(buf_cstr(&b), "<b class=\"tile-t\">Beta</b><span class=\"tile-s\">Prime · Up next at ");
    app_ctx_free(&c);
    buf_free(&b);
    th_halt();
}

TEST(tv_channel_full_screen_for_the_tv) {
    library();
    CHECK(th_act(adam, "tv.save", "number=1&name=Main&all=1&order=rotate&align=none").ok);
    CHECK(th_act(adam, "tv.save", "number=2&name=Films&movies=1&align=none").ok);
    g_fake_now += 125;
    const char *h = th_page(adam, "/tv/1?tv=1&back=%2Fscreens%2F2");
    CHECK(str_starts(h, "<div id=\"app\" class=\"bare\" data-mod=\"tv\"><div class=\"media-tv tv-full\">"
                        "<a class=\"media-tv-back\" href=\"/screens/2\" data-esc>")); /* Back comes first */
    CHECK_HAS(h, "data-resume=\"125\"");
    CHECK_HAS(h, "data-next=\"/tv/1?tv=1&amp;back=%2Fscreens%2F2\"");
    CHECK_HAS(h, "autoplay");
    CHECK_HAS(h, "aria-pressed=\"false\" autofocus>"); /* OK on the remote: the sound (live: nothing to pause) */
    CHECK(!strstr(h, " controls"));
    CHECK_HAS(h, "href=\"/tv/2?tv=1&amp;back=%2Fscreens%2F2\" data-key=\"ArrowUp\""); /* the remote's channel keys */
    CHECK_HAS(h, "href=\"/tv/2?tv=1&amp;back=%2Fscreens%2F2\" data-key=\"ArrowDown\"");
    CHECK_HAS(h, "data-key=\"2\" title=\"Films\"");
    CHECK_HAS(h, "S01E01 · Pilot · 20 min left");
    CHECK(!strstr(h, "class=\"side\"") && !strstr(h, "tv-info") && !strstr(h, "/tv?edit="));
    CHECK_HAS(th_page(adam, "/tv/1?tv=1&back=%2F%2Fevil.example"), "<a class=\"media-tv-back\" href=\"/\" data-esc>");
    CHECK_HAS(th_page(adam, "/tv/1?tv=1&back=%2F%5Cevil"), "<a class=\"media-tv-back\" href=\"/\" data-esc>");
    /* without tv=1: the TV set under Watch's tabs, as before */
    h = th_page(adam, "/tv/1");
    CHECK(str_starts(h, "<div id=\"app\" class=\"shell\""));
    CHECK_HAS(h, "tv-set");
    CHECK_HAS(h, "data-next=\"/tv/1\"");
    CHECK(!strstr(h, "autofocus"));
    h = th_page(adam, "/tv/42?tv=1&back=%2Fscreens%2F2");
    CHECK(str_starts(h, "<div id=\"app\" class=\"bare\""));
    CHECK_HAS(h, "No such channel");
    CHECK_HAS(h, "href=\"/screens/2\"");
    th_halt();
}
