/* modules/av/mpeg2_test.c — IDCT accuracy, VLC tables, and decoding small
 * streams written here bit by bit (I/P/B reordering, motion vectors, skipped
 * macroblocks, escapes, field pictures), plus damaged input. A real file is
 * decoded when HEMMA_TEST_MKV points at a MakeMKV/DVD .mkv. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "mkv.h"
#include "mpeg2.h"

#include <math.h>

void mpeg2_idct_for_test(int16_t block[64]);
const char *mpeg2_selftest(void);

/* ------------------------------------------------------- IEEE 1180-1990 */

static uint32_t ieee_randx;

static long ieee_rand(long l, long h) { /* the generator the standard prescribes (32-bit wrap-around) */
    static const double z = (double)0x7fffffff;
    ieee_randx = ieee_randx * 1103515245u + 12345u;
    double x = (double)(ieee_randx & 0x7ffffffe) / z * (double)(l + h + 1);
    return (long)x - l;
}

static double cosines[8][8]; /* c[u][x] = C(u)/2 * cos((2x+1) u pi / 16) */

static void init_cosines(void) {
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++)
            cosines[u][x] = (u ? 0.5 : 0.5 / sqrt(2.0)) * cos((2 * x + 1) * u * 3.14159265358979323846 / 16);
}

static void fdct_ref(const double in[64], double out[64]) {
    double t[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            double s = 0;
            for (int x = 0; x < 8; x++) s += cosines[u][x] * in[y * 8 + x];
            t[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            double s = 0;
            for (int y = 0; y < 8; y++) s += cosines[v][y] * t[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

static void idct_ref(const double in[64], double out[64]) {
    double t[64];
    for (int v = 0; v < 8; v++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int u = 0; u < 8; u++) s += cosines[u][x] * in[v * 8 + u];
            t[v * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int v = 0; v < 8; v++) s += cosines[v][y] * t[v * 8 + x];
            out[y * 8 + x] = s;
        }
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* One IEEE 1180 run: 10000 random blocks in [-l, h] (times sign). */
static const char *ieee_run(long l, long h, int sign) {
    static char why[160];
    double err_sum[64] = {0}, err_sq[64] = {0};
    int peak = 0;
    ieee_randx = 1;
    for (int n = 0; n < 10000; n++) {
        double px[64], coef[64], ref[64];
        int16_t blk[64];
        for (int i = 0; i < 64; i++) px[i] = (double)(ieee_rand(l, h) * sign);
        fdct_ref(px, coef);
        for (int i = 0; i < 64; i++) {
            coef[i] = clampd(floor(coef[i] + 0.5), -2048, 2047);
            blk[i] = (int16_t)coef[i];
        }
        idct_ref(coef, ref);
        mpeg2_idct_for_test(blk);
        for (int i = 0; i < 64; i++) {
            int r = (int)clampd(floor(ref[i] + 0.5), -256, 255), e = blk[i] - r;
            if (abs(e) > peak) peak = abs(e);
            err_sum[i] += e;
            err_sq[i] += (double)e * e;
        }
    }
    double total = 0, total_sq = 0, worst_mse = 0, worst_me = 0;
    for (int i = 0; i < 64; i++) {
        total += err_sum[i];
        total_sq += err_sq[i];
        if (err_sq[i] / 10000 > worst_mse) worst_mse = err_sq[i] / 10000;
        if (fabs(err_sum[i]) / 10000 > worst_me) worst_me = fabs(err_sum[i]) / 10000;
    }
    double omse = total_sq / 640000, ome = fabs(total) / 640000;
    if (peak > 1 || worst_mse > 0.06 || omse > 0.02 || worst_me > 0.015 || ome > 0.0015) {
        snprintf(why, sizeof why, "L=%ld H=%ld sign=%d: peak %d, pixel mse %.4f, mse %.4f, pixel me %.4f, me %.5f", l, h,
                 sign, peak, worst_mse, omse, worst_me, ome);
        return why;
    }
    return NULL;
}

TEST(mpeg2_idct_meets_ieee_1180) {
    init_cosines();
    static const long ranges[3][2] = {{256, 255}, {5, 5}, {300, 300}};
    for (int r = 0; r < 3; r++) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            const char *why = ieee_run(ranges[r][0], ranges[r][1], sign);
            if (why) test_fail(__FILE__, __LINE__, "%s", why);
            CHECK(why == NULL);
        }
    }
    int16_t zero[64] = {0};
    mpeg2_idct_for_test(zero);
    for (int i = 0; i < 64; i++) CHECK_INT(zero[i], 0);
}

TEST(mpeg2_vlc_tables_are_prefix_free_and_decode) {
    const char *bad = mpeg2_selftest();
    if (bad) test_fail(__FILE__, __LINE__, "broken table: %s", bad);
    CHECK(bad == NULL);
}

/* ------------------------------------------------- writing tiny streams */

typedef struct {
    uint8_t b[16384];
    size_t n; /* bits */
} Bw;

static void put(Bw *w, uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--, w->n++)
        if (v >> i & 1) w->b[w->n >> 3] |= (uint8_t)(0x80 >> (w->n & 7));
}

static void start_code(Bw *w, int code) {
    w->n = (w->n + 7) & ~(size_t)7;
    put(w, 1, 24);
    put(w, (uint32_t)code, 8);
}

static size_t bytes(const Bw *w) { return (w->n + 7) / 8; }

static void seq_header(Bw *w, int width, int height, bool progressive) {
    start_code(w, 0xB3);
    put(w, (uint32_t)width, 12);
    put(w, (uint32_t)height, 12);
    put(w, 1, 4);        /* square samples */
    put(w, 4, 4);        /* 29.97 */
    put(w, 0x3FFFF, 18); /* bit rate */
    put(w, 1, 1);
    put(w, 112, 10);
    put(w, 0, 3); /* constrained, no matrices */
    start_code(w, 0xB5);
    put(w, 1, 4);
    put(w, 0x48, 8);
    put(w, progressive, 1);
    put(w, 1, 2); /* 4:2:0 */
    put(w, 0, 4);
    put(w, 0, 12);
    put(w, 1, 1);
    put(w, 0, 8);
    put(w, 0, 1);
    put(w, 0, 7);
}

static void gop(Bw *w, bool closed) {
    start_code(w, 0xB8);
    put(w, 0, 25);
    put(w, closed, 1);
    put(w, 0, 1);
}

typedef struct {
    int type, tref, f[2][2], structure;
    bool fpfd, progressive, tff;
} Pic;

static void picture(Bw *w, const Pic *p) {
    start_code(w, 0x00);
    put(w, (uint32_t)p->tref, 10);
    put(w, (uint32_t)p->type, 3);
    put(w, 0xFFFF, 16);
    if (p->type >= 2) put(w, 7, 4);
    if (p->type == 3) put(w, 7, 4);
    put(w, 0, 1);
    start_code(w, 0xB5);
    put(w, 8, 4);
    for (int s = 0; s < 2; s++)
        for (int t = 0; t < 2; t++) put(w, (uint32_t)p->f[s][t], 4);
    put(w, 3, 2); /* intra_dc_precision 11 bits: odd DC values give flat blocks without mismatch control */
    put(w, (uint32_t)p->structure, 2);
    put(w, p->tff, 1);
    put(w, p->fpfd, 1);
    put(w, 0, 4); /* concealment, q_scale_type, intra_vlc_format, alternate_scan */
    put(w, 0, 1); /* repeat_first_field */
    put(w, 1, 1); /* chroma_420_type */
    put(w, p->progressive, 1);
    put(w, 0, 1);
}

static void slice(Bw *w, int row, int qcode) {
    start_code(w, row + 1);
    put(w, (uint32_t)qcode, 5);
    put(w, 0, 1);
}

static void mba(Bw *w, int inc) {
    static const uint8_t codes[4][2] = {{0, 0}, {1, 1}, {3, 3}, {2, 3}};
    put(w, codes[inc][0], codes[inc][1]);
}

static void dc(Bw *w, int comp, int diff) {
    static const uint16_t lum[12][2] = {{4, 3}, {0, 2}, {1, 2}, {5, 3}, {6, 3}, {14, 4},
                                        {30, 5}, {62, 6}, {126, 7}, {254, 8}, {510, 9}, {511, 9}};
    static const uint16_t chrom[12][2] = {{0, 2}, {1, 2}, {2, 2}, {6, 3}, {14, 4}, {30, 5},
                                          {62, 6}, {126, 7}, {254, 8}, {510, 9}, {1022, 10}, {1023, 10}};
    int a = abs(diff), size = 0;
    while (a >> size) size++;
    if (comp) put(w, chrom[size][0], chrom[size][1]);
    else put(w, lum[size][0], lum[size][1]);
    if (size) put(w, (uint32_t)(diff > 0 ? diff : diff + (1 << size) - 1), size);
}

/* An intra macroblock of flat blocks: levels for Y, Cb, Cr (dc = 8 * level + 1). */
static void intra_flat(Bw *w, int pred[3], int y, int cb, int cr) {
    int lv[3] = {y, cb, cr};
    for (int k = 0; k < 6; k++) {
        int c = k < 4 ? 0 : k - 3, v = 8 * lv[c] + 1;
        dc(w, c, v - pred[c]);
        pred[c] = v;
        put(w, 2, 2); /* end of block */
    }
}

static void reset_pred(int pred[3]) { pred[0] = pred[1] = pred[2] = 1024; }

static void mv(Bw *w, int fcode, int delta) {
    static const uint8_t codes[17][2] = {{1, 1}, {1, 2}, {1, 3}, {1, 4}, {3, 6}, {5, 7}, {4, 7}, {3, 7}, {11, 9},
                                         {10, 9}, {9, 9}, {17, 10}, {16, 10}, {15, 10}, {14, 10}, {13, 10}, {12, 10}};
    if (!delta) {
        put(w, 1, 1);
        return;
    }
    int r = fcode - 1, f = 1 << r, a = abs(delta), code = (a - 1) / f + 1;
    put(w, codes[code][0], codes[code][1]);
    put(w, delta < 0, 1);
    if (r) put(w, (uint32_t)((a - 1) % f), r);
}

typedef struct {
    int n;
    Mpeg2Frame f[8];
    uint8_t y[8][48 * 32]; /* copies: frames are only valid until the next call */
} Out;

static void collect(Mpeg2 *d, Out *o) {
    Mpeg2Frame f;
    while (mpeg2_frame(d, &f) && o->n < 8) {
        o->f[o->n] = f;
        for (int y = 0; y < f.height && y < 32; y++)
            for (int x = 0; x < f.width && x < 48; x++) o->y[o->n][y * 48 + x] = f.y[y * f.stride_y + x];
        o->n++;
    }
}

static int luma(const Out *o, int frame, int x, int y) { return o->y[frame][y * 48 + x]; }

/* I (tref 0), P (tref 2), B (tref 1) on a 48x32 picture (3x2 macroblocks):
 *   I: flat blocks   100  60 140 / 200  30  90
 *   P: (0,0) MC from (+16,0) -> 60; (1,0) skipped -> 60; (2,0) MC from (0,+16) -> 90
 *      (0,1) intra 150; (1,1) no MC + residual +5 in block 0; (2,1) MC from (0,-16) -> 140
 *   B: (0,0) interpolated (100+60)/2; (1,0) skipped = interpolated; (2,0) backward 90
 *      (0,1) forward 200; (1,1) skipped = forward 30; (2,1) interpolated (90+140)/2 */
static void ipb_stream(Bw *i, Bw *p, Bw *b) {
    int pred[3];
    seq_header(i, 48, 32, true);
    gop(i, true);
    Pic hdr = {.type = 1, .tref = 0, .f = {{15, 15}, {15, 15}}, .structure = 3, .fpfd = true, .progressive = true};
    picture(i, &hdr);
    static const int ylev[2][3] = {{100, 60, 140}, {200, 30, 90}};
    for (int row = 0; row < 2; row++) {
        slice(i, row, 8);
        reset_pred(pred);
        for (int x = 0; x < 3; x++) {
            mba(i, 1);
            put(i, 1, 1); /* intra */
            bool special = row == 1 && x == 2;
            intra_flat(i, pred, ylev[row][x], special ? 100 : 128, special ? 150 : 128);
        }
    }

    hdr = (Pic){.type = 2, .tref = 2, .f = {{3, 3}, {15, 15}}, .structure = 3, .fpfd = true, .progressive = true};
    picture(p, &hdr);
    slice(p, 0, 8);
    mba(p, 1);
    put(p, 1, 3); /* MC, not coded */
    mv(p, 3, 32);
    mv(p, 3, 0);
    mba(p, 2); /* (1,0) skipped */
    put(p, 1, 3);
    mv(p, 3, 0); /* predictors were reset by the skip */
    mv(p, 3, 32);
    slice(p, 1, 1);
    reset_pred(pred);
    mba(p, 1);
    put(p, 3, 5); /* intra */
    intra_flat(p, pred, 150, 128, 128);
    mba(p, 1);
    put(p, 1, 2);    /* no MC, coded */
    put(p, 0xa, 4);  /* coded_block_pattern 32: block 0 only */
    put(p, 1, 6);    /* escape */
    put(p, 0, 6);    /* run 0 */
    put(p, 20, 12);  /* level 20: (2*20+1) * 2 * 16 / 32 = 41 -> +5 */
    put(p, 2, 2);    /* end of block */
    mba(p, 1);
    put(p, 1, 3); /* MC, not coded */
    mv(p, 3, 0);
    mv(p, 3, -32);

    hdr = (Pic){.type = 3, .tref = 1, .f = {{1, 1}, {1, 1}}, .structure = 3, .fpfd = true, .progressive = true};
    picture(b, &hdr);
    slice(b, 0, 8);
    mba(b, 1);
    put(b, 2, 2); /* interpolated, not coded */
    mv(b, 1, 0), mv(b, 1, 0), mv(b, 1, 0), mv(b, 1, 0);
    mba(b, 2); /* (1,0) skipped */
    put(b, 2, 3); /* backward, not coded */
    mv(b, 1, 0), mv(b, 1, 0);
    slice(b, 1, 8);
    mba(b, 1);
    put(b, 2, 4); /* forward, not coded */
    mv(b, 1, 0), mv(b, 1, 0);
    mba(b, 2); /* (1,1) skipped */
    put(b, 2, 2); /* interpolated */
    mv(b, 1, 0), mv(b, 1, 0), mv(b, 1, 0), mv(b, 1, 0);
}

TEST(mpeg2_decodes_ipb_in_display_order) {
    static Bw i, p, b;
    memset(&i, 0, sizeof i), memset(&p, 0, sizeof p), memset(&b, 0, sizeof b);
    ipb_stream(&i, &p, &b);
    Mpeg2 *d = mpeg2_new();
    static Out o;
    memset(&o, 0, sizeof o);
    CHECK_INT(mpeg2_decode(d, i.b, bytes(&i), 1000), 0); /* the I waits for the next reference */
    CHECK_INT(mpeg2_decode(d, p.b, bytes(&p), 3000), 1); /* now the I */
    collect(d, &o);
    CHECK_INT(mpeg2_decode(d, b.b, bytes(&b), 2000), 1); /* the B, right away */
    collect(d, &o);
    mpeg2_flush(d); /* and the P */
    collect(d, &o);
    CHECK_INT(o.n, 3);
    CHECK_INT(o.f[0].type, 'I');
    CHECK_INT(o.f[1].type, 'B');
    CHECK_INT(o.f[2].type, 'P');
    CHECK_INT(o.f[0].pts, 1000);
    CHECK_INT(o.f[1].pts, 2000);
    CHECK_INT(o.f[2].pts, 3000);
    CHECK_INT(o.f[0].width, 48);
    CHECK_INT(o.f[0].height, 32);
    CHECK_INT(o.f[0].rate_num, 30000);
    CHECK_INT(o.f[0].rate_den, 1001);
    CHECK_INT(o.f[0].dar_num, 3);
    CHECK_INT(o.f[0].dar_den, 2);
    for (int k = 0; k < 3; k++) CHECK_INT(o.f[k].concealed, 0);
    /* I */
    static const int ilev[2][3] = {{100, 60, 140}, {200, 30, 90}};
    for (int row = 0; row < 2; row++)
        for (int x = 0; x < 3; x++) {
            CHECK_INT(luma(&o, 0, 16 * x + 3, 16 * row + 5), ilev[row][x]);
            CHECK_INT(luma(&o, 0, 16 * x + 15, 16 * row + 15), ilev[row][x]);
        }
    /* P */
    CHECK_INT(luma(&o, 2, 4, 4), 60);
    CHECK_INT(luma(&o, 2, 20, 4), 60);
    CHECK_INT(luma(&o, 2, 40, 12), 90);
    CHECK_INT(luma(&o, 2, 4, 20), 150);
    CHECK_INT(luma(&o, 2, 18, 18), 35); /* block 0 of (1,1): 30 + 5 */
    CHECK_INT(luma(&o, 2, 28, 18), 30); /* block 1 untouched */
    CHECK_INT(luma(&o, 2, 18, 28), 30);
    CHECK_INT(luma(&o, 2, 40, 24), 140);
    /* B */
    CHECK_INT(luma(&o, 1, 4, 4), 80);
    CHECK_INT(luma(&o, 1, 20, 4), 60);
    CHECK_INT(luma(&o, 1, 40, 4), 90);
    CHECK_INT(luma(&o, 1, 4, 20), 200);
    CHECK_INT(luma(&o, 1, 20, 20), 30);
    CHECK_INT(luma(&o, 1, 40, 20), 115);
    mpeg2_free(d);
}

TEST(mpeg2_chroma_follows_the_vectors) {
    static Bw i, p, b;
    memset(&i, 0, sizeof i), memset(&p, 0, sizeof p), memset(&b, 0, sizeof b);
    ipb_stream(&i, &p, &b);
    Mpeg2 *d = mpeg2_new();
    mpeg2_decode(d, i.b, bytes(&i), 0);
    mpeg2_decode(d, p.b, bytes(&p), 2);
    Mpeg2Frame f;
    CHECK(mpeg2_frame(d, &f)); /* I */
    CHECK_INT(f.u[12 * f.stride_c + 20], 100); /* (2,1) Cb */
    CHECK_INT(f.v[12 * f.stride_c + 20], 150);
    CHECK_INT(f.u[4 * f.stride_c + 20], 128); /* (2,0) */
    mpeg2_decode(d, b.b, bytes(&b), 1);
    CHECK(mpeg2_frame(d, &f)); /* B: (2,1) interpolated with the P's 128 */
    CHECK_INT(f.u[12 * f.stride_c + 20], 114);
    CHECK_INT(f.v[12 * f.stride_c + 20], 139);
    mpeg2_flush(d);
    CHECK(mpeg2_frame(d, &f)); /* P: (2,0) took (2,1)'s colours through the vertical vector */
    CHECK_INT(f.u[4 * f.stride_c + 20], 100);
    CHECK_INT(f.v[4 * f.stride_c + 20], 150);
    mpeg2_free(d);
}

TEST(mpeg2_drops_pictures_without_references) {
    static Bw i, p, b;
    memset(&i, 0, sizeof i), memset(&p, 0, sizeof p), memset(&b, 0, sizeof b);
    ipb_stream(&i, &p, &b);
    Mpeg2 *d = mpeg2_new();
    mpeg2_decode(d, i.b, bytes(&i), 0); /* learn the sequence */
    mpeg2_reset(d);                     /* a seek */
    CHECK_INT(mpeg2_decode(d, b.b, bytes(&b), 1), 0); /* B without references: dropped */
    CHECK_INT(mpeg2_decode(d, p.b, bytes(&p), 2), 0); /* P without a reference: dropped */
    mpeg2_flush(d);
    Mpeg2Frame f;
    CHECK(!mpeg2_frame(d, &f));
    /* an I starts things again */
    CHECK_INT(mpeg2_decode(d, i.b, bytes(&i), 10), 0);
    CHECK_INT(mpeg2_decode(d, p.b, bytes(&p), 12), 1);
    CHECK(mpeg2_frame(d, &f));
    CHECK_INT(f.pts, 10);
    mpeg2_free(d);
}

/* Interlaced 32x32: A = I field pair (top 100, bottom 50); B = P frame picture
 * with field prediction swapping the fields of A; C = P field pair whose second
 * field predicts from its own first field. */
static void field_stream(Bw *a, Bw *bb, Bw *c) {
    int pred[3];
    seq_header(a, 32, 32, false);
    gop(a, true);
    for (int f = 0; f < 2; f++) {
        Pic hdr = {.type = 1, .tref = 0, .f = {{15, 15}, {15, 15}}, .structure = f ? 2 : 1, .tff = true};
        picture(a, &hdr);
        slice(a, 0, 8);
        reset_pred(pred);
        for (int x = 0; x < 2; x++) {
            mba(a, 1);
            put(a, 1, 1);
            intra_flat(a, pred, f ? 50 : 100, 128, 128);
        }
    }
    Pic hdr = {.type = 2, .tref = 1, .f = {{1, 1}, {15, 15}}, .structure = 3, .tff = true};
    picture(bb, &hdr);
    for (int row = 0; row < 2; row++) {
        slice(bb, row, 8);
        for (int x = 0; x < 2; x++) {
            mba(bb, 1);
            put(bb, 1, 3); /* MC, not coded */
            put(bb, 1, 2); /* frame_motion_type: field */
            put(bb, 1, 1); /* top lines from the bottom field */
            mv(bb, 1, 0), mv(bb, 1, 0);
            put(bb, 0, 1); /* bottom lines from the top field */
            mv(bb, 1, 0), mv(bb, 1, 0);
        }
    }
    for (int f = 0; f < 2; f++) {
        hdr = (Pic){.type = 2, .tref = 2, .f = {{1, 1}, {15, 15}}, .structure = f ? 2 : 1, .tff = true};
        picture(c, &hdr);
        slice(c, 0, 8);
        for (int x = 0; x < 2; x++) {
            mba(c, 1);
            put(c, 1, 3);       /* MC, not coded */
            put(c, 1, 2);       /* field_motion_type: field */
            put(c, f ? 0 : 1, 1); /* top field from B's bottom; bottom field from this frame's top */
            mv(c, 1, 0), mv(c, 1, 0);
        }
    }
}

TEST(mpeg2_field_pictures_and_field_prediction) {
    static Bw a, b, c;
    memset(&a, 0, sizeof a), memset(&b, 0, sizeof b), memset(&c, 0, sizeof c);
    field_stream(&a, &b, &c);
    Mpeg2 *d = mpeg2_new();
    static Out o;
    memset(&o, 0, sizeof o);
    mpeg2_decode(d, a.b, bytes(&a), 1);
    collect(d, &o);
    mpeg2_decode(d, b.b, bytes(&b), 2);
    collect(d, &o);
    mpeg2_decode(d, c.b, bytes(&c), 3);
    collect(d, &o);
    mpeg2_flush(d);
    collect(d, &o);
    CHECK_INT(o.n, 3);
    for (int k = 0; k < 3; k++) {
        CHECK_INT(o.f[k].pts, k + 1);
        CHECK_INT(o.f[k].concealed, 0);
    }
    CHECK(!o.f[0].progressive_frame);
    CHECK(o.f[0].top_field_first);
    for (int y = 0; y < 32; y += 7) {
        CHECK_INT(luma(&o, 0, 9, y), y & 1 ? 50 : 100);
        CHECK_INT(luma(&o, 1, 25, y), y & 1 ? 100 : 50);
        CHECK_INT(luma(&o, 2, 9, y), 100);
    }
    mpeg2_free(d);
}

/* A stream (or a seek) that starts on an I field + P field frame: the P field's own parity would come
 * from the previous frame, which isn't there. Nightly fuzzing (2026-10-22) found the decoder then read
 * the very field it was writing - overlapping copies. The first field stands in now. 48x32: an I top
 * field of 100, then a P bottom field: (0) from its own parity 1 px to the right, (1) skipped (own
 * parity, zero vector), (2) from its own parity 1 px to the left. */
TEST(mpeg2_second_field_without_a_previous_frame) {
    static Bw w;
    memset(&w, 0, sizeof w);
    int pred[3];
    seq_header(&w, 48, 32, false);
    gop(&w, true);
    Pic hdr = {.type = 1, .tref = 0, .f = {{15, 15}, {15, 15}}, .structure = 1, .tff = true};
    picture(&w, &hdr);
    slice(&w, 0, 8);
    reset_pred(pred);
    for (int x = 0; x < 3; x++) {
        mba(&w, 1);
        put(&w, 1, 1);
        intra_flat(&w, pred, 100, 128, 128);
    }
    hdr = (Pic){.type = 2, .tref = 0, .f = {{1, 1}, {15, 15}}, .structure = 2, .tff = true};
    picture(&w, &hdr);
    slice(&w, 0, 8);
    mba(&w, 1);
    put(&w, 1, 3); /* MC, not coded */
    put(&w, 1, 2); /* field */
    put(&w, 1, 1); /* the bottom field: this one's own parity */
    mv(&w, 1, 2), mv(&w, 1, 0);
    mba(&w, 2); /* (1) skipped */
    put(&w, 1, 3);
    put(&w, 1, 2);
    put(&w, 1, 1);
    mv(&w, 1, -2), mv(&w, 1, 0);
    for (int seek = 0; seek < 2; seek++) {
        Mpeg2 *d = mpeg2_new();
        static Out o;
        memset(&o, 0, sizeof o);
        if (seek) { /* the same frame right after a seek, with an earlier frame decoded first */
            static Bw a, b, c;
            memset(&a, 0, sizeof a), memset(&b, 0, sizeof b), memset(&c, 0, sizeof c);
            field_stream(&a, &b, &c);
            mpeg2_decode(d, a.b, bytes(&a), 0);
            mpeg2_reset(d);
        }
        mpeg2_decode(d, w.b, bytes(&w), 5);
        mpeg2_flush(d);
        collect(d, &o);
        CHECK_INT(o.n, 1);
        CHECK_INT(o.f[0].pts, 5);
        for (int y = 0; y < 32; y += 3)
            for (int x = 0; x < 48; x += 5) CHECK_INT(luma(&o, 0, x, y), 100);
        mpeg2_free(d);
    }
}

/* After A (top field 100, bottom 50):
 *   16x8 field pair: top field upper half from A's top (100), lower half from
 *   A's bottom (50); bottom field upper half from A's bottom (50), lower half
 *   from this frame's own top field (50 there; A's top would give 100).
 *   dual prime frame: every line averages its own parity with the other (75). */
static void p16x8_and_dual_prime(Bw *a, Bw *p16, Bw *dp) {
    Bw dummy1, dummy2;
    memset(&dummy1, 0, sizeof dummy1), memset(&dummy2, 0, sizeof dummy2);
    field_stream(a, &dummy1, &dummy2);
    for (int f = 0; f < 2; f++) {
        Pic hdr = {.type = 2, .tref = 1, .f = {{1, 1}, {15, 15}}, .structure = f ? 2 : 1, .tff = true};
        picture(p16, &hdr);
        slice(p16, 0, 8);
        for (int x = 0; x < 2; x++) {
            mba(p16, 1);
            put(p16, 1, 3); /* MC, not coded */
            put(p16, 2, 2); /* field_motion_type: 16x8 */
            put(p16, f ? 1 : 0, 1);
            mv(p16, 1, 0), mv(p16, 1, 0);
            put(p16, f ? 0 : 1, 1);
            mv(p16, 1, 0), mv(p16, 1, 0);
        }
    }
    Pic hdr = {.type = 2, .tref = 1, .f = {{1, 1}, {15, 15}}, .structure = 3, .tff = true};
    picture(dp, &hdr);
    for (int row = 0; row < 2; row++) {
        slice(dp, row, 8);
        for (int x = 0; x < 2; x++) {
            mba(dp, 1);
            put(dp, 1, 3); /* MC, not coded */
            put(dp, 3, 2); /* frame_motion_type: dual prime */
            mv(dp, 1, 0);
            put(dp, 0, 1); /* dmvector x 0 */
            mv(dp, 1, 0);
            put(dp, 0, 1); /* dmvector y 0 */
        }
    }
}

TEST(mpeg2_16x8_and_dual_prime_prediction) {
    static Bw a, p16, dp;
    memset(&a, 0, sizeof a), memset(&p16, 0, sizeof p16), memset(&dp, 0, sizeof dp);
    p16x8_and_dual_prime(&a, &p16, &dp);
    for (int which = 0; which < 2; which++) {
        Mpeg2 *d = mpeg2_new();
        static Out o;
        memset(&o, 0, sizeof o);
        mpeg2_decode(d, a.b, bytes(&a), 1);
        const Bw *p = which ? &dp : &p16;
        mpeg2_decode(d, p->b, bytes(p), 2);
        mpeg2_flush(d);
        collect(d, &o);
        CHECK_INT(o.n, 2);
        CHECK_INT(o.f[1].concealed, 0);
        for (int y = 0; y < 32; y++) {
            int want = which ? 75 : y < 16 && !(y & 1) ? 100 : 50;
            CHECK_INT(luma(&o, 1, 5, y), want);
            CHECK_INT(luma(&o, 1, 30, y), want);
        }
        mpeg2_free(d);
    }
}

/* MPEG-1: no extensions, f_code and full-pel flag in the picture header, a
 * slice wrapping into the next macroblock row, DC without precision bits.
 * 16x32: I = 100 over 60; P = (0,0) from 16 pixels down (60), (0,1) from 16
 * pixels down too, i.e. past the bottom edge (repeated last line: 60). */
TEST(mpeg2_decodes_mpeg1) {
    static Bw i, p;
    memset(&i, 0, sizeof i), memset(&p, 0, sizeof p);
    start_code(&i, 0xB3);
    put(&i, 16, 12), put(&i, 32, 12), put(&i, 1, 4), put(&i, 3, 4);
    put(&i, 0x3FFFF, 18), put(&i, 1, 1), put(&i, 20, 10), put(&i, 0, 3);
    start_code(&i, 0x00);
    put(&i, 0, 10), put(&i, 1, 3), put(&i, 0xFFFF, 16), put(&i, 0, 1);
    start_code(&i, 1);
    put(&i, 8, 5), put(&i, 0, 1);
    int pred[3] = {128, 128, 128};
    for (int k = 0; k < 2; k++) {
        mba(&i, 1);
        put(&i, 1, 1);
        int lv[3] = {k ? 60 : 100, 128, 128};
        for (int b = 0; b < 6; b++) {
            int c = b < 4 ? 0 : b - 3;
            dc(&i, c, lv[c] - pred[c]);
            pred[c] = lv[c];
            put(&i, 2, 2);
        }
    }
    start_code(&p, 0x00);
    put(&p, 1, 10), put(&p, 2, 3), put(&p, 0xFFFF, 16);
    put(&p, 1, 1), put(&p, 2, 3); /* full_pel_forward_vector, forward_f_code 2 */
    put(&p, 0, 1);
    start_code(&p, 1);
    put(&p, 8, 5), put(&p, 0, 1);
    mba(&p, 1);
    put(&p, 1, 3);
    mv(&p, 2, 0), mv(&p, 2, 16);
    mba(&p, 1);
    put(&p, 1, 3);
    mv(&p, 2, 0), mv(&p, 2, 0);
    Mpeg2 *d = mpeg2_new();
    static Out o;
    memset(&o, 0, sizeof o);
    mpeg2_decode(d, i.b, bytes(&i), 1);
    mpeg2_decode(d, p.b, bytes(&p), 2);
    collect(d, &o);
    mpeg2_flush(d);
    collect(d, &o);
    CHECK_INT(o.n, 2);
    CHECK(o.f[0].mpeg1);
    CHECK_INT(o.f[0].rate_num, 25);
    CHECK_INT(o.f[0].concealed, 0);
    CHECK_INT(o.f[1].concealed, 0);
    CHECK_INT(luma(&o, 0, 8, 8), 100);
    CHECK_INT(luma(&o, 0, 8, 24), 60);
    CHECK_INT(luma(&o, 1, 8, 8), 60);
    CHECK_INT(luma(&o, 1, 8, 24), 60);
    mpeg2_free(d);
}

/* ------------------------------------------------------- damaged input */

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

TEST(mpeg2_survives_damaged_streams) {
    static Bw i, p, b;
    memset(&i, 0, sizeof i), memset(&p, 0, sizeof p), memset(&b, 0, sizeof b);
    ipb_stream(&i, &p, &b);
    const Bw *parts[3] = {&i, &p, &b};
    uint8_t buf[2048];
    for (int round = 0; round < 3000; round++) {
        Mpeg2 *d = mpeg2_new();
        for (int k = 0; k < 6; k++) {
            const Bw *src = parts[k % 3];
            size_t n = bytes(src);
            memcpy(buf, src->b, n);
            int flips = 1 + (int)(rnd() % 6);
            for (int f = 0; f < flips; f++) buf[rnd() % n] ^= (uint8_t)(1 << (rnd() % 8));
            if (rnd() % 4 == 0) n = rnd() % n; /* truncated */
            mpeg2_decode(d, buf, n, k);
            Mpeg2Frame fr;
            while (mpeg2_frame(d, &fr)) {
                CHECK(fr.width > 0 && fr.width <= 4096 && fr.height > 0 && fr.height <= 4096);
                volatile uint8_t sink = fr.y[(fr.height - 1) * fr.stride_y + fr.width - 1];
                (void)sink;
            }
        }
        mpeg2_flush(d);
        mpeg2_free(d);
    }
    /* random garbage with start codes sprinkled in */
    for (int round = 0; round < 500; round++) {
        Mpeg2 *d = mpeg2_new();
        mpeg2_decode(d, i.b, bytes(&i), 0);
        for (size_t k = 0; k < sizeof buf; k++) buf[k] = (uint8_t)rnd();
        for (int k = 0; k < 20; k++) {
            size_t at = rnd() % (sizeof buf - 4);
            buf[at] = buf[at + 1] = 0, buf[at + 2] = 1;
            static const uint8_t headers[] = {0x00, 0xB5, 0xB7, 0xB8}; /* no 0xB3: random sizes would be slow */
            buf[at + 3] = rnd() % 3 ? (uint8_t)(rnd() % 0xB0) : headers[rnd() % 4];
        }
        mpeg2_decode(d, buf, sizeof buf, 1);
        mpeg2_flush(d);
        mpeg2_free(d);
    }
    CHECK(true);
}

/* --------------------------------------------- a real file (opt-in) */

static bool read_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

TEST(mpeg2_decodes_real_mkv_when_given) {
    const char *path = plat_getenv("HEMMA_TEST_MKV");
    if (!path || !*path) return; /* opt-in: HEMMA_TEST_MKV=/path/to/dvd-rip.mkv */
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    MkvReader r = {fp, size, read_cb};
    char err[100];
    Mkv *m = mkv_open(&r, err, sizeof err);
    CHECK(m);
    const MkvTrack *v = mkv_find_track(m, MKV_VIDEO);
    if (!v || strcmp(v->codec, "V_MPEG2") != 0) { /* not MPEG-2: nothing to decode */
        mkv_close(m);
        fclose(fp);
        return;
    }
    Mpeg2 *d = mpeg2_new();
    mpeg2_decode(d, v->priv, v->priv_len, MPEG2_NOPTS);
    int frames = 0, concealed = 0;
    int64_t last = INT64_MIN;
    bool ordered = true;
    MkvFrame f;
    while (frames < 300 && mkv_next(m, &f) == 1) {
        if (f.track != v->number) continue;
        mpeg2_decode(d, f.data, f.size, f.pts);
        Mpeg2Frame fr;
        while (mpeg2_frame(d, &fr)) {
            frames++;
            concealed += fr.concealed;
            if (fr.pts != MPEG2_NOPTS) {
                ordered &= fr.pts > last;
                last = fr.pts;
            }
        }
    }
    CHECK_INT(frames, 300);
    CHECK_INT(concealed, 0);
    CHECK(ordered);
    mpeg2_free(d);
    mkv_close(m);
    fclose(fp);
}
