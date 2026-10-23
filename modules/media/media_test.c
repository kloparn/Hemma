/* Media pages, actions, widgets and the streaming endpoint logic. */
#include "../../core/auth.h"
#include "../../core/db.h"
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../av/mp4.h"
#include "convert.h"
#include "library.h"
#include "media.h"
#include "remove.h"
#include "../../core/codec.h"

static void put(const char *root, const char *rel, const char *text) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, text, strlen(text));
}

static char g_root[512];

/* Boot with a small library; returns Adam's id. */
static int boot_with_library(void) {
    th_boot();
    int adam = th_person("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put(g_root, "Movies/Inception (2010)/Inception.2010.1080p.BluRay.x264.mkv", "m1");
    put(g_root, "Movies/Flower (2020)/Flower.mp4", "m2");
    put(g_root, "Movies/Flower (2020)/Flower.sv.srt", "1\n00:00:01,000 --> 00:00:02,000\nHej\n");
    put(g_root, "Movies/Flower (2020)/Flower.en.srt", "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.mp4", "e1");
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E02.Roots.mp4", "e2");
    put(g_root, "Series/Garden Show/Season 2/Garden.Show.S02E01.mkv", "e3");
    put(g_root, "Home Videos/Midsommar 2024/IMG_1234.mov", "home");
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

static const char *url(const char *fmt, uint64_t id) {
    static char u[200];
    char hex[17];
    media_id_hex(id, hex);
    snprintf(u, sizeof u, fmt, hex);
    return u;
}

/* An admin who can sign in, named on members' pages (issue #58). */
static int signing_admin(const char *name, const char *user) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    str_copy(p.username, sizeof p.username, user);
    str_copy(p.pass, sizeof p.pass, "hash");
    tbl_put(&T_person, &p);
    return id;
}

TEST(media_home_without_folders_invites_setup) {
    th_boot();
    int adam = signing_admin("Adam", "adam");
    int eva = th_person("Eva");
    const char *html = th_page(adam, "/media");
    CHECK_HAS(html, "Point Hemma at your movies");
    CHECK_HAS(html, "Choose folders");
    /* a member can't choose folders: no button to a page where they can't, but who to ask (issue #58) */
    html = th_page(eva, "/media");
    CHECK(!strstr(html, "Choose folders"));
    CHECK_HAS(html, "Ask Adam (admin) to add the movie folders.");
    signing_admin("Åsa <3", "asa");
    CHECK_HAS(th_page(eva, "/media"), "Ask Adam or Åsa &lt;3 (admins) to add");
    signing_admin("Bo", "bo");
    CHECK_HAS(th_page(eva, "/media"), "Ask Adam, Åsa &lt;3 or Bo (admins) to add");
    signing_admin("Cia", "cia");
    CHECK_HAS(th_page(eva, "/media"), "Ask an admin to add");
    CHECK_HAS(th_page(eva, "/media/upload"), "Ask an admin to add videos");
    th_halt();
}

TEST(media_home_shows_shelves_and_live_search) {
    int adam = boot_with_library();
    const char *html = th_page(adam, "/media");
    CHECK_HAS(html, "Recently added");
    CHECK_HAS(html, "Garden Show");
    CHECK_HAS(html, "Inception");
    CHECK_HAS(html, "Other videos");
    CHECK_HAS(html, "data-live=\"media.search\"");
    CHECK_HAS(html, "2 movies · 1 series · 1 other video");
    CHECK(!strstr(html, "Continue watching"));
    /* search (the no-JS path: ?q=) */
    html = th_page(adam, "/media?q=garden");
    CHECK_HAS(html, "Garden Show");
    CHECK(!strstr(html, "Inception"));
    html = th_page(adam, "/media?q=FLOW");
    CHECK_HAS(html, "Flower");
    CHECK_HAS(th_page(adam, "/media?q=roots"), "Garden Show"); /* episode titles are searched */
    CHECK_HAS(th_page(adam, "/media?q=zzzz"), "Nothing matches");
    /* the live action just stores the query for this screen */
    CHECK(th_act(adam, "media.search", "q=garden").ok);
    th_halt();
}

TEST(media_continue_watching_and_watched_ticks) {
    int adam = boot_with_library();
    const MediaFile *ep1 = find("S01E01.Seeds.mp4");
    char form[200];
    snprintf(form, sizeof form, "id=%016llx&pos=120&dur=1200", (unsigned long long)ep1->id);
    CHECK(th_act(adam, "media.progress", form).ok);
    const char *html = th_page(adam, "/media");
    CHECK_HAS(html, "Continue watching");
    CHECK_HAS(html, "S01E01");
    /* the player resumes */
    html = th_page(adam, url("/media/watch/%s", ep1->id));
    CHECK_HAS(html, "data-resume=\"120\"");
    CHECK_HAS(html, "#t=120\""); /* (in its address too: without wire.js the browser starts there - issue #49) */
    /* not in its last 5 seconds - that would end at once (the same rule as wire.js); a short clip, as
       92% counts as watched */
    const MediaFile *clip = find("Flower.mp4"); /* (a film: the series' next-up below stays as it is) */
    CHECK(clip != NULL);
    snprintf(form, sizeof form, "id=%016llx&pos=30&dur=40", (unsigned long long)clip->id);
    CHECK(th_act(adam, "media.progress", form).ok);
    CHECK_HAS(th_page(adam, url("/media/watch/%s", clip->id)), "data-resume=\"30\"");
    snprintf(form, sizeof form, "id=%016llx&pos=36&dur=40", (unsigned long long)clip->id);
    CHECK(th_act(adam, "media.progress", form).ok);
    const char *end = th_page(adam, url("/media/watch/%s", clip->id));
    CHECK(!strstr(end, "data-resume") && !strstr(end, "#t="));
    /* nobody else sees Adam's progress */
    int eva = th_person("Eva");
    CHECK(!strstr(th_page(eva, "/media"), "Continue watching"));
    /* an x takes it out of the list: Adam's progress goes, Eva's stays */
    CHECK_HAS(html = th_page(adam, "/media"), "id=\"media-going-");
    CHECK_HAS(html, "data-a=\"media.forget\"");
    CHECK_HAS(th_page(adam, "/"), "data-a=\"media.forget\""); /* the home card too */
    snprintf(form, sizeof form, "id=%016llx&pos=300&dur=1200", (unsigned long long)ep1->id);
    CHECK(th_act(eva, "media.progress", form).ok);
    snprintf(form, sizeof form, "id=%016llx", (unsigned long long)ep1->id);
    ThResult fr = th_act(adam, "media.forget", form);
    CHECK(fr.ok);
    CHECK_HAS(fr.toast, "Removed Garden Show S01E01 from Continue watching");
    CHECK(!strstr(th_page(adam, "/media"), "Continue watching"));
    CHECK(!media_watch_get(adam, ep1->id));
    CHECK(!strstr(th_page(adam, url("/media/watch/%s", ep1->id)), "data-resume")); /* from the start next time */
    CHECK(media_watch_get(eva, ep1->id) && media_watch_get(eva, ep1->id)->pos == 300);
    CHECK_HAS(th_page(eva, "/media"), "Continue watching");
    snprintf(form, sizeof form, "id=%016llx&pos=120&dur=1200", (unsigned long long)ep1->id);
    CHECK(th_act(adam, "media.progress", form).ok);
    /* finishing it (92%+) moves it out of "continue watching" */
    snprintf(form, sizeof form, "id=%016llx&pos=1150&dur=1200", (unsigned long long)ep1->id);
    CHECK(th_act(adam, "media.progress", form).ok);
    CHECK(!strstr(th_page(adam, "/media"), "Continue watching"));
    /* series page: next up is episode 2, episode 1 is ticked */
    const MediaSeries *s = media_series(ep1->series);
    html = th_page(adam, url("/media/series/%s", s->id));
    CHECK_HAS(html, "Next up S01E02");
    CHECK_HAS(html, "class=\"tick on\"");
    CHECK_HAS(html, "Season 2");
    CHECK_HAS(html, "Seeds");
    /* untick */
    snprintf(form, sizeof form, "id=%016llx&done=0", (unsigned long long)ep1->id);
    CHECK(th_act(adam, "media.watched", form).ok);
    CHECK(!media_watch_get(adam, ep1->id)->done);
    CHECK(!th_act(adam, "media.watched", "id=0000000000000001&done=1").ok);
    th_halt();
}

TEST(media_player_page_has_tracks_next_and_badges) {
    int adam = boot_with_library();
    const MediaFile *flower = find("Flower.mp4");
    const char *html = th_page(adam, url("/media/watch/%s", flower->id));
    /* one player element for good: the box goes full screen (not the video: the card shows over it) */
    CHECK_HAS(html, "<div class=\"media-player-box\" id=\"media-box\"><video id=\"media-video\" class=\"media-player\"");
    char want[160];
    snprintf(want, sizeof want, "data-static=\"%016llx\"", (unsigned long long)flower->id);
    CHECK_HAS(html, want);
    /* our own controls over it (the browser's - `controls` - only until wire.js takes over), with the
       same data-static as the video: the next episode renews both */
    CHECK_HAS(html, " controls ");
    snprintf(want, sizeof want, "<div class=\"pl\" id=\"media-box-pl\" data-player=\"media-video\" data-static=\"%016llx\" "
             "data-paused hidden>", (unsigned long long)flower->id);
    CHECK_HAS(html, want);
    CHECK_HAS(html, "data-p=\"play\"");
    CHECK_HAS(html, "data-p=\"seek\"");
    CHECK_HAS(html, "id=\"media-box-fs\" data-fullscreen=\"media-box\" data-static hidden aria-pressed=\"false\"");
    /* its subtitles in the player's menu, as the <track>s have them */
    CHECK_HAS(html, "<button type=\"button\" role=\"menuitemradio\" data-sub=\"\" aria-checked=\"true\">Off</button>");
    CHECK_HAS(html, "data-sub=\"sv\" aria-checked=\"false\">Svenska</button>");
    CHECK(!strstr(html, "<span class=\"pl-onair\">")); /* (not live) */
    /* what it says when the browser can't play it (wire.js: the video's "error") */
    CHECK_HAS(html, "<p class=\"pl-err\" role=\"alert\">This video won&#39;t play in this browser.</p>");
    CHECK_HAS(html, "data-report=\"media.progress\"");
    CHECK(!strstr(html, "data-upnext")); /* a film: nothing comes next */
    CHECK_HAS(html, "srclang=\"sv\" label=\"Svenska\"");
    CHECK_HAS(html, "srclang=\"en\" label=\"English\"");
    CHECK(!strstr(html, " default>"));
    CHECK(!strstr(html, "data-next"));
    /* a subtitle preference turns that track on */
    CHECK(th_act(adam, "media.prefs", "sublang=sv").ok);
    html = th_page(adam, url("/media/watch/%s", flower->id));
    CHECK_HAS(html, "label=\"Svenska\" default>");
    CHECK_HAS(html, "data-sub=\"sv\" aria-checked=\"true\">Svenska</button>"); /* the menu ticks it too */
    CHECK_HAS(html, "data-sub=\"\" aria-checked=\"false\">Off</button>");
    /* episodes chain to the next one and autoplay it */
    const MediaFile *ep1 = find("S01E01.Seeds.mp4"), *ep2 = find("S01E02.Roots.mp4");
    html = th_page(adam, url("/media/watch/%s", ep1->id));
    char next[80];
    snprintf(next, sizeof next, "data-next=\"/media/watch/%016llx?auto=1\"", (unsigned long long)ep2->id);
    CHECK_HAS(html, next);
    CHECK_HAS(html, "More episodes");
    CHECK_HAS(th_page(adam, url("/media/watch/%s?auto=1", ep2->id)), " autoplay");
    /* ... and say so near the end: "Next episode in 30 s", Play now, Hide (wire.js shows it) */
    html = th_page(adam, url("/media/watch/%s", ep1->id));
    snprintf(want, sizeof want, "id=\"media-upnext\" data-upnext=\"30\" data-static=\"%016llx\" hidden>",
             (unsigned long long)ep2->id);
    CHECK_HAS(html, want);
    CHECK_HAS(html, "Next episode in <span data-count>30</span> s</small><b>S01E02 · Roots</b>");
    snprintf(want, sizeof want, "<a class=\"btn primary\" href=\"/media/watch/%016llx?auto=1\"><svg",
             (unsigned long long)ep2->id);
    CHECK_HAS(html, want); /* (no autofocus off a TV: Space still pauses) */
    CHECK_HAS(html, "<button type=\"button\" class=\"btn\" data-hide>Hide</button>");
    /* tiny fake files can't be probed: the page says so honestly */
    CHECK_HAS(html, "badge");
    CHECK_HAS(th_page(adam, "/media/watch/0000000000000001"), "isn&#39;t in the library");
    th_halt();
}

TEST(media_lists_and_folders) {
    int adam = boot_with_library();
    const char *html = th_page(adam, "/media/movies?sort=year");
    CHECK_HAS(html, "2 movies");
    CHECK(strstr(html, "Flower") < strstr(html, "Inception")); /* 2020 before 2010 */
    html = th_page(adam, "/media/movies");
    CHECK(strstr(html, "Flower") < strstr(html, "Inception"));
    CHECK_HAS(th_page(adam, "/media/series"), "Garden Show");
    html = th_page(adam, "/media/browse");
    CHECK_HAS(html, "Home Videos");
    CHECK_HAS(html, "Series");
    html = th_page(adam, "/media/browse?p=0:Series/Garden%20Show");
    CHECK_HAS(html, "Season 1");
    CHECK_HAS(html, "Season 2");
    CHECK_HAS(html, "2 videos");
    html = th_page(adam, "/media/browse?p=0:Home%20Videos/Midsommar%202024");
    CHECK_HAS(html, "/media/watch/");
    CHECK_HAS(th_page(adam, "/media/nope"), "Nothing here");
    CHECK_HAS(th_page(adam, "/media/series/00000000000000aa"), "isn&#39;t in the library");
    th_halt();
}

TEST(media_settings_validate_folders) {
    th_boot();
    int adam = th_person("Adam");
    Person admin = *person_get(adam);
    admin.role = ROLE_ADMIN; /* library folders are a server-side setting: admins only */
    tbl_put(&T_person, &admin);
    int eva = th_person("Eva");
    ThResult denied = th_act(eva, "media.roots", "roots=C%3A%2F");
    CHECK(!denied.ok);
    CHECK_HAS(denied.toast, "Only an admin");
    CHECK(!strstr(th_page(eva, "/media/settings"), "name=\"roots\""));
    CHECK(!th_act(adam, "media.roots", "roots=Z%3A%2Fdefinitely%2Fnot%2Fhere").ok);
    CHECK_STR(setting("media.roots", ""), "");
    char root[512], form[700];
    str_copy(root, sizeof root, test_tmpdir());
    put(root, "a.mp4", "x");
    snprintf(form, sizeof form, "roots=%%0D%%0A%s%%0D%%0A%%0D%%0A", root);
    CHECK(th_act(adam, "media.roots", form).ok);
    CHECK_INT(media_lib()->nroots, 1);
    CHECK_INT(media_lib()->nrecent, 1);
    CHECK_HAS(th_page(adam, "/media/settings"), "Other videos");
    /* too many folders */
    Buf many = {0};
    buf_puts(&many, "roots=");
    for (int i = 0; i < 9; i++) buf_printf(&many, "%s%%0A", root);
    CHECK(!th_act(adam, "media.roots", buf_cstr(&many)).ok);
    buf_free(&many);
    CHECK(th_act(adam, "media.rescan", NULL).ok);
    CHECK(th_act(adam, "media.roots", "roots=").ok);
    CHECK_INT(media_lib()->nroots, 0);
    th_halt();
}

TEST(media_widgets_for_home_and_wall) {
    int adam = boot_with_library();
    const Module *m = module_find("media");
    CHECK(m && m->widget);
    Ctx c;
    Buf b = {0};
    app_ctx(&c, 0, "/"); /* nobody in particular: nothing to continue, so what's new */
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "Recently added");
    app_ctx_free(&c);
    const MediaFile *f = find("Inception.2010.1080p.BluRay.x264.mkv");
    media_watch_report(adam, f->id, 600, 6000, false, now_sec());
    buf_reset(&b);
    app_ctx(&c, adam, "/");
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "Continue watching");
    CHECK_HAS(buf_cstr(&b), "Inception");
    app_ctx_free(&c);
    buf_free(&b);
    /* the dashboard picks it up */
    CHECK_HAS(th_page(adam, "/"), "Continue watching");
    th_halt();
}

TEST(media_widget_invites_setup_without_folders) {
    th_boot();
    int adam = signing_admin("Adam", "adam");
    int eva = th_person("Eva");
    const char *html = th_page(adam, "/");
    CHECK_HAS(html, "Point Hemma at the drive");
    CHECK_HAS(html, "\"/media/settings\">Set up</a>");
    html = th_page(eva, "/"); /* issue #58: "Set up" led members to a page they can't change */
    CHECK(!strstr(html, "\"/media/settings\">Set up</a>"));
    CHECK_HAS(html, "Ask Adam (admin) to add the movie folders.");
    CHECK_HAS(th_page(eva, "/media/settings"), "An admin chooses which folders are in the library: ask Adam (admin).");
    th_halt();
}

/* ------------------------------------------------------------- streaming */

TEST(media_range_windows) {
    MediaRange r = media_range(NULL, 1000);
    CHECK(r.status == 200 && r.from == 0 && r.to == 999 && r.len == 1000);
    r = media_range("bytes=0-", 1000);
    CHECK(r.status == 206 && r.from == 0 && r.to == 999 && r.len == 1000);
    r = media_range("bytes=100-199", 1000);
    CHECK(r.status == 206 && r.from == 100 && r.to == 199 && r.len == 100);
    r = media_range("bytes=-300", 1000);
    CHECK(r.status == 206 && r.from == 700 && r.len == 300);
    r = media_range("bytes=990-5000", 1000);
    CHECK(r.status == 206 && r.to == 999 && r.len == 10);
    CHECK_INT(media_range("bytes=1000-", 1000).status, 416);
    CHECK_INT(media_range("bytes=0-1,4-5", 1000).status, 200);
    /* > 4 GB files keep 64-bit offsets */
    int64_t big = 6LL * 1024 * 1024 * 1024;
    r = media_range("bytes=5000000000-", big);
    CHECK(r.status == 206 && r.from == 5000000000LL && r.len == big - 5000000000LL);
}

TEST(media_range_headers) {
    Buf b = {0};
    MediaRange r = media_range("bytes=100-199", 1000);
    media_range_head(&b, &r, 1000, "video/mp4", "no-cache");
    const char *s = buf_cstr(&b);
    CHECK(str_starts(s, "HTTP/1.1 206 Partial Content\r\n"));
    CHECK_HAS(s, "Content-Type: video/mp4\r\n");
    CHECK_HAS(s, "Content-Length: 100\r\n");
    CHECK_HAS(s, "Content-Range: bytes 100-199/1000\r\n");
    CHECK_HAS(s, "Accept-Ranges: bytes\r\n");
    CHECK_HAS(s, "Connection: close\r\n");
    CHECK(str_ends(s, "\r\n\r\n"));
    buf_reset(&b);
    r = media_range("bytes=5000-", 1000);
    media_range_head(&b, &r, 1000, "video/mp4", NULL);
    CHECK(str_starts(buf_cstr(&b), "HTTP/1.1 416 "));
    CHECK_HAS(buf_cstr(&b), "Content-Range: bytes */1000\r\n");
    buf_free(&b);
}

typedef struct {
    sock_t s;
    char path[600];
    int64_t from, len;
    char head[64];
} Job;

static void run_job(void *arg) {
    Job *j = arg;
    media_stream_run(j->s, j->path, j->from, j->len, j->head, strlen(j->head));
}

TEST(media_stream_sends_exactly_the_range) {
    /* a 3 MB file of a known pattern, streamed over loopback from a worker thread */
    char path[600];
    snprintf(path, sizeof path, "%s/clip.bin", test_tmpdir());
    size_t size = 3u << 20;
    unsigned char *data = xmalloc(size);
    for (size_t i = 0; i < size; i++) data[i] = (unsigned char)((i * 7 + i / 251) & 0xFF);
    CHECK(file_write_atomic(path, data, size));
    char err[100];
    sock_t ls = tcp_listen("127.0.0.1", 0, err, sizeof err);
    CHECK(ls != SOCK_BAD);
    sock_t cl = tcp_connect_nb(0x7F000001, tcp_local_port(ls));
    sock_t sv = SOCK_BAD;
    for (int i = 0; i < 400 && sv == SOCK_BAD; i++) {
        sv = tcp_accept(ls, NULL);
        if (sv == SOCK_BAD) plat_sleep_ms(5);
    }
    CHECK(sv != SOCK_BAD);
    media_stream_resume(); /* an earlier test's th_halt() ran the module's stop hook */
    static Job job;
    job.s = sv;
    str_copy(job.path, sizeof job.path, path);
    job.from = 1000001;
    job.len = 1500000;
    str_copy(job.head, sizeof job.head, "HEAD\r\n\r\n");
    CHECK(thread_spawn(run_job, &job));
    Buf got = {0};
    int64_t until = now_ms() + 10000;
    while (now_ms() < until) {
        pollfd_t pf = {.fd = cl, .events = POLLIN};
        if (plat_poll(&pf, 1, 100) != 1) continue;
        char tmp[65536];
        long n = sock_recv(cl, tmp, sizeof tmp);
        if (n > 0) buf_put(&got, tmp, (size_t)n);
        else if (n == 0 || !sock_again(sock_errno())) break;
    }
    CHECK_INT(got.len, 8 + 1500000);
    CHECK(memcmp(got.p, "HEAD\r\n\r\n", 8) == 0);
    CHECK(memcmp(got.p + 8, data + 1000001, 1500000) == 0);
    sock_close(cl);
    sock_close(ls);
    buf_free(&got);
    free(data);
}

/* Run one request through the module's HTTP hook with a socket-less Conn;
 * the reply stays in conn.out (the flush to the dummy socket fails). */
static const char *serve(int person, const char *method, const char *path, const char *range, Buf *keep) {
    char raw[400];
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: t\r\n%s%s%s\r\n", method, path, range ? "Range: " : "",
             range ? range : "", range ? "\r\n" : "");
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

TEST(media_endpoints_require_sign_in) {
    int adam = boot_with_library();
    const MediaFile *f = find("Flower.mp4"), *sv = find("Flower.sv.srt");
    char path[80];
    Buf out = {0};
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)f->id);
    CHECK(str_starts(serve(0, "GET", path, NULL, &out), "HTTP/1.1 401"));
    snprintf(path, sizeof path, "/media/img/%016llx", (unsigned long long)f->id);
    CHECK(str_starts(serve(0, "GET", path, NULL, &out), "HTTP/1.1 401"));
    snprintf(path, sizeof path, "/media/sub/%016llx.vtt", (unsigned long long)sv->id);
    CHECK(str_starts(serve(0, "GET", path, NULL, &out), "HTTP/1.1 401"));
    CHECK_STR(serve(adam, "GET", "/media/other", NULL, &out), "not handled");
    /* signed in: HEAD describes the file, ranges work, subtitles convert */
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)f->id);
    const char *s = serve(adam, "HEAD", path, NULL, &out);
    CHECK(str_starts(s, "HTTP/1.1 200 OK"));
    CHECK_HAS(s, "Content-Length: 2\r\n"); /* the fake clip is "m2" */
    CHECK_HAS(s, "Accept-Ranges: bytes");
    s = serve(adam, "HEAD", path, "bytes=1-", &out);
    CHECK(str_starts(s, "HTTP/1.1 206"));
    CHECK_HAS(s, "Content-Range: bytes 1-1/2");
    CHECK(str_starts(serve(adam, "GET", path, "bytes=9-", &out), "HTTP/1.1 416"));
    CHECK(str_starts(serve(adam, "GET", "/media/file/0000000000000001", NULL, &out), "HTTP/1.1 404"));
    snprintf(path, sizeof path, "/media/img/%016llx", (unsigned long long)f->id); /* a video isn't an image */
    CHECK(str_starts(serve(adam, "GET", path, NULL, &out), "HTTP/1.1 404"));
    snprintf(path, sizeof path, "/media/sub/%016llx.vtt", (unsigned long long)sv->id);
    s = serve(adam, "GET", path, NULL, &out);
    CHECK(str_starts(s, "HTTP/1.1 200"));
    CHECK_HAS(s, "text/vtt");
    CHECK_HAS(s, "WEBVTT");
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)f->id);
    CHECK(str_starts(serve(adam, "POST", path, NULL, &out), "HTTP/1.1 405"));
    buf_free(&out);
    th_halt();
}

static int cache_rows(void) {
    sqlite3_stmt *st = db_prep("SELECT count(*) FROM media_file");
    int n = st && sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    if (st) sqlite3_reset(st); /* (a statement still reading keeps the table from being dropped) */
    return n;
}

TEST(media_cache_in_an_older_layout_is_dropped) {
    /* issue #38: CREATE TABLE IF NOT EXISTS never changes a table that's there, so a new column
       would have broken the library cache quietly. The cache has a format of its own. */
    boot_with_library();
    int n = cache_rows();
    CHECK(n > 0);
    media_lib_start(); /* the same layout: kept */
    CHECK_INT(cache_rows(), n);
    setting_set_int("media.cache_format", 0); /* written by an older layout */
    media_lib_start();
    CHECK_INT(cache_rows(), 0); /* dropped: the next scan fills it again */
    CHECK(setting_int("media.cache_format", 0) > 0);
    media_scan_sync();
    CHECK_INT(cache_rows(), n);
    th_halt();
}

/* A Samsung TV's video player fetches without the page's cookie: the player's addresses carry the
   screen's key (ctx_url_key; core/app_http_test.c checks what a key opens). */
TEST(media_player_addresses_carry_the_screens_key) {
    int adam = boot_with_library();
    auth_configure("media-salt", 1000);
    g_fake_now = now_sec(); /* (one clock for the page and the check: no hour turning in between) */
    char sid[33];
    CHECK(session_create(adam, false, "test", sid));
    Session *s = NULL;
    for (int i = 0; i < tbl_count(&T_session); i++)
        if (((Session *)tbl_at(&T_session, i))->person == adam) s = tbl_at(&T_session, i);
    CHECK(s != NULL);
    const MediaFile *f = find("Flower.mp4"), *sv = find("Flower.sv.srt");
    Ctx c;
    app_ctx(&c, adam, url("/media/watch/%s", f->id));
    c.sess = s; /* a stored session, as a real screen has */
    Buf page = {0};
    app_render(&c, &page);
    app_ctx_free(&c);
    Ctx kc = {.sess = s, .ip = 0x7F000001}; /* (app_ctx's screen is at 127.0.0.1) */
    char path[60], k[CTX_KEY_LEN], want[160];
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)f->id);
    ctx_url_key(&kc, path, k);
    CHECK(k[0]);
    snprintf(want, sizeof want, "src=\"%s?k=%s\"", path, k);
    CHECK_HAS(buf_cstr(&page), want);
    snprintf(path, sizeof path, "/media/sub/%016llx.vtt", (unsigned long long)sv->id);
    ctx_url_key(&kc, path, k);
    snprintf(want, sizeof want, "src=\"%s?k=%s\"", path, k);
    CHECK_HAS(buf_cstr(&page), want);
    /* no stored session (the harness's pages): plain addresses */
    snprintf(want, sizeof want, "src=\"/media/file/%016llx\"", (unsigned long long)f->id);
    CHECK_HAS(th_page(adam, url("/media/watch/%s", f->id)), want);
    g_fake_now = 0;
    buf_free(&page);
    th_halt();
}

/* Two subtitles in one language are two choices (C4): each <track> has its key as its id ("sub-en",
   "sub-en.sdh", "sub-en.2"), the menu picks by key, in the same order - the plain one first, which the
   viewer's language turns on. Picture subtitles (.sup, a VobSub's .idx) are in the same list by the same
   rules (C5), with their cue list's address (data-subpic) instead of a <track>; a VobSub's .sub never. */
TEST(media_player_offers_each_subtitle_by_its_key) {
    int adam = boot_with_library();
    static const char *const files[] = {"Night (1993).en.forced.srt", "Night (1993).en.sdh.srt", "Night (1993).English.srt",
                                        "Night (1993).en.2.srt",      "Night (1993).sv.srt",     "Night (1993).en.srt",
                                        "Night (1993).en.sup",        "Night (1993).en.3.idx",   "Night (1993).en.3.sub"};
    put(g_root, "Movies/Night (1993)/Night (1993).mp4", "film");
    for (int i = 0; i < countof(files); i++) {
        char rel[200];
        snprintf(rel, sizeof rel, "Movies/Night (1993)/%s", files[i]);
        put(g_root, rel, "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
    }
    put(g_root, "Movies/Day (1994)/Day (1994).mp4", "film");
    put(g_root, "Movies/Day (1994)/Day (1994).en.sup", "PG");
    media_scan_sync();
    const MediaFile *night = find("Night (1993).mp4");
    CHECK(night);
    if (!night) {
        th_halt();
        return;
    }
    char want[300], hex[17];
    const char *html = th_page(adam, url("/media/watch/%s", night->id));
    /* keyed - "English.srt" and "en.sup" would be more "en"s: the next numbers free (en.2 and en.3 are files') */
    static const struct {
        const char *file, *key, *lang, *label;
    } t[] = {{"Night (1993).en.srt", "en", "en", "English"},
             {"Night (1993).en.2.srt", "en.2", "en", "English 2"},
             {"Night (1993).en.3.idx", "en.3", "en", "English 3"},
             {"Night (1993).en.sup", "en.4", "en", "English 4"},
             {"Night (1993).English.srt", "en.5", "en", "English 5"},
             {"Night (1993).en.sdh.srt", "en.sdh", "en", "English (for the hard of hearing)"},
             {"Night (1993).en.forced.srt", "en.forced", "en", "English (foreign speech only)"},
             {"Night (1993).sv.srt", "sv", "sv", "Svenska"}};
    const char *last_track = html, *last_item = html;
    for (int i = 0; i < countof(t); i++) {
        const MediaFile *s = find(t[i].file);
        CHECK(s && s->type == MF_SUB && s->video == night->id);
        if (!s) continue;
        media_id_hex(s->id, hex);
        bool pic = str_ends(t[i].file, ".sup") || str_ends(t[i].file, ".idx");
        if (!pic) {
            snprintf(want, sizeof want, "<track id=\"sub-%s\" kind=\"subtitles\" src=\"/media/sub/%s.vtt\" srclang=\"%s\" label=\"%s\">",
                     t[i].key, hex, t[i].lang, t[i].label);
            CHECK_HAS(html, want);
            const char *at = strstr(html, want);
            CHECK(at && at > last_track); /* (in this order) */
            if (at) last_track = at;
            snprintf(want, sizeof want, "data-sub=\"%s\" aria-checked=\"false\">%s</button>", t[i].key, t[i].label);
        } else {
            snprintf(want, sizeof want, "<track id=\"sub-%s\"", t[i].key);
            CHECK(!strstr(html, want));
            snprintf(want, sizeof want, "data-sub=\"%s\" data-subpic=\"/media/subpic/%s.json?v=%lld-%lld\" aria-checked=\"false\">%s</button>",
                     t[i].key, hex, (long long)s->size, (long long)s->mtime, t[i].label);
        }
        CHECK_HAS(html, want);
        const char *at = strstr(html, want);
        CHECK(at && at > last_item); /* the menu: the same order, text and pictures together */
        if (at) last_item = at;
    }
    int tracks = 0;
    for (const char *p = html; (p = strstr(p, "<track ")) != NULL; p++) tracks++;
    CHECK_INT(tracks, 6); /* (none for pictures) */
    const MediaFile *vob = find("Night (1993).en.3.sub");
    CHECK(vob && vob->type == MF_SUB && vob->video == night->id);
    media_id_hex(vob ? vob->id : 0, hex);
    CHECK(!strstr(html, hex)); /* (a VobSub's pictures: through its .idx) */
    CHECK(!strstr(html, " default>"));
    CHECK(!strstr(html, "aria-checked=\"true\">English"));
    /* the viewer's language: the plain one first, in the track and in the menu */
    CHECK(th_act(adam, "media.prefs", "sublang=en").ok);
    html = th_page(adam, url("/media/watch/%s", night->id));
    CHECK_HAS(html, "srclang=\"en\" label=\"English\" default>");
    CHECK_HAS(html, "data-sub=\"en\" aria-checked=\"true\">English</button>");
    CHECK_HAS(html, "data-sub=\"en.sdh\" aria-checked=\"false\">");
    CHECK_HAS(html, "data-sub=\"\" aria-checked=\"false\">Off</button>");
    const char *d = strstr(html, " default>");
    CHECK(d && !strstr(d + 1, " default>")); /* (one) */
    CHECK(!strstr(html, "aria-checked=\"true\">English ")); /* (the text one, not a picture one) */
    /* a picture one comes on only where the language has no text one */
    const MediaFile *day = find("Day (1994).mp4"), *daysup = find("Day (1994).en.sup");
    CHECK(day && daysup);
    if (day && daysup) {
        media_id_hex(daysup->id, hex);
        snprintf(want, sizeof want, "data-sub=\"en\" data-subpic=\"/media/subpic/%s.json?v=%lld-%lld\" aria-checked=\"true\">English</button>",
                 hex, (long long)daysup->size, (long long)daysup->mtime);
        CHECK_HAS(th_page(adam, url("/media/watch/%s", day->id)), want);
    }
    /* in the reader's words */
    pref_set(adam, PREF_LANG, "sv");
    html = th_page(adam, url("/media/watch/%s", night->id));
    CHECK_HAS(html, "data-sub=\"en.sdh\" aria-checked=\"false\">English (för hörselskadade)</button>");
    CHECK_HAS(html, "label=\"English (bara främmande språk)\"");
    pref_set(adam, PREF_LANG, "es");
    CHECK_HAS(th_page(adam, url("/media/watch/%s", night->id)), ">English (para personas sordas)</button>");
    th_halt();
}

/* A DVD VobSub is two files (x.en.idx + x.en.sub): removing a film or an episode takes both, with
   every other subtitle of it (.sup, numbered ones). */
TEST(media_remove_takes_a_vobsub_pair_along) {
    int adam = boot_with_library();
    (void)adam;
    static const char *const film[] = {"Movies/Night.1993.mkv", "Movies/Night.1993.en.idx", "Movies/Night.1993.en.sub",
                                       "Movies/Night.1993.en.2.srt", "Movies/Night.1993.sv.sup"};
    for (int i = 0; i < countof(film); i++) put(g_root, film[i], "x");
    put(g_root, "Movies/Other (2000).mkv", "other"); /* (Movies isn't the film's own folder) */
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.en.idx", "idx");
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E01.Seeds.en.sub", "pictures");
    media_scan_sync();
    const MediaFile *f = find("Night.1993.mkv");
    CHECK(f);
    if (!f) {
        th_halt();
        return;
    }
    RemovePlan p;
    CHECK(media_remove_plan(f, &p));
    CHECK_INT(p.files, countof(film));
    for (int i = 0; i < countof(film); i++) {
        bool in = false;
        for (int k = 0; k < p.n; k++) in |= str_eq(p.items[k].rel, film[i]);
        if (!in) test_fail(__FILE__, __LINE__, "%s isn't removed with the film", film[i]);
    }
    media_remove_free(&p);
    const MediaFile *ep = find("S01E01.Seeds.mp4"), *idx = find("Seeds.en.idx"), *sub = find("Seeds.en.sub");
    CHECK(ep && idx && sub && idx->video == ep->id && sub->video == ep->id);
    MediaPurge mp = {0};
    CHECK(ep && media_purge_episode(ep, &mp));
    bool has_idx = false, has_sub = false;
    for (int k = 0; k < mp.n; k++) {
        has_idx |= idx && mp.ids[k] == idx->id;
        has_sub |= sub && mp.ids[k] == sub->id;
    }
    CHECK(has_idx && has_sub);
    media_purge_free(&mp);
    th_halt();
}

TEST(media_subtitles_convert_to_vtt) {
    int adam = boot_with_library();
    (void)adam;
    const MediaFile *sv = find("Flower.sv.srt");
    CHECK(sv && sv->type == MF_SUB);
    Buf b = {0};
    CHECK(media_sub_vtt(sv, &b));
    CHECK(str_starts(buf_cstr(&b), "WEBVTT"));
    CHECK_HAS(buf_cstr(&b), "00:00:01.000 --> 00:00:02.000");
    CHECK_HAS(buf_cstr(&b), "Hej");
    buf_reset(&b);
    CHECK(!media_sub_vtt(find("Flower.mp4"), &b)); /* not a subtitle */
    CHECK(!media_sub_vtt(NULL, &b));
    buf_free(&b);
    th_halt();
}

/* Two episodes in one file ("S01E01-E02"): the row says 1–2, and the series counts both. */
TEST(media_double_episodes_show_their_range) {
    th_boot();
    int adam = th_person("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put(g_root, "Series/Arthur/Season 01/Arthur - S01E01-E02 - Arthur's Eyes + Francine's Bad Hair Day SDTV.mkv", "a");
    put(g_root, "Series/Arthur/Season 01/Arthur - S01E03 & E04 - Arthur's Baby + D.W.'s Baby.mkv", "b");
    put(g_root, "Series/Arthur/Season 01/Arthur - S01E05 - Buster's Back.mkv", "c");
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->nseries, 1);
    const char *html = th_page(adam, url("/media/series/%s", ix->series[0].id));
    CHECK_HAS(html, "5 episodes"); /* 3 files, 5 episodes */
    CHECK_HAS(html, "<span class=\"media-ep-n media-ep-nn\">1–2</span>");
    CHECK_HAS(html, "<span class=\"media-ep-n media-ep-nn\">3–4</span>");
    CHECK_HAS(html, "<span class=\"media-ep-n\">5</span>");
    CHECK_HAS(html, "Arthur&#39;s Baby + D.W.&#39;s Baby");
    th_halt();
}

/* Admins see how full the library's disk is, at the foot of the sidebar (and on More). */
TEST(media_sidebar_shows_the_library_disk_to_admins) {
    int adam = boot_with_library();
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int eva = th_person("Eva");
    const char *html = th_page(adam, "/book");
    CHECK_HAS(html, "class=\"media-disk");
    CHECK_HAS(html, " free</b>");
    CHECK_HAS(html, " used of ");
    CHECK_HAS(html, "<span class=\"media-disk-bar\"><i style=\"width:");
    CHECK(strstr(html, "class=\"media-disk") < strstr(html, "class=\"me\"")); /* above the profile */
    CHECK_HAS(th_page(adam, "/more"), "class=\"media-disk");
    CHECK(!strstr(th_page(eva, "/book"), "media-disk")); /* members don't manage the disk */
    th_halt();
}

static bool there(const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    return path_stat(path, NULL, NULL, NULL);
}

static const char *id_form(uint64_t id) {
    static char form[40];
    char hex[17];
    media_id_hex(id, hex);
    snprintf(form, sizeof form, "id=%s", hex);
    return form;
}

TEST(media_admins_remove_films_and_their_own_folders) {
    int adam = boot_with_library();
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int eva = th_person("Eva");
    const char *own = "Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK";
    put(g_root, "Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK/Hocus Pocus (1993) (1080p BluRay x265).mkv", "hp");
    put(g_root, "Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK/Hocus Pocus (1993) (1080p BluRay x265).web.mp4", "c");
    put(g_root, "Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK/Hocus Pocus (1993) (1080p BluRay x265).en.srt",
        "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
    put(g_root, "Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK/release.nfo", "nfo");
    put(g_root, "Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK/Extras/Trailer.mkv", "t");
    put(g_root, "Movies/Kill Bill (2003) CD1.mkv", "kb1"); /* flat in "Movies" */
    put(g_root, "Movies/Kill Bill (2003) CD1.srt", "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
    put(g_root, "Movies/Kill Bill (2003) CD2.mkv", "kb2");
    put(g_root, "Movies/Dune (2021)/Dune (2021).mkv", "d1"); /* a folder that holds another film too */
    put(g_root, "Movies/Dune (2021)/Dune Part Two (2024).mkv", "d2");
    media_scan_sync();
    const MediaFile *hp = find("x265).mkv"), *ep = find("S01E01.Seeds.mp4");
    CHECK(hp && ep);
    if (!hp || !ep) return;
    uint64_t hp_id = hp->id, ep_id = ep->id;

    /* the button and the dialog are the admin's, for films and other videos (episodes belong to their series) */
    CHECK_HAS(th_page(adam, url("/media/watch/%s", hp_id)), "?remove=1");
    CHECK(!strstr(th_page(eva, url("/media/watch/%s", hp_id)), "?remove=1"));
    CHECK(!strstr(th_page(adam, url("/media/watch/%s", ep_id)), "?remove=1"));
    const char *dlg = th_page(adam, url("/media/watch/%s?remove=1", hp_id));
    CHECK_HAS(dlg, "Its folder <code class=\"media-code\">Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK</code> goes "
                   "with everything in it: 5 files");
    CHECK_HAS(dlg, "<code class=\"media-code\">Extras/Trailer.mkv</code>");
    CHECK_HAS(dlg, "media.remove");
    CHECK(!strstr(th_page(eva, url("/media/watch/%s?remove=1", hp_id)), "media.remove"));

    /* members can't, episodes can't */
    CHECK(!th_act(eva, "media.remove", id_form(hp_id)).ok);
    CHECK(!th_act(adam, "media.remove", id_form(ep_id)).ok);
    CHECK(there(own));

    /* the film's own folder goes as a whole; "Movies" stays; so does nobody's progress in it */
    media_watch_report(eva, hp_id, 100, 5000, false, now_ms());
    CHECK(media_watch_get(eva, hp_id) != NULL);
    ThResult r = th_act(adam, "media.remove", id_form(hp_id));
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Removed Hocus Pocus (1993)");
    CHECK_STR(r.redirect, "/media/movies");
    CHECK(!there(own));
    CHECK(there("Movies/Kill Bill (2003) CD2.mkv"));
    CHECK(!media_file(hp_id));
    CHECK(!media_watch_get(eva, hp_id));
    const Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_HAS(a->text, "removed Hocus Pocus (1993) from the library");

    /* in a shared folder only the film's own files go: CD2 (another file) stays */
    const MediaFile *kb = find("Kill Bill (2003) CD1.mkv");
    CHECK(kb != NULL);
    if (kb) CHECK(th_act(adam, "media.remove", id_form(kb->id)).ok);
    CHECK(!there("Movies/Kill Bill (2003) CD1.mkv"));
    CHECK(!there("Movies/Kill Bill (2003) CD1.srt"));
    CHECK(there("Movies/Kill Bill (2003) CD2.mkv"));

    /* a folder named after the film but holding another one keeps that one */
    const MediaFile *dune = find("Dune (2021).mkv");
    CHECK(dune != NULL);
    if (dune) {
        CHECK(!strstr(th_page(adam, url("/media/watch/%s?remove=1", dune->id)), "Its folder"));
        CHECK(th_act(adam, "media.remove", id_form(dune->id)).ok);
    }
    CHECK(!there("Movies/Dune (2021)/Dune (2021).mkv"));
    CHECK(there("Movies/Dune (2021)/Dune Part Two (2024).mkv"));
    th_halt();
}

/* A real (tiny) MP4 the probe can read: one picture lasting `seconds`, H.264 or HEVC. */
static bool mp4_put(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)ctx) == n; }
static bool mp4_put_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    fflush(f);
    bool ok = file_seek(f, off) && fwrite(p, 1, n, f) == n;
    fflush(f);
    return fseek(f, 0, SEEK_END) == 0 && ok;
}

static void put_video(const char *rel, bool hevc, int seconds) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    FILE *f = file_open(path, "wb");
    Mp4Out out = {f, mp4_put, mp4_put_at};
    Mp4 *m = mp4_new(&out, 0, 4096);
    static const uint8_t cfg[] = {1, 0x42, 0, 0x1E, 0xFF, 0xE0, 0};
    Mp4Track t = {.codec = hevc ? MP4_HEVC : MP4_H264, .timescale = 1000, .config = cfg, .config_len = sizeof cfg,
                  .width = 64, .height = 48};
    mp4_add_track(m, &t);
    mp4_begin(m);
    static const uint8_t frame[16] = {0, 0, 0, 12};
    mp4_sample(m, 0, frame, sizeof frame, (uint32_t)seconds * 1000, 0, true);
    mp4_finish(m);
    mp4_free(m);
    fclose(f);
}

static int subs_of(uint64_t video) {
    const MediaIndex *ix = media_lib();
    int n = 0;
    for (int i = 0; i < ix->nfiles; i++) n += ix->files[i].type == MF_SUB && ix->files[i].video == video;
    return n;
}

/* An original and its converted copy are two versions of one video: the one that plays in fewer
   places goes, the other stays - with everyone's progress and the subtitles. */
TEST(media_keeps_only_the_most_compatible_version) {
    int adam = boot_with_library();
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int eva = th_person("Eva");
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nHej\n";
    put_video("Movies/Big (1988)/Big (1988).mp4", true, 60);      /* HEVC original... */
    put_video("Movies/Big (1988)/Big (1988).web.mp4", false, 60); /* ...H.264 copy: plays in more places */
    put(g_root, "Movies/Big (1988)/Big (1988).en.srt", srt);
    put_video("Movies/Odd (2001)/Odd (2001).mp4", false, 60); /* the other way round: the copy goes */
    put_video("Movies/Odd (2001)/Odd (2001).web.mp4", true, 60);
    put_video("Movies/Cut (2002)/Cut (2002).mp4", true, 60); /* a cut-off copy: both stay */
    put_video("Movies/Cut (2002)/Cut (2002).web.mp4", false, 20);
    put_video("Movies/Solo (2003)/Solo (2003).web.mp4", false, 60); /* only a copy, with subtitles */
    put(g_root, "Movies/Solo (2003)/Solo (2003).sv.srt", srt);
    media_scan_sync();
    const MediaFile *big = find("Big (1988).mp4"), *bigc = find("Big (1988).web.mp4"), *solo = find("Solo (2003).web.mp4");
    CHECK(big && bigc && solo);
    if (!big || !bigc || !solo) return;
    uint64_t big_id = big->id, bigc_id = bigc->id;
    CHECK_INT((int)big->probe.duration_ms, 60000);
    CHECK(big->web == bigc_id);

    /* subtitles: a copy on its own has them; next to its original they are the original's */
    CHECK_INT(subs_of(solo->id), 1);
    CHECK_INT(subs_of(big_id), 1);

    /* the library settings offer it (admins only) */
    const char *page = th_page(adam, "/media/settings");
    CHECK_HAS(page, "2 videos have two versions");
    CHECK_HAS(page, "Keep only the most compatible version");
    CHECK_HAS(page, "name=\"on\" value=\"1\" data-auto>");
    CHECK(!th_act(eva, "media.drop_originals", "").ok);

    media_watch_report(eva, big_id, 30, 60, false, now_ms());
    ThResult r = th_act(adam, "media.drop_originals", "");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Deleted 2 files");
    CHECK(!there("Movies/Big (1988)/Big (1988).mp4"));
    CHECK(there("Movies/Big (1988)/Big (1988).web.mp4"));
    CHECK(there("Movies/Odd (2001)/Odd (2001).mp4"));
    CHECK(!there("Movies/Odd (2001)/Odd (2001).web.mp4"));
    CHECK(there("Movies/Cut (2002)/Cut (2002).mp4"));
    CHECK(there("Movies/Cut (2002)/Cut (2002).web.mp4"));
    CHECK(!media_watch_get(eva, big_id)); /* Eva's 30 seconds moved to the copy */
    const MediaWatch *w = media_watch_get(eva, bigc_id);
    CHECK(w && w->pos == 30);
    bigc = media_file(bigc_id); /* (rescanned) the copy plays in its own right now, with the subtitles */
    CHECK(bigc && bigc->role == MF_MAIN);
    CHECK_INT(subs_of(bigc_id), 1);

    /* an HEVC copy (as far as Hemma goes) says where it plays rather than just "may not" */
    put_video("Movies/Hev (2006)/Hev (2006).mp4", true, 60);
    put_video("Movies/Hev (2006)/Hev (2006).web.mp4", true, 60);
    media_scan_sync();
    const MediaFile *hev = find("Hev (2006).mp4");
    CHECK(hev != NULL);
    if (hev) {
        page = th_page(adam, url("/media/watch/%s", hev->id));
        CHECK_HAS(page, ">Plays on newer devices</span>");
        CHECK_HAS(page, "data-p=\"play\" aria-label=\"Play (Space)\""); /* wire.js: "Pause (Space)" while playing (#64) */
        CHECK_HAS(page, "Converted copy · HEVC video: Safari on iPhone, iPad and Mac");
        /* the file's codecs by the names people know, folded away (issue #62: "MP4 · H264 · AAC") */
        CHECK_HAS(page, "<details class=\"media-tech\"><summary>File details</summary><span>MP4 file · HEVC (H.265) "
                        "video</span></details>");
    }

    /* the setting, and what it does right after a conversion: the copy file itself is checked */
    CHECK(th_act(adam, "media.drop_auto", "on=1").ok);
    CHECK_INT((int)setting_int("media.drop_originals", 0), 1);
    CHECK(th_act(adam, "media.drop_auto", "").ok);
    CHECK_INT((int)setting_int("media.drop_originals", 1), 0);
    put_video("Movies/Auto (2004)/Auto (2004).mp4", true, 60);
    put_video("Movies/Short (2005)/Short (2005).mp4", true, 60);
    media_scan_sync();
    const MediaFile *au = find("Auto (2004).mp4"), *sh = find("Short (2005).mp4");
    CHECK(au && sh);
    if (!au || !sh) return;
    uint64_t au_id = au->id, sh_id = sh->id;
    put_video("Movies/Auto (2004)/Auto (2004).web.mp4", false, 60); /* just made: not in the library yet */
    put_video("Movies/Short (2005)/Short (2005).web.mp4", false, 30);
    char src[1024], dst[1024];
    snprintf(src, sizeof src, "%s/Movies/Auto (2004)/Auto (2004).mp4", g_root);
    snprintf(dst, sizeof dst, "%s/Movies/Auto (2004)/Auto (2004).web.mp4", g_root);
    media_watch_report(eva, au_id, 12, 60, false, now_ms());
    CHECK(convert_replace_original(au_id, src, dst));
    CHECK(!there("Movies/Auto (2004)/Auto (2004).mp4"));
    w = media_watch_get(eva, media_make_id(g_root, "Movies/Auto (2004)/Auto (2004).web.mp4"));
    CHECK(w && w->pos == 12);
    snprintf(src, sizeof src, "%s/Movies/Short (2005)/Short (2005).mp4", g_root);
    snprintf(dst, sizeof dst, "%s/Movies/Short (2005)/Short (2005).web.mp4", g_root);
    CHECK(!convert_replace_original(sh_id, src, dst)); /* half as long: the original stays */
    CHECK(there("Movies/Short (2005)/Short (2005).mp4"));
    th_halt();
}


static int with_login(const char *name, const char *user, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* Rescanning is for admins - and for the members an admin lets; display accounts never, and
   members can't let themselves. */
TEST(media_rescan_is_for_admins_and_who_they_let) {
    int adam = boot_with_library();
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int eva = with_login("Eva", "eva", ROLE_MEMBER);
    int wall = with_login("Hall tablet", "wall", ROLE_DISPLAY);
    int kid = th_person("Kid"); /* no sign-in */
    const char *button = "data-a=\"media.rescan\"";

    CHECK(th_act(adam, "media.rescan", "").ok);
    const char *page = th_page(adam, "/media/settings");
    CHECK_HAS(page, button);
    char want[120], form[120];
    snprintf(want, sizeof want, "name=\"p\" value=\"%d\" data-auto> Eva</label>", eva);
    CHECK_HAS(page, want); /* the members who can sign in, to choose from */
    snprintf(want, sizeof want, "value=\"%d\"", wall);
    CHECK(!strstr(page, want));
    snprintf(want, sizeof want, "value=\"%d\"", kid);
    CHECK(!strstr(page, want));

    ThResult r = th_act(eva, "media.rescan", "");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Rescanning is for admins");
    CHECK(!strstr(th_page(eva, "/media/settings"), button));
    CHECK(!strstr(th_page(eva, "/media/settings"), "media.rescanners")); /* the choice is the admins' */
    snprintf(form, sizeof form, "p=%d", eva);
    CHECK(!th_act(eva, "media.rescanners", form).ok); /* no letting yourself */
    CHECK(!th_act(eva, "media.rescan", "").ok);

    /* Adam lets Eva (the tablet and the kid don't count, nor a second tick) */
    snprintf(form, sizeof form, "p=%d&p=%d&p=%d&p=%d", eva, wall, kid, eva);
    r = th_act(adam, "media.rescanners", form);
    CHECK(r.ok);
    CHECK_HAS(r.toast, "1 member may rescan the library too");
    snprintf(want, sizeof want, "%d", eva);
    CHECK_STR(setting("media.rescanners", ""), want);
    CHECK(th_act(eva, "media.rescan", "").ok);
    CHECK_HAS(th_page(eva, "/media/settings"), button);
    snprintf(want, sizeof want, "name=\"p\" value=\"%d\" data-auto checked> Eva</label>", eva);
    CHECK_HAS(th_page(adam, "/media/settings"), want);

    /* a display account never, even when its id is in the list */
    snprintf(form, sizeof form, "%d,%d", eva, wall);
    setting_set("media.rescanners", form);
    CHECK(!th_act(wall, "media.rescan", "").ok);
    CHECK(!strstr(th_page(wall, "/media/settings"), button));

    /* Eva leaves: her grant goes with her (ids are never handed out again) */
    tbl_del(&T_person, eva);
    int finn = with_login("Finn", "finn", ROLE_MEMBER);
    CHECK(finn != eva);
    CHECK(!th_act(finn, "media.rescan", "").ok);

    /* none but admins again */
    r = th_act(adam, "media.rescanners", "");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Only admins may rescan");
    CHECK_STR(setting("media.rescanners", "-"), "");
    th_halt();
}

/* Issue #44: a folder an admin marks "Not for children" stays out of a child's Watch - lists, search, the
   watch page, the stream, a TV with a child picked - and a series with an episode in it goes whole. */
TEST(media_folders_not_for_children) {
    int adam = boot_with_library();
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int eva = th_person("Eva"), leo = th_person("Leo"), tv = th_person("Living room TV");
    p = *person_get(leo);
    p.role = ROLE_CHILD;
    tbl_put(&T_person, &p);
    p = *person_get(tv);
    p.role = ROLE_DISPLAY;
    tbl_put(&T_person, &p);
    const MediaFile *inc = find("Inception.2010.1080p.BluRay.x264.mkv"), *ep = find("Garden.Show.S01E01.Seeds.mp4");
    uint64_t inc_id = inc->id, show = ep->series;
    CHECK(th_act(adam, "media.kids", "p=0:Movies/Inception%20(2010)&on=1").ok);
    CHECK(th_act(adam, "media.kids", "p=0:Series/Garden%20Show/Season%202&on=1").ok); /* one season: the show goes */
    ThResult r = th_act(leo, "media.kids", "p=0:Movies&on=0");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Ask a grown-up");
    const char *h = th_page(leo, "/media/movies");
    CHECK_HAS(h, "Flower");
    CHECK(!strstr(h, "Inception"));
    CHECK_HAS(h, "1 movie");
    CHECK(!strstr(th_page(leo, "/media/series"), "Garden Show"));
    h = th_page(leo, "/media");
    CHECK(!strstr(h, "Inception"));
    CHECK(!strstr(h, "Garden Show"));
    CHECK(!strstr(th_page(leo, "/media?q=incep"), "Inception"));
    CHECK_HAS(th_page(leo, url("/media/watch/%s", inc_id)), "isn&#39;t in the library");
    CHECK_HAS(th_page(leo, url("/media/series/%s", show)), "isn&#39;t in the library");
    CHECK(!strstr(th_page(leo, "/media/browse?p=0:Movies/Inception%20(2010)"), "/media/watch/"));
    Buf out = {0};
    char path[80];
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)inc_id);
    CHECK(str_starts(serve(leo, "GET", path, NULL, &out), "HTTP/1.1 404"));
    CHECK(str_starts(serve(eva, "HEAD", path, NULL, &out), "HTTP/1.1 200"));
    buf_free(&out);
    /* everyone else sees it all */
    CHECK_HAS(th_page(eva, "/media/movies"), "Inception");
    CHECK_HAS(th_page(eva, "/media/series"), "Garden Show");
    /* the TV, with Leo picked */
    Ctx c;
    app_ctx(&c, tv, "/");
    CHECK(media_kids_ok(&c, media_file(inc_id)));
    ctx_set_viewer(&c, leo);
    CHECK(!media_kids_ok(&c, media_file(inc_id)));
    CHECK_INT(media_lib_for(&c)->nmovies, 1);
    CHECK_INT(media_lib_for(&c)->nseries, 0);
    ctx_set_viewer(&c, eva);
    CHECK_INT(media_lib_for(&c)->nmovies, 2);
    app_ctx_free(&c);
    /* the admin's views */
    h = th_page(adam, "/media/settings");
    CHECK_HAS(h, "Not for children");
    CHECK_HAS(h, "Movies/Inception (2010)");
    h = th_page(adam, "/media/browse?p=0:Movies");
    CHECK_HAS(h, "not for children");
    h = th_page(adam, "/media/browse?p=0:Movies/Inception%20(2010)");
    CHECK_HAS(h, "name=\"on\" value=\"1\" data-auto checked");
    /* open to children again (Library settings' list names the folder itself) */
    Buf form = {0};
    buf_puts(&form, "root=");
    url_encode(&form, g_root, false);
    buf_puts(&form, "&dir=Movies%2FInception%20(2010)");
    CHECK(th_act(adam, "media.kids", buf_cstr(&form)).ok);
    buf_free(&form);
    CHECK_HAS(th_page(leo, "/media/movies"), "Inception");
    th_halt();
}

/* Issue #45: erasing someone takes what they watched; someone who moved out keeps it. */
TEST(media_someone_who_leaves) {
    int adam = boot_with_library();
    int eva = th_person("Eva");
    const MediaFile *f = find("Flower.mp4");
    media_watch_report(eva, f->id, 30, 100, false, now_sec());
    media_watch_report(adam, f->id, 50, 100, false, now_sec());
    Forget fg = {.person = eva, .keep = true, .apply = true, .now = now_sec()};
    person_forget(&fg);
    CHECK(media_watch_get(eva, f->id) != NULL);
    fg = (Forget){.person = eva, .apply = true, .now = now_sec()};
    person_forget(&fg);
    CHECK(media_watch_get(eva, f->id) == NULL);
    CHECK(media_watch_get(adam, f->id) != NULL);
    th_halt();
}

/* Issue #27: the folders the installer was told about (HEMMA_MEDIA in the settings file) are offered with one tap -
   no path typed from memory. One that isn't there isn't offered; one in the library already isn't either. */
TEST(media_offers_the_folders_the_installer_knows) {
    th_boot();
    int adam = signing_admin("Adam", "adam"), eva = th_person("Eva");
    char a[512], b2[512];
    str_copy(a, sizeof a, test_tmpdir());
    str_copy(b2, sizeof b2, test_tmpdir());
    Buf told = {0}, form = {0};
    buf_printf(&told, "%s; %s ;/no/such/folder", a, b2);
    char disk[512];
    str_copy(disk, sizeof disk, test_tmpdir());
    media_test_mounts(disk); /* (a disk "this machine" has: offered while the library has no folders) */
    plat_setenv("HEMMA_MEDIA", buf_cstr(&told), true);
    const char *h = th_page(adam, "/media"); /* (no library folders yet: the first-run buttons) */
    CHECK_HAS(h, "Use <code class=\"media-code\">");
    CHECK_HAS(h, a);
    CHECK_HAS(h, disk);
    CHECK(!strstr(h, "/no/such/folder"));
    h = th_page(adam, "/media/settings");
    CHECK_HAS(h, "from the installer");
    buf_puts(&form, "dir=");
    url_encode(&form, a, false);
    CHECK(!th_act(eva, "media.roots_add", buf_cstr(&form)).ok); /* (an admin's) */
    CHECK(th_act(adam, "media.roots_add", buf_cstr(&form)).ok);
    CHECK_STR(setting("media.roots", ""), a);
    ThResult r = th_act(adam, "media.roots_add", buf_cstr(&form));
    CHECK_HAS(r.toast, "in the library already");
    h = th_page(adam, "/media/settings");
    char label[600];
    snprintf(label, sizeof label, "aria-label=\"Add %s\"", a);
    CHECK(!strstr(h, label)); /* (taken) */
    snprintf(label, sizeof label, "aria-label=\"Add %s\"", b2);
    CHECK_HAS(h, label);      /* (the other one: still offered) */
    CHECK(!strstr(h, disk));  /* (the library has a folder now: no more disks offered) */
    buf_reset(&form);
    buf_puts(&form, "dir=");
    url_encode(&form, b2, false);
    CHECK(th_act(adam, "media.roots_add", buf_cstr(&form)).ok);
    Buf both = {0};
    buf_printf(&both, "%s\n%s", a, b2);
    CHECK_STR(setting("media.roots", ""), buf_cstr(&both));
    plat_setenv("HEMMA_MEDIA", "", true);
    buf_free(&told);
    buf_free(&form);
    buf_free(&both);
    th_halt();
}

/* 1.1: adding videos where an admin looks for it - every library tab's header, not Library settings' corner */
TEST(media_admins_find_add_videos_on_watch) {
    int member = boot_with_library();
    int admin = signing_admin("Anna", "anna");
    static const char *const tabs[] = {"/media", "/media/movies", "/media/series", "/media/browse"};
    for (size_t i = 0; i < sizeof tabs / sizeof tabs[0]; i++) {
        CHECK_HAS(th_page(admin, tabs[i]), "<a class=\"btn primary media-add\" href=\"/media/upload\">");
        CHECK(!strstr(th_page(member, tabs[i]), "href=\"/media/upload\""));
    }
    /* a series' page: "Add episodes" into its folder (not while managing it) */
    CHECK(media_lib()->nseries > 0);
    uint64_t sid = media_lib()->series[0].id;
    char add[80], hex[17];
    media_id_hex(sid, hex);
    snprintf(add, sizeof add, "<a class=\"btn\" href=\"/media/upload?series=%s\">", hex);
    CHECK_HAS(th_page(admin, url("/media/series/%s", sid)), add);
    CHECK(!strstr(th_page(member, url("/media/series/%s", sid)), "/media/upload"));
    CHECK(!strstr(th_page(admin, url("/media/series/%s?manage=1", sid)), add));
    setting_set("media.roots", ""); /* no folder yet: the empty page offers folders first */
    media_scan_sync();
    CHECK(!strstr(th_page(admin, "/media"), "media-add"));
    th_halt();
}
