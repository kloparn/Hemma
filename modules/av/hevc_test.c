/* modules/av/hevc_test.c — the HEVC decoder: streams written here (hevc_testenc.h) whose pictures are known to the
 * sample (PCM, whole-sample motion, bi-prediction), the display order and block times, 10-bit samples, the cropping
 * window, Annex B in pieces of any size, every syntax element read whole, what it refuses, damage, a seek - and,
 * opt-in, a real file against FFmpeg's frames (HEVC decoding is exact: every sample must be FFmpeg's):
 *
 *   HEMMA_TEST_HEVC=<file.mkv|.mp4|.hevc> [HEMMA_TEST_HEVC_N=frames] [HEMMA_TEST_HEVC_REF=<ffmpeg's .yuv>]
 *   ./build/test hevc_real
 *
 * with the reference made by `ffmpeg -i <file> -map 0:v -f rawvideo -pix_fmt yuv420p ref.yuv` (yuv420p10le for a
 * 10-bit file: its samples are compared at their own depth). */
#include "../../tests/test.h"
#include "../../core/base.h"
#include "../../core/plat.h"
#include "hevc.h"
#include "hevc_testenc.h"
#include "mkv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------- reading what comes out */

#define MAXW 96
#define MAXH 64

typedef struct {
    int n, damaged;
    char type[16];
    int64_t pts[16];
    int w, h, depth;
    uint16_t px[16][3][MAXW * MAXH]; /* the samples at their own depth */
    uint8_t px8[16][MAXW * MAXH];    /* luma as 8 bits */
} Out;

static void take(Hevc *d, Out *o) {
    HevcFrame f;
    while (hevc_frame(d, &f)) {
        if (o->n >= 16) continue;
        o->w = f.width, o->h = f.height, o->depth = f.bit_depth, o->damaged += f.damaged;
        o->type[o->n] = f.type, o->pts[o->n] = f.pts;
        for (int c = 0; c < 3; c++) {
            int w = c ? (f.width + 1) / 2 : f.width, h = c ? (f.height + 1) / 2 : f.height;
            const uint16_t *p = c == 0 ? f.y16 : c == 1 ? f.u16 : f.v16;
            int st = c ? f.stride16_c : f.stride16_y;
            for (int y = 0; y < h && y < MAXH; y++)
                for (int x = 0; x < w && x < MAXW; x++) o->px[o->n][c][y * MAXW + x] = p[(size_t)y * st + x];
        }
        for (int y = 0; y < f.height && y < MAXH; y++) memcpy(o->px8[o->n] + y * MAXW, f.y + (size_t)y * f.stride_y, (size_t)(f.width < MAXW ? f.width : MAXW));
        o->n++;
    }
}

static uint8_t g_au[1 << 20];

/* decode a written stream, picture by picture (hvcC + length-prefixed units, each with its time) */
static void decode_stream(const HwStream *s, Out *o, bool flush) {
    uint8_t hvcc[2048];
    size_t hn = hw_hvcc(s, hvcc, sizeof hvcc);
    Hevc *d = hevc_new();
    CHECK(d && hevc_config(d, hvcc, hn));
    for (int k = 0; k < hw_count(s); k++) {
        int poc;
        size_t n = hw_picture(s, k, false, g_au, sizeof g_au, &poc);
        CHECK(n > 0);
        CHECK(hevc_decode(d, g_au, n, 1000 * poc) >= 0);
        take(d, o);
    }
    if (flush) hevc_flush(d), take(d, o);
    hevc_free(d);
}

/* the exact stream's pictures, worked out here: PCM, then whole-sample moves (the edges repeated), averages */
static int ref_sample(const HwStream *s, uint16_t (*pics)[3][MAXW * MAXH], int poc, int c, int x, int y) {
    int w = c ? s->width / 2 : s->width, h = c ? s->height / 2 : s->height;
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    return pics[poc][c][y * MAXW + x];
}

static void expected(const HwStream *s, uint16_t (*pics)[3][MAXW * MAXH]) {
    for (int k = 0; k < hw_count(s); k++) {
        HwPic p = hw_order(s, k);
        for (int c = 0; c < 3; c++) {
            int w = c ? s->width / 2 : s->width, h = c ? s->height / 2 : s->height, sh = c ? 1 : 0;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int v;
                    if (p.type == 2) v = hw_pattern(s, c, x, y);
                    else if (p.type == 1) v = ref_sample(s, pics, p.ref[0], c, x + (s->mv[0][0] >> sh), y + (s->mv[0][1] >> sh));
                    else
                        v = (ref_sample(s, pics, p.ref[0], c, x + (s->mv[0][0] >> sh), y + (s->mv[0][1] >> sh)) +
                             ref_sample(s, pics, p.ref[1], c, x + (s->mv[1][0] >> sh), y + (s->mv[1][1] >> sh)) + 1) >> 1;
                    pics[p.poc][c][y * MAXW + x] = (uint16_t)v;
                }
        }
    }
}

static HwStream exact(int w, int h, int depth, int frames) {
    HwStream s;
    memset(&s, 0, sizeof s);
    s.width = w, s.height = h, s.depth = depth, s.chroma = 1, s.frames = frames, s.seed = 1;
    s.mv[0][0] = 4, s.mv[0][1] = -2, s.mv[1][0] = -6, s.mv[1][1] = 2; /* (even: the chroma moves by whole samples too) */
    return s;
}

/* --------------------------------------------- the tests */

TEST(hevc_decodes_pcm_moves_and_averages_exactly) {
    static Out o;
    static uint16_t want[16][3][MAXW * MAXH];
    memset(&o, 0, sizeof o);
    HwStream s = exact(64, 48, 8, 7);
    expected(&s, want);
    decode_stream(&s, &o, true);
    CHECK_INT(o.n, 7);
    CHECK_INT(o.damaged, 0);
    CHECK_INT(o.w, 64);
    CHECK_INT(o.h, 48);
    CHECK_INT(o.depth, 8);
    int bad = 0;
    for (int f = 0; f < o.n; f++)
        for (int c = 0; c < 3; c++)
            for (int y = 0; y < (c ? 24 : 48); y++)
                for (int x = 0; x < (c ? 32 : 64); x++)
                    if (o.px[f][c][y * MAXW + x] != want[f][c][y * MAXW + x] && bad++ < 3)
                        test_fail(__FILE__, __LINE__, "frame %d plane %d at %d,%d: %d, want %d", f, c, x, y,
                                  o.px[f][c][y * MAXW + x], want[f][c][y * MAXW + x]);
    CHECK_INT(bad, 0);
    for (int y = 0; y < 48; y++)
        for (int x = 0; x < 64; x++)
            if (o.px8[3][y * MAXW + x] != o.px[3][0][y * MAXW + x]) bad++;
    CHECK_INT(bad, 0);
}

TEST(hevc_frames_come_in_display_order_with_their_times) {
    static Out o;
    memset(&o, 0, sizeof o);
    HwStream s = exact(32, 32, 8, 7); /* decode order I0 P2 B1 P4 B3 P6 B5 */
    decode_stream(&s, &o, true);
    CHECK_INT(o.n, 7);
    for (int f = 0; f < o.n; f++) CHECK_INT(o.pts[f], 1000 * f);
    CHECK_INT(o.type[0], 'I');
    CHECK_INT(o.type[1], 'B');
    CHECK_INT(o.type[2], 'P');
    /* without hevc_flush the last frames wait (reordering), but the earlier ones are already out */
    static Out p;
    memset(&p, 0, sizeof p);
    decode_stream(&s, &p, false);
    CHECK(p.n >= 4 && p.n < 7);
}

/* 10-bit samples, and 9-bit (Main 10 allows it; a fuzzed stream had them) - each at its own depth, and rounded to 8 */
TEST(hevc_ten_and_nine_bit_samples_and_their_eight_bit_planes) {
    static Out o;
    static uint16_t want[16][3][MAXW * MAXH];
    for (int depth = 9; depth <= 10; depth++) {
        memset(&o, 0, sizeof o);
        HwStream s = exact(48, 32, depth, 3);
        expected(&s, want);
        decode_stream(&s, &o, true);
        CHECK_INT(o.n, 3);
        CHECK_INT(o.depth, depth);
        int bad = 0, bad8 = 0, up = depth - 8;
        for (int f = 0; f < o.n; f++)
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 48; x++) {
                    int v = o.px[f][0][y * MAXW + x];
                    if (v != want[f][0][y * MAXW + x]) bad++;
                    int r = (v + (1 << (up - 1))) >> up;
                    if (o.px8[f][y * MAXW + x] != (r > 255 ? 255 : r)) bad8++;
                }
        CHECK_INT(bad, 0);
        CHECK_INT(bad8, 0);
    }
}

TEST(hevc_shows_the_conformance_window) {
    static Out o;
    memset(&o, 0, sizeof o);
    HwStream s = exact(64, 48, 8, 1);
    s.crop_r = 6, s.crop_b = 8; /* 58 x 40 shown */
    decode_stream(&s, &o, true);
    CHECK_INT(o.n, 1);
    CHECK_INT(o.w, 58);
    CHECK_INT(o.h, 40);
    CHECK_INT(o.px[0][0][39 * MAXW + 57], hw_pattern(&s, 0, 57, 39));
    CHECK_INT(o.px[0][1][19 * MAXW + 28], hw_pattern(&s, 1, 28, 19));
    uint8_t hvcc[2048];
    size_t hn = hw_hvcc(&s, hvcc, sizeof hvcc);
    Hevc *d = hevc_new();
    int w = 0, h = 0;
    CHECK(!hevc_info(d, &w, &h));
    CHECK(hevc_config(d, hvcc, hn));
    CHECK(hevc_info(d, &w, &h));
    CHECK_INT(w, 58);
    CHECK_INT(h, 40);
    hevc_free(d);
}

/* Annex B, fed in pieces of 1 to 13 bytes: the same frames as the hvcC path's */
TEST(hevc_annex_b_in_pieces_of_any_size) {
    static Out a, b;
    static uint8_t stream[1 << 18];
    memset(&a, 0, sizeof a), memset(&b, 0, sizeof b);
    HwStream s = exact(48, 32, 8, 5);
    s.flags = HW_RANDOM | HW_SAO;
    decode_stream(&s, &a, true);
    size_t n = hevc_test_stream(stream, sizeof stream, &s);
    CHECK(n > 0);
    Hevc *d = hevc_new();
    for (size_t o = 0, step = 1; o < n; o += step, step = step % 13 + 1) {
        size_t k = o + step > n ? n - o : step;
        CHECK(hevc_decode(d, stream + o, k, HEVC_NOPTS) >= 0);
        take(d, &b);
    }
    hevc_flush(d);
    take(d, &b);
    hevc_free(d);
    CHECK_INT(a.n, 5);
    CHECK_INT(b.n, 5);
    CHECK_INT(b.damaged, 0);
    CHECK(!memcmp(a.px, b.px, sizeof a.px));
}

/* streams using every kind of syntax the writer knows - read whole, every frame clean */
TEST(hevc_reads_every_syntax_element_whole) {
    static Out o;
    static const struct { int w, h, depth; unsigned flags; } kinds[] = {
        {64, 48, 8, HW_RANDOM}, {64, 48, 10, HW_RANDOM}, {96, 64, 8, HW_RANDOM | HW_SAO | HW_PCM_LF},
        {80, 48, 8, HW_RANDOM | HW_TMVP | HW_SAO}, {48, 40, 10, HW_RANDOM | HW_NODEBLOCK | HW_SAO}, {72, 56, 8, HW_RANDOM | HW_TMVP | HW_PCM_LF},
        /* QP deltas, a third far out of range (a fuzzed stream's took the QP below zero: a table read at -1) */
        {64, 48, 8, HW_RANDOM | HW_QP | HW_SAO}, {48, 40, 9, HW_RANDOM | HW_QP | HW_TMVP}, {64, 32, 10, HW_RANDOM | HW_QP}};
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++)
        for (uint32_t seed = 1; seed <= 4; seed++) {
            memset(&o, 0, sizeof o);
            HwStream s = exact(kinds[k].w, kinds[k].h, kinds[k].depth, 9);
            s.flags = kinds[k].flags, s.seed = seed * 7919 + (uint32_t)k;
            decode_stream(&s, &o, true);
            if (o.n != 9 || o.damaged)
                test_fail(__FILE__, __LINE__, "kind %d seed %u: %d frames, %d damaged slices", (int)k, seed, o.n, o.damaged);
        }
}

TEST(hevc_names_what_it_doesnt_decode) {
    static const struct { int chroma, depth; bool field; const char *says; } cases[] = {
        {2, 8, false, "4:2:2 chroma"}, {3, 8, false, "4:4:4 chroma"}, {1, 12, false, "more than 10 bits a sample"},
        {1, 8, true, "interlaced pictures sent as fields"}};
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        HwStream s = exact(32, 32, cases[k].depth, 2);
        s.chroma = cases[k].chroma, s.field_seq = cases[k].field;
        uint8_t hvcc[2048];
        size_t hn = hw_hvcc(&s, hvcc, sizeof hvcc);
        Hevc *d = hevc_new();
        CHECK(!hevc_unsupported(d));
        hevc_config(d, hvcc, hn);
        int poc;
        size_t n = hw_picture(&s, 0, false, g_au, sizeof g_au, &poc);
        hevc_decode(d, g_au, n, 0);
        hevc_flush(d);
        HevcFrame f;
        CHECK(!hevc_frame(d, &f));
        CHECK(hevc_unsupported(d) && !strcmp(hevc_unsupported(d), cases[k].says));
        hevc_free(d);
    }
}

static uint32_t g_rnd = 12345;
static uint32_t rnd(void) { return g_rnd = g_rnd * 1103515245u + 12345u, g_rnd >> 8; }

TEST(hevc_survives_damaged_streams) {
    HwStream s = exact(48, 32, 8, 6);
    s.flags = HW_RANDOM | HW_SAO | HW_TMVP;
    uint8_t hvcc[2048];
    size_t hn = hw_hvcc(&s, hvcc, sizeof hvcc);
    static uint8_t pics[8][1 << 15], buf[1 << 15];
    size_t lens[8];
    int count = hw_count(&s);
    for (int k = 0; k < count; k++) lens[k] = hw_picture(&s, k, false, pics[k], sizeof pics[k], NULL);
    for (int round = 0; round < 400; round++) {
        Hevc *d = hevc_new();
        if (round % 5) hevc_config(d, hvcc, hn);
        for (int k = 0; k < count; k++) {
            size_t n = lens[k];
            memcpy(buf, pics[k], n);
            int flips = (int)(rnd() % 6);
            for (int f = 0; f < flips; f++) buf[4 + rnd() % (n - 4)] ^= (uint8_t)(1u << (rnd() % 8));
            if (round % 7 == 0) n = rnd() % (n + 1); /* cut short */
            if (round % 11 == 0)
                for (size_t i = 4; i < n; i++) buf[i] = (uint8_t)rnd(); /* noise */
            CHECK(hevc_decode(d, buf, n, k) >= 0);
            HevcFrame f;
            while (hevc_frame(d, &f)) CHECK(f.width > 0 && f.height > 0 && f.y && f.u && f.v && f.y16);
        }
        hevc_flush(d);
        HevcFrame f;
        while (hevc_frame(d, &f)) {
        }
        hevc_free(d);
    }
}

/* after hevc_reset, P and B pictures wait for the next key picture */
TEST(hevc_after_a_seek_waits_for_a_key_picture) {
    HwStream s = exact(32, 32, 8, 5);
    uint8_t hvcc[2048];
    size_t hn = hw_hvcc(&s, hvcc, sizeof hvcc);
    Hevc *d = hevc_new();
    hevc_config(d, hvcc, hn);
    static Out o;
    memset(&o, 0, sizeof o);
    for (int k = 0; k < 3; k++) {
        size_t n = hw_picture(&s, k, false, g_au, sizeof g_au, NULL);
        hevc_decode(d, g_au, n, k);
        take(d, &o);
    }
    hevc_reset(d);
    int before = o.n;
    for (int k = 3; k < 5; k++) {
        size_t n = hw_picture(&s, k, false, g_au, sizeof g_au, NULL);
        hevc_decode(d, g_au, n, k);
        take(d, &o);
    }
    hevc_flush(d);
    take(d, &o);
    CHECK_INT(o.n, before); /* (P4 and B3 refer to pictures from before the seek: nothing) */
    size_t n = hw_picture(&s, 0, false, g_au, sizeof g_au, NULL);
    hevc_decode(d, g_au, n, 99);
    hevc_flush(d);
    take(d, &o);
    CHECK_INT(o.n, before + 1);
    CHECK_INT(o.pts[before], 99);
    hevc_free(d);
}

/* --------------------------------------------- a real file, opt-in */

static bool read_cb(void *ctx, int64_t off, void *dst, size_t n) {
    FILE *f = ctx;
    return !fseek(f, (long)off, SEEK_SET) && fread(dst, 1, n, f) == n;
}

TEST(hevc_real) {
    const char *path = plat_getenv("HEMMA_TEST_HEVC");
    if (!path || !*path) return;
    const char *ns = plat_getenv("HEMMA_TEST_HEVC_N"), *refp = plat_getenv("HEMMA_TEST_HEVC_REF");
    long want = ns && *ns ? strtol(ns, NULL, 10) : 300;
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    if (!fp) return;
    Hevc *d = hevc_new();
    Mkv *m = NULL;
    const MkvTrack *v = NULL;
    const char *ext = strrchr(path, '.');
    bool annexb = ext && (!strcmp(ext, ".hevc") || !strcmp(ext, ".265"));
    if (!annexb) {
        MkvReader r = {fp, size, read_cb};
        char err[160];
        m = mkv_open(&r, err, sizeof err);
        CHECK(m);
        if (!m) return;
        v = mkv_find_track(m, MKV_VIDEO);
        CHECK(v && !strcmp(v->codec, "V_MPEGH/ISO/HEVC"));
        if (!v) return;
        CHECK(hevc_config(d, v->priv, v->priv_len));
    }
    FILE *ref = refp && *refp ? file_open(refp, "rb") : NULL;
    long frames = 0, exact_frames = 0, damaged = 0, first_bad = -1;
    uint8_t *rb = NULL, chunk[65536];
    double t0 = (double)now_ms();
    for (bool more = true; frames < want;) {
        if (more) {
            if (annexb) {
                size_t n = fread(chunk, 1, sizeof chunk, fp);
                if (n) hevc_decode(d, chunk, n, HEVC_NOPTS);
                else more = false, hevc_flush(d);
            } else {
                MkvFrame f;
                if (mkv_next(m, &f) == 1) {
                    if (f.track != v->number) continue;
                    hevc_decode(d, f.data, f.size, f.pts);
                } else {
                    more = false, hevc_flush(d);
                }
            }
        }
        HevcFrame fr;
        bool got = false;
        while (frames < want && hevc_frame(d, &fr)) {
            got = true;
            frames++;
            damaged += fr.damaged;
            if (!ref) continue;
            int bps = fr.bit_depth > 8 ? 2 : 1, cw = (fr.width + 1) / 2, ch = (fr.height + 1) / 2;
            size_t fsz = ((size_t)fr.width * fr.height + 2 * (size_t)cw * ch) * (size_t)bps;
            if (!rb) rb = malloc(fsz);
            if (!rb || fread(rb, 1, fsz, ref) != fsz) {
                fprintf(stderr, "    the reference ends at frame %ld\n", frames - 1);
                fclose(ref);
                ref = NULL;
                continue;
            }
            bool same = true;
            const uint8_t *q = rb;
            for (int c = 0; c < 3 && same; c++) {
                int w = c ? cw : fr.width, h = c ? ch : fr.height, st = c ? fr.stride16_c : fr.stride16_y;
                const uint16_t *p = c == 0 ? fr.y16 : c == 1 ? fr.u16 : fr.v16;
                for (int y = 0; y < h && same; y++)
                    for (int x = 0; x < w; x++, q += bps)
                        if (p[(size_t)y * st + x] != (bps == 2 ? q[0] | q[1] << 8 : q[0])) {
                            same = false;
                            break;
                        }
            }
            if (same) exact_frames++;
            else if (first_bad < 0) first_bad = frames - 1;
        }
        if (!more && !got) break;
    }
    double secs = ((double)now_ms() - t0) / 1000;
    fprintf(stderr, "    %s: %ld frames, %ld damaged slices, %.1f frames a second", path, frames, damaged, secs > 0 ? frames / secs : 0);
    if (refp && *refp) fprintf(stderr, ", %ld exactly FFmpeg's (first different: %ld)", exact_frames, first_bad);
    fprintf(stderr, "\n");
    if (hevc_unsupported(d)) fprintf(stderr, "    unsupported: %s\n", hevc_unsupported(d));
    CHECK(frames > 0);
    if (refp && *refp) CHECK_INT(first_bad, -1);
    free(rb);
    if (ref) fclose(ref);
    hevc_free(d);
    if (m) mkv_close(m);
    fclose(fp);
}
