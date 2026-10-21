/* Live TV in Swedish and Spanish (issue #46): the channel list and guide, the channel dialog, a channel's
 * page and its TV mode (on the air, between programmes, no signal), the "On TV" row on a TV screen and a
 * wall, the home card - with nothing left in English but names. A small fake library of synthetic MP4s,
 * as in tv_test.c. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../media/media.h"
#include "../screens/screens.h"
#include "tv.h"

/* What other parts of Hemma show around these pages - the shell, a dialog's Close, the colour picker, the
   player, Watch's tabs and search, other modules' names and rows - is theirs to translate: allowed here
   only while this build has no translation for that part (the gate: a word of its own), so this test
   reads this part's words now, and every word once all parts are translated. */
static bool untranslated(const char *en) { return !strcmp(T_in(LANG_SV, en), en); }

static int allow_others(const char **out, int n, int max) {
    static const struct {
        const char *gate, *text;
    } fixed[] = {
        {"Skip to content", "Skip to content"}, {"Skip to content", "More"}, {"Skip to content", "Here now"},
        {"Skip to content", "Close"},
        {"Amber", "Colour"}, {"Amber", "Red"}, {"Amber", "Orange"}, {"Amber", "Amber"}, {"Amber", "Lime"},
        {"Amber", "Green"}, {"Amber", "Teal"}, {"Amber", "Sky"}, {"Amber", "Blue"}, {"Amber", "Indigo"},
        {"Amber", "Purple"}, {"Amber", "Magenta"}, {"Amber", "Pink"},
        {"Volume", "Volume"}, {"Volume", "Live"}, {"Volume", "Sound off"}, {"Volume", "Sound on"},
        {"Volume", "Full screen"}, {"Volume", "Full screen (F)"}, {"Volume", "Mute (M)"}, {"Volume", "Sound (M)"},
        {"Volume", "Play"}, {"Volume", "Play (Space)"}, {"Volume", "Pause (Space)"},
        {"Volume", "Back 10 seconds (Left)"}, {"Volume", "Forward 10 seconds (Right)"}, {"Volume", "Subtitles"},
        {"Library settings", "Library settings"}, {"Library settings", "Search the library"},
        {"Library settings", "Search movies and series…"}, {"Library settings", "Movies"},
        {"Library settings", "Series"}, {"Library settings", "Folders"}, {"Library settings", "Live TV"},
        {"Library settings", "Back"}, /* (TV mode's Back: media_tv_open) */
    };
    for (int i = 0; i < countof(fixed) && n < max; i++)
        if (untranslated(fixed[i].gate)) out[n++] = fixed[i].text;
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (str_eq(m->id, "screens") || str_eq(m->id, "tv") || str_eq(m->id, "net")) continue;
        if (n < max && untranslated(m->title)) out[n++] = m->title;
        for (const Block *bl = m->blocks; bl && bl->id && n < max; bl++)
            if (untranslated(bl->title)) out[n++] = bl->title;
    }
    return n;
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

static int with_role(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    if (role == ROLE_DISPLAY) th_act(id, "screens.viewer", "who=0"); /* (the harness keeps a pick across tests) */
    return id;
}

/* An action's toast (in the actor's language) */
static const char *toast(int person, const char *action, const char *form) {
    static ThResult r;
    r = th_act(person, action, form);
    return r.toast;
}

static const char *series_hex(const char *title, char hex[17]) {
    const MediaIndex *ix = media_lib();
    hex[0] = '\0';
    for (int i = 0; i < ix->nseries; i++)
        if (str_eq(ix->series[i].title, title)) media_id_hex(ix->series[i].id, hex);
    return hex;
}

/* The home card, rendered in a language */
static const char *card(int person, const char *lang, Buf *b) {
    pref_set(person, PREF_LANG, lang);
    Ctx c;
    app_ctx(&c, person, "/");
    buf_reset(b);
    module_find("tv")->widget(&c, b, WIDGET_HOME);
    app_ctx_free(&c);
    pref_set(person, PREF_LANG, "");
    return buf_cstr(b);
}

TEST(tv_speaks_swedish_and_spanish) {
    th_boot();
    const char *allow[160] = {"Anna", "Jonas", "Living room TV", "Hall tablet", "Main", "Prime", "Films", "Quiet",
                              "Alpha", "Beta", "Gamma", "Delta", "Pilot", "Kids", "Hall", "CH", "A", "J", "HT",
                              "LR"}; /* (people's, channels', shows' and screens' names; avatars' letters; a TV's
                                       on-screen "CH 03" stays as old TVs said it) */
    int na = 0;
    while (allow[na]) na++;
    allow_others(allow, na, countof(allow) - 1);
    Buf r = {0};
    int bad = 0;

    /* no library yet: an admin is told to set one up, a member whom to ask */
    int anna = with_role("Anna", ROLE_ADMIN), jonas = th_person("Jonas");
    bad += th_untranslated(anna, "/tv", allow, &r);
    bad += th_untranslated(jonas, "/tv", allow, &r);

    /* Alpha: 2 × 22 min + one unreadable episode; Beta: 45 + 44 min; Gamma: a 90 min movie; Delta: nothing
       that airs */
    g_fake_now = dt_make(2026, 10, 10, 20, 0);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_mp4(root, "Series/Alpha/Season 1/Alpha.S01E01.Pilot.mp4", 1320000);
    put_mp4(root, "Series/Alpha/Season 1/Alpha.S01E02.mp4", 1320000);
    put_junk(root, "Series/Alpha/Season 1/Alpha.S01E03.mp4");
    put_mp4(root, "Series/Beta/Season 1/Beta.S01E01.mp4", 2700000);
    put_mp4(root, "Series/Beta/Season 1/Beta.S01E02.mp4", 2640000);
    put_junk(root, "Series/Delta/Season 1/Delta.S01E01.mp4");
    put_mp4(root, "Movies/Gamma (2001)/Gamma.2001.mp4", 5400000);
    setting_set("media.roots", root);
    media_scan_sync();
    int tv = with_role("Living room TV", ROLE_DISPLAY), tablet = with_role("Hall tablet", ROLE_DISPLAY);

    /* library but no channels: start the first one */
    bad += th_untranslated(anna, "/tv", allow, &r);
    Buf b = {0};
    CHECK_HAS(card(jonas, "sv", &b), "Gör dina serier till kanaler som aldrig slutar");

    /* channels: one on the air, one between programmes, films, one with nothing that airs */
    char form[200], hex[17];
    CHECK(th_act(anna, "tv.save", "number=1&name=Main&all=1&order=rotate&align=none").ok);
    CHECK(th_act(anna, "tv.save", "number=2&name=Prime&all=1&align=half").ok);
    CHECK(th_act(anna, "tv.save", "number=3&name=Films&movies=1&align=none").ok);
    snprintf(form, sizeof form, "number=4&name=Quiet&show=%s&align=none", series_hex("Delta", hex));
    CHECK(th_act(anna, "tv.save", form).ok);
    g_fake_now += 22 * 60 + 60; /* Main: Alpha's second episode; Prime: between programmes (Beta at 20:30) */
    CHECK(tv_channel_by_number(2) != NULL);

    /* a TV screen and a wall with the "On TV" row */
    ThResult res = th_act(anna, "screens.create", "name=Kids&kind=tv");
    int kids = (int)strtol(res.redirect + 9, NULL, 10);
    res = th_act(anna, "screens.create", "name=Hall&kind=wall");
    int hall = (int)strtol(res.redirect + 9, NULL, 10);
    snprintf(form, sizeof form, "id=%d&block=tv.onnow", kids);
    CHECK(th_act(anna, "screens.row_add", form).ok);
    snprintf(form, sizeof form, "id=%d&block=tv.onnow", hall);
    CHECK(th_act(anna, "screens.row_add", form).ok);
    snprintf(form, sizeof form, "id=%d&acc=%d", kids, tv);
    CHECK(th_act(anna, "screens.shows", form).ok);
    snprintf(form, sizeof form, "who=%d", jonas);
    CHECK(th_act(tv, "screens.viewer", form).ok);

    char u[8][64];
    snprintf(u[0], sizeof u[0], "/tv?edit=%d", tv_channel_by_number(4)->id);
    snprintf(u[1], sizeof u[1], "/screens/%d", kids);
    snprintf(u[2], sizeof u[2], "/screens/%d", hall);
    const char *urls[] = {"/tv",          "/tv?new=1", u[0],   "/tv/1", "/tv/2", "/tv/3", "/tv/4", "/tv/42",
                          "/tv/1?tv=1&back=%2Fscreens%2F1", "/tv/2?tv=1&back=%2Fscreens%2F1",
                          "/tv/4?tv=1&back=%2Fscreens%2F1", "/tv/42?tv=1&back=%2F", u[1], u[2]};
    for (int i = 0; i < countof(urls); i++) bad += th_untranslated(anna, urls[i], allow, &r);
    bad += th_untranslated(jonas, "/tv", allow, &r);
    bad += th_untranslated(jonas, "/tv/2", allow, &r);
    bad += th_untranslated(tv, u[1], allow, &r);
    bad += th_untranslated(tv, "/tv/1?tv=1&back=%2F", allow, &r);
    bad += th_untranslated(tablet, u[2], allow, &r);
    /* a little later: under a minute left of Main's programme */
    g_fake_now = tv_channel_by_number(1)->epoch + 2 * 1320 - 30;
    bad += th_untranslated(anna, "/tv/1", allow, &r);
    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);

    /* the home card */
    g_fake_now = tv_channel_by_number(1)->epoch + 22 * 60 + 60;
    const char *h = card(jonas, "sv", &b);
    CHECK_HAS(h, "<h2>På TV nu</h2>");
    CHECK_HAS(h, ">Tablå</a>");
    CHECK_HAS(h, "<span class=\"muted\">Härnäst: </span><b>Beta</b> <small>kl. 20:30</small>");
    CHECK_HAS(h, " · till ");
    CHECK_HAS(h, "Ingen signal");
    h = card(jonas, "es", &b);
    CHECK_HAS(h, "<h2>Ahora en la tele</h2>");
    CHECK_HAS(h, "<small>a las 20:30</small>");
    buf_free(&b);

    /* toasts in the actor's language; the Latest line in each reader's */
    pref_set(anna, PREF_LANG, "sv");
    CHECK_STR(toast(anna, "tv.save", "number=0&all=1"), "Kanalnummer går från 1 till 99");
    CHECK_STR(toast(anna, "tv.save", "number=5&name=Late&all=1&align=none"), "Kanal 5 sänder");
    pref_set(anna, PREF_LANG, "");
    const Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "Anna started channel 5 · Late");
    Buf line = {0};
    Lang was = i18n_use(LANG_ES);
    activity_text(a, &line);
    i18n_use(was);
    CHECK_STR(buf_cstr(&line), "Anna creó el canal 5 · Late");
    buf_free(&line);
    g_fake_now = 0;
    th_halt();
}
