/* modules/av/mpeg4_test.c — the MPEG-4 Part 2 decoder: its code tables, streams written here whose pictures are
 * easy to know (DC prediction, vectors, half samples, video packets), the display order and block times (B-VOPs,
 * packed blocks, VOPs not coded), damage - and, opt-in, a real file against FFmpeg's frames:
 *
 *   HEMMA_TEST_MPEG4=<file.mkv|.mp4> [HEMMA_TEST_MPEG4_N=frames] [HEMMA_TEST_MPEG4_REF=<ffmpeg's .yuv>]
 *   [HEMMA_TEST_MPEG4_OUT=<ours .yuv>] ./build/test mpeg4_real
 *
 * with the reference made by `ffmpeg -i <file> -map 0:v -fps_mode passthrough -f rawvideo -pix_fmt yuv420p ref.yuv`
 * (add `-idct simple` or `-idct xvid` to see how much the IDCT alone accounts for). */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "mkv.h"
#include "mpeg4.h"
#include "mpeg4_testenc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool mpeg4_tables_ok_for_test(const char **bad);
void mpeg2_idct_for_test(int16_t block[64]); /* (the IDCT both decoders share: modules/av/idct.h) */

TEST(mpeg4_tables_are_prefix_free) {
    const char *bad = "";
    if (!mpeg4_tables_ok_for_test(&bad)) test_fail(__FILE__, __LINE__, "table %s isn't prefix-free", bad);
}

/* --------------------------------------------- reading what comes out */

typedef struct {
    int n;
    char type[16];
    int64_t pts[16];
    uint8_t y[16][32 * 32], u[16][16 * 16], v[16][16 * 16];
    int w, h, concealed;
} Out;

static void take(Mpeg4 *d, Out *o) {
    Mpeg4Frame f;
    while (mpeg4_frame(d, &f) && o->n < 16) {
        o->w = f.width, o->h = f.height, o->concealed += f.concealed;
        o->type[o->n] = f.type, o->pts[o->n] = f.pts;
        for (int j = 0; j < f.height && j < 32; j++) memcpy(o->y[o->n] + j * 32, f.y + j * f.stride_y, (size_t)f.width);
        for (int j = 0; j < f.height / 2 && j < 16; j++) {
            memcpy(o->u[o->n] + j * 16, f.u + j * f.stride_c, (size_t)f.width / 2);
            memcpy(o->v[o->n] + j * 16, f.v + j * f.stride_c, (size_t)f.width / 2);
        }
        o->n++;
    }
}

static void feed(Mpeg4 *d, const M4w *w, int64_t pts, Out *o) {
    CHECK(mpeg4_decode(d, w->b, m4_bytes(w), pts) >= 0);
    take(d, o);
}

static bool flat(const uint8_t *p, int n, int v) {
    for (int i = 0; i < n; i++)
        if (p[i] != v) return false;
    return true;
}

/* --------------------------------------------- the streams */

TEST(mpeg4_decodes_flat_intra_and_skipped_macroblocks) {
    static M4w a, b;
    static Out o;
    memset(&a, 0, sizeof a), memset(&b, 0, sizeof b), memset(&o, 0, sizeof o);
    M4Dc g = {.mbw = 2};
    m4_vol(&a, 32, 32, 0);
    m4_vop(&a, 0, 0, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_intra_mb(&a, &g, mb, 100, 60, 200, false, 0);
    m4_stuffing(&a);
    m4_vop(&b, 1, 1, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_put(&b, 1, 1); /* not coded */
    m4_stuffing(&b);

    Mpeg4 *d = mpeg4_new();
    feed(d, &a, 0, &o);
    CHECK_INT(o.n, 0); /* (held until the next reference: a B-VOP may come between) */
    int w = 0, h = 0;
    CHECK(mpeg4_info(d, &w, &h) && w == 32 && h == 32);
    CHECK(mpeg4_unsupported(d) == NULL);
    feed(d, &b, 1000, &o);
    mpeg4_flush(d);
    take(d, &o);
    CHECK_INT(o.n, 2);
    CHECK(o.type[0] == 'I' && o.type[1] == 'P');
    CHECK(o.pts[0] == 0 && o.pts[1] == 1000);
    for (int f = 0; f < 2; f++) {
        CHECK(flat(o.y[f], 32 * 32, 100));
        CHECK(flat(o.u[f], 16 * 16, 60));
        CHECK(flat(o.v[f], 16 * 16, 200));
    }
    CHECK_INT(o.concealed, 0);
    mpeg4_free(d);
}

/* Two macroblocks side by side, 50 and 151. A P-VOP moves the first half a sample right (rounding 0): its last
   column averages 50 and 151 to 101. The next (rounding 1) moves the first 8 samples right and the second half a
   sample left - predicted from the first one's vector, so coded as -17: its first column (101 + 151) / 2 = 126. */
TEST(mpeg4_vectors_and_half_samples) {
    static M4w a, p1, p2;
    static Out o;
    memset(&a, 0, sizeof a), memset(&p1, 0, sizeof p1), memset(&p2, 0, sizeof p2), memset(&o, 0, sizeof o);
    M4Dc g = {.mbw = 2};
    m4_vol(&a, 32, 16, 0);
    m4_vop(&a, 0, 0, true, 0);
    m4_intra_mb(&a, &g, 0, 50, 128, 128, false, 0);
    m4_intra_mb(&a, &g, 1, 151, 128, 128, false, 0);
    m4_stuffing(&a);
    m4_vop(&p1, 1, 1, true, 0);
    m4_moved_mb(&p1, 1, 0);
    m4_put(&p1, 1, 1);
    m4_stuffing(&p1);
    m4_vop(&p2, 1, 2, true, 1);
    m4_moved_mb(&p2, 16, 0);
    m4_moved_mb(&p2, -1 - 16, 0);
    m4_stuffing(&p2);

    Mpeg4 *d = mpeg4_new();
    feed(d, &a, 0, &o);
    feed(d, &p1, 1, &o);
    feed(d, &p2, 2, &o);
    mpeg4_flush(d);
    take(d, &o);
    CHECK_INT(o.n, 3);
    for (int row = 0; row < 16; row++) {
        const uint8_t *i0 = o.y[0] + row * 32, *f1 = o.y[1] + row * 32, *f2 = o.y[2] + row * 32;
        CHECK(i0[0] == 50 && i0[15] == 50 && i0[16] == 151 && i0[31] == 151);
        CHECK(f1[0] == 50 && f1[14] == 50 && f1[15] == 101 && f1[16] == 151 && f1[31] == 151);
        CHECK(f2[0] == 50 && f2[6] == 50 && f2[7] == 101 && f2[8] == 151 && f2[15] == 151);
        CHECK(f2[16] == 126 && f2[17] == 151 && f2[31] == 151);
    }
    CHECK(flat(o.u[2], 16 * 8, 128) && flat(o.v[2], 16 * 8, 128));
    mpeg4_free(d);
}

/* I (time 0), P (3), B (1), B (2) in decoding order - the P-VOP skips everything, so do the B-VOPs (co-located
   macroblocks skipped: no bits). Out: I B B P, each with its block's time. */
TEST(mpeg4_b_vops_come_out_in_display_order_with_their_times) {
    static M4w a, p, b1, b2;
    static Out o;
    memset(&a, 0, sizeof a), memset(&p, 0, sizeof p), memset(&b1, 0, sizeof b1), memset(&b2, 0, sizeof b2);
    memset(&o, 0, sizeof o);
    M4Dc g = {.mbw = 2};
    m4_vol(&a, 32, 32, 0);
    m4_vop(&a, 0, 0, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_intra_mb(&a, &g, mb, 90, 100, 110, false, 0);
    m4_stuffing(&a);
    m4_vop(&p, 1, 3, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_put(&p, 1, 1);
    m4_stuffing(&p);
    m4_vop(&b1, 2, 1, true, 0);
    m4_stuffing(&b1);
    m4_vop(&b2, 2, 2, true, 0);
    m4_stuffing(&b2);

    Mpeg4 *d = mpeg4_new();
    feed(d, &a, 0, &o);
    feed(d, &p, 300, &o);
    feed(d, &b1, 100, &o);
    feed(d, &b2, 200, &o);
    mpeg4_flush(d);
    take(d, &o);
    CHECK_INT(o.n, 4);
    CHECK(!memcmp(o.type, "IBBP", 4));
    for (int i = 0; i < 4; i++) {
        CHECK(o.pts[i] == i * 100);
        CHECK(flat(o.y[i], 32 * 32, 90));
    }
    /* after a seek, a B-VOP without its references is dropped - and so is its time */
    mpeg4_reset(d);
    memset(&o, 0, sizeof o);
    feed(d, &b1, 100, &o);
    feed(d, &p, 300, &o);
    mpeg4_flush(d);
    take(d, &o);
    CHECK_INT(o.n, 1);
    CHECK(o.type[0] == 'P' && o.pts[0] == 300);
    mpeg4_free(d);
}

/* DivX's packed blocks: a P- and a B-VOP in one block, an empty (not coded) VOP in the next to keep its place -
   that empty block's time is the P-VOP's. Elsewhere a not coded VOP brings nothing: the frame before shows on. */
TEST(mpeg4_packed_blocks_and_vops_not_coded) {
    static M4w a, pb, nv, p3;
    static Out o;
    memset(&a, 0, sizeof a), memset(&pb, 0, sizeof pb), memset(&nv, 0, sizeof nv), memset(&p3, 0, sizeof p3);
    memset(&o, 0, sizeof o);
    M4Dc g = {.mbw = 2};
    m4_vol(&a, 32, 32, 0);
    m4_vop(&a, 0, 0, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_intra_mb(&a, &g, mb, 70, 80, 90, false, 0);
    m4_stuffing(&a);
    m4_vop(&pb, 1, 2, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_put(&pb, 1, 1);
    m4_stuffing(&pb);
    m4_vop(&pb, 2, 1, true, 0);
    m4_stuffing(&pb);
    m4_vop(&nv, 1, 2, false, 0);
    m4_vop(&p3, 1, 3, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_put(&p3, 1, 1);
    m4_stuffing(&p3);

    Mpeg4 *d = mpeg4_new();
    feed(d, &a, 0, &o);
    feed(d, &pb, 100, &o);
    feed(d, &nv, 200, &o);
    feed(d, &p3, 300, &o);
    mpeg4_flush(d);
    take(d, &o);
    CHECK_INT(o.n, 4);
    CHECK(!memcmp(o.type, "IBPP", 4));
    for (int i = 0; i < 4; i++) CHECK(o.pts[i] == i * 100);
    mpeg4_free(d);

    d = mpeg4_new(); /* not packed: I, P, a VOP not coded, P */
    memset(&o, 0, sizeof o);
    static M4w p1;
    memset(&p1, 0, sizeof p1);
    m4_vop(&p1, 1, 1, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_put(&p1, 1, 1);
    m4_stuffing(&p1);
    feed(d, &a, 0, &o);
    feed(d, &p1, 100, &o);
    feed(d, &nv, 200, &o);
    feed(d, &p3, 300, &o);
    mpeg4_flush(d);
    take(d, &o);
    CHECK_INT(o.n, 3);
    CHECK(o.pts[0] == 0 && o.pts[1] == 100 && o.pts[2] == 300);
    mpeg4_free(d);
}

/* Video packets: four macroblocks, a resync marker before the third (DC prediction starts afresh there). Then
   the second macroblock damaged: it's patched up (grey - there's no reference) and decoding goes on at the
   marker. */
TEST(mpeg4_video_packets_and_damage) {
    static M4w a, bad;
    static Out o;
    for (int broken = 0; broken < 2; broken++) {
        M4w *w = broken ? &bad : &a;
        memset(w, 0, sizeof *w);
        memset(&o, 0, sizeof o);
        M4Dc g = {.mbw = 2};
        m4_vol(w, 32, 32, M4_RESYNC);
        m4_vop(w, 0, 0, true, 0);
        m4_intra_mb(w, &g, 0, 40, 128, 128, false, 0);
        if (broken) m4_put(w, 0, 9); /* no mcbpc starts with nine zeros (that's stuffing, which needs its 1) */
        else m4_intra_mb(w, &g, 1, 60, 128, 128, false, 0);
        m4_stuffing(w);
        m4_put(w, 1, 17);              /* resync_marker: 16 zeros and a 1 */
        m4_put(w, 2, 2), m4_put(w, 4, 5), m4_put(w, 0, 1); /* macroblock_number 2, quant_scale 4, no header extension */
        g.pkt = 2;
        m4_intra_mb(w, &g, 2, 80, 128, 128, false, 0);
        m4_intra_mb(w, &g, 3, 100, 128, 128, false, 0);
        m4_stuffing(w);

        Mpeg4 *d = mpeg4_new();
        feed(d, w, 0, &o);
        mpeg4_flush(d);
        take(d, &o);
        CHECK_INT(o.n, 1);
        CHECK_INT(o.concealed, broken ? 1 : 0);
        for (int row = 0; row < 16; row++) {
            CHECK(flat(o.y[0] + row * 32, 16, 40));
            CHECK(flat(o.y[0] + row * 32 + 16, 16, broken ? 128 : 60));
            CHECK(flat(o.y[0] + (16 + row) * 32, 16, 80));
            CHECK(flat(o.y[0] + (16 + row) * 32 + 16, 16, 100));
        }
        mpeg4_free(d);
    }
}

static uint32_t rnd_state = 4711;
static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13, rnd_state ^= rnd_state >> 17, rnd_state ^= rnd_state << 5;
    return rnd_state;
}

TEST(mpeg4_survives_damaged_streams) {
    static M4w a, p;
    memset(&a, 0, sizeof a), memset(&p, 0, sizeof p);
    M4Dc g = {.mbw = 2};
    m4_vol(&a, 32, 32, M4_RESYNC);
    m4_vop(&a, 0, 0, true, 0);
    for (int mb = 0; mb < 4; mb++) m4_intra_mb(&a, &g, mb, 30 + 40 * mb, 100, 150, false, 0);
    m4_stuffing(&a);
    m4_vop(&p, 1, 1, true, 1);
    m4_moved_mb(&p, 3, -2);
    m4_put(&p, 1, 1);
    m4_moved_mb(&p, -5, 7);
    m4_put(&p, 1, 1);
    m4_stuffing(&p);
    uint8_t buf[512];
    for (int round = 0; round < 3000; round++) {
        Mpeg4 *d = mpeg4_new();
        for (int k = 0; k < 6; k++) {
            const M4w *src = k == 0 ? &a : &p;
            size_t n = m4_bytes(src);
            if (n > sizeof buf) n = sizeof buf;
            memcpy(buf, src->b, n);
            int flips = (int)(rnd() % 8);
            for (int f = 0; f < flips; f++) buf[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
            if (round % 7 == 0) n = rnd() % (n + 1); /* cut short */
            if (round % 11 == 0)
                for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd(); /* noise */
            CHECK(mpeg4_decode(d, buf, n, k * 100) >= 0);
            Mpeg4Frame f;
            while (mpeg4_frame(d, &f)) CHECK(f.width > 0 && f.height > 0 && f.y && f.u && f.v);
        }
        mpeg4_flush(d);
        Mpeg4Frame f;
        while (mpeg4_frame(d, &f)) {}
        mpeg4_free(d);
    }
}

/* An intra block's AC coefficient (escape 3, level 9 at scan position 1 = raster 1): dequantised as H.263 says,
   (2|QF| + 1)Q - 1 for an even Q (4: 75), beside its DC (8 x 100), through the IDCT. */
TEST(mpeg4_dequantises_an_escaped_coefficient) {
    static M4w a;
    static Out o;
    memset(&a, 0, sizeof a), memset(&o, 0, sizeof o);
    M4Dc g = {.mbw = 1};
    m4_vol(&a, 16, 16, 0);
    m4_vop(&a, 0, 0, true, 0);
    m4_intra_mb(&a, &g, 0, 100, 128, 128, false, 9);
    m4_stuffing(&a);
    Mpeg4 *d = mpeg4_new();
    feed(d, &a, 0, &o);
    mpeg4_flush(d);
    take(d, &o);
    CHECK_INT(o.n, 1);
    int16_t blk[64] = {800, 2 * 9 * 4 + 3};
    mpeg2_idct_for_test(blk);
    bool same = true;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) same &= o.y[0][j * 32 + i] == (blk[j * 8 + i] < 0 ? 0 : blk[j * 8 + i] > 255 ? 255 : blk[j * 8 + i]);
    CHECK(same);
    CHECK(o.y[0][0] != o.y[0][7]);                 /* (a horizontal ripple: not flat any more) */
    CHECK(flat(o.y[0] + 8, 8, 100));               /* block 1 untouched */
    mpeg4_free(d);
}

/* Whole streams (mpeg4_test_block: most of the syntax) in every way the writer makes them: every picture comes
   out, in display order with its time, nothing patched up. */
TEST(mpeg4_decodes_whole_streams) {
    static uint8_t buf[32768];
    static const struct {
        int w, h;
        unsigned flags;
        bool packed;
    } V[] = {{48, 32, 0, false},         {72, 40, M4_RESYNC, false}, {64, 48, M4_MPEGQ | M4_QPEL, false},
             {40, 24, 0, true},          {56, 32, M4_RESYNC | M4_MPEGQ, true}};
    for (size_t v = 0; v < sizeof V / sizeof *V; v++) {
        Mpeg4 *d = mpeg4_new();
        int blocks = 1 + 3 * 4, frames = 0, concealed = 0;
        int64_t last = -1000;
        bool ordered = true;
        for (int k = 0; k <= blocks; k++) {
            if (k == blocks) mpeg4_flush(d);
            else {
                int disp = 0;
                size_t n = mpeg4_test_block(buf, sizeof buf, k, V[v].w, V[v].h, V[v].flags, V[v].packed, &disp);
                CHECK(mpeg4_decode(d, buf, n, disp * 1000) >= 0);
            }
            Mpeg4Frame f;
            while (mpeg4_frame(d, &f)) {
                frames++;
                concealed += f.concealed;
                ordered &= f.pts == last + 1000;
                last = f.pts;
                CHECK(f.width == V[v].w && f.height == V[v].h);
            }
        }
        if (frames != blocks || concealed || !ordered)
            test_fail(__FILE__, __LINE__, "stream %d: %d frames of %d, %d concealed, %s", (int)v, frames, blocks, concealed,
                      ordered ? "in order" : "out of order");
        mpeg4_free(d);
    }
}

TEST(mpeg4_names_what_it_doesnt_decode) {
    static uint8_t buf[32768];
    static const struct {
        unsigned flags;
        const char *why;
    } V[] = {{M4_INTERLACED, "interlaced coding"}, {M4_GMC, "global motion compensation (GMC)"}};
    for (size_t v = 0; v < 2; v++) {
        Mpeg4 *d = mpeg4_new();
        int disp, frames = 0;
        for (int k = 0; k < 4; k++) {
            size_t n = mpeg4_test_block(buf, sizeof buf, k, 32, 32, V[v].flags, false, &disp);
            mpeg4_decode(d, buf, n, disp);
        }
        mpeg4_flush(d);
        Mpeg4Frame f;
        while (mpeg4_frame(d, &f)) frames++;
        CHECK_INT(frames, 0);
        CHECK(mpeg4_unsupported(d) && !strcmp(mpeg4_unsupported(d), V[v].why));
        mpeg4_free(d);
    }
}

/* --------------------------------------------- a real file (opt-in) */

static bool read_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

static double psnr(const uint8_t *a, int as, const uint8_t *b, int bs, int w, int h, int *maxd) {
    double se = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int d = a[j * as + i] - b[j * bs + i];
            se += d * d;
            if (abs(d) > *maxd) *maxd = abs(d);
        }
    return se ? 10 * log10(255.0 * 255.0 * w * h / se) : 99;
}

TEST(mpeg4_real) {
    const char *path = plat_getenv("HEMMA_TEST_MPEG4");
    if (!path || !*path) return;
    const char *ns = plat_getenv("HEMMA_TEST_MPEG4_N"), *refp = plat_getenv("HEMMA_TEST_MPEG4_REF"),
               *outp = plat_getenv("HEMMA_TEST_MPEG4_OUT");
    long want = ns && *ns ? strtol(ns, NULL, 10) : 300;
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    if (!fp) return;
    MkvReader r = {fp, size, read_cb};
    char err[160];
    Mkv *m = mkv_open(&r, err, sizeof err);
    CHECK(m);
    if (!m) return;
    const MkvTrack *v = mkv_find_track(m, MKV_VIDEO);
    CHECK(v && !strncmp(v->codec, "V_MPEG4/ISO/", 12) && strcmp(v->codec, "V_MPEG4/ISO/AVC"));
    FILE *ref = refp && *refp ? file_open(refp, "rb") : NULL, *out = outp && *outp ? file_open(outp, "wb") : NULL;
    Mpeg4 *d = mpeg4_new();
    mpeg4_decode(d, v->priv, v->priv_len, MPEG4_NOPTS);
    long frames = 0, concealed = 0, below40 = 0, types[4] = {0}, worst_at = -1;
    double sum = 0, worst = 99, worst_c = 99;
    int maxd = 0;
    int64_t last = INT64_MIN;
    bool ordered = true;
    uint8_t *rb = NULL;
    MkvFrame f;
    for (bool more = true; frames < want;) {
        if (more && mkv_next(m, &f) == 1) {
            if (f.track != v->number) continue;
            mpeg4_decode(d, f.data, f.size, f.pts);
        } else if (more) {
            more = false;
            mpeg4_flush(d);
        } else break;
        Mpeg4Frame fr;
        while (frames < want && mpeg4_frame(d, &fr)) {
            frames++;
            concealed += fr.concealed;
            types[fr.type == 'I' ? 0 : fr.type == 'P' ? 1 : fr.type == 'B' ? 2 : 3]++;
            if (fr.pts != MPEG4_NOPTS) {
                ordered &= fr.pts > last;
                last = fr.pts;
            }
            int cw = (fr.width + 1) / 2, ch = (fr.height + 1) / 2;
            size_t fsz = (size_t)fr.width * fr.height + 2 * (size_t)cw * ch;
            if (out) {
                for (int j = 0; j < fr.height; j++) fwrite(fr.y + j * fr.stride_y, 1, (size_t)fr.width, out);
                for (int j = 0; j < ch; j++) fwrite(fr.u + j * fr.stride_c, 1, (size_t)cw, out);
                for (int j = 0; j < ch; j++) fwrite(fr.v + j * fr.stride_c, 1, (size_t)cw, out);
            }
            if (ref) {
                if (!rb) rb = malloc(fsz);
                if (!rb || fread(rb, 1, fsz, ref) != fsz) {
                    fprintf(stderr, "    the reference ends at frame %ld\n", frames - 1);
                    fclose(ref);
                    ref = NULL;
                    continue;
                }
                int md = 0;
                double py = psnr(fr.y, fr.stride_y, rb, fr.width, fr.width, fr.height, &md);
                const uint8_t *ru = rb + (size_t)fr.width * fr.height, *rv = ru + (size_t)cw * ch;
                double pu = psnr(fr.u, fr.stride_c, ru, cw, cw, ch, &md), pv = psnr(fr.v, fr.stride_c, rv, cw, cw, ch, &md);
                double pc = pu < pv ? pu : pv;
                sum += py;
                if (md > maxd) maxd = md;
                if (py < 40) below40++;
                if (py < worst) worst = py, worst_at = frames - 1;
                if (pc < worst_c) worst_c = pc;
            }
        }
    }
    fprintf(stderr, "    %s: %ld frames (I %ld, P %ld, B %ld, S %ld), %ld macroblocks concealed, times %s\n", path, frames,
            types[0], types[1], types[2], types[3], concealed, ordered ? "in order" : "OUT OF ORDER");
    if (mpeg4_unsupported(d)) fprintf(stderr, "    unsupported: %s\n", mpeg4_unsupported(d));
    if (refp && *refp && frames)
        fprintf(stderr, "    vs the reference: luma PSNR average %.2f dB, worst %.2f dB (frame %ld), chroma worst %.2f dB, "
                        "%ld frames under 40 dB, largest difference %d\n",
                sum / (double)frames, worst, worst_at, worst_c, below40, maxd);
    CHECK(frames > 0);
    CHECK(ordered);
    free(rb);
    if (ref) fclose(ref);
    if (out) fclose(out);
    mpeg4_free(d);
    mkv_close(m);
    fclose(fp);
}
