/* Audiobooks with real files (1.6): what their tags make of a book - title, author, narrator, description,
 * chapters, a series - and the endpoints over real sockets: the picture inside a file (/media/cover/<id>), the
 * books' own files with their kind of sound, a book in parts as one .zip, and none of it for a child when its
 * folder isn't for children. The files are made by modules/av/audiotags_testmk.h. */
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../tests/harness.h"
#include "../av/audiotags_testmk.h"
#include "library.h"
#include "media.h"

static void put(const char *root, const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

/* not a real picture: a JPEG's first bytes, then a pattern */
static uint8_t g_cover[3000];
static void cover_bytes(void) {
    static const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0};
    memcpy(g_cover, head, sizeof head);
    for (size_t i = sizeof head; i < sizeof g_cover; i++) g_cover[i] = (uint8_t)(i * 7 + 3);
}

static const AtTestChapter STORY[] = {{0, "Opening Credits"}, {65000, "Chapter 1 - The Boy"}, {300000, "Chapter 2 - The Glass"}};

static void library(const char *root) {
    cover_bytes();
    Buf b = {0};
    at_test_m4b(&b, &(AtTestM4b){.title = "The First Story (Unabridged)", .artist = "Ann Author", .narrator = "Nora Narrator",
                                 .ldes = "A story about a boy\nwho lived.", .year = 2025, .cover = g_cover,
                                 .cover_len = sizeof g_cover, .chapters = STORY, .nchapters = 3, .moov_last = true,
                                 .duration_ms = 600000});
    put(root, "Stories/1 Story.m4b", b.p, b.len);
    buf_reset(&b);
    at_test_m4b(&b, &(AtTestM4b){.title = "The Second Story", .artist = "Ann Author", .duration_ms = 300000});
    put(root, "Stories/2 Story.m4b", b.p, b.len);
    static const char *const parts[] = {"One", "Two"};
    for (int i = 0; i < 2; i++) {
        buf_reset(&b);
        at_test_mp3(&b, &(AtTestMp3){.id3 = 3, .title = parts[i], .album = "A Tale", .artist = "Bo Writer", .track = i + 1,
                                     .frames = 200, .kbps = 128, .rate = 44100});
        char rel[80];
        snprintf(rel, sizeof rel, "Audiobooks/Tale/0%d.mp3", i + 1);
        put(root, rel, b.p, b.len);
    }
    buf_free(&b);
    setting_set("media.roots", root);
    media_scan_sync();
}

static const MediaBook *book_titled(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nbooks; i++)
        if (str_eq(ix->books[i].title, title)) return &ix->books[i];
    return NULL;
}

static bool holds(const char *p, size_t n, const char *what) { /* (memmem is no C standard) */
    size_t k = strlen(what);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(p + i, what, k)) return true;
    return false;
}

static const char *url(const char *fmt, uint64_t id) {
    static char u[200];
    char hex[17];
    media_id_hex(id, hex);
    snprintf(u, sizeof u, fmt, hex);
    return u;
}

TEST(books_read_their_tags_and_chapters) {
    th_boot();
    int adam = th_person("Adam");
    library(test_tmpdir());
    const MediaBook *one = book_titled("The First Story"), *two = book_titled("The Second Story"), *tale = book_titled("A Tale");
    CHECK(one && two && tale);
    if (!one || !two || !tale) {
        th_halt();
        return;
    }
    CHECK_STR(one->author, "Ann Author");
    CHECK_STR(one->narrator, "Nora Narrator");
    CHECK_INT(one->year, 2025);
    CHECK(one->duration_ms >= 599000 && one->duration_ms <= 601000);
    CHECK_STR(one->series, "Stories");
    CHECK_INT(one->number, 1);
    CHECK_INT(two->number, 2);
    CHECK(one->cover == one->id && !two->cover); /* (the picture inside the first) */
    CHECK_STR(tale->author, "Bo Writer");
    CHECK_INT(tale->count, 2);
    CHECK(tale->duration_ms > 9000); /* (200 frames of 1152 samples at 44.1 kHz each: 5.2 s) */
    const MediaTags *t = media_tags(one->id);
    CHECK(t && t->nch == 3 && t->ch[1].start_ms == 65000 && t->cover_len == (int64_t)sizeof g_cover);

    const char *html = th_page(adam, url("/media/book/%s", one->id));
    CHECK_HAS(html, "<h1>The First Story</h1>"); /* ("(Unabridged)" left off) */
    CHECK_HAS(html, "Ann Author · read by Nora Narrator · 2025 · 10 min");
    CHECK_HAS(html, "Stories · book 1");
    CHECK_HAS(html, "About this book");
    CHECK_HAS(html, "A story about a boy\nwho lived.");
    CHECK_HAS(html, "Chapter 1 - The Boy");
    CHECK_HAS(html, "data-at=\"65\"");
    CHECK_HAS(html, url("data-art=\"/media/cover/%s?v=", one->id));
    CHECK_HAS(html, url("<img src=\"/media/cover/%s?v=", one->id));
    html = th_page(adam, url("/media/book/%s", tale->id)); /* its parts are its chapters, named by their tags */
    CHECK_HAS(html, "<h1>A Tale</h1>");
    CHECK_HAS(html, ">One<");
    CHECK_HAS(html, ">Two<");
    CHECK_HAS(th_page(adam, "/media?q=nora"), "The First Story"); /* (the narrator is searched too) */
    th_halt();
}

/* LibriVox's files (the free audiobooks most people find first) carry titles like these */
TEST(books_titles_from_librivox_tags) {
    th_boot();
    const char *root = test_tmpdir();
    static const struct { const char *rel, *title, *album, *want; } CASES[] = {
        {"Oz/1 Wizard.m4b", "Wonderful Wizard of Oz ver 2", "The Wonderful Wizard of Oz ver 2", "Wonderful Wizard of Oz"},
        {"Oz/2 Land.m4b", "themarvelouslandofozver3_1501", "The Marvelous Land of Oz (version 3)", "The Marvelous Land of Oz"},
        {"Oz/3 Ozma.m4b", "Ozma of Oz v2", "Ozma of Oz version 2", "Ozma of Oz"},
        {"Audiobooks/Rabbit.m4b", "velveteen_rabbit", "The Velveteen Rabbit", "The Velveteen Rabbit"},
        {"Audiobooks/Some Story.m4b", "some_story_0911", NULL, "Some Story"}, /* (no album: the file's name) */
        {"Audiobooks/Solo.m4b", "A Tale (solo version 2) (Unabridged)", NULL, "A Tale"},
        /* what stays */
        {"Audiobooks/Apollo.m4b", "Apollo 13", NULL, "Apollo 13"},
        {"Audiobooks/Clover.m4b", "Clover 2", NULL, "Clover 2"},
        {"Audiobooks/Catch.m4b", "Catch-22", NULL, "Catch-22"},
        {"Audiobooks/V2.m4b", "v2", NULL, "v2"},
    };
    Buf b = {0};
    for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        buf_reset(&b);
        at_test_m4b(&b, &(AtTestM4b){.title = CASES[i].title, .album = CASES[i].album, .artist = "L. Frank Baum",
                                     .duration_ms = 60000});
        put(root, CASES[i].rel, b.p, b.len);
    }
    buf_free(&b);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK_INT(media_lib()->nbooks, (int)(sizeof CASES / sizeof *CASES));
    for (size_t i = 0; i < sizeof CASES / sizeof *CASES; i++) {
        const MediaBook *bk = book_titled(CASES[i].want);
        if (!bk) fprintf(stderr, "  no book titled \"%s\" (from \"%s\")\n", CASES[i].want, CASES[i].title);
        CHECK(bk != NULL);
    }
    const MediaBook *land = book_titled("The Marvelous Land of Oz");
    CHECK(land && str_eq(land->series, "Oz") && land->number == 2);
    th_halt();
}

/* ------------------------------------------------------------- over sockets */

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

static void exchange(const char *req, Buf *resp) {
    buf_reset(resp);
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, req, strlen(req));
    for (int64_t end = now_ms() + 10000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[16384];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(resp, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
}

static void get(const char *path, const char *cookie, const char *extra, Buf *resp) {
    char head[1024];
    snprintf(head, sizeof head, "GET %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\n%sConnection: close\r\n\r\n", path, cookie,
             extra ? extra : "");
    exchange(head, resp);
}

static const char *body_of(Buf *r, size_t *n) {
    const char *e = r->p ? strstr(buf_cstr(r), "\r\n\r\n") : NULL;
    *n = e ? r->len - (size_t)(e + 4 - r->p) : 0;
    return e ? e + 4 : "";
}

static void sign_in(const char *user, const char *pass, char *cookie, size_t cap) {
    char req[600], form[200];
    snprintf(form, sizeof form, "username=%s&password=%s&next=%%2F", user, pass);
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s",
             strlen(form), form);
    Buf r = {0};
    exchange(req, &r);
    const char *c = strstr(buf_cstr(&r), "Set-Cookie: hemma=");
    cookie[0] = '\0';
    if (c) str_ncopy(cookie, cap, c + 18, strcspn(c + 18, ";\r\n"));
    buf_free(&r);
}

static int person(const char *name, const char *user, const char *pass, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash(pass, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

TEST(books_serve_covers_files_and_zips) {
    th_boot();
    auth_configure("books-salt", 1000);
    person("Eva", "eva", "pw-eva-1234", ROLE_MEMBER);
    person("Leo", "leo", "pw-leo-1234", ROLE_CHILD);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    library(root);
    const MediaBook *one = book_titled("The First Story"), *tale = book_titled("A Tale");
    CHECK(one && tale);
    if (!one || !tale) {
        th_halt();
        return;
    }
    uint64_t one_id = one->id, tale_id = tale->id, part2 = media_book_part(tale, 1)->id;
    media_stream_resume(); /* (an earlier test's th_halt() ran the module's stop hook) */
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char eva[64], leo[64];
    sign_in("eva", "pw-eva-1234", eva, sizeof eva);
    sign_in("leo", "pw-leo-1234", leo, sizeof leo);
    CHECK(strlen(eva) == 32 && strlen(leo) == 32);
    Buf r = {0};
    size_t n = 0;
    const char *body;

    /* the picture inside the book: just its bytes */
    get(url("/media/cover/%s?v=1", one_id), eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "Content-Type: image/jpeg");
    body = body_of(&r, &n);
    CHECK(n == sizeof g_cover && memcmp(body, g_cover, n) == 0);
    get(url("/media/cover/%s", part2), eva, NULL, &r); /* (an MP3 part without one) */
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    /* the files, as sound, with ranges */
    get(url("/media/file/%s", one_id), eva, "Range: bytes=0-99\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 206 "));
    CHECK_HAS(buf_cstr(&r), "Content-Type: audio/mp4");
    body_of(&r, &n);
    CHECK_INT((int)n, 100);
    get(url("/media/file/%s", part2), eva, NULL, &r);
    CHECK_HAS(buf_cstr(&r), "Content-Type: audio/mpeg");
    /* a book in parts as one zip: "<title>/<each part>" */
    get(url("/media/zip/%s", tale_id), eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "application/zip");
    CHECK_HAS(buf_cstr(&r), "A Tale.zip");
    body = body_of(&r, &n);
    CHECK(holds(body, n, "A Tale/01.mp3") && holds(body, n, "A Tale/02.mp3"));
    /* a child: nothing of a book in a folder that isn't for children */
    get(url("/media/cover/%s?v=1", one_id), leo, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    media_kids_set(root, "Stories", true);
    get(url("/media/cover/%s?v=1", one_id), leo, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    get(url("/media/file/%s", one_id), leo, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    get(url("/media/zip/%s", tale_id), leo, NULL, &r); /* (downloads: grown-ups only, whatever the folder) */
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 403 "));
    get(url("/media/cover/%s?v=1", one_id), eva, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    for (int i = 0; i < 300 && media_streams_active(); i++) plat_sleep_ms(10);

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}
