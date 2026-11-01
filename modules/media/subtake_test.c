/* The subtitles inside the library's MKVs, taken out by themselves (subtake.c): MKVs written here with a
 * text, a Blu-ray PGS and a DVD VobSub track (and with none), read once - whole only when there's
 * something to take - their tracks saved beside them as C1 names them, never read twice (a restart too),
 * read again once changed; giving way to someone watching, a conversion, a library folder that's away and
 * the switch; a file stopped half-way read again; the Library settings card in three languages.
 * Then the picture subtitles' endpoints over real sockets (C5): the cue list, each cue as a PNG (the same
 * bytes subpic + png_indexed make), the address key, 404s, the cache of opened files - and the player's
 * menu offering them with data-subpic. */
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../tests/harness.h"
#include "../av/subpic.h"
#include "download.h"
#include "library.h"
#include "media.h"
#include "room.h"
#include "subtake.h"

/* ------------------------------------------------------------ bytes */

static void b8(Buf *b, int v) { buf_putc(b, (char)(uint8_t)v); }
static void b16(Buf *b, unsigned v) {
    b8(b, (int)(v >> 8 & 255));
    b8(b, (int)(v & 255));
}
static void b24(Buf *b, uint32_t v) {
    b8(b, (int)(v >> 16 & 255));
    b16(b, v & 0xFFFF);
}

/* ------------------------------------------------------------ a small picture, as PGS and as VobSub */

#define PW 8
#define PH 4
static uint8_t pic_px(int x, int y) { return (uint8_t)(x < PW / 2 ? 1 : (y & 1) + 2); } /* values 1..3 */

static void pgs_seg(Buf *o, int type, const Buf *pay) {
    b8(o, type);
    b16(o, (unsigned)pay->len);
    if (pay->len) buf_put(o, pay->p, pay->len);
}

/* runs of one value (len < 64; a lone pixel as itself), each line ended by 0 0 */
static void pgs_rle(Buf *o) {
    for (int y = 0; y < PH; y++) {
        for (int x = 0; x < PW;) {
            int k = 1;
            while (x + k < PW && pic_px(x + k, y) == pic_px(x, y)) k++;
            if (k <= 2)
                for (int i = 0; i < k; i++) b8(o, pic_px(x, y));
            else b8(o, 0), b8(o, 0x80 | k), b8(o, pic_px(x, y));
            x += k;
        }
        b8(o, 0), b8(o, 0);
    }
}

/* One display set as a Blu-ray block holds it (segments, no "PG" headers): shown at (x, y) on 1920x1080, or a
   clear (show false) */
static void pgs_set(Buf *o, bool show, int x, int y) {
    Buf p = {0};
    b16(&p, 1920), b16(&p, 1080), b8(&p, 0x10), b16(&p, show ? 0 : 1), b8(&p, show ? 0x80 : 0), b8(&p, 0), b8(&p, 0);
    b8(&p, show);
    if (show) b16(&p, 0), b8(&p, 0), b8(&p, 0), b16(&p, (unsigned)x), b16(&p, (unsigned)y);
    pgs_seg(o, 0x16, &p);
    buf_reset(&p);
    b8(&p, 1), b8(&p, 0), b16(&p, 0), b16(&p, 0), b16(&p, 1920), b16(&p, 1080);
    pgs_seg(o, 0x17, &p);
    if (show) {
        static const int pal[4][5] = {{0, 16, 128, 128, 0}, {1, 235, 128, 128, 255}, {2, 16, 128, 128, 255}, {3, 81, 240, 90, 200}};
        buf_reset(&p);
        b8(&p, 0), b8(&p, 0);
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 5; k++) b8(&p, pal[i][k]);
        pgs_seg(o, 0x14, &p);
        Buf rle = {0};
        pgs_rle(&rle);
        buf_reset(&p);
        b16(&p, 0), b8(&p, 0), b8(&p, 0xC0), b24(&p, (uint32_t)rle.len + 4), b16(&p, PW), b16(&p, PH);
        buf_put(&p, rle.p, rle.len);
        buf_free(&rle);
        pgs_seg(o, 0x15, &p);
    }
    buf_reset(&p);
    pgs_seg(o, 0x80, &p);
    buf_free(&p);
}

/* DVD subpicture: nibble RLE per field, then its control sequence (colours, alpha, area, start; a stop) */
typedef struct {
    Buf *b;
    int cur;
    bool half;
} Nib;

static void nib(Nib *n, int v) {
    if (!n->half) n->cur = v << 4, n->half = true;
    else b8(n->b, n->cur | v), n->half = false;
}

static void vob_code(Nib *n, int len, int c) { /* len 1..63 */
    int v = len << 2 | c;
    if (len < 4) nib(n, v);
    else if (len < 16) nib(n, v >> 4), nib(n, v & 15);
    else nib(n, v >> 8), nib(n, v >> 4 & 15), nib(n, v & 15);
}

static void vob_field(Buf *o, int field) {
    Nib n = {o, 0, false};
    for (int y = field; y < PH; y += 2) {
        for (int x = 0; x < PW;) {
            int k = 1;
            while (x + k < PW && pic_px(x + k, y) == pic_px(x, y)) k++;
            vob_code(&n, k, pic_px(x, y));
            x += k;
        }
        if (n.half) nib(&n, 0);
    }
}

static void spu(Buf *o, int x, int y, int stop) {
    Buf top = {0}, bot = {0};
    vob_field(&top, 0), vob_field(&bot, 1);
    unsigned ctrl = 4 + (unsigned)top.len + (unsigned)bot.len, d2 = ctrl + 24, size = d2 + 6;
    b16(o, size), b16(o, ctrl);
    buf_put(o, top.p, top.len), buf_put(o, bot.p, bot.len);
    b16(o, 0), b16(o, d2);
    b8(o, 0x03), b8(o, 3 << 4 | 2), b8(o, 1 << 4 | 0); /* colours 0, 1, 2, 3 of the palette */
    b8(o, 0x04), b8(o, 15 << 4 | 15), b8(o, 15 << 4 | 0);
    int x2 = x + PW - 1, y2 = y + PH - 1;
    b8(o, 0x05), b8(o, x >> 4), b8(o, (x & 15) << 4 | x2 >> 8), b8(o, x2 & 255);
    b8(o, y >> 4), b8(o, (y & 15) << 4 | y2 >> 8), b8(o, y2 & 255);
    b8(o, 0x06), b16(o, 4), b16(o, 4 + (unsigned)top.len);
    b8(o, 0x01), b8(o, 0xFF);
    b16(o, (unsigned)stop), b16(o, d2), b8(o, 0x02), b8(o, 0xFF);
    buf_free(&top), buf_free(&bot);
}

static const char VOB_HEAD[] = "size: 720x480\npalette: 000000, ffffff, 808080, ff0000, 00ff00, 0000ff, 123456, abcdef, "
                               "102030, 405060, 708090, a0b0c0, d0e0f0, 111111, 222222, fedcba\n";

/* ------------------------------------------------------------ MKVs */

static void w_id(Buf *w, uint32_t id) {
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; i--) b8(w, (int)(id >> (8 * i)));
}
static void w_size8(Buf *w, uint64_t v) {
    b8(w, 0x01);
    for (int i = 6; i >= 0; i--) b8(w, (int)(v >> (8 * i)));
}
static size_t w_open(Buf *w, uint32_t id) {
    w_id(w, id);
    size_t at = w->len;
    w_size8(w, 0);
    return at;
}
static void w_close(Buf *w, size_t at) {
    uint64_t n = w->len - at - 8;
    for (int i = 1; i < 8; i++) w->p[at + i] = (char)(uint8_t)(n >> (8 * (7 - i)));
}
static void w_uint(Buf *w, uint32_t id, uint64_t v) {
    int n = 1;
    while (n < 8 && v >> (8 * n)) n++;
    w_id(w, id);
    b8(w, 0x80 | n);
    for (int i = n - 1; i >= 0; i--) b8(w, (int)(v >> (8 * i)));
}
static void w_bin(Buf *w, uint32_t id, const void *d, size_t n) {
    w_id(w, id);
    w_size8(w, n);
    buf_put(w, d, n);
}
static void w_float(Buf *w, uint32_t id, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    w_id(w, id);
    b8(w, 0x88);
    for (int i = 7; i >= 0; i--) b8(w, (int)(u >> (8 * i)));
}
static void w_block(Buf *w, int track, int ms, int dur_ms, const void *d, size_t n) { /* a BlockGroup */
    size_t g = w_open(w, 0xA0);
    w_id(w, 0xA1);
    w_size8(w, 4 + n);
    b8(w, 0x80 | track), b8(w, (ms >> 8) & 0xFF), b8(w, ms & 0xFF), b8(w, 0);
    buf_put(w, d, n);
    if (dur_ms) w_uint(w, 0x9B, (uint64_t)dur_ms);
    w_close(w, g);
}
static void w_simple(Buf *w, int track, int ms, const void *d, size_t n) {
    w_id(w, 0xA3);
    w_size8(w, 4 + n);
    b8(w, 0x80 | track), b8(w, (ms >> 8) & 0xFF), b8(w, ms & 0xFF), b8(w, 0x80);
    buf_put(w, d, n);
}

enum { S_NONE, S_TEXT, S_PGS, S_VOB };
typedef struct {
    int kind;
    const char *lang, *name;
} Track;

/* An MKV: an H.264 picture every second from 0 to 4 s (any bytes: nothing decodes them), Dolby sound when
   asked (a video the converter would take), and subtitle tracks: text "Hello there" at 0.5-1.5 s, a PGS
   picture at (100, 900) from 1 s, cleared at 3 s, a VobSub picture at (300, 400) at 2 s for 90 dates
   (1,024 ms). `extra` changes the text (a changed file). */
static void mkv(const char *path, const Track *t, int nt, bool ac3, const char *extra) {
    Buf w = {0};
    size_t e = w_open(&w, 0x1A45DFA3);
    w_uint(&w, 0x4286, 1);
    w_bin(&w, 0x4282, "matroska", 8);
    w_uint(&w, 0x4287, 4);
    w_close(&w, e);
    size_t seg = w_open(&w, 0x18538067), info = w_open(&w, 0x1549A966);
    w_uint(&w, 0x2AD7B1, 1000000);
    w_float(&w, 0x4489, 5000);
    w_close(&w, info);
    size_t tracks = w_open(&w, 0x1654AE6B), tr = w_open(&w, 0xAE);
    static const uint8_t avcc[] = {1, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0, 0};
    w_uint(&w, 0xD7, 1), w_uint(&w, 0x83, 1), w_bin(&w, 0x86, "V_MPEG4/ISO/AVC", 15), w_bin(&w, 0x63A2, avcc, sizeof avcc);
    size_t v = w_open(&w, 0xE0);
    w_uint(&w, 0xB0, 64), w_uint(&w, 0xBA, 48);
    w_close(&w, v);
    w_close(&w, tr);
    if (ac3) {
        tr = w_open(&w, 0xAE);
        w_uint(&w, 0xD7, 2), w_uint(&w, 0x83, 2), w_bin(&w, 0x86, "A_AC3", 5);
        size_t a = w_open(&w, 0xE1);
        w_float(&w, 0xB5, 48000.0), w_uint(&w, 0x9F, 2);
        w_close(&w, a);
        w_close(&w, tr);
    }
    for (int k = 0; k < nt; k++) {
        static const char *const codec[] = {"", "S_TEXT/UTF8", "S_HDMV/PGS", "S_VOBSUB"};
        tr = w_open(&w, 0xAE);
        w_uint(&w, 0xD7, (uint64_t)(3 + k)), w_uint(&w, 0x83, 0x11);
        w_bin(&w, 0x86, codec[t[k].kind], strlen(codec[t[k].kind]));
        if (t[k].lang) w_bin(&w, 0x22B59C, t[k].lang, strlen(t[k].lang));
        if (t[k].name) w_bin(&w, 0x536E, t[k].name, strlen(t[k].name));
        if (t[k].kind == S_VOB) w_bin(&w, 0x63A2, VOB_HEAD, sizeof VOB_HEAD - 1);
        w_close(&w, tr);
    }
    w_close(&w, tracks);
    size_t cl = w_open(&w, 0x1F43B675);
    w_uint(&w, 0xE7, 0);
    char text[100];
    snprintf(text, sizeof text, "Hello there%s", extra ? extra : "");
    Buf show = {0}, clear = {0}, sp = {0};
    pgs_set(&show, true, 100, 900);
    pgs_set(&clear, false, 0, 0);
    spu(&sp, 300, 400, 90);
    static char frame[40000]; /* (40 KB a picture: a file read whole takes more than one read) */
    for (size_t i = 0; i < sizeof frame; i++) frame[i] = (char)(i * 7);
    for (int ms = 0; ms <= 4000; ms += 500) {
        if (ms % 1000 == 0) w_simple(&w, 1, ms, frame, sizeof frame);
        for (int k = 0; k < nt; k++) {
            if (t[k].kind == S_TEXT && ms == 500) w_block(&w, 3 + k, 500, 1000, text, strlen(text));
            if (t[k].kind == S_PGS && ms == 1000) w_simple(&w, 3 + k, 1000, show.p, show.len);
            if (t[k].kind == S_PGS && ms == 3000) w_simple(&w, 3 + k, 3000, clear.p, clear.len);
            if (t[k].kind == S_VOB && ms == 2000) w_block(&w, 3 + k, 2000, 0, sp.p, sp.len);
        }
    }
    w_close(&w, cl);
    w_close(&w, seg);
    char dir[1024];
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, w.p, w.len);
    buf_free(&w), buf_free(&show), buf_free(&clear), buf_free(&sp);
}

static void put(const char *root, const char *rel, const char *text) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, text, strlen(text));
}

static bool there(const char *root, const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    return path_stat(path, NULL, NULL, NULL);
}

static void slurp(const char *root, const char *rel, Buf *out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    buf_reset(out);
    file_read_all(path, out, 64u << 20);
}

static const MediaFile *file_named(const char *suffix) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_ends(ix->files[i].rel, suffix)) return &ix->files[i];
    return NULL;
}

static int admin(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

static const Track ALL3[] = {{S_TEXT, "eng", "English"}, {S_PGS, "swe", NULL}, {S_VOB, "eng", NULL}};

/* the steps till there's nothing left (or it waits): how many files were looked at */
static int run_all(void) {
    int n = 0;
    for (int i = 0; i < 50 && subtake_test_step() == 1; i++) n++;
    return n;
}

/* ------------------------------------------------------------ the job */

TEST(subtake_reads_each_mkv_once_and_saves_its_tracks) {
    char data[512], root[512];
    str_copy(data, sizeof data, test_tmpdir());
    str_copy(root, sizeof root, test_tmpdir());
    th_boot_in(data);
    int adam = admin("Adam");
    char path[1024];
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E01.mkv", root);
    mkv(path, ALL3, 3, false, NULL);
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E02.mkv", root);
    mkv(path, NULL, 0, false, NULL); /* no subtitles */
    snprintf(path, sizeof path, "%s/Movies/Film (2001)/Film (2001).mkv", root);
    mkv(path, ALL3, 1, false, NULL);
    put(root, "Movies/Film (2001)/Film (2001).en.srt", "the household's own\n"); /* (its one track is beside it) */
    put(root, "Movies/Fake (2002)/Fake (2002).mkv", "not Matroska");
    put(root, "Movies/Clip (2003)/Clip (2003).mp4", "an MP4: nothing to take");
    setting_set("media.roots", root);
    media_scan_sync();

    /* the card while it's to do */
    subtake_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings/converting"), "Taking subtitles out of videos: 0 of 4 checked (waits while someone watches)");

    CHECK_INT(run_all(), 4);
    CHECK_INT(subtake_test_opens(), 4);
    CHECK_INT(subtake_test_reads(), 1); /* only the one with tracks to take: the rest by their headers */
    Buf b = {0};
    slurp(root, "Show/Season 1/Show.S01E01.en.srt", &b);
    CHECK_STR(buf_cstr(&b), "1\n00:00:00,500 --> 00:00:01,500\nHello there\n\n");
    slurp(root, "Show/Season 1/Show.S01E01.sv.sup", &b);
    CHECK(b.len > 13 && !memcmp(b.p, "PG", 2));
    CHECK(there(root, "Show/Season 1/Show.S01E01.en.2.idx") && there(root, "Show/Season 1/Show.S01E01.en.2.sub"));
    slurp(root, "Show/Season 1/Show.S01E01.en.2.idx", &b);
    CHECK_HAS(buf_cstr(&b), "size: 720x480\n");
    CHECK_HAS(buf_cstr(&b), "id: en, index: 0\ntimestamp: 00:00:02:000, filepos: 000000000\n");
    slurp(root, "Movies/Film (2001)/Film (2001).en.srt", &b);
    CHECK_STR(buf_cstr(&b), "the household's own\n"); /* never over a file there */
    CHECK(!there(root, "Show/Season 1/Show.S01E02.en.srt") && !there(root, "Movies/Fake (2002)/Fake (2002).en.srt"));

    /* the library rescanned when nothing was left: the new files are the video's subtitles */
    const MediaFile *e1 = file_named("Show.S01E01.mkv"), *sup = file_named("Show.S01E01.sv.sup"),
                    *idx = file_named("Show.S01E01.en.2.idx"), *srt = file_named("Show.S01E01.en.srt");
    CHECK(e1 && sup && idx && srt);
    if (e1 && sup && idx && srt) CHECK(sup->video == e1->id && idx->video == e1->id && srt->video == e1->id);
    subtake_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings/converting"), "Subtitles found in 2 videos");

    /* never read twice: not now, not after a restart */
    CHECK_INT(subtake_test_step(), 0);
    CHECK_INT(subtake_test_opens(), 4);
    th_halt();
    th_boot_in(data);
    CHECK_INT(subtake_test_step(), 0);
    CHECK_INT(subtake_test_opens(), 0);
    CHECK_INT(subtake_test_reads(), 0);

    /* a changed file is read again */
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E02.mkv", root);
    mkv(path, ALL3, 1, false, " again");
    media_scan_sync();
    CHECK_INT(run_all(), 1);
    CHECK_INT(subtake_test_reads(), 1);
    slurp(root, "Show/Season 1/Show.S01E02.en.srt", &b);
    CHECK_STR(buf_cstr(&b), "1\n00:00:00,500 --> 00:00:01,500\nHello there again\n\n");
    buf_free(&b);
    th_halt();
}

/* It never gets in the way: someone watching, a conversion queued, a library folder away (an unplugged disk)
   or the switch off - it waits, and goes on after. A file stopped half-way is read again from its start. */
TEST(subtake_gives_way_and_carries_on) {
    th_boot();
    int adam = admin("Adam");
    char root[512], root2[512], path[1024];
    str_copy(root, sizeof root, test_tmpdir());
    str_copy(root2, sizeof root2, test_tmpdir());
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E01.mkv", root);
    mkv(path, ALL3, 3, true, NULL); /* (H.264 + Dolby: the converter would take it) */
    snprintf(path, sizeof path, "%s/Film (2001)/Film (2001).mkv", root2);
    mkv(path, ALL3, 1, false, NULL);
    char roots[1100];
    snprintf(roots, sizeof roots, "%s\n%s", root, root2);
    setting_set("media.roots", roots);
    media_scan_sync();

    /* someone watching */
    media_download_test_pace(0, 1);
    CHECK_INT(subtake_test_step(), -1);
    subtake_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings/converting"), "0 of 2 checked - waiting while someone watches");
    media_download_test_pace(0, 0);

    /* a conversion queued (none runs in tests: it waits in the queue) */
    const MediaFile *e1 = file_named("Show.S01E01.mkv");
    CHECK(e1);
    char form[40], hex[17];
    media_id_hex(e1 ? e1->id : 0, hex);
    snprintf(form, sizeof form, "id=%s", hex);
    media_test_free_space(100LL << 30);
    CHECK(th_act(adam, "media.convert", form).ok);
    CHECK_INT(subtake_test_step(), -1);
    subtake_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings/converting"), "waiting for the conversions to finish");
    CHECK(th_act(adam, "media.convert_stop", "").ok);
    media_test_free_space(-1);

    /* switched off */
    CHECK(th_act(adam, "media.take_subs", "").ok);
    CHECK_INT(subtake_test_step(), -1);
    const char *html = th_page(adam, "/media/settings/converting");
    CHECK_HAS(html, "Switched off - 0 of 2 videos checked so far.");
    CHECK(!strstr(html, "data-auto checked> Take subtitles out of videos by themselves"));
    CHECK(th_act(adam, "media.take_subs", "on=1").ok);
    CHECK_HAS(th_page(adam, "/media/settings/converting"), "data-auto checked> Take subtitles out of videos by themselves");
    CHECK_INT(subtake_test_reads(), 0);

    /* a library folder away: nothing is read, from any folder */
    snprintf(path, sizeof path, "%s/Film (2001)/Film (2001).mkv", root2);
    Buf film = {0};
    file_read_all(path, &film, 1u << 20);
    file_remove(path);
    media_scan_sync();
    CHECK_INT(subtake_test_step(), -1);
    subtake_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings/converting"), "waiting for a library folder that's missing");
    file_write_atomic(path, film.p, film.len);
    buf_free(&film);
    media_scan_sync();
    CHECK_INT(subtake_test_opens(), 0);

    /* stopped half-way (as when someone starts watching): nothing saved, nothing done - read again, whole */
    subtake_test_halt_after(3);
    int n = 0;
    for (int i = 0; i < 4; i++) n += subtake_test_step() == 1;
    CHECK_INT(n, 4);
    CHECK_INT(subtake_test_reads(), 4); /* (the same file each time: it's never done) */
    CHECK(!there(root, "Show/Season 1/Show.S01E01.en.srt") && !there(root2, "Film (2001)/Film (2001).en.srt"));
    subtake_tick(now_ms());
    CHECK_HAS(th_page(adam, "/media/settings/converting"), "0 of 2 checked");
    subtake_test_halt_after(0);
    CHECK_INT(run_all(), 2);
    CHECK_INT(subtake_test_reads(), 6);
    CHECK(there(root, "Show/Season 1/Show.S01E01.en.srt") && there(root, "Show/Season 1/Show.S01E01.en.2.sub"));
    CHECK(there(root2, "Film (2001)/Film (2001).en.srt"));
    th_halt();
}

TEST(subtake_card_speaks_swedish_and_spanish) {
    th_boot();
    int anna = admin("Anna");
    char root[512], path[1024];
    str_copy(root, sizeof root, test_tmpdir());
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E01.mkv", root);
    mkv(path, ALL3, 1, false, NULL);
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E02.mkv", root);
    mkv(path, NULL, 0, false, NULL);
    setting_set("media.roots", root);
    media_scan_sync();
    /* (data: names, the library folder; the subtitle languages, each in its own words; the services categories are
       looked up at) */
    const char *const allow[] = {"Anna", "A",     "Show",  "MKV",   root,       "Svenska", "English",
                                 "Norsk", "Dansk", "Suomi", "Deutsch", "Français", "Español", "TVmaze", "OMDb", "Wikipedia",
                                 "omdbapi.com", NULL};
    Buf r = {0};
    int bad = 0;
    subtake_tick(now_ms());
    bad += th_untranslated(anna, "/media/settings/converting", allow, &r); /* to do */
    media_download_test_pace(0, 1);
    subtake_tick(now_ms());
    bad += th_untranslated(anna, "/media/settings/converting", allow, &r); /* waiting */
    media_download_test_pace(0, 0);
    CHECK_INT(subtake_test_step(), 1);
    bad += th_untranslated(anna, "/media/settings/converting", allow, &r); /* half-way, found so far */
    CHECK_INT(run_all(), 1);
    bad += th_untranslated(anna, "/media/settings/converting", allow, &r); /* done */
    CHECK(th_act(anna, "media.take_subs", "").ok);
    bad += th_untranslated(anna, "/media/settings/converting", allow, &r); /* switched off */
    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    pref_set(anna, PREF_LANG, "sv");
    CHECK_HAS(th_page(anna, "/media/settings/converting"), "Undertexter hittade i 1 video");
    ThResult on = th_act(anna, "media.take_subs", "on=1");
    CHECK_STR(on.toast, "Hemma tar ut undertexterna ur videor av sig själv");
    pref_set(anna, PREF_LANG, "es");
    CHECK_HAS(th_page(anna, "/media/settings/converting"), "Subtítulos encontrados en 1 vídeo");
    /* members see none of it */
    int eva = th_person("Eva");
    CHECK(!strstr(th_page(eva, "/media/settings/converting"), "media-subtake"));
    CHECK(!th_act(eva, "media.take_subs", "on=1").ok);
    buf_free(&r);
    th_halt();
}

/* ------------------------------------------------------------ the endpoints, over sockets */

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

static void get(const char *method, const char *path, const char *cookie, Buf *resp) {
    char head[1024];
    snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", method,
             path, cookie);
    buf_reset(resp);
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, head, strlen(head));
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
    cookie[0] = '\0';
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, req, strlen(req));
    Buf r = {0};
    for (int64_t end = now_ms() + 5000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[4096];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(&r, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
    const char *c = strstr(buf_cstr(&r), "Set-Cookie: hemma=");
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

static bool mem_read(void *ctx, int64_t off, void *dst, size_t n) {
    const Buf *b = ctx;
    if (off < 0 || (size_t)off > b->len || n > b->len - (size_t)off) return false;
    memcpy(dst, b->p + off, n);
    return true;
}

/* What the endpoints must answer, made here from the files: the cue list, and cue n as a PNG */
static SubPic *open_here(const char *root, const char *rel, Buf *data, Buf *idx) {
    char err[100];
    if (str_ends(rel, ".sup")) {
        slurp(root, rel, data);
        MkvReader r = {data, (int64_t)data->len, mem_read};
        return subpic_open_sup(&r, err, sizeof err);
    }
    char sub[400];
    str_copy(sub, sizeof sub, rel);
    memcpy(sub + strlen(sub) - 3, "sub", 3);
    slurp(root, rel, idx);
    slurp(root, sub, data);
    MkvReader r = {data, (int64_t)data->len, mem_read};
    return subpic_open_vobsub(idx->p, idx->len, &r, err, sizeof err);
}

static bool png_is(SubPic *s, int n, const char *body, size_t len) {
    const SubPicCue *c = subpic_cue(s, n);
    if (!c) return false;
    uint8_t *px = xmalloc((size_t)c->w * (size_t)c->h);
    uint32_t pal[256];
    int nc = 0;
    size_t plen = 0;
    uint8_t *png = subpic_render(s, n, px, (size_t)c->w * (size_t)c->h, pal, &nc) ? png_indexed(px, c->w, c->h, pal, nc, &plen) : NULL;
    bool same = png && plen == len && !memcmp(png, body, len);
    bool drawn = true; /* (and the picture is the one written: values 1..3 where they were put) */
    for (int y = 0; png && y < PH && y < c->h; y++)
        for (int x = 0; x < PW && x < c->w; x++) drawn &= px[y * c->w + x] == pic_px(x, y);
    free(px);
    free(png);
    return same && drawn;
}

static void url_of(const char *html, const char *hex, char *out, size_t cap) { /* the data-subpic address of a file */
    out[0] = '\0';
    char want[80];
    snprintf(want, sizeof want, "data-subpic=\"/media/subpic/%s.json?", hex);
    const char *at = strstr(html, want);
    if (!at) return;
    at += strlen("data-subpic=\"");
    size_t n = strcspn(at, "\"");
    Buf u = {0};
    for (size_t i = 0; i < n; i++) {
        if (!strncmp(at + i, "&amp;", 5)) buf_putc(&u, '&'), i += 4;
        else buf_putc(&u, at[i]);
    }
    str_copy(out, cap, buf_cstr(&u));
    buf_free(&u);
}

TEST(subtake_picture_subtitles_go_out_as_cues_and_pngs) {
    th_boot();
    auth_configure("subpic-salt", 1000);
    int adam = person("Adam", "adam", "pw-adam-123", ROLE_ADMIN);
    person("Leo", "leo", "pw-leo-1234", ROLE_CHILD);
    char root[512], path[1024];
    str_copy(root, sizeof root, test_tmpdir());
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E01.mkv", root);
    mkv(path, ALL3, 3, false, NULL);
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK_INT(run_all(), 1); /* (the job takes them out; the library rescans) */
    const MediaFile *e1 = file_named("Show.S01E01.mkv"), *sup = file_named("Show.S01E01.sv.sup"),
                    *idx = file_named("Show.S01E01.en.2.idx"), *sub = file_named("Show.S01E01.en.2.sub"),
                    *srt = file_named("Show.S01E01.en.srt");
    CHECK(e1 && sup && idx && sub && srt);
    if (!e1 || !sup || !idx || !sub || !srt) {
        th_halt();
        return;
    }
    char hv[17], hsup[17], hidx[17], hsub[17], hsrt[17], u[300], u2[300];
    media_id_hex(e1->id, hv), media_id_hex(sup->id, hsup), media_id_hex(idx->id, hidx), media_id_hex(sub->id, hsub);
    media_id_hex(srt->id, hsrt);
    uint64_t sup_id = sup->id;

    /* the menu (no stored session in the harness: plain addresses) - text and pictures in one list */
    char want[300];
    snprintf(u, sizeof u, "/media/watch/%s", hv);
    const char *html = th_page(adam, u);
    snprintf(want, sizeof want, "data-sub=\"en.2\" data-subpic=\"/media/subpic/%s.json?v=%lld-%lld\" aria-checked=\"false\">English 2</button>",
             hidx, (long long)idx->size, (long long)idx->mtime);
    CHECK_HAS(html, want);
    snprintf(want, sizeof want, "data-sub=\"sv\" data-subpic=\"/media/subpic/%s.json?v=", hsup);
    CHECK_HAS(html, want);
    CHECK_HAS(html, "data-sub=\"en\" aria-checked=\"false\">English</button>");
    CHECK(!strstr(html, hsub)); /* (the VobSub's pictures: through its .idx only) */
    int tracks = 0;
    for (const char *p = html; (p = strstr(p, "<track ")) != NULL; p++) tracks++;
    CHECK_INT(tracks, 1); /* (pictures have no <track>) */

    media_stream_resume();
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char ck[64], leo[64];
    sign_in("adam", "pw-adam-123", ck, sizeof ck);
    sign_in("leo", "pw-leo-1234", leo, sizeof leo);
    CHECK(strlen(ck) == 32 && strlen(leo) == 32);
    Buf r = {0}, data = {0}, ix = {0};
    size_t n = 0;
    const char *body;

    /* the cue lists */
    SubPic *ps = open_here(root, sup->rel, &data, &ix);
    CHECK(ps);
    snprintf(u, sizeof u, "/media/subpic/%s.json", hsup);
    get("GET", u, ck, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "Content-Type: application/json\r\n");
    CHECK_HAS(buf_cstr(&r), "Cache-Control: private, max-age=86400\r\n");
    body = body_of(&r, &n);
    CHECK_STR(body, "{\"w\":1920,\"h\":1080,\"cues\":[[1000,3000,100,900,8,4]]}");
    snprintf(u, sizeof u, "/media/subpic/%s/0.png", hsup);
    get("GET", u, ck, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "Content-Type: image/png\r\n");
    body = body_of(&r, &n);
    CHECK(ps && png_is(ps, 0, body, n));
    if (ps) subpic_close(ps);

    SubPic *pv = open_here(root, idx->rel, &data, &ix);
    CHECK(pv);
    snprintf(u, sizeof u, "/media/subpic/%s.json", hidx);
    get("GET", u, ck, &r);
    CHECK_STR(body_of(&r, &n), "{\"w\":720,\"h\":480,\"cues\":[[2000,3024,300,400,8,4]]}");
    snprintf(u, sizeof u, "/media/subpic/%s/0.png?v=1-2", hidx);
    get("GET", u, ck, &r);
    body = body_of(&r, &n);
    CHECK(pv && png_is(pv, 0, body, n));
    if (pv) subpic_close(pv);
    get("HEAD", u, ck, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    body_of(&r, &n);
    CHECK_INT(n, 0);
    CHECK_INT(media_subpic_test_opens(), 2); /* each file opened once: the cache */

    /* the address the page gives carries its key: a screen without the cookie gets it too, only it */
    snprintf(u, sizeof u, "/media/watch/%s", hv);
    get("GET", u, ck, &r);
    url_of(buf_cstr(&r), hsup, u2, sizeof u2);
    CHECK_HAS(u2, "&k=");
    get("GET", u2, "-", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_STR(body_of(&r, &n), "{\"w\":1920,\"h\":1080,\"cues\":[[1000,3000,100,900,8,4]]}");
    snprintf(u, sizeof u, "/media/subpic/%s.json", hidx); /* (no key, no cookie: the sign-in page) */
    get("GET", u, "-", &r);
    CHECK(!strstr(buf_cstr(&r), "application/json"));
    CHECK_HAS(body_of(&r, &n), "data-mod=\"auth\"");

    /* what isn't a picture subtitle of the library */
    const char *nope[] = {"/media/subpic/%s.json", "/media/subpic/%s/1.png", "/media/subpic/%s.txt", "/media/subpic/%s/x.png"};
    const char *ids[] = {hsrt, hv, hsub, "0123456789abcdef"};
    for (int i = 0; i < 4; i++) {
        snprintf(u, sizeof u, nope[0], ids[i]);
        get("GET", u, ck, &r);
        CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    }
    for (int i = 1; i < 4; i++) {
        snprintf(u, sizeof u, nope[i], hsup);
        get("GET", u, ck, &r);
        CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    }
    /* ... nor for a child, in a folder that's not for children */
    snprintf(u, sizeof u, "/media/subpic/%s.json", hsup);
    get("GET", u, leo, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK(th_act(adam, "media.kids", "p=0:Show&on=1").ok);
    get("GET", u, leo, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    CHECK(th_act(adam, "media.kids", "p=0:Show&on=0").ok);

    /* a file made again (same name, new size): opened again, its new cues */
    Buf two = {0}, set = {0};
    slurp(root, "Show/Season 1/Show.S01E01.sv.sup", &two);
    pgs_set(&set, true, 50, 60); /* + a second picture at 4 s, never cleared */
    buf_put(&two, "PG\x00\x05\x7e\x40\0\0\0\0", 10);
    for (size_t at = 0; at < set.len;) { /* (each segment its own record) */
        size_t len = 3 + ((size_t)(uint8_t)set.p[at + 1] << 8 | (uint8_t)set.p[at + 2]);
        if (at) buf_put(&two, "PG\x00\x05\x7e\x40\0\0\0\0", 10);
        buf_put(&two, set.p + at, len);
        at += len;
    }
    snprintf(path, sizeof path, "%s/Show/Season 1/Show.S01E01.sv.sup", root);
    file_write_atomic(path, two.p, two.len);
    buf_free(&two), buf_free(&set);
    get("GET", u, ck, &r);
    CHECK_HAS(body_of(&r, &n), "[[1000,3000,100,900,8,4],[4000,");
    CHECK_INT(media_subpic_test_opens(), 3);

    /* at most MEDIA_SUBPIC_CACHE kept: the least lately asked for goes */
    static const char *const more[] = {"de", "fr", "it", "nl"};
    for (int i = 0; i < 4; i++) {
        char rel[200];
        snprintf(rel, sizeof rel, "Show/Season 1/Show.S01E01.%s.sup", more[i]);
        snprintf(path, sizeof path, "%s/%s", root, rel);
        Buf copy = {0};
        slurp(root, "Show/Season 1/Show.S01E01.sv.sup", &copy);
        file_write_atomic(path, copy.p, copy.len);
        buf_free(&copy);
    }
    media_scan_sync();
    for (int i = 0; i < 4; i++) {
        char rel[200], h[17];
        snprintf(rel, sizeof rel, "Show.S01E01.%s.sup", more[i]);
        const MediaFile *f = file_named(rel);
        CHECK(f);
        media_id_hex(f ? f->id : 0, h);
        snprintf(u, sizeof u, "/media/subpic/%s.json", h);
        get("GET", u, ck, &r);
        CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    }
    CHECK_INT(media_subpic_test_opens(), 7);
    snprintf(u, sizeof u, "/media/subpic/%016llx.json", (unsigned long long)sup_id); /* (the first: gone from the cache) */
    get("GET", u, ck, &r);
    CHECK_INT(media_subpic_test_opens(), 8);
    get("GET", u, ck, &r);
    CHECK_INT(media_subpic_test_opens(), 8);

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r), buf_free(&data), buf_free(&ix);
    (void)adam;
    th_halt();
}
