/* Adding videos from a browser: names, the plan, streamed PUTs over real
 * sockets, resuming after a dropped connection, and what must be refused. */
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../tests/harness.h"
#include "upload.h"

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
    snprintf(part, sizeof part, "%s.part", final);
    CHECK(path_stat(part, NULL, NULL, NULL));  /* half a file waits as .part ... */
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
    snprintf(url, sizeof url, "/media/watch/%s", hfilm);
    CHECK_HAS(th_page(adam_id, url), "?pic=1");
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
    put_file(root, "Series/Show/poster.jpg.part", "OLD-PICTURE-HEAD", 16); /* another picture's leftover */
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
    CHECK(!exists(root, "Movies/Film (2020)/Film (2020).png.part"));

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
                       "[\"S/z.srt\",5],[\"S/w.mp4\",7]]}";
    plan_for(adam, plan, &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":0,\"f\":0},{\"i\":1,\"f\":1},[2,0],[3,0]]");
    /* without the box ticked (or a browser that can't) the originals go */
    const char *plain = "{\"root\":\"0\",\"into\":\"Up2\",\"files\":[[\"S/x.mkv\",3000]]}";
    char other[17];
    plan_for(adam, plain, &r, other);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
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
    CHECK_HAS(page, "0 of 2 play-everywhere copies ready");
    CHECK_HAS(page, "S/x.web.mp4</code></span><progress class=\"media-up-bar\" max=\"1000\" value=\"420\">");

    /* no room for the copy: refused, and the upload stops */
    upload_test_free_space((1LL << 30) + 5);
    snprintf(url, sizeof url, "/media/upload/%s/0?size=10", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(buf_cstr(&r), "507");
    CHECK_HAS(body_of(&r), "No room for the copy");
    upload_test_free_space(-1);

    /* the copy is ready: sized, then sent like any file, under the copy's name */
    put_file(root, "Up/S/x.web.mp4.part", "stale", 5); /* an earlier try's leftover starts over */
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    CHECK(!exists(root, "Up/S/x.web.mp4.part"));
    call("POST", url, adam, NULL, "", 0, 0, &r); /* asked again (a lost answer): same */
    CHECK_HAS(body_of(&r), "\"at\":0");
    snprintf(url, sizeof url, "/media/upload/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, "copy-bytes", 10, 10, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    char path[1024];
    snprintf(path, sizeof path, "%s/Up/S/x.web.mp4", root);
    CHECK(same_file(path, "copy-bytes", 10));
    CHECK(!exists(root, "Up/S/x.mkv")); /* the original never came */

    /* one the converter couldn't handle: the original goes under its own name */
    snprintf(url, sizeof url, "/media/upload/%s/1?original=1&why=Can%%27t+convert+V_MPEGH%%2FISO%%2FHEVC+video+yet", batch);
    call("POST", url, adam, NULL, "", 0, 0, &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    snprintf(url, sizeof url, "/media/upload/%s/1?at=0", batch);
    call("PUT", url, adam, NULL, "original-bytes-20xx!", 20, 20, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    CHECK(exists(root, "Up/S/y.mkv"));
    CHECK(!exists(root, "Up/S/y.web.mp4"));
    for (int i = 2; i <= 3; i++) {
        snprintf(url, sizeof url, "/media/upload/%s/%d?at=0", batch, i);
        call("PUT", url, adam, NULL, "1234567", i == 2 ? 5 : 7, i == 2 ? 5 : 7, &r);
        CHECK_HAS(body_of(&r), "\"ok\":true");
    }
    for (int64_t end = now_ms() + 3000; now_ms() < end; plat_sleep_ms(20)) { /* the worker finishes after it answers */
        page = th_page(adam_id, "/media/upload");
        if (strstr(page, "Finished")) break;
    }
    CHECK_HAS(page, "Finished · 4 files sent");
    CHECK_HAS(page, "1 sent unconverted");
    CHECK_HAS(page, "S/y.mkv</code> — sent as it is: Can&#39;t convert V_MPEGH/ISO/HEVC video yet");

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
    put_file(root, "Up/V/big.mp4.part", "0123456789ABCDEF", 16);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"V/big.mp4\",10]]}", &r, batch);
    CHECK_HAS(body_of(&r), "\"todo\":[[0,0]]");
    CHECK(!exists(root, "Up/V/big.mp4.part"));

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
    plan_for(eva, both, &r, evas);
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
    CHECK_HAS(body_of(&r), "\"todo\":[{\"i\":1,\"f\":1},{\"i\":3,\"f\":3},[7,0]],\"skipped\":5");
    const char *page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "the library has it: Season 1/Arthur - S01E01 - Arthur&#39;s Eyes.mkv");
    CHECK_HAS(page, "the library has it: Season 4/Arthur S04E05-E06.mkv");
    CHECK_HAS(page, "its play-everywhere copy is already there");
    CHECK_HAS(page, "goes with Arthur S01E01 720p.mkv, which the library has");
    CHECK_HAS(page, "name=\"dupes\"");

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
