/* OFFLINE IN THE APP (2.3): the manifests the Android app saves things by (a film, an episode, a book, a season's
 * list), who may (members, admins, children - a child only what's open to them; never guests or screens), the
 * buttons only inside the app, and the place the app reports back landing in media_watch. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../av/audiotags_testmk.h"
#include "../av/mp4.h"
#include "library.h"
#include "media.h"

static char g_root[512];

static void put(const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

static bool off_put(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)ctx) == n; }
static bool off_put_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    fflush(f);
    bool ok = file_seek(f, off) && fwrite(p, 1, n, f) == n;
    fflush(f);
    return fseek(f, 0, SEEK_END) == 0 && ok;
}

/* A tiny MP4 the probe reads: a picture lasting `seconds`, then a sound track per language (ISO 639-2) */
static void put_mp4(const char *rel, int seconds, const char *const *langs, int n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    FILE *f = file_open(path, "wb");
    Mp4Out out = {f, off_put, off_put_at};
    Mp4 *m = mp4_new(&out, 0, 8192);
    static const uint8_t avcc[] = {1, 0x42, 0, 0x1E, 0xFF, 0xE0, 0}, asc[] = {0x11, 0x90};
    Mp4Track v = {.codec = MP4_H264, .timescale = 1000, .config = avcc, .config_len = sizeof avcc, .width = 64, .height = 48};
    mp4_add_track(m, &v);
    for (int i = 0; i < n; i++) {
        Mp4Track a = {.codec = MP4_AAC, .timescale = 48000, .config = asc, .config_len = sizeof asc, .channels = 2,
                      .sample_rate = 48000};
        snprintf(a.lang, sizeof a.lang, "%s", langs[i]);
        mp4_add_track(m, &a);
    }
    mp4_begin(m);
    static const uint8_t frame[16] = {0, 0, 0, 12}, block[8] = {0x21, 0x10};
    mp4_sample(m, 0, frame, sizeof frame, (uint32_t)seconds * 1000, 0, true);
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < n; i++) mp4_sample(m, 1 + i, block, sizeof block, 1024, 0, true);
    mp4_finish(m);
    mp4_free(m);
    fclose(f);
}

static const AtTestChapter STORY[] = {{0, "Opening Credits"}, {65000, "Chapter 1 - The Boy"}, {300000, "Chapter 2 - The Glass"}};

static int person_as(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* Adam (an admin) and a library: a film with two sounds and two subtitles, one "Not for children" later, a series
   of two seasons, an audiobook with chapters */
static int boot_with_offline(void) {
    th_boot();
    int adam = person_as("Adam", ROLE_ADMIN);
    str_copy(g_root, sizeof g_root, test_tmpdir());
    static const char *const two[] = {"eng", "swe"}, *const one[] = {"eng"};
    put_mp4("Movies/Two Voices (2024)/Two Voices (2024).mp4", 600, two, 2);
    static const char en[] = "1\n00:00:01,000 --> 00:00:02,000\nHi\n", sv[] = "1\n00:00:01,000 --> 00:00:02,000\nHej\n";
    put("Movies/Two Voices (2024)/Two Voices (2024).en.srt", en, sizeof en - 1);
    put("Movies/Two Voices (2024)/Two Voices (2024).sv.srt", sv, sizeof sv - 1);
    put_mp4("Movies/Grown Up (2020)/Grown Up (2020).mp4", 60, one, 1);
    put_mp4("Series/Garden Show/Season 1/Garden.Show.S01E02.Roots.mp4", 60, one, 1);
    put_mp4("Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.mp4", 60, one, 1);
    put_mp4("Series/Garden Show/Season 2/Garden.Show.S02E01.Weeds.mp4", 60, one, 1);
    Buf b = {0};
    at_test_m4b(&b, &(AtTestM4b){.title = "The First Story (Unabridged)", .artist = "Ann Author", .narrator = "Nora Narrator",
                                 .chapters = STORY, .nchapters = 3, .duration_ms = 600000});
    put("Stories/1 Story.m4b", b.p, b.len);
    buf_free(&b);
    setting_set("media.roots", g_root);
    media_scan_sync();
    return adam;
}

static const MediaFile *find(const char *rel_suffix) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_ends(ix->files[i].rel, rel_suffix)) return &ix->files[i];
    return NULL;
}

static uint64_t book_id(void) {
    const MediaIndex *ix = media_lib();
    return ix->nbooks ? ix->books[0].id : 0;
}

/* A GET of path as person: the whole answer (the manifests answer at once, on the loop) */
static const char *get(int person, const char *method, const char *path, Buf *keep) {
    char raw[400];
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: t\r\n\r\n", method, path);
    HttpReq r;
    if (http_parse(raw, strlen(raw), 1 << 20, &r) != 1) return "unparsable";
    Conn conn;
    memset(&conn, 0, sizeof conn);
    conn.fd = SOCK_BAD;
    conn.keep_alive = true;
    Ctx c;
    app_ctx(&c, person, r.target);
    bool handled = media_http(&c, &conn, &r);
    app_ctx_free(&c);
    http_req_free(&r);
    buf_reset(keep);
    buf_put(keep, conn.out.p, conn.out.len);
    buf_free(&conn.out);
    buf_free(&conn.in);
    return handled ? buf_cstr(keep) : "not handled";
}

static const char *body(const char *answer) {
    const char *b = strstr(answer, "\r\n\r\n");
    return b ? b + 4 : "";
}

static const char *path_of(const char *fmt, uint64_t id) {
    static char p[120];
    snprintf(p, sizeof p, fmt, (unsigned long long)id);
    return p;
}

/* A page as the app shows it: a session whose browser says HemmaApp/ (as people_test.c's home_from) */
static const char *app_page(int person, const char *url) {
    static Buf page;
    char sid[33];
    if (!session_create(person, false,
                        "Mozilla/5.0 (Linux; Android 17; Pixel 10; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 "
                        "Chrome/156.0.0.0 Mobile Safari/537.36 HemmaApp/2.3.0",
                        sid))
        return "";
    const Session *made = NULL;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        const Session *s = tbl_at(&T_session, i);
        if (s->person == person) made = s;
    }
    Ctx c;
    app_ctx(&c, person, url);
    str_copy(c.sess->agent, sizeof c.sess->agent, made ? made->agent : "");
    buf_reset(&page);
    app_render(&c, &page);
    app_ctx_free(&c);
    return buf_cstr(&page);
}

TEST(offline_manifests_say_what_to_save) {
    boot_with_offline();
    int eva = th_person("Eva");
    const MediaFile *film = find("Two Voices (2024).mp4"), *ep = find("S01E02.Roots.mp4");
    CHECK(film && ep && book_id());
    uint64_t fid = film->id, eid = ep->id, bid = book_id();
    int64_t fsize = film->size;
    Buf keep = {0};
    char want[300];

    /* a film: one file, the viewer's sound (the file's default until they pick), its text subtitles */
    const char *a = get(eva, "GET", path_of("/media/offline/%016llx.json", fid), &keep);
    CHECK(str_starts(a, "HTTP/1.1 200"));
    CHECK_HAS(a, "Content-Type: application/json; charset=utf-8");
    CHECK_HAS(a, "Cache-Control: no-store");
    snprintf(want, sizeof want,
             "{\"v\":1,\"kind\":\"video\",\"id\":\"%016llx\",\"title\":\"Two Voices (2024)\",\"series\":\"\",\"code\":\"\","
             "\"poster\":\"\",\"duration_ms\":600000,\"resume_s\":0,\"files\":[{\"url\":\"/media/file/%016llx?a=0\","
             "\"bytes\":%lld}],\"subs\":[",
             (unsigned long long)fid, (unsigned long long)fid, (long long)fsize);
    CHECK(str_starts(body(a), want));
    CHECK_HAS(body(a), "\"lang\":\"en\",\"label\":\"English\",\"on\":false}");
    CHECK_HAS(body(a), "\"lang\":\"sv\",\"label\":\"Svenska\",\"on\":false}]");
    CHECK_HAS(body(a), "],\"chapters\":[],\"author\":\"\",\"narrator\":\"\"}");
    /* Swedish sound and subtitles, and a place to start from: the manifest says so */
    CHECK(th_act(eva, "media.soundlang", "lang=sv").ok);
    CHECK(th_act(eva, "media.prefs", "sublang=sv").ok);
    char form[120];
    snprintf(form, sizeof form, "id=%016llx&pos=120&dur=600", (unsigned long long)fid); /* (what the app sends back) */
    CHECK(th_act(eva, "media.progress", form).ok);
    const MediaWatch *w = media_watch_get(eva, fid);
    CHECK(w && w->pos == 120 && !w->done);
    a = get(eva, "GET", path_of("/media/offline/%016llx.json", fid), &keep);
    CHECK_HAS(body(a), "\"resume_s\":120,");
    snprintf(want, sizeof want, "\"url\":\"/media/file/%016llx?a=1\"", (unsigned long long)fid);
    CHECK_HAS(body(a), want);
    CHECK_HAS(body(a), "\"lang\":\"sv\",\"label\":\"Svenska\",\"on\":true}");

    /* an episode: its own title, its show and its number */
    a = get(eva, "GET", path_of("/media/offline/%016llx.json", eid), &keep);
    CHECK(str_starts(a, "HTTP/1.1 200"));
    CHECK_HAS(body(a), "\"title\":\"Roots\",\"series\":\"Garden Show\",\"code\":\"S01E02\"");
    snprintf(want, sizeof want, "\"files\":[{\"url\":\"/media/file/%016llx\",", (unsigned long long)eid); /* (one sound) */
    CHECK_HAS(body(a), want);
    CHECK_HAS(body(a), "\"subs\":[]");

    /* a book: its parts, chapters in the whole book's seconds, author and narrator, the place in the book */
    snprintf(form, sizeof form, "id=%016llx&pos=400&dur=600", (unsigned long long)bid);
    CHECK(th_act(eva, "media.progress", form).ok);
    w = media_watch_get(eva, bid);
    CHECK(w && w->pos == 400);
    a = get(eva, "GET", path_of("/media/offline/%016llx.json", bid), &keep);
    CHECK(str_starts(a, "HTTP/1.1 200"));
    snprintf(want, sizeof want, "{\"v\":1,\"kind\":\"book\",\"id\":\"%016llx\",\"title\":\"The First Story\"", (unsigned long long)bid);
    CHECK(str_starts(body(a), want));
    CHECK_HAS(body(a), "\"duration_ms\":600000,\"resume_s\":400,\"files\":[{\"url\":\"/media/file/");
    CHECK_HAS(body(a), "\"subs\":[],\"chapters\":[{\"t\":0,\"title\":\"Opening Credits\"},{\"t\":65,\"title\":\"Chapter 1 - "
                       "The Boy\"},{\"t\":300,\"title\":\"Chapter 2 - The Glass\"}],\"author\":\"Ann Author\","
                       "\"narrator\":\"Nora Narrator\"}");
    buf_free(&keep);
    th_halt();
}

/* 2.7.1: where the viewer is in what the app saved, and when they got there - the app's copies take a newer place */
TEST(offline_places_say_where_and_when) {
    int adam = boot_with_offline();
    int eva = th_person("Eva"), gus = person_as("Gus", ROLE_GUEST);
    const MediaFile *film = find("Two Voices (2024).mp4"), *e1 = find("S01E01.Seeds.mp4"), *e2 = find("S01E02.Roots.mp4");
    CHECK(film && e1 && e2 && book_id());
    uint64_t fid = film->id, e1id = e1->id, e2id = e2->id, bid = book_id();
    Buf keep = {0};
    char form[120], path[200], want[200];

    snprintf(form, sizeof form, "id=%016llx&pos=250&dur=600", (unsigned long long)fid); /* (as the TV reports) */
    CHECK(th_act(eva, "media.progress", form).ok);
    snprintf(form, sizeof form, "id=%016llx&pos=58&dur=60", (unsigned long long)e2id); /* (past 92%: done) */
    CHECK(th_act(eva, "media.progress", form).ok);
    snprintf(form, sizeof form, "id=%016llx&pos=30&dur=60", (unsigned long long)e1id); /* (someone else's place) */
    CHECK(th_act(adam, "media.progress", form).ok);
    snprintf(form, sizeof form, "id=%016llx&pos=400&dur=600", (unsigned long long)bid);
    CHECK(th_act(eva, "media.progress", form).ok);
    const MediaWatch *w = media_watch_get(eva, fid);
    CHECK(w && w->updated > 0);
    int64_t at = w->updated;

    /* only what has a place, in the order asked; not-ids and a place of someone else's left out */
    snprintf(path, sizeof path, "/media/offline/places.json?ids=%016llx,nothing,%016llx,%016llx,%016llx", (unsigned long long)fid,
             (unsigned long long)e1id, (unsigned long long)e2id, (unsigned long long)bid);
    const char *a = get(eva, "GET", path, &keep);
    CHECK(str_starts(a, "HTTP/1.1 200"));
    CHECK_HAS(a, "Cache-Control: no-store");
    CHECK(str_starts(body(a), "{\"v\":1,\"now\":"));
    long long now = strtoll(body(a) + strlen("{\"v\":1,\"now\":"), NULL, 10);
    CHECK(now >= at);
    snprintf(want, sizeof want, "\"places\":[{\"id\":\"%016llx\",\"pos\":250,\"dur\":600,\"done\":false,\"at\":%lld},", (unsigned long long)fid,
             (long long)at);
    CHECK_HAS(body(a), want);
    snprintf(want, sizeof want, "{\"id\":\"%016llx\",\"pos\":58,\"dur\":60,\"done\":true,", (unsigned long long)e2id);
    CHECK_HAS(body(a), want);
    snprintf(want, sizeof want, "{\"id\":\"%016llx\",\"pos\":400,\"dur\":600,\"done\":false,", (unsigned long long)bid);
    CHECK_HAS(body(a), want);
    snprintf(want, sizeof want, "%016llx", (unsigned long long)e1id);
    CHECK(!strstr(body(a), want));
    CHECK(str_ends(body(a), "}]}"));
    CHECK(str_ends(get(eva, "GET", "/media/offline/places.json", &keep), "\"places\":[]}")); /* (nothing asked) */
    CHECK(str_starts(get(gus, "GET", path, &keep), "HTTP/1.1 403"));                     /* (as every manifest) */
    buf_free(&keep);
    th_halt();
}

TEST(offline_season_lists_and_who_may) {
    int adam = boot_with_offline();
    int eva = th_person("Eva"), leo = person_as("Leo", ROLE_CHILD), gus = person_as("Gus", ROLE_GUEST),
        tv = person_as("Living room TV", ROLE_DISPLAY);
    const MediaFile *e1 = find("S01E01.Seeds.mp4"), *e2 = find("S01E02.Roots.mp4"), *e3 = find("S02E01.Weeds.mp4"),
                    *film = find("Two Voices (2024).mp4"), *grown = find("Grown Up (2020).mp4");
    CHECK(e1 && e2 && e3 && film && grown);
    uint64_t sid = e1->series, fid = film->id, gid = grown->id;
    char want[300];
    Buf keep = {0};

    /* a season: its episodes' manifests, in order */
    const char *a = get(eva, "GET", path_of("/media/offline/season/%016llx-1.json", sid), &keep);
    CHECK(str_starts(a, "HTTP/1.1 200"));
    snprintf(want, sizeof want,
             "{\"v\":1,\"kind\":\"list\",\"title\":\"Garden Show · Season 1\",\"items\":[\"/media/offline/%016llx.json\","
             "\"/media/offline/%016llx.json\"]}",
             (unsigned long long)e1->id, (unsigned long long)e2->id);
    CHECK_STR(body(a), want);
    a = get(eva, "GET", path_of("/media/offline/season/%016llx-2.json", sid), &keep);
    snprintf(want, sizeof want, "\"items\":[\"/media/offline/%016llx.json\"]", (unsigned long long)e3->id);
    CHECK_HAS(body(a), want);
    CHECK(str_starts(get(eva, "GET", path_of("/media/offline/season/%016llx-0.json", sid), &keep), "HTTP/1.1 404")); /* (no specials) */
    CHECK(str_starts(get(eva, "GET", path_of("/media/offline/season/%016llx-x.json", sid), &keep), "HTTP/1.1 404"));

    /* guests and screens: never; signed out: the usual */
    a = get(gus, "GET", path_of("/media/offline/%016llx.json", fid), &keep);
    CHECK(str_starts(a, "HTTP/1.1 403"));
    CHECK_HAS(body(a), "Saving for offline isn't open to guests or screens.");
    CHECK(str_starts(get(tv, "GET", path_of("/media/offline/%016llx.json", fid), &keep), "HTTP/1.1 403"));
    CHECK(str_starts(get(0, "GET", path_of("/media/offline/%016llx.json", fid), &keep), "HTTP/1.1 401"));
    CHECK(str_starts(get(eva, "POST", path_of("/media/offline/%016llx.json", fid), &keep), "HTTP/1.1 405"));
    CHECK(str_starts(get(eva, "GET", "/media/offline/0000000000000001.json", &keep), "HTTP/1.1 404"));
    CHECK(str_starts(get(eva, "GET", "/media/offline/nothing.json", &keep), "HTTP/1.1 404"));

    /* a child: what's open to them - not a film or a series an admin marked "Not for children" */
    CHECK(str_starts(get(leo, "GET", path_of("/media/offline/%016llx.json", gid), &keep), "HTTP/1.1 200"));
    CHECK(th_act(adam, "media.kids", "p=0:Movies/Grown%20Up%20(2020)&on=1").ok);
    CHECK(th_act(adam, "media.kids", "p=0:Series/Garden%20Show/Season%202&on=1").ok);
    CHECK(str_starts(get(leo, "GET", path_of("/media/offline/%016llx.json", gid), &keep), "HTTP/1.1 404"));
    CHECK(str_starts(get(leo, "GET", path_of("/media/offline/%016llx.json", fid), &keep), "HTTP/1.1 200"));
    CHECK(str_starts(get(leo, "GET", path_of("/media/offline/season/%016llx-1.json", sid), &keep), "HTTP/1.1 404"));
    CHECK(str_starts(get(leo, "GET", path_of("/media/offline/%016llx.json", e1->id), &keep), "HTTP/1.1 404"));
    CHECK(str_starts(get(eva, "GET", path_of("/media/offline/season/%016llx-1.json", sid), &keep), "HTTP/1.1 200"));
    CHECK(str_starts(get(eva, "GET", path_of("/media/offline/%016llx.json", gid), &keep), "HTTP/1.1 200"));
    buf_free(&keep);
    th_halt();
}

TEST(offline_buttons_only_inside_the_app) {
    boot_with_offline();
    int eva = th_person("Eva"), leo = person_as("Leo", ROLE_CHILD), gus = person_as("Gus", ROLE_GUEST);
    const MediaFile *film = find("Two Voices (2024).mp4"), *e1 = find("S01E01.Seeds.mp4");
    CHECK(film && e1 && book_id());
    uint64_t fid = film->id, sid = e1->series, bid = book_id();
    char watch[64], series[64], book[64], want[200];
    snprintf(watch, sizeof watch, "/media/watch/%016llx", (unsigned long long)fid);
    snprintf(series, sizeof series, "/media/series/%016llx?s=1", (unsigned long long)sid);
    snprintf(book, sizeof book, "/media/book/%016llx", (unsigned long long)bid);

    CHECK(!strstr(th_page(eva, watch), "hemma-app:")); /* (a browser couldn't follow them) */
    CHECK(!strstr(th_page(eva, "/media"), "hemma-app:"));
    snprintf(want, sizeof want,
             "<a class=\"btn media-offline\" href=\"hemma-app:save?m=%%2Fmedia%%2Foffline%%2F%016llx.json\">Save for "
             "offline</a>",
             (unsigned long long)fid);
    CHECK_HAS(app_page(eva, watch), want);
    snprintf(want, sizeof want, "href=\"hemma-app:save?m=%%2Fmedia%%2Foffline%%2Fseason%%2F%016llx-1.json\">Save season for offline</a>",
             (unsigned long long)sid);
    CHECK_HAS(app_page(eva, series), want);
    CHECK_HAS(app_page(leo, series), want); /* (a child too - who gets no Download) */
    CHECK(!strstr(app_page(leo, series), "/media/zip/"));
    snprintf(want, sizeof want, "href=\"hemma-app:save?m=%%2Fmedia%%2Foffline%%2F%016llx.json\">Save for offline</a>",
             (unsigned long long)bid);
    CHECK_HAS(app_page(eva, book), want);
    CHECK_HAS(app_page(eva, "/media"), "<a class=\"btn media-offline-go\" href=\"hemma-app:offline\">Offline</a>");
    CHECK_HAS(app_page(leo, watch), "Save for offline");
    const char *g = app_page(gus, watch);
    CHECK(!strstr(g, "hemma-app:"));
    th_halt();
}
