/* modules/av/mpeg4_testenc.h — writing MPEG-4 Part 2 (DivX/Xvid) streams for the tests: mpeg4_test.c (pictures
 * that are easy to know), conv_test.c (conversions, the fuzz seeds) and the raw seeds of tools/avfuzz.c. Header-only,
 * static: each test file gets its own copy. Names start m4_ so they never meet another test's writer.
 *
 * Exact pictures come from flat intra macroblocks (DC only, predicted as 7.4.3 says - M4Dc keeps what the decoder
 * will) and vectors; mpeg4_test_block() writes a whole stream for the converter and the fuzzers, using most of the
 * syntax - coefficients (as escape 3: every (last, run, level) has that form), skipped, moved, intra and four-vector
 * macroblocks, every B-VOP kind, video packets, DivX's packed blocks - whose pictures nobody checks. */
#ifndef HEMMA_AV_MPEG4_TESTENC_H
#define HEMMA_AV_MPEG4_TESTENC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t b[32768];
    int n; /* bits */
} M4w;

static void m4_put(M4w *w, uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--, w->n++) {
        size_t at = (size_t)w->n >> 3;
        if (v >> i & 1 && at < sizeof w->b) w->b[at] |= (uint8_t)(0x80 >> (w->n & 7));
    }
}
static void m4_stuffing(M4w *w) { /* next_start_code(): a 0, then 1s to the byte */
    m4_put(w, 0, 1);
    while (w->n & 7) m4_put(w, 1, 1);
}
static void m4_start(M4w *w, int code) {
    m4_put(w, 1, 24);
    m4_put(w, (uint32_t)code, 8);
}
static size_t m4_bytes(const M4w *w) { return (size_t)(w->n + 7) / 8; }

#define M4_RES 30 /* vop_time_increment_resolution: 5 bits an increment */
enum { M4_RESYNC = 1, M4_MPEGQ = 2, M4_QPEL = 4, M4_INTERLACED = 8, M4_GMC = 16 };

static void m4_vol(M4w *w, int width, int height, unsigned flags) {
    m4_start(w, 0x00); /* video_object */
    m4_start(w, 0x20);
    m4_put(w, 0, 1);                                    /* random_accessible_vol */
    m4_put(w, 17, 8);                                   /* Advanced Simple */
    m4_put(w, 1, 1), m4_put(w, 2, 4), m4_put(w, 1, 3);  /* verid 2 */
    m4_put(w, 1, 4);                                    /* square pixels */
    m4_put(w, 1, 1), m4_put(w, 1, 2), m4_put(w, 0, 1), m4_put(w, 0, 1); /* 4:2:0, B-VOPs may come, no vbv */
    m4_put(w, 0, 2);                                    /* rectangular */
    m4_put(w, 1, 1), m4_put(w, M4_RES, 16), m4_put(w, 1, 1), m4_put(w, 0, 1);
    m4_put(w, 1, 1), m4_put(w, (uint32_t)width, 13), m4_put(w, 1, 1), m4_put(w, (uint32_t)height, 13), m4_put(w, 1, 1);
    m4_put(w, flags & M4_INTERLACED ? 1 : 0, 1), m4_put(w, 1, 1); /* interlaced, obmc_disable */
    if (flags & M4_GMC) m4_put(w, 2, 2), m4_put(w, 2, 6), m4_put(w, 0, 2), m4_put(w, 0, 1); /* GMC, 2 warping points */
    else m4_put(w, 0, 2);                                 /* no sprites */
    m4_put(w, 0, 1);                                      /* 8-bit */
    m4_put(w, flags & M4_MPEGQ ? 1 : 0, 1);               /* quant_type */
    if (flags & M4_MPEGQ) m4_put(w, 0, 1), m4_put(w, 0, 1); /* the default matrices */
    m4_put(w, flags & M4_QPEL ? 1 : 0, 1);                /* quarter_sample */
    m4_put(w, 1, 1);                                      /* complexity estimation off */
    m4_put(w, flags & M4_RESYNC ? 0 : 1, 1);              /* resync_marker_disable */
    m4_put(w, 0, 1), m4_put(w, 0, 1), m4_put(w, 0, 1), m4_put(w, 0, 1); /* data partitioning, newpred, reduced, scalab. */
    m4_stuffing(w);
}

/* A VOP header: type 0 I, 1 P, 2 B; incr = modulo_time_base (whole seconds past the base it counts from), inc the
   increment (in 1/M4_RES s); q the quantiser; fcode for P and B, bcode for B. */
static void m4_vop_at(M4w *w, int type, int incr, int inc, bool coded, int rc, int q, int fcode, int bcode) {
    m4_start(w, 0xB6);
    m4_put(w, (uint32_t)type, 2);
    for (int i = 0; i < incr; i++) m4_put(w, 1, 1);
    m4_put(w, 0, 1);
    m4_put(w, 1, 1), m4_put(w, (uint32_t)inc, 5), m4_put(w, 1, 1);
    m4_put(w, coded, 1);
    if (!coded) {
        m4_stuffing(w);
        return;
    }
    if (type == 1) m4_put(w, (uint32_t)rc, 1);
    m4_put(w, 0, 3); /* intra_dc_vlc_thr: DC VLCs always */
    m4_put(w, (uint32_t)q, 5);
    if (type) m4_put(w, (uint32_t)fcode, 3);
    if (type == 2) m4_put(w, (uint32_t)bcode, 3);
}
/* ... at a time below a second, quantiser 4 (dc_scaler 8), fcode 1 */
static void m4_vop(M4w *w, int type, int time, bool coded, int rc) { m4_vop_at(w, type, 0, time, coded, rc, 4, 1, 1); }

static void m4_dc(M4w *w, bool luma, int diff) {
    static const uint8_t LUM[13][2] = {{3, 3}, {3, 2}, {2, 2}, {2, 3}, {1, 3}, {1, 4}, {1, 5},
                                       {1, 6}, {1, 7}, {1, 8}, {1, 9}, {1, 10}, {1, 11}};
    static const uint8_t CHR[13][2] = {{3, 2}, {2, 2}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6},
                                       {1, 7}, {1, 8}, {1, 9}, {1, 10}, {1, 11}, {1, 12}};
    int a = abs(diff), size = 0;
    while (a >> size) size++;
    if (size > 12) size = 12, diff = diff < 0 ? -4095 : 4095;
    m4_put(w, luma ? LUM[size][0] : CHR[size][0], luma ? LUM[size][1] : CHR[size][1]);
    if (size) m4_put(w, (uint32_t)(diff > 0 ? diff : diff + (1 << size) - 1), size);
    if (size > 8) m4_put(w, 1, 1);
}

/* DC prediction as 7.4.3.1 says, for a writer of intra macroblocks: a block outside the picture or the video packet
   gives 1024. (Kept as the decoder will: every macroblock of an I-VOP intra, at dc_scaler 8.) */
typedef struct {
    int mbw, pkt, l[16 * 16], c[2][8 * 8];
} M4Dc;

static int m4_dc_at(const M4Dc *g, bool luma, int plane, int gx, int gy) {
    int m = luma ? (gy >> 1) * g->mbw + (gx >> 1) : gy * g->mbw + gx;
    if (gx < 0 || gy < 0 || m < g->pkt) return 1024;
    return luma ? g->l[gy * 2 * g->mbw + gx] : g->c[plane][gy * g->mbw + gx];
}

static int m4_dc_pred(const M4Dc *g, bool luma, int plane, int gx, int gy) {
    int a = m4_dc_at(g, luma, plane, gx - 1, gy), b = m4_dc_at(g, luma, plane, gx - 1, gy - 1),
        c = m4_dc_at(g, luma, plane, gx, gy - 1);
    return ((abs(a - b) < abs(b - c) ? c : a) + 4) / 8;
}

/* A coefficient as escape 3 (7 + 2 + 1 + 6 + 1 + 12 + 1 bits): any (last, run, level), the same in both tables */
static void m4_coef(M4w *w, bool last, int run, int level) {
    m4_put(w, 3, 7);
    m4_put(w, 3, 2);
    m4_put(w, last, 1);
    m4_put(w, (uint32_t)run, 6);
    m4_put(w, 1, 1);
    m4_put(w, (uint32_t)level & 0xFFF, 12);
    m4_put(w, 1, 1);
}

/* A flat intra macroblock in an I-VOP (in_p: a P-VOP): Y y, Cb u, Cr v, DC only. ac: block 0 also gets an AC
   coefficient (level ac at scan position 1), which spoils its flatness. */
static void m4_intra_mb(M4w *w, M4Dc *g, int mb, int y, int u, int v, bool in_p, int ac) {
    int mbx = mb % g->mbw, mby = mb / g->mbw;
    if (in_p) m4_put(w, 0, 1), m4_put(w, 3, 5); /* coded, mcbpc: intra cbpc 00 */
    else m4_put(w, 1, 1);                         /* mcbpc: intra cbpc 00 */
    m4_put(w, 0, 1);                              /* ac_pred_flag */
    m4_put(w, ac ? 2 : 3, ac ? 5 : 4);            /* cbpy: 1000 (block 0 coded) / 0000 */
    for (int k = 0; k < 4; k++) {
        int gx = 2 * mbx + (k & 1), gy = 2 * mby + (k >> 1);
        m4_dc(w, true, y - m4_dc_pred(g, true, 0, gx, gy));
        g->l[gy * 2 * g->mbw + gx] = y * 8;
        if (!k && ac) m4_coef(w, true, 0, ac);
    }
    for (int p = 0; p < 2; p++) {
        int val = p ? v : u;
        m4_dc(w, false, val - m4_dc_pred(g, false, p, mbx, mby));
        g->c[p][mby * g->mbw + mbx] = val * 8;
    }
}

static void m4_mvcode(M4w *w, int d) { /* fcode 1: -32..31 */
    static const uint8_t MV[33][2] = {
        {1, 1},   {1, 2},   {1, 3},   {1, 4},   {3, 6},   {5, 7},   {4, 7},   {3, 7},   {11, 9},  {10, 9},  {9, 9},
        {17, 10}, {16, 10}, {15, 10}, {14, 10}, {13, 10}, {12, 10}, {11, 10}, {10, 10}, {9, 10},  {8, 10},  {7, 10},
        {6, 10},  {5, 10},  {4, 10},  {7, 11},  {6, 11},  {5, 11},  {4, 11},  {3, 11},  {2, 11},  {3, 12},  {2, 12},
    };
    int a = abs(d);
    if (a > 32) a = 32;
    m4_put(w, MV[a][0], MV[a][1]);
    if (a) m4_put(w, d < 0, 1);
}

/* A P-VOP macroblock with one vector difference and no coefficients */
static void m4_moved_mb(M4w *w, int dx, int dy) {
    m4_put(w, 0, 1); /* coded */
    m4_put(w, 1, 1); /* mcbpc: inter, cbpc 00 */
    m4_put(w, 3, 2); /* cbpy: inter blocks invert it - 1111 coded = none */
    m4_mvcode(w, dx);
    m4_mvcode(w, dy);
}

/* ------------------------------------------------------------ whole streams */

/* What a P-VOP's macroblock is in mpeg4_test_block's streams: 0 skipped, 1 moved, 2 moved with coefficients and
   a quantiser change, 3 intra, 4 four vectors, 5 skipped */
static int m4_p_kind(int mb, int p) { return (mb * 7 + p * 3) % 6; }

static void m4_inter_coefs(M4w *w, int cbp, int salt) { /* the coded blocks' coefficients */
    for (int n = 0; n < 6; n++)
        if (cbp & (32 >> n)) {
            m4_coef(w, false, (salt + n) % 5, ((salt * 3 + n) % 9) - 4 ? ((salt * 3 + n) % 9) - 4 : 3);
            m4_coef(w, true, (salt + 2 * n) % 7, -((salt + n) % 13) - 1);
        }
}

/* Block k of a stream of width x height: k 0 = VOL + I-VOP, then P, B, B, P, B, B ... in decoding order (each P
   two pictures ahead of the B-VOPs after it). *display = its place in display order. Packed (DivX): blocks 1, 4,
   7 ... carry the P- and its first B-VOP, blocks 2, 5 ... the second B-VOP, blocks 3, 6 ... a VOP not coded. */
static size_t mpeg4_test_block(uint8_t *out, size_t cap, int k, int width, int height, unsigned flags, bool packed,
                               int *display) {
    static M4w w;
    memset(&w, 0, sizeof w);
    int mbw = (width + 15) / 16, mbh = (height + 15) / 16, nmb = mbw * mbh;
    int g = (k - 1) / 3, pos = (k - 1) % 3, d = k ? (pos == 0 ? 3 * (g + 1) : 3 * g + pos) : 0;
    int bits = 1;
    while ((1 << bits) < nmb) bits++;
    if (!k) {
        m4_vol(&w, width, height, flags);
        m4_vop(&w, 0, 0, true, 0);
        M4Dc dc = {.mbw = mbw};
        for (int mb = 0; mb < nmb; mb++) {
            if ((flags & M4_RESYNC) && mb == nmb / 2 && mb) { /* a video packet */
                m4_stuffing(&w);
                m4_put(&w, 1, 17);
                m4_put(&w, (uint32_t)mb, bits), m4_put(&w, 4, 5), m4_put(&w, 0, 1);
                dc.pkt = mb;
            }
            m4_intra_mb(&w, &dc, mb, 40 + (mb * 37) % 170, 90 + mb % 7 * 9, 160 - mb % 5 * 11, false, mb % 3 ? 0 : 7 - mb % 9);
        }
        m4_stuffing(&w);
        *display = 0;
    } else if (packed && pos == 2) {
        m4_vop_at(&w, 1, 0, 0, false, 0, 4, 1, 1); /* the place-keeper */
        *display = k;
    } else {
        /* the VOPs of this block: P (part 0), first B (1), second B (2) - packed: P with the first B, then the second */
        int from = packed ? (pos ? 2 : 0) : pos, to = packed ? (pos ? 2 : 1) : pos;
        for (int part = from; part <= to; part++) {
            int dp = part == 0 ? 3 * (g + 1) : 3 * g + part, prev = 3 * g, type = part == 0 ? 1 : 2;
            /* modulo_time_base: whole seconds since the base - a P's since the reference before it, a B's
               since the reference before its own two */
            m4_vop_at(&w, type, dp / M4_RES - prev / M4_RES, dp % M4_RES, true, g & 1, 6, 1, 1);
            for (int mb = 0; mb < nmb; mb++) {
                if (type == 1 && (flags & M4_RESYNC) && mb == nmb / 2 && mb) {
                    m4_stuffing(&w);
                    m4_put(&w, 1, 15 + 1 + 1);
                    m4_put(&w, (uint32_t)mb, bits), m4_put(&w, 6, 5), m4_put(&w, 0, 1);
                }
                if (type == 1) {
                    int kind = m4_p_kind(mb, g);
                    if (kind == 0 || kind == 5) m4_put(&w, 1, 1);
                    else if (kind == 1) m4_moved_mb(&w, (mb % 9) - 4, (mb % 5) - 2);
                    else if (kind == 2) { /* inter + q, cbpc 01, cbpy (inverted) 0110 */
                        m4_put(&w, 0, 1), m4_put(&w, 7, 7), m4_put(&w, 2, 6), m4_put(&w, mb & 1 ? 3 : 0, 2);
                        m4_mvcode(&w, -(mb % 6)), m4_mvcode(&w, mb % 4);
                        m4_inter_coefs(&w, (0x9 << 2) | 1, mb + g);
                    } else if (kind == 3) { /* intra in a P-VOP, every block coded (cbpc 11, cbpy 1111), ac prediction on */
                        m4_put(&w, 0, 1), m4_put(&w, 3, 7), m4_put(&w, 1, 1), m4_put(&w, 3, 2);
                        for (int n = 0; n < 6; n++) m4_dc(&w, n < 4, (mb + n) % 5 - 2), m4_coef(&w, true, n % 3, n - 3 ? n - 3 : 5);
                    } else { /* four vectors, cbpc 00, no coefficients */
                        m4_put(&w, 0, 1), m4_put(&w, 2, 3), m4_put(&w, 3, 2);
                        for (int i = 0; i < 4; i++) m4_mvcode(&w, (mb + i) % 7 - 3), m4_mvcode(&w, (mb * i) % 5 - 2);
                    }
                } else {
                    int kind = m4_p_kind(mb, g);
                    if (kind == 0 || kind == 5) continue; /* skipped in the newer reference: no bits */
                    int bk = (mb + dp) % 5;
                    if (bk == 0) m4_put(&w, 1, 1); /* modb 1: direct, nothing more */
                    else {
                        bool cbp = bk != 2;
                        m4_put(&w, 0, 1), m4_put(&w, cbp ? 0 : 1, 1); /* modb 00 / 01 */
                        int type_b = bk == 1 ? 0 : bk - 1;               /* direct, both, backward, forward */
                        for (int i = 0; i < type_b; i++) m4_put(&w, 0, 1);
                        m4_put(&w, 1, 1);
                        if (cbp) m4_put(&w, 0x21, 6);
                        if (cbp && type_b) m4_put(&w, 1, 1), m4_put(&w, mb & 1, 1); /* dbquant +-2 */
                        if (type_b == 0) m4_mvcode(&w, mb % 3 - 1), m4_mvcode(&w, 1 - mb % 3);
                        if (type_b == 1 || type_b == 3) m4_mvcode(&w, mb % 5 - 2), m4_mvcode(&w, mb % 3 - 1);
                        if (type_b == 1 || type_b == 2) m4_mvcode(&w, 2 - mb % 5), m4_mvcode(&w, 1 - mb % 3);
                        if (cbp) m4_inter_coefs(&w, 0x21, mb + dp);
                    }
                }
            }
            m4_stuffing(&w);
        }
        *display = packed ? k : d; /* (packed: each block keeps a picture's place - an AVI's frames) */
    }
    size_t n = m4_bytes(&w);
    if (n > cap) n = cap;
    memcpy(out, w.b, n);
    return n;
}

#endif
