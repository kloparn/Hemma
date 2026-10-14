/* Watch on the TV: the tabs, "Recently added" grouped by series, the rows media offers to screens,
   the viewer on a display account, and the TV-mode pages (?tv=1) with their way back. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "media.h"

static void put(const char *root, const char *rel, const char *text) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, text, strlen(text));
}

static char g_root[512];

/* Two series, two films; returns Adam's id. */
static int library(void) {
    th_boot();
    int adam = th_person("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.mp4", "e1");
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E02.Roots.mp4", "e2");
    put(g_root, "Series/Garden Show/Season 2/Garden.Show.S02E01.mp4", "e3");
    put(g_root, "Series/Garden Show/poster.jpg", "jpeg");
    put(g_root, "Series/Night Owls/Season 1/Night.Owls.S01E01.mp4", "n1");
    put(g_root, "Series/Night Owls/Season 1/Night.Owls.S01E02.mp4", "n2");
    put(g_root, "Movies/Inception (2010)/Inception.2010.mkv", "m1");
    put(g_root, "Movies/Flower (2020)/Flower.mp4", "m2");
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

static const MediaSeries *series_named(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nseries; i++)
        if (str_eq(ix->series[i].title, title)) return &ix->series[i];
    return NULL;
}

static int display_account(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_DISPLAY;
    tbl_put(&T_person, &p);
    return id;
}

/* Who's watching on a display account (the screens module's picker does this on a real TV). */
static void pick(int display, int person) {
    Ctx c;
    app_ctx(&c, display, "/");
    ctx_set_viewer(&c, person);
    app_ctx_free(&c);
}

/* One block's row as `person` sees it on the page `path` (kind: SCREEN_TV, or 0 = Watch's home). */
static bool row(const char *id, int person, const char *path, int kind, const char *arg, Buf *out) {
    const Block *bl = block_find(id, NULL);
    if (!bl) return false;
    Ctx c;
    app_ctx(&c, person, path);
    buf_reset(out);
    bool any = bl->render(&c, out, kind, arg);
    app_ctx_free(&c);
    return any;
}

static int count(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static void progress(int person, const MediaFile *f, int pos, int dur) {
    char form[120];
    snprintf(form, sizeof form, "id=%016llx&pos=%d&dur=%d", (unsigned long long)f->id, pos, dur);
    CHECK(th_act(person, "media.progress", form).ok);
}

/* ------------------------------------------------------------------ tabs */

TEST(media_watch_has_live_tv_among_its_tabs) {
    int adam = library();
    const char *h = th_page(adam, "/media");
    CHECK_HAS(h, "<a href=\"/media\" class=\"on\" aria-current=\"page\">Home</a>");
    CHECK_HAS(h, "<a href=\"/media/movies\">Movies</a><a href=\"/media/series\">Series</a><a href=\"/tv\">Live TV</a>"
                 "<a href=\"/media/browse\">Folders</a>");
    CHECK_HAS(h, "data-live=\"media.search\"");
    h = th_page(adam, "/media/series");
    CHECK_HAS(h, "<a href=\"/media/series\" class=\"on\" aria-current=\"page\">Series</a>");
    th_halt();
}

/* ------------------------------------------------------- recently added */

static void set_mtime(const char *rel_suffix, int64_t t) { ((MediaFile *)find(rel_suffix))->mtime = t; }

static const MediaIndex *g_ix;
static int newest_first(const void *x, const void *y) {
    int64_t a = g_ix->files[*(const int *)x].mtime, b = g_ix->files[*(const int *)y].mtime;
    return a != b ? (a > b ? -1 : 1) : 0;
}

TEST(media_recently_added_groups_a_series_new_episodes) {
    int adam = library();
    /* everything came at once: each series is one entry, the films themselves */
    MediaRecent r[10];
    int n = media_recent(r, countof(r));
    CHECK_INT(n, 4);
    int garden = -1;
    for (int i = 0; i < n; i++)
        if (r[i].series && str_eq(r[i].series->title, "Garden Show")) garden = i;
    CHECK(garden >= 0 && r[garden].n == 3);
    const char *h = th_page(adam, "/media");
    CHECK_HAS(h, "Garden Show<small>3 new episodes</small>");
    CHECK_HAS(h, "Night Owls<small>2 new episodes</small>");
    CHECK(!strstr(h, "Garden Show<small>S01E01")); /* no poster per episode any more */
    CHECK_HAS(h, "Inception");

    /* days ago: interleaved series still make one entry each; an older batch is its own */
    int64_t now = now_sec(), day = 86400;
    set_mtime("Flower.mp4", now);
    set_mtime("S02E01.mp4", now - 1 * day);
    set_mtime("Night.Owls.S01E02.mp4", now - 1 * day - day / 2);
    set_mtime("S01E02.Roots.mp4", now - 2 * day);
    set_mtime("Night.Owls.S01E01.mp4", now - 2 * day - day / 2);
    set_mtime("Inception.2010.mkv", now - 3 * day);
    set_mtime("S01E01.Seeds.mp4", now - 20 * day);
    MediaIndex *ix = (MediaIndex *)media_lib();
    g_ix = ix;
    qsort(ix->recent, (size_t)ix->nrecent, sizeof(int), newest_first);
    n = media_recent(r, countof(r));
    CHECK_INT(n, 5);
    CHECK(r[0].f == find("Flower.mp4") && r[0].n == 1 && !r[0].series);
    CHECK(r[1].series == series_named("Garden Show") && r[1].n == 2 && r[1].f == find("S02E01.mp4"));
    CHECK(r[2].series == series_named("Night Owls") && r[2].n == 2);
    CHECK(r[3].f == find("Inception.2010.mkv"));
    CHECK(r[4].f == find("S01E01.Seeds.mp4") && r[4].n == 1 && !r[4].series); /* three weeks older */
    CHECK_INT(media_recent(r, 2), 2); /* room for two: the batches still count every episode */
    CHECK(r[1].n == 2);
    th_halt();
}

/* ------------------------------------------------------------------ rows */

TEST(media_rows_draw_tiles_or_stay_out) {
    int adam = library();
    Buf b = {0};
    const MediaFile *ep1 = find("S01E01.Seeds.mp4"), *flower = find("Flower.mp4");
    const MediaSeries *garden = series_named("Garden Show");
    char want[200];

    /* nothing started yet: no Continue watching row */
    CHECK(!row("media.continue", adam, "/screens/2", SCREEN_TV, "", &b));
    CHECK_INT(b.len, 0);
    progress(adam, flower, 120, 1200);
    CHECK(row("media.continue", adam, "/screens/2", SCREEN_TV, "", &b));
    CHECK(str_starts(buf_cstr(&b), "<div class=\"tiles\"><a class=\"tile wide\" href=\""));
    snprintf(want, sizeof want, "href=\"/media/watch/%016llx?tv=1&amp;back=%%2Fscreens%%2F2\"",
             (unsigned long long)flower->id);
    CHECK_HAS(buf_cstr(&b), want); /* on a screen: the TV-mode player, and back to the screen */
    CHECK_HAS(buf_cstr(&b), "<span class=\"tile-bar\"><i style=\"width:10%\"></i></span>");
    CHECK_HAS(buf_cstr(&b), "18 min left");
    CHECK_HAS(buf_cstr(&b), "<b class=\"tile-t\">Flower</b>");
    CHECK_HAS(buf_cstr(&b), "media-drawn"); /* no picture: a drawn poster in the title's colour */
    CHECK_HAS(buf_cstr(&b), "--h2:");
    CHECK(row("media.continue", adam, "/media", 0, "", &b)); /* an ordinary page: plain links */
    snprintf(want, sizeof want, "href=\"/media/watch/%016llx\"", (unsigned long long)flower->id);
    CHECK_HAS(buf_cstr(&b), want);
    CHECK(!strstr(buf_cstr(&b), "tv=1"));

    CHECK(row("media.recent", adam, "/screens/2", SCREEN_TV, "", &b));
    CHECK_HAS(buf_cstr(&b), "3 new episodes");
    snprintf(want, sizeof want, "href=\"/media/series/%016llx?tv=1&amp;back=%%2Fscreens%%2F2\"",
             (unsigned long long)garden->id);
    CHECK_HAS(buf_cstr(&b), want);
    CHECK_HAS(buf_cstr(&b), "<img src=\"/media/img/"); /* Garden Show has a picture */
    /* on a wall: the same tiles, nothing that opens (issue #80: a tap started the TV player) */
    CHECK(row("media.recent", adam, "/screens/3", SCREEN_WALL, "", &b));
    CHECK_HAS(buf_cstr(&b), "3 new episodes");
    CHECK_HAS(buf_cstr(&b), "<div class=\"tile\">");
    CHECK(!strstr(buf_cstr(&b), "href="));
    /* and the home page's card groups them too (issue #59: one card per episode) - for someone
       with nothing on the go (Adam has: his card is Continue watching) */
    const char *home = th_page(th_person("Bo"), "/");
    CHECK_HAS(home, "Garden Show<small>3 new episodes</small>");
    CHECK(!strstr(home, "Garden Show<small>S01E01"));

    CHECK(row("media.series", adam, "/screens/2", SCREEN_TV, "", &b));
    CHECK_INT(count(buf_cstr(&b), "<a class=\"tile\""), 2);
    CHECK_HAS(buf_cstr(&b), "3 episodes · 2 seasons");
    CHECK(row("media.movies", adam, "/screens/2", SCREEN_TV, "", &b));
    CHECK_INT(count(buf_cstr(&b), "<a class=\"tile\""), 2);
    CHECK_HAS(buf_cstr(&b), "Inception");

    /* one series, picked in the builder: the next episode first, then the whole series */
    const Block *one = block_find("media.series_one", NULL);
    CHECK(one && one->choices);
    BlockChoice ch[8];
    Ctx c;
    app_ctx(&c, adam, "/screens/2");
    CHECK_INT(one->choices(&c, ch, 8), 2);
    app_ctx_free(&c);
    CHECK_STR(ch[0].label, "Garden Show");
    char hex[17];
    media_id_hex(garden->id, hex);
    CHECK_STR(ch[0].arg, hex);
    CHECK(row("media.series_one", adam, "/screens/2", SCREEN_TV, hex, &b));
    CHECK_HAS(buf_cstr(&b), "Next up · S01E01");
    CHECK_INT(count(buf_cstr(&b), "<a class=\"tile wide"), 4); /* three episodes + "All episodes" */
    CHECK_HAS(buf_cstr(&b), "<b class=\"tile-t\">All episodes</b>");
    progress(adam, ep1, 1190, 1200); /* finished: the row starts at the next one */
    CHECK(row("media.series_one", adam, "/screens/2", SCREEN_TV, hex, &b));
    CHECK(strstr(buf_cstr(&b), "Next up · S01E02") && !strstr(buf_cstr(&b), "S01E01"));
    CHECK(!row("media.series_one", adam, "/screens/2", SCREEN_TV, "0000000000000001", &b)); /* gone */
    CHECK(!row("media.series_one", adam, "/screens/2", SCREEN_TV, "", &b));
    buf_free(&b);
    th_halt();
}

TEST(media_rows_stay_out_of_an_empty_library) {
    th_boot();
    int adam = th_person("Adam");
    Buf b = {0};
    static const char *const ids[] = {"media.continue", "media.recent", "media.series", "media.movies"};
    for (int i = 0; i < countof(ids); i++) CHECK(!row(ids[i], adam, "/screens/1", SCREEN_TV, "", &b));
    const Block *one = block_find("media.series_one", NULL);
    BlockChoice ch[4];
    CHECK_INT(one->choices(NULL, ch, 4), 0);
    CHECK(block_find("media.recent", NULL)->kinds & SCREEN_WALL);
    CHECK(!(block_find("media.continue", NULL)->kinds & SCREEN_WALL));
    buf_free(&b);
    th_halt();
}

/* ------------------------------------------------------------- the viewer */

TEST(media_a_tv_watches_for_whoever_was_picked) {
    int adam = library();
    int eva = th_person("Eva"), tv = display_account("Living room TV");
    const MediaFile *ep2 = find("S01E02.Roots.mp4");
    Buf b = {0};
    char form[120];

    /* nobody picked: the TV's watching actions quietly do nothing */
    progress(tv, ep2, 300, 1200);
    CHECK(!media_watch_get(tv, ep2->id) && !media_watch_get(eva, ep2->id));
    snprintf(form, sizeof form, "id=%016llx&done=1", (unsigned long long)ep2->id);
    ThResult r = th_act(tv, "media.watched", form);
    CHECK(r.ok && !r.toast[0]);
    CHECK(!row("media.continue", tv, "/screens/2", SCREEN_TV, "", &b));
    CHECK(!strstr(th_page(tv, "/media"), "Continue watching"));
    CHECK(!th_act(tv, "media.prefs", "sublang=sv").ok); /* everything else stays view-only */

    /* Eva picked: her progress */
    pick(tv, eva);
    progress(tv, ep2, 300, 1200);
    CHECK(media_watch_get(eva, ep2->id) && media_watch_get(eva, ep2->id)->pos == 300);
    CHECK(!media_watch_get(tv, ep2->id) && !media_watch_get(adam, ep2->id));
    CHECK(row("media.continue", tv, "/screens/2", SCREEN_TV, "", &b));
    CHECK_HAS(buf_cstr(&b), "S01E02");
    CHECK(!row("media.continue", adam, "/screens/2", SCREEN_TV, "", &b)); /* not Adam's */
    const char *h = th_page(tv, "/media");
    CHECK_HAS(h, "Continue watching");
    CHECK_HAS(h, "data-a=\"media.forget\""); /* the x drops Eva's progress */
    char page[120];
    snprintf(page, sizeof page, "/media/watch/%016llx?tv=1&back=%%2Fscreens%%2F2", (unsigned long long)ep2->id);
    h = th_page(tv, page);
    CHECK_HAS(h, "data-resume=\"300\"");
    CHECK_HAS(h, "Watching as Eva");
    snprintf(form, sizeof form, "id=%016llx&done=1", (unsigned long long)ep2->id);
    CHECK(th_act(tv, "media.watched", form).ok);
    CHECK(media_watch_get(eva, ep2->id)->done);
    snprintf(form, sizeof form, "id=%016llx&done=0", (unsigned long long)ep2->id);
    CHECK(th_act(tv, "media.watched", form).ok);
    snprintf(form, sizeof form, "id=%016llx", (unsigned long long)ep2->id);
    CHECK(th_act(tv, "media.forget", form).ok);
    CHECK(!media_watch_get(eva, ep2->id));

    pick(tv, 0); /* (the harness remembers picks by account id: leave none behind) */
    buf_free(&b);
    th_halt();
}

/* Run one request through media's HTTP hook with a socket-less Conn (the reply stays in conn.out). */
static const char *serve(int person, const char *method, const char *path, Buf *keep) {
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

TEST(media_a_display_account_gets_pictures_and_streams) {
    library();
    int tv = display_account("Living room TV");
    const MediaSeries *garden = series_named("Garden Show");
    CHECK(garden && garden->poster);
    Buf out = {0};
    char path[80];
    snprintf(path, sizeof path, "/media/img/%016llx", (unsigned long long)garden->poster);
    CHECK(str_starts(serve(tv, "HEAD", path, &out), "HTTP/1.1 200"));
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)find("Flower.mp4")->id);
    CHECK(str_starts(serve(tv, "HEAD", path, &out), "HTTP/1.1 200"));
    CHECK(str_starts(serve(0, "HEAD", path, &out), "HTTP/1.1 401")); /* still nobody signed out */
    /* the dashboard's cards show a signed-in screen the picture; drawn ones only for nobody */
    int eva = th_person("Eva");
    pick(tv, eva);
    progress(tv, find("S01E02.Roots.mp4"), 300, 1200);
    const Module *m = module_find("media");
    Ctx c;
    Buf b = {0};
    app_ctx(&c, tv, "/");
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "Continue watching");
    CHECK_HAS(buf_cstr(&b), "<img src=\"/media/img/");
    app_ctx_free(&c);
    buf_reset(&b);
    app_ctx(&c, 0, "/");
    m->widget(&c, &b, WIDGET_HOME);
    CHECK(!strstr(buf_cstr(&b), "<img"));
    app_ctx_free(&c);
    pick(tv, 0);
    buf_free(&b);
    buf_free(&out);
    th_halt();
}

/* --------------------------------------------------------------- TV mode */

TEST(media_safe_back_keeps_to_this_site) {
    char out[400];
    static const char *const ok[] = {"/screens/2", "/media/series/00ab?tv=1&back=%2Fscreens%2F2", "/"};
    for (int i = 0; i < countof(ok); i++) {
        media_safe_back(ok[i], out, sizeof out);
        CHECK_STR(out, ok[i]);
    }
    static const char *const bad[] = {"//evil.example", "/\\evil",      "https://evil.example", "javascript:alert(1)",
                                      "",               "/a b",         "/a\tb",                "screens/2",
                                      "/x//y",          "/a\nLocation:"};
    for (int i = 0; i < countof(bad); i++) {
        media_safe_back(bad[i], out, sizeof out);
        CHECK_STR(out, "/");
    }
    media_safe_back(NULL, out, sizeof out);
    CHECK_STR(out, "/");
    char longer[600];
    memset(longer, 'a', sizeof longer - 1);
    longer[0] = '/', longer[sizeof longer - 1] = '\0';
    media_safe_back(longer, out, sizeof out);
    CHECK_STR(out, "/");
}

TEST(media_tv_mode_pages_are_bare_with_a_way_back) {
    int adam = library();
    const MediaFile *ep1 = find("S01E01.Seeds.mp4"), *ep2 = find("S01E02.Roots.mp4"), *flower = find("Flower.mp4");
    const MediaSeries *garden = series_named("Garden Show");
    char url[300], want[300];

    /* the player */
    progress(adam, ep1, 200, 1200);
    snprintf(url, sizeof url, "/media/watch/%016llx?tv=1&back=%%2Fscreens%%2F2", (unsigned long long)ep1->id);
    const char *h = th_page(adam, url);
    CHECK(str_starts(h, "<div id=\"app\" class=\"bare\" data-mod=\"media\"><div class=\"media-tv media-tv-watch\">"
                        "<a class=\"media-tv-back\" href=\"/screens/2\" data-esc>")); /* Back comes first */
    CHECK_HAS(h, "class=\"media-tv-video\"");
    CHECK_HAS(h, " autoplay ");
    CHECK_HAS(h, "autofocus");
    CHECK_HAS(h, "data-report=\"media.progress\"");
    CHECK_HAS(h, "data-resume=\"200\"");
    snprintf(want, sizeof want, "data-next=\"/media/watch/%016llx?tv=1&amp;auto=1&amp;back=%%2Fscreens%%2F2\"",
             (unsigned long long)ep2->id);
    CHECK_HAS(h, want);
    CHECK_HAS(h, "Next episode · S01E02");
    /* one player element (the next episode morphs into it: full screen stays), the box full screen */
    CHECK_HAS(h, "<div class=\"media-tv-player\" id=\"media-box\"><video id=\"media-video\" class=\"media-tv-video\"");
    CHECK_HAS(h, "data-fullscreen=\"media-box\"");
    /* near the end the card takes the focus: OK on the remote plays the next one now */
    snprintf(want, sizeof want, "data-upnext=\"%d\" data-static=\"%016llx\" hidden data-arrows>", MEDIA_UPNEXT_S,
             (unsigned long long)ep2->id);
    CHECK_HAS(h, want);
    snprintf(want, sizeof want, "href=\"/media/watch/%016llx?tv=1&amp;auto=1&amp;back=%%2Fscreens%%2F2\" autofocus>",
             (unsigned long long)ep2->id);
    CHECK_HAS(h, want);
    snprintf(want, sizeof want, "href=\"/media/series/%016llx?tv=1&amp;back=%%2Fscreens%%2F2\"",
             (unsigned long long)garden->id);
    CHECK_HAS(h, want); /* All episodes */
    CHECK(!strstr(h, "class=\"side\"")); /* no app chrome */
    CHECK(!strstr(h, "Mark watched"));
    /* the next episode starts from the top */
    snprintf(url, sizeof url, "/media/watch/%016llx?tv=1&auto=1&back=%%2Fscreens%%2F2", (unsigned long long)ep1->id);
    CHECK(!strstr(th_page(adam, url), "data-resume"));
    /* a film: no next, no series */
    snprintf(url, sizeof url, "/media/watch/%016llx?tv=1&back=%%2Fscreens%%2F2", (unsigned long long)flower->id);
    h = th_page(adam, url);
    CHECK(!strstr(h, "data-next") && !strstr(h, "All episodes") && !strstr(h, "data-upnext"));

    /* Back only ever leads to this site's own pages */
    static const char *const evil[] = {"%2F%2Fevil.example", "%2F%5Cevil", "https%3A%2F%2Fevil.example", ""};
    for (int i = 0; i < countof(evil); i++) {
        snprintf(url, sizeof url, "/media/watch/%016llx?tv=1&back=%s", (unsigned long long)flower->id, evil[i]);
        CHECK_HAS(th_page(adam, url), "<a class=\"media-tv-back\" href=\"/\" data-esc>");
        snprintf(url, sizeof url, "/media/series/%016llx?tv=1&back=%s", (unsigned long long)garden->id, evil[i]);
        CHECK_HAS(th_page(adam, url), "<a class=\"media-tv-back\" href=\"/\" data-esc>");
    }

    /* without tv=1 nothing changes */
    snprintf(url, sizeof url, "/media/watch/%016llx", (unsigned long long)flower->id);
    h = th_page(adam, url);
    CHECK(str_starts(h, "<div id=\"app\" class=\"shell\""));
    CHECK_HAS(h, "class=\"media-player\"");
    CHECK(!strstr(h, "media-tv"));

    /* a series: the next episode first (it has the focus), a row per season; its episodes come back here */
    snprintf(url, sizeof url, "/media/series/%016llx?tv=1&back=%%2Fscreens%%2F2", (unsigned long long)garden->id);
    h = th_page(adam, url);
    CHECK(str_starts(h, "<div id=\"app\" class=\"bare\""));
    CHECK_HAS(h, "<a class=\"media-tv-back\" href=\"/screens/2\" data-esc>");
    CHECK_HAS(h, "autofocus");
    CHECK_HAS(h, "<span>Resume S01E01</span>");
    CHECK_HAS(h, "Season 2");
    CHECK_INT(count(h, "<a class=\"tile wide"), 3);
    snprintf(want, sizeof want, "data-start=\"media-t-%016llx\"", (unsigned long long)ep1->id);
    CHECK_HAS(h, want);
    snprintf(want, sizeof want,
             "href=\"/media/watch/%016llx?tv=1&amp;back=%%2Fmedia%%2Fseries%%2F%016llx%%3Ftv%%3D1%%26back%%3D%%252Fscreens%%252F2\"",
             (unsigned long long)ep2->id, (unsigned long long)garden->id);
    CHECK_HAS(h, want);
    /* ... and that episode's Back and "All episodes" both return to it */
    snprintf(url, sizeof url, "/media/watch/%016llx?tv=1&back=%%2Fmedia%%2Fseries%%2F%016llx%%3Ftv%%3D1%%26back%%3D%%252Fscreens%%252F2",
             (unsigned long long)ep2->id, (unsigned long long)garden->id);
    h = th_page(adam, url);
    snprintf(want, sizeof want, "href=\"/media/series/%016llx?tv=1&amp;back=%%2Fscreens%%2F2\"", (unsigned long long)garden->id);
    CHECK_INT(count(h, want), 2);

    /* gone from the library: still bare, still a way back */
    h = th_page(adam, "/media/watch/0000000000000001?tv=1&back=%2Fscreens%2F2");
    CHECK(str_starts(h, "<div id=\"app\" class=\"bare\""));
    CHECK_HAS(h, "href=\"/screens/2\"");
    CHECK_HAS(h, "This video isn");
    th_halt();
}
