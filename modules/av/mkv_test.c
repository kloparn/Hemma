/* modules/av/mkv_test.c — Matroska files written here in memory: headers via
 * the SeekHead, all lacing kinds, BlockGroups, unknown sizes, header
 * stripping, Cues and cue-less seeking, then mangled copies of them.
 * HEMMA_TEST_MKV=/path/file.mkv adds a check against a real file. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "mkv.h"

/* ------------------------------------------------------------ EBML writer */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} W;

static void w_put(W *w, const void *d, size_t n) {
    if (w->len + n > w->cap) {
        w->cap = (w->len + n) * 2 + 256;
        w->p = xrealloc(w->p, w->cap);
    }
    if (n) memcpy(w->p + w->len, d, n);
    w->len += n;
}

static void w_byte(W *w, int b) {
    uint8_t c = (uint8_t)b;
    w_put(w, &c, 1);
}

static void w_id(W *w, uint32_t id) {
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; i--) w_byte(w, (int)(id >> (8 * i)));
}

static void w_size8(W *w, uint64_t v) { /* 8-byte size field (patchable) */
    w_byte(w, 0x01);
    for (int i = 6; i >= 0; i--) w_byte(w, (int)(v >> (8 * i)));
}

static size_t w_open(W *w, uint32_t id) {
    w_id(w, id);
    size_t at = w->len;
    w_size8(w, 0);
    return at;
}

static void w_close(W *w, size_t at) {
    uint64_t n = w->len - at - 8;
    for (int i = 1; i < 8; i++) w->p[at + i] = (uint8_t)(n >> (8 * (7 - i)));
}

static void w_unknown(W *w, uint32_t id) {
    w_id(w, id);
    w_byte(w, 0x01);
    for (int i = 0; i < 7; i++) w_byte(w, 0xFF);
}

static void w_uint(W *w, uint32_t id, uint64_t v) {
    int n = 1;
    while (n < 8 && v >> (8 * n)) n++;
    w_id(w, id);
    w_byte(w, 0x80 | n);
    for (int i = n - 1; i >= 0; i--) w_byte(w, (int)(v >> (8 * i)));
}

static size_t w_uint8(W *w, uint32_t id, uint64_t v) { /* fixed 8 bytes, returns where the value sits */
    w_id(w, id);
    w_byte(w, 0x88);
    size_t at = w->len;
    for (int i = 7; i >= 0; i--) w_byte(w, (int)(v >> (8 * i)));
    return at;
}

static void w_patch8(W *w, size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) w->p[at + i] = (uint8_t)(v >> (8 * (7 - i)));
}

static void w_bin(W *w, uint32_t id, const void *d, size_t n) {
    w_id(w, id);
    w_size8(w, n);
    w_put(w, d, n);
}

static void w_str(W *w, uint32_t id, const char *s) { w_bin(w, id, s, strlen(s)); }

static void w_float(W *w, uint32_t id, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    w_id(w, id);
    w_byte(w, 0x88);
    for (int i = 7; i >= 0; i--) w_byte(w, (int)(u >> (8 * i)));
}

/* (Simple)Block payload header: 1-byte track vint, relative time, flags */
static void w_block(W *w, uint32_t id, int track, int rel, int flags, const W *payload) {
    w_id(w, id);
    w_size8(w, 4 + payload->len);
    w_byte(w, 0x80 | track);
    w_byte(w, (rel >> 8) & 0xFF);
    w_byte(w, rel & 0xFF);
    w_byte(w, flags);
    w_put(w, payload->p, payload->len);
}

static void fill(W *w, size_t n, int seed) {
    for (size_t i = 0; i < n; i++) w_byte(w, (int)((seed * 31 + i * 7) & 0xFF));
}

static bool mem_read(void *ctx, int64_t off, void *dst, size_t n) {
    const W *w = ctx;
    if (off < 0 || (uint64_t)off + n > w->len) return false;
    memcpy(dst, w->p + off, n);
    return true;
}

static Mkv *open_w(W *w) {
    MkvReader r = {w, (int64_t)w->len, mem_read};
    char err[100];
    return mkv_open(&r, err, sizeof err);
}

/* -------------------------------------------------------- a test file */

typedef struct {
    size_t c1, c2, c3; /* cluster offsets */
} Layout;

static void ebml_header(W *w, const char *doctype) {
    size_t e = w_open(w, 0x1A45DFA3);
    w_uint(w, 0x4286, 1);
    w_str(w, 0x4282, doctype);
    w_uint(w, 0x4287, 4);
    w_close(w, e);
}

/* 3 tracks (MPEG-2 video, laced AC-3 audio, header-stripped VobSub), 3
   clusters (the middle one of unknown size), Cues at the end via the SeekHead. */
static void build_file(W *w, Layout *lay, bool with_cues) {
    memset(w, 0, sizeof *w);
    ebml_header(w, "matroska");
    size_t seg = w_open(w, 0x18538067);
    size_t data = w->len;
    size_t sh = w_open(w, 0x114D9B74);
    size_t s = w_open(w, 0x4DBB);
    w_bin(w, 0x53AB, "\x1C\x53\xBB\x6B", 4);
    size_t cues_pos = w_uint8(w, 0x53AC, 0);
    w_close(w, s);
    w_close(w, sh);
    w_bin(w, 0xEC, "\0\0\0\0", 4); /* a Void between level-1 elements */
    size_t info = w_open(w, 0x1549A966);
    w_uint(w, 0x2AD7B1, 1000000);
    w_float(w, 0x4489, 3000.0);
    w_str(w, 0x7BA9, "Test");
    w_str(w, 0x5741, "hemma-test");
    w_close(w, info);
    size_t tracks = w_open(w, 0x1654AE6B);
    size_t t = w_open(w, 0xAE);
    w_uint(w, 0xD7, 1);
    w_uint(w, 0x73C5, 11);
    w_uint(w, 0x83, 1);
    w_str(w, 0x86, "V_MPEG2");
    w_bin(w, 0x63A2, "SEQ", 3);
    w_uint(w, 0x23E383, 33366667);
    size_t v = w_open(w, 0xE0);
    w_uint(w, 0xB0, 720);
    w_uint(w, 0xBA, 480);
    w_uint(w, 0x54B0, 640);
    w_uint(w, 0x54BA, 480);
    w_uint(w, 0x9A, 1);
    w_close(w, v);
    w_close(w, t);
    t = w_open(w, 0xAE);
    w_uint(w, 0xD7, 2);
    w_uint(w, 0x83, 2);
    w_str(w, 0x86, "A_AC3");
    w_str(w, 0x22B59C, "swe");
    w_str(w, 0x536E, "Svenska");
    w_uint(w, 0x88, 0);
    w_uint(w, 0x23E383, 32000000);
    size_t a = w_open(w, 0xE1);
    w_float(w, 0xB5, 48000.0);
    w_uint(w, 0x9F, 6);
    w_close(w, a);
    w_close(w, t);
    t = w_open(w, 0xAE);
    w_uint(w, 0xD7, 3);
    w_uint(w, 0x83, 17);
    w_str(w, 0x86, "S_VOBSUB");
    size_t encs = w_open(w, 0x6D80);
    size_t enc = w_open(w, 0x6240);
    size_t comp = w_open(w, 0x5034);
    w_uint(w, 0x4254, 3);
    w_bin(w, 0x4255, "\x00\x00\x01", 3);
    w_close(w, comp);
    w_close(w, enc);
    w_close(w, encs);
    w_close(w, t);
    w_close(w, tracks);

    W pl = {0};
    /* cluster 1 at 0 ms */
    lay->c1 = w->len;
    size_t c = w_open(w, 0x1F43B675);
    w_uint(w, 0xE7, 0);
    pl.len = 0;
    w_put(&pl, "V0", 2);
    w_block(w, 0xA3, 1, 0, 0x80, &pl);
    pl.len = 0; /* Xiph lacing: 3 frames of 300, 2 and 7 bytes */
    w_byte(&pl, 2);
    w_byte(&pl, 255), w_byte(&pl, 45), w_byte(&pl, 2);
    fill(&pl, 300, 1), fill(&pl, 2, 2), fill(&pl, 7, 3);
    w_block(w, 0xA3, 2, 0, 0x80 | 0x02, &pl);
    size_t g = w_open(w, 0xA0);
    pl.len = 0;
    w_put(&pl, "V1", 2);
    w_block(w, 0xA1, 1, 40, 0, &pl);
    w_uint(w, 0xFB, 0); /* ReferenceBlock: not a keyframe */
    w_uint(w, 0x9B, 40);
    w_close(w, g);
    pl.len = 0;
    w_put(&pl, "SUB", 3);
    w_block(w, 0xA3, 3, 10, 0x80, &pl);
    w_close(w, c);
    /* cluster 2 at 1000 ms, unknown size */
    lay->c2 = w->len;
    w_unknown(w, 0x1F43B675);
    w_uint(w, 0xE7, 1000);
    pl.len = 0;
    w_put(&pl, "V2", 2);
    w_block(w, 0xA3, 1, 0, 0x80, &pl);
    pl.len = 0; /* EBML lacing: 100, 90 (-10), 120 (+30), 50 */
    w_byte(&pl, 3);
    w_byte(&pl, 0x80 | 100), w_byte(&pl, 0x80 | (63 - 10)), w_byte(&pl, 0x80 | (63 + 30));
    fill(&pl, 100, 4), fill(&pl, 90, 5), fill(&pl, 120, 6), fill(&pl, 50, 7);
    w_block(w, 0xA3, 2, 5, 0x80 | 0x06, &pl);
    pl.len = 0; /* fixed lacing: 2 x 10 */
    w_byte(&pl, 1);
    fill(&pl, 20, 8);
    w_block(w, 0xA3, 2, 133, 0x80 | 0x04, &pl);
    /* cluster 3 at 2000 ms (also ends the unknown-size one) */
    lay->c3 = w->len;
    c = w_open(w, 0x1F43B675);
    w_uint(w, 0xE7, 2000);
    pl.len = 0;
    w_put(&pl, "V3", 2);
    w_block(w, 0xA3, 1, 0, 0x80, &pl);
    w_close(w, c);
    if (with_cues) {
        w_patch8(w, cues_pos, w->len - data);
        size_t cues = w_open(w, 0x1C53BB6B);
        size_t at[3] = {lay->c1, lay->c2, lay->c3};
        for (int k = 0; k < 3; k++) {
            size_t cp = w_open(w, 0xBB);
            w_uint(w, 0xB3, (uint64_t)k * 1000);
            size_t tp = w_open(w, 0xB7);
            w_uint(w, 0xF7, 1);
            w_uint(w, 0xF1, at[k] - data);
            w_close(w, tp);
            w_close(w, cp);
        }
        w_close(w, cues);
    } else {
        w_patch8(w, cues_pos, 0x7FFFFFFF); /* points nowhere */
    }
    w_close(w, seg);
    free(pl.p);
}

static bool same(const MkvFrame *f, const char *s) { return f->size == strlen(s) && !memcmp(f->data, s, f->size); }

static bool filled(const MkvFrame *f, size_t n, int seed) {
    if (f->size != n) return false;
    for (size_t i = 0; i < n; i++)
        if (f->data[i] != (uint8_t)((seed * 31 + i * 7) & 0xFF)) return false;
    return true;
}

#define MS 1000000LL

TEST(mkv_reads_headers_tracks_and_cues) {
    W w;
    Layout lay;
    build_file(&w, &lay, true);
    Mkv *m = open_w(&w);
    CHECK(m);
    CHECK_STR(mkv_doctype(m), "matroska");
    CHECK_STR(mkv_title(m), "Test");
    CHECK_STR(mkv_writing_app(m), "hemma-test");
    CHECK_INT(mkv_duration(m), 3000 * MS);
    CHECK_INT(mkv_track_count(m), 3);
    CHECK_INT(mkv_cue_count(m), 3);
    const MkvTrack *v = mkv_track_by_number(m, 1), *a = mkv_track_by_number(m, 2), *s = mkv_track_by_number(m, 3);
    CHECK(v && a && s);
    CHECK_STR(v->codec, "V_MPEG2");
    CHECK_INT(v->type, MKV_VIDEO);
    CHECK_INT(v->uid, 11);
    CHECK_INT(v->priv_len, 3);
    CHECK(!memcmp(v->priv, "SEQ", 3));
    CHECK_INT(v->width, 720);
    CHECK_INT(v->height, 480);
    CHECK_INT(v->disp_width, 640);
    CHECK_INT(v->interlaced, 1);
    CHECK_INT(v->default_duration, 33366667);
    CHECK(v->is_default);
    CHECK_STR(v->lang, "eng");
    CHECK_STR(a->codec, "A_AC3");
    CHECK_STR(a->lang, "swe");
    CHECK_STR(a->name, "Svenska");
    CHECK(!a->is_default);
    CHECK_INT((int)a->rate, 48000);
    CHECK_INT(a->channels, 6);
    CHECK_INT(s->type, MKV_SUBTITLE);
    CHECK_INT(s->strip_len, 3);
    CHECK(!s->compressed);
    CHECK(mkv_find_track(m, MKV_VIDEO) == v);
    CHECK(mkv_find_track(m, MKV_AUDIO) == a);
    CHECK(mkv_find_track(m, MKV_BUTTONS) == NULL);
    mkv_close(m);
    free(w.p);
}

TEST(mkv_frames_with_every_lacing) {
    W w;
    Layout lay;
    build_file(&w, &lay, true);
    Mkv *m = open_w(&w);
    CHECK(m);
    MkvFrame f;
    CHECK_INT(mkv_next(m, &f), 1);
    CHECK(f.track == 1 && f.pts == 0 && f.key && same(&f, "V0"));
    CHECK_INT(f.duration, 33366667);
    static const size_t xiph[3] = {300, 2, 7};
    for (int i = 0; i < 3; i++) { /* Xiph lace: pts step by DefaultDuration */
        CHECK_INT(mkv_next(m, &f), 1);
        CHECK_INT(f.track, 2);
        CHECK_INT(f.pts, i * 32 * MS);
        CHECK(filled(&f, xiph[i], i + 1));
    }
    CHECK_INT(mkv_next(m, &f), 1); /* BlockGroup with a ReferenceBlock */
    CHECK(f.track == 1 && f.pts == 40 * MS && !f.key && same(&f, "V1"));
    CHECK_INT(f.duration, 40 * MS);
    CHECK_INT(mkv_next(m, &f), 1); /* header stripping puts 00 00 01 back */
    CHECK_INT(f.track, 3);
    CHECK_INT(f.pts, 10 * MS);
    CHECK(f.size == 6 && !memcmp(f.data, "\x00\x00\x01SUB", 6));
    CHECK_INT(mkv_next(m, &f), 1); /* unknown-size cluster */
    CHECK(f.track == 1 && f.pts == 1000 * MS && same(&f, "V2"));
    static const size_t ebml[4] = {100, 90, 120, 50};
    for (int i = 0; i < 4; i++) {
        CHECK_INT(mkv_next(m, &f), 1);
        CHECK_INT(f.pts, (1005 + i * 32) * MS);
        CHECK(filled(&f, ebml[i], i + 4));
    }
    for (int i = 0; i < 2; i++) { /* fixed lacing */
        CHECK_INT(mkv_next(m, &f), 1);
        CHECK_INT(f.pts, (1133 + i * 32) * MS);
        CHECK_INT(f.size, 10);
        CHECK_INT(f.data[0], (uint8_t)((8 * 31 + i * 10 * 7) & 0xFF));
    }
    CHECK_INT(mkv_next(m, &f), 1);
    CHECK(f.track == 1 && f.pts == 2000 * MS && same(&f, "V3"));
    CHECK_INT(mkv_next(m, &f), 0);
    CHECK_INT(mkv_next(m, &f), 0);
    mkv_close(m);
    free(w.p);
}

static int64_t first_video_pts(Mkv *m) {
    MkvFrame f;
    while (mkv_next(m, &f) == 1)
        if (f.track == 1) return f.pts;
    return -1;
}

TEST(mkv_seeks_with_cues_and_without) {
    for (int with = 1; with >= 0; with--) {
        W w;
        Layout lay;
        build_file(&w, &lay, with);
        Mkv *m = open_w(&w);
        CHECK(m);
        CHECK_INT(mkv_cue_count(m), with ? 3 : 0);
        CHECK(mkv_seek(m, 1500 * MS, 0));
        CHECK_INT(first_video_pts(m), 1000 * MS);
        CHECK(mkv_seek(m, 500 * MS, 1));
        CHECK_INT(first_video_pts(m), 0);
        CHECK(mkv_seek(m, 99000 * MS, 0));
        CHECK_INT(first_video_pts(m), 2000 * MS);
        CHECK(mkv_seek(m, 0, 0));
        CHECK_INT(first_video_pts(m), 0);
        mkv_rewind(m);
        CHECK_INT(first_video_pts(m), 0);
        mkv_close(m);
        free(w.p);
    }
}

/* A long cue-less file (unknown-size segment): seeking bisects the file. */
TEST(mkv_bisects_big_files_without_cues) {
    W w;
    memset(&w, 0, sizeof w);
    ebml_header(&w, "webm");
    w_unknown(&w, 0x18538067);
    size_t tracks = w_open(&w, 0x1654AE6B);
    size_t t = w_open(&w, 0xAE);
    w_uint(&w, 0xD7, 1);
    w_uint(&w, 0x83, 1);
    w_str(&w, 0x86, "V_VP9");
    w_close(&w, t);
    w_close(&w, tracks);
    W pl = {0};
    fill(&pl, 6000, 9);
    for (int k = 0; k < 400; k++) { /* 400 one-second clusters, ~2.4 MB */
        size_t c = w_open(&w, 0x1F43B675);
        w_uint(&w, 0xE7, (uint64_t)k * 1000);
        w_block(&w, 0xA3, 1, 0, 0x80, &pl);
        w_close(&w, c);
    }
    Mkv *m = open_w(&w);
    CHECK(m);
    CHECK_STR(mkv_doctype(m), "webm");
    static const int targets[] = {0, 1, 137, 250, 399, 398, 5};
    for (size_t i = 0; i < sizeof targets / sizeof *targets; i++) {
        CHECK(mkv_seek(m, targets[i] * 1000 * MS + 500 * MS, 0));
        CHECK_INT(first_video_pts(m), targets[i] * 1000 * MS);
    }
    mkv_close(m);
    free(w.p);
    free(pl.p);
}

static uint32_t rnd_state = 2463534242u;
static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

TEST(mkv_survives_mangled_files) {
    W orig;
    Layout lay;
    build_file(&orig, &lay, true);
    W w = {xmalloc(orig.len), orig.len, orig.len};
    int opened = 0;
    for (int round = 0; round < 4000; round++) {
        memcpy(w.p, orig.p, orig.len);
        w.len = orig.len;
        int changes = 1 + (int)(rnd() % 8);
        for (int k = 0; k < changes; k++) {
            size_t at = rnd() % w.len;
            switch (rnd() % 3) {
            case 0: w.p[at] ^= (uint8_t)(1 << (rnd() % 8)); break;
            case 1: w.p[at] = (uint8_t)rnd(); break;
            default: w.p[at] = rnd() % 2 ? 0xFF : 0x01; break; /* size-ish bytes */
            }
        }
        if (rnd() % 5 == 0) w.len = rnd() % orig.len;
        Mkv *m = open_w(&w);
        if (!m) continue;
        opened++;
        MkvFrame f;
        int n = 0, rc;
        while ((rc = mkv_next(m, &f)) == 1 && n < 10000) {
            CHECK(f.size <= w.len + 3);
            if (f.size) {
                volatile uint8_t sink = f.data[f.size - 1];
                (void)sink;
            }
            n++;
        }
        CHECK(n < 10000); /* it ends */
        mkv_seek(m, (int64_t)(rnd() % 4000) * MS, 0);
        for (n = 0; n < 100 && mkv_next(m, &f) == 1; n++) {}
        mkv_close(m);
    }
    CHECK(opened > 500); /* plenty of mangled copies still open and get read */
    free(w.p);
    free(orig.p);
}

/* ------------------------------------------------- a real file (opt-in) */

static bool file_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

TEST(mkv_reads_real_file_when_given) {
    const char *path = plat_getenv("HEMMA_TEST_MKV");
    if (!path || !*path) return; /* opt-in: HEMMA_TEST_MKV=/path/to/file.mkv */
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    MkvReader r = {fp, size, file_cb};
    char err[100];
    Mkv *m = mkv_open(&r, err, sizeof err);
    CHECK(m);
    const MkvTrack *v = mkv_find_track(m, MKV_VIDEO);
    CHECK(v && v->width > 0 && v->height > 0);
    MkvFrame f;
    int64_t pos = -1;
    int frames = 0, keys = 0;
    bool forward = true;
    while (frames < 3000 && mkv_next(m, &f) == 1) {
        forward &= f.pos >= pos;
        pos = f.pos;
        frames++;
        keys += f.track == v->number && f.key;
    }
    CHECK(frames > 100);
    CHECK(keys > 0);
    CHECK(forward);
    int64_t dur = mkv_duration(m);
    if (dur > 120 * 1000 * MS) { /* seek to a minute: the next video keyframe is near it */
        CHECK(mkv_seek(m, 60 * 1000 * MS, 0));
        int64_t at = -1;
        while (mkv_next(m, &f) == 1)
            if (f.track == v->number && f.key) {
                at = f.pts;
                break;
            }
        CHECK(at > 50 * 1000 * MS && at <= 61 * 1000 * MS);
    }
    mkv_close(m);
    fclose(fp);
}
