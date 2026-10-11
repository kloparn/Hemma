/* modules/av/h264enc.c — see h264enc.h. Clause and table numbers refer to
 * ITU-T Rec. H.264 (ISO/IEC 14496-10).
 *
 * The encoder keeps the decoder's view of every frame (the reconstruction
 * before and after deblocking), so everything a decoder computes — intra
 * prediction, sub-pel interpolation, scaling and inverse transforms, motion
 * vector prediction, CAVLC context, the deblocking filter — follows the
 * standard to the bit. Only the decisions (modes, vectors, quantisation,
 * dropping cheap coefficients) are the encoder's own. */
#include "h264enc.h"

#include <stdlib.h>
#include <string.h>

#define PAD 80        /* luma border around every frame buffer */
#define PADC 40       /* chroma border */
#define MARGIN 48     /* the motion search keeps blocks within this many pels of the picture */
#define HEDGE 76      /* half-pel planes are computed this far around the picture */
#define MAXLEVEL 2063 /* largest |level| CAVLC can carry in Baseline (level_prefix <= 15) */

enum { MB_I4, MB_I16, MB_P16, MB_P16x8, MB_P8x16, MB_SKIP };

static inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint8_t clip1(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int median3(int a, int b, int c) { return imax(imin(a, b), imin(imax(a, b), c)); }

/* ------------------------------------------------------------------ tables */

/* luma4x4BlkIdx (decoding order, 6.4.3) <-> raster position y*4+x inside the macroblock */
static const uint8_t blk2raster[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
/* frame zig-zag scan (Table 8-13): scan index -> row*4+col */
static const uint8_t zigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};

/* coeff_token (Table 9-5) for 0<=nC<2, 2<=nC<4, 4<=nC<8, 8<=nC: [TotalCoeff*4 + TrailingOnes] */
static const uint8_t tok_len[4][68] = {
    {1,  0,  0,  0,  6,  2,  0,  0,  8,  6,  3,  0,  9,  8,  7,  5,  10, 9,  8,  6,  11, 10, 9,
     7,  13, 11, 10, 8,  13, 13, 11, 9,  13, 13, 13, 10, 14, 14, 13, 11, 14, 14, 14, 13, 15, 15,
     14, 14, 15, 15, 15, 14, 16, 15, 15, 15, 16, 16, 16, 15, 16, 16, 16, 16, 16, 16, 16, 16},
    {2,  0,  0,  0,  6,  2,  0,  0,  6,  5,  3,  0,  7,  6,  6,  4,  8,  6,  6,  4,  8,  7,  7,
     5,  9,  8,  8,  6,  11, 9,  9,  6,  11, 11, 11, 7,  12, 11, 11, 9,  12, 12, 12, 11, 12, 12,
     12, 11, 13, 13, 13, 12, 13, 13, 13, 13, 13, 14, 13, 13, 14, 14, 14, 13, 14, 14, 14, 14},
    {4,  0, 0, 0, 6, 4, 0, 0, 6, 5, 4, 0, 6, 5, 5, 4, 7, 5, 5,  4,  7,  5,  5,  4,  7,  6,  6, 4,
     7,  6, 6, 4, 8, 7, 7, 5, 8, 8, 7, 6, 9, 8, 8, 7, 9, 9, 8,  8,  9,  9,  9,  8,  10, 9,  9, 9,
     10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10},
    {6, 0, 0, 0, 6, 6, 0, 0, 6, 6, 6, 0, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
     6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6},
};
static const uint8_t tok_bits[4][68] = {
    {1, 0, 0, 0, 5, 1,  0,  0,  7, 4,  1, 0, 7,  6,  5,  3,  7,  6,  5,  3,  7,  6, 5,
     4, 15, 6, 5, 4, 11, 14, 5, 4, 8, 10, 13, 4, 15, 14, 9, 4, 11, 10, 13, 12, 15, 14,
     9, 12, 11, 10, 13, 8, 15, 1, 9, 12, 11, 14, 13, 8, 7, 10, 9, 12, 4, 6, 5, 8},
    {3, 0, 0, 0, 11, 2, 0, 0, 7, 7, 3, 0, 7, 10, 9, 5, 7, 6, 5, 4, 4, 6, 5, 6, 7, 6, 5, 8, 15, 6, 5, 4, 11, 14,
     13, 4, 15, 10, 9, 4, 11, 14, 13, 12, 8, 10, 9, 8, 15, 14, 13, 12, 11, 10, 9, 12, 7, 11, 6, 8, 9, 8, 10, 1,
     7, 6, 5, 4},
    {15, 0,  0,  0,  15, 14, 0,  0,  11, 15, 13, 0,  8,  12, 14, 12, 15, 10, 11, 11, 11, 8,  9,
     10, 9,  14, 13, 9,  8,  10, 9,  8,  15, 14, 13, 13, 11, 14, 10, 12, 15, 10, 13, 12, 11, 14,
     9,  12, 8,  10, 13, 8,  13, 7,  9,  12, 9,  12, 11, 10, 5,  8,  7,  6,  1,  4,  3,  2},
    {3,  0,  0,  0,  0,  1,  0,  0,  4,  5,  6,  0,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18,
     19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
     42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63},
};
/* coeff_token for chroma DC 4:2:0 (nC = -1) */
static const uint8_t cdc_tok_len[20] = {2, 0, 0, 0, 6, 1, 0, 0, 6, 6, 3, 0, 6, 7, 7, 6, 6, 8, 8, 7};
static const uint8_t cdc_tok_bits[20] = {1, 0, 0, 0, 7, 1, 0, 0, 4, 6, 1, 0, 3, 3, 2, 5, 2, 3, 2, 0};

/* total_zeros for 4x4 blocks (Tables 9-7, 9-8): [TotalCoeff-1][total_zeros] */
static const uint8_t tz_len[15][16] = {
    {1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9}, {3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6},
    {4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6},       {5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5},
    {4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5},             {6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6},
    {6, 5, 3, 3, 3, 2, 3, 4, 3, 6},                   {6, 4, 5, 3, 2, 2, 3, 3, 6},
    {6, 6, 4, 2, 2, 3, 2, 5},                         {5, 5, 3, 2, 2, 2, 4},
    {4, 4, 3, 3, 1, 3},                               {4, 4, 2, 1, 3},
    {3, 3, 1, 2},                                     {2, 2, 1},
    {1, 1},
};
static const uint8_t tz_bits[15][16] = {
    {1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1}, {7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0},
    {5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0},       {3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0},
    {5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0},             {1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0},
    {1, 1, 5, 4, 3, 3, 2, 1, 1, 0},                   {1, 1, 1, 3, 3, 2, 2, 1, 0},
    {1, 0, 1, 3, 2, 1, 1, 1},                         {1, 0, 1, 3, 2, 1, 1},
    {0, 1, 1, 2, 1, 3},                               {0, 1, 1, 1, 1},
    {0, 1, 1, 1},                                     {0, 1, 1},
    {0, 1},
};
/* total_zeros for chroma DC 4:2:0 (Table 9-9a) */
static const uint8_t ctz_len[3][4] = {{1, 2, 3, 3}, {1, 2, 2, 0}, {1, 1, 0, 0}};
static const uint8_t ctz_bits[3][4] = {{1, 1, 1, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}};
/* run_before (Table 9-10): [min(zerosLeft, 7) - 1][run_before] */
static const uint8_t run_len[7][15] = {
    {1, 1}, {1, 2, 2}, {2, 2, 2, 2}, {2, 2, 2, 3, 3}, {2, 2, 3, 3, 3, 3}, {2, 3, 3, 3, 3, 3, 3},
    {3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11},
};
static const uint8_t run_bits[7][15] = {
    {1, 0}, {1, 1, 0}, {3, 2, 1, 0}, {3, 2, 1, 1, 0}, {3, 2, 3, 2, 1, 0}, {3, 0, 1, 3, 2, 5, 4},
    {7, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1},
};

/* coded_block_pattern codeNum -> value (Table 9-4, ChromaArrayType 1) */
static const uint8_t cbp_intra_of[48] = {47, 31, 15, 0,  23, 27, 29, 30, 7,  11, 13, 14, 39, 43, 45, 46,
                                         16, 3,  5,  10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1,  2,  4,
                                         8,  17, 18, 20, 24, 6,  9,  22, 25, 32, 33, 34, 36, 40, 38, 41};
static const uint8_t cbp_inter_of[48] = {0,  16, 1,  2,  4,  8,  32, 3,  5,  10, 12, 15, 47, 7,  11, 13,
                                         14, 6,  9,  31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
                                         17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41};

/* QPc from qPI (Table 8-15) */
static const uint8_t chroma_qp[52] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
                                      18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33,
                                      34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};
/* deblocking (Tables 8-16, 8-17) */
static const uint8_t alpha_tab[52] = {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   0,   0,   0,   0,   0,   0,   4,   4,
                                      5,  6,  7,  8,  9,  10, 12, 13, 15, 17,  20,  22,  25,  28,  32,  36,  40,  45,
                                      50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255};
static const uint8_t beta_tab[52] = {0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  2,
                                     2, 3, 3, 3, 3, 4, 4,  4,  6,  6,  7,  7,  8,  8,  9,  9,  10, 10,
                                     11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18};
static const uint8_t tc0_tab[52][3] = {
    {0, 0, 0},   {0, 0, 0},   {0, 0, 0},   {0, 0, 0},   {0, 0, 0},    {0, 0, 0},    {0, 0, 0},
    {0, 0, 0},   {0, 0, 0},   {0, 0, 0},   {0, 0, 0},   {0, 0, 0},    {0, 0, 0},    {0, 0, 0},
    {0, 0, 0},   {0, 0, 0},   {0, 0, 0},   {0, 0, 1},   {0, 0, 1},    {0, 0, 1},    {0, 0, 1},
    {0, 1, 1},   {0, 1, 1},   {1, 1, 1},   {1, 1, 1},   {1, 1, 1},    {1, 1, 1},    {1, 1, 2},
    {1, 1, 2},   {1, 1, 2},   {1, 1, 2},   {1, 2, 3},   {1, 2, 3},    {2, 2, 3},    {2, 2, 4},
    {2, 3, 4},   {2, 3, 4},   {3, 3, 5},   {3, 4, 6},   {3, 4, 6},    {4, 5, 7},    {4, 5, 8},
    {4, 6, 9},   {5, 7, 10},  {6, 8, 11},  {6, 8, 13},  {7, 10, 14},  {8, 11, 16},  {9, 12, 18},
    {10, 13, 20}, {11, 15, 23}, {13, 17, 25},
};

/* forward quantisation multipliers and the normAdjust4x4 values of 8.5.9 */
static const int quant_mf[6][3] = {{13107, 5243, 8066}, {11916, 4660, 7490}, {10082, 4194, 6554},
                                   {9362, 3647, 5825},  {8192, 3355, 5243},  {7282, 2893, 4559}};
static const int norm_v[6][3] = {{10, 16, 13}, {11, 18, 14}, {13, 20, 16}, {14, 23, 18}, {16, 25, 20}, {18, 29, 23}};

/* lambda for SAD/SATD costs: 2^((qp-12)/6), at least 1 */
static const uint8_t lambda_tab[52] = {1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,
                                       2,  2,  3,  3,  3,  4,  4,  4,  5,  6,  6,  7,  8,  9,  10, 11, 13, 14,
                                       16, 18, 20, 23, 25, 29, 32, 36, 40, 45, 51, 57, 64, 72, 81, 91};

static inline int pos_class(int pos) { /* 8.5.9: 0 = (even,even), 1 = (odd,odd), 2 = the rest */
    int r = pos >> 2, c = pos & 3;
    return !(r & 1) && !(c & 1) ? 0 : (r & 1) && (c & 1) ? 1 : 2;
}

/* ------------------------------------------------------------- bit writer */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    uint64_t acc; /* bits not yet in buf (low nacc bits) */
    int nacc;
    bool oom;
} Bw;

static void bw_reserve(Bw *b, size_t n) {
    if (b->len + n <= b->cap || b->oom) return;
    size_t cap = b->cap ? b->cap * 2 : 1 << 16;
    while (cap < b->len + n) cap *= 2;
    uint8_t *p = realloc(b->buf, cap);
    if (!p) {
        b->oom = true;
        return;
    }
    b->buf = p;
    b->cap = cap;
}

static void bw_reset(Bw *b) {
    b->len = 0;
    b->acc = 0;
    b->nacc = 0;
}

static inline void bw_put(Bw *b, uint32_t v, int n) { /* 1 <= n <= 32 */
    b->acc = n == 32 ? (b->acc << 32) | v : (b->acc << n) | (v & ((1u << n) - 1));
    b->nacc += n;
    if (b->nacc >= 32) {
        b->nacc -= 32;
        uint32_t w = (uint32_t)(b->acc >> b->nacc);
        bw_reserve(b, 4);
        if (b->oom) return;
        b->buf[b->len] = (uint8_t)(w >> 24);
        b->buf[b->len + 1] = (uint8_t)(w >> 16);
        b->buf[b->len + 2] = (uint8_t)(w >> 8);
        b->buf[b->len + 3] = (uint8_t)w;
        b->len += 4;
    }
}

static inline int ue_bits(uint32_t v) { return 2 * (31 - __builtin_clz(v + 1)) + 1; }
static inline int se_bits(int v) { return ue_bits(v > 0 ? 2u * (uint32_t)v - 1 : 2u * (uint32_t)-v); }

static void bw_ue(Bw *b, uint32_t v) {
    uint32_t x = v + 1;
    int len = 32 - __builtin_clz(x);
    if (len <= 16) bw_put(b, x, 2 * len - 1);
    else {
        bw_put(b, 0, len - 1);
        bw_put(b, x, len);
    }
}

static void bw_se(Bw *b, int v) { bw_ue(b, v > 0 ? 2u * (uint32_t)v - 1 : 2u * (uint32_t)-v); }

static void bw_flush(Bw *b) { /* zero bits up to a byte boundary, then everything into buf */
    if (b->nacc & 7) bw_put(b, 0, 8 - (b->nacc & 7));
    bw_reserve(b, 8);
    while (!b->oom && b->nacc >= 8) {
        b->nacc -= 8;
        b->buf[b->len++] = (uint8_t)(b->acc >> b->nacc);
    }
}

static void bw_trailing(Bw *b) { /* rbsp_trailing_bits */
    bw_put(b, 1, 1);
    bw_flush(b);
}

static void bytes_put(Bw *b, const void *p, size_t n) {
    bw_reserve(b, n);
    if (b->oom) return;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

/* NAL unit: header byte + RBSP with emulation prevention (7.4.1), behind a
   start code or a 4-byte big-endian length. */
static void nal_put(Bw *out, bool annexb, int ref_idc, int type, const uint8_t *rbsp, size_t n) {
    bw_reserve(out, n + n / 2 + 8);
    if (out->oom) return;
    size_t at = out->len;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    bytes_put(out, sc, 4);
    out->buf[out->len++] = (uint8_t)(ref_idc << 5 | type);
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && rbsp[i] <= 3) {
            out->buf[out->len++] = 3;
            zeros = 0;
        }
        out->buf[out->len++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    if (!annexb) {
        size_t len = out->len - at - 4;
        out->buf[at] = (uint8_t)(len >> 24);
        out->buf[at + 1] = (uint8_t)(len >> 16);
        out->buf[at + 2] = (uint8_t)(len >> 8);
        out->buf[at + 3] = (uint8_t)len;
    }
}

/* ------------------------------------------------------------ the encoder */

typedef struct {
    uint8_t *mem;
    uint8_t *y, *u, *v;    /* picture origin inside the padded planes */
    uint8_t *hh, *hv, *hc; /* half-pel planes: b (horizontal), h (vertical), j (centre) of 8.4.2.2.1 */
} Frame;

typedef struct {
    uint8_t type, cbp, qp;
    int8_t ref;        /* 0 = predicted from the previous frame, -1 = intra */
    uint8_t nz[16];    /* TotalCoeff of each luma 4x4 block (raster order inside the MB) */
    uint8_t nzc[2][4]; /* TotalCoeff of each chroma AC block */
    int8_t i4[16];     /* intra 4x4 prediction modes (raster); 2 (DC) unless the MB is intra 4x4 */
    int16_t mv[16][2]; /* per 4x4 block (raster), quarter pels */
} Mb;

typedef struct {
    int16_t luma[16][16];  /* levels per 4x4 block (raster), scan order; intra 16x16: [1..15] = AC */
    int16_t dc[16];        /* intra 16x16 DC levels, scan order */
    int16_t cdc[2][4];     /* chroma DC levels */
    int16_t cac[2][4][16]; /* chroma AC levels, scan order [1..15] */
    uint8_t mode16, cmode;
    uint8_t i4flag[16], i4rem[16]; /* prev_intra4x4_pred_mode_flag / rem_intra4x4_pred_mode (raster) */
    int16_t mvd[2][2];             /* per partition */
} Code;

typedef struct {
    int x, y;
} Mv;

struct H264Enc {
    H264Params p;
    int w, h, w16, h16, mbw, mbh;
    int sy, sc;   /* strides of the padded planes */
    int level, keyint, log2fn, vrange;
    int frame, gop, idr_id, frame_num;
    int qp_add;          /* rate control: QP raise while over the bitrate cap */
    double vbv, vbv_size, frame_bits;
    Frame f[2];
    int cur; /* f[cur] is being coded, f[cur ^ 1] is the reference */
    uint8_t *src_mem, *srcy, *srcu, *srcv;
    int ssy, ssc;
    Mb *mbs;
    Code code;
    Bw slice, out;
    uint8_t sps[64], pps[16];
    size_t sps_len, pps_len;
    int mf[52][16], dq[52][16]; /* per QP, raster position */
    /* per frame */
    int qp, qpc, lambda;
    bool islice;
};

/* ------------------------------------------------------------ parameters */

static void write_sps(H264Enc *e) {
    Bw b = {0};
    bw_put(&b, 66, 8);   /* profile_idc: Baseline */
    bw_put(&b, 0xC0, 8); /* constraint_set0 + constraint_set1 = Constrained Baseline */
    bw_put(&b, (uint32_t)e->level, 8);
    bw_ue(&b, 0); /* seq_parameter_set_id */
    bw_ue(&b, (uint32_t)(e->log2fn - 4));
    bw_ue(&b, 2); /* pic_order_cnt_type: output order = decoding order */
    bw_ue(&b, 1); /* max_num_ref_frames */
    bw_put(&b, 0, 1);
    bw_ue(&b, (uint32_t)(e->mbw - 1));
    bw_ue(&b, (uint32_t)(e->mbh - 1));
    bw_put(&b, 1, 1); /* frame_mbs_only_flag */
    bw_put(&b, 1, 1); /* direct_8x8_inference_flag */
    bool crop = e->w != e->w16 || e->h != e->h16;
    bw_put(&b, crop, 1);
    if (crop) {
        bw_ue(&b, 0);
        bw_ue(&b, (uint32_t)(e->w16 - e->w) / 2);
        bw_ue(&b, 0);
        bw_ue(&b, (uint32_t)(e->h16 - e->h) / 2);
    }
    bw_put(&b, 1, 1); /* vui_parameters_present_flag */
    static const int16_t sars[17][2] = {{0, 0},   {1, 1},   {12, 11}, {10, 11}, {16, 11}, {40, 33},
                                        {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11}, {15, 11},
                                        {64, 33}, {160, 99}, {4, 3},  {3, 2},   {2, 1}};
    int idc = 255;
    for (int i = 1; i < 17; i++)
        if (sars[i][0] * e->p.sar_h == sars[i][1] * e->p.sar_w) idc = i;
    bw_put(&b, 1, 1); /* aspect_ratio_info_present_flag */
    bw_put(&b, (uint32_t)idc, 8);
    if (idc == 255) {
        bw_put(&b, (uint32_t)e->p.sar_w, 16);
        bw_put(&b, (uint32_t)e->p.sar_h, 16);
    }
    bw_put(&b, 0, 1); /* overscan_info_present_flag */
    bw_put(&b, 1, 1); /* video_signal_type_present_flag */
    bw_put(&b, 5, 3); /* video_format: unspecified */
    bw_put(&b, 0, 1); /* video_full_range_flag: limited (16..235) */
    bw_put(&b, 1, 1); /* colour_description_present_flag */
    int cd = e->p.colour == 709 ? 1 : 6; /* BT.709, or SMPTE 170M (BT.601 525) */
    bw_put(&b, (uint32_t)cd, 8);
    bw_put(&b, (uint32_t)cd, 8);
    bw_put(&b, (uint32_t)cd, 8);
    bw_put(&b, 0, 1); /* chroma_loc_info_present_flag */
    bw_put(&b, 1, 1); /* timing_info_present_flag */
    bw_put(&b, (uint32_t)e->p.fps_den, 32);
    bw_put(&b, (uint32_t)e->p.fps_num * 2, 32);
    bw_put(&b, 1, 1); /* fixed_frame_rate_flag */
    bw_put(&b, 0, 1); /* nal_hrd_parameters_present_flag */
    bw_put(&b, 0, 1); /* vcl_hrd_parameters_present_flag */
    bw_put(&b, 0, 1); /* pic_struct_present_flag */
    bw_put(&b, 1, 1); /* bitstream_restriction_flag */
    bw_put(&b, 1, 1); /* motion_vectors_over_pic_boundaries_flag */
    bw_ue(&b, 0);     /* max_bytes_per_pic_denom */
    bw_ue(&b, 0);     /* max_bits_per_mb_denom */
    bw_ue(&b, 15);    /* log2_max_mv_length_horizontal */
    bw_ue(&b, 15);    /* log2_max_mv_length_vertical */
    bw_ue(&b, 0);     /* max_num_reorder_frames: no B frames, nothing to wait for */
    bw_ue(&b, 1);     /* max_dec_frame_buffering */
    bw_trailing(&b);
    Bw n = {0};
    nal_put(&n, true, 3, 7, b.buf, b.len);
    e->sps_len = n.len - 4 <= sizeof e->sps ? n.len - 4 : 0;
    if (n.buf && e->sps_len) memcpy(e->sps, n.buf + 4, e->sps_len);
    free(b.buf);
    free(n.buf);
}

static void write_pps(H264Enc *e) {
    Bw b = {0};
    bw_ue(&b, 0);     /* pic_parameter_set_id */
    bw_ue(&b, 0);     /* seq_parameter_set_id */
    bw_put(&b, 0, 1); /* entropy_coding_mode_flag: CAVLC */
    bw_put(&b, 0, 1); /* bottom_field_pic_order_in_frame_present_flag */
    bw_ue(&b, 0);     /* num_slice_groups_minus1 */
    bw_ue(&b, 0);     /* num_ref_idx_l0_default_active_minus1 */
    bw_ue(&b, 0);     /* num_ref_idx_l1_default_active_minus1 */
    bw_put(&b, 0, 1); /* weighted_pred_flag */
    bw_put(&b, 0, 2); /* weighted_bipred_idc */
    bw_se(&b, 0);     /* pic_init_qp_minus26 */
    bw_se(&b, 0);     /* pic_init_qs_minus26 */
    bw_se(&b, 0);     /* chroma_qp_index_offset */
    bw_put(&b, 0, 1); /* deblocking_filter_control_present_flag: always on, default strength */
    bw_put(&b, 0, 1); /* constrained_intra_pred_flag */
    bw_put(&b, 0, 1); /* redundant_pic_cnt_present_flag */
    bw_trailing(&b);
    Bw n = {0};
    nal_put(&n, true, 3, 8, b.buf, b.len);
    e->pps_len = n.len - 4 <= sizeof e->pps ? n.len - 4 : 0;
    if (n.buf && e->pps_len) memcpy(e->pps, n.buf + 4, e->pps_len);
    free(b.buf);
    free(n.buf);
}

/* Annex A: the lowest level that fits the frame size, the macroblock rate and the bitrate. */
static int pick_level(int mbs, double mbps, int kbps) {
    static const struct {
        int idc, maxfs, maxmbps, maxbr;
    } L[] = {{10, 99, 1485, 64},          {11, 396, 3000, 192},       {12, 396, 6000, 384},
             {13, 396, 11880, 768},       {20, 396, 11880, 2000},     {21, 792, 19800, 4000},
             {22, 1620, 20250, 4000},     {30, 1620, 40500, 10000},   {31, 3600, 108000, 14000},
             {32, 5120, 216000, 20000},   {40, 8192, 245760, 20000},  {41, 8192, 245760, 50000},
             {42, 8704, 522240, 50000},   {50, 22080, 589824, 135000}, {51, 36864, 983040, 240000},
             {52, 36864, 2073600, 240000}};
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++)
        if (mbs <= L[i].maxfs && mbps <= L[i].maxmbps && kbps <= L[i].maxbr) return L[i].idc;
    return 52;
}

/* ----------------------------------------------------------- frame buffers */

/* Picture planes for each frame; the half-pel planes are shared: only the
   reference needs them, and they are rebuilt once its successor is coded. */
static bool frame_alloc(H264Enc *e, Frame *f, const Frame *hpel_from) {
    size_t ly = (size_t)e->sy * (size_t)(e->h16 + 2 * PAD), lc = (size_t)e->sc * (size_t)(e->h16 / 2 + 2 * PADC);
    f->mem = calloc(ly * (hpel_from ? 1 : 4) + lc * 2, 1);
    if (!f->mem) return false;
    uint8_t *m = f->mem;
    size_t oy = (size_t)PAD * (size_t)e->sy + PAD, oc = (size_t)PADC * (size_t)e->sc + PADC;
    f->y = m + oy;
    size_t chroma = hpel_from ? ly : 4 * ly; /* the chroma planes follow the luma ones */
    if (hpel_from) {
        f->hh = hpel_from->hh;
        f->hv = hpel_from->hv;
        f->hc = hpel_from->hc;
    } else {
        f->hh = m + ly + oy;
        f->hv = m + 2 * ly + oy;
        f->hc = m + 3 * ly + oy;
    }
    f->u = m + chroma + oc;
    f->v = m + chroma + lc + oc;
    return true;
}

static void extend_plane(uint8_t *p, int stride, int w, int h, int pad) {
    for (int y = 0; y < h; y++) {
        uint8_t *row = p + (ptrdiff_t)y * stride;
        memset(row - pad, row[0], (size_t)pad);
        memset(row + w, row[w - 1], (size_t)pad);
    }
    for (int y = 1; y <= pad; y++) {
        memcpy(p - (ptrdiff_t)y * stride - pad, p - pad, (size_t)(w + 2 * pad));
        memcpy(p + (ptrdiff_t)(h - 1 + y) * stride - pad, p + (ptrdiff_t)(h - 1) * stride - pad, (size_t)(w + 2 * pad));
    }
}

/* The half-pel planes of 8.4.2.2.1 over the picture and HEDGE pels around it. */
static void half_pels(H264Enc *e, Frame *f) {
    int s = e->sy, x0 = -HEDGE, x1 = e->w16 + HEDGE, w = x1 - x0;
    int16_t *b1 = malloc(sizeof(int16_t) * (size_t)w * 6); /* rolling rows of unrounded horizontal sums */
    if (!b1) return;
    for (int y = -HEDGE - 2; y < e->h16 + HEDGE + 3; y++) {
        const uint8_t *r = f->y + (ptrdiff_t)y * s;
        int16_t *t = b1 + (size_t)((y + HEDGE + 2) % 6) * (size_t)w;
        for (int x = 0; x < w; x++) {
            const uint8_t *q = r + x0 + x;
            t[x] = (int16_t)(q[-2] - 5 * q[-1] + 20 * q[0] + 20 * q[1] - 5 * q[2] + q[3]);
        }
        if (y >= -HEDGE && y < e->h16 + HEDGE) {
            uint8_t *hh = f->hh + (ptrdiff_t)y * s + x0;
            for (int x = 0; x < w; x++) hh[x] = clip1((t[x] + 16) >> 5);
            uint8_t *hv = f->hv + (ptrdiff_t)y * s + x0;
            const uint8_t *c = f->y + (ptrdiff_t)y * s + x0;
            for (int x = 0; x < w; x++) {
                int v = c[x - 2 * s] - 5 * c[x - s] + 20 * c[x] + 20 * c[x + s] - 5 * c[x + 2 * s] + c[x + 3 * s];
                hv[x] = clip1((v + 16) >> 5);
            }
        }
        int yc = y - 3; /* the centre plane needs b1 rows yc-2 .. yc+3 */
        if (yc >= -HEDGE && yc < e->h16 + HEDGE) {
            const int16_t *r0 = b1 + (size_t)((yc - 2 + HEDGE + 2) % 6) * (size_t)w;
            const int16_t *r1 = b1 + (size_t)((yc - 1 + HEDGE + 2) % 6) * (size_t)w;
            const int16_t *r2 = b1 + (size_t)((yc + HEDGE + 2) % 6) * (size_t)w;
            const int16_t *r3 = b1 + (size_t)((yc + 1 + HEDGE + 2) % 6) * (size_t)w;
            const int16_t *r4 = b1 + (size_t)((yc + 2 + HEDGE + 2) % 6) * (size_t)w;
            const int16_t *r5 = b1 + (size_t)((yc + 3 + HEDGE + 2) % 6) * (size_t)w;
            uint8_t *hc = f->hc + (ptrdiff_t)yc * s + x0;
            for (int x = 0; x < w; x++) {
                int j1 = r0[x] - 5 * r1[x] + 20 * r2[x] + 20 * r3[x] - 5 * r4[x] + r5[x];
                hc[x] = clip1((j1 + 512) >> 10);
            }
        }
    }
    free(b1);
}

/* -------------------------------------------------------- transform & quant */

static void fdct4(int32_t d[16], const int16_t r[16]) {
    int32_t t[16];
    for (int i = 0; i < 4; i++) {
        const int16_t *x = r + i * 4;
        int s0 = x[0] + x[3], s3 = x[0] - x[3], s1 = x[1] + x[2], s2 = x[1] - x[2];
        t[i * 4 + 0] = s0 + s1;
        t[i * 4 + 2] = s0 - s1;
        t[i * 4 + 1] = 2 * s3 + s2;
        t[i * 4 + 3] = s3 - 2 * s2;
    }
    for (int j = 0; j < 4; j++) {
        int s0 = t[j] + t[12 + j], s3 = t[j] - t[12 + j], s1 = t[4 + j] + t[8 + j], s2 = t[4 + j] - t[8 + j];
        d[j] = s0 + s1;
        d[8 + j] = s0 - s1;
        d[4 + j] = 2 * s3 + s2;
        d[12 + j] = s3 - 2 * s2;
    }
}

/* 8.5.12.2: rows, then columns, then (x + 32) >> 6 onto the prediction. */
static void idct4_add(uint8_t *dst, int ds, const uint8_t *pred, int ps, const int32_t d[16]) {
    int32_t f[16];
    for (int i = 0; i < 4; i++) {
        const int32_t *x = d + i * 4;
        int e0 = x[0] + x[2], e1 = x[0] - x[2], e2 = (x[1] >> 1) - x[3], e3 = x[1] + (x[3] >> 1);
        f[i * 4 + 0] = e0 + e3;
        f[i * 4 + 1] = e1 + e2;
        f[i * 4 + 2] = e1 - e2;
        f[i * 4 + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        int g0 = f[j] + f[8 + j], g1 = f[j] - f[8 + j], g2 = (f[4 + j] >> 1) - f[12 + j], g3 = f[4 + j] + (f[12 + j] >> 1);
        int h[4] = {g0 + g3, g1 + g2, g1 - g2, g0 - g3};
        for (int i = 0; i < 4; i++) dst[i * ds + j] = clip1(pred[i * ps + j] + ((h[i] + 32) >> 6));
    }
}

static void copy_block(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h) {
    for (int y = 0; y < h; y++) memcpy(dst + (ptrdiff_t)y * ds, src + (ptrdiff_t)y * ss, (size_t)w);
}

/* Quantise coef (raster) into lev (scan order) from scan index `start`; returns the non-zero count. */
static int quant4(int16_t lev[16], const int32_t coef[16], const int *mf, int qbits, int f, int start) {
    int nz = 0;
    for (int i = start; i < 16; i++) {
        int pos = zigzag[i], c = coef[pos], a = c < 0 ? -c : c;
        int l = (int)(((uint32_t)a * (uint32_t)mf[pos] + (uint32_t)f) >> qbits); /* < 2^32: |coef| <= 9180 */
        if (l > MAXLEVEL) l = MAXLEVEL;
        lev[i] = (int16_t)(c < 0 ? -l : l);
        nz += l != 0;
    }
    return nz;
}

/* Scaled coefficients (raster) from levels (scan order), 8.5.12.1 with flat weights. */
static void dequant4(int32_t d[16], const int16_t lev[16], const int *dq, int start) {
    for (int i = start; i < 16; i++) d[zigzag[i]] = lev[i] * dq[zigzag[i]];
}

/* x264-style "is it worth coding": a few lone +-1 levels cost more bits than they're worth. */
static int decimate_score(const int16_t *lev, int n) {
    static const uint8_t ds_table[16] = {3, 2, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int i = n - 1, score = 0;
    while (i >= 0 && !lev[i]) i--;
    while (i >= 0) {
        if (lev[i] > 1 || lev[i] < -1) return 9;
        int run = 0;
        i--;
        while (i >= 0 && !lev[i]) {
            i--;
            run++;
        }
        score += ds_table[run];
    }
    return score;
}

/* -------------------------------------------------------- intra prediction */

/* 8.3.1.2: t[0] = p[-1,-1], t[1 + x] = p[x,-1] (x = 0..7), l[y] = p[-1,y]. */
static void pred4x4(uint8_t *d, int mode, const uint8_t *t, const uint8_t *l, bool has_t, bool has_l) {
#define P(x, y) ((y) < 0 ? t[(x) + 1] : l[(y)])
    switch (mode) {
    case 0:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) d[y * 4 + x] = t[1 + x];
        break;
    case 1:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) d[y * 4 + x] = l[y];
        break;
    case 2: {
        int s;
        if (has_t && has_l) s = (t[1] + t[2] + t[3] + t[4] + l[0] + l[1] + l[2] + l[3] + 4) >> 3;
        else if (has_l) s = (l[0] + l[1] + l[2] + l[3] + 2) >> 2;
        else if (has_t) s = (t[1] + t[2] + t[3] + t[4] + 2) >> 2;
        else s = 128;
        memset(d, s, 16);
        break;
    }
    case 3: /* diagonal down left */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                d[y * 4 + x] = (x == 3 && y == 3) ? (uint8_t)((t[7] + 3 * t[8] + 2) >> 2)
                                                  : (uint8_t)((t[x + y + 1] + 2 * t[x + y + 2] + t[x + y + 3] + 2) >> 2);
        break;
    case 4: /* diagonal down right */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int v;
                if (x > y) v = (P(x - y - 2, -1) + 2 * P(x - y - 1, -1) + P(x - y, -1) + 2) >> 2;
                else if (x < y) v = (P(-1, y - x - 2) + 2 * P(-1, y - x - 1) + P(-1, y - x) + 2) >> 2;
                else v = (P(0, -1) + 2 * t[0] + P(-1, 0) + 2) >> 2;
                d[y * 4 + x] = (uint8_t)v;
            }
        break;
    case 5: /* vertical right */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int z = 2 * x - y, v;
                if (z >= 0 && !(z & 1)) v = (P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 1) >> 1;
                else if (z > 0) v = (P(x - (y >> 1) - 2, -1) + 2 * P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 2) >> 2;
                else if (z == -1) v = (P(-1, 0) + 2 * t[0] + P(0, -1) + 2) >> 2;
                else v = (P(-1, y - 1) + 2 * P(-1, y - 2) + P(-1, y - 3) + 2) >> 2;
                d[y * 4 + x] = (uint8_t)v;
            }
        break;
    case 6: /* horizontal down */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int z = 2 * y - x, v;
                if (z >= 0 && !(z & 1)) v = (P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 1) >> 1;
                else if (z > 0) v = (P(-1, y - (x >> 1) - 2) + 2 * P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 2) >> 2;
                else if (z == -1) v = (P(-1, 0) + 2 * t[0] + P(0, -1) + 2) >> 2;
                else v = (P(x - 1, -1) + 2 * P(x - 2, -1) + P(x - 3, -1) + 2) >> 2;
                d[y * 4 + x] = (uint8_t)v;
            }
        break;
    case 7: /* vertical left */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int i = x + (y >> 1);
                d[y * 4 + x] = !(y & 1) ? (uint8_t)((t[i + 1] + t[i + 2] + 1) >> 1)
                                        : (uint8_t)((t[i + 1] + 2 * t[i + 2] + t[i + 3] + 2) >> 2);
            }
        break;
    case 8: /* horizontal up */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int z = x + 2 * y, v;
                if (z > 5) v = l[3];
                else if (z == 5) v = (l[2] + 3 * l[3] + 2) >> 2;
                else if (!(z & 1)) v = (l[y + (x >> 1)] + l[y + (x >> 1) + 1] + 1) >> 1;
                else v = (l[y + (x >> 1)] + 2 * l[y + (x >> 1) + 1] + l[y + (x >> 1) + 2] + 2) >> 2;
                d[y * 4 + x] = (uint8_t)v;
            }
        break;
    }
#undef P
}

/* 8.3.3 (n = 16, luma) and 8.3.4 (n = 8, 4:2:0 chroma). Modes: luma V H DC Plane; chroma DC H V Plane. */
static void pred16(uint8_t *d, int mode, const uint8_t *t, const uint8_t *l, int tl, bool has_t, bool has_l) {
    switch (mode) {
    case 0:
        for (int y = 0; y < 16; y++) memcpy(d + y * 16, t, 16);
        break;
    case 1:
        for (int y = 0; y < 16; y++) memset(d + y * 16, l[y], 16);
        break;
    case 2: {
        int s = 0;
        for (int i = 0; i < 16; i++) s += (has_t ? t[i] : 0) + (has_l ? l[i] : 0);
        s = has_t && has_l ? (s + 16) >> 5 : has_t || has_l ? (s + 8) >> 4 : 128;
        memset(d, s, 256);
        break;
    }
    case 3: {
        int hh = 0, vv = 0;
        for (int i = 0; i < 8; i++) {
            hh += (i + 1) * (t[8 + i] - (i == 7 ? tl : t[6 - i]));
            vv += (i + 1) * (l[8 + i] - (i == 7 ? tl : l[6 - i]));
        }
        int a = 16 * (l[15] + t[15]), b = (5 * hh + 32) >> 6, c = (5 * vv + 32) >> 6;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) d[y * 16 + x] = clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5);
        break;
    }
    }
}

static void pred_chroma(uint8_t *d, int mode, const uint8_t *t, const uint8_t *l, int tl, bool has_t, bool has_l) {
    switch (mode) {
    case 0: /* DC, per 4x4 block (8.3.4.1-3) */
        for (int by = 0; by < 2; by++)
            for (int bx = 0; bx < 2; bx++) {
                int st = 0, sl = 0, s;
                for (int i = 0; i < 4; i++) {
                    st += t[bx * 4 + i];
                    sl += l[by * 4 + i];
                }
                bool corner = bx == by; /* (0,0) and (4,4) use both edges */
                if (corner && has_t && has_l) s = (st + sl + 4) >> 3;
                else if (corner) s = has_l ? (sl + 2) >> 2 : has_t ? (st + 2) >> 2 : 128;
                else if (bx == 1) s = has_t ? (st + 2) >> 2 : has_l ? (sl + 2) >> 2 : 128;
                else s = has_l ? (sl + 2) >> 2 : has_t ? (st + 2) >> 2 : 128;
                for (int y = 0; y < 4; y++) memset(d + (by * 4 + y) * 8 + bx * 4, s, 4);
            }
        break;
    case 1:
        for (int y = 0; y < 8; y++) memset(d + y * 8, l[y], 8);
        break;
    case 2:
        for (int y = 0; y < 8; y++) memcpy(d + y * 8, t, 8);
        break;
    case 3: {
        int hh = 0, vv = 0;
        for (int i = 0; i < 4; i++) {
            hh += (i + 1) * (t[4 + i] - (i == 3 ? tl : t[2 - i]));
            vv += (i + 1) * (l[4 + i] - (i == 3 ? tl : l[2 - i]));
        }
        int a = 16 * (l[7] + t[7]), b = (34 * hh + 32) >> 6, c = (34 * vv + 32) >> 6;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) d[y * 8 + x] = clip1((a + b * (x - 3) + c * (y - 3) + 16) >> 5);
        break;
    }
    }
}

/* ------------------------------------------------------ motion compensation */

/* Luma prediction for a w x h block at picture position (x, y) displaced by mv
   (quarter pels), from the reference's full- and half-pel planes (8.4.2.2.1). */
static void mc_luma_slow(const H264Enc *e, const Frame *r, int x, int y, Mv mv, int w, int h, uint8_t *d, int ds);

/* The plane (a) or two planes (a, b: the prediction is (a + b + 1) >> 1)
   holding the samples at mv; false when the block reaches beyond the
   half-pel planes (then mc_luma_slow computes it). */
static bool mc_planes(const H264Enc *e, const Frame *r, int x, int y, Mv mv, int w, int h, const uint8_t **pa,
                      const uint8_t **pb) {
    int xi = x + (mv.x >> 2), yi = y + (mv.y >> 2), fx = mv.x & 3, fy = mv.y & 3, s = e->sy;
    if (xi < -HEDGE + 3 || yi < -HEDGE + 3 || xi + w + 1 > e->w16 + HEDGE - 3 || yi + h + 1 > e->h16 + HEDGE - 3)
        return false;
    ptrdiff_t o = (ptrdiff_t)yi * s + xi;
    const uint8_t *a = NULL, *b = NULL;
    switch (fy * 4 + fx) {
    case 0: a = r->y + o; break;
    case 2: a = r->hh + o; break;
    case 8: a = r->hv + o; break;
    case 10: a = r->hc + o; break;
    case 1: a = r->y + o, b = r->hh + o; break;           /* a = (G + b + 1) >> 1 */
    case 3: a = r->y + o + 1, b = r->hh + o; break;       /* c = (H + b + 1) >> 1 */
    case 4: a = r->y + o, b = r->hv + o; break;           /* d = (G + h + 1) >> 1 */
    case 12: a = r->y + o + s, b = r->hv + o; break;      /* n = (M + h + 1) >> 1 */
    case 6: a = r->hh + o, b = r->hc + o; break;          /* f = (b + j + 1) >> 1 */
    case 14: a = r->hh + o + s, b = r->hc + o; break;     /* q = (j + s + 1) >> 1 */
    case 9: a = r->hv + o, b = r->hc + o; break;          /* i = (h + j + 1) >> 1 */
    case 11: a = r->hv + o + 1, b = r->hc + o; break;     /* k = (j + m + 1) >> 1 */
    case 5: a = r->hh + o, b = r->hv + o; break;          /* e = (b + h + 1) >> 1 */
    case 7: a = r->hh + o, b = r->hv + o + 1; break;      /* g = (b + m + 1) >> 1 */
    case 13: a = r->hv + o, b = r->hh + o + s; break;     /* p = (h + s + 1) >> 1 */
    case 15: a = r->hv + o + 1, b = r->hh + o + s; break; /* r = (m + s + 1) >> 1 */
    }
    *pa = a;
    *pb = b;
    return true;
}

static void mc_luma(const H264Enc *e, const Frame *r, int x, int y, Mv mv, int w, int h, uint8_t *d, int ds) {
    const uint8_t *a, *b;
    int s = e->sy;
    if (!mc_planes(e, r, x, y, mv, w, h, &a, &b)) mc_luma_slow(e, r, x, y, mv, w, h, d, ds);
    else if (!b) copy_block(d, ds, a, s, w, h);
    else
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) d[j * ds + i] = (uint8_t)((a[j * s + i] + b[j * s + i] + 1) >> 1);
}

/* The same samples straight from the formulas, with coordinates clamped into
   the picture: for vectors pointing beyond the half-pel planes. */
static int ref_px(const H264Enc *e, const Frame *r, int x, int y) {
    return r->y[(ptrdiff_t)clip3(0, e->h16 - 1, y) * e->sy + clip3(0, e->w16 - 1, x)];
}
static int tap6(int a, int b, int c, int d, int e, int f) { return a - 5 * b + 20 * c + 20 * d - 5 * e + f; }
static int hpel_b1(const H264Enc *e, const Frame *r, int x, int y) {
    return tap6(ref_px(e, r, x - 2, y), ref_px(e, r, x - 1, y), ref_px(e, r, x, y), ref_px(e, r, x + 1, y),
                ref_px(e, r, x + 2, y), ref_px(e, r, x + 3, y));
}
static int hpel_h1(const H264Enc *e, const Frame *r, int x, int y) {
    return tap6(ref_px(e, r, x, y - 2), ref_px(e, r, x, y - 1), ref_px(e, r, x, y), ref_px(e, r, x, y + 1),
                ref_px(e, r, x, y + 2), ref_px(e, r, x, y + 3));
}
static int sample_at(const H264Enc *e, const Frame *r, int xi, int yi, int fx, int fy) {
    int G = ref_px(e, r, xi, yi);
    if (!fx && !fy) return G;
    int b = clip1((hpel_b1(e, r, xi, yi) + 16) >> 5), h = clip1((hpel_h1(e, r, xi, yi) + 16) >> 5);
    int s = clip1((hpel_b1(e, r, xi, yi + 1) + 16) >> 5), m = clip1((hpel_h1(e, r, xi + 1, yi) + 16) >> 5);
    int j1 = tap6(hpel_b1(e, r, xi, yi - 2), hpel_b1(e, r, xi, yi - 1), hpel_b1(e, r, xi, yi), hpel_b1(e, r, xi, yi + 1),
                  hpel_b1(e, r, xi, yi + 2), hpel_b1(e, r, xi, yi + 3));
    int j = clip1((j1 + 512) >> 10);
    switch (fy * 4 + fx) {
    case 2: return b;
    case 8: return h;
    case 10: return j;
    case 1: return (G + b + 1) >> 1;
    case 3: return (ref_px(e, r, xi + 1, yi) + b + 1) >> 1;
    case 4: return (G + h + 1) >> 1;
    case 12: return (ref_px(e, r, xi, yi + 1) + h + 1) >> 1;
    case 6: return (b + j + 1) >> 1;
    case 14: return (j + s + 1) >> 1;
    case 9: return (h + j + 1) >> 1;
    case 11: return (j + m + 1) >> 1;
    case 5: return (b + h + 1) >> 1;
    case 7: return (b + m + 1) >> 1;
    case 13: return (h + s + 1) >> 1;
    default: return (m + s + 1) >> 1;
    }
}

static void mc_luma_slow(const H264Enc *e, const Frame *r, int x, int y, Mv mv, int w, int h, uint8_t *d, int ds) {
    int xi = x + (mv.x >> 2), yi = y + (mv.y >> 2);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) d[j * ds + i] = (uint8_t)sample_at(e, r, xi + i, yi + j, mv.x & 3, mv.y & 3);
}

/* 8.4.2.2.2: chroma at eighth-pel precision, bilinear; (x, y) in chroma samples. */
static void mc_chroma(const H264Enc *e, const uint8_t *plane, int x, int y, Mv mv, int w, int h, uint8_t *d, int ds) {
    int xi = x + (mv.x >> 3), yi = y + (mv.y >> 3), fx = mv.x & 7, fy = mv.y & 7, s = e->sc;
    int cw = e->w16 / 2, ch = e->h16 / 2;
    int wa = (8 - fx) * (8 - fy), wb = fx * (8 - fy), wc = (8 - fx) * fy, wd = fx * fy;
    if (xi < -PADC + 1 || yi < -PADC + 1 || xi + w + 1 > cw + PADC - 1 || yi + h + 1 > ch + PADC - 1) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                int x0 = clip3(0, cw - 1, xi + i), x1 = clip3(0, cw - 1, xi + i + 1);
                int y0 = clip3(0, ch - 1, yi + j), y1 = clip3(0, ch - 1, yi + j + 1);
                d[j * ds + i] = (uint8_t)((wa * plane[y0 * s + x0] + wb * plane[y0 * s + x1] + wc * plane[y1 * s + x0] +
                                           wd * plane[y1 * s + x1] + 32) >> 6);
            }
        return;
    }
    const uint8_t *p = plane + (ptrdiff_t)yi * s + xi;
    for (int j = 0; j < h; j++, p += s)
        for (int i = 0; i < w; i++)
            d[j * ds + i] = (uint8_t)((wa * p[i] + wb * p[i + 1] + wc * p[i + s] + wd * p[i + s + 1] + 32) >> 6);
}

/* -------------------------------------------------------------- costs */

/* Fixed widths so the compiler turns the rows into vector code. */
static int sad16(const uint8_t *a, int sa, const uint8_t *b, int sb, int h) {
    int s = 0;
    for (int y = 0; y < h; y++, a += sa, b += sb)
        for (int x = 0; x < 16; x++) s += iabs(a[x] - b[x]);
    return s;
}
static int sad8(const uint8_t *a, int sa, const uint8_t *b, int sb, int h) {
    int s = 0;
    for (int y = 0; y < h; y++, a += sa, b += sb)
        for (int x = 0; x < 8; x++) s += iabs(a[x] - b[x]);
    return s;
}
static int sad(const uint8_t *a, int sa, const uint8_t *b, int sb, int w, int h) {
    return w == 16 ? sad16(a, sa, b, sb, h) : sad8(a, sa, b, sb, h);
}

/* SATD (sum of absolute 4x4 Hadamard coefficients, halved) of a strip of four
   rows, w = 4, 8 or 16 wide. The vertical transform runs across the whole row
   (vectorises); the horizontal one uses |a+b| + |a-b| = 2 max(|a|, |b|), which
   also makes the halving exact. */
static int satd_strip(const uint8_t *a, int sa, const uint8_t *b, int sb, int w) {
    int16_t v[4][16];
    for (int x = 0; x < w; x++) {
        int d0 = a[x] - b[x], d1 = a[sa + x] - b[sb + x], d2 = a[2 * sa + x] - b[2 * sb + x],
            d3 = a[3 * sa + x] - b[3 * sb + x];
        int s01 = d0 + d1, d01 = d0 - d1, s23 = d2 + d3, d23 = d2 - d3;
        v[0][x] = (int16_t)(s01 + s23);
        v[1][x] = (int16_t)(s01 - s23);
        v[2][x] = (int16_t)(d01 - d23);
        v[3][x] = (int16_t)(d01 + d23);
    }
    int sum = 0;
    for (int r = 0; r < 4; r++)
        for (int x = 0; x < w; x += 4) {
            const int16_t *c = v[r] + x;
            int s01 = c[0] + c[1], d01 = c[0] - c[1], s23 = c[2] + c[3], d23 = c[2] - c[3];
            sum += imax(iabs(s01), iabs(s23)) + imax(iabs(d01), iabs(d23));
        }
    return sum;
}

/* The same against the average of two prediction planes, (pa + pb + 1) >> 1:
   quarter-pel candidates scored straight from the half-pel planes. */
static int satd_strip_avg(const uint8_t *a, int sa, const uint8_t *pa, const uint8_t *pb, int sp, int w) {
    int16_t v[4][16];
    for (int x = 0; x < w; x++) {
        int d0 = a[x] - ((pa[x] + pb[x] + 1) >> 1), d1 = a[sa + x] - ((pa[sp + x] + pb[sp + x] + 1) >> 1),
            d2 = a[2 * sa + x] - ((pa[2 * sp + x] + pb[2 * sp + x] + 1) >> 1),
            d3 = a[3 * sa + x] - ((pa[3 * sp + x] + pb[3 * sp + x] + 1) >> 1);
        int s01 = d0 + d1, d01 = d0 - d1, s23 = d2 + d3, d23 = d2 - d3;
        v[0][x] = (int16_t)(s01 + s23);
        v[1][x] = (int16_t)(s01 - s23);
        v[2][x] = (int16_t)(d01 - d23);
        v[3][x] = (int16_t)(d01 + d23);
    }
    int sum = 0;
    for (int r = 0; r < 4; r++)
        for (int x = 0; x < w; x += 4) {
            const int16_t *c = v[r] + x;
            int s01 = c[0] + c[1], d01 = c[0] - c[1], s23 = c[2] + c[3], d23 = c[2] - c[3];
            sum += imax(iabs(s01), iabs(s23)) + imax(iabs(d01), iabs(d23));
        }
    return sum;
}

static int satd4(const uint8_t *a, int sa, const uint8_t *b, int sb) { return satd_strip(a, sa, b, sb, 4); }

static int satd(const uint8_t *a, int sa, const uint8_t *b, int sb, int w, int h) {
    int s = 0;
    for (int y = 0; y < h; y += 4) s += satd_strip(a + y * sa, sa, b + y * sb, sb, w);
    return s;
}

/* SATD of a source block against the prediction at mv, read straight from the
   reference planes (no prediction block is built). */
static int satd_mv(const H264Enc *e, const Frame *r, const uint8_t *src, int ss, int x, int y, Mv mv, int w, int h) {
    const uint8_t *a, *b;
    if (!mc_planes(e, r, x, y, mv, w, h, &a, &b)) {
        uint8_t pred[256];
        mc_luma(e, r, x, y, mv, w, h, pred, 16);
        return satd(src, ss, pred, 16, w, h);
    }
    int s = e->sy, sum = 0;
    for (int j = 0; j < h; j += 4)
        sum += b ? satd_strip_avg(src + j * ss, ss, a + j * s, b + j * s, s, w) : satd_strip(src + j * ss, ss, a + j * s, s, w);
    return sum;
}

static inline int mv_cost(int lambda, Mv mv, Mv pred) { return lambda * (se_bits(mv.x - pred.x) + se_bits(mv.y - pred.y)); }

/* --------------------------------------------------------- neighbours */

static inline Mb *mb_at(const H264Enc *e, int mbx, int mby) { return &e->mbs[mby * e->mbw + mbx]; }

/* Motion data of the 4x4 block at (x4, y4) relative to the current macroblock
   (8.4.1.3.2). Unavailable: false, ref -1, mv 0. Intra: true, ref -1, mv 0. */
static bool nb_motion(const H264Enc *e, int mbx, int mby, const Mb *cur, int x4, int y4, int *ref, Mv *mv) {
    *ref = -1;
    mv->x = mv->y = 0;
    const Mb *m;
    if (x4 >= 0 && x4 < 4 && y4 >= 0 && y4 < 4) m = cur;
    else if (y4 >= 0 && x4 >= 4) return false; /* right of us: not decoded yet */
    else {
        int nx = mbx + (x4 < 0 ? -1 : x4 >= 4 ? 1 : 0), ny = mby + (y4 < 0 ? -1 : 0);
        if (nx < 0 || nx >= e->mbw || ny < 0) return false;
        m = mb_at(e, nx, ny);
        x4 &= 3;
        y4 &= 3;
    }
    if (m->ref < 0) return true;
    *ref = 0;
    mv->x = m->mv[y4 * 4 + x4][0];
    mv->y = m->mv[y4 * 4 + x4][1];
    return true;
}

/* 8.4.1.3: motion vector predictor of the partition at (x4, y4), w4 blocks wide.
   shape: 0 = 16x16 (median), 1/2 = 16x8 top/bottom, 3/4 = 8x16 left/right. */
static Mv mv_pred(const H264Enc *e, int mbx, int mby, const Mb *cur, int x4, int y4, int w4, int shape) {
    int ra, rb, rc;
    Mv a, b, c;
    bool ha = nb_motion(e, mbx, mby, cur, x4 - 1, y4, &ra, &a);
    bool hb = nb_motion(e, mbx, mby, cur, x4, y4 - 1, &rb, &b);
    bool hc = nb_motion(e, mbx, mby, cur, x4 + w4, y4 - 1, &rc, &c);
    if (!hc) hc = nb_motion(e, mbx, mby, cur, x4 - 1, y4 - 1, &rc, &c); /* D stands in for C */
    if (shape == 1 && rb == 0) return b;
    if (shape == 2 && ra == 0) return a;
    if (shape == 3 && ra == 0) return a;
    if (shape == 4 && rc == 0) return c;
    if (!hb && !hc && ha) {
        b = c = a;
        rb = rc = ra;
    }
    int n = (ra == 0) + (rb == 0) + (rc == 0);
    if (n == 1) return ra == 0 ? a : rb == 0 ? b : c;
    return (Mv){median3(a.x, b.x, c.x), median3(a.y, b.y, c.y)};
}

/* 8.4.1.1: the vector a skipped macroblock moves with. */
static Mv mv_skip(const H264Enc *e, int mbx, int mby, const Mb *cur) {
    if (mbx == 0 || mby == 0) return (Mv){0, 0};
    int ra, rb;
    Mv a, b;
    nb_motion(e, mbx, mby, cur, -1, 0, &ra, &a);
    nb_motion(e, mbx, mby, cur, 0, -1, &rb, &b);
    if ((ra == 0 && !a.x && !a.y) || (rb == 0 && !b.x && !b.y)) return (Mv){0, 0};
    return mv_pred(e, mbx, mby, cur, 0, 0, 4, 0);
}

/* 9.2.1: nC from the blocks left of and above (bx, by). */
static int nc_luma(const H264Enc *e, int mbx, int mby, const Mb *cur, int bx, int by) {
    int na = -1, nb = -1;
    if (bx > 0) na = cur->nz[by * 4 + bx - 1];
    else if (mbx > 0) na = mb_at(e, mbx - 1, mby)->nz[by * 4 + 3];
    if (by > 0) nb = cur->nz[(by - 1) * 4 + bx];
    else if (mby > 0) nb = mb_at(e, mbx, mby - 1)->nz[12 + bx];
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    return na >= 0 ? na : nb >= 0 ? nb : 0;
}

static int nc_chroma(const H264Enc *e, int mbx, int mby, const Mb *cur, int c, int bx, int by) {
    int na = -1, nb = -1;
    if (bx > 0) na = cur->nzc[c][by * 2];
    else if (mbx > 0) na = mb_at(e, mbx - 1, mby)->nzc[c][by * 2 + 1];
    if (by > 0) nb = cur->nzc[c][bx];
    else if (mby > 0) nb = mb_at(e, mbx, mby - 1)->nzc[c][2 + bx];
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    return na >= 0 ? na : nb >= 0 ? nb : 0;
}

/* ------------------------------------------------------------------ CAVLC */

static void put_level(Bw *b, int code, int sl) {
    if (sl == 0) {
        if (code < 14) bw_put(b, 1, code + 1);
        else if (code < 30) {
            bw_put(b, 1, 15);
            bw_put(b, (uint32_t)(code - 14), 4);
        } else {
            bw_put(b, 1, 16);
            bw_put(b, (uint32_t)(code - 30), 12);
        }
    } else if (code < (15 << sl)) {
        bw_put(b, 1, (code >> sl) + 1);
        bw_put(b, (uint32_t)(code & ((1 << sl) - 1)), sl);
    } else {
        bw_put(b, 1, 16);
        bw_put(b, (uint32_t)(code - (15 << sl)), 12);
    }
}

/* 9.2: one residual block, coefficients in scan order (n = 16, 15 or 4 for
   chroma DC with nc = -1). Returns TotalCoeff. */
static int cavlc_block(Bw *b, const int16_t *c, int n, int nc) {
    int idx[16], total = 0;
    for (int i = n - 1; i >= 0; i--)
        if (c[i]) idx[total++] = i; /* highest frequency first */
    int t1 = 0;
    while (t1 < total && t1 < 3 && (c[idx[t1]] == 1 || c[idx[t1]] == -1)) t1++;
    if (nc == -1) bw_put(b, cdc_tok_bits[total * 4 + t1], cdc_tok_len[total * 4 + t1]);
    else {
        int tab = nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3;
        bw_put(b, tok_bits[tab][total * 4 + t1], tok_len[tab][total * 4 + t1]);
    }
    if (!total) return 0;
    for (int k = 0; k < t1; k++) bw_put(b, c[idx[k]] < 0, 1);
    int sl = total > 10 && t1 < 3 ? 1 : 0;
    for (int k = t1; k < total; k++) {
        int lv = c[idx[k]], code = lv > 0 ? 2 * lv - 2 : -2 * lv - 1;
        if (k == t1 && t1 < 3) code -= 2;
        put_level(b, code, sl);
        if (!sl) sl = 1;
        if (iabs(lv) > (3 << (sl - 1)) && sl < 6) sl++;
    }
    int tz = idx[0] + 1 - total;
    if (total < n) {
        if (n == 4) bw_put(b, ctz_bits[total - 1][tz], ctz_len[total - 1][tz]);
        else bw_put(b, tz_bits[total - 1][tz], tz_len[total - 1][tz]);
    }
    for (int k = 0, left = tz; k < total - 1 && left > 0; k++) {
        int run = idx[k] - idx[k + 1] - 1, t = imin(left, 7) - 1;
        bw_put(b, run_bits[t][run], run_len[t][run]);
        left -= run;
    }
    return total;
}

/* ------------------------------------------------------- macroblock coding */

typedef struct {
    uint8_t t[17]; /* luma: [0] = p[-1,-1], [1..16] = row above; chroma uses [1..8] */
    uint8_t l[16];
    bool has_t, has_l;
} Edge;

static void edges16(const H264Enc *e, const uint8_t *pl, int stride, int x, int y, int n, Edge *g) {
    g->has_t = y > 0;
    g->has_l = x > 0;
    memset(g->t, 0, sizeof g->t);
    memset(g->l, 0, sizeof g->l);
    if (g->has_t) memcpy(g->t + 1, pl + (ptrdiff_t)(y - 1) * stride + x, (size_t)n);
    if (g->has_l)
        for (int i = 0; i < n; i++) g->l[i] = pl[(ptrdiff_t)(y + i) * stride + x - 1];
    if (g->has_t && g->has_l) g->t[0] = pl[(ptrdiff_t)(y - 1) * stride + x - 1];
    (void)e;
}

static inline int qbits_of(int qp) { return 15 + qp / 6; }

/* Chroma of one macroblock: residual of both planes against pred, DC
   Hadamard, quantisation, reconstruction. Returns the chroma CBP (0, 1, 2). */
static int code_chroma(H264Enc *e, Mb *mb, int mbx, int mby, const uint8_t pred[2][64], bool intra) {
    Code *k = &e->code;
    int qpc = e->qpc, qb = qbits_of(qpc), f = (1 << qb) / (intra ? 3 : 6);
    const int *mf = e->mf[qpc], *dq = e->dq[qpc];
    int32_t coef[2][4][16];
    int cbp = 0, acscore = 0;
    for (int c = 0; c < 2; c++) {
        const uint8_t *src = (c ? e->srcv : e->srcu) + (ptrdiff_t)(mby * 8) * e->ssc + mbx * 8;
        int dc[4];
        for (int bl = 0; bl < 4; bl++) {
            int bx = bl & 1, by = bl >> 1;
            int16_t r[16];
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    r[y * 4 + x] = (int16_t)(src[(by * 4 + y) * e->ssc + bx * 4 + x] - pred[c][(by * 4 + y) * 8 + bx * 4 + x]);
            fdct4(coef[c][bl], r);
            dc[bl] = coef[c][bl][0];
            mb->nzc[c][bl] = (uint8_t)quant4(k->cac[c][bl], coef[c][bl], mf, qb, f, 1);
            acscore += mb->nzc[c][bl] ? decimate_score(k->cac[c][bl] + 1, 15) : 0;
        }
        int t[4] = {dc[0] + dc[1] + dc[2] + dc[3], dc[0] - dc[1] + dc[2] - dc[3], dc[0] + dc[1] - dc[2] - dc[3],
                    dc[0] - dc[1] - dc[2] + dc[3]};
        for (int i = 0; i < 4; i++) {
            int a = iabs(t[i]), l = (int)(((int64_t)a * mf[0] + 2 * f) >> (qb + 1));
            if (l > MAXLEVEL) l = MAXLEVEL;
            k->cdc[c][i] = (int16_t)(t[i] < 0 ? -l : l);
            if (l) cbp = imax(cbp, 1);
        }
    }
    if (!intra && acscore < 7) { /* a few lone +-1s in the chroma AC aren't worth their bits */
        memset(k->cac, 0, sizeof k->cac);
        memset(mb->nzc, 0, sizeof mb->nzc);
    }
    for (int c = 0; c < 2; c++)
        for (int bl = 0; bl < 4; bl++)
            if (mb->nzc[c][bl]) cbp = 2;
    if (cbp < 2) memset(mb->nzc, 0, sizeof mb->nzc);
    /* reconstruction (8.5.11) */
    int ls = 16 * norm_v[qpc % 6][0];
    for (int c = 0; c < 2; c++) {
        uint8_t *rec = (c ? e->f[e->cur].v : e->f[e->cur].u) + (ptrdiff_t)(mby * 8) * e->sc + mbx * 8;
        const int16_t *cl = k->cdc[c];
        int fh[4] = {cl[0] + cl[1] + cl[2] + cl[3], cl[0] - cl[1] + cl[2] - cl[3], cl[0] + cl[1] - cl[2] - cl[3],
                     cl[0] - cl[1] - cl[2] + cl[3]};
        for (int bl = 0; bl < 4; bl++) {
            int bx = bl & 1, by = bl >> 1;
            int32_t d[16] = {0};
            if (cbp == 2) dequant4(d, k->cac[c][bl], dq, 1);
            d[0] = (fh[bl] * ls * (1 << (qpc / 6))) >> 5; /* multiply: shifting a negative left is undefined */
            const uint8_t *p = pred[c] + by * 4 * 8 + bx * 4;
            uint8_t *dst = rec + (ptrdiff_t)(by * 4) * e->sc + bx * 4;
            if (!cbp) copy_block(dst, e->sc, p, 8, 4, 4);
            else idct4_add(dst, e->sc, p, 8, d);
        }
    }
    if (cbp < 2) memset(k->cac, 0, sizeof k->cac);
    if (!cbp) memset(k->cdc, 0, sizeof k->cdc);
    return cbp;
}

static void chroma_edges(H264Enc *e, int mbx, int mby, Edge g[2]) {
    Frame *f = &e->f[e->cur];
    edges16(e, f->u, e->sc, mbx * 8, mby * 8, 8, &g[0]);
    edges16(e, f->v, e->sc, mbx * 8, mby * 8, 8, &g[1]);
}

/* Intra chroma: pick the mode by SATD over both planes, then code it. */
static int code_chroma_intra(H264Enc *e, Mb *mb, int mbx, int mby) {
    Edge g[2];
    chroma_edges(e, mbx, mby, g);
    uint8_t pred[2][64], best[2][64];
    int bestc = -1, bestm = 0;
    for (int m = 0; m < 4; m++) {
        if ((m == 1 && !g[0].has_l) || (m == 2 && !g[0].has_t) || (m == 3 && !(g[0].has_t && g[0].has_l))) continue;
        int cost = 0;
        for (int c = 0; c < 2; c++) {
            pred_chroma(pred[c], m, g[c].t + 1, g[c].l, g[c].t[0], g[c].has_t, g[c].has_l);
            const uint8_t *src = (c ? e->srcv : e->srcu) + (ptrdiff_t)(mby * 8) * e->ssc + mbx * 8;
            cost += satd(src, e->ssc, pred[c], 8, 8, 8);
        }
        cost += e->lambda * ue_bits((uint32_t)m);
        if (bestc < 0 || cost < bestc) {
            bestc = cost;
            bestm = m;
            memcpy(best, pred, sizeof best);
        }
    }
    e->code.cmode = (uint8_t)bestm;
    return code_chroma(e, mb, mbx, mby, (const uint8_t(*)[64])best, true);
}

/* Intra 16x16 luma with the given mode: prediction, transform, DC Hadamard,
   quantisation, reconstruction. Returns the luma CBP (0 or 15). */
static int code_i16(H264Enc *e, Mb *mb, int mbx, int mby, int mode, const Edge *g) {
    Code *k = &e->code;
    Frame *fr = &e->f[e->cur];
    uint8_t pred[256];
    pred16(pred, mode, g->t + 1, g->l, g->t[0], g->has_t, g->has_l);
    const uint8_t *src = e->srcy + (ptrdiff_t)(mby * 16) * e->ssy + mbx * 16;
    int qp = e->qp, qb = qbits_of(qp), f = (1 << qb) / 3;
    const int *mf = e->mf[qp], *dq = e->dq[qp];
    int32_t coef[16][16];
    int dc[16], any = 0;
    for (int r = 0; r < 16; r++) {
        int bx = r & 3, by = r >> 2;
        int16_t res[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                res[y * 4 + x] = (int16_t)(src[(by * 4 + y) * e->ssy + bx * 4 + x] - pred[(by * 4 + y) * 16 + bx * 4 + x]);
        fdct4(coef[r], res);
        dc[r] = coef[r][0]; /* DC matrix [row][col] = block (col, row) */
        mb->nz[r] = (uint8_t)quant4(k->luma[r], coef[r], mf, qb, f, 1);
        any |= mb->nz[r];
    }
    /* 4x4 Hadamard of the DCs, halved, quantised like the chroma DC (8.5.10) */
    int t[16], u[16];
    for (int i = 0; i < 4; i++) {
        const int *x = dc + i * 4;
        int s01 = x[0] + x[1], d01 = x[0] - x[1], s23 = x[2] + x[3], d23 = x[2] - x[3];
        t[i * 4 + 0] = s01 + s23;
        t[i * 4 + 1] = s01 - s23;
        t[i * 4 + 2] = d01 - d23;
        t[i * 4 + 3] = d01 + d23;
    }
    for (int j = 0; j < 4; j++) {
        int s01 = t[j] + t[4 + j], d01 = t[j] - t[4 + j], s23 = t[8 + j] + t[12 + j], d23 = t[8 + j] - t[12 + j];
        u[j] = (s01 + s23) >> 1;
        u[4 + j] = (s01 - s23) >> 1;
        u[8 + j] = (d01 - d23) >> 1;
        u[12 + j] = (d01 + d23) >> 1;
    }
    for (int i = 0; i < 16; i++) {
        int pos = zigzag[i], c = u[pos], a = iabs(c);
        int l = (int)(((int64_t)a * mf[0] + 2 * f) >> (qb + 1));
        if (l > MAXLEVEL) l = MAXLEVEL;
        k->dc[i] = (int16_t)(c < 0 ? -l : l);
    }
    int cbp = any ? 15 : 0;
    if (!cbp) memset(mb->nz, 0, sizeof mb->nz);
    /* reconstruction: inverse DC transform and scaling (8.5.10), then each block */
    int c4[16], g4[16], h4[16];
    for (int i = 0; i < 16; i++) c4[zigzag[i]] = k->dc[i];
    for (int i = 0; i < 4; i++) {
        const int *x = c4 + i * 4;
        int s01 = x[0] + x[1], d01 = x[0] - x[1], s23 = x[2] + x[3], d23 = x[2] - x[3];
        g4[i * 4 + 0] = s01 + s23;
        g4[i * 4 + 1] = s01 - s23;
        g4[i * 4 + 2] = d01 - d23;
        g4[i * 4 + 3] = d01 + d23;
    }
    for (int j = 0; j < 4; j++) {
        int s01 = g4[j] + g4[4 + j], d01 = g4[j] - g4[4 + j], s23 = g4[8 + j] + g4[12 + j], d23 = g4[8 + j] - g4[12 + j];
        h4[j] = s01 + s23;
        h4[4 + j] = s01 - s23;
        h4[8 + j] = d01 - d23;
        h4[12 + j] = d01 + d23;
    }
    int ls = 16 * norm_v[qp % 6][0];
    uint8_t *rec = fr->y + (ptrdiff_t)(mby * 16) * e->sy + mbx * 16;
    for (int r = 0; r < 16; r++) {
        int bx = r & 3, by = r >> 2;
        int32_t d[16] = {0};
        if (cbp) dequant4(d, k->luma[r], dq, 1);
        d[0] = qp >= 36 ? h4[r] * ls * (1 << (qp / 6 - 6)) : (h4[r] * ls + (1 << (5 - qp / 6))) >> (6 - qp / 6);
        idct4_add(rec + (ptrdiff_t)(by * 4) * e->sy + bx * 4, e->sy, pred + by * 4 * 16 + bx * 4, 16, d);
    }
    if (!cbp) memset(k->luma, 0, sizeof k->luma);
    k->mode16 = (uint8_t)mode;
    return cbp;
}

/* Best intra 16x16 mode by SATD; returns its cost. */
static int best_i16(H264Enc *e, int mbx, int mby, const Edge *g, int *mode) {
    const uint8_t *src = e->srcy + (ptrdiff_t)(mby * 16) * e->ssy + mbx * 16;
    uint8_t pred[256];
    int best = -1;
    for (int m = 0; m < 4; m++) {
        if ((m == 0 && !g->has_t) || (m == 1 && !g->has_l) || (m == 3 && !(g->has_t && g->has_l))) continue;
        pred16(pred, m, g->t + 1, g->l, g->t[0], g->has_t, g->has_l);
        int cost = satd(src, e->ssy, pred, 16, 16, 16) + e->lambda * 4;
        if (best < 0 || cost < best) {
            best = cost;
            *mode = m;
        }
    }
    return best;
}

/* Intra 4x4: choose and code the 16 blocks in decoding order (each block's
   prediction needs its reconstructed neighbours). Returns the SATD cost. */
static int code_i4(H264Enc *e, Mb *mb, int mbx, int mby) {
    Code *k = &e->code;
    Frame *fr = &e->f[e->cur];
    int qp = e->qp, qb = qbits_of(qp), f = (1 << qb) / 3, total = e->lambda * 6;
    const int *mf = e->mf[qp], *dq = e->dq[qp];
    const Mb *left = mbx > 0 ? mb_at(e, mbx - 1, mby) : NULL, *top = mby > 0 ? mb_at(e, mbx, mby - 1) : NULL;
    for (int blk = 0; blk < 16; blk++) {
        int r = blk2raster[blk], bx = r & 3, by = r >> 2, px = mbx * 16 + bx * 4, py = mby * 16 + by * 4;
        const uint8_t *src = e->srcy + (ptrdiff_t)py * e->ssy + px;
        uint8_t *rec = fr->y + (ptrdiff_t)py * e->sy + px;
        bool has_t = py > 0, has_l = px > 0, has_tr;
        if (!has_t) has_tr = false;
        else if (by == 0) has_tr = bx < 3 || mbx < e->mbw - 1;
        else has_tr = bx < 3 && blk2raster[(by - 1) * 4 + bx + 1] < blk; /* decoded already? */
        uint8_t t[9] = {0}, l[4] = {0};
        if (has_t) {
            memcpy(t + 1, rec - e->sy, 4);
            if (has_tr) memcpy(t + 5, rec - e->sy + 4, 4);
            else memset(t + 5, t[4], 4);
        }
        if (has_l)
            for (int i = 0; i < 4; i++) l[i] = rec[i * e->sy - 1];
        if (has_t && has_l) t[0] = rec[-e->sy - 1];
        /* predicted mode (8.3.1.1) */
        int ma = -1, mb_ = -1;
        if (bx > 0) ma = mb->i4[r - 1];
        else if (left) ma = left->type == MB_I4 ? left->i4[r + 3] : 2;
        if (by > 0) mb_ = mb->i4[r - 4];
        else if (top) mb_ = top->type == MB_I4 ? top->i4[r + 12] : 2;
        int predm = ma < 0 || mb_ < 0 ? 2 : imin(ma, mb_);
        int bestc = -1, bestm = 2;
        uint8_t pred[16], bp[16];
        for (int m = 0; m < 9; m++) {
            bool ok = m == 2 || ((m == 0 || m == 3 || m == 7) && has_t) || ((m == 1 || m == 8) && has_l) ||
                      ((m == 4 || m == 5 || m == 6) && has_t && has_l);
            if (!ok) continue;
            pred4x4(pred, m, t, l, has_t, has_l);
            int cost = satd4(src, e->ssy, pred, 4) + e->lambda * (m == predm ? 1 : 4);
            if (bestc < 0 || cost < bestc) {
                bestc = cost;
                bestm = m;
                memcpy(bp, pred, 16);
            }
        }
        total += bestc;
        mb->i4[r] = (int8_t)bestm;
        k->i4flag[r] = bestm == predm;
        k->i4rem[r] = (uint8_t)(bestm < predm ? bestm : bestm - 1);
        int16_t res[16];
        int32_t coef[16], d[16] = {0};
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) res[y * 4 + x] = (int16_t)(src[y * e->ssy + x] - bp[y * 4 + x]);
        fdct4(coef, res);
        mb->nz[r] = (uint8_t)quant4(k->luma[r], coef, mf, qb, f, 0);
        if (mb->nz[r]) {
            dequant4(d, k->luma[r], dq, 0);
            idct4_add(rec, e->sy, bp, 4, d);
        } else copy_block(rec, e->sy, bp, 4, 4, 4);
    }
    return total;
}

static int luma_cbp(const Mb *mb) {
    int cbp = 0;
    for (int r = 0; r < 16; r++)
        if (mb->nz[r]) cbp |= 1 << ((r >> 3) * 2 + ((r & 3) >> 1));
    return cbp;
}

static void set_mv(Mb *mb, int x4, int y4, int w4, int h4, Mv mv) {
    for (int y = y4; y < y4 + h4; y++)
        for (int x = x4; x < x4 + w4; x++) {
            mb->mv[y * 4 + x][0] = (int16_t)mv.x;
            mb->mv[y * 4 + x][1] = (int16_t)mv.y;
        }
}

/* Inter macroblock from the motion in mb->mv: prediction, residual, dropping
   cheap coefficients, reconstruction. Sets cbp. */
static void code_inter(H264Enc *e, Mb *mb, int mbx, int mby) {
    Code *k = &e->code;
    Frame *fr = &e->f[e->cur], *ref = &e->f[e->cur ^ 1];
    uint8_t pred[256], cpred[2][64];
    int x = mbx * 16, y = mby * 16;
    if (mb->type == MB_P16x8)
        for (int p = 0; p < 2; p++) {
            Mv mv = {mb->mv[p * 8][0], mb->mv[p * 8][1]};
            mc_luma(e, ref, x, y + p * 8, mv, 16, 8, pred + p * 128, 16);
            mc_chroma(e, ref->u, x / 2, y / 2 + p * 4, mv, 8, 4, cpred[0] + p * 32, 8);
            mc_chroma(e, ref->v, x / 2, y / 2 + p * 4, mv, 8, 4, cpred[1] + p * 32, 8);
        }
    else if (mb->type == MB_P8x16)
        for (int p = 0; p < 2; p++) {
            Mv mv = {mb->mv[p * 2][0], mb->mv[p * 2][1]};
            mc_luma(e, ref, x + p * 8, y, mv, 8, 16, pred + p * 8, 16);
            mc_chroma(e, ref->u, x / 2 + p * 4, y / 2, mv, 4, 8, cpred[0] + p * 4, 8);
            mc_chroma(e, ref->v, x / 2 + p * 4, y / 2, mv, 4, 8, cpred[1] + p * 4, 8);
        }
    else {
        Mv mv = {mb->mv[0][0], mb->mv[0][1]};
        mc_luma(e, ref, x, y, mv, 16, 16, pred, 16);
        mc_chroma(e, ref->u, x / 2, y / 2, mv, 8, 8, cpred[0], 8);
        mc_chroma(e, ref->v, x / 2, y / 2, mv, 8, 8, cpred[1], 8);
    }
    const uint8_t *src = e->srcy + (ptrdiff_t)y * e->ssy + x;
    int qp = e->qp, qb = qbits_of(qp), f = (1 << qb) / 6;
    const int *mf = e->mf[qp], *dq = e->dq[qp];
    int score8[4] = {0}, score = 0;
    for (int r = 0; r < 16; r++) {
        int bx = r & 3, by = r >> 2;
        int16_t res[16];
        int32_t coef[16];
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                res[j * 4 + i] = (int16_t)(src[(by * 4 + j) * e->ssy + bx * 4 + i] - pred[(by * 4 + j) * 16 + bx * 4 + i]);
        fdct4(coef, res);
        mb->nz[r] = (uint8_t)quant4(k->luma[r], coef, mf, qb, f, 0);
        int s = mb->nz[r] ? decimate_score(k->luma[r], 16) : 0;
        score8[(by >> 1) * 2 + (bx >> 1)] += s;
    }
    for (int i = 0; i < 4; i++) {
        if (score8[i] < 4) /* this 8x8 isn't worth coding */
            for (int r = 0; r < 16; r++)
                if ((r >> 3) * 2 + ((r & 3) >> 1) == i) {
                    mb->nz[r] = 0;
                    memset(k->luma[r], 0, sizeof k->luma[r]);
                }
        score += score8[i] < 4 ? 0 : score8[i];
    }
    if (score && score < 6) { /* nor the whole macroblock */
        memset(mb->nz, 0, sizeof mb->nz);
        memset(k->luma, 0, sizeof k->luma);
    }
    uint8_t *rec = fr->y + (ptrdiff_t)y * e->sy + x;
    for (int r = 0; r < 16; r++) {
        int bx = r & 3, by = r >> 2;
        uint8_t *dst = rec + (ptrdiff_t)(by * 4) * e->sy + bx * 4;
        const uint8_t *p = pred + by * 4 * 16 + bx * 4;
        if (mb->nz[r]) {
            int32_t d[16] = {0};
            dequant4(d, k->luma[r], dq, 0);
            idct4_add(dst, e->sy, p, 16, d);
        } else copy_block(dst, e->sy, p, 16, 4, 4);
    }
    mb->cbp = (uint8_t)(luma_cbp(mb) | code_chroma(e, mb, mbx, mby, (const uint8_t(*)[64])cpred, false) << 4);
}

/* ---------------------------------------------------------- motion search */

typedef struct {
    int x0, x1, y0, y1; /* allowed full-pel block positions */
} Range;

static Range range_of(const H264Enc *e, int x, int y, int w, int h) {
    Range r = {imax(-MARGIN, x - 2047), imin(e->w16 - w + MARGIN, x + 2047), imax(-MARGIN, y - e->vrange + 1),
               imin(e->h16 - h + MARGIN, y + e->vrange - 2)};
    return r;
}

static Mv clamp_mv(Mv mv, const Range *r, int x, int y) { /* full pels */
    mv.x = clip3(r->x0 - x, r->x1 - x, mv.x);
    mv.y = clip3(r->y0 - y, r->y1 - y, mv.y);
    return mv;
}

/* Integer search (candidates, hexagon, square), then half- and quarter-pel
   refinement by SATD. Returns the cost; *best gets the vector (quarter pels). */
static int search(H264Enc *e, int x, int y, int w, int h, Mv pmv, const Mv *cand, int ncand, Mv *best) {
    const Frame *ref = &e->f[e->cur ^ 1];
    const uint8_t *src = e->srcy + (ptrdiff_t)y * e->ssy + x;
    Range rg = range_of(e, x, y, w, h);
    int lam = e->lambda, s = e->sy;
    Mv bm = {0, 0};
    int bc = -1;
#define FCOST(mx, my) \
    (sad(src, e->ssy, ref->y + (ptrdiff_t)(y + (my)) * s + x + (mx), s, w, h) + mv_cost(lam, (Mv){(mx) * 4, (my) * 4}, pmv))
    for (int i = 0; i < ncand; i++) {
        Mv c = clamp_mv((Mv){(cand[i].x + 2) >> 2, (cand[i].y + 2) >> 2}, &rg, x, y);
        int cost = FCOST(c.x, c.y);
        if (bc < 0 || cost < bc) {
            bc = cost;
            bm = c;
        }
    }
    static const int8_t hex[6][2] = {{-2, 0}, {-1, -2}, {1, -2}, {2, 0}, {1, 2}, {-1, 2}};
    int iters = e->p.preset == H264_MEDIUM ? 24 : 12;
    for (int it = 0; it < iters; it++) {
        Mv c0 = bm;
        for (int i = 0; i < 6; i++) {
            Mv c = {c0.x + hex[i][0], c0.y + hex[i][1]};
            if (c.x + x < rg.x0 || c.x + x > rg.x1 || c.y + y < rg.y0 || c.y + y > rg.y1) continue;
            int cost = FCOST(c.x, c.y);
            if (cost < bc) {
                bc = cost;
                bm = c;
            }
        }
        if (bm.x == c0.x && bm.y == c0.y) break;
    }
    {
        Mv c0 = bm;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                Mv c = {c0.x + dx, c0.y + dy};
                if ((!dx && !dy) || c.x + x < rg.x0 || c.x + x > rg.x1 || c.y + y < rg.y0 || c.y + y > rg.y1) continue;
                int cost = FCOST(c.x, c.y);
                if (cost < bc) {
                    bc = cost;
                    bm = c;
                }
            }
    }
#undef FCOST
    /* sub-pel by SATD: half pels, then quarter pels; a diamond in the fast
       preset, the whole square in medium */
    static const int8_t around[8][2] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    int npts = e->p.preset == H264_MEDIUM ? 8 : 4;
    Mv q = {bm.x * 4, bm.y * 4};
    int qc = satd_mv(e, ref, src, e->ssy, x, y, q, w, h) + mv_cost(lam, q, pmv);
    for (int step = 2; step >= 1; step--) {
        Mv c0 = q;
        for (int i = 0; i < npts; i++) {
            Mv c = {c0.x + around[i][0] * step, c0.y + around[i][1] * step};
            if ((c.x >> 2) + x < rg.x0 - 1 || (c.x >> 2) + x > rg.x1 || (c.y >> 2) + y < rg.y0 - 1 ||
                (c.y >> 2) + y > rg.y1)
                continue;
            int cost = satd_mv(e, ref, src, e->ssy, x, y, c, w, h) + mv_cost(lam, c, pmv);
            if (cost < qc) {
                qc = cost;
                q = c;
            }
        }
    }
    *best = q;
    return qc;
}

/* ------------------------------------------------------- mode decisions */

static int partition_cost(H264Enc *e, int mbx, int mby, Mb *mb, int type, Mv base, Mv out[2]) {
    int cost = 0, x = mbx * 16, y = mby * 16;
    for (int p = 0; p < 2; p++) {
        int x4 = type == MB_P8x16 ? p * 2 : 0, y4 = type == MB_P16x8 ? p * 2 : 0;
        int w = type == MB_P8x16 ? 8 : 16, h = type == MB_P16x8 ? 8 : 16;
        Mv pmv = mv_pred(e, mbx, mby, mb, x4, y4, w / 4, type == MB_P16x8 ? 1 + p : 3 + p);
        Mv cand[2] = {base, pmv};
        cost += search(e, x + x4 * 4, y + y4 * 4, w, h, pmv, cand, 2, &out[p]);
        set_mv(mb, x4, y4, w / 4, h / 4, out[p]); /* the second partition predicts from the first */
    }
    return cost + e->lambda * 3;
}

static void decide_p(H264Enc *e, Mb *mb, int mbx, int mby) {
    int x = mbx * 16, y = mby * 16, lam = e->lambda;
    const uint8_t *src = e->srcy + (ptrdiff_t)y * e->ssy + x;
    Frame *ref = &e->f[e->cur ^ 1];
    mb->ref = 0;
    mb->type = MB_P16;
    memset(mb->i4, 2, sizeof mb->i4);
    Mv skip = mv_skip(e, mbx, mby, mb);
    /* early skip: if the skip prediction leaves nothing worth coding, we're done */
    {
        uint8_t pred[256];
        mc_luma(e, ref, x, y, skip, 16, 16, pred, 16);
        int qstep = (int)(lam * 4) + 4;
        if (sad(src, e->ssy, pred, 16, 16, 16) < 16 * qstep) {
            set_mv(mb, 0, 0, 4, 4, skip);
            code_inter(e, mb, mbx, mby);
            if (!mb->cbp) {
                mb->type = MB_SKIP;
                return;
            }
        }
    }
    Mv pmv = mv_pred(e, mbx, mby, mb, 0, 0, 4, 0), best;
    Mv cand[6];
    int nc = 0;
    cand[nc++] = pmv;
    cand[nc++] = skip;
    cand[nc++] = (Mv){0, 0};
    int r;
    Mv t;
    if (nb_motion(e, mbx, mby, mb, -1, 0, &r, &t) && r == 0) cand[nc++] = t;
    if (nb_motion(e, mbx, mby, mb, 0, -1, &r, &t) && r == 0) cand[nc++] = t;
    if (nb_motion(e, mbx, mby, mb, 4, -1, &r, &t) && r == 0) cand[nc++] = t;
    int cost16 = search(e, x, y, 16, 16, pmv, cand, nc, &best) + lam;
    int bestc = cost16, type = MB_P16;
    Mv part[2] = {best, best};
    if (e->p.preset == H264_MEDIUM) {
        Mv p8[2], p16[2];
        int c = partition_cost(e, mbx, mby, mb, MB_P16x8, best, p16);
        if (c < bestc) {
            bestc = c;
            type = MB_P16x8;
            part[0] = p16[0], part[1] = p16[1];
        }
        c = partition_cost(e, mbx, mby, mb, MB_P8x16, best, p8);
        if (c < bestc) {
            bestc = c;
            type = MB_P8x16;
            part[0] = p8[0], part[1] = p8[1];
        }
    }
    /* intra instead? */
    Edge g;
    edges16(e, e->f[e->cur].y, e->sy, x, y, 16, &g);
    int mode16 = 2, ci16 = best_i16(e, mbx, mby, &g, &mode16) + lam * 2;
    if (ci16 < bestc) {
        int ci4 = e->p.preset == H264_MEDIUM ? code_i4(e, mb, mbx, mby) : -1;
        mb->ref = -1;
        memset(mb->mv, 0, sizeof mb->mv);
        if (ci4 >= 0 && ci4 < ci16) mb->type = MB_I4;
        else {
            mb->type = MB_I16;
            memset(mb->i4, 2, sizeof mb->i4);
            int cbpl = code_i16(e, mb, mbx, mby, mode16, &g);
            mb->cbp = (uint8_t)cbpl;
        }
        mb->cbp = (uint8_t)((mb->type == MB_I4 ? luma_cbp(mb) : mb->cbp & 15) | code_chroma_intra(e, mb, mbx, mby) << 4);
        return;
    }
    mb->type = (uint8_t)type;
    if (type == MB_P16) set_mv(mb, 0, 0, 4, 4, part[0]);
    else if (type == MB_P16x8) {
        set_mv(mb, 0, 0, 4, 2, part[0]);
        set_mv(mb, 0, 2, 4, 2, part[1]);
    } else {
        set_mv(mb, 0, 0, 2, 4, part[0]);
        set_mv(mb, 2, 0, 2, 4, part[1]);
    }
    code_inter(e, mb, mbx, mby);
    if (type == MB_P16 && !mb->cbp && part[0].x == skip.x && part[0].y == skip.y) mb->type = MB_SKIP;
}

static void decide_i(H264Enc *e, Mb *mb, int mbx, int mby) {
    mb->ref = -1;
    memset(mb->mv, 0, sizeof mb->mv);
    Edge g;
    edges16(e, e->f[e->cur].y, e->sy, mbx * 16, mby * 16, 16, &g);
    int mode16 = 2, ci16 = best_i16(e, mbx, mby, &g, &mode16);
    mb->type = MB_I4;
    int ci4 = code_i4(e, mb, mbx, mby); /* also reconstructs, in case it wins */
    if (ci16 <= ci4) {
        mb->type = MB_I16;
        memset(mb->i4, 2, sizeof mb->i4);
        mb->cbp = (uint8_t)code_i16(e, mb, mbx, mby, mode16, &g);
    } else mb->cbp = (uint8_t)luma_cbp(mb);
    mb->cbp = (uint8_t)((mb->cbp & 15) | code_chroma_intra(e, mb, mbx, mby) << 4);
}

/* ------------------------------------------------------- writing a macroblock */

static int cbp_code(int cbp, bool intra) {
    const uint8_t *t = intra ? cbp_intra_of : cbp_inter_of;
    for (int i = 0; i < 48; i++)
        if (t[i] == cbp) return i;
    return 0;
}

static void write_mb(H264Enc *e, Bw *b, Mb *mb, int mbx, int mby) {
    Code *k = &e->code;
    int base = e->islice ? 0 : 5, cbpl = mb->cbp & 15, cbpc = mb->cbp >> 4;
    switch (mb->type) {
    case MB_P16:
    case MB_P16x8:
    case MB_P8x16: {
        bw_ue(b, mb->type == MB_P16 ? 0u : mb->type == MB_P16x8 ? 1u : 2u);
        int np = mb->type == MB_P16 ? 1 : 2;
        for (int p = 0; p < np; p++) {
            int x4 = mb->type == MB_P8x16 ? p * 2 : 0, y4 = mb->type == MB_P16x8 ? p * 2 : 0;
            int shape = mb->type == MB_P16 ? 0 : mb->type == MB_P16x8 ? 1 + p : 3 + p;
            /* the predictor sees the final vectors of earlier partitions */
            Mv pmv = mv_pred(e, mbx, mby, mb, x4, y4, mb->type == MB_P8x16 ? 2 : 4, shape);
            bw_se(b, mb->mv[y4 * 4 + x4][0] - pmv.x);
            bw_se(b, mb->mv[y4 * 4 + x4][1] - pmv.y);
        }
        break;
    }
    case MB_I4:
        bw_ue(b, (uint32_t)base);
        for (int blk = 0; blk < 16; blk++) {
            int r = blk2raster[blk];
            bw_put(b, k->i4flag[r], 1);
            if (!k->i4flag[r]) bw_put(b, k->i4rem[r], 3);
        }
        bw_ue(b, k->cmode);
        break;
    case MB_I16:
        bw_ue(b, (uint32_t)(base + 1 + k->mode16 + 4 * cbpc + (cbpl ? 12 : 0)));
        bw_ue(b, k->cmode);
        break;
    }
    if (mb->type != MB_I16) bw_ue(b, (uint32_t)cbp_code(mb->cbp & 0x3F, mb->type == MB_I4));
    if (!mb->cbp && mb->type != MB_I16) return;
    bw_se(b, 0); /* mb_qp_delta: one QP per frame */
    if (mb->type == MB_I16) cavlc_block(b, k->dc, 16, nc_luma(e, mbx, mby, mb, 0, 0));
    for (int blk = 0; blk < 16; blk++) {
        int r = blk2raster[blk];
        if (!(cbpl & 1 << (blk >> 2))) continue;
        int nc = nc_luma(e, mbx, mby, mb, r & 3, r >> 2);
        if (mb->type == MB_I16) cavlc_block(b, k->luma[r] + 1, 15, nc);
        else cavlc_block(b, k->luma[r], 16, nc);
    }
    if (cbpc)
        for (int c = 0; c < 2; c++) cavlc_block(b, k->cdc[c], 4, -1);
    if (cbpc == 2)
        for (int c = 0; c < 2; c++)
            for (int bl = 0; bl < 4; bl++) cavlc_block(b, k->cac[c][bl] + 1, 15, nc_chroma(e, mbx, mby, mb, c, bl & 1, bl >> 1));
}

/* ------------------------------------------------------------- deblocking */

static int bs_of(const Mb *p, int pb, const Mb *q, int qb, bool mb_edge) {
    if (p->ref < 0 || q->ref < 0) return mb_edge ? 4 : 3;
    if (p->nz[pb] || q->nz[qb]) return 2;
    if (iabs(p->mv[pb][0] - q->mv[qb][0]) >= 4 || iabs(p->mv[pb][1] - q->mv[qb][1]) >= 4) return 1;
    return 0;
}

/* 8.7.2: one edge of 4 segments x n lines (n = 4 luma, 2 chroma); `across`
   steps over the edge, `along` follows it. */
static void filter_edge(uint8_t *pix, int across, int along, const int bs[4], int qpav, bool chroma, int n) {
    int ia = clip3(0, 51, qpav), alpha = alpha_tab[ia], beta = beta_tab[ia];
    if (!alpha || !beta) return;
    for (int seg = 0; seg < 4; seg++) {
        if (!bs[seg]) continue;
        int tc0 = bs[seg] < 4 ? tc0_tab[ia][bs[seg] - 1] : 0;
        for (int i = 0; i < n; i++) {
            uint8_t *q = pix + (seg * n + i) * along;
            int p0 = q[-across], p1 = q[-2 * across], q0 = q[0], q1 = q[across];
            if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta || iabs(q1 - q0) >= beta) continue;
            if (chroma) {
                if (bs[seg] < 4) {
                    int tc = tc0 + 1, dl = clip3(-tc, tc, ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3);
                    q[-across] = clip1(p0 + dl);
                    q[0] = clip1(q0 - dl);
                } else {
                    q[-across] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
                    q[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
                }
                continue;
            }
            int p2 = q[-3 * across], q2 = q[2 * across];
            int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
            if (bs[seg] < 4) {
                int tc = tc0 + (ap < beta) + (aq < beta);
                int dl = clip3(-tc, tc, ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3);
                q[-across] = clip1(p0 + dl);
                q[0] = clip1(q0 - dl);
                if (ap < beta) q[-2 * across] = (uint8_t)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
                if (aq < beta) q[across] = (uint8_t)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
            } else {
                int p3 = q[-4 * across], q3 = q[3 * across];
                bool strong = iabs(p0 - q0) < ((alpha >> 2) + 2);
                if (ap < beta && strong) {
                    q[-across] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                    q[-2 * across] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                    q[-3 * across] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
                } else q[-across] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
                if (aq < beta && strong) {
                    q[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                    q[across] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                    q[2 * across] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
                } else q[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
            }
        }
    }
}

static void deblock(H264Enc *e) {
    Frame *f = &e->f[e->cur];
    for (int mby = 0; mby < e->mbh; mby++)
        for (int mbx = 0; mbx < e->mbw; mbx++) {
            const Mb *q = mb_at(e, mbx, mby);
            uint8_t *py = f->y + (ptrdiff_t)(mby * 16) * e->sy + mbx * 16;
            uint8_t *pu = f->u + (ptrdiff_t)(mby * 8) * e->sc + mbx * 8, *pv = f->v + (ptrdiff_t)(mby * 8) * e->sc + mbx * 8;
            for (int dir = 0; dir < 2; dir++) { /* 0: vertical edges, 1: horizontal edges */
                int bsv[4][4];
                for (int edge = 0; edge < 4; edge++) {
                    if (edge == 0 && (dir ? mby == 0 : mbx == 0)) {
                        memset(bsv[0], 0, sizeof bsv[0]);
                        continue;
                    }
                    const Mb *p = edge ? q : dir ? mb_at(e, mbx, mby - 1) : mb_at(e, mbx - 1, mby);
                    for (int s = 0; s < 4; s++) {
                        int qb = dir ? edge * 4 + s : s * 4 + edge;
                        int pb = dir ? (edge ? qb - 4 : 12 + s) : (edge ? qb - 1 : s * 4 + 3);
                        bsv[edge][s] = bs_of(p, pb, q, qb, edge == 0);
                    }
                    int qpav = (p->qp + q->qp + 1) >> 1;
                    if (dir) filter_edge(py + edge * 4 * e->sy, e->sy, 1, bsv[edge], qpav, false, 4);
                    else filter_edge(py + edge * 4, 1, e->sy, bsv[edge], qpav, false, 4);
                }
                for (int edge = 0; edge < 4; edge += 2) { /* chroma: luma edges 0 and 2 */
                    if (edge == 0 && (dir ? mby == 0 : mbx == 0)) continue;
                    const Mb *p = edge ? q : dir ? mb_at(e, mbx, mby - 1) : mb_at(e, mbx - 1, mby);
                    int qpav = (chroma_qp[p->qp] + chroma_qp[q->qp] + 1) >> 1;
                    int o = edge * 2; /* chroma samples */
                    if (dir) {
                        filter_edge(pu + o * e->sc, e->sc, 1, bsv[edge], qpav, true, 2);
                        filter_edge(pv + o * e->sc, e->sc, 1, bsv[edge], qpav, true, 2);
                    } else {
                        filter_edge(pu + o, 1, e->sc, bsv[edge], qpav, true, 2);
                        filter_edge(pv + o, 1, e->sc, bsv[edge], qpav, true, 2);
                    }
                }
            }
        }
}

/* ------------------------------------------------------------ the frame */

static void load_source(H264Enc *e, const H264Picture *pic) {
    int cw = e->w / 2, ch = e->h / 2;
    for (int y = 0; y < e->h16; y++) {
        uint8_t *d = e->srcy + (ptrdiff_t)y * e->ssy;
        memcpy(d, pic->y + (ptrdiff_t)imin(y, e->h - 1) * pic->stride_y, (size_t)e->w);
        memset(d + e->w, d[e->w - 1], (size_t)(e->w16 - e->w));
    }
    for (int y = 0; y < e->h16 / 2; y++) {
        uint8_t *du = e->srcu + (ptrdiff_t)y * e->ssc, *dv = e->srcv + (ptrdiff_t)y * e->ssc;
        memcpy(du, pic->u + (ptrdiff_t)imin(y, ch - 1) * pic->stride_u, (size_t)cw);
        memcpy(dv, pic->v + (ptrdiff_t)imin(y, ch - 1) * pic->stride_v, (size_t)cw);
        memset(du + cw, du[cw - 1], (size_t)(e->w16 / 2 - cw));
        memset(dv + cw, dv[cw - 1], (size_t)(e->w16 / 2 - cw));
    }
}

bool h264enc_encode(H264Enc *e, const H264Picture *pic, H264Packet *out) {
    bool idr = e->gop == 0;
    e->islice = idr;
    int qp = clip3(1, 51, e->p.quality + e->qp_add - (idr ? 3 : 0));
    e->qp = qp;
    e->qpc = chroma_qp[qp];
    e->lambda = lambda_tab[qp];
    load_source(e, pic);
    if (idr) e->frame_num = 0;

    Bw *b = &e->slice;
    bw_reset(b);
    bw_ue(b, 0);                     /* first_mb_in_slice */
    bw_ue(b, idr ? 7u : 5u);         /* slice_type: I or P, the same for the whole picture */
    bw_ue(b, 0);                     /* pic_parameter_set_id */
    bw_put(b, (uint32_t)e->frame_num & ((1u << e->log2fn) - 1), e->log2fn);
    if (idr) bw_ue(b, (uint32_t)(e->idr_id & 1));
    else {
        bw_put(b, 0, 1); /* num_ref_idx_active_override_flag */
        bw_put(b, 0, 1); /* ref_pic_list_modification_flag_l0 */
    }
    if (idr) {
        bw_put(b, 0, 1); /* no_output_of_prior_pics_flag */
        bw_put(b, 0, 1); /* long_term_reference_flag */
    } else bw_put(b, 0, 1); /* adaptive_ref_pic_marking_mode_flag: sliding window */
    bw_se(b, qp - 26);       /* slice_qp_delta */

    int skips = 0;
    for (int mby = 0; mby < e->mbh; mby++)
        for (int mbx = 0; mbx < e->mbw; mbx++) {
            Mb *mb = mb_at(e, mbx, mby);
            memset(mb, 0, sizeof *mb);
            mb->qp = (uint8_t)qp;
            if (idr) decide_i(e, mb, mbx, mby);
            else decide_p(e, mb, mbx, mby);
            if (mb->type == MB_SKIP) {
                skips++;
                memset(mb->nz, 0, sizeof mb->nz);
                memset(mb->nzc, 0, sizeof mb->nzc);
                mb->cbp = 0;
                continue;
            }
            if (!idr) {
                bw_ue(b, (uint32_t)skips);
                skips = 0;
            }
            write_mb(e, b, mb, mbx, mby);
        }
    if (skips) bw_ue(b, (uint32_t)skips);
    bw_trailing(b);

    deblock(e);
    Frame *f = &e->f[e->cur];
    extend_plane(f->y, e->sy, e->w16, e->h16, PAD);
    extend_plane(f->u, e->sc, e->w16 / 2, e->h16 / 2, PADC);
    extend_plane(f->v, e->sc, e->w16 / 2, e->h16 / 2, PADC);
    if (e->gop + 1 < e->keyint) half_pels(e, f); /* the next frame predicts from this one */

    bw_reset(&e->out);
    if (idr && e->p.annexb) {
        static const uint8_t sc[4] = {0, 0, 0, 1};
        bytes_put(&e->out, sc, 4);
        bytes_put(&e->out, e->sps, e->sps_len);
        bytes_put(&e->out, sc, 4);
        bytes_put(&e->out, e->pps, e->pps_len);
    }
    nal_put(&e->out, e->p.annexb, idr ? 3 : 2, idr ? 5 : 1, b->buf, b->len);
    if (e->out.oom || b->oom) return false;

    /* bitrate cap: a leaky bucket two seconds deep */
    if (e->p.max_kbps > 0) {
        e->vbv += (double)e->out.len * 8 - e->frame_bits;
        if (e->vbv < 0) e->vbv = 0;
        if (e->vbv > e->vbv_size * 0.5 && e->qp_add < 18) e->qp_add++;
        else if (e->vbv < e->vbv_size * 0.2 && e->qp_add > 0) e->qp_add--;
    }

    out->data = e->out.buf;
    out->size = e->out.len;
    out->idr = idr;
    out->pts = pic->pts;
    out->qp = qp;
    out->frame = e->frame;
    e->cur ^= 1;
    e->frame++;
    e->frame_num++;
    if (++e->gop >= e->keyint) {
        e->gop = 0;
        e->idr_id++;
    }
    return true;
}

/* ------------------------------------------------------------------ API */

H264Enc *h264enc_new(const H264Params *pp) {
    H264Params p = *pp;
    /* hardware decoders refuse tiny pictures (a GPU decoder rejected 2x2), so 16x16 is the minimum */
    if (p.width < 16 || p.height < 16 || p.width > 4096 || p.height > 4096 || (p.width & 1) || (p.height & 1)) return NULL;
    if (p.fps_num <= 0 || p.fps_den <= 0) p.fps_num = 25, p.fps_den = 1;
    if (p.sar_w <= 0 || p.sar_h <= 0) p.sar_w = p.sar_h = 1;
    while (p.sar_w > 65535 || p.sar_h > 65535) p.sar_w /= 2, p.sar_h /= 2;
    if (p.quality <= 0) p.quality = 23;
    p.quality = clip3(1, 51, p.quality);
    if (p.colour != 601 && p.colour != 709) p.colour = p.height >= 720 ? 709 : 601;
    H264Enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->p = p;
    e->w = p.width;
    e->h = p.height;
    e->w16 = (p.width + 15) & ~15;
    e->h16 = (p.height + 15) & ~15;
    e->mbw = e->w16 / 16;
    e->mbh = e->h16 / 16;
    e->sy = e->w16 + 2 * PAD;
    e->sc = e->w16 / 2 + 2 * PADC;
    double fps = (double)p.fps_num / p.fps_den;
    e->keyint = p.keyint > 0 ? p.keyint : imax(1, (int)(fps * 2 + 0.5));
    e->log2fn = 4;
    while ((1 << e->log2fn) < e->keyint + 1 && e->log2fn < 16) e->log2fn++;
    e->level = pick_level(e->mbw * e->mbh, e->mbw * e->mbh * fps, p.max_kbps);
    e->vrange = e->level <= 10 ? 64 : e->level <= 20 ? 128 : e->level <= 30 ? 256 : 512;
    if (p.max_kbps > 0) {
        e->vbv_size = p.max_kbps * 1000.0 * 2;
        e->frame_bits = p.max_kbps * 1000.0 / fps;
    }
    for (int qp = 0; qp < 52; qp++)
        for (int pos = 0; pos < 16; pos++) {
            e->mf[qp][pos] = quant_mf[qp % 6][pos_class(pos)];
            e->dq[qp][pos] = norm_v[qp % 6][pos_class(pos)] << (qp / 6);
        }
    e->ssy = e->w16;
    e->ssc = e->w16 / 2;
    e->src_mem = malloc((size_t)e->w16 * e->h16 * 3 / 2);
    e->mbs = calloc((size_t)e->mbw * e->mbh, sizeof *e->mbs);
    if (!e->src_mem || !e->mbs || !frame_alloc(e, &e->f[0], NULL) || !frame_alloc(e, &e->f[1], &e->f[0])) {
        h264enc_free(e);
        return NULL;
    }
    e->srcy = e->src_mem;
    e->srcu = e->srcy + (size_t)e->w16 * e->h16;
    e->srcv = e->srcu + (size_t)e->w16 * e->h16 / 4;
    write_sps(e);
    write_pps(e);
    if (!e->sps_len || !e->pps_len) {
        h264enc_free(e);
        return NULL;
    }
    return e;
}

void h264enc_free(H264Enc *e) {
    if (!e) return;
    free(e->f[0].mem);
    free(e->f[1].mem);
    free(e->src_mem);
    free(e->mbs);
    free(e->slice.buf);
    free(e->out.buf);
    free(e);
}

const uint8_t *h264enc_sps(const H264Enc *e, size_t *n) {
    *n = e->sps_len;
    return e->sps;
}

const uint8_t *h264enc_pps(const H264Enc *e, size_t *n) {
    *n = e->pps_len;
    return e->pps;
}

size_t h264enc_avcc(const H264Enc *e, uint8_t *out, size_t cap) {
    size_t n = 11 + e->sps_len + e->pps_len;
    if (cap < n) return 0;
    uint8_t *p = out;
    *p++ = 1;            /* configurationVersion */
    *p++ = e->sps[1];    /* AVCProfileIndication */
    *p++ = e->sps[2];    /* profile_compatibility */
    *p++ = e->sps[3];    /* AVCLevelIndication */
    *p++ = 0xFF;         /* 6 reserved bits, lengthSizeMinusOne = 3 */
    *p++ = 0xE1;         /* 3 reserved bits, one SPS */
    *p++ = (uint8_t)(e->sps_len >> 8);
    *p++ = (uint8_t)e->sps_len;
    memcpy(p, e->sps, e->sps_len);
    p += e->sps_len;
    *p++ = 1; /* one PPS */
    *p++ = (uint8_t)(e->pps_len >> 8);
    *p++ = (uint8_t)e->pps_len;
    memcpy(p, e->pps, e->pps_len);
    return n;
}

void h264enc_codec(const H264Enc *e, char out[16]) {
    static const char hex[] = "0123456789ABCDEF";
    memcpy(out, "avc1.", 5);
    for (int i = 0; i < 3; i++) {
        out[5 + i * 2] = hex[e->sps[1 + i] >> 4];
        out[6 + i * 2] = hex[e->sps[1 + i] & 15];
    }
    out[11] = '\0';
}

void h264enc_recon(const H264Enc *e, const uint8_t **y, const uint8_t **u, const uint8_t **v, int *stride_y,
                   int *stride_c) {
    const Frame *f = &e->f[e->cur ^ 1]; /* the frame just coded */
    *y = f->y;
    *u = f->u;
    *v = f->v;
    *stride_y = e->sy;
    *stride_c = e->sc;
}

/* ------------------------------------------------------------- test hooks */
/* Not API: declared in h264enc_test.c to check the pieces against the standard. */

/* One CAVLC residual block into out (MSB first); returns the number of bits. */
size_t h264enc_cavlc_for_test(const int16_t *coef, int n, int nc, uint8_t *out, size_t cap) {
    Bw b = {0};
    cavlc_block(&b, coef, n, nc);
    size_t bits = b.len * 8 + (size_t)b.nacc;
    bw_flush(&b);
    if (b.oom || b.len > cap) bits = 0;
    else if (b.len) memcpy(out, b.buf, b.len);
    free(b.buf);
    return bits;
}

/* Motion vector prediction (8.4.1.3) and the P_Skip vector (8.4.1.1) for the
   macroblock at (mbx, mby) of an mbw x mbh picture. st[i] = {ref, mvx, mvy} of
   macroblock i (ref -1 = intra), the same vector in all its blocks. shape:
   0 = 16x16, 1/2 = 16x8 top/bottom, 3/4 = 8x16 left/right; for 2 and 4 the
   first partition moves by first[]. shape -1 = the P_Skip vector. */
void h264enc_mvpred_for_test(const int (*st)[3], int mbw, int mbh, int mbx, int mby, int shape, const int first[2],
                             int out[2]) {
    H264Enc e;
    memset(&e, 0, sizeof e);
    e.mbw = mbw;
    e.mbh = mbh;
    e.mbs = calloc((size_t)mbw * mbh, sizeof *e.mbs);
    if (!e.mbs) return;
    for (int i = 0; i < mbw * mbh; i++) {
        e.mbs[i].ref = (int8_t)st[i][0];
        set_mv(&e.mbs[i], 0, 0, 4, 4, (Mv){st[i][0] < 0 ? 0 : st[i][1], st[i][0] < 0 ? 0 : st[i][2]});
    }
    Mb *cur = mb_at(&e, mbx, mby);
    memset(cur, 0, sizeof *cur);
    if (shape == 2) set_mv(cur, 0, 0, 4, 2, (Mv){first[0], first[1]});
    if (shape == 4) set_mv(cur, 0, 0, 2, 4, (Mv){first[0], first[1]});
    static const int8_t geo[5][3] = {{0, 0, 4}, {0, 0, 4}, {0, 2, 4}, {0, 0, 2}, {2, 0, 2}};
    Mv p = shape < 0 ? mv_skip(&e, mbx, mby, cur) : mv_pred(&e, mbx, mby, cur, geo[shape][0], geo[shape][1], geo[shape][2], shape);
    out[0] = p.x;
    out[1] = p.y;
    free(e.mbs);
}

/* The deblocking filter on one line of 8 samples p3 p2 p1 p0 | q0 q1 q2 q3. */
void h264enc_filter_for_test(uint8_t line[8], int bs, int qp, bool chroma) {
    int bsv[4] = {bs, 0, 0, 0};
    filter_edge(line + 4, 1, 0, bsv, qp, chroma, 1);
}

/* 8.5.12.2 on scaled coefficients d (raster), added to a flat prediction of 128. */
void h264enc_idct_for_test(const int32_t d[16], uint8_t out[16]) {
    uint8_t pred[16];
    memset(pred, 128, sizeof pred);
    idct4_add(out, 4, pred, 4, d);
}

/* The code words the encoder writes (value; *len = 0 when not a valid entry):
   kind 0-3 coeff_token for nC classes 0-1, 2-3, 4-7, 8+ (i = TotalCoeff,
   j = TrailingOnes), 4 chroma DC coeff_token, 5 total_zeros (i = TotalCoeff,
   j = total_zeros), 6 chroma DC total_zeros, 7 run_before (i = zerosLeft,
   7 = more, j = run_before). */
uint32_t h264enc_code_for_test(int kind, int i, int j, int *len) {
    *len = 0;
    if (kind >= 0 && kind < 4 && i >= 0 && i <= 16 && j >= 0 && j <= 3 && j <= i) {
        *len = tok_len[kind][i * 4 + j];
        return tok_bits[kind][i * 4 + j];
    }
    if (kind == 4 && i >= 0 && i <= 4 && j >= 0 && j <= 3 && j <= i) {
        *len = cdc_tok_len[i * 4 + j];
        return cdc_tok_bits[i * 4 + j];
    }
    if (kind == 5 && i >= 1 && i <= 15 && j >= 0 && j <= 16 - i) {
        *len = tz_len[i - 1][j];
        return tz_bits[i - 1][j];
    }
    if (kind == 6 && i >= 1 && i <= 3 && j >= 0 && j <= 4 - i) {
        *len = ctz_len[i - 1][j];
        return ctz_bits[i - 1][j];
    }
    if (kind == 7 && i >= 1 && i <= 7 && j >= 0 && j <= (i < 7 ? i : 14)) {
        *len = run_len[i - 1][j];
        return run_bits[i - 1][j];
    }
    return 0;
}
