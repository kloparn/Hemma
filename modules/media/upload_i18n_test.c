/* Uploads, conversions and USB disks in Swedish and Spanish (issue #46): the upload page and its
 * cards in their states, the "Play everywhere" card, the disk cards, the format dialog and their
 * checks on the System page - nothing left in English but names. Also: what the browser hears
 * (an upload's answers, a worker thread's too) comes in the uploader's language. */
#include "../../core/auth.h"
#include "../../core/codec.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../tests/harness.h"
#include "../av/mp4.h"
#include "convert.h"
#include "drives.h"
#include "library.h"
#include "media.h"
#include "room.h"
#include "upload.h"

void conv_test_rip(const char *root, const char *rel, const char *doctype, int64_t size); /* convert_test.c */

/* what reads the same in every language on the pages below: the subtitle languages Library settings
   offers (each in itself), its example folders and the services categories are looked up at (1.4) */
#define SAME_EVERYWHERE \
    "Svenska", "English", "Norsk", "Dansk", "Suomi", "Deutsch", "Français", "Español", "/mnt/media&#10;D:\\Movies", \
        "TVmaze", "OMDb", "omdbapi.com", "Wikipedia"

/* every url as person, in Swedish and Spanish; what reads as in English is printed */
static int untranslated(int person, const char *const *urls, size_t n, const char *const *allow) {
    Buf r = {0};
    int bad = 0;
    for (size_t i = 0; i < n; i++) bad += th_untranslated(person, urls[i], allow, &r);
    if (bad) printf("%s", buf_cstr(&r));
    buf_free(&r);
    return bad;
}

static void put_file(const char *root, const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

static int make_user(const char *name, const char *user, const char *pass, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    if (pass) auth_hash(pass, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* ------------------------------------------------- a tiny HTTP client (as upload_test.c's) */

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

static void call(const char *method, const char *path, const char *cookie, const char *extra, const void *body,
                 size_t body_n, Buf *resp) {
    char head[1024];
    snprintf(head, sizeof head,
             "%s %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
             method, path, cookie, extra ? extra : "", body_n);
    buf_reset(resp);
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, head, strlen(head));
    send_all(s, body, body_n);
    read_reply(s, resp);
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
    call("POST", "/login", "-", "Content-Type: application/x-www-form-urlencoded\r\n", form, strlen(form), &r);
    const char *c = strstr(buf_cstr(&r), "Set-Cookie: hemma=");
    cookie[0] = '\0';
    if (c) str_ncopy(cookie, cap, c + 18, strcspn(c + 18, ";\r\n"));
    buf_free(&r);
}

static void plan_for(const char *cookie, const char *json, Buf *r, char *batch) {
    call("POST", "/media/upload/plan", cookie, "Content-Type: application/json\r\n", json, strlen(json), r);
    const char *bp = strstr(body_of(r), "\"batch\":\"");
    batch[0] = '\0';
    if (bp) str_ncopy(batch, 17, bp + 9, 16);
}

static void post(const char *cookie, const char *batch, const char *rest, Buf *r) {
    char url[300];
    snprintf(url, sizeof url, "/media/upload/%s/%s", batch, rest);
    call("POST", url, cookie, NULL, "", 0, r);
}

static void put_bytes(const char *cookie, const char *batch, int i, const void *data, size_t n, Buf *r) {
    char url[200];
    snprintf(url, sizeof url, "/media/upload/%s/%d?at=0", batch, i);
    call("PUT", url, cookie, NULL, data, n, r);
}

/* ------------------------------------------------------------------- the upload page */

TEST(media_upload_pages_speak_swedish_and_spanish) {
    th_boot();
    auth_configure("upload-salt", 1000);
    int adam_id = make_user("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    int maja_id = make_user("Maja", "maja", "pw-maja-123", ROLE_ADMIN);
    int eva_id = make_user("Eva", "eva", "pw-eva-1234", ROLE_MEMBER);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    /* (names, their initials, the library folder) */
    const char *allow[] = {"Adam", "Maja", "Eva", "A", "M", "Show", "x.mkv", "w.avi", root, SAME_EVERYWHERE, NULL};

    /* no library folder yet; a member */
    static const char *const up[] = {"/media/upload"};
    CHECK_INT(untranslated(adam_id, up, 1, allow), 0);
    CHECK_INT(untranslated(eva_id, up, 1, allow), 0);

    /* a library with one episode, things already in the upload's folder */
    put_file(root, "Series/Show/Season 1/Show.S01E01.mp4", "e1", 2);
    put_file(root, "Up/S/there.avi", "1234", 4);
    put_file(root, "Up/S/other.avi", "12", 2);
    put_file(root, "Up/S/dir.avi/inside.txt", "x", 1);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK_INT(media_lib()->nseries, 1);
    char hshow[17];
    media_id_hex(media_lib()->series[0].id, hshow);

    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], maja[64], batch[17], art[17];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    sign_in("maja", "pw-maja-123", maja, sizeof maja);
    CHECK_INT(strlen(adam), 32);
    Buf r = {0};

    /* the browser hears its answers in the uploader's language - a worker thread's too */
    pref_set(adam_id, PREF_LANG, "sv");
    plan_for(adam, "{\"root\":\"0\",\"files\":[]}", &r, batch);
    CHECK_HAS(body_of(&r), "Inga filer valdes");
    char json[400];
    snprintf(json, sizeof json, "{\"art\":\"%s\",\"files\":[[\"p.png\",28]]}", hshow);
    plan_for(adam, json, &r, art);
    put_bytes(adam, art, 0, "this is not a picture at all", 28, &r);
    CHECK_HAS(buf_cstr(&r), "415");
    CHECK_HAS(body_of(&r), "Den filen är ingen JPEG-, PNG- eller WebP-bild"); /* (said on the upload's thread) */
    pref_set(adam_id, PREF_LANG, "es");
    plan_for(adam, "{\"root\":\"0\",\"files\":[]}", &r, batch);
    CHECK_HAS(body_of(&r), "No se eligió ningún archivo");
    pref_set(adam_id, PREF_LANG, "");

    /* a picture for the series: its dialog says it's saved */
    static const char png[] = "\x89PNG\r\n\x1a\n pretend the rest is a picture";
    snprintf(json, sizeof json, "{\"art\":\"%s\",\"files\":[[\"p.png\",%zu]]}", hshow, sizeof png - 1);
    plan_for(adam, json, &r, art);
    put_bytes(adam, art, 0, png, sizeof png - 1, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    char pic[100];
    snprintf(pic, sizeof pic, "/media/series/%s?pic=1", hshow);
    CHECK_HAS(th_page(adam_id, pic), "id=\"media-pic-st\"");
    const char *pics[] = {pic};
    CHECK_INT(untranslated(adam_id, pics, 1, allow), 0);

    /* an upload with every kind of reason, copies made by the browser, a play-all, one sent as it is */
    upload_test_free_space(1LL << 40);
    const char *plan =
        "{\"root\":\"0\",\"into\":\"Up\",\"conv\":\"1\",\"files\":["
        "[\"S/x.mkv\",3000],[\"S/y.mkv\",20],[\"S/notes.txt\",10],[\"../evil.mkv\",5],[\"S/Thumbs.db\",3],"
        "[\"S/w.avi\",7],[\"S/there.avi\",4],[\"S/other.avi\",9],[\"S/w.avi\",7],[\"S/big.avi\",3000],"  /* (plain sends: an MP4 is converted if it needs it - 2.5) */
        "[\"S/nosize.avi\",-1],[\"S/dir.avi\",5],[\"Show/Season 1/Show 1x01.mp4\",50],"
        "[\"Show/Season 1/Show 1x01.en.srt\",30],"
        "[\"D/Disc - 1/A1_t01.mkv\",943718400],[\"D/Disc - 1/A2_t02.mkv\",996147200],"
        "[\"D/Disc - 1/A3_t03.mkv\",1048576000],[\"D/Disc - 1/A4_t04.mkv\",1048576000],"
        "[\"D/Disc - 1/A7_t00.mkv\",4037017600]]}";
    plan_for(adam, plan, &r, batch);
    CHECK_INT(strlen(batch), 16);
    CHECK_HAS(body_of(&r), "[18,0,1048576]"); /* (the play-all: its header) */
    CHECK_HAS(body_of(&r), "[13,0]"); /* (1.3: the subtitles of the episode the library has go next to it) */
    post(adam, batch, "0?pm=420", &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    post(adam, batch, "1?original=1&why=Can%27t+convert+V_MPEGH%2FISO%2FHEVC+video+yet", &r);
    CHECK_HAS(body_of(&r), "\"at\":0");
    put_bytes(adam, batch, 5, "1234567", 7, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    upload_test_free_space(MEDIA_RESERVE + 1000); /* the big one: no room when it comes */
    char *big = xcalloc(1, 3000);
    put_bytes(adam, batch, 9, big, 3000, &r);
    free(big);
    CHECK_HAS(buf_cstr(&r), "507");
    upload_test_free_space(1LL << 40);
    const char *page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "1 video couldn't be converted on this device");
    CHECK_HAS(page, "the library has it: Season 1/Show.S01E01.mp4");
    CHECK_HAS(page, "1 play-all header");
    pref_set(adam_id, PREF_LANG, "sv");
    page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "biblioteket har den: Season 1/Show.S01E01.mp4"); /* (a reason kept in English, said in Swedish) */
    CHECK_HAS(page, "inte tillräckligt med plats på disken"); /* (a worker's reason) */
    CHECK_HAS(page, "skickades som den var: Can&#39;t convert V_MPEGH/ISO/HEVC video yet"); /* (the browser's words) */
    pref_set(adam_id, PREF_LANG, "");
    static const char *const pages[] = {"/media/upload", "/media/settings"};
    CHECK_INT(untranslated(adam_id, pages, 2, allow), 0);
    CHECK_INT(untranslated(maja_id, up, 1, allow), 0); /* (Adam's upload, as another admin sees it) */

    /* one at a time: Maja hears who is uploading, in her language */
    pref_set(maja_id, PREF_LANG, "es");
    plan_for(maja, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"M/m.mp4\",10]]}", &r, art);
    CHECK_HAS(body_of(&r), "La subida de Adam está en curso");
    pref_set(maja_id, PREF_LANG, "");

    /* stopped (the toast in Swedish), then a finished one */
    pref_set(adam_id, PREF_LANG, "sv");
    ThResult t = th_act(adam_id, "media.upload_stop", "");
    CHECK_HAS(t.toast, "Uppladdningen stoppad");
    pref_set(adam_id, PREF_LANG, "");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "</i>Stopped</span>");
    CHECK_INT(untranslated(adam_id, up, 1, allow), 0);
    plan_for(adam, "{\"root\":\"0\",\"into\":\"Up\",\"files\":[[\"F/one.mp4\",5]]}", &r, batch);
    put_bytes(adam, batch, 0, "12345", 5, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    for (int64_t end = now_ms() + 3000; now_ms() < end && !strstr(th_page(adam_id, "/media/upload"), "Finished");)
        plat_sleep_ms(20);
    CHECK_HAS(th_page(adam_id, "/media/upload"), "</i>Finished</span><span class=\"sr-only\"> · </span><span class=\"media-upc-count\">1 file sent</span>");
    CHECK_INT(untranslated(adam_id, up, 1, allow), 0);

    upload_test_free_space(-1);
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* --------------------------------------------- the upload page's preview (what Start will do) */

TEST(media_upload_preview_speaks_swedish_and_spanish) {
    th_boot();
    int adam_id = make_user("Adam", "adam", NULL, ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_file(root, "Series/Show/Season 1/Show.S01E01.mp4", "e1", 2);
    put_file(root, "Series/Show/Season 1/Show.S01E02.mp4", "e2", 2);
    setting_set("media.roots", root);
    media_scan_sync();
    char hex[17], url[100];
    media_id_hex(media_lib()->series[0].id, hex);
    snprintf(url, sizeof url, "/media/upload?series=%s", hex);
    /* every kind of file, and every fate: sent, converted here, skipped, a play-all's header, left out, an
       episode of no series, a disc rip, a film, an extra, subtitles, pictures, a copy beside its original,
       audiobooks (1.6: a book in parts, a book in one file) and a show's theme song */
    const char *files =
        "[[\"Show.S01E02.mp4\",2],[\"Show.S01E03.Third.mkv\",3000],[\"Show.S01E03.Third.en.srt\",30],"
        "[\"Show/poster.jpg\",40],[\"Show/Show.S01E04.mp4\",50],[\"Film (2001).mp4\",60],[\"Film (2001)-trailer.mp4\",7],"
        "[\"notes.txt\",3],[\"Season 2/01 - Unknown.mp4\",8],[\"Holiday/IMG_1.mov\",9],"
        "[\"Rip/Season 1/Disc - 1/A1_t01.mkv\",943718400],[\"Rip/Season 1/Disc - 1/A2_t02.mkv\",996147200],"
        "[\"Rip/Season 1/Disc - 1/A3_t03.mkv\",1048576000],[\"Rip/Season 1/Disc - 1/A4_t04.mkv\",1048576000],"
        "[\"Rip/Season 1/Disc - 1/A7_t00.mkv\",4037017600],[\"Sub only.srt\",5],"
        "[\"Book/01.mp3\",3],[\"Book/02.mp3\",3],[\"Lone.m4b\",4],[\"Show/theme.mp3\",2]]";
    Buf form = {0};
    buf_puts(&form, "sort=1&root=0&conv=1&originals=1&all=99&files=");
    url_encode(&form, files, false);
    upload_test_free_space(1LL << 40);
    upload_test_preview(adam_id, buf_cstr(&form));
    const char *page = th_page(adam_id, "/media/upload");
    CHECK_HAS(page, "This shows the first 20 of 99 files");
    CHECK_HAS(page, "Which series are these episodes from?");
    CHECK_HAS(page, "Rip · season 1, disc 1");
    CHECK_HAS(page, "An extra for Film");
    CHECK_HAS(page, "Just its first MB");
    CHECK_HAS(page, "the copy of Show.S01E03.Third.mkv, made on this device");
    CHECK_HAS(page, "Part of an audiobook");
    CHECK_HAS(page, "<small>An audiobook</small>");
    CHECK_HAS(page, "2 audiobook files");
    CHECK_HAS(page, "<small>Goes with Show</small>");
    CHECK_HAS(page, "1 audio file");
    /* (names, folders and file names are data; "Series"/"Movies"/"Audiobooks" are folder names) */
    static const char *const allow[] = {"Adam", "A", "Show", "Rip", "Film", "Season 1", "Season 2", "Holiday",
                                        "Series", "Movies", "Audiobooks", "Book", "01.mp3", "02.mp3", "Lone.m4b",
                                        "theme.mp3",
                                        "Disc - 1", "Film (2001)", "Show · S01E03 · Third",
                                        "Show · S01E02", "Show · S01E04", "Show · S01E03",
                                        /* the files' own names */
                                        "01 - Unknown.mp4", "A1_t01.mkv", "A1_t01.web.mp4", "A2_t02.mkv",
                                        "A2_t02.web.mp4", "A3_t03.mkv", "A3_t03.web.mp4", "A4_t04.mkv",
                                        "A4_t04.web.mp4", "A7_t00.mkv", "Film (2001).mp4", "Film (2001)-trailer.mp4",
                                        "IMG_1.mov", "notes.txt", "poster.jpg", "Show.S01E02.mp4",
                                        "Show.S01E03.Third.en.srt", "Show.S01E03.Third.mkv", "Show.S01E03.Third.web.mp4",
                                        "Show.S01E04.mp4", "Sub only.srt", "Other S02E01.mp4",
                                        /* (2.5: an MP4's copy, made only if its sound needs it) */
                                        "IMG_1.web.mp4", "Film (2001)-trailer.web.mp4", "Film (2001).web.mp4",
                                        "01 - Unknown.web.mp4", "Show.S01E04.web.mp4",
                                        "1 video", /* (Swedish too: "These are films" counts a folder's videos - 2.4) */
                                        NULL};
    static const char *const pages[] = {"/media/upload"};
    CHECK_INT(untranslated(adam_id, pages, 1, allow), 0);
    upload_test_free_space((1LL << 30) + 10); /* no room */
    upload_test_preview(adam_id, buf_cstr(&form));
    CHECK_HAS(th_page(adam_id, "/media/upload"), "Not enough room");
    CHECK_INT(untranslated(adam_id, pages, 1, allow), 0);
    upload_test_free_space(1LL << 40);
    /* a refused list */
    upload_test_preview(adam_id, "sort=1&root=0&files=%5B%5D");
    CHECK_HAS(th_page(adam_id, "/media/upload"), "No files were chosen");
    CHECK_INT(untranslated(adam_id, pages, 1, allow), 0);
    /* "Add episodes" on a series' page, with its preview */
    buf_reset(&form);
    buf_printf(&form, "sort=1&series=%s&files=", hex);
    url_encode(&form, "[[\"Other S02E01.mp4\",4]]", false);
    upload_test_preview(adam_id, buf_cstr(&form));
    const char *series[] = {url};
    CHECK_HAS(th_page(adam_id, url), "Season 2");
    CHECK_INT(untranslated(adam_id, series, 1, allow), 0);
    /* a DVD VobSub whose .sub can't go (another file has its name): its .idx stays behind too; a .sub alone */
    put_file(root, "Series/Show/Season 1/Show.S01E05.en.sub", "other", 5);
    buf_reset(&form);
    buf_puts(&form, "sort=1&root=0&files=");
    url_encode(&form, "[[\"Show.S01E05.mp4\",4],[\"Show.S01E05.en.idx\",5],[\"Show.S01E05.en.sub\",7],[\"lone.sub\",3]]", false);
    upload_test_preview(adam_id, buf_cstr(&form));
    CHECK_HAS(th_page(adam_id, "/media/upload"), "goes with Show.S01E05.en.sub, which isn&#39;t sent");
    static const char *const vob[] = {"Adam", "A", "Show", "Season 1", "Series", "Show · S01E05", "Show.S01E05.mp4",
                                      "Show.S01E05.en.idx", "Show.S01E05.en.sub", "lone.sub", NULL};
    CHECK_INT(untranslated(adam_id, pages, 1, vob), 0);
    upload_test_free_space(-1);
    upload_test_preview(adam_id, NULL);
    buf_free(&form);
    th_halt();
}

/* ------------------------------------------------------------------- converting */

static bool mp4_put(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)ctx) == n; }
static bool mp4_put_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    fflush(f);
    bool ok = file_seek(f, off) && fwrite(p, 1, n, f) == n;
    fflush(f);
    return fseek(f, 0, SEEK_END) == 0 && ok;
}

static void put_video(const char *root, const char *rel, bool hevc, int seconds) { /* (as media_test.c's) */
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    FILE *f = file_open(path, "wb");
    Mp4Out out = {f, mp4_put, mp4_put_at};
    Mp4 *m = mp4_new(&out, 0, 4096);
    static const uint8_t cfg[] = {1, 0x42, 0, 0x1E, 0xFF, 0xE0, 0};
    Mp4Track tr = {.codec = hevc ? MP4_HEVC : MP4_H264, .timescale = 1000, .config = cfg, .config_len = sizeof cfg,
                   .width = 64, .height = 48};
    mp4_add_track(m, &tr);
    mp4_begin(m);
    static const uint8_t frame[16] = {0, 0, 0, 12};
    mp4_sample(m, 0, frame, sizeof frame, (uint32_t)seconds * 1000, 0, true);
    mp4_finish(m);
    mp4_free(m);
    fclose(f);
}

TEST(media_convert_card_speaks_swedish_and_spanish) {
    th_boot();
    int adam = make_user("Adam", "adam", NULL, ROLE_ADMIN);
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    conv_test_rip(root, "Series/Show/Season 1/Show.S01E01.mkv", "matroska", 3000);
    conv_test_rip(root, "Series/Show/Season 1/Show.S01E02.mkv", "matroska", 3000);
    conv_test_rip(root, "Series/Show/Season 1/Show.S01E03.mkv", "matroska", 20000);
    put_video(root, "Movies/Big (1988)/Big (1988).mp4", true, 60); /* two versions of one film */
    put_video(root, "Movies/Big (1988)/Big (1988).web.mp4", false, 60);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK_INT(media_lib()->nepisodes, 3);
    static const char *const allow[] = {"Adam", "A", "Show S01E01", "Show S01E02", "Show S01E03", SAME_EVERYWHERE, NULL};
    static const char *const settings[] = {"/media/settings"};
    const char *page = th_page(adam, "/media/settings");
    CHECK_HAS(page, "Convert 3 videos");
    CHECK_HAS(page, "1 video has two versions");
    CHECK_INT(untranslated(adam, settings, 1, allow), 0);

    /* room for one small copy at a time: two queued, the big one set aside */
    media_test_free_space(MEDIA_RESERVE + 5000);
    pref_set(adam, PREF_LANG, "sv");
    ThResult t = th_act(adam, "media.convert", "");
    CHECK(t.ok);
    CHECK_HAS(t.toast, "Konverterar 2 videor, 1 åt gången - 1 till får inte plats på disken");
    pref_set(adam, PREF_LANG, "es");
    page = th_page(adam, "/media/settings");
    CHECK_HAS(page, "1 vídeo no cupo en el disco: Show S01E03 necesita hasta");
    pref_set(adam, PREF_LANG, "");
    CHECK_INT(untranslated(adam, settings, 1, allow), 0);

    /* one fails, one is done */
    CHECK(convert_test_start() != NULL);
    convert_test_finish(false);
    CHECK(convert_test_start() != NULL);
    convert_test_finish(true);
    page = th_page(adam, "/media/settings");
    CHECK_HAS(page, "1 converted, 1 failed");
    /* (+ the test's own reason for the failure: data, like the av library's English) */
    static const char *const failed[] = {"Adam", "A", "The", "test", "said", "so", "Show", SAME_EVERYWHERE, NULL};
    CHECK_INT(untranslated(adam, settings, 1, failed), 0);
    pref_set(adam, PREF_LANG, "sv");
    t = th_act(adam, "media.convert_stop", "");
    CHECK_HAS(t.toast, "Stoppar - färdiga videor finns kvar");
    pref_set(adam, PREF_LANG, "");
    media_test_free_space(-1);
    th_halt();
}

/* ------------------------------------------------------------------- USB disks */

typedef struct {
    char root[600], sys[700], udev[700], minfo[700], ask[700], ans[700], mnt[700];
} Machine;

static void put(const char *dir, const char *name, const char *text) {
    char path[900];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    CHECK(file_write_atomic(path, text, strlen(text)));
}

static void disk(Machine *m, const char *name, const char *dev, long long sectors, const char *udev) {
    char d[900], v[64];
    snprintf(d, sizeof d, "%s/%s", m->sys, name);
    CHECK(dir_make(d));
    put(d, "dev", dev);
    snprintf(v, sizeof v, "%lld\n", sectors);
    put(d, "size", v);
    snprintf(v, sizeof v, "b%s", dev);
    if (udev) put(m->udev, v, udev);
}

static void part(Machine *m, const char *disk_name, const char *name, const char *dev, const char *udev) {
    char d[900], v[64];
    snprintf(d, sizeof d, "%s/%s/%s", m->sys, disk_name, name);
    CHECK(dir_make(d));
    put(d, "dev", dev);
    put(d, "partition", "1\n");
    snprintf(v, sizeof v, "b%s", dev);
    if (udev) put(m->udev, v, udev);
}

#define USB "E:ID_BUS=usb\nE:ID_USB_DRIVER=usb-storage\n"
#define ROOT_MOUNTS "26 1 179-2 / / rw,noatime shared:1 - ext4 /dev/mmcblk0p2 rw\n"

/* a Pi's card, a new exFAT disk, an empty one and a Mac's (as drives_test.c's machine) */
static void machine(Machine *m) {
    str_copy(m->root, sizeof m->root, test_tmpdir());
    snprintf(m->sys, sizeof m->sys, "%s/sys", m->root);
    snprintf(m->udev, sizeof m->udev, "%s/udev", m->root);
    snprintf(m->minfo, sizeof m->minfo, "%s/mountinfo", m->root);
    snprintf(m->ask, sizeof m->ask, "%s/data-disks", m->root);
    snprintf(m->ans, sizeof m->ans, "%s/answers", m->root);
    snprintf(m->mnt, sizeof m->mnt, "%s/mnt/film-disk", m->root);
    CHECK(dir_make(m->sys));
    CHECK(dir_make(m->udev));
    CHECK(dir_make(m->ask));
    CHECK(dir_make(m->ans));
    CHECK(dir_make(m->mnt));
    disk(m, "mmcblk0", "179-0", 249737216, "E:ID_NAME=SD\n");
    part(m, "mmcblk0", "mmcblk0p2", "179-2", "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=a368\nE:ID_FS_LABEL=rootfs\n");
    disk(m, "sdb", "8-16", 1953525168LL, USB "E:ID_MODEL=Expansion_HDD\n");
    part(m, "sdb", "sdb1", "8-17",
         "E:ID_FS_TYPE=exfat\nE:ID_FS_UUID=5E2A-1B3C\nE:ID_FS_LABEL_ENC=Film\\x20disk\nE:ID_FS_LABEL=Film_disk\n");
    disk(m, "sdc", "8-32", 976773168LL, USB "E:ID_MODEL_ENC=Blank\\x20One\nE:ID_PART_TABLE_TYPE=gpt\n");
    part(m, "sdc", "sdc1", "8-33", "E:ID_PART_ENTRY_NUMBER=1\n");
    disk(m, "sdd", "8-48", 500118192LL, USB "E:ID_MODEL=Mac_Disk\n");
    part(m, "sdd", "sdd1", "8-49", "E:ID_FS_TYPE=apfs\nE:ID_FS_UUID=aaaa-bbbb\n");
    CHECK(file_write_atomic(m->minfo, ROOT_MOUNTS, strlen(ROOT_MOUNTS)));
}

static void look(Machine *m) { /* the helper there, everything looked at again now */
    media_test_drives(m->sys, m->udev, m->minfo);
    media_test_drive_dirs(m->ask, m->ans);
    media_drives_tick(now_ms());
}

TEST(media_disks_speak_swedish_and_spanish) {
    char data[512];
    str_copy(data, sizeof data, test_tmpdir());
    th_boot_in(data);
    int adam = make_user("Adam", "adam", NULL, ROLE_ADMIN);
    Machine m;
    machine(&m);
    look(&m);
    const char *allow[] = {"Adam",     "A",   "Film disk",     "Expansion HDD", "Blank One",
                           "Mac Disk", m.mnt, SAME_EVERYWHERE, NULL};
    static const char *const new_disks[] = {"/media/settings", "/media/settings?format=sdc", "/media", "/system"};
    CHECK_HAS(th_page(adam, "/media/settings"), "New disk found: Film disk");
    CHECK_HAS(th_page(adam, "/media"), "id=\"media-drive-offer\"");
    CHECK_INT(untranslated(adam, new_disks, countof(new_disks), allow), 0);

    /* the typed word stays ERASE; the toasts are in Adam's language */
    pref_set(adam, PREF_LANG, "sv");
    char form[200];
    snprintf(form, sizeof form, "disk=sdc&bytes=%lld&confirm=yes", 976773168LL * 512);
    ThResult t = th_act(adam, "media.drive_format", form);
    CHECK_STR(t.field, "confirm");
    CHECK_HAS(t.toast, "Skriv ERASE för att formatera den");
    t = th_act(adam, "media.drive_use", "uuid=5E2A-1B3C");
    CHECK(t.ok);
    CHECK_HAS(t.toast, "Ställer in \"Film disk\"…");
    pref_set(adam, PREF_LANG, "es");
    t = th_act(adam, "media.drive_use", "uuid=5E2A-1B3C");
    CHECK_HAS(t.toast, "El programa auxiliar de discos está ocupado");
    pref_set(adam, PREF_LANG, "");
    static const char *const settings[] = {"/media/settings"};
    CHECK_HAS(th_page(adam, "/media/settings"), "Setting up the disk");
    CHECK_INT(untranslated(adam, settings, 1, allow), 0);

    /* the helper did it: ready, mounted, in the library */
    char line[1200];
    snprintf(line, sizeof line, "5E2A-1B3C %s exfat Film disk\n", m.mnt);
    put(m.ans, "disks", line);
    snprintf(line, sizeof line, ROOT_MOUNTS "60 26 8-17 / %s rw - exfat /dev/sdb1 rw\n", m.mnt);
    CHECK(file_write_atomic(m.minfo, line, strlen(line)));
    snprintf(line, sizeof line, "used %lld 5E2A-1B3C %s\n", (long long)now_sec() + 1, m.mnt);
    put(m.ans, "result", line);
    look(&m);
    CHECK_HAS(th_page(adam, "/media/settings"), "<b>Film disk is ready.</b>");
    CHECK_HAS(th_page(adam, "/media/settings"), " free of ");
    CHECK_INT(untranslated(adam, settings, 1, allow), 0);
    const Activity *a = tbl_count(&T_activity) ? tbl_at(&T_activity, tbl_count(&T_activity) - 1) : NULL;
    CHECK(a && strstr(a->text, "Adam added the movie disk \"Film disk\"")); /* (Latest, in the reader's language) */
    if (a) {
        Buf said = {0};
        Lang was = i18n_use(LANG_SV);
        activity_text(a, &said);
        i18n_use(was);
        CHECK_HAS(buf_cstr(&said), "Adam lade till filmdisken \"Film disk\"");
        buf_free(&said);
    }

    /* refused (the helper's words are its own), then forgotten */
    snprintf(line, sizeof line, "refused %lld - - the disk is in use\n", (long long)now_sec() + 2);
    put(m.ans, "result", line);
    look(&m);
    CHECK_HAS(th_page(adam, "/media/settings"), "the disk is in use. Nothing on the disk was changed.");
    CHECK_INT(untranslated(adam, settings, 1, allow), 0);
    put(m.ans, "disks", "");
    snprintf(line, sizeof line, "forgotten %lld 5E2A-1B3C %s\n", (long long)now_sec() + 3, m.mnt);
    put(m.ans, "result", line);
    look(&m);
    CHECK_HAS(th_page(adam, "/media/settings"), "<b>Forgotten.</b>");
    CHECK_INT(untranslated(adam, settings, 1, allow), 0);

    /* one of Hemma's disks missing; the helper silent for a quarter of an hour (asked before a restart) */
    snprintf(line, sizeof line, "5E2A-1B3C %s exfat Film disk\n", m.mnt);
    put(m.ans, "disks", line);
    char d[800];
    snprintf(d, sizeof d, "%s/sdb/sdb1/dev", m.sys);
    CHECK(file_remove(d));
    snprintf(d, sizeof d, "%s/sdb/dev", m.sys);
    CHECK(file_remove(d));
    CHECK(file_write_atomic(m.minfo, ROOT_MOUNTS, strlen(ROOT_MOUNTS)));
    snprintf(d, sizeof d, "%s/result", m.ans);
    CHECK(file_remove(d));
    char asked[100];
    snprintf(asked, sizeof asked, "%lld %d use 5E2A-1B3C", (long long)now_sec() - 2000, adam);
    setting_set("media.drive_asked", asked);
    th_halt();
    th_boot_in(data);
    look(&m);
    const char *h = th_page(adam, "/media/settings");
    CHECK_HAS(h, "missing: plug it back in");
    CHECK_HAS(h, "The disk helper didn't answer");
    CHECK_HAS(th_page(adam, "/system"), "The movie disk &quot;Film disk&quot;");
    static const char *const gone[] = {"/media/settings", "/system"};
    CHECK_INT(untranslated(adam, gone, countof(gone), allow), 0);

    /* without the helper: how to get it */
    media_test_drive_dirs(NULL, NULL);
    media_drives_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings"), "needs its disk helper");
    CHECK_INT(untranslated(adam, settings, 1, allow), 0);
    th_halt();
}
