/* modules/av/hevc_testenc.h — writing HEVC streams for the tests: hevc_test.c (pictures that are easy to know),
 * conv_test.c (conversions, the fuzz seeds) and the raw seeds of tools/avfuzz.c. Header-only, static: each test file
 * gets its own copy. Names start hw_ so they never meet another test's writer.
 *
 * A real CABAC encoder (9.3.4.x's encoding side) over the decoder's own tables. Two kinds of stream:
 *  - exact: the IDR picture is PCM (its samples hw_pattern), every P and B picture is the reference(s) moved by
 *    whole samples (one vector set by hand, then merged everywhere): the tests know every sample;
 *  - HW_RANDOM: anything goes - intra modes and residuals, transform splits, PCM, SAO, merge and AMVP, uni- and
 *    bi-prediction, partitions - written by the rules the decoder reads by; its pictures nobody checks, but a
 *    slice the decoder can't read whole (a damaged frame) means the writer and the reader disagree.
 * Pictures come in decode order I P B P B ... (display I B P B P), each P referring to the picture two before it,
 * each B to the pictures either side; one slice a picture; CTBs 16x16, coding blocks 8 or 16. */
#ifndef HEMMA_AV_HEVC_TESTENC_H
#define HEMMA_AV_HEVC_TESTENC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hevc_int.h" /* hv_lps, hv_next_lps, hv_init, the C_* context places */

enum { HW_RANDOM = 1, HW_SAO = 2, HW_NODEBLOCK = 4, HW_TMVP = 8, HW_PCM_LF = 16, HW_QP = 32 };
/* HW_PCM_LF: PCM samples go through the loop filters too; HW_QP: cu_qp_delta on (one group a CTB), a third of the deltas
   far outside what the standard allows (a damaged stream: the decoder keeps them in range) */

typedef struct {
    int width, height;    /* multiples of 8 */
    int crop_r, crop_b;   /* conformance window (luma samples, even) */
    int depth;            /* 8, 9 or 10 */
    int chroma;           /* chroma_format_idc: 1 (others only to be refused) */
    bool field_seq;       /* VUI field_seq_flag (to be refused) */
    int rate_num, rate_den; /* VUI timing (0 = none) */
    unsigned flags;
    int frames;           /* pictures in all */
    uint32_t seed;
    int mv[2][2];         /* exact streams: P pictures move by mv[0], B pictures average mv[0] (L0) and mv[1] (L1) */
} HwStream;

/* ---- bits ---- */

typedef struct {
    uint8_t *b;
    size_t cap;
    size_t n; /* bits */
} HwBits;

static void hw_u(HwBits *w, uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--, w->n++) {
        size_t at = w->n >> 3;
        if (at >= w->cap) continue;
        if (!(w->n & 7)) w->b[at] = 0;
        if (v >> i & 1) w->b[at] |= (uint8_t)(0x80 >> (w->n & 7));
    }
}
static void hw_ue(HwBits *w, uint32_t v) {
    int k = 0;
    while ((v + 1) >> (k + 1)) k++;
    hw_u(w, 0, k);
    hw_u(w, v + 1, k + 1);
}
static void hw_se(HwBits *w, int v) { hw_ue(w, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v)); }
static void hw_align1(HwBits *w) { /* rbsp_trailing_bits / byte_alignment(): a one, then zeros */
    hw_u(w, 1, 1);
    while (w->n & 7) hw_u(w, 0, 1);
}

/* ---- NAL units: emulation prevention, a 4-byte length or an Annex B start code ---- */

static size_t hw_nal(uint8_t *out, size_t cap, bool annexb, int type, const uint8_t *rbsp, size_t n) {
    uint8_t tmp[1 << 16];
    size_t o = 0;
    tmp[o++] = (uint8_t)(type << 1), tmp[o++] = 1;
    int zeros = 0;
    for (size_t i = 0; i < n && o + 2 < sizeof tmp; i++) {
        if (zeros >= 2 && rbsp[i] <= 3) tmp[o++] = 3, zeros = 0;
        tmp[o++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    if (o + 4 > cap) return 0;
    if (annexb) out[0] = 0, out[1] = 0, out[2] = 0, out[3] = 1;
    else out[0] = (uint8_t)(o >> 24), out[1] = (uint8_t)(o >> 16), out[2] = (uint8_t)(o >> 8), out[3] = (uint8_t)o;
    memcpy(out + 4, tmp, o);
    return o + 4;
}

/* ---- parameter sets ---- */

static void hw_ptl(HwBits *w, int depth) {
    hw_u(w, depth > 8 ? 2 : 1, 8); /* space 0, tier 0, Main / Main 10 */
    hw_u(w, depth > 8 ? 0x20000000u : 0x60000000u, 32);
    hw_u(w, 0x9, 4);  /* progressive, frame only */
    hw_u(w, 0, 32), hw_u(w, 0, 12);
    hw_u(w, 93, 8); /* level 3.1 */
}

static size_t hw_vps(const HwStream *s, uint8_t *b, size_t cap) {
    HwBits w = {b, cap, 0};
    hw_u(&w, 0, 4), hw_u(&w, 3, 2), hw_u(&w, 0, 6), hw_u(&w, 0, 3), hw_u(&w, 1, 1), hw_u(&w, 0xffff, 16);
    hw_ptl(&w, s->depth);
    hw_u(&w, 1, 1), hw_ue(&w, 4), hw_ue(&w, 2), hw_ue(&w, 0);
    hw_u(&w, 0, 6), hw_ue(&w, 0), hw_u(&w, 0, 1), hw_u(&w, 0, 1);
    hw_align1(&w);
    return w.n / 8;
}

static size_t hw_sps(const HwStream *s, uint8_t *b, size_t cap) {
    HwBits w = {b, cap, 0};
    hw_u(&w, 0, 4), hw_u(&w, 0, 3), hw_u(&w, 1, 1);
    hw_ptl(&w, s->depth);
    hw_ue(&w, 0);
    hw_ue(&w, (uint32_t)(s->chroma ? s->chroma : 1));
    if (s->chroma == 3) hw_u(&w, 0, 1);
    hw_ue(&w, (uint32_t)s->width), hw_ue(&w, (uint32_t)s->height);
    hw_u(&w, s->crop_r || s->crop_b, 1);
    if (s->crop_r || s->crop_b) hw_ue(&w, 0), hw_ue(&w, (uint32_t)s->crop_r / 2), hw_ue(&w, 0), hw_ue(&w, (uint32_t)s->crop_b / 2);
    hw_ue(&w, (uint32_t)(s->depth - 8)), hw_ue(&w, (uint32_t)(s->depth - 8));
    hw_ue(&w, 4);                                /* POC lsb: 8 bits */
    hw_u(&w, 1, 1), hw_ue(&w, 4), hw_ue(&w, 2), hw_ue(&w, 0);
    hw_ue(&w, 0), hw_ue(&w, 1), hw_ue(&w, 0), hw_ue(&w, 2); /* CB 8..16, TB 4..16 */
    hw_ue(&w, 1), hw_ue(&w, 1);                  /* transform depth: inter 1, intra 1 */
    hw_u(&w, 0, 1);                              /* no scaling lists */
    hw_u(&w, 0, 1), hw_u(&w, (s->flags & HW_SAO) != 0, 1), hw_u(&w, 1, 1); /* no AMP, SAO, PCM */
    hw_u(&w, (uint32_t)s->depth - 1, 4), hw_u(&w, (uint32_t)s->depth - 1, 4), hw_ue(&w, 0), hw_ue(&w, 1), hw_u(&w, !(s->flags & HW_PCM_LF), 1);
    hw_ue(&w, 0), hw_u(&w, 0, 1);                /* reference sets in the slice headers, no long-term */
    hw_u(&w, (s->flags & HW_TMVP) != 0, 1), hw_u(&w, 1, 1);
    bool vui = s->rate_num || s->field_seq;
    hw_u(&w, vui, 1);
    if (vui) {
        hw_u(&w, 0, 5), hw_u(&w, s->field_seq, 1), hw_u(&w, 0, 2); /* (aspect, overscan, signal, chroma loc, neutral; field_seq; frame-field info, display window) */
        hw_u(&w, s->rate_num != 0, 1);
        if (s->rate_num) hw_u(&w, (uint32_t)s->rate_den, 32), hw_u(&w, (uint32_t)s->rate_num, 32), hw_u(&w, 0, 1), hw_u(&w, 0, 1);
        hw_u(&w, 0, 1);
    }
    hw_u(&w, 0, 1);
    hw_align1(&w);
    return w.n / 8;
}

static size_t hw_pps(const HwStream *s, uint8_t *b, size_t cap) {
    HwBits w = {b, cap, 0};
    hw_ue(&w, 0), hw_ue(&w, 0), hw_u(&w, 0, 1), hw_u(&w, 0, 1), hw_u(&w, 0, 3), hw_u(&w, 0, 1), hw_u(&w, 0, 1);
    hw_ue(&w, 0), hw_ue(&w, 0), hw_se(&w, 0);
    hw_u(&w, 0, 1), hw_u(&w, 0, 1), hw_u(&w, (s->flags & HW_QP) != 0, 1); /* constrained intra, transform skip, cu_qp_delta */
    if (s->flags & HW_QP) hw_ue(&w, 0);                                       /* (diff_cu_qp_delta_depth: a group = a CTB) */
    hw_se(&w, 0), hw_se(&w, 0), hw_u(&w, 0, 1), hw_u(&w, 0, 1), hw_u(&w, 0, 1), hw_u(&w, 0, 1);
    hw_u(&w, 0, 1), hw_u(&w, 0, 1), hw_u(&w, 0, 1);    /* tiles, WPP, across slices */
    hw_u(&w, 1, 1), hw_u(&w, 0, 1), hw_u(&w, (s->flags & HW_NODEBLOCK) != 0, 1);
    if (!(s->flags & HW_NODEBLOCK)) hw_se(&w, 0), hw_se(&w, 0);
    hw_u(&w, 0, 1), hw_u(&w, 0, 1), hw_ue(&w, 0), hw_u(&w, 0, 1), hw_u(&w, 0, 1);
    hw_align1(&w);
    return w.n / 8;
}

/* the HEVCDecoderConfigurationRecord (an MKV's CodecPrivate) holding the three sets */
static inline size_t hw_hvcc(const HwStream *s, uint8_t *out, size_t cap) {
    uint8_t sets[3][256];
    size_t len[3] = {hw_vps(s, sets[0], 256), hw_sps(s, sets[1], 256), hw_pps(s, sets[2], 256)};
    if (cap < 23 + 3 * 300) return 0;
    memset(out, 0, 23);
    out[0] = 1, out[1] = s->depth > 8 ? 2 : 1, out[12] = 93, out[13] = 0xf0, out[15] = 0xfc, out[16] = 0xfd;
    out[17] = (uint8_t)(0xf8 | (s->depth - 8)), out[18] = (uint8_t)(0xf8 | (s->depth - 8));
    out[21] = 0x0f; /* 4-byte lengths */
    out[22] = 3;
    size_t o = 23;
    static const int types[3] = {32, 33, 34};
    for (int k = 0; k < 3; k++) {
        uint8_t nalb[300];
        size_t n = hw_nal(nalb, sizeof nalb, false, types[k], sets[k], len[k]) - 4;
        out[o++] = (uint8_t)(0x80 | types[k]), out[o++] = 0, out[o++] = 1;
        out[o++] = (uint8_t)(n >> 8), out[o++] = (uint8_t)n;
        memcpy(out + o, nalb + 4, n);
        o += n;
    }
    return o;
}

/* ---- the picture order ---- */

typedef struct {
    int poc, type, nal; /* type: 2 I, 1 P, 0 B */
    int ref[2];         /* the POCs referred to (-1 none) */
} HwPic;

static HwPic hw_order(const HwStream *s, int k) { /* the k-th picture in decode order */
    HwPic p = {0, 2, 19, {-1, -1}};
    if (k == 0) return p;
    int m = (k + 1) / 2;
    if (k & 1) { /* P at 2m (or 2m - 1 at the very end) */
        p.poc = 2 * m < s->frames ? 2 * m : 2 * m - 1;
        p.type = 1, p.nal = 1, p.ref[0] = 2 * m - 2;
    } else {     /* B at 2m - 1 between 2m - 2 and 2m */
        p.poc = 2 * m - 1, p.type = 0, p.nal = 0, p.ref[0] = 2 * m - 2, p.ref[1] = 2 * m;
    }
    return p;
}

static int hw_count(const HwStream *s) { /* pictures in decode order (the last pair may have no B) */
    int n = 0;
    for (int k = 0; k < 2 * s->frames + 2; k++) {
        HwPic p = hw_order(s, k);
        if (p.poc < s->frames && (p.type != 0 || p.ref[1] < s->frames)) n = k + 1;
        else break;
    }
    return n;
}

/* the PCM picture's samples */
static int hw_pattern(const HwStream *s, int c, int x, int y) {
    int v = (x * 7 + y * 13 + c * 61 + ((x / 8 + y / 8) & 1) * 40) & 255;
    int up = s->depth - 8; /* (above 8 bits: the low bits vary too) */
    return v * (1 << up) + ((x + y) & ((1 << up) - 1));
}

/* ---- CABAC, encoding side ---- */

typedef struct {
    HwBits *w;
    uint32_t low, range;
    int outstanding;
    bool first;
    uint8_t ctx[HV_NCTX];
} HwCabac;

static void hw_putbit(HwCabac *e, int b) {
    if (e->first) e->first = false;
    else hw_u(e->w, (uint32_t)b, 1);
    for (; e->outstanding > 0; e->outstanding--) hw_u(e->w, (uint32_t)!b, 1);
}
static void hw_renorm(HwCabac *e) {
    while (e->range < 256) {
        if (e->low < 256) hw_putbit(e, 0);
        else if (e->low >= 512) e->low -= 512, hw_putbit(e, 1);
        else e->low -= 256, e->outstanding++;
        e->range <<= 1, e->low <<= 1;
    }
}
static void hw_start(HwCabac *e) { e->low = 0, e->range = 510, e->outstanding = 0, e->first = true; }
static void hw_bin(HwCabac *e, int ctx, int bin) {
    uint8_t *c = &e->ctx[ctx];
    int s = *c >> 1, mps = *c & 1;
    uint32_t lps = hv_lps[s][(e->range >> 6) & 3];
    e->range -= lps;
    if (bin != mps) {
        e->low += e->range, e->range = lps;
        if (!s) mps = !mps;
        s = hv_next_lps[s];
    } else {
        s = s < 62 ? s + 1 : 62;
    }
    *c = (uint8_t)(s << 1 | mps);
    hw_renorm(e);
}
static void hw_byp(HwCabac *e, int bin) {
    e->low <<= 1;
    if (bin) e->low += e->range;
    if (e->low >= 1024) hw_putbit(e, 1), e->low -= 1024;
    else if (e->low < 512) hw_putbit(e, 0);
    else e->low -= 512, e->outstanding++;
}
static void hw_bypn(HwCabac *e, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) hw_byp(e, (int)(v >> i & 1));
}
static void hw_term(HwCabac *e, int bin) {
    e->range -= 2;
    if (bin) {
        e->low += e->range, e->range = 2;
        hw_renorm(e);
        hw_putbit(e, (int)(e->low >> 9 & 1));
        hw_u(e->w, e->low >> 8 & 1, 1), hw_u(e->w, 1, 1); /* the last of these: the stop bit / an alignment one */
    } else {
        hw_renorm(e);
    }
}

/* ---- the coding tree ---- */

typedef struct {
    const HwStream *s;
    HwPic pic;
    HwCabac e;
    uint32_t rng;
    int w4, h4;
    uint8_t *coded, *intra, *pcm, *skip, *depth, *mode; /* per 4x4 */
    int qp;
    bool qp_coded; /* (HW_QP) this CTB's delta sent */
} HwCtu;

static uint32_t hw_rand(HwCtu *t) {
    t->rng = t->rng * 1103515245u + 12345u;
    return t->rng >> 8;
}
static int hw_pick(HwCtu *t, int n) { return (int)(hw_rand(t) % (uint32_t)n); }

#define HW_AT(t, x, y) ((size_t)((y) >> 2) * (size_t)(t)->w4 + (size_t)((x) >> 2))
static bool hw_avail(const HwCtu *t, int x, int y) {
    return x >= 0 && y >= 0 && x < t->s->width && y < t->s->height && t->coded[HW_AT(t, x, y)];
}

static void hw_mark(HwCtu *t, int x0, int y0, int n, int depth, bool skip, bool intra, bool pcm, int mode) {
    for (int y = y0; y < y0 + n && y < t->s->height; y += 4)
        for (int x = x0; x < x0 + n && x < t->s->width; x += 4) {
            size_t a = HW_AT(t, x, y);
            t->coded[a] = 1, t->depth[a] = (uint8_t)depth, t->skip[a] = skip, t->intra[a] = intra, t->pcm[a] = pcm;
            t->mode[a] = (uint8_t)mode;
        }
}

static const uint8_t HW_SCAN4[3][16][2] = {
    {{0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {2, 0}, {0, 3}, {1, 2}, {2, 1}, {3, 0}, {1, 3}, {2, 2}, {3, 1}, {2, 3}, {3, 2}, {3, 3}},
    {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}, {1, 1}, {2, 1}, {3, 1}, {0, 2}, {1, 2}, {2, 2}, {3, 2}, {0, 3}, {1, 3}, {2, 3}, {3, 3}},
    {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 0}, {1, 1}, {1, 2}, {1, 3}, {2, 0}, {2, 1}, {2, 2}, {2, 3}, {3, 0}, {3, 1}, {3, 2}, {3, 3}}};
static const uint8_t HW_SCAN2[3][4][2] = {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}};
static const uint8_t HW_SCAN4x4[16][2] = { /* the diagonal over 4x4 sub-blocks (a 16x16 block) */
    {0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {2, 0}, {0, 3}, {1, 2}, {2, 1}, {3, 0}, {1, 3}, {2, 2}, {3, 1}, {2, 3}, {3, 2}, {3, 3}};
static const uint8_t HW_CTX_IDX_MAP[16] = {0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8};

/* last_sig_coeff_x/y_prefix (its suffix follows both prefixes) */
static void hw_last(HwCabac *e, int base, int v, int log2, int k) {
    int off = !k ? 3 * (log2 - 2) + ((log2 - 1) >> 2) : 15, shift = !k ? (log2 + 1) >> 2 : log2 - 2, max = (log2 << 1) - 1;
    int pre = v;
    if (v >= 4) {
        int msb = 31 - __builtin_clz((unsigned)v);
        pre = 2 * msb + ((v >> (msb - 1)) & 1);
    }
    for (int i = 0; i < pre; i++) hw_bin(e, base + off + (i >> shift), 1);
    if (pre < max) hw_bin(e, base + off + (pre >> shift), 0);
}

/* a transform block's coefficients (row-major, n x n) as residual_coding() reads them; at least one non-zero */
static void hw_residual(HwCtu *t, const int16_t *coef, int log2, int k, int scan) {
    HwCabac *e = &t->e;
    int n = 1 << log2, lsb = log2 - 2, nsb = 1 << lsb;
    const uint8_t (*sbscan)[2] = lsb == 0 ? HW_SCAN2[0] : lsb == 1 ? HW_SCAN2[scan] : (const uint8_t (*)[2])HW_SCAN4x4;
    const uint8_t (*sc)[2] = HW_SCAN4[scan];
    /* the last coefficient in scan order */
    int last_sb = -1, last_pos = -1;
    for (int i = nsb * nsb - 1; i >= 0 && last_sb < 0; i--)
        for (int m = 15; m >= 0; m--) {
            int x = sbscan[i][0] * 4 + sc[m][0], y = sbscan[i][1] * 4 + sc[m][1];
            if (coef[y * n + x]) {
                last_sb = i, last_pos = m;
                break;
            }
        }
    int lx = sbscan[last_sb][0] * 4 + sc[last_pos][0], ly = sbscan[last_sb][1] * 4 + sc[last_pos][1];
    int cx = scan == 2 ? ly : lx, cy = scan == 2 ? lx : ly; /* (a vertical scan sends them swapped) */
    hw_last(e, C_LAST_X, cx, log2, k);
    hw_last(e, C_LAST_Y, cy, log2, k);
    for (int pass = 0; pass < 2; pass++) { /* the suffixes after both prefixes */
        int v = pass ? cy : cx;
        if (v >= 4) {
            int msb = 31 - __builtin_clz((unsigned)v), pre = 2 * msb + ((v >> (msb - 1)) & 1), sbits = (pre >> 1) - 1;
            hw_bypn(e, (uint32_t)(v - ((2 + (pre & 1)) << sbits)), sbits);
        }
    }
    uint8_t csbf[8][8];
    memset(csbf, 0, sizeof csbf);
    int c1 = 1;
    bool first_sb = true;
    for (int i = last_sb; i >= 0; i--) {
        int xs = sbscan[i][0], ys = sbscan[i][1];
        int vals[16];
        bool any = false;
        for (int m = 0; m < 16; m++) {
            vals[m] = coef[(ys * 4 + sc[m][1]) * n + xs * 4 + sc[m][0]];
            if (i == last_sb && m > last_pos) vals[m] = 0;
            if (vals[m]) any = true;
        }
        bool infer_dc = false;
        if (i < last_sb && i > 0) {
            int inc = 0;
            if (xs < nsb - 1) inc += csbf[xs + 1][ys];
            if (ys < nsb - 1) inc += csbf[xs][ys + 1];
            hw_bin(e, C_CSBF + (inc ? 1 : 0) + (k ? 2 : 0), any);
            csbf[xs][ys] = any;
            infer_dc = true;
        } else {
            csbf[xs][ys] = 1;
        }
        if (!csbf[xs][ys]) continue;
        int prev = 0;
        if (xs < nsb - 1) prev += csbf[xs + 1][ys];
        if (ys < nsb - 1) prev += csbf[xs][ys + 1] << 1;
        int pos[16], np = 0;
        if (i == last_sb) pos[np++] = last_pos;
        for (int m = i == last_sb ? last_pos - 1 : 15; m >= 0; m--) {
            int xc = xs * 4 + sc[m][0], yc = ys * 4 + sc[m][1];
            if (m > 0 || !infer_dc) {
                int sctx;
                if (log2 == 2) sctx = HW_CTX_IDX_MAP[(yc << 2) + xc];
                else if (xc + yc == 0) sctx = 0;
                else {
                    int xp = xc & 3, yp = yc & 3;
                    if (prev == 0) sctx = xp + yp == 0 ? 2 : xp + yp < 3 ? 1 : 0;
                    else if (prev == 1) sctx = yp == 0 ? 2 : yp == 1 ? 1 : 0;
                    else if (prev == 2) sctx = xp == 0 ? 2 : xp == 1 ? 1 : 0;
                    else sctx = 2;
                    if (!k) {
                        if (xs + ys > 0) sctx += 3;
                        sctx += log2 == 3 ? (scan == 0 ? 9 : 15) : 21;
                    } else {
                        sctx += log2 == 3 ? 9 : 12;
                    }
                }
                hw_bin(e, C_SIG + (k ? 27 : 0) + sctx, vals[m] != 0);
                if (vals[m]) pos[np++] = m, infer_dc = false;
            } else {
                pos[np++] = 0; /* (the decoder infers it: it must be non-zero here) */
            }
        }
        int ctx_set = (i == 0 || k) ? 0 : 2;
        if (!first_sb && c1 == 0) ctx_set++;
        first_sb = false;
        c1 = 1;
        int first_g1 = -1;
        for (int j = 0; j < np && j < 8; j++) {
            int a = abs(vals[pos[j]]), g1 = a > 1;
            hw_bin(e, C_G1 + (k ? 16 : 0) + ctx_set * 4 + c1, g1);
            if (g1) {
                c1 = 0;
                if (first_g1 < 0) first_g1 = j;
            } else if (c1 > 0 && c1 < 3) {
                c1++;
            }
        }
        if (first_g1 >= 0) hw_bin(e, C_G2 + (k ? 4 : 0) + ctx_set, abs(vals[pos[first_g1]]) > 2);
        for (int j = 0; j < np; j++) hw_byp(e, vals[pos[j]] < 0);
        int rice = 0;
        for (int j = 0; j < np; j++) {
            int a = abs(vals[pos[j]]);
            int base = 1 + (j < 8 ? a > 1 : 0) + (j == first_g1 ? a > 2 : 0);
            if (base != ((j < 8) ? (j == first_g1 ? 3 : 2) : 1)) continue;
            int v = a - base;
            if (v < (4 << rice)) {
                for (int q = 0; q < (v >> rice); q++) hw_byp(e, 1);
                hw_byp(e, 0);
                hw_bypn(e, (uint32_t)(v & ((1 << rice) - 1)), rice);
            } else {
                v -= 4 << rice;
                int kk = rice + 1, ones = 4;
                while (v >= (1 << kk)) v -= 1 << kk, kk++, ones++;
                for (int q = 0; q < ones; q++) hw_byp(e, 1);
                hw_byp(e, 0);
                hw_bypn(e, (uint32_t)v, kk);
            }
            if (a > 3 * (1 << rice)) rice = rice < 4 ? rice + 1 : 4;
        }
    }
}

static int hw_scan_of(bool intra, int log2, int k, int mode) {
    if (!intra || !(log2 == 2 || (log2 == 3 && k == 0))) return 0;
    return mode >= 6 && mode <= 14 ? 2 : mode >= 22 && mode <= 30 ? 1 : 0;
}

/* some coefficients (random places and sizes, now and then a big one; never none) */
static void hw_coefs(HwCtu *t, int16_t *coef, int log2) {
    int n = 1 << log2, count = 1 + hw_pick(t, 5);
    memset(coef, 0, sizeof(int16_t) * (size_t)(n * n));
    for (int i = 0; i < count; i++) {
        int x = hw_pick(t, n > 8 ? 8 : n), y = hw_pick(t, n > 8 ? 8 : n);
        int v = 1 + hw_pick(t, hw_pick(t, 8) ? 3 : 40);
        coef[y * n + x] = (int16_t)(hw_pick(t, 2) ? v : -v);
    }
}

/* cu_qp_delta_abs (a TU prefix up to 5, the first bin its own context, then EG0) and its sign */
static void hw_qp_delta(HwCtu *t) {
    HwCabac *e = &t->e;
    int v = hw_pick(t, 3) ? hw_pick(t, 9) - 4 : (hw_pick(t, 2) ? 1 : -1) * (27 + hw_pick(t, 300));
    int a = abs(v);
    hw_bin(e, C_QP, a > 0);
    for (int i = 1; i < 5 && a >= i; i++) hw_bin(e, C_QP + 1, a > i);
    if (a >= 5) {
        int r = a - 5, k = 0;
        while (r >= (1 << k)) hw_byp(e, 1), r -= 1 << k, k++;
        hw_byp(e, 0);
        hw_bypn(e, (uint32_t)r, k);
    }
    if (a) hw_byp(e, v < 0);
    t->qp_coded = true;
}

static void hw_tree(HwCtu *t, bool intra, int luma_mode, int chroma_mode, int x0, int y0, int log2, int depth, int blk,
                    bool pcb, bool pcr) {
    HwCabac *e = &t->e;
    int16_t coef[16 * 16];
    bool split = false;
    if (log2 <= 4 && log2 > 2 && depth < 1) {
        split = hw_pick(t, 3) == 0;
        hw_bin(e, C_SPLIT_TF + 5 - log2, split);
    }
    bool cb = pcb, cr = pcr;
    if (log2 > 2) {
        cb = cr = false;
        if (depth == 0 || pcb) hw_bin(e, C_CBF_CHROMA + depth, cb = hw_pick(t, 2));
        if (depth == 0 || pcr) hw_bin(e, C_CBF_CHROMA + depth, cr = hw_pick(t, 2));
    }
    if (split) {
        int h = 1 << (log2 - 1);
        for (int i = 0; i < 4; i++) hw_tree(t, intra, luma_mode, chroma_mode, x0 + (i & 1) * h, y0 + (i >> 1) * h, log2 - 1, depth + 1, i, cb, cr);
        return;
    }
    bool cbf_l = true;
    if (intra || depth != 0 || cb || cr) hw_bin(e, C_CBF_LUMA + (depth == 0 ? 1 : 0), cbf_l = hw_pick(t, 3) != 0);
    if ((t->s->flags & HW_QP) && !t->qp_coded && (cbf_l || cb || cr)) hw_qp_delta(t);
    if (cbf_l) {
        hw_coefs(t, coef, log2);
        hw_residual(t, coef, log2, 0, hw_scan_of(intra, log2, 0, luma_mode));
    }
    if (log2 > 2 || blk == 3) {
        int l2 = log2 > 2 ? log2 - 1 : 2;
        for (int k = 1; k < 3; k++)
            if (k == 1 ? cb : cr) {
                hw_coefs(t, coef, l2);
                hw_residual(t, coef, l2, k, hw_scan_of(intra, l2, k, chroma_mode));
            }
    }
}

static int hw_cand(const HwCtu *t, int yp, int xn, int yn, bool above) {
    if (!hw_avail(t, xn, yn)) return 1;
    size_t a = HW_AT(t, xn, yn);
    if (!t->intra[a] || t->pcm[a]) return 1;
    if (above && yn < ((yp >> 4) << 4)) return 1;
    return t->mode[a];
}

static void hw_mvd(HwCtu *t, int x, int y) {
    HwCabac *e = &t->e;
    int v[2] = {x, y};
    for (int i = 0; i < 2; i++) hw_bin(e, C_MVD_G0, v[i] != 0);
    for (int i = 0; i < 2; i++)
        if (v[i]) hw_bin(e, C_MVD_G1, abs(v[i]) > 1);
    for (int i = 0; i < 2; i++) {
        if (!v[i]) continue;
        int a = abs(v[i]);
        if (a > 1) { /* abs - 2 as EG1 */
            int r = a - 2, k = 1;
            while (r >= (1 << k)) hw_byp(e, 1), r -= 1 << k, k++;
            hw_byp(e, 0);
            hw_bypn(e, (uint32_t)r, k);
        }
        hw_byp(e, v[i] < 0);
    }
}

static void hw_merge_idx(HwCtu *t, int idx) {
    if (idx) {
        hw_bin(&t->e, C_MERGE_IDX, 1);
        for (int i = 1; i < 4; i++) {
            hw_byp(&t->e, i < idx);
            if (i >= idx) break;
        }
    } else {
        hw_bin(&t->e, C_MERGE_IDX, 0);
    }
}

/* one prediction unit; exact streams: the first sets the vectors, the rest merge them */
static void hw_pu(HwCtu *t, int w, int h, int depth, bool skip, bool first, bool *merged0) {
    HwCabac *e = &t->e;
    bool random = t->s->flags & HW_RANDOM;
    bool mrg = skip || (random ? hw_pick(t, 2) : !first);
    if (!skip) hw_bin(e, C_MERGE_FLAG, mrg);
    if (merged0) *merged0 = mrg;
    if (mrg) {
        hw_merge_idx(t, random ? hw_pick(t, 5) : 0);
        return;
    }
    int dir = t->pic.type == 0 ? 3 : 1;
    if (random && t->pic.type == 0) dir = 1 + hw_pick(t, 3);
    if (w + h == 12 && dir == 3) dir = 1 + hw_pick(t, 2);
    if (t->pic.type == 0) {
        if (w + h != 12) {
            hw_bin(e, C_INTER_DIR + depth, dir == 3);
            if (dir != 3) hw_bin(e, C_INTER_DIR + 4, dir == 2);
        } else {
            hw_bin(e, C_INTER_DIR + 4, dir == 2);
        }
    }
    for (int l = 0; l < 2; l++) {
        if (!(dir & (1 << l))) continue;
        if (random) hw_mvd(t, hw_pick(t, 41) - 20, hw_pick(t, 41) - 20);
        else hw_mvd(t, t->s->mv[l][0] * 4, t->s->mv[l][1] * 4);
        hw_bin(e, C_MVP, random ? hw_pick(t, 2) : 0);
    }
}

static void hw_pcm(HwCtu *t, int x0, int y0, int n) { /* (after pcm_flag's terminating bin of 1) */
    HwCabac *e = &t->e;
    while (e->w->n & 7) hw_u(e->w, 0, 1); /* pcm_alignment_zero_bit */
    const HwStream *s = t->s;
    for (int c = 0; c < 3; c++) {
        int sz = c ? n / 2 : n, xs = c ? x0 / 2 : x0, ys = c ? y0 / 2 : y0;
        for (int y = 0; y < sz; y++)
            for (int x = 0; x < sz; x++)
                hw_u(e->w, (uint32_t)((s->flags & HW_RANDOM) ? (int)(hw_rand(t) & ((1u << s->depth) - 1)) : hw_pattern(s, c, xs + x, ys + y)),
                     s->depth);
    }
    hw_start(e);
}

static void hw_cu(HwCtu *t, int x0, int y0, int log2, int depth) {
    HwCabac *e = &t->e;
    const HwStream *s = t->s;
    bool random = s->flags & HW_RANDOM;
    int n = 1 << log2;
    bool inter_pic = t->pic.type != 2, skip = false;
    if (inter_pic) {
        int inc = (hw_avail(t, x0 - 1, y0) && t->skip[HW_AT(t, x0 - 1, y0)]) + (hw_avail(t, x0, y0 - 1) && t->skip[HW_AT(t, x0, y0 - 1)]);
        skip = random ? hw_pick(t, 3) == 0 : !(x0 == 0 && y0 == 0);
        hw_bin(e, C_SKIP + inc, skip);
    }
    if (skip) {
        hw_pu(t, n, n, depth, true, false, NULL);
        hw_mark(t, x0, y0, n, depth, true, false, false, 1);
        return;
    }
    bool intra = !inter_pic || (random && hw_pick(t, 3) == 0);
    if (inter_pic) hw_bin(e, C_PRED, intra);
    int part = 0; /* 0 2Nx2N, 1 2NxN, 2 Nx2N */
    if (!intra) {
        part = random ? hw_pick(t, 3) : 0;
        hw_bin(e, C_PART, part == 0);
        if (part) hw_bin(e, C_PART + 1, part == 1);
    } else if (log2 == 3) {
        hw_bin(e, C_PART, 1); /* 2Nx2N (no NxN here) */
    }
    if (intra) {
        bool pcm = !random || hw_pick(t, 4) == 0;
        if (inter_pic && !random) pcm = false;
        hw_term(e, pcm);
        if (pcm) {
            hw_pcm(t, x0, y0, n);
            hw_mark(t, x0, y0, n, depth, false, true, true, 1);
            return;
        }
        int a = hw_cand(t, y0, x0 - 1, y0, false), b = hw_cand(t, y0, x0, y0 - 1, true), list[3];
        if (a == b) {
            if (a < 2) list[0] = 0, list[1] = 1, list[2] = 26;
            else list[0] = a, list[1] = 2 + ((a + 29) % 32), list[2] = 2 + ((a - 2 + 1) % 32);
        } else {
            list[0] = a, list[1] = b;
            list[2] = (a && b) ? 0 : (a != 1 && b != 1) ? 1 : 26;
        }
        int mode = hw_pick(t, 35), idx = -1;
        for (int i = 0; i < 3; i++)
            if (list[i] == mode) idx = i;
        hw_bin(e, C_PREV_INTRA, idx >= 0);
        if (idx >= 0) {
            hw_byp(e, idx > 0);
            if (idx > 0) hw_byp(e, idx > 1);
        } else {
            int sorted[3] = {list[0], list[1], list[2]};
            for (int i = 0; i < 3; i++)
                for (int j = i + 1; j < 3; j++)
                    if (sorted[j] < sorted[i]) { int q = sorted[i]; sorted[i] = sorted[j]; sorted[j] = q; }
            int rem = mode;
            for (int i = 2; i >= 0; i--)
                if (rem > sorted[i]) rem--;
            hw_bypn(e, (uint32_t)rem, 5);
        }
        int cm = hw_pick(t, 5);
        hw_bin(e, C_CHROMA, cm != 4);
        if (cm != 4) hw_bypn(e, (uint32_t)cm, 2);
        static const int CM[4] = {0, 26, 10, 1};
        int chroma = cm == 4 ? mode : CM[cm] == mode ? 34 : CM[cm];
        hw_mark(t, x0, y0, n, depth, false, true, false, mode);
        hw_tree(t, true, mode, chroma, x0, y0, log2, 0, 0, false, false);
        return;
    }
    bool merged0 = false;
    int h = n / 2;
    if (part == 0) hw_pu(t, n, n, depth, false, x0 == 0 && y0 == 0, &merged0);
    else if (part == 1) hw_pu(t, n, h, depth, false, false, &merged0), hw_pu(t, n, h, depth, false, false, NULL);
    else hw_pu(t, h, n, depth, false, false, &merged0), hw_pu(t, h, n, depth, false, false, NULL);
    hw_mark(t, x0, y0, n, depth, false, false, false, 1);
    bool root = true;
    if (!(part == 0 && merged0)) hw_bin(e, C_ROOT_CBF, root = random ? hw_pick(t, 2) : 0);
    if (root) hw_tree(t, false, 0, 0, x0, y0, log2, 0, 0, false, false);
}

static void hw_quadtree(HwCtu *t, int x0, int y0, int log2, int depth) {
    const HwStream *s = t->s;
    int n = 1 << log2;
    bool split = log2 > 3;
    if (x0 + n <= s->width && y0 + n <= s->height && log2 > 3) {
        int inc = (hw_avail(t, x0 - 1, y0) && t->depth[HW_AT(t, x0 - 1, y0)] > depth) +
                  (hw_avail(t, x0, y0 - 1) && t->depth[HW_AT(t, x0, y0 - 1)] > depth);
        split = (s->flags & HW_RANDOM) ? hw_pick(t, 2) : false;
        hw_bin(&t->e, C_SPLIT_CU + inc, split);
    }
    if (!split) {
        hw_cu(t, x0, y0, log2, depth);
        return;
    }
    int h = n >> 1;
    for (int i = 0; i < 4; i++) {
        int x = x0 + (i & 1) * h, y = y0 + (i >> 1) * h;
        if (x < s->width && y < s->height) hw_quadtree(t, x, y, log2 - 1, depth + 1);
    }
}

static void hw_sao(HwCtu *t, int cx, int cy) {
    HwCabac *e = &t->e;
    if (cx > 0 && hw_pick(t, 3) == 0) {
        hw_bin(e, C_SAO_MERGE, 1);
        return;
    } else if (cx > 0) {
        hw_bin(e, C_SAO_MERGE, 0);
    }
    if (cy > 0) {
        bool up = hw_pick(t, 3) == 0;
        hw_bin(e, C_SAO_MERGE, up);
        if (up) return;
    }
    int cmax = (1 << ((t->s->depth < 10 ? t->s->depth : 10) - 5)) - 1, type1 = 0;
    for (int k = 0; k < 3; k++) {
        int type = k == 2 ? type1 : hw_pick(t, 3);
        if (k < 2) {
            hw_bin(e, C_SAO_TYPE, type != 0);
            if (type) hw_byp(e, type == 2);
        }
        if (k == 1) type1 = type;
        if (!type) continue;
        int abs4[4];
        for (int i = 0; i < 4; i++) {
            abs4[i] = hw_pick(t, cmax + 1);
            for (int q = 0; q < abs4[i]; q++) hw_byp(e, 1);
            if (abs4[i] < cmax) hw_byp(e, 0);
        }
        if (type == 1) {
            for (int i = 0; i < 4; i++)
                if (abs4[i]) hw_byp(e, hw_pick(t, 2));
            hw_bypn(e, (uint32_t)hw_pick(t, 32), 5);
        } else if (k < 2) {
            hw_bypn(e, (uint32_t)hw_pick(t, 4), 2);
        }
    }
}

/* the slice header and data of picture `p` into w */
static void hw_slice(const HwStream *s, const HwPic *p, uint32_t seed, HwBits *w) {
    bool idr = p->type == 2;
    hw_u(w, 1, 1); /* first slice segment */
    if (idr) hw_u(w, 0, 1);
    hw_ue(w, 0);
    hw_ue(w, (uint32_t)p->type);
    if (!idr) {
        hw_u(w, (uint32_t)p->poc & 255, 8);
        hw_u(w, 0, 1); /* the reference set right here */
        if (p->type == 1) {
            hw_ue(w, 1), hw_ue(w, 0);
            hw_ue(w, (uint32_t)(p->poc - p->ref[0] - 1)), hw_u(w, 1, 1);
        } else {
            hw_ue(w, 1), hw_ue(w, 1);
            hw_ue(w, (uint32_t)(p->poc - p->ref[0] - 1)), hw_u(w, 1, 1);
            hw_ue(w, (uint32_t)(p->ref[1] - p->poc - 1)), hw_u(w, 1, 1);
        }
        if (s->flags & HW_TMVP) hw_u(w, 1, 1);
    }
    if (s->flags & HW_SAO) hw_u(w, 1, 1), hw_u(w, 1, 1);
    if (!idr) {
        hw_u(w, 0, 1); /* the PPS's one reference a list */
        if (p->type == 0) hw_u(w, 0, 1); /* mvd_l1_zero_flag */
        if ((s->flags & HW_TMVP) && p->type == 0) hw_u(w, 1, 1); /* collocated_from_l0 */
        hw_ue(w, 0); /* five merge candidates */
    }
    hw_se(w, 0); /* slice QP 26 */
    hw_align1(w);
    HwCtu t;
    memset(&t, 0, sizeof t);
    t.s = s, t.pic = *p, t.rng = seed * 2654435761u + (uint32_t)p->poc * 40503u + 1;
    t.w4 = s->width / 4, t.h4 = s->height / 4;
    size_t n4 = (size_t)t.w4 * t.h4;
    uint8_t *maps = calloc(6 * n4, 1);
    if (!maps) return;
    t.coded = maps, t.intra = maps + n4, t.pcm = maps + 2 * n4, t.skip = maps + 3 * n4, t.depth = maps + 4 * n4, t.mode = maps + 5 * n4;
    t.e.w = w;
    hw_start(&t.e);
    int it = p->type == 2 ? 0 : p->type == 1 ? 1 : 2; /* (cabac_init_flag is never sent) */
    for (int i = 0; i < HV_NCTX; i++) {
        int v = hv_init[it][i], m = (v >> 4) * 5 - 45, nn = ((v & 15) << 3) - 16;
        int pre = CLIP3(1, 126, ((m * 26) >> 4) + nn), mps = pre > 63;
        t.e.ctx[i] = (uint8_t)(((mps ? pre - 64 : 63 - pre) << 1) | mps);
    }
    int wctb = (s->width + 15) / 16, hctb = (s->height + 15) / 16;
    for (int cy = 0; cy < hctb; cy++)
        for (int cx = 0; cx < wctb; cx++) {
            if (s->flags & HW_SAO) hw_sao(&t, cx, cy);
            t.qp_coded = false;
            hw_quadtree(&t, cx * 16, cy * 16, 4, 0);
            hw_term(&t.e, cy == hctb - 1 && cx == wctb - 1);
        }
    while (w->n & 7) hw_u(w, 0, 1);
    free(maps);
}

/* Picture k (decode order) as an access unit: 4-byte-length NAL units (Annex B start codes with annexb, and then the
   parameter sets in front of the IDR picture). Returns its bytes (0: no such picture / no room); *poc = its place. */
static inline size_t hw_picture(const HwStream *s, int k, bool annexb, uint8_t *out, size_t cap, int *poc) {
    if (k >= hw_count(s)) return 0;
    HwPic p = hw_order(s, k);
    if (poc) *poc = p.poc;
    size_t o = 0;
    if (annexb && k == 0) {
        uint8_t set[256];
        size_t n = hw_vps(s, set, sizeof set);
        o += hw_nal(out + o, cap - o, true, 32, set, n);
        n = hw_sps(s, set, sizeof set);
        o += hw_nal(out + o, cap - o, true, 33, set, n);
        n = hw_pps(s, set, sizeof set);
        o += hw_nal(out + o, cap - o, true, 34, set, n);
    }
    uint8_t *rbsp = malloc(1 << 16);
    if (!rbsp) return 0;
    HwBits w = {rbsp, 1 << 16, 0};
    hw_slice(s, &p, s->seed, &w);
    size_t n = hw_nal(out + o, cap - o, annexb, p.nal, rbsp, w.n / 8);
    free(rbsp);
    return n ? o + n : 0;
}

/* a whole Annex B stream (the fuzzers' raw seeds) */
static inline size_t hevc_test_stream(uint8_t *out, size_t cap, const HwStream *s) {
    size_t o = 0;
    for (int k = 0; k < hw_count(s); k++) {
        size_t n = hw_picture(s, k, true, out + o, cap - o, NULL);
        if (!n) break;
        o += n;
    }
    return o;
}

#endif
