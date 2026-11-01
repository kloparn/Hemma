/* Adding videos from a browser: names, the plan, streamed PUTs over real
 * sockets, resuming after a dropped connection, and what must be refused. */
#include "../../core/assets.h"
#include "../../core/auth.h"
#include "../../core/codec.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../tests/harness.h"
#include "convert.h"
#include "room.h"
#include "upload.h"

void conv_test_rip(const char *root, const char *rel, const char *doctype, int64_t size); /* convert_test.c */
void conv_test_hevc(const char *root, const char *rel, int64_t size);                    /* (2.6) */
void conv_test_hevc_mp4(const char *root, const char *rel);
void conv_test_h264_mp4(const char *root, const char *rel); /* (2.7) */

TEST(upload_names_keep_folders_and_refuse_tricks) {
    char out[400];
    CHECK(upload_clean_rel("70's show/season 1 70's show/Disc - 1/B1_t01.mkv", out, sizeof out));
    CHECK_STR(out, "70's show/season 1 70's show/Disc - 1/B1_t01.mkv");
    CHECK(upload_clean_rel("Show\\Season 2\\ep.mkv", out, sizeof out)); /* Windows separators */
    CHECK_STR(out, "Show/Season 2/ep.mkv");
    CHECK(upload_clean_rel("Säsong 1/Avsnitt – ett & två.mp4", out, sizeof out));
    CHECK(upload_clean_rel("console.mkv", out, sizeof out)); /* only the exact device names are taken */
    static const char *const bad[] = {
        "../evil.mkv", "a/../../evil.mkv", "/etc/passwd", "C:/Windows/x.mkv", "a//b.mkv", "./x.mkv",
        "con.mkv",     "Show/LPT1.txt",    "what?.mkv",   "dot./x.mkv",       " lead.mkv", "bad\x01name.mkv",
        "bad\xff.mkv", "",                 "a\\..\\b.mkv"};
    for (int i = 0; i < (int)countof(bad); i++)
        if (upload_clean_rel(bad[i], out, sizeof out)) test_fail(__FILE__, __LINE__, "accepted bad name #%d", i);
}

/* ------------------------------------------------- a tiny HTTP client */

static volatile int g_done;
static int g_port;
static void serve(void *arg) {
    (void)arg;
    app_serve();
    g_done = 1;
}

static sock_t dial(void) {
    sock_t s = tcp_connect_nb(0x7F000001, g_port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) return SOCK_BAD;
    return s;
}

static void send_all(sock_t s, const void *p, size_t n) {
    const char *c = p;
    for (int64_t end = now_ms() + 5000; n && now_ms() < end;) {
        long k = sock_send(s, c, n);
        if (k > 0) c += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
}

static void read_reply(sock_t s, Buf *out) {
    buf_reset(out);
    for (int64_t end = now_ms() + 5000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[8192];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(out, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
}

/* One request (Connection: close); `send_n` < body_n sends only part of the
   body and hangs up - a dropped connection. */
static void call(const char *method, const char *path, const char *cookie, const char *extra, const void *body,
                 size_t body_n, size_t send_n, Buf *resp) {
    char head[1024];
    snprintf(head, sizeof head,
             "%s %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
             method, path, cookie, extra ? extra : "", body_n);
    buf_reset(resp);
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, head, strlen(head));
    send_all(s, body, send_n);
    if (send_n == body_n) read_reply(s, resp);
    sock_close(s);
}

static const char *body_of(const Buf *r) {
    const char *e = r->p ? strstr(r->p, "\r\n\r\n") : NULL;
    return e ? e + 4 : "";
}

static void sign_in(const char *user, const char *pass, char *cookie, size_t cap) {
    char form[200];
    snprintf(form, sizeof form, "username=%s&password=%s&next=%%2F", user, pass);
    Buf r = {0};
    call("POST", "/login", "-", "Content-Type: application/x-www-form-urlencoded\r\n", form, strlen(form), strlen(form),
         &r);
    const char *c = strstr(buf_cstr(&r), "Set-Cookie: hemma=");
    cookie[0] = '\0';
    if (c) str_ncopy(cookie, cap, c + 18, strcspn(c + 18, ";\r\n"));
    buf_free(&r);
}

static int make_user(const char *name, const char *user, const char *pass, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash(pass, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* GET the resume point once the server has settled (no worker on the file). */
static long long resume_at(const char *url, const char *cookie) {
    Buf r = {0};
    long long at = -1;
    for (int64_t end = now_ms() + 4000; now_ms() < end; plat_sleep_ms(20)) {
        call("GET", url, cookie, NULL, "", 0, 0, &r);
        const char *b = body_of(&r), *a = strstr(b, "\"at\":");
        if (a && !strstr(b, "busy")) {
            at = strtoll(a + 5, NULL, 10);
            break;
        }
    }
    buf_free(&r);
    return at;
}

static void put_file(const char *root, const char *rel, const void *data, size_t n);

static bool same_file(const char *path, const char *data, size_t n) {
    Buf got = {0};
    bool ok = file_read_all(path, &got, n + 1) && got.len == n && memcmp(got.p, data, n) == 0;
    buf_free(&got);
    return ok;
}

TEST(upload_streams_resumes_skips_and_refuses) {
    th_boot();
    auth_configure("upload-salt", 1000);
    make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    make_user("Eva", "eva", "pw-eva-1234", ROLE_MEMBER);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);

    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], eva[64];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    sign_in("eva", "pw-eva-1234", eva, sizeof eva);
    CHECK_INT(strlen(adam), 32);
    CHECK_INT(strlen(eva), 32);

    /* a 3 MB "episode": bigger than any buffered body, so it streams to disk */
    size_t big_n = 3u << 20;
    char *big = xmalloc(big_n);
    for (size_t i = 0; i < big_n; i++) big[i] = (char)(i * 31 + (i >> 9));
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nHej\n";
    char plan[1024];
    snprintf(plan, sizeof plan,
             "{\"root\":\"0\",\"into\":\"Series\",\"files\":[[\"Show/Season 1/Show.S01E01.mkv\",%zu],"
             "[\"Show/Season 1/notes.txt\",10],[\"../evil.mkv\",5],[\"Show/Thumbs.db\",3],"
             "[\"Show/Season 1/Show.S01E01.sv.srt\",%zu]]}",
             big_n, strlen(srt));
    Buf r = {0};
    const char *json = "Content-Type: application/json\r\n";

    /* refused: a member, a cross-site page, a signed-out visitor */
    call("POST", "/media/upload/plan", eva, json, plan, strlen(plan), strlen(plan), &r);
    CHECK_HAS(buf_cstr(&r), "403");
    call("POST", "/media/upload/plan", adam, "Origin: http://evil.example\r\n", plan, strlen(plan), strlen(plan), &r);
    CHECK_HAS(buf_cstr(&r), "403");
    call("POST", "/media/upload/plan", "nope", json, plan, strlen(plan), strlen(plan), &r);
    CHECK(!strstr(buf_cstr(&r), "\"batch\""));

    /* the plan: two files to send, three skipped with reasons */
    call("POST", "/media/upload/plan", adam, json, plan, strlen(plan), strlen(plan), &r);
    CHECK_HAS(buf_cstr(&r), "200 OK");
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[4,0]]");
    CHECK_HAS(body_of(&r), "\"skipped\":3");
    const char *bp = strstr(body_of(&r), "\"batch\":\"");
    char batch[17] = "";
    if (bp) str_ncopy(batch, sizeof batch, bp + 9, 16);
    CHECK_INT(strlen(batch), 16);
    char url0[96], url4[96], put0[128];
    snprintf(url0, sizeof url0, "/media/upload/%s/0", batch);
    snprintf(url4, sizeof url4, "/media/upload/%s/4", batch);

    /* the connection drops after 1.25 MB ... */
    snprintf(put0, sizeof put0, "%s?at=0", url0);
    call("PUT", put0, adam, NULL, big, big_n, 1310720, &r);
    long long at = resume_at(url0, adam);
    CHECK(at > 0 && at <= 1310720);
    char final[1024], part[1100];
    snprintf(final, sizeof final, "%s/Series/Show/Season 1/Show.S01E01.mkv", root);
    snprintf(part, sizeof part, "%s.hemma-part", final);
    CHECK(path_stat(part, NULL, NULL, NULL));  /* half a file waits as .hemma-part ... */
    CHECK(!path_stat(final, NULL, NULL, NULL)); /* ... and the library never sees it */

    /* ... a wrong offset is answered with the right one ... */
    call("PUT", put0, adam, NULL, big, big_n, big_n, &r);
    CHECK_HAS(buf_cstr(&r), "409");
    CHECK(strstr(body_of(&r), "\"at\":") != NULL);

    /* ... and the browser resumes where the server stopped */
    snprintf(put0, sizeof put0, "%s?at=%lld", url0, at);
    call("PUT", put0, adam, NULL, big + at, big_n - (size_t)at, big_n - (size_t)at, &r);
    CHECK_HAS(buf_cstr(&r), "200 OK");
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(same_file(final, big, big_n));
    CHECK(!path_stat(part, NULL, NULL, NULL));

    /* a small file comes in one buffered request */
    char put4[128];
    snprintf(put4, sizeof put4, "%s?at=0", url4);
    call("PUT", put4, adam, NULL, srt, strlen(srt) - 1, strlen(srt) - 1, &r);
    CHECK_HAS(buf_cstr(&r), "400"); /* wrong size: refused, nothing written */
    call("PUT", put4, adam, NULL, srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    char sub[1024];
    snprintf(sub, sizeof sub, "%s/Series/Show/Season 1/Show.S01E01.sv.srt", root);
    CHECK(same_file(sub, srt, strlen(srt)));

    /* the same folder again: everything is already there */
    call("POST", "/media/upload/plan", adam, json, plan, strlen(plan), strlen(plan), &r);
    CHECK_HAS(body_of(&r), "\"todo\":[]");
    CHECK_HAS(body_of(&r), "\"skipped\":5");

    /* the page shows the upload */
    call("GET", "/media/upload", adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "data-upload=\"/media/upload\"");
    CHECK_HAS(body_of(&r), "Finished");

    /* a plan that doesn't fit on the disk is refused before a byte is sent */
    upload_test_free_space(1000);
    const char *huge = "{\"root\":\"0\",\"files\":[[\"Movie (2024)/Movie.mkv\",5000000000]]}";
    call("POST", "/media/upload/plan", adam, json, huge, strlen(huge), strlen(huge), &r);
    CHECK_HAS(body_of(&r), "Not enough room");
    upload_test_free_space(-1);

    /* DVD "play all" titles stay behind but for their first MB (the header, which says how long
       the disc is) - sizes (MB) from the owner's season 1 disc 4 and season 6 disc 4 (where an
       episode sits next to bonus clips that add up to it), plus a movie next to its trailers */
    static const struct {
        const char *rel;
        int mb;
    } disc[] = {
        {"S1/Disc - 4/B1_t01.mkv", 927},   {"S1/Disc - 4/B2_t02.mkv", 949},  {"S1/Disc - 4/B3_t03.mkv", 959},
        {"S1/Disc - 4/B4_t04.mkv", 1000},  {"S1/Disc - 4/B5_t05.mkv", 1000}, {"S1/Disc - 4/B6_t06.mkv", 1024},
        {"S1/Disc - 4/B7_t00.mkv", 5859},  {"S1/Disc - 4/C1_t07.mkv", 738},  {"S1/Disc - 4/C2_t08.mkv", 183},
        {"S1/Disc - 4/C3_t09.mkv", 107},   {"S6/Disc - 4/C4_t08.mkv", 145},  {"S6/Disc - 4/D1_t00.mkv", 3465},
        {"S6/Disc - 4/E1_t01.mkv", 867},   {"S6/Disc - 4/F1_t03.mkv", 857},  {"S6/Disc - 4/G1_t04.mkv", 857},
        {"S6/Disc - 4/H3_t02.mkv", 884},   {"S6/Disc - 4/H4_t05.mkv", 218},  {"S6/Disc - 4/H6_t06.mkv", 209},
        {"S6/Disc - 4/H7_t07.mkv", 196},   {"Film (2020)/Film.mkv", 8000},   {"Film (2020)/Trailer 1.mp4", 900},
        {"Film (2020)/Trailer 2.mp4", 800}, {"Film (2020)/Trailer 3.mp4", 900},
    };
    Buf dj = {0};
    buf_puts(&dj, "{\"root\":\"0\",\"into\":\"Discs\",\"files\":[");
    for (int i = 0; i < (int)countof(disc); i++)
        buf_printf(&dj, "%s[\"%s\",%lld]", i ? "," : "", disc[i].rel, (long long)disc[i].mb << 20);
    buf_puts(&dj, "]}");
    upload_test_free_space(1LL << 40);
    call("POST", "/media/upload/plan", adam, json, dj.p, dj.len, dj.len, &r);
    CHECK_HAS(body_of(&r), "\"skipped\":0");
    CHECK_HAS(body_of(&r), "[6,0,1048576]");  /* season 1's play-all: its first MB */
    CHECK_HAS(body_of(&r), "[11,0,1048576]"); /* season 6's */
    CHECK_HAS(body_of(&r), "[12,0]");         /* the episode next to the bonus clips is whole */
    CHECK_HAS(body_of(&r), "[19,0]");         /* and the film next to its trailers */
    bp = strstr(body_of(&r), "\"batch\":\"");
    if (bp) str_ncopy(batch, sizeof batch, bp + 9, 16);
    snprintf(put0, sizeof put0, "/media/upload/%s/6?at=0", batch);
    call("PUT", put0, adam, NULL, big, big_n, big_n, &r); /* an old page sends the whole file */
    CHECK_HAS(buf_cstr(&r), "400");
    CHECK_HAS(body_of(&r), "reload the page");
    call("PUT", put0, adam, NULL, big, 1u << 20, 1u << 20, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(final, sizeof final, "%s/Discs/S1/Disc - 4/B7_t00.playall.mkv", root);
    CHECK(same_file(final, big, 1u << 20));
    /* the same folders again (after stopping the one still running - one upload at a time): that
       header is there, season 6's still to come */
    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);
    call("POST", "/media/upload/plan", adam, json, dj.p, dj.len, dj.len, &r);
    CHECK_HAS(body_of(&r), "\"skipped\":1");
    CHECK(!strstr(body_of(&r), "[6,"));
    CHECK_HAS(body_of(&r), "[11,0,1048576]");
    call("GET", "/media/upload", adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "B7_t00.playall.mkv</code> — a &quot;play all&quot; copy of the 7 episodes next to it");
    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);
    buf_free(&dj);

    /* episodes already on the server still give a play-all away (it used to go whole) */
    size_t ep_n = 16u << 20;
    char *ep = xcalloc(1, ep_n);
    for (int i = 1; i <= 3; i++) {
        snprintf(final, sizeof final, "Discs/Small/Disc - 1/A%d_t0%d.mkv", i, i);
        put_file(root, final, ep, ep_n);
    }
    const char *small = "{\"root\":\"0\",\"into\":\"Discs\",\"files\":[[\"Small/Disc - 1/A1_t01.mkv\",16777216],"
                        "[\"Small/Disc - 1/A2_t02.mkv\",16777216],[\"Small/Disc - 1/A3_t03.mkv\",16777216],"
                        "[\"Small/Disc - 1/A4_t00.mkv\",67108864]]}";
    call("POST", "/media/upload/plan", adam, json, small, strlen(small), strlen(small), &r);
    CHECK_HAS(body_of(&r), "\"todo\":[[3,0,1048576]]");
    CHECK_HAS(body_of(&r), "\"skipped\":3");
    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);
    for (int i = 1; i <= 3; i++) {
        snprintf(final, sizeof final, "%s/Discs/Small/Disc - 1/A%d_t0%d.mkv", root, i, i);
        file_remove(final);
    }
    free(ep);
    upload_test_free_space(-1);

    /* stop: the browser is told not to bother retrying */
    const char *two = "{\"root\":\"0\",\"files\":[[\"Clip.mp4\",2000000]]}";
    call("POST", "/media/upload/plan", adam, json, two, strlen(two), strlen(two), &r);
    bp = strstr(body_of(&r), "\"batch\":\"");
    if (bp) str_ncopy(batch, sizeof batch, bp + 9, 16);
    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);
    snprintf(url0, sizeof url0, "/media/upload/%s/0", batch);
    call("GET", url0, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"stop\":true");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    free(big);
    buf_free(&r);
    th_halt();
}

/* ------------------------------------------------------ pictures, copies */

static void put_file(const char *root, const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

static bool exists(const char *root, const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    return path_stat(path, NULL, NULL, NULL);
}

static void art_plan_for(const char *cookie, const char *id, const char *name, size_t size, Buf *r, char *batch) {
    char plan[300];
    snprintf(plan, sizeof plan, "{\"art\":\"%s\",\"files\":[[\"%s\",%zu]]}", id, name, size);
    call("POST", "/media/upload/plan", cookie, "Content-Type: application/json\r\n", plan, strlen(plan), strlen(plan), r);
    const char *bp = strstr(body_of(r), "\"batch\":\"");
    batch[0] = '\0';
    if (bp) str_ncopy(batch, 17, bp + 9, 16);
}

TEST(upload_sets_pictures_and_sends_copies_not_originals) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    int eva_id = make_user("Eva", "eva", "pw-eva-1234", ROLE_MEMBER);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_file(root, "Series/Show/Season 1/Disc - 1/Show.S01E01.mkv", "e1", 2);
    put_file(root, "Series/Show/Poster.PNG", "old", 3); /* an earlier picture, its name in other case */
    put_file(root, "Movies/Film (2020)/Film (2020).web.mp4", "f", 1); /* only the play-everywhere copy */
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->nseries, 1);
    CHECK_INT(ix->nmovies, 1);
    CHECK(ix->series[0].poster != 0);
    uint64_t show = ix->series[0].id, film = ix->files[ix->movies[0]].id, ep = ix->files[ix->episodes[0]].id;

    /* where pictures go: the show folder above season and disc, next to the film by its name */
    int rt;
    char dir[400], stem[400];
    CHECK(media_art_target(show, &rt, dir, sizeof dir, stem, sizeof stem));
    CHECK_STR(dir, "Series/Show");
    CHECK_STR(stem, "poster");
    CHECK(media_art_target(film, &rt, dir, sizeof dir, stem, sizeof stem));
    CHECK_STR(dir, "Movies/Film (2020)");
    CHECK_STR(stem, "Film (2020)"); /* "x.web.mp4" looks for x.jpg, like its original would */
    CHECK(!media_art_target(ep, &rt, dir, sizeof dir, stem, sizeof stem)); /* episodes show the series' */
    char hshow[17], hfilm[17], hep[17], url[160];
    media_id_hex(show, hshow);
    media_id_hex(film, hfilm);
    media_id_hex(ep, hep);

    /* the dialog is the admin's */
    snprintf(url, sizeof url, "/media/series/%s", hshow);
    CHECK_HAS(th_page(adam_id, url), "?pic=1");
    CHECK(!strstr(th_page(eva_id, url), "?pic=1"));
    snprintf(url, sizeof url, "/media/series/%s?pic=1", hshow);
    CHECK_HAS(th_page(adam_id, url), "data-upload=\"/media/upload\"");
    CHECK_HAS(th_page(adam_id, url), hshow);
    CHECK_HAS(th_page(adam_id, url), "Remove picture"); /* Poster.PNG is in that place */
    CHECK(!strstr(th_page(eva_id, url), "data-upload"));
    snprintf(url, sizeof url, "/media/watch/%s?manage=1", hfilm); /* (2.7: Picture is in a film's Manage view) */
    CHECK_HAS(th_page(adam_id, url), "&amp;pic=1");
    snprintf(url, sizeof url, "/media/watch/%s", hep);
    CHECK(!strstr(th_page(adam_id, url), "?pic=1"));

    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], eva[64], batch[17], put0[96];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    sign_in("eva", "pw-eva-1234", eva, sizeof eva);
    Buf r = {0};

    /* refused: a member, two pictures, a GIF, an episode, a huge file */
    art_plan_for(eva, hshow, "p.jpg", 100, &r, batch);
    CHECK_HAS(buf_cstr(&r), "403");
    char plan[300];
    snprintf(plan, sizeof plan, "{\"art\":\"%s\",\"files\":[[\"a.jpg\",10],[\"b.jpg\",10]]}", hshow);
    call("POST", "/media/upload/plan", adam, "Content-Type: application/json\r\n", plan, strlen(plan), strlen(plan), &r);
    CHECK_HAS(body_of(&r), "just one picture");
    art_plan_for(adam, hshow, "p.gif", 100, &r, batch);
    CHECK_HAS(body_of(&r), "JPEG, PNG or WebP");
    art_plan_for(adam, hep, "p.jpg", 100, &r, batch);
    CHECK_HAS(body_of(&r), "series or a film");
    art_plan_for(adam, hshow, "p.jpg", 30u << 20, &r, batch);
    CHECK_HAS(body_of(&r), "too big");

    /* a JPEG for the series: saved as poster.jpg in the show folder, the old picture goes */
    static const char jpg[] = "\xFF\xD8\xFF\xE0\0\x10JFIF\0 pretend the rest is a picture";
    put_file(root, "Series/Show/poster.jpg.hemma-part", "OLD-PICTURE-HEAD", 16); /* another picture's leftover */
    art_plan_for(adam, hshow, "My poster.JPEG", sizeof jpg - 1, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
    snprintf(put0, sizeof put0, "/media/upload/%s/0?at=0", batch);
    call("PUT", put0, adam, NULL, jpg, sizeof jpg - 1, sizeof jpg - 1, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    char path[1024];
    snprintf(path, sizeof path, "%s/Series/Show/poster.jpg", root);
    CHECK(same_file(path, jpg, sizeof jpg - 1));
    CHECK(!exists(root, "Series/Show/Poster.PNG"));

    /* a film: what only claims to be a PNG is refused and leaves nothing behind ... */
    static const char fake[] = "this is not a picture at all";
    art_plan_for(adam, hfilm, "cover.png", sizeof fake - 1, &r, batch);
    snprintf(put0, sizeof put0, "/media/upload/%s/0?at=0", batch);
    call("PUT", put0, adam, NULL, fake, sizeof fake - 1, sizeof fake - 1, &r);
    CHECK_HAS(buf_cstr(&r), "415");
    CHECK_HAS(body_of(&r), "isn't a JPEG, PNG or WebP");
    CHECK(!exists(root, "Movies/Film (2020)/Film (2020).png"));
    CHECK(!exists(root, "Movies/Film (2020)/Film (2020).png.hemma-part"));

    /* ... a real PNG is saved under the film's name, and served (?v= is only for caches) */
    static const char png[] = "\x89PNG\r\n\x1a\n pretend the rest is a picture";
    art_plan_for(adam, hfilm, "cover.png", sizeof png - 1, &r, batch);
    snprintf(put0, sizeof put0, "/media/upload/%s/0?at=0", batch);
    call("PUT", put0, adam, NULL, png, sizeof png - 1, sizeof png - 1, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(exists(root, "Movies/Film (2020)/Film (2020).png"));

    /* removing the series' picture deletes its file (and rescans) */
    char form[40];
    snprintf(form, sizeof form, "id=%s", hshow);
    call("POST", "/a/media.art_remove", eva, "Content-Type: application/x-www-form-urlencoded\r\n", form, strlen(form),
         strlen(form), &r);
    CHECK(exists(root, "Series/Show/poster.jpg")); /* not by a member */
    call("POST", "/a/media.art_remove", adam, "Content-Type: application/x-www-form-urlencoded\r\n", form, strlen(form),
         strlen(form), &r);
    CHECK(!exists(root, "Series/Show/poster.jpg"));
    char img[96];
    snprintf(img, sizeof img, "/media/img/%016llx?v=12",
             (unsigned long long)media_make_id(root, "Movies/Film (2020)/Film (2020).png"));
    for (int64_t end = now_ms() + 3000; now_ms() < end; plat_sleep_ms(20)) { /* once the rescan has it */
        call("GET", img, adam, NULL, "", 0, 0, &r);
        if (strstr(buf_cstr(&r), "200 OK")) break;
    }
    CHECK_HAS(buf_cstr(&r), "200 OK");
    CHECK_HAS(body_of(&r), "PNG");

    /* a converted folder sends the copies, not the originals - unless asked */
#define CONV_PLAN "{\"root\":\"0\",\"into\":\"Up\",%s\"files\":[[\"S/a.mkv\",10],[\"S/a.web.mp4\",5],[\"S/b.mkv\",10]]}"
    snprintf(plan, sizeof plan, CONV_PLAN, "");
    call("POST", "/media/upload/plan", adam, "Content-Type: application/json\r\n", plan, strlen(plan), strlen(plan), &r);
    CHECK_HAS(body_of(&r), "\"todo\":[[1,0],[2,0]]");
    CHECK_HAS(body_of(&r), "\"skipped\":1");
    snprintf(plan, sizeof plan, CONV_PLAN, "\"originals\":\"1\",");
    call("POST", "/media/upload/plan", adam, "Content-Type: application/json\r\n", plan, strlen(plan), strlen(plan), &r);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[1,0],[2,0]]");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);

    /* the next scan shows the film's new picture, and the series falls back to drawn art */
    media_scan_sync();
    CHECK(media_series(show) && media_series(show)->poster == 0);
    CHECK(media_file(film) && media_file(film)->poster != 0);
    CHECK(media_art_current(film) == media_file(film)->poster);
    th_halt();
}

/* ------------------------------------------- copies made by the sending browser */

/* An upload's card (1.5): its state's pill and the count beside it */
static bool card_is(const char *page, const char *state, const char *count) {
    char pill[96], says[128];
    snprintf(pill, sizeof pill, "<i aria-hidden=\"true\"></i>%s</span>", state);
    snprintf(says, sizeof says, "<span class=\"media-upc-count\">%s</span>", count);
    return strstr(page, pill) && strstr(page, says);
}

/* ... and the row of entry i there: tagged `tag` (Saved, Uploading, Ready, Processing, Subtitles); NULL = none */
static const char *card_row(const char *page, const char *batch, int i, const char *tag) {
    char row[160];
    snprintf(row, sizeof row, "id=\"media-upc-%s-%d\"><span class=\"media-upc-tag\">%s</span>", batch, i, tag);
    return strstr(page, row);
}

static void plan_for(const char *cookie, const char *json, Buf *r, char *batch) {
    call("POST", "/media/upload/plan", cookie, "Content-Type: application/json\r\n", json, strlen(json), strlen(json), r);
    const char *bp = strstr(body_of(r), "\"batch\":\"");
    batch[0] = '\0';
    if (bp) str_ncopy(batch, 17, bp + 9, 16);
}

TEST(upload_takes_copies_the_browser_makes) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[128];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};

    /* MKVs come as copies the browser makes ({i,f}); the rest as they are */
    const char *plan = "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"files\":[[\"S/x.mkv\",3000],[\"S/y.mkv\",20],"
                       "[\"S/z.srt\",5],[\"S/w.avi\",7]]}"; /* (an AVI: as it is - an MP4 would be read first, 2.5) */
    plan_for(adam, plan, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":0,\"f\":0},{\"i\":1,\"f\":1},[2,0],[3,0]]");
    /* without the box ticked (or a browser that can't) the originals go */
    const char *plain = "{\"root\":\"0\",\"into\":\"Up2\",\"files\":[[\"S/x.mkv\",3000]]}";
    char other[17];
    plan_for(adam, plain, &r, other);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
    /* the room slider's choice is kept for next time (the browser's converter keeps to it), within 2-32 GB */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up3\",\"conv\":\"1\",\"room\":\"16\",\"files\":[[\"S/x.mkv\",3000]]}", &r, other);
    CHECK_STR(pref(adam_id, "media.upload_room", ""), "16");
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up3\",\"conv\":\"1\",\"room\":\"99\",\"files\":[[\"S/x.mkv\",3000]]}", &r, other);
    CHECK_STR(pref(adam_id, "media.upload_room", ""), "32");
    const char *form = th_page(adam_id, "/media/upload");
    if (strstr(form, "data-wasm")) { /* (when the converter is built in: the slider comes with it) */
        CHECK_HAS(form, "name=\"room\" min=\"2\" max=\"32\" step=\"1\" value=\"32\" data-show=\"media-room-n\"");
        CHECK_HAS(form, "<input type=\"checkbox\" name=\"ram\" value=\"1\"><span>"); /* (1.3: off at first) */
    }
    /* ... and the memory box's (1.3), when converting */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up3\",\"conv\":\"1\",\"ram\":\"1\",\"files\":[[\"S/x.mkv\",3000]]}", &r, other);
    CHECK_STR(pref(adam_id, "media.upload_ram", ""), "1");
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up3\",\"files\":[[\"S/x.mkv\",3000]]}", &r, other);
    CHECK_STR(pref(adam_id, "media.upload_ram", ""), "1"); /* (not converting: kept as it was) */
    form = th_page(adam_id, "/media/upload");
    if (strstr(form, "data-wasm")) CHECK_HAS(form, "name=\"ram\" value=\"1\" checked>");
    plan_for(adam, plan, &r, batch); /* (that replaced the first plan: plan again) */

    /* nothing to send before the copy exists */
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, "0123456789", 10, 10, &r);
    CHECK_HAS(buf_cstr(&r), "409");
    CHECK_HAS(body_of(&r), "isn't made yet");

    /* progress shows on the page */
    snprintf(url, sizeof url, "/media/upload/%s/0?pm=420", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    const char *page = th_page(adam_id, "/media/upload");
    /* (1.5: the card lists it as being processed, how far, by its name on the sending device) */
    CHECK(card_is(page, "Processing", "0 of 4 files"));
    CHECK(card_row(page, batch, 0, "Processing"));
    CHECK_HAS(page, "<span class=\"media-upc-num\">42%</span><span class=\"media-upc-rbar\" aria-hidden=\"true\"><i "
                    "style=\"width:42.0%\"></i>");
    CHECK_HAS(page, "<b>x.mkv</b><small>Up/S</small>"); /* (a name that says nothing more: where it goes) */
    CHECK_HAS(page, "3 more waiting");
    /* made, waiting its turn: "Ready" - and a report sent before that, arriving after, doesn't take it back */
    snprintf(url, sizeof url, "/media/upload/%s/0?pm=1000", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    snprintf(url, sizeof url, "/media/upload/%s/0?pm=990", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    page = th_page(adam_id, "/media/upload");
    CHECK(card_row(page, batch, 0, "Ready"));
    CHECK(card_is(page, "Uploading", "0 of 4 files"));
    /* the furthest along on top: ready above processing (more of it done above less) */
    snprintf(url, sizeof url, "/media/upload/%s/1?pm=300", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    page = th_page(adam_id, "/media/upload");
    CHECK(card_row(page, batch, 0, "Ready") && card_row(page, batch, 1, "Processing"));
    CHECK(card_row(page, batch, 0, "Ready") < card_row(page, batch, 1, "Processing"));
    CHECK_HAS(page, "2 more waiting");
    /* how fast it's being made: what the converter took in of the original, per second - 0 when it stands still
       (the heartbeat's reports carry the same count) */
    upload_test_pace_window(0);
    snprintf(url, sizeof url, "/media/upload/%s/1?pm=310&fed=1000", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    plat_sleep_ms(100);
    snprintf(url, sizeof url, "/media/upload/%s/1?pm=320&fed=%d", batch, 1000 + (10 << 20)); /* (10 MB in ~0.1 s) */
    call("POST", url, adam, NULL, "", 0, 0, &r);
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "<span class=\"media-upc-num\">32% · ");
    CHECK_HAS(page, " MB/s</span>");
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(th_page(adam_id, "/media/upload"), "<span class=\"media-upc-num\">32% · 0 KB/s</span>");
    upload_test_pace_window(-1);

    /* no room for the copy: refused, and the upload stops */
    upload_test_free_space((1LL << 30) + 5);
    snprintf(url, sizeof url, "/media/upload/%s/0?size=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(buf_cstr(&r), "507");
    CHECK_HAS(body_of(&r), "No room for the copy");
    upload_test_free_space(-1);

    /* the copy is ready: sized, then sent like any file, under the copy's name */
    put_file(root, "Up/S/x.web.mp4.hemma-part", "stale", 5); /* an earlier try's leftover starts over */
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    CHECK(!exists(root, "Up/S/x.web.mp4.hemma-part"));
    call("POST", url, adam, NULL, "", 0, 0, &r); /* asked again (a lost answer): same */
    CHECK_HAS(body_of(&r), "\"at\":0");
    /* (1.3: a copy kept in the browser's memory goes in pieces - each answered with where the file is) */
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0&end=4", batch);
    call("PUT", url, adam, NULL, "copy", 4, 4, &r);
    CHECK_HAS(body_of(&r), "{\"ok\":true,\"at\":4}");
    CHECK(exists(root, "Up/S/x.web.mp4.hemma-part"));
    CHECK(!exists(root, "Up/S/x.web.mp4"));
    /* (1.5: between its pieces - no PUT running - the card still says it's uploading, and how far) */
    page = th_page(adam_id, "/media/upload");
    CHECK(card_row(page, batch, 0, "Uploading"));
    CHECK(card_is(page, "Uploading", "0 of 4 files"));
    CHECK_HAS(page, "<span class=\"media-upc-num\">40%</span>");
    CHECK(card_row(page, batch, 0, "Uploading") < card_row(page, batch, 1, "Processing"));
    snprintf(url, sizeof url, "/media/upload/%s/0?at=4&end=11", batch); /* (past the file: refused) */
    call("PUT", url, adam, NULL, "-bytes!", 7, 7, &r);
    CHECK_HAS(buf_cstr(&r), "400");
    snprintf(url, sizeof url, "/media/upload/%s/0?at=4&end=10", batch);
    call("PUT", url, adam, NULL, "-bytes", 6, 6, &r);
    CHECK_HAS(body_of(&r), "{\"ok\":true}");
    char path[1024];
    snprintf(path, sizeof path, "%s/Up/S/x.web.mp4", root);
    CHECK(same_file(path, "copy-bytes", 10));
    CHECK(!exists(root, "Up/S/x.mkv")); /* the original never came */
    page = th_page(adam_id, "/media/upload"); /* (saved: on top, with the time) */
    CHECK(card_is(page, "Processing", "1 of 4 files"));
    CHECK(card_row(page, batch, 0, "Saved") && card_row(page, batch, 0, "Saved") < card_row(page, batch, 1, "Processing"));

    /* the file's text subtitles, taken out by the browser's converter: saved next to the copy, once - a
       second English track in the file as ".2" (as the server's converter names it), never over a file */
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nHi\n\n";
    snprintf(url, sizeof url, "/media/upload/%s/0?sub=1&lang=eng&name=English&forced=0", batch);
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Up/S/x.en.srt", root);
    CHECK(same_file(path, srt, strlen(srt)));
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(body_of(&r), "\"ok\":true"); /* sent again (its answer was lost): the same answer, nothing new */
    CHECK(!exists(root, "Up/S/x.en.2.srt"));
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", "other", 5, 5, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true"); /* the file's second English track */
    CHECK(same_file(path, srt, strlen(srt)));
    snprintf(path, sizeof path, "%s/Up/S/x.en.2.srt", root);
    CHECK(same_file(path, "other", 5));
    put_file(root, "Up/S/x.sv.srt", "the household's own", 19);
    snprintf(url, sizeof url, "/media/upload/%s/0?sub=1&lang=swe&name=&forced=0", batch);
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(body_of(&r), "\"skip\":true"); /* (a file there stays) */
    snprintf(path, sizeof path, "%s/Up/S/x.sv.srt", root);
    CHECK(same_file(path, "the household's own", 19));
    snprintf(url, sizeof url, "/media/upload/%s/0?sub=1&lang=swe&forced=1", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(buf_cstr(&r), "400");

    /* one the converter couldn't handle: the original goes under its own name */
    snprintf(url, sizeof url, "/media/upload/%s/1?original=1&why=Can%%27t+convert+V_MPEGH%%2FISO%%2FHEVC+video+yet", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    snprintf(url, sizeof url, "/media/upload/%s/1?at=0", batch);
    call("PUT", url, adam, NULL, "original-bytes-20xx!", 20, 20, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(exists(root, "Up/S/y.mkv"));
    CHECK(!exists(root, "Up/S/y.web.mp4"));
    snprintf(url, sizeof url, "/media/upload/%s/1?sub=1&lang=eng", batch); /* no copy made of it: no subtitles */
    call("POST", url, adam, NULL, srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(buf_cstr(&r), "404");
    CHECK(!exists(root, "Up/S/y.en.srt"));
    for (int i = 2; i <= 3; i++) {
        snprintf(url, sizeof url, "/media/upload/%s/%d?at=0", batch, i);
        call("PUT", url, adam, NULL, "1234567", i == 2 ? 5 : 7, i == 2 ? 5 : 7, &r);
        CHECK_HAS(body_of(&r), "\"ok\":true");
    }
    for (int64_t end = now_ms() + 3000; now_ms() < end; plat_sleep_ms(20)) { /* the worker finishes after it answers */
        page = th_page(adam_id, "/media/upload");
        if (strstr(page, "Finished")) break;
    }
    CHECK(card_is(page, "Finished", "4 files sent"));
    CHECK_HAS(page, "1 sent unconverted");
    CHECK_HAS(page, "S/y.mkv</code> — sent as it is: Can&#39;t convert V_MPEGH/ISO/HEVC video yet");
    CHECK_HAS(page, "1 video couldn't be converted on this device and went up as it was - it may not play "
                    "everywhere. Why: Can&#39;t convert V_MPEGH/ISO/HEVC video yet.</p>"); /* said, not only listed */

    /* again: what's there is skipped - the copy for x, the original for y */
    plan_for(adam, plan, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[]");
    CHECK_HAS(body_of(&r), "\"skipped\":4");
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "S/x.web.mp4</code> — its play-everywhere copy is already there");

    /* "also send the originals": the copy is an added entry, dropped when it can't be made */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"originals\":\"1\",\"files\":[[\"T/a.mkv\",30]]}", &r,
             batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],{\"i\":1,\"f\":0}]");
    snprintf(url, sizeof url, "/media/upload/%s/1?original=1", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"skip\":true");
    snprintf(url, sizeof url, "/media/upload/%s/1?pm=5", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r); /* a late progress report: harmless, stops nothing */
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(!strstr(body_of(&r), "stop"));

    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);

    /* a leftover .part longer than the file can't be its beginning: it goes, the file starts over */
    put_file(root, "Up/V/big.mp4.hemma-part", "0123456789ABCDEF", 16);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"V/big.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
    CHECK(!exists(root, "Up/V/big.mp4.hemma-part"));

    /* falling back to the original needs room for it too */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"files\":[[\"W/c.mkv\",4000]]}", &r, batch);
    upload_test_free_space((1LL << 30) + 2000);
    snprintf(url, sizeof url, "/media/upload/%s/0?original=1", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(buf_cstr(&r), "507");
    CHECK_HAS(body_of(&r), "No room for the original");
    upload_test_free_space(-1);

    /* one upload at a time: while Adam's runs, Eva's new one is refused - unless she ticks "upload
       anyway"; then both run, Adam's files left to him. Stop stops only its own upload. */
    make_user("Eva", "eva", "pw-eva-1234", ROLE_ADMIN);
    char eva[64], evas[17];
    int eva_id = person_by_username("eva")->id;
    sign_in("eva", "pw-eva-1234", eva, sizeof eva);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"X/e.mp4\",10],[\"X/g.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[1,0]]");
    CHECK_HAS(th_page(eva_id, "/media/upload"), "id=\"media-up-force\" hidden"); /* nothing runs yet */
    snprintf(url, sizeof url, "/media/upload/%s/1?at=0", batch);
    call("PUT", url, adam, NULL, "0123456789", 10, 10, &r); /* g.mp4 arrives: Adam's upload is running */
    CHECK_HAS(body_of(&r), "\"ok\":true");
    const char *both = "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"X/e.mp4\",10],[\"X/f.mp4\",10]]}";
    plan_for(eva, both, &r, evas); /* (g.mp4 counted before its "ok" went: never "0 of 2" here) */
    CHECK_HAS(buf_cstr(&r), "422");
    CHECK_HAS(body_of(&r), "Adam's upload is running (1 of 2 files)");
    CHECK_HAS(th_page(eva_id, "/media/upload"), "id=\"media-up-force\"><input");
    plan_for(eva, "{\"root\":\"0\",\"into\":\"Up\",\"force\":\"1\",\"files\":[[\"X/e.mp4\",10],[\"X/f.mp4\",10]]}", &r, evas);
    CHECK_HAS(body_of(&r), "\"todo\":[[1,0]]"); /* e.mp4 is Adam's */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"X/h.mp4\",10]]}", &r, other);
    CHECK_HAS(body_of(&r), "Your upload is still running"); /* also in another tab of his own */
    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);
    snprintf(url, sizeof url, "/media/upload/%s/1", evas);
    call("GET", url, eva, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0"); /* Eva's upload goes on */
    snprintf(url, sizeof url, "/media/upload/%s/0", batch);
    call("GET", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(buf_cstr(&r), "410"); /* Adam's stopped */

    /* stopped: the browser's next progress report hears it */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"files\":[[\"U/b.mkv\",30]]}", &r, batch);
    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);
    snprintf(url, sizeof url, "/media/upload/%s/0?pm=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(buf_cstr(&r), "410");
    CHECK_HAS(body_of(&r), "\"stop\":true");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* ------------------------------------------- converted on this device (2.6) */

/* "On this device": the plan names the library's HEVC videos ({"here"}), each read from the server as the very file
   (?src=1 - the original, not its copy), its H.264 copy sent back in place of Hemma's HEVC copy; one the device couldn't
   convert is skipped, never sent as an "original" */
TEST(upload_here_makes_library_hevc_h264_on_the_device) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN), eva_id = th_person("Eva");
    char root[512], path[1100];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    conv_test_hevc(root, "Series/H/Season 1/H.S01E01.mkv", 3000);
    conv_test_hevc_mp4(root, "Series/H/Season 1/H.S01E01.web.mp4");
    conv_test_hevc_mp4(root, "Series/H/Season 1/H.S01E02.web.mp4");
    media_scan_sync();
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[160], json[200], hex[17];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    const MediaFile *e1 = NULL;
    for (int i = 0; i < media_lib()->nfiles; i++)
        if (str_eq(media_lib()->files[i].rel, "Series/H/Season 1/H.S01E01.mkv")) e1 = &media_lib()->files[i];
    CHECK(e1);
    if (!e1) {
        th_halt();
        return;
    }
    uint64_t e1_id = e1->id;
    snprintf(json, sizeof json, "{\"here\":\"series=%016llx\",\"conv\":\"1\"}", (unsigned long long)e1->series);
    plan_for(adam, json, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":0,\"f\":0},{\"i\":1,\"f\":1}]");
    media_id_hex(e1->id, hex);
    snprintf(url, sizeof url, "\"remote\":[{\"u\":\"/media/file/%s?src=1\",\"s\":3000,\"h264\":true}", hex);
    CHECK_HAS(body_of(&r), url);

    /* the very file - the original MKV - in pieces (Range), where the plain address streams its copy */
    snprintf(url, sizeof url, "/media/file/%s?src=1", hex);
    call("GET", url, adam, "Range: bytes=0-3\r\n", "", 0, 0, &r);
    CHECK_HAS(buf_cstr(&r), "206");
    CHECK(!memcmp(body_of(&r), "\x1A\x45\xDF\xA3", 4)); /* (Matroska's EBML header) */
    snprintf(url, sizeof url, "/media/file/%s", hex);
    call("GET", url, adam, "Range: bytes=4-7\r\n", "", 0, 0, &r);
    CHECK(!memcmp(body_of(&r), "ftyp", 4)); /* (the copy) */

    /* its H.264 copy takes the place of the HEVC one */
    snprintf(url, sizeof url, "/media/upload/%s/0?size=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, "0123456789", 10, 10, &r);
    CHECK_HAS(buf_cstr(&r), "200");
    snprintf(path, sizeof path, "%s/Series/H/Season 1/H.S01E01.web.mp4", root);
    CHECK(same_file(path, "0123456789", 10));
    snprintf(path, sizeof path, "%s/Series/H/Season 1/H.S01E01.mkv", root);
    CHECK(path_stat(path, NULL, NULL, NULL)); /* (the original stays) */

    /* one the device couldn't make: skipped - its file stays as it was */
    snprintf(url, sizeof url, "/media/upload/%s/1?original=1&why=Nope", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"skip\":true");
    snprintf(path, sizeof path, "%s/Series/H/Season 1/H.S01E02.web.mp4", root);
    Buf got = {0};
    CHECK(file_read_all(path, &got, 1 << 20) && got.len > 8 && !memcmp(got.p + 4, "ftyp", 4));
    buf_free(&got);

    /* a set with nothing to make */
    plan_for(adam, "{\"here\":\"films=1\",\"conv\":\"1\"}", &r, batch);
    CHECK_HAS(body_of(&r), "Nothing there needs converting any more");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    /* its card where it was started - Library settings, a video's page - for the admin who started it, a while after too */
    CHECK_HAS(th_page(adam_id, "/media/settings/converting"), "Converting on this device");
    snprintf(url, sizeof url, "/media/watch/%016llx", (unsigned long long)e1_id);
    CHECK_HAS(th_page(adam_id, url), "id=\"media-h264-run\"");
    CHECK(!strstr(th_page(eva_id, "/media/settings/converting"), "media-h264-run"));
    th_halt();
}

/* (2.7) With "Do that by itself after each conversion" ticked, a copy made on a device takes its original's place as
   one made by Hemma's own queue does (owner, 2026-11-01: the original stayed beside it) - once it's in place, on the
   loop (upload_tick), with the same checks: the copy lasts as long and plays in at least as many places */
TEST(upload_here_copies_drop_originals_when_ticked) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512], path[1100];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    conv_test_hevc(root, "Series/H/Season 1/H.S01E01.mkv", 3000); /* an HEVC original, a minute long */
    conv_test_hevc_mp4(root, "Series/H/Season 1/H.S01E01.web.mp4"); /* and Hemma's HEVC copy of it */
    media_scan_sync();
    CHECK(th_act(adam_id, "media.drop_auto", "on=1").ok);
    char made[512]; /* the H.264 copy the device made: a minute long too */
    str_copy(made, sizeof made, test_tmpdir());
    conv_test_h264_mp4(made, "copy.mp4");
    Buf copy = {0};
    snprintf(path, sizeof path, "%s/copy.mp4", made);
    CHECK(file_read_all(path, &copy, 1 << 20) && copy.len > 0);
    const MediaFile *e1 = NULL;
    for (int i = 0; i < media_lib()->nfiles; i++)
        if (str_eq(media_lib()->files[i].rel, "Series/H/Season 1/H.S01E01.mkv")) e1 = &media_lib()->files[i];
    CHECK(e1);
    if (!e1 || !copy.len) {
        buf_free(&copy);
        th_halt();
        return;
    }
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[160], json[200];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    snprintf(json, sizeof json, "{\"here\":\"series=%016llx\",\"conv\":\"1\"}", (unsigned long long)e1->series);
    plan_for(adam, json, &r, batch);
    snprintf(url, sizeof url, "/media/upload/%s/0?size=%d", batch, (int)copy.len);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, (const char *)copy.p, copy.len, (int64_t)copy.len, &r);
    CHECK_HAS(buf_cstr(&r), "200");
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    upload_tick(now_ms()); /* (the server's own ticks may have done it already) */
    snprintf(path, sizeof path, "%s/Series/H/Season 1/H.S01E01.mkv", root);
    CHECK(!path_stat(path, NULL, NULL, NULL)); /* the original went, as after Hemma's own conversions */
    snprintf(path, sizeof path, "%s/Series/H/Season 1/H.S01E01.web.mp4", root);
    CHECK(same_file(path, (const char *)copy.p, copy.len));
    buf_free(&r);
    buf_free(&copy);
    th_halt();
}

bool conv_test_hevc_three(const char *path); /* av/conv_test.c (2.7): a real HEVC + AAC MKV, 0.9 s */

/* (2.7) Full throttle on this device, fewer videos than it makes at once: a long one made in parts side by side - each
   part an entry of the plan's ({"part": [from, to]}), sent as any copy to its own file - and the server joins them into
   the video's copy (its sound from the library's file), the parts' files gone after */
TEST(upload_here_makes_a_video_in_parts_and_the_server_joins_them) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512], src[700], made[512];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    snprintf(src, sizeof src, "%s/Movies/R (2020)", root);
    CHECK(dir_make(src));
    snprintf(src, sizeof src, "%s/Movies/R (2020)/R (2020).mkv", root);
    CHECK(conv_test_hevc_three(src)); /* a real HEVC film, 0.9 s */
    media_scan_sync();
    const MediaFile *film = NULL;
    for (int i = 0; i < media_lib()->nfiles; i++)
        if (str_eq(media_lib()->files[i].rel, "Movies/R (2020)/R (2020).mkv")) film = &media_lib()->files[i];
    CHECK(film);
    if (!film) {
        th_halt();
        return;
    }
    pref_set(adam_id, "media.power_here", "full");
    convert_test_device_cores(20);
    upload_test_part_min(300); /* (parts of 300 ms: three) */
    /* the parts, as this device would make them */
    str_copy(made, sizeof made, test_tmpdir());
    char mdst[700];
    snprintf(mdst, sizeof mdst, "%s/made.web.mp4", made);
    Buf piece[3] = {{0}};
    for (int k = 0; k < 3; k++) {
        ConvertJob *pj = xcalloc(1, sizeof *pj);
        str_copy(pj->src, sizeof pj->src, src);
        str_copy(pj->dst, sizeof pj->dst, mdst);
        pj->h264 = true, pj->piece = k;
        pj->piece_from = k * 300000000LL;
        pj->piece_to = k < 2 ? (k + 1) * 300000000LL : AV_CONV_TO_END;
        CHECK(convert_file(pj));
        char path[800];
        convert_piece_path(mdst, k, path, sizeof path);
        CHECK(file_read_all(path, &piece[k], 1 << 22) && piece[k].len > 0);
        free(pj);
    }
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[160], json[200];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    snprintf(json, sizeof json, "{\"here\":\"id=%016llx\",\"conv\":\"1\"}", (unsigned long long)film->id);
    plan_for(adam, json, &r, batch);
    CHECK_HAS(body_of(&r), "{\"i\":1,\"f\":0,\"part\":[0,300]");
    CHECK_HAS(body_of(&r), "{\"i\":2,\"f\":0,\"part\":[300,600]");
    CHECK_HAS(body_of(&r), "{\"i\":3,\"f\":0,\"part\":[600,-1]");
    CHECK(!strstr(body_of(&r), "{\"i\":0,")); /* (the film itself: joined, never sent) */
    for (int k = 0; k < 3; k++) {
        snprintf(url, sizeof url, "/media/upload/%s/%d?size=%d", batch, 1 + k, (int)piece[k].len);
        call("POST", url, adam, NULL, "", 0, 0, &r);
        CHECK_HAS(body_of(&r), "\"at\":0");
        snprintf(url, sizeof url, "/media/upload/%s/%d?at=0", batch, 1 + k);
        call("PUT", url, adam, NULL, piece[k].p, piece[k].len, piece[k].len, &r);
        CHECK_HAS(buf_cstr(&r), "200");
    }
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    upload_tick(now_ms()); /* (the join - the server's own ticks may have done it) */
    char copy[800], part[900];
    snprintf(copy, sizeof copy, "%s/Movies/R (2020)/R (2020).web.mp4", root);
    Buf got = {0};
    CHECK(file_read_all(copy, &got, 1 << 22) && got.len > 8 && !memcmp(got.p + 4, "ftyp", 4));
    buf_free(&got);
    for (int k = 0; k < 3; k++) {
        convert_piece_path(copy, k, part, sizeof part);
        CHECK(!path_stat(part, NULL, NULL, NULL)); /* (the parts went) */
    }
    CHECK(path_stat(src, NULL, NULL, NULL)); /* (the original stays: the box isn't ticked) */
    convert_test_device_cores(0);
    upload_test_part_min(0);
    for (int k = 0; k < 3; k++) buf_free(&piece[k]);
    buf_free(&r);
    th_halt();
}

/* (2.6) A "here" batch holds room for what's under way, not every copy's guess at once (a whole series from a device was
   refused on a disk with room for it, a copy at a time), and a copy whose place went meanwhile - its video removed or
   renamed on Manage - is skipped when it's ready instead of bringing the episode back */
TEST(upload_here_holds_what_is_under_way_and_skips_what_went) {
    th_boot();
    auth_configure("upload-salt", 1000);
    make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512], path[1100], rel[80];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    for (int e = 1; e <= 4; e++) {
        snprintf(rel, sizeof rel, "Series/R/Season 1/R.S01E%02d.web.mp4", e);
        conv_test_hevc_mp4(root, rel);
    }
    media_scan_sync();
    int64_t one = 0;
    snprintf(path, sizeof path, "%s/Series/R/Season 1/R.S01E01.web.mp4", root);
    CHECK(path_stat(path, &one, NULL, NULL) && one > 0);
    const MediaFile *e1 = NULL;
    for (int i = 0; i < media_lib()->nfiles; i++)
        if (str_eq(media_lib()->files[i].rel, "Series/R/Season 1/R.S01E01.web.mp4")) e1 = &media_lib()->files[i];
    CHECK(e1);
    if (!e1) {
        th_halt();
        return;
    }
    upload_test_free_space(MEDIA_RESERVE + 2 * one); /* room for a copy or so - not four at once */
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[160], json[200];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    snprintf(json, sizeof json, "{\"here\":\"series=%016llx\",\"conv\":\"1\"}", (unsigned long long)e1->series);
    plan_for(adam, json, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":0,\"f\":0},{\"i\":1,\"f\":1},{\"i\":2,\"f\":2},{\"i\":3,\"f\":3}]");

    /* E02 is removed meanwhile: its copy, ready, has no place - skipped, and nothing comes back */
    snprintf(path, sizeof path, "%s/Series/R/Season 1/R.S01E02.web.mp4", root);
    CHECK(file_remove(path));
    snprintf(url, sizeof url, "/media/upload/%s/1?size=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"skip\":true");
    CHECK(!path_stat(path, NULL, NULL, NULL));
    /* E01's goes as ever */
    snprintf(url, sizeof url, "/media/upload/%s/0?size=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    upload_test_free_space(-1);
    th_halt();
}

/* ------------------------------------------- picture subtitles */

/* A picture subtitle track's PUT, sent again while the server says "busy" (an earlier try still being written) */
static void put_pic(const char *cookie, const char *url, const void *body, size_t n, Buf *r) {
    for (int64_t end = now_ms() + 5000; now_ms() < end; plat_sleep_ms(20)) {
        call("PUT", url, cookie, NULL, body, n, n, r);
        if (!strstr(body_of(r), "busy")) return;
    }
}

TEST(upload_saves_picture_subtitles_next_to_the_copy) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[200], path[1024];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};

    /* the browser made x's copy (and couldn't make y's) */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"files\":[[\"S/x.mkv\",3000],[\"S/y.mkv\",20]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":0,\"f\":0},{\"i\":1,\"f\":1}]");
    snprintf(url, sizeof url, "/media/upload/%s/0?size=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, "copy-bytes", 10, 10, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");

    /* the file's tracks in its order: English text, then English pictures - the file's second English
       track, so ".2" (text and pictures are counted together, as the server's converter counts them) */
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nHi\n\n";
    snprintf(url, sizeof url, "/media/upload/%s/0?sub=1&lang=eng&name=English&forced=0&t=0", batch);
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    static const char sup[] = "PG\0\0\0\0\0\0\0\0\x80\0\0 pretend the rest is a Blu-ray track";
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=pgs&lang=eng&name=English&forced=0&t=1", batch);
    put_pic(adam, url, sup, sizeof sup - 1, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Up/S/x.en.srt", root);
    CHECK(same_file(path, srt, strlen(srt)));
    snprintf(path, sizeof path, "%s/Up/S/x.en.2.sup", root);
    CHECK(same_file(path, sup, sizeof sup - 1));
    put_pic(adam, url, sup, sizeof sup - 1, &r); /* sent again (its answer was lost): the same answer, nothing new */
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(!exists(root, "Up/S/x.en.3.sup"));

    /* a DVD VobSub: its .idx text first (idx= bytes), then its .sub - saved as the pair */
    static const char idx[] = "# VobSub index file, v7\nsize: 720x480\nid: sv, index: 0\n"
                              "timestamp: 00:00:01:000, filepos: 000000000\n";
    static const char pics[] = "\0\0\1\xBA pretend the rest is MPEG-PS";
    Buf both = {0};
    buf_put(&both, idx, sizeof idx - 1);
    buf_put(&both, pics, sizeof pics - 1);
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=vobsub&lang=swe&name=&forced=0&t=2&idx=%zu", batch, sizeof idx - 1);
    put_pic(adam, url, both.p, both.len, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Up/S/x.sv.idx", root);
    CHECK(same_file(path, idx, sizeof idx - 1));
    snprintf(path, sizeof path, "%s/Up/S/x.sv.sub", root);
    CHECK(same_file(path, pics, sizeof pics - 1));

    /* never over a file: the household's own .sup stays, and a VobSub whose .sub is taken saves neither half */
    put_file(root, "Up/S/x.de.sup", "the household's own", 19);
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=pgs&lang=ger&name=&forced=0&t=3", batch);
    put_pic(adam, url, sup, sizeof sup - 1, &r);
    CHECK_HAS(body_of(&r), "\"skip\":true");
    snprintf(path, sizeof path, "%s/Up/S/x.de.sup", root);
    CHECK(same_file(path, "the household's own", 19));
    put_file(root, "Up/S/x.fi.sub", "{1}{25}MicroDVD", 15);
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=vobsub&lang=fin&name=&forced=0&t=4&idx=%zu", batch, sizeof idx - 1);
    put_pic(adam, url, both.p, both.len, &r);
    CHECK_HAS(body_of(&r), "\"skip\":true");
    CHECK(!exists(root, "Up/S/x.fi.idx"));
    snprintf(path, sizeof path, "%s/Up/S/x.fi.sub", root);
    CHECK(same_file(path, "{1}{25}MicroDVD", 15));

    /* what isn't a picture track is refused, and leaves nothing behind */
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=pgs&lang=fre&name=&forced=0&t=5", batch);
    put_pic(adam, url, "not a .sup at all", 17, &r);
    CHECK_HAS(buf_cstr(&r), "415");
    CHECK_HAS(body_of(&r), "aren't subtitle pictures");
    CHECK(!exists(root, "Up/S/x.fr.sup"));
    CHECK(!exists(root, "Up/S/x.fr.sup.hemma-part"));

    /* a big track streams to the disk (past what one request may hold) ... */
    Buf big = {0};
    buf_put(&big, "PG", 2);
    for (int k = 0; k < 3 << 20; k += 4) buf_put(&big, "\x01\x02\x03\x04", 4);
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=pgs&lang=ita&name=&forced=0&t=6", batch);
    put_pic(adam, url, big.p, big.len, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Up/S/x.it.sup", root);
    CHECK(same_file(path, big.p, big.len));
    /* ... and one that broke off is sent again whole, under the same name */
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=pgs&lang=spa&name=&forced=0&t=7", batch);
    call("PUT", url, adam, NULL, big.p, big.len, 200000, &r);
    put_pic(adam, url, big.p, big.len, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Up/S/x.es.sup", root);
    CHECK(same_file(path, big.p, big.len));
    CHECK(!exists(root, "Up/S/x.es.2.sup"));
    CHECK(!exists(root, "Up/S/x.es.sup.hemma-part"));

    /* refused: an .idx longer than the body, a track past 64 MB, a file sent as it was (no copy made here) */
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=vobsub&lang=dan&name=&forced=0&t=8&idx=999", batch);
    put_pic(adam, url, pics, sizeof pics - 1, &r);
    CHECK_HAS(buf_cstr(&r), "400");
    sock_t s = dial();
    CHECK(s != SOCK_BAD);
    if (s != SOCK_BAD) {
        char head[400];
        snprintf(head, sizeof head,
                 "PUT /media/upload/%s/0?pic=pgs&lang=nor&t=9 HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\n"
                 "Content-Length: %lld\r\nConnection: close\r\n\r\n",
                 batch, adam, (long long)AV_CONV_PIC_BYTES + 1);
        send_all(s, head, strlen(head));
        read_reply(s, &r);
        sock_close(s);
    }
    CHECK_HAS(buf_cstr(&r), "413");
    CHECK_HAS(body_of(&r), "too big");
    snprintf(url, sizeof url, "/media/upload/%s/1?original=1", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    snprintf(url, sizeof url, "/media/upload/%s/1?pic=pgs&lang=eng&t=0", batch);
    put_pic(adam, url, sup, sizeof sup - 1, &r);
    CHECK_HAS(buf_cstr(&r), "404");
    CHECK(!exists(root, "Up/S/y.en.sup"));
    call("POST", "/a/media.upload_stop", adam, "Content-Type: application/x-www-form-urlencoded\r\n", "", 0, 0, &r);

    /* a VobSub the household drops in: the .idx and the .sub go together, the .sub first; a .sub alone
       is MicroDVD text - not taken */
    plan_for(adam,
             "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"D/y.mkv\",10],[\"D/y.en.idx\",5],[\"D/y.en.sub\",7],"
             "[\"D/z.sub\",3]]}",
             &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[2,0],[1,0]]");
    CHECK_HAS(body_of(&r), "\"skipped\":1");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "D/z.sub</code> — not a video, subtitle, poster or audiobook");
    /* both or neither: a different .sub has the name - the .idx stays behind too ... */
    put_file(root, "Up/E/w.en.sub", "someone else's", 14);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"E/w.mkv\",10],[\"E/w.en.idx\",5],[\"E/w.en.sub\",7]]}", &r,
             batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "E/w.en.idx</code> — goes with w.en.sub, which isn&#39;t sent");
    /* ... but the same .idx there already: the pictures complete the pair */
    put_file(root, "Up/F/v.en.idx", "12345", 5);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"F/v.mkv\",10],[\"F/v.en.idx\",5],[\"F/v.en.sub\",7]]}", &r,
             batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[2,0]]");
    /* and sent like any file */
    snprintf(url, sizeof url, "/media/upload/%s/2?at=0", batch);
    call("PUT", url, adam, NULL, "\0\0\1\xBA..", 7, 7, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(exists(root, "Up/F/v.en.sub"));
    CHECK_HAS(th_page(adam_id, "/media/upload"), ".sup,.idx,.sub,"); /* the file picker offers them */

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    buf_free(&both);
    buf_free(&big);
    th_halt();
}

/* ------------------------------------------- uploads and the server's converter */

static void convert_api(const char *cookie, const char *root, const char *rel, Buf *r) {
    char form[40], hex[17];
    media_id_hex(media_make_id(root, rel), hex);
    snprintf(form, sizeof form, "id=%s", hex);
    call("POST", "/api/media.convert", cookie, "Content-Type: application/x-www-form-urlencoded\r\n", form, strlen(form),
         strlen(form), r);
}

static bool queued(const char *root, const char *rel) { return convert_queued(media_make_id(root, rel)); }

/* The server's converter and an upload never write the same copy. A copy being converted right
   now stays the converter's; one only queued there is the upload's (the queue lets go of it -
   a set-aside or stopped conversion would otherwise leave nobody to make it). The converter in
   turn leaves alone what an upload brings, and a quiet upload that comes back after the
   converter started on its copy hears "sent". Each counts the room the other needs. */
TEST(upload_and_the_converter_never_write_the_same_copy) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    conv_test_rip(root, "Up/S/x.mkv", "matroska", 4000); /* a folder with its copy arrives */
    conv_test_rip(root, "Up/W/z.webm", "webm", 4000);    /* its copy z.web.mp4 is what a browser makes of z.mkv */
    conv_test_rip(root, "Up/V/v.mkv", "matroska", 4000); /* their copies come by upload */
    conv_test_rip(root, "Up/V/u.mkv", "matroska", 4000);
    conv_test_rip(root, "Up/C/c.webm", "webm", 4000); /* its copy is made by the uploading browser */
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaFile *x = media_file(media_make_id(root, "Up/S/x.mkv"));
    CHECK(x && convert_need(x) == CONV_TRANSCODE);
    upload_test_free_space(1LL << 40);
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[128], path[1100];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    const char *form = "Content-Type: application/x-www-form-urlencoded\r\n";

    /* 1. only queued on the server: the upload brings x.web.mp4 and the queue lets go of it -
       unless the plan is refused, then the conversion stays */
    convert_api(adam, root, "Up/S/x.mkv", &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(queued(root, "Up/S/x.mkv"));
    upload_test_free_space(MEDIA_RESERVE + 5);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"S/x.web.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "Not enough room");
    CHECK(queued(root, "Up/S/x.mkv"));
    upload_test_free_space(1LL << 40);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"S/x.mkv\",4000],[\"S/x.web.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[1,0]],\"skipped\":1"); /* (x.mkv is there) */
    CHECK(!queued(root, "Up/S/x.mkv"));
    call("POST", "/a/media.upload_stop", adam, form, "", 0, 0, &r);
    /* ... the same for a copy the uploading browser makes (z.webm here, z.mkv there: z.web.mp4) */
    convert_api(adam, root, "Up/W/z.webm", &r);
    CHECK(queued(root, "Up/W/z.webm"));
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"dupes\":\"1\",\"files\":[[\"W/z.mkv\",4000]]}", &r,
             batch);
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":0,\"f\":0}]");
    CHECK(!queued(root, "Up/W/z.webm"));
    call("POST", "/a/media.upload_stop", adam, form, "", 0, 0, &r);

    /* ... being converted: it stays the converter's, and its half-made file (its own name) is
       nobody's resume point */
    convert_api(adam, root, "Up/S/x.mkv", &r);
    CHECK(queued(root, "Up/S/x.mkv"));
    const char *running = convert_test_start();
    CHECK(running != NULL);
    put_file(root, "Up/S/x.web.mp4.conv.part", "half", 4);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"S/x.mkv\",4000],[\"S/x.web.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[],\"skipped\":2");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "S/x.web.mp4</code> — Hemma is making this copy on the server right now");
    snprintf(path, sizeof path, "%s/Up/S/x.web.mp4.conv.part", root);
    CHECK(same_file(path, "half", 4));
    CHECK(!exists(root, "Up/S/x.web.mp4.hemma-part"));
    convert_api(adam, root, "Up/W/z.webm", &r);
    CHECK(convert_test_start() != NULL);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"dupes\":\"1\",\"files\":[[\"W/z.mkv\",4000]]}", &r,
             batch);
    CHECK_HAS(body_of(&r), "\"todo\":[],\"skipped\":1");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "Hemma is making its play-everywhere copy on the server right now");

    /* 2. an upload first: the converter leaves the copies to it - files sent as they are, or a copy
       the browser is making */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"dupes\":\"1\",\"files\":[[\"V/v.web.mp4\",10],"
                   "[\"V/u.web.mp4\",10],[\"C/c.mkv\",4000]]}",
             &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[1,0],{\"i\":2,\"f\":2}]");
    convert_api(adam, root, "Up/V/v.mkv", &r);
    CHECK_HAS(body_of(&r), "An upload is bringing this video's play-everywhere copy right now");
    CHECK(!queued(root, "Up/V/v.mkv"));
    convert_api(adam, root, "Up/C/c.webm", &r);
    CHECK_HAS(body_of(&r), "An upload is bringing");
    CHECK(!queued(root, "Up/C/c.webm"));

    /* 3. that upload goes quiet (a laptop asleep) and the converter queues all three; it starts on
       u and c. Back again, the browser sends v (the queue lets go of it), and hears "sent" for u
       and c - nothing written twice, and u's half-sent part goes */
    g_fake_now = now_sec() + 120;
    convert_api(adam, root, "Up/V/u.mkv", &r);
    CHECK(convert_test_start() != NULL);
    convert_api(adam, root, "Up/C/c.webm", &r);
    CHECK(convert_test_start() != NULL);
    convert_api(adam, root, "Up/V/v.mkv", &r);
    CHECK(queued(root, "Up/V/v.mkv"));
    put_file(root, "Up/V/u.web.mp4.hemma-part", "01234", 5);
    snprintf(url, sizeof url, "/media/upload/%s/1?at=5", batch);
    call("PUT", url, adam, NULL, "56789", 5, 5, &r);
    CHECK_HAS(buf_cstr(&r), "200 OK");
    CHECK(!exists(root, "Up/V/u.web.mp4"));
    CHECK(!exists(root, "Up/V/u.web.mp4.hemma-part"));
    snprintf(url, sizeof url, "/media/upload/%s/1", batch);
    call("GET", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":10"); /* (a browser that lost the answer: done) */
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, "0123456789", 10, 10, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(url, sizeof url, "/media/upload/%s/0", batch);
    CHECK(resume_at(url, adam) == 10);
    snprintf(path, sizeof path, "%s/Up/V/v.web.mp4", root);
    CHECK(same_file(path, "0123456789", 10));
    CHECK(!queued(root, "Up/V/v.mkv"));
    snprintf(url, sizeof url, "/media/upload/%s/2?size=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"skip\":true");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "V/u.web.mp4</code> — Hemma started making this copy on the server meanwhile");
    g_fake_now = 0;
    call("POST", "/a/media.upload_stop", adam, form, "", 0, 0, &r);

    /* 4. room: a running conversion holds its original's size from uploads ... */
    convert_test_finish(false);
    upload_test_free_space(MEDIA_RESERVE + 6000);
    convert_api(adam, root, "Up/S/x.mkv", &r);
    CHECK(convert_test_start() != NULL);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"R/big.mp4\",3000]]}", &r, batch);
    CHECK_HAS(body_of(&r), "Not enough room: this needs 3 KB, and the drive has 2 KB to spare (Hemma keeps 1 GB free, and "
                           "4 KB for the uploads and conversions running now)");
    /* ... and an upload what it has left to send from conversions */
    convert_test_finish(false);
    upload_test_free_space(MEDIA_RESERVE + 5000);
    convert_api(adam, root, "Up/W/z.webm", &r);
    CHECK(queued(root, "Up/W/z.webm"));
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"R/big.mp4\",3000]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
    CHECK(convert_test_start() == NULL); /* a 4 KB original, 2 KB to spare past the upload */
    CHECK(!queued(root, "Up/W/z.webm"));
    const char *page = th_page(adam_id, "/media/settings/converting");
    CHECK_HAS(page, "1 video didn't fit on the drive");
    CHECK_HAS(page, "and 3 KB for the uploads and conversions running now");

    /* 5. a PUT checks the room too: what an upload held lapsed while it was quiet */
    upload_test_free_space(MEDIA_RESERVE + 1000);
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    char *body = xcalloc(1, 3000);
    call("PUT", url, adam, NULL, body, 3000, 3000, &r);
    free(body);
    CHECK_HAS(buf_cstr(&r), "507");
    CHECK_HAS(body_of(&r), "No room for the rest of this file: it needs 3 KB, and the drive has 1 KB to spare");
    CHECK_HAS(body_of(&r), "\"stop\":true");
    CHECK(!exists(root, "Up/R/big.mp4.hemma-part"));
    CHECK_HAS(th_page(adam_id, "/media/upload"), "R/big.mp4</code> — not enough room on the drive");
    upload_test_free_space(-1);

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* A copy's half-sent file from before this server started is never resumed from: the browser makes
   the copy again, maybe elsewhere - its bytes needn't match. Other files resume as ever. */
TEST(upload_never_resumes_a_copy_from_an_older_part) {
    g_fake_now = now_sec() + 600; /* this server "started" after the parts below were written */
    th_boot();
    auth_configure("upload-salt", 1000);
    make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    put_file(root, "Up/P/a.web.mp4.hemma-part", "AAAAA", 5);
    put_file(root, "Up/P/b.mp4.hemma-part", "BBBBB", 5);
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"P/a.web.mp4\",10],[\"P/b.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[1,5]]");
    CHECK(!exists(root, "Up/P/a.web.mp4.hemma-part"));
    CHECK(exists(root, "Up/P/b.mp4.hemma-part"));
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
    g_fake_now = 0;
}

TEST(upload_skips_what_the_library_has_under_other_names) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_file(root, "Series/Arthur/Season 1/Arthur - S01E01 - Arthur's Eyes.mkv", "a", 1);
    put_file(root, "Series/Arthur/Season 1/Arthur S01E03.web.mp4", "c", 1); /* only the copy */
    put_file(root, "Series/Arthur/Season 3/Arthur S03E01.mkv", "a", 1);
    put_file(root, "Series/Arthur/Season 3/Arthur S03E02.mkv", "b", 1);
    put_file(root, "Series/Arthur/Season 4/Arthur S04E05-E06.mkv", "d", 1);
    put_file(root, "Movies/Kill Bill (2003)/Kill Bill (2003) CD1.mkv", "k", 1);
    put_file(root, "Movies/Hocus Pocus (1993)/Hocus Pocus (1993).mkv", "h", 1);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK_INT(media_lib()->nseries, 1);
    CHECK_INT(media_lib()->nmovies, 2);
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};

    /* episodes: the same show, season and episode under another name stay behind - before converting */
    const char *eps = "{\"root\":\"0\",\"into\":\"Series\",\"conv\":\"1\",\"files\":["
                      "[\"Arthur/Season 1/Arthur S01E01 720p.mkv\",10],"           /* 0: have E01 */
                      "[\"Arthur/Season 1/Arthur - S01E01-E02 - Eyes + Hair.mkv\",10]," /* 1: E02 is new */
                      "[\"Arthur/Season 1/Arthur S01E03.mkv\",10],"                /* 2: its own copy is there */
                      "[\"Arthur/Season 2/Arthur S02E01.mkv\",10],"                /* 3: new */
                      "[\"Arthur/Season 3/Arthur S03E01E02.mkv\",10],"             /* 4: both are there */
                      "[\"Arthur/Season 4/Arthur S04E06.mkv\",10],"                /* 5: inside E05-E06 */
                      "[\"Arthur/Season 1/Arthur S01E01 720p.en.srt\",5],"         /* 6: goes with 0 */
                      "[\"Arthur/Season 2/Arthur S02E01.sv.srt\",5]]}";            /* 7: goes with 3 */
    plan_for(adam, eps, &r, batch);
    /* (1.3: what stays behind still gives its subtitles - 2, 4 and 5 are read for those inside them, 0's own
       subtitle file goes next to the library's episode instead, so 0 isn't read) */
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":1,\"f\":1},{\"i\":2,\"f\":2,\"subs\":1},{\"i\":3,\"f\":3},{\"i\":4,\"f\":4,"
                           "\"subs\":1},{\"i\":5,\"f\":5,\"subs\":1},[6,0],[7,0]],\"skipped\":4");
    const char *page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "the library has it: Season 1/Arthur - S01E01 - Arthur&#39;s Eyes.mkv");
    CHECK_HAS(page, "the library has it (Arthur S04E05-E06.mkv): only the subtitles inside it go, next to that one");
    CHECK_HAS(page, "its copy is there already: only the subtitles inside the original go");
    CHECK_HAS(page, "name=\"dupes\"");
    /* (what's sent shows no reason once under way: the preview says where 0's subtitle file goes) */
    Buf form = {0};
    buf_puts(&form, "root=0&into=Series&conv=1&files=");
    url_encode(&form, "[[\"Arthur/Season 1/Arthur S01E01 720p.mkv\",10],[\"Arthur/Season 1/Arthur S01E01 720p.en.srt\",5],"
                      "[\"Arthur/Season 4/Arthur S04E06.mkv\",10]]", false);
    upload_test_preview(adam_id, buf_cstr(&form));
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "goes next to Arthur - S01E01 - Arthur&#39;s Eyes.mkv, which the library has");
    CHECK_HAS(page, "Only its subtitles");
    buf_free(&form);

    /* films: the same title, year and part */
    const char *films = "{\"root\":\"0\",\"into\":\"Movies\",\"files\":["
                        "[\"Kill Bill (2003)/Kill Bill (2003) CD2.mkv\",10],"   /* 0: part 2 is new */
                        "[\"Kill.Bill.2003.CD1.1080p.mkv\",10],"                /* 1: have part 1 */
                        "[\"Hocus.Pocus.1993.1080p.BluRay.mkv\",10],"           /* 2: have it */
                        "[\"Hocus Pocus 2 (2022).mkv\",10],"                    /* 3: another film */
                        "[\"Hocus Pocus (1993)/Extras/Trailer.mkv\",10]]}";     /* 4: extras aren't checked */
    plan_for(adam, films, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[3,0],[4,0]],\"skipped\":2");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "the library has it: Hocus Pocus (1993)/Hocus Pocus (1993).mkv");

    /* an episode's part 2 is new too (1.4: "Dark Side Pt. 2" is S08E20, like the "Dark Side" there) */
    const char *parts = "{\"root\":\"0\",\"into\":\"Series\",\"files\":["
                        "[\"Arthur/Season 3/Arthur S03E01 Pt. 2.mkv\",10]," /* 0: new */
                        "[\"Arthur/Season 3/Arthur.S03E02.720p.mkv\",10]]}"; /* 1: have it */
    plan_for(adam, parts, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]],\"skipped\":1");

    /* the box sends them anyway */
    const char *anyway = "{\"root\":\"0\",\"into\":\"Movies\",\"dupes\":\"1\",\"files\":["
                         "[\"Kill.Bill.2003.CD1.1080p.mkv\",10],[\"Hocus.Pocus.1993.1080p.BluRay.mkv\",10]]}";
    plan_for(adam, anyway, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[1,0]],\"skipped\":0");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* 1.3: what stays behind still gives its subtitles - the browser reads them out of the original
   ({i, f, subs:1}) and they go next to the video the library has; a subtitle file goes there too */
TEST(upload_sends_the_subtitles_of_what_stays_behind) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_file(root, "Series/Arthur/Season 4/Arthur S04E05-E06.mkv", "d", 1);
    put_file(root, "Series/Arthur/Season 1/Arthur S01E03.web.mp4", "c", 1); /* only the copy */
    put_file(root, "Series/Arthur/Season 3/Arthur S03E01.mkv", "a", 1);
    put_file(root, "Series/Arthur/Season 3/Arthur S03E01.en.srt", "1\n", 2); /* has subtitles of its own */
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[200], path[1024];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};

    const char *eps = "{\"root\":\"0\",\"into\":\"Series\",\"conv\":\"1\",\"files\":["
                      "[\"Arthur/Season 4/Arthur S04E06.mkv\",10],"       /* 0: the library has it, without subtitles */
                      "[\"Arthur/Season 1/Arthur S01E03.mkv\",10],"       /* 1: its copy is there */
                      "[\"Arthur/Season 3/Arthur S03E01 720p.mkv\",10]]}"; /* 2: the library's has subtitles: not read */
    plan_for(adam, eps, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":0,\"f\":0,\"subs\":1},{\"i\":1,\"f\":1,\"subs\":1}],\"skipped\":3");
    CHECK(card_is(th_page(adam_id, "/media/upload"), "Uploading", "0 of 2 files"));

    /* the browser reads 0: its progress shows, then its tracks go next to the library's E05-E06 */
    snprintf(url, sizeof url, "/media/upload/%s/0?pm=400", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    const char *page = th_page(adam_id, "/media/upload");
    CHECK(card_row(page, batch, 0, "Subtitles")); /* (1.5: read for its subtitles, named as Hemma sees it) */
    CHECK_HAS(page, "<b>Arthur · S04E06</b><small>Arthur S04E06.mkv</small></span><span class=\"media-upc-num\">40%</span>");
    CHECK(card_is(page, "Processing", "0 of 2 files"));
    const char *srt = "1\n00:00:01,000 --> 00:00:02,000\nHi\n\n";
    snprintf(url, sizeof url, "/media/upload/%s/0?sub=1&lang=eng&name=&forced=0&t=0", batch);
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Series/Arthur/Season 4/Arthur S04E05-E06.en.srt", root);
    CHECK(same_file(path, srt, strlen(srt)));
    static const char sup[] = "PG\0\0\0\0\0\0\0\0\x80\0\0 pretend the rest is a Blu-ray track";
    snprintf(url, sizeof url, "/media/upload/%s/0?pic=pgs&lang=swe&name=&forced=0&t=1", batch);
    put_pic(adam, url, sup, sizeof sup - 1, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Series/Arthur/Season 4/Arthur S04E05-E06.sv.sup", root);
    CHECK(same_file(path, sup, sizeof sup - 1));
    /* 1's go next to its copy; 2 gives none */
    snprintf(url, sizeof url, "/media/upload/%s/1?sub=1&lang=swe&name=&forced=0&t=0", batch);
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", srt, strlen(srt), strlen(srt), &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(path, sizeof path, "%s/Series/Arthur/Season 1/Arthur S01E03.sv.srt", root);
    CHECK(same_file(path, srt, strlen(srt)));
    snprintf(url, sizeof url, "/media/upload/%s/2?sub=1&lang=swe&name=&forced=0&t=0", batch);
    call("POST", url, adam, "Content-Type: text/plain; charset=utf-8\r\n", srt, strlen(srt), strlen(srt), &r);
    CHECK(!strstr(body_of(&r), "\"ok\":true"));
    CHECK(!exists(root, "Series/Arthur/Season 3/Arthur S03E01.sv.srt"));
    /* done once the browser says so (1000) for each */
    CHECK(card_is(th_page(adam_id, "/media/upload"), "Processing", "0 of 2 files"));
    for (int i = 0; i < 2; i++) {
        snprintf(url, sizeof url, "/media/upload/%s/%d?pm=1000", batch, i);
        call("POST", url, adam, NULL, "", 0, 0, &r);
    }
    CHECK(card_is(th_page(adam_id, "/media/upload"), "Finished", "2 files sent"));

    /* a subtitle file alone: named for an episode the library has, it goes next to it - every tag kept */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Series\",\"files\":[[\"Arthur/Season 4/Arthur.S04E05-E06.720p.sv.forced.srt\",5]]}",
             &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, "1\nHi\n", 5, 5, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(exists(root, "Series/Arthur/Season 4/Arthur S04E05-E06.sv.forced.srt"));
    /* ... once: there now, the same again stays behind */
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Series\",\"files\":[[\"Arthur/Season 4/Arthur.S04E05-E06.720p.sv.forced.srt\",5]]}",
             &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[],\"skipped\":1");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "Arthur S04E05-E06.mkv has these subtitles already");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* Issue #89: "Remove them" asked upload_claims about the half-sent file's own name, which never
   matched an upload's, so an upload about to resume lost what it had sent - and a torrent's
   ".part" went with them. */
TEST(upload_keeps_a_half_sent_file_an_upload_is_about_to_resume) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);
    put_file(root, "Up/Q/c.mp4.hemma-part", "CCCCC", 5);  /* an earlier try, a week ago ... */
    put_file(root, "Up/Q/d.mp4.hemma-part", "DDDDD", 5);  /* ... and one nobody comes back for */
    put_file(root, "Up/Q/t.mkv.part", "a torrent", 9);    /* not Hemma's */
    g_fake_now = now_sec() + 8 * 86400;
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"Q/c.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,5]]"); /* it resumes from what it sent */
    media_scan_sync();
    CHECK_INT(media_lib()->nparts_all, 2);
    CHECK(th_act(adam_id, "media.parts_remove", "").ok);
    CHECK(exists(root, "Up/Q/c.mp4.hemma-part")); /* the upload is coming back for it */
    CHECK(!exists(root, "Up/Q/d.mp4.hemma-part"));
    CHECK(exists(root, "Up/Q/t.mkv.part"));
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
    g_fake_now = 0;
}

/* ------------------------------------------- the page: the preview, and Start doing what it said */

/* the page's fields for a preview: the options, then files = [[path, size], ...] as JSON (url-encoded) */
static void preview_of(int person, const char *opts, const char *files_json) {
    Buf form = {0};
    buf_printf(&form, "%s&all=0&files=", opts);
    url_encode(&form, files_json, false);
    upload_test_preview(person, buf_cstr(&form));
    buf_free(&form);
}

TEST(upload_page_previews_where_everything_goes_and_start_does_that) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    int eva_id = make_user("Eva", "eva", "pw-eva-1234", ROLE_MEMBER);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_file(root, "Series/Show/Season 01/Show.S01E01.mp4", "e1", 2);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK_INT(media_lib()->nseries, 1);

    /* the page before anything's chosen: one place to drop or pick, nothing else in the way */
    const char *page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "data-drop>");
    CHECK_HAS(page, "data-preview=\"media.upload_preview\"");
    CHECK_HAS(page, "<input type=\"hidden\" name=\"sort\" value=\"1\">");
    CHECK_HAS(page, "name=\"folder\" webkitdirectory");
    CHECK_HAS(page, "<details class=\"media-up-more\" id=\"media-up-more\"><summary>More options</summary>");
    CHECK(!strstr(page, "media-up-sum"));
    CHECK(!strstr(page, "<select name=\"root\">")); /* (one library folder: no choice to make) */
    CHECK_HAS(page, "id=\"media-up-force\" hidden"); /* nothing runs ... */
    Buf css = {0}; /* ... and hidden it stays: .check's display beat the attribute once */
    for (int i = 0; i < g_nassets; i++)
        if (str_eq(g_assets[i].path, "/app.css")) buf_put(&css, g_assets[i].data, g_assets[i].len);
    CHECK_HAS(buf_cstr(&css), ".media-up-form [hidden] { display: none !important; }");
    buf_free(&css);

    /* what Hemma makes of a few files: where each goes, what happens to it */
    const char *files = "[[\"Show.S01E02.Second.mp4\",5000],[\"Show.S01E02.Second.en.srt\",300],"
                        "[\"New/Season 2/New.S02E01.mkv\",4000],[\"Arrival (2016).mp4\",6000],[\"notes.txt\",10],"
                        "[\"Season 1/03 - Third.mp4\",7]]";
    preview_of(adam_id, "sort=1&root=0&conv=1", files);
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "<p class=\"media-up-sum\" data-say>5 files to send · ");
    CHECK_HAS(page, " · 1 to convert here first · 1 skipped</p>");
    CHECK_HAS(page, "<span>Series <i>/</i> Show <i>/</i> Season 01</span></h3>"); /* next to the old one: no "New folder" */
    CHECK_HAS(page, "<span>Series <i>/</i> New <i>/</i> Season 2</span><small class=\"badge\">New folder</small>");
    CHECK_HAS(page, "<span>Movies <i>/</i> Arrival (2016)</span>");
    CHECK_HAS(page, "1 episode, 1 subtitle file · ");
    CHECK_HAS(page, "<span class=\"media-up-name\">Show.S01E02.Second.mp4</span><small>Show · S01E02 · Second</small>");
    CHECK_HAS(page, "<small>Subtitles for Show · S01E02</small></div><span class=\"badge\">Sent as it is</span>");
    CHECK_HAS(page, "<small>New · S02E01</small></div><span class=\"badge ok\">Converted here first</span>");
    CHECK_HAS(page, "<span>Left out</span>");
    CHECK_HAS(page, "<small>not a video, subtitle, poster or audiobook</small></div>"
                    "<span class=\"badge warn\">Skipped</span>");
    /* an episode of no series it can name: asked for, and it goes as chosen until then */
    CHECK_HAS(page, "<details class=\"media-up-names\" id=\"media-up-names\" open><summary>Which series are these episodes from?");
    CHECK_HAS(page, "<span>1 episode Hemma can't place</span><input name=\"to.");
    CHECK_HAS(page, "<span>Season 1</span><small class=\"badge\">New folder</small></h3>");
    CHECK_HAS(page, "Hemma can&#39;t tell where these belong, so they go in as they are");
    CHECK_HAS(page, "<datalist id=\"media-up-shows\"><option value=\"Show\"></datalist>");
    CHECK_HAS(page, "data-drop-clear>Clear the list</button>");
    /* named: it goes with that series' others */
    char opts[200];
    snprintf(opts, sizeof opts, "sort=1&root=0&to.%016llx=show", (unsigned long long)upload_show_key("Unknown series"));
    preview_of(adam_id, opts, files);
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "<span class=\"media-up-name\">03 - Third.mp4</span><small>Show · S01E03 · Third</small>");
    CHECK(!strstr(page, "go in as they are"));
    /* keeping the folders' layout, under a folder of its own */
    preview_of(adam_id, "sort=1&keep=1&root=0&into=Up", files);
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "<span>Up <i>/</i> New <i>/</i> Season 2</span>");
    CHECK(!strstr(page, "media-up-names"));
    /* no room: said before anything is sent */
    upload_test_free_space((1LL << 30) + 100);
    preview_of(adam_id, "sort=1&root=0", files);
    CHECK_HAS(th_page(adam_id, "/media/upload"), "Not enough room: this needs ");
    upload_test_free_space(-1);
    /* a list that doesn't make sense, and too many files */
    upload_test_preview(adam_id, "sort=1&root=0&files=%5B%5Bbroken");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "That upload list didn&#39;t make sense");
    /* nothing chosen any more: no preview; and only the person who chose sees it */
    preview_of(adam_id, "sort=1&root=0", files);
    CHECK(!strstr(th_page(eva_id, "/media/upload"), "media-up-sum"));
    upload_test_preview(adam_id, NULL);
    CHECK(!strstr(th_page(adam_id, "/media/upload"), "media-up-sum"));
    CHECK(th_act(eva_id, "media.upload_preview", "files=%5B%5D").ok); /* (a member: nothing happens) */

    /* "Add episodes" from the series' page: into its folder, whatever the names say */
    char url[100], hex[17];
    media_id_hex(media_lib()->series[0].id, hex);
    snprintf(url, sizeof url, "/media/upload?series=%s", hex);
    page = th_page(adam_id, url);
    CHECK_HAS(page, "Everything goes into the folder of <b>Show</b>");
    CHECK_HAS(page, "<input type=\"hidden\" name=\"series\" value=\"");
    CHECK(!strstr(page, "name=\"keep\""));
    snprintf(opts, sizeof opts, "sort=1&series=%s", hex);
    preview_of(adam_id, opts, "[[\"Other.Name.S03E07.mp4\",9]]");
    CHECK_HAS(th_page(adam_id, url), "<span>Series <i>/</i> Show <i>/</i> Season 3</span>");

    /* Start: the plan puts every file where the preview said */
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    char plan[1200];
    snprintf(plan, sizeof plan, "{\"sort\":\"1\",\"root\":\"0\",\"to.%016llx\":\"Show\",\"files\":%s}",
             (unsigned long long)upload_show_key("Unknown series"), files);
    plan_for(adam, plan, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[1,0],[2,0],[3,0],[5,0]]");
    static const struct {
        int i;
        const char *rel;
    } want[] = {{0, "Series/Show/Season 01/Show.S01E02.Second.mp4"},
                {1, "Series/Show/Season 01/Show.S01E02.Second.en.srt"},
                {2, "Series/New/Season 2/New.S02E01.mkv"},
                {3, "Movies/Arrival (2016)/Arrival (2016).mp4"},
                {5, "Series/Show/Season 01/03 - Third.mp4"}};
    char *bytes = xcalloc(1, 6000);
    for (int k = 0; k < (int)countof(want); k++) {
        static const int sizes[] = {5000, 300, 4000, 6000, 10, 7};
        snprintf(url, sizeof url, "/media/upload/%s/%d?at=0", batch, want[k].i);
        call("PUT", url, adam, NULL, bytes, (size_t)sizes[want[k].i], (size_t)sizes[want[k].i], &r);
        CHECK_HAS(body_of(&r), "\"ok\":true");
        CHECK(exists(root, want[k].rel));
    }
    free(bytes);
    /* the same again: everything's there now, and the preview says so */
    snprintf(opts, sizeof opts, "sort=1&root=0&to.%016llx=Show", (unsigned long long)upload_show_key("Unknown series"));
    preview_of(adam_id, opts, files);
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "<small>Show · S01E02 · Second · it&#39;s already there</small>");
    CHECK_HAS(page, "0 files to send · 6 skipped</p>");
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* ------------------------------------------------------------------ audiobooks (1.6) */

TEST(upload_sends_audiobooks_into_the_books_folder) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    setting_set("media.roots", root);

    /* the preview: a book in parts with its cover, a book in one file, what's still no audiobook, and a show's
       theme song (it goes with the show) */
    const char *files = "[[\"Book/CD1/01.mp3\",3000],[\"Book/CD1/02.mp3\",2000],[\"Book/cover.jpg\",300],"
                        "[\"Loose.m4b\",2097152],[\"Loose.flac\",10],"
                        "[\"Garden Show/Season 1/Garden.Show.S01E01.mp4\",10],"
                        "[\"Garden Show/theme.mp3\",20]]";
    preview_of(adam_id, "sort=1&root=0&conv=1", files);
    const char *page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "<p class=\"media-up-sum\" data-say>6 files to send · ");
    CHECK(!strstr(page, "to convert here first")); /* (never converted - nor counted: the MP4 only if it needs it) */
    CHECK_HAS(page, "Converted here if needed"); /* (2.5: the episode, an MP4) */
    CHECK_HAS(page, "<span>Audiobooks <i>/</i> Book <i>/</i> CD1</span><small class=\"badge\">New folder</small>");
    CHECK_HAS(page, "2 audiobook files · ");
    CHECK_HAS(page, "<span class=\"media-up-name\">01.mp3</span><small>Part of an audiobook</small></div>"
                    "<span class=\"badge\">Sent as it is</span>");
    CHECK_HAS(page, "<span class=\"media-up-name\">cover.jpg</span><small>A picture for Book</small>");
    CHECK_HAS(page, "<span class=\"media-up-name\">Loose.m4b</span><small>An audiobook</small>");
    CHECK_HAS(page, "<span class=\"media-up-name\">Loose.flac</span>"
                    "<small>not a video, subtitle, poster or audiobook</small>");
    CHECK_HAS(page, ".webp,.m4b,.m4a,.mp3,audio/*\">");
    CHECK_HAS(page, "audiobooks into Audiobooks");
    CHECK_HAS(page, "<span>Series <i>/</i> Garden Show</span><small class=\"badge\">New folder</small></h3>"
                    "<p class=\"muted media-small\">1 audio file · ");
    CHECK_HAS(page, "<span class=\"media-up-name\">theme.mp3</span><small>Goes with Garden Show</small>");
    upload_test_preview(adam_id, NULL);

    /* Start: the plan (with the converter on: still plain entries) puts them where the preview said */
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], batch[17], url[100], plan[600];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    Buf r = {0};
    snprintf(plan, sizeof plan, "{\"sort\":\"1\",\"root\":\"0\",\"conv\":\"1\",\"files\":%s}", files);
    plan_for(adam, plan, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0],[1,0],[2,0],[3,0],{\"i\":5,\"f\":5},[6,0]]"); /* (2.5: the MP4 - converted if it needs it) */
    CHECK_HAS(body_of(&r), "\"skipped\":1");
    size_t m4b_n = 2u << 20; /* (bigger than a buffered body: it streams to disk) */
    char *m4b = xmalloc(m4b_n);
    for (size_t i = 0; i < m4b_n; i++) m4b[i] = (char)(i * 7 + (i >> 11));
    snprintf(url, sizeof url, "/media/upload/%s/3?at=0", batch);
    call("PUT", url, adam, NULL, m4b, m4b_n, m4b_n, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    char final[1024];
    snprintf(final, sizeof final, "%s/Audiobooks/Loose.m4b", root);
    CHECK(same_file(final, m4b, m4b_n));
    free(m4b);
    /* the card while the rest is to come: the file by its name, and below it where it went */
    page = th_page(adam_id, "/media/upload");
    CHECK(card_row(page, batch, 3, "Saved") != NULL);
    CHECK_HAS(page, "<b>Loose.m4b</b><small>Audiobooks</small>");
    char *mp3 = xcalloc(1, 3000);
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, mp3, 3000, 3000, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    free(mp3);
    CHECK(exists(root, "Audiobooks/Book/CD1/01.mp3"));
    snprintf(url, sizeof url, "/media/upload/%s/6?at=0", batch);
    call("PUT", url, adam, NULL, "theme song: 20 bytes", 20, 20, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(exists(root, "Series/Garden Show/theme.mp3"));
    CHECK(!exists(root, "Book"));
    /* the MP4 (2.5): its browser found it plays as it is - the original goes under its own name, nothing said */
    snprintf(url, sizeof url, "/media/upload/%s/5?original=1&plays=1", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    snprintf(url, sizeof url, "/media/upload/%s/5?at=0", batch);
    call("PUT", url, adam, NULL, "ten bytes!", 10, 10, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(exists(root, "Series/Garden Show/Season 1/Garden.Show.S01E01.mp4"));
    CHECK(!exists(root, "Series/Garden Show/Season 1/Garden.Show.S01E01.web.mp4"));
    page = th_page(adam_id, "/media/upload");
    CHECK(!strstr(page, "sent as it is"));
    CHECK(!strstr(page, "media-up-asis"));
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}
