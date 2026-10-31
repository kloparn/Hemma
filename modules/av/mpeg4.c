/* modules/av/mpeg4.c — see mpeg4.h. Clause and table numbers refer to ISO/IEC 14496-2 (MPEG-4 visual). */
#include "mpeg4.h"
#include "idct.h"

#include <stdlib.h>
#include <string.h>

#define PAD 64  /* zero bytes after the input copy: the bit reader looks up to 8 bytes past where it is */
#define NPIC 6  /* frame buffers: 2 references, the picture being decoded, the ones waiting to be taken */
#define QMAX 3  /* frames waiting in display order (the caller takes them after every block) */
#define NPTS 16 /* block times waiting for their frames */
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#define MAX_PIXELS (720 * 576) /* (fuzzers: bigger is right, just slow under the sanitizers) */
#else
#define MAX_PIXELS (4096 * 2304) /* (six of them: ~85 MB) */
#endif

/* ------------------------------------------------------------ bit reader */

typedef struct {
    const uint8_t *buf;
    size_t pos, end; /* in bits */
} Br;

static inline uint64_t be64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return v;
#else
    return __builtin_bswap64(v);
#endif
}

/* The next n bits (1..32) without consuming them. */
static inline uint32_t show(const Br *b, int n) {
    return (uint32_t)((be64(b->buf + (b->pos >> 3)) << (b->pos & 7)) >> (64 - n));
}
static inline void skip(Br *b, int n) { b->pos += (size_t)n; }
static inline uint32_t get(Br *b, int n) {
    if (n <= 0) return 0;
    uint32_t v = show(b, n);
    b->pos += (size_t)n;
    return v;
}
static inline int bit(Br *b) { return (int)get(b, 1); }
static inline bool overrun(const Br *b) { return b->pos > b->end; }

/* ------------------------------------------------------------ VLC tables (Annex B) */

typedef struct {
    int16_t sym;
    uint8_t len; /* 0 = not a valid code */
} Vlc;

typedef struct {
    uint16_t code;
    uint8_t len;
} Code;

/* A lookup by the next `bits` bits; a code's symbol is its place in the list. */
static void vlc_build(Vlc *t, int bits, const Code *c, int n) {
    memset(t, 0, sizeof(Vlc) << bits);
    for (int k = 0; k < n; k++) {
        int shift = bits - c[k].len;
        for (int i = 0; i < 1 << shift; i++) t[(c[k].code << shift) + i] = (Vlc){(int16_t)k, c[k].len};
    }
}

/* B-6 mcbpc for I-VOPs: intra cbpc 0..3, intra+q cbpc 0..3, stuffing */
static const Code MCBPC_I[9] = {{1, 1}, {1, 3}, {2, 3}, {3, 3}, {1, 4}, {1, 6}, {2, 6}, {3, 6}, {1, 9}};
#define MCBPC_I_STUFF 8
/* B-7 mcbpc for P-VOPs, by (4 = intra | 8 = +quant | 16 = four vectors) + cbpc; 20 = stuffing */
static const Code MCBPC_P[21] = {
    {1, 1}, {3, 4}, {2, 4}, {5, 6},  /* inter */
    {3, 5}, {4, 8}, {3, 8}, {3, 7},  /* intra */
    {3, 3}, {7, 7}, {6, 7}, {5, 9},  /* inter + q */
    {4, 6}, {4, 9}, {3, 9}, {2, 9},  /* intra + q */
    {2, 3}, {5, 7}, {4, 7}, {5, 8},  /* inter, four vectors */
    {1, 9},                          /* stuffing */
};
#define MCBPC_P_STUFF 20
/* B-8 cbpy (as for intra macroblocks; inter ones invert it), by its value */
static const Code CBPY[16] = {{3, 4}, {5, 5}, {4, 5}, {9, 4}, {3, 5}, {7, 4}, {2, 6}, {11, 4},
                              {2, 5}, {3, 6}, {5, 4}, {10, 4}, {4, 4}, {8, 4}, {6, 4}, {3, 2}};
/* B-13 / B-14 dct_dc_size_luminance / _chrominance, by size */
static const Code DC_LUM[13] = {{3, 3}, {3, 2}, {2, 2}, {2, 3}, {1, 3}, {1, 4}, {1, 5},
                                {1, 6}, {1, 7}, {1, 8}, {1, 9}, {1, 10}, {1, 11}};
static const Code DC_CHROM[13] = {{3, 2}, {2, 2}, {1, 2}, {1, 3}, {1, 4}, {1, 5}, {1, 6},
                                  {1, 7}, {1, 8}, {1, 9}, {1, 10}, {1, 11}, {1, 12}};
/* B-12 motion code magnitudes 0..32 (a sign bit follows the non-zero ones) */
static const Code MVD[33] = {
    {1, 1},   {1, 2},   {1, 3},   {1, 4},   {3, 6},   {5, 7},   {4, 7},   {3, 7},   {11, 9},  {10, 9},  {9, 9},
    {17, 10}, {16, 10}, {15, 10}, {14, 10}, {13, 10}, {12, 10}, {11, 10}, {10, 10}, {9, 10},  {8, 10},  {7, 10},
    {6, 10},  {5, 10},  {4, 10},  {7, 11},  {6, 11},  {5, 11},  {4, 11},  {3, 11},  {2, 11},  {3, 12},  {2, 12},
};

/* B-16 / B-17 DCT coefficients, without their sign bit: the (last, run, level) entries in the order of
   the LMAX tables below (last 0 first; by run, then level), then the escape. */
#define TC_ESC 102
static const Code TC_INTRA[103] = {
    {0x2, 2},   {0x6, 3},   {0xf, 4},   {0xd, 5},   {0xc, 5},   {0x15, 6},  {0x13, 6},  {0x12, 6},  {0x17, 7},
    {0x1f, 8},  {0x1e, 8},  {0x1d, 8},  {0x25, 9},  {0x24, 9},  {0x23, 9},  {0x21, 9},  {0x21, 10}, {0x20, 10},
    {0xf, 10},  {0xe, 10},  {0x7, 11},  {0x6, 11},  {0x20, 11}, {0x21, 11}, {0x50, 12}, {0x51, 12}, {0x52, 12},
    {0xe, 4},   {0x14, 6},  {0x16, 7},  {0x1c, 8},  {0x20, 9},  {0x1f, 9},  {0xd, 10},  {0x22, 11}, {0x53, 12},
    {0x55, 12}, {0xb, 5},   {0x15, 7},  {0x1e, 9},  {0xc, 10},  {0x56, 12}, {0x11, 6},  {0x1b, 8},  {0x1d, 9},
    {0xb, 10},  {0x10, 6},  {0x22, 9},  {0xa, 10},  {0xd, 6},   {0x1c, 9},  {0x8, 10},  {0x12, 7},  {0x1b, 9},
    {0x54, 12}, {0x14, 7},  {0x1a, 9},  {0x57, 12}, {0x19, 8},  {0x9, 10},  {0x18, 8},  {0x23, 11}, {0x17, 8},
    {0x19, 9},  {0x18, 9},  {0x7, 10},  {0x58, 12}, {0x7, 4},   {0xc, 6},   {0x16, 8},  {0x17, 9},  {0x6, 10},
    {0x5, 11},  {0x4, 11},  {0x59, 12}, {0xf, 6},   {0x16, 9},  {0x5, 10},  {0xe, 6},   {0x4, 10},  {0x11, 7},
    {0x24, 11}, {0x10, 7},  {0x25, 11}, {0x13, 7},  {0x5a, 12}, {0x15, 8},  {0x5b, 12}, {0x14, 8},  {0x13, 8},
    {0x1a, 8},  {0x15, 9},  {0x14, 9},  {0x13, 9},  {0x12, 9},  {0x11, 9},  {0x26, 11}, {0x27, 11}, {0x5c, 12},
    {0x5d, 12}, {0x5e, 12}, {0x5f, 12}, {0x3, 7},
};
static const Code TC_INTER[103] = {
    {0x2, 2},   {0xf, 4},   {0x15, 6},  {0x17, 7},  {0x1f, 8},  {0x25, 9},  {0x24, 9},  {0x21, 10}, {0x20, 10},
    {0x7, 11},  {0x6, 11},  {0x20, 11}, {0x6, 3},   {0x14, 6},  {0x1e, 8},  {0xf, 10},  {0x21, 11}, {0x50, 12},
    {0xe, 4},   {0x1d, 8},  {0xe, 10},  {0x51, 12}, {0xd, 5},   {0x23, 9},  {0xd, 10},  {0xc, 5},   {0x22, 9},
    {0x52, 12}, {0xb, 5},   {0xc, 10},  {0x53, 12}, {0x13, 6},  {0xb, 10},  {0x54, 12}, {0x12, 6},  {0xa, 10},
    {0x11, 6},  {0x9, 10},  {0x10, 6},  {0x8, 10},  {0x16, 7},  {0x55, 12}, {0x15, 7},  {0x14, 7},  {0x1c, 8},
    {0x1b, 8},  {0x21, 9},  {0x20, 9},  {0x1f, 9},  {0x1e, 9},  {0x1d, 9},  {0x1c, 9},  {0x1b, 9},  {0x1a, 9},
    {0x22, 11}, {0x23, 11}, {0x56, 12}, {0x57, 12}, {0x7, 4},   {0x19, 9},  {0x5, 11},  {0xf, 6},   {0x4, 11},
    {0xe, 6},   {0xd, 6},   {0xc, 6},   {0x13, 7},  {0x12, 7},  {0x11, 7},  {0x10, 7},  {0x1a, 8},  {0x19, 8},
    {0x18, 8},  {0x17, 8},  {0x16, 8},  {0x15, 8},  {0x14, 8},  {0x13, 8},  {0x18, 9},  {0x17, 9},  {0x16, 9},
    {0x15, 9},  {0x14, 9},  {0x13, 9},  {0x12, 9},  {0x11, 9},  {0x7, 10},  {0x6, 10},  {0x5, 10},  {0x4, 10},
    {0x24, 11}, {0x25, 11}, {0x26, 11}, {0x27, 11}, {0x58, 12}, {0x59, 12}, {0x5a, 12}, {0x5b, 12}, {0x5c, 12},
    {0x5d, 12}, {0x5e, 12}, {0x5f, 12}, {0x3, 7},
};
/* B-19 / B-20 LMAX: the levels each run has, for last = 0 and 1 (runs past the list have none) */
static const uint8_t LMAX_INTRA0[15] = {27, 10, 5, 4, 3, 3, 3, 3, 2, 2, 1, 1, 1, 1, 1};
static const uint8_t LMAX_INTRA1[21] = {8, 3, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static const uint8_t LMAX_INTER0[27] = {12, 6, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static const uint8_t LMAX_INTER1[41] = {3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

typedef struct {
    Vlc vlc[4096]; /* by the next 12 bits: sym = entry, TC_ESC = escape */
    uint8_t last[102], run[102], level[102];
    uint8_t lmax[2][64]; /* LMAX[last][run] */
    uint8_t rmax[2][64]; /* RMAX[last][level]: the longest run with that level */
} Rl;

static void rl_build(Rl *r, const Code *codes, const uint8_t *l0, int n0, const uint8_t *l1, int n1) {
    vlc_build(r->vlc, 12, codes, 103);
    memset(r->lmax, 0, sizeof r->lmax);
    memset(r->rmax, 0, sizeof r->rmax);
    int k = 0;
    for (int last = 0; last < 2; last++) {
        const uint8_t *lm = last ? l1 : l0;
        int n = last ? n1 : n0;
        for (int run = 0; run < n; run++) {
            r->lmax[last][run] = lm[run];
            for (int level = 1; level <= lm[run] && k < 102; level++, k++) {
                r->last[k] = (uint8_t)last, r->run[k] = (uint8_t)run, r->level[k] = (uint8_t)level;
                if (level < 64) r->rmax[last][level] = (uint8_t)run;
            }
        }
    }
}

/* scan position -> raster index (7.4.3.1): zigzag, alternate vertical; alternate horizontal is its transpose */
static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
static const uint8_t ALT_V[64] = {
    0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,  11,
    4,  12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44,
    52, 60, 37, 45, 53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

/* 6.3.3 default quantiser matrices (raster order) */
static const uint8_t DEF_INTRA[64] = {
    8,  17, 18, 19, 21, 23, 25, 27, 17, 18, 19, 21, 23, 25, 27, 28, 20, 21, 22, 23, 24, 26,
    28, 30, 21, 22, 23, 24, 26, 28, 30, 32, 22, 23, 24, 26, 28, 30, 32, 35, 23, 24, 26, 28,
    30, 32, 35, 38, 25, 26, 28, 30, 32, 35, 38, 41, 27, 28, 30, 32, 35, 38, 41, 45,
};
static const uint8_t DEF_INTER[64] = {
    16, 17, 18, 19, 20, 21, 22, 23, 17, 18, 19, 20, 21, 22, 23, 24, 18, 19, 20, 21, 22, 23,
    24, 25, 19, 20, 21, 22, 23, 24, 26, 27, 20, 21, 22, 23, 25, 26, 27, 28, 21, 22, 23, 24,
    26, 27, 28, 30, 22, 23, 24, 26, 27, 28, 30, 31, 23, 24, 25, 27, 28, 30, 31, 33,
};

/* ------------------------------------------------------------ the decoder */

enum { VOP_I, VOP_P, VOP_B, VOP_S };
enum { MB_INTRA = 1, MB_SKIP = 2, MB_4MV = 4 };
enum { B_DIRECT, B_BI, B_BWD, B_FWD };

typedef struct {
    uint8_t *y, *u, *v;
    char type;
    int concealed;
} Pic;

struct Mpeg4 {
    Vlc mcbpc_i[512], mcbpc_p[512], cbpy[64], dcl[2048], dcc[4096], mvd[4096];
    Rl intra, inter;
    uint8_t alt_h[64];
    /* the VOL */
    bool vol;
    const char *unsup;
    int w, h, mbw, mbh, nmb, sy, sc;
    int res, inc_bits, fixed_inc, par_num, par_den;
    bool qpel, mpeg_quant, resync, gmc0; /* gmc0: S-VOPs are GMC with no warping points (no motion) */
    uint8_t qm[2][64];                   /* MPEG quantiser matrices, intra and inter (raster) */
    int divx, divx_build, xvid;          /* the encoder by its user data, -1 = not said */
    bool direct16, qpel_chroma, qpel_chroma2; /* their bugs FFmpeg follows (see bugs()) */
    /* pictures */
    uint8_t *mem;
    Pic pic[NPIC];
    int ref[2]; /* the older and the newer reference (I/P/S), -1 = none */
    int held;   /* the newer reference while it waits to be shown, -1 = none */
    int queue[QMAX], nq;
    int64_t qpts[QMAX];
    int64_t pts[NPTS]; /* block times, smallest first */
    int npts, debt;    /* debt: frames that came in a block with another (packed) - a later empty block brings their time */
    /* times (6.3.5): in time_increment_resolution units */
    int64_t time_base, last_time_base, last_nonb;
    int64_t pp, pb; /* times between the references, and from the older one to a B-VOP */
    /* the VOP being decoded */
    int type, q, rc, dcthr, fcode, bcode, cur, pkt, fwd, bwd;
    bool err;
    int bmv[2][2]; /* B-VOP vector predictors: forward, backward */
    /* per macroblock / 8x8 block */
    uint8_t *mbf, *colf; /* MB_*: this VOP's, the newer reference's (B-VOPs' direct mode) */
    int8_t *mbq;
    int16_t (*mv)[2], (*colmv)[2]; /* vectors of luma 8x8 blocks: this VOP's, the newer reference's */
    int16_t *dc[3];                /* intra blocks' scaled DC: luma (2mbw x 2mbh), Cb, Cr (mbw x mbh) */
    int16_t (*ac[3])[14];          /* their first row (1..7) and first column (1..7), quantised */
    /* input */
    uint8_t *in;
    size_t cap;
    uint8_t win[17 * 17], pb2[16 * 16 + 2 * 64]; /* a window of a reference; the backward prediction */
};

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline uint8_t u8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

Mpeg4 *mpeg4_new(void) {
    Mpeg4 *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    vlc_build(d->mcbpc_i, 9, MCBPC_I, 9);
    vlc_build(d->mcbpc_p, 9, MCBPC_P, 21);
    vlc_build(d->cbpy, 6, CBPY, 16);
    vlc_build(d->dcl, 11, DC_LUM, 13);
    vlc_build(d->dcc, 12, DC_CHROM, 13);
    vlc_build(d->mvd, 12, MVD, 33);
    rl_build(&d->intra, TC_INTRA, LMAX_INTRA0, 15, LMAX_INTRA1, 21);
    rl_build(&d->inter, TC_INTER, LMAX_INTER0, 27, LMAX_INTER1, 41);
    for (int i = 0; i < 64; i++) d->alt_h[i] = (uint8_t)((ALT_V[i] & 7) * 8 + (ALT_V[i] >> 3));
    d->ref[0] = d->ref[1] = d->held = -1;
    d->divx = d->divx_build = d->xvid = -1;
    return d;
}

static void drop_pictures(Mpeg4 *d) {
    free(d->mem);
    free(d->mbf), free(d->colf), free(d->mbq), free(d->mv), free(d->colmv);
    for (int c = 0; c < 3; c++) free(d->dc[c]), free(d->ac[c]);
    d->mem = NULL, d->mbf = d->colf = NULL, d->mbq = NULL, d->mv = d->colmv = NULL;
    for (int c = 0; c < 3; c++) d->dc[c] = NULL, d->ac[c] = NULL;
    d->ref[0] = d->ref[1] = d->held = -1;
    d->nq = 0;
    d->vol = false;
}

void mpeg4_free(Mpeg4 *d) {
    if (!d) return;
    drop_pictures(d);
    free(d->in);
    free(d);
}

static bool alloc_pictures(Mpeg4 *d, int w, int h) {
    drop_pictures(d);
    d->w = w, d->h = h;
    d->mbw = (w + 15) >> 4, d->mbh = (h + 15) >> 4, d->nmb = d->mbw * d->mbh;
    d->sy = d->mbw * 16, d->sc = d->mbw * 8;
    size_t lum = (size_t)d->sy * (size_t)d->mbh * 16, chr = (size_t)d->sc * (size_t)d->mbh * 8, one = lum + 2 * chr;
    size_t nb = (size_t)d->nmb, n8 = 4 * nb;
    d->mem = malloc(one * NPIC);
    d->mbf = calloc(nb, 1), d->colf = calloc(nb, 1), d->mbq = calloc(nb, 1);
    d->mv = calloc(n8, sizeof *d->mv), d->colmv = calloc(n8, sizeof *d->colmv);
    d->dc[0] = calloc(n8, sizeof *d->dc[0]), d->ac[0] = calloc(n8, sizeof *d->ac[0]);
    for (int c = 1; c < 3; c++) d->dc[c] = calloc(nb, sizeof *d->dc[c]), d->ac[c] = calloc(nb, sizeof *d->ac[c]);
    if (!d->mem || !d->mbf || !d->colf || !d->mbq || !d->mv || !d->colmv || !d->dc[0] || !d->dc[1] || !d->dc[2] ||
        !d->ac[0] || !d->ac[1] || !d->ac[2]) {
        drop_pictures(d);
        return false;
    }
    memset(d->mem, 128, one * NPIC); /* (never read before written; grey keeps tools like valgrind quiet) */
    for (int i = 0; i < NPIC; i++) {
        uint8_t *p = d->mem + one * (size_t)i;
        d->pic[i] = (Pic){p, p + lum, p + lum + chr, 'I', 0};
    }
    d->vol = true;
    return true;
}

/* ------------------------------------------------------------ headers (6.2) */

static void load_matrix(Br *b, uint8_t *m) {
    uint8_t v[64];
    int last = 0, i = 0;
    for (; i < 64; i++) {
        int x = (int)get(b, 8);
        if (!x) break;
        v[i] = (uint8_t)(last = x);
    }
    if (!last) return; /* (an empty matrix: keep the default) */
    for (; i < 64; i++) v[i] = (uint8_t)last;
    for (i = 0; i < 64; i++) m[ZIGZAG[i]] = v[i];
}

/* The encoder's known bugs, as FFmpeg reads them from its user data (mpeg4videodec.c): DivX codes direct mode
   with one vector as 16x16 even with quarter samples, and some DivX 5 and Xvid builds derived chroma vectors
   from quarter-sample ones wrongly. */
static void bugs(Mpeg4 *d) {
    d->direct16 = d->divx >= 0;
    d->qpel_chroma2 = d->divx > 502 && d->divx_build < 1814;
    d->qpel_chroma = (d->divx >= 500 && d->divx_build < 1814) || (d->xvid >= 0 && d->xvid <= 1);
}

static int digits(const char **p) {
    int v = 0, n = 0;
    while (**p >= '0' && **p <= '9' && n < 9) v = v * 10 + (*(*p)++ - '0'), n++;
    return n ? v : -1;
}

static void user_data(Mpeg4 *d, const uint8_t *p, size_t n) {
    char s[64];
    size_t k = n < sizeof s - 1 ? n : sizeof s - 1;
    memcpy(s, p, k);
    s[k] = '\0';
    const char *q = s + 4;
    if (!strncmp(s, "DivX", 4)) { /* "DivX503b1393p", "DivX501Build413" */
        int ver = digits(&q), build = -1;
        if (!strncmp(q, "Build", 5)) q += 5, build = digits(&q);
        else if (*q == 'b') q++, build = digits(&q);
        if (ver >= 0 && build >= 0) d->divx = ver, d->divx_build = build;
    } else if (!strncmp(s, "XviD", 4)) { /* "XviD0065" */
        int build = digits(&q);
        if (build >= 0) d->xvid = build;
    }
    bugs(d);
}

static void vol(Mpeg4 *d, Br *b) {
    const char *unsup = NULL;
    skip(b, 1); /* random_accessible_vol */
    skip(b, 8); /* video_object_type_indication */
    int verid = 1;
    if (bit(b)) verid = (int)get(b, 4), skip(b, 3); /* is_object_layer_identifier */
    int ar = (int)get(b, 4), pn = 1, pd = 1;
    static const int PAR[6][2] = {{1, 1}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}};
    if (ar == 15) pn = (int)get(b, 8), pd = (int)get(b, 8);
    else if (ar >= 1 && ar <= 5) pn = PAR[ar][0], pd = PAR[ar][1];
    if (!pn || !pd) pn = pd = 1;
    if (bit(b)) { /* vol_control_parameters */
        if (get(b, 2) != 1) unsup = "a chroma format other than 4:2:0";
        skip(b, 1);      /* low_delay: frames are always held until the next reference shows them */
        if (bit(b)) skip(b, 79); /* vbv_parameters (with their markers) */
    }
    if (get(b, 2) != 0) { /* video_object_layer_shape */
        d->unsup = "shapes (not a rectangle)";
        return;
    }
    skip(b, 1);
    int res = (int)get(b, 16);
    skip(b, 1);
    if (!res) return; /* damaged */
    int bits = 1;
    while ((1 << bits) < res) bits++;
    int fixed = bit(b) ? (int)get(b, bits) : 0;
    skip(b, 1);
    int w = (int)get(b, 13);
    skip(b, 1);
    int h = (int)get(b, 13);
    skip(b, 1);
    if (bit(b)) unsup = "interlaced coding";
    skip(b, 1); /* obmc_disable (an encoder that says otherwise is buggy: FFmpeg ignores it too) */
    int sprite = verid == 1 ? (int)get(b, 1) : (int)get(b, 2), warps = 0;
    if (sprite == 1 || sprite == 2) {
        if (sprite == 1) skip(b, 56); /* static sprite: its size and place */
        warps = (int)get(b, 6);
        skip(b, 2); /* sprite_warping_accuracy */
        if (bit(b)) unsup = "global motion with brightness changes";
        if (sprite == 1) skip(b, 1), unsup = "static sprites";
        else if (warps) unsup = "global motion compensation (GMC)";
    } else if (sprite == 3) unsup = "sprites";
    if (bit(b)) unsup = "video that isn't 8-bit"; /* not_8_bit */
    bool mq = bit(b);
    uint8_t qm[2][64];
    memcpy(qm[0], DEF_INTRA, 64);
    memcpy(qm[1], DEF_INTER, 64);
    if (mq) {
        if (bit(b)) load_matrix(b, qm[0]);
        if (bit(b)) load_matrix(b, qm[1]);
    }
    bool qpel = verid != 1 && bit(b);
    if (!bit(b)) unsup = "complexity estimation";
    bool resync = !bit(b);
    if (bit(b)) unsup = "data partitioning";
    if (verid != 1) {
        if (bit(b)) unsup = "NEWPRED";
        if (bit(b)) unsup = "reduced resolution VOPs";
    }
    if (bit(b)) unsup = "scalability";
    if (overrun(b) || w <= 0 || h <= 0 || (int64_t)w * h > MAX_PIXELS) return; /* damaged, or too big */
    if (!d->vol || w != d->w || h != d->h)
        if (!alloc_pictures(d, w, h)) return;
    d->unsup = unsup;
    d->res = res, d->inc_bits = bits, d->fixed_inc = fixed;
    d->par_num = pn, d->par_den = pd;
    d->qpel = qpel, d->mpeg_quant = mq, d->resync = resync, d->gmc0 = sprite == 2 && !warps;
    memcpy(d->qm, qm, sizeof qm);
}

/* ------------------------------------------------------------ pictures out, block times */

static void pts_push(Mpeg4 *d, int64_t t) {
    if (t == MPEG4_NOPTS) return;
    if (d->npts == NPTS) d->npts--; /* (frames never came for these: forget the latest) */
    int i = d->npts;
    while (i > 0 && d->pts[i - 1] > t) d->pts[i] = d->pts[i - 1], i--;
    d->pts[i] = t;
    d->npts++;
}

static int64_t pts_pop(Mpeg4 *d) {
    if (!d->npts) return MPEG4_NOPTS;
    int64_t t = d->pts[0];
    memmove(d->pts, d->pts + 1, (size_t)--d->npts * sizeof *d->pts);
    return t;
}

static void show_pic(Mpeg4 *d, int p) {
    if (d->nq == QMAX) { /* the caller didn't take them: the oldest goes */
        memmove(d->queue, d->queue + 1, (QMAX - 1) * sizeof *d->queue);
        memmove(d->qpts, d->qpts + 1, (QMAX - 1) * sizeof *d->qpts);
        d->nq--;
    }
    d->queue[d->nq] = p;
    d->qpts[d->nq] = pts_pop(d);
    d->nq++;
}

static int free_pic(const Mpeg4 *d, int besides) {
    for (int i = 0; i < NPIC; i++) {
        bool used = i == d->ref[0] || i == d->ref[1] || i == d->held || i == besides;
        for (int k = 0; k < d->nq && !used; k++) used = d->queue[k] == i;
        if (!used) return i;
    }
    return -1;
}

/* ------------------------------------------------------------ motion compensation (7.6) */

/* A w x h piece of a plane from (x, y), the plane's edges repeated outwards beyond its pw x ph picture
   (7.6.4, unrestricted motion vectors): the plane itself where it fits, else a copy in tmp. */
static const uint8_t *window(const uint8_t *p, int stride, int pw, int ph, int x, int y, int w, int h, uint8_t *tmp,
                             int *ts) {
    if (x >= 0 && y >= 0 && x + w <= pw && y + h <= ph) {
        *ts = stride;
        return p + (size_t)y * (size_t)stride + x;
    }
    for (int j = 0; j < h; j++) {
        const uint8_t *row = p + (size_t)clampi(y + j, 0, ph - 1) * (size_t)stride;
        for (int i = 0; i < w; i++) tmp[j * w + i] = row[clampi(x + i, 0, pw - 1)];
    }
    *ts = w;
    return tmp;
}

/* half-sample interpolation (7.6.2.1), rounding_control rc */
static void hpel(uint8_t *d, int ds, const uint8_t *s, int ss, int w, int h, int fx, int fy, int rc) {
    for (int j = 0; j < h; j++, d += ds, s += ss) {
        if (!fx && !fy) memcpy(d, s, (size_t)w);
        else if (!fy)
            for (int i = 0; i < w; i++) d[i] = (uint8_t)((s[i] + s[i + 1] + 1 - rc) >> 1);
        else if (!fx)
            for (int i = 0; i < w; i++) d[i] = (uint8_t)((s[i] + s[i + ss] + 1 - rc) >> 1);
        else
            for (int i = 0; i < w; i++) d[i] = (uint8_t)((s[i] + s[i + 1] + s[i + ss] + s[i + ss + 1] + 2 - rc) >> 2);
    }
}

/* Quarter-sample interpolation (7.6.2.2): the half-sample values come from an 8-tap filter over the block's
   own n + 1 samples, mirrored at its edges; quarter ones average a half-sample value with its neighbour.
   Horizontal first (the rows), then vertical over those. */
static inline int mirror(int j, int n) { return j < 0 ? -1 - j : j > n ? 2 * n + 1 - j : j; }

static void lowpass(uint8_t *out, const uint8_t *in, int step, int n, int rc) {
    for (int i = 0; i < n; i++) {
#define P(k) in[mirror(i + (k), n) * step]
        int v = 20 * (P(0) + P(1)) - 6 * (P(-1) + P(2)) + 3 * (P(-2) + P(3)) - (P(-3) + P(4));
#undef P
        out[i] = u8((v + 16 - rc) >> 5);
    }
}

static void qpel(uint8_t *dst, int ds, const uint8_t *s, int ss, int n, int fx, int fy, int rc) {
    uint8_t r[17 * 17], t[17];
    int rows = fy ? n + 1 : n;
    for (int j = 0; j < rows; j++) {
        const uint8_t *row = s + j * ss;
        uint8_t *o = r + j * 17;
        if (!fx) {
            memcpy(o, row, (size_t)n);
            continue;
        }
        lowpass(t, row, 1, n, rc);
        for (int i = 0; i < n; i++)
            o[i] = fx == 2 ? t[i] : (uint8_t)(((fx == 1 ? row[i] + t[i] : t[i] + row[i + 1]) + 1 - rc) >> 1);
    }
    if (!fy) {
        for (int j = 0; j < n; j++) memcpy(dst + j * ds, r + j * 17, (size_t)n);
        return;
    }
    for (int i = 0; i < n; i++) {
        lowpass(t, r + i, 17, n, rc);
        for (int j = 0; j < n; j++) {
            int a = r[j * 17 + i], b = r[(j + 1) * 17 + i];
            dst[j * ds + i] = fy == 2 ? t[j] : (uint8_t)(((fy == 1 ? a : b) + t[j] + 1 - rc) >> 1);
        }
    }
}

/* An n x n luma block at (px, py) predicted from ref with vector (mx, my) in half or quarter samples. */
static void pred_luma(Mpeg4 *d, const Pic *ref, uint8_t *dst, int ds, int px, int py, int n, int mx, int my, int rc) {
    int sh = d->qpel ? 2 : 1, ws;
    int fx = mx & ((1 << sh) - 1), fy = my & ((1 << sh) - 1);
    const uint8_t *s = window(ref->y, d->sy, d->w, d->h, px + (mx >> sh), py + (my >> sh), n + 1, n + 1, d->win, &ws);
    if (d->qpel) qpel(dst, ds, s, ws, n, fx, fy, rc);
    else hpel(dst, ds, s, ws, n, n, fx, fy, rc);
}

/* Both 8x8 chroma blocks of macroblock (mbx, mby) from a chroma vector in half samples. */
static void pred_chroma(Mpeg4 *d, const Pic *ref, uint8_t *du, uint8_t *dv, int ds, int mbx, int mby, int cx, int cy,
                        int rc) {
    int x = mbx * 8 + (cx >> 1), y = mby * 8 + (cy >> 1), ws;
    const uint8_t *s = window(ref->u, d->sc, d->w >> 1, d->h >> 1, x, y, 9, 9, d->win, &ws);
    hpel(du, ds, s, ws, 8, 8, cx & 1, cy & 1, rc);
    s = window(ref->v, d->sc, d->w >> 1, d->h >> 1, x, y, 9, 9, d->win, &ws);
    hpel(dv, ds, s, ws, 8, 8, cx & 1, cy & 1, rc);
}

/* 7.6.3 chroma vectors, in chroma half samples: from one luma vector, or from four (summed, Table 7-9) */
static int chroma1(const Mpeg4 *d, int v) {
    if (!d->qpel) return (v >> 1) | (v & 1);
    if (d->qpel_chroma2) {
        static const int R[8] = {0, 0, 1, 1, 0, 0, 0, 1};
        return (v >> 1) + R[v & 7];
    }
    if (d->qpel_chroma) return (v >> 1) | (v & 1);
    v /= 2; /* (to half samples, towards zero) */
    return (v >> 1) | (v & 1);
}

static int chroma4(const Mpeg4 *d, const int *v) {
    static const uint8_t R[16] = {0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2};
    int s = 0;
    for (int k = 0; k < 4; k++) s += d->qpel ? v[k] / 2 : v[k];
    return R[s & 15] + ((s >> 3) & ~1);
}

/* A macroblock's prediction from ref: one vector (all four alike) or four, each 8x8. */
static void predict(Mpeg4 *d, const Pic *ref, int mbx, int mby, bool four, const int *vx, const int *vy, int rc,
                    uint8_t *y, uint8_t *u, uint8_t *v, int sy, int sc) {
    int px = mbx * 16, py = mby * 16;
    if (!four) {
        pred_luma(d, ref, y, sy, px, py, 16, vx[0], vy[0], rc);
        pred_chroma(d, ref, u, v, sc, mbx, mby, chroma1(d, vx[0]), chroma1(d, vy[0]), rc);
        return;
    }
    for (int k = 0; k < 4; k++)
        pred_luma(d, ref, y + (k >> 1) * 8 * sy + (k & 1) * 8, sy, px + (k & 1) * 8, py + (k >> 1) * 8, 8, vx[k], vy[k], rc);
    pred_chroma(d, ref, u, v, sc, mbx, mby, chroma4(d, vx), chroma4(d, vy), rc);
}

/* ------------------------------------------------------------ blocks (7.4) */

static int dc_scale(int q, bool luma) {
    if (luma) return q < 5 ? 8 : q < 9 ? 2 * q : q < 25 ? q + 8 : 2 * q - 16;
    return q < 5 ? 8 : q < 25 ? (q + 13) / 2 : q - 6;
}

/* (last, run, level) codes into q[] (raster) from scan position start + 1 on; false on a bad code. */
static bool coefs(Br *b, const Rl *rl, const uint8_t *scan, int16_t *q, int start) {
    for (int i = start;;) {
        const Vlc *e = &rl->vlc[show(b, 12)];
        if (!e->len) return false;
        skip(b, e->len);
        int last, run, level;
        if (e->sym != TC_ESC) {
            last = rl->last[e->sym], run = rl->run[e->sym], level = rl->level[e->sym];
            if (bit(b)) level = -level;
        } else {
            int esc = !bit(b) ? 1 : !bit(b) ? 2 : 3; /* "0": level + LMAX, "10": run + RMAX + 1, "11": fixed length */
            if (esc == 3) { /* last, run, marker, a 12-bit level, marker */
                last = bit(b), run = (int)get(b, 6);
                if (!bit(b)) return false;
                level = (int)get(b, 12);
                if (level >= 2048) level -= 4096;
                if (!bit(b) || !level) return false;
            } else {
                e = &rl->vlc[show(b, 12)];
                if (!e->len || e->sym == TC_ESC) return false;
                skip(b, e->len);
                last = rl->last[e->sym], run = rl->run[e->sym], level = rl->level[e->sym];
                if (esc == 1) level += rl->lmax[last][run];
                else run += rl->rmax[last][level] + 1;
                if (bit(b)) level = -level;
            }
        }
        i += run + 1;
        if (i > 63) return false;
        q[scan[i]] = (int16_t)level;
        if (last) return true;
    }
}

static inline int16_t sat(int v) { return (int16_t)(v < -2048 ? -2048 : v > 2047 ? 2047 : v); }

/* 7.4.4 inverse quantisation (in place): H.263's (2|QF| + 1)Q, minus 1 for an even Q, or MPEG's with its matrix
   and mismatch control. An intra block's q[0] is its DC level, times dc_scaler. */
static void dequant(const Mpeg4 *d, int16_t *q, bool intra, int scale) {
    int Q = d->q, i0 = 0, sum = 0;
    if (intra) q[0] = sat(q[0] * scale), sum = q[0], i0 = 1;
    if (!d->mpeg_quant) {
        int mul = 2 * Q, add = (Q - 1) | 1;
        for (int i = i0; i < 64; i++)
            if (q[i]) q[i] = sat(q[i] > 0 ? q[i] * mul + add : q[i] * mul - add);
        return;
    }
    const uint8_t *m = d->qm[intra ? 0 : 1];
    for (int i = i0; i < 64; i++) {
        if (!q[i]) continue;
        int a = q[i] < 0 ? -q[i] : q[i];
        a = intra ? (a * Q * m[i]) >> 3 : ((2 * a + 1) * Q * m[i]) >> 4;
        q[i] = sat(q[i] < 0 ? -a : a);
        sum += q[i];
    }
    if (!(sum & 1)) q[63] ^= 1;
}

static int rows_of(const int16_t *q) {
    int r = 0;
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++)
            if (q[j * 8 + i]) {
                r |= 1 << j;
                break;
            }
    return r;
}

static void put_block(int16_t *q, uint8_t *dst, int ds) {
    idct(q, rows_of(q));
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) dst[j * ds + i] = u8(q[j * 8 + i]);
}

static void add_block(int16_t *q, uint8_t *dst, int ds) {
    idct(q, rows_of(q));
    for (int j = 0; j < 8; j++)
        for (int i = 0; i < 8; i++) dst[j * ds + i] = u8(dst[j * ds + i] + q[j * 8 + i]);
}

/* A neighbouring block for DC/AC prediction (7.4.3): its place in the plane's grid, -1 when it isn't there
   to predict from - outside the VOP, in another video packet, or not intra. *mbn = its macroblock. */
static int nb_block(const Mpeg4 *d, bool luma, int gx, int gy, int mb, int *mbn) {
    if (gx < 0 || gy < 0) return -1;
    int m = luma ? (gy >> 1) * d->mbw + (gx >> 1) : gy * d->mbw + gx;
    if (m != mb && (m < d->pkt || !(d->mbf[m] & MB_INTRA))) return -1;
    *mbn = m;
    return gy * (luma ? 2 * d->mbw : d->mbw) + gx;
}

static int rdiv(int a, int b) { return (a > 0 ? a + (b >> 1) : a - (b >> 1)) / b; }

/* An intra block n (0..3 luma, 4 Cb, 5 Cr) of macroblock mb: its DC (by the DC VLCs, or as the first
   coefficient), the rest when coded, DC and AC prediction from the left or the top (7.4.3), out to dst. */
static bool intra_block(Mpeg4 *d, Br *b, int mb, int mbx, int mby, int n, bool coded, bool acpred, bool dcvlc,
                        uint8_t *dst, int ds) {
    bool luma = n < 4;
    int c = luma ? 0 : n - 3, gw = luma ? 2 * d->mbw : d->mbw;
    int gx = luma ? 2 * mbx + (n & 1) : mbx, gy = luma ? 2 * mby + (n >> 1) : mby, gi = gy * gw + gx;
    int ma = mb, mbb = mb, mc = mb;
    int ia = nb_block(d, luma, gx - 1, gy, mb, &ma), ib = nb_block(d, luma, gx - 1, gy - 1, mb, &mbb),
        ic = nb_block(d, luma, gx, gy - 1, mb, &mc);
    int a = ia >= 0 ? d->dc[c][ia] : 1024, bb = ib >= 0 ? d->dc[c][ib] : 1024, cc = ic >= 0 ? d->dc[c][ic] : 1024;
    bool top = abs(a - bb) < abs(bb - cc); /* the gradient says which neighbour to follow */
    int scale = dc_scale(d->q, luma), pred = ((top ? cc : a) + (scale >> 1)) / scale;
    int16_t q[64];
    memset(q, 0, sizeof q);
    int start = -1;
    if (dcvlc) {
        const Vlc *e = luma ? &d->dcl[show(b, 11)] : &d->dcc[show(b, 12)];
        if (!e->len) return false;
        skip(b, e->len);
        int size = e->sym, diff = 0;
        if (size) {
            int v = (int)get(b, size);
            diff = v >> (size - 1) ? v : v - (1 << size) + 1;
            if (size > 8) skip(b, 1); /* marker */
        }
        q[0] = (int16_t)diff;
        start = 0;
    }
    const uint8_t *scan = !acpred ? ZIGZAG : top ? d->alt_h : ALT_V;
    if (coded && !coefs(b, &d->intra, scan, q, start)) return false;
    q[0] = (int16_t)(q[0] + pred);
    if (acpred) { /* the top neighbour's first row, or the left one's first column, at this quantiser */
        int in = top ? ic : ia, qn = d->mbq[top ? mc : ma];
        if (in >= 0)
            for (int i = 1; i < 8; i++) {
                int v = top ? d->ac[c][in][i - 1] : d->ac[c][in][6 + i];
                if (qn != d->q) v = rdiv(v * qn, d->q);
                q[top ? i : i * 8] = (int16_t)(q[top ? i : i * 8] + v);
            }
    }
    int16_t *keep = d->ac[c][gi];
    for (int i = 1; i < 8; i++) keep[i - 1] = q[i], keep[6 + i] = q[i * 8];
    d->dc[c][gi] = (int16_t)clampi(q[0] * scale, 0, 2047);
    dequant(d, q, true, scale);
    put_block(q, dst, ds);
    return true;
}

static bool inter_block(Mpeg4 *d, Br *b, uint8_t *dst, int ds) {
    int16_t q[64];
    memset(q, 0, sizeof q);
    if (!coefs(b, &d->inter, ZIGZAG, q, -1)) return false;
    dequant(d, q, false, 0);
    add_block(q, dst, ds);
    return true;
}

/* ------------------------------------------------------------ motion vectors (7.6.5) */

static inline int b8(const Mpeg4 *d, int mbx, int mby, int k) {
    return (2 * mby + (k >> 1)) * 2 * d->mbw + 2 * mbx + (k & 1);
}

/* The median of block k's left, above and above-right neighbours: one that is outside the VOP or in another
   video packet counts as 0, two such give the third, three give 0. */
static void pred_mv(const Mpeg4 *d, int mbx, int mby, int k, int *px, int *py) {
    static const int OFF[4] = {2, 1, 1, -1};
    int gw = 2 * d->mbw, gh = 2 * d->mbh, bx = 2 * mbx + (k & 1), by = 2 * mby + (k >> 1), mb = mby * d->mbw + mbx;
    int cx[3] = {bx - 1, bx, bx + OFF[k]}, cy[3] = {by, by - 1, by - 1}, vx[3], vy[3], ok = 0, which = 0;
    for (int t = 0; t < 3; t++) {
        int x = cx[t], y = cy[t];
        bool in = x >= 0 && y >= 0 && x < gw && y < gh;
        if (in) {
            int m = (y >> 1) * d->mbw + (x >> 1);
            in = m == mb || m >= d->pkt;
        }
        vx[t] = in ? d->mv[y * gw + x][0] : 0;
        vy[t] = in ? d->mv[y * gw + x][1] : 0;
        if (in) ok++, which = t;
    }
    if (ok == 1) {
        *px = vx[which], *py = vy[which];
        return;
    }
#define MID(a, b, c) ((a) > (b) ? ((b) > (c) ? (b) : (a) > (c) ? (c) : (a)) : ((a) > (c) ? (a) : (b) > (c) ? (c) : (b)))
    *px = MID(vx[0], vx[1], vx[2]);
    *py = MID(vy[0], vy[1], vy[2]);
#undef MID
}

/* A vector component: motion_code (+ motion_residual for fcode > 1) added to pred, wrapped into its range. */
static int mvd(Mpeg4 *d, Br *b, int pred, int fcode) {
    const Vlc *e = &d->mvd[show(b, 12)];
    if (!e->len) {
        d->err = true;
        return pred;
    }
    skip(b, e->len);
    int val = e->sym;
    if (!val) return pred;
    bool neg = bit(b);
    int shift = fcode - 1;
    if (shift) val = (((val - 1) << shift) | (int)get(b, shift)) + 1;
    if (neg) val = -val;
    int low = -(32 << shift);
    return (int)((unsigned)(val + pred - low) & ((64u << shift) - 1)) + low;
}

/* ------------------------------------------------------------ macroblocks (6.2.6) */

static void set_mvs(Mpeg4 *d, int mbx, int mby, const int *vx, const int *vy) {
    for (int k = 0; k < 4; k++) {
        int g = b8(d, mbx, mby, k);
        d->mv[g][0] = (int16_t)vx[k], d->mv[g][1] = (int16_t)vy[k];
    }
}

/* An I-, P- or S-VOP macroblock. */
static bool mb_ip(Mpeg4 *d, Br *b, int mb) {
    int mbx = mb % d->mbw, mby = mb / d->mbw, sy = d->sy, sc = d->sc, k;
    Pic *p = &d->pic[d->cur];
    uint8_t *y = p->y + (size_t)mby * 16 * sy + mbx * 16, *u = p->u + (size_t)mby * 8 * sc + mbx * 8,
            *v = p->v + (size_t)mby * 8 * sc + mbx * 8;
    int vx[4] = {0}, vy[4] = {0};
    bool intra, quant, four = false, mcsel = false;
    if (d->type == VOP_I) {
        do {
            const Vlc *e = &d->mcbpc_i[show(b, 9)];
            if (!e->len) return false;
            skip(b, e->len);
            k = e->sym;
        } while (k == MCBPC_I_STUFF && !overrun(b));
        intra = true, quant = k >= 4, k &= 3;
    } else {
        for (;;) {
            if (overrun(b)) return false;
            if (bit(b)) { /* not_coded: the reference's macroblock as it is (an S-VOP's: global motion, none) */
                d->mbf[mb] = d->type == VOP_S ? 0 : MB_SKIP;
                d->mbq[mb] = (int8_t)d->q;
                set_mvs(d, mbx, mby, vx, vy);
                predict(d, &d->pic[d->fwd], mbx, mby, false, vx, vy, d->rc, y, u, v, sy, sc);
                return true;
            }
            const Vlc *e = &d->mcbpc_p[show(b, 9)];
            if (!e->len) return false;
            skip(b, e->len);
            k = e->sym;
            if (k != MCBPC_P_STUFF) break;
        }
        intra = k & 4, quant = k & 8, four = k & 16, k &= 3;
        if (!intra && !four && d->type == VOP_S) mcsel = bit(b);
    }
    bool acpred = intra && bit(b);
    const Vlc *e = &d->cbpy[show(b, 6)];
    if (!e->len) return false;
    skip(b, e->len);
    int cbp = ((intra ? e->sym : e->sym ^ 15) << 2) | k; /* bit 5 = block 0 ... bit 0 = block 5 */
    int running = d->q;                                  /* intra_dc_vlc_thr compares the QP before dquant */
    if (quant) {
        static const int DQ[4] = {-1, -2, 1, 2};
        d->q = clampi(d->q + DQ[get(b, 2)], 1, 31);
    }
    d->mbq[mb] = (int8_t)d->q;
    if (intra) {
        static const int THR[8] = {32, 13, 15, 17, 19, 21, 23, 0};
        bool dcvlc = running < THR[d->dcthr];
        d->mbf[mb] = MB_INTRA;
        set_mvs(d, mbx, mby, vx, vy);
        for (int n = 0; n < 6; n++) {
            uint8_t *dst = n < 4 ? y + (n >> 1) * 8 * sy + (n & 1) * 8 : n == 4 ? u : v;
            if (!intra_block(d, b, mb, mbx, mby, n, cbp & (32 >> n), acpred, dcvlc, dst, n < 4 ? sy : sc)) return false;
        }
        return true;
    }
    d->mbf[mb] = four ? MB_4MV : 0;
    if (!mcsel)
        for (int i = 0; i < (four ? 4 : 1); i++) {
            int px, py;
            pred_mv(d, mbx, mby, i, &px, &py);
            vx[i] = mvd(d, b, px, d->fcode);
            vy[i] = mvd(d, b, py, d->fcode);
            if (four) { /* (the next block predicts from this one) */
                int g = b8(d, mbx, mby, i);
                d->mv[g][0] = (int16_t)vx[i], d->mv[g][1] = (int16_t)vy[i];
            }
        }
    if (d->err) return false;
    if (!four) vx[1] = vx[2] = vx[3] = vx[0], vy[1] = vy[2] = vy[3] = vy[0];
    set_mvs(d, mbx, mby, vx, vy);
    predict(d, &d->pic[d->fwd], mbx, mby, four, vx, vy, d->rc, y, u, v, sy, sc);
    for (int n = 0; n < 6; n++)
        if (cbp & (32 >> n) && !inter_block(d, b, n < 4 ? y + (n >> 1) * 8 * sy + (n & 1) * 8 : n == 4 ? u : v,
                                            n < 4 ? sy : sc))
            return false;
    return true;
}

static void average(uint8_t *dst, int ds, const uint8_t *s, int ss, int n) {
    for (int j = 0; j < n; j++, dst += ds, s += ss)
        for (int i = 0; i < n; i++) dst[i] = (uint8_t)((dst[i] + s[i] + 1) >> 1);
}

/* A B-VOP macroblock: forward from the older reference, backward from the newer, both (averaged), or direct -
   both, from the newer reference's co-located vectors scaled by the times (7.6.9.5). */
static bool mb_b(Mpeg4 *d, Br *b, int mb) {
    int mbx = mb % d->mbw, mby = mb / d->mbw, sy = d->sy, sc = d->sc;
    Pic *p = &d->pic[d->cur];
    uint8_t *y = p->y + (size_t)mby * 16 * sy + mbx * 16, *u = p->u + (size_t)mby * 8 * sc + mbx * 8,
            *v = p->v + (size_t)mby * 8 * sc + mbx * 8;
    const Pic *f = &d->pic[d->fwd], *bk = &d->pic[d->bwd];
    int fx[4] = {0}, fy[4] = {0}, bx[4] = {0}, by[4] = {0};
    if (mbx == 0) memset(d->bmv, 0, sizeof d->bmv);
    if (d->colf[mb] & MB_SKIP) { /* skipped in the newer reference: skipped here too, the older one's as it is */
        predict(d, f, mbx, mby, false, fx, fy, 0, y, u, v, sy, sc);
        return true;
    }
    int type = B_DIRECT, cbp = 0;
    bool modb1 = bit(b), four = false;
    if (!modb1) {
        bool modb2 = bit(b);
        type = 0;
        while (type < 4 && !bit(b)) type++; /* "1" direct, "01" both, "001" backward, "0001" forward */
        if (type == 4) return false;
        if (!modb2) cbp = (int)get(b, 6);
        if (type != B_DIRECT && cbp && bit(b)) d->q = clampi(d->q + (bit(b) ? 2 : -2), 1, 31);
    }
    bool use_f = type != B_BWD, use_b = type != B_FWD;
    if (type == B_DIRECT) {
        int dx = modb1 ? 0 : mvd(d, b, 0, 1), dy = modb1 ? 0 : mvd(d, b, 0, 1);
        int64_t pp = d->pp, pb = d->pb;
        if (pp <= 0 || pb <= 0 || pb >= pp) pp = 2, pb = 1; /* (times out of order: halfway) */
        four = d->colf[mb] & MB_4MV;
        for (int i = 0; i < 4; i++) {
            int g = b8(d, mbx, mby, four ? i : 0), cx = d->colmv[g][0], cy = d->colmv[g][1];
            fx[i] = (int)(cx * pb / pp) + dx, fy[i] = (int)(cy * pb / pp) + dy;
            bx[i] = dx ? fx[i] - cx : (int)(cx * (pb - pp) / pp);
            by[i] = dy ? fy[i] - cy : (int)(cy * (pb - pp) / pp);
        }
        four = four || (d->qpel && !d->direct16); /* (quarter samples: 8x8 blocks even with one vector) */
    } else {
        if (use_f) {
            fx[0] = d->bmv[0][0] = mvd(d, b, d->bmv[0][0], d->fcode);
            fy[0] = d->bmv[0][1] = mvd(d, b, d->bmv[0][1], d->fcode);
        }
        if (use_b) {
            bx[0] = d->bmv[1][0] = mvd(d, b, d->bmv[1][0], d->bcode);
            by[0] = d->bmv[1][1] = mvd(d, b, d->bmv[1][1], d->bcode);
        }
    }
    if (d->err) return false;
    if (use_f) predict(d, f, mbx, mby, four, fx, fy, 0, y, u, v, sy, sc);
    if (use_b && !use_f) predict(d, bk, mbx, mby, four, bx, by, 0, y, u, v, sy, sc);
    if (use_b && use_f) {
        uint8_t *t = d->pb2;
        predict(d, bk, mbx, mby, four, bx, by, 0, t, t + 256, t + 256 + 64, 16, 8);
        average(y, sy, t, 16, 16);
        average(u, sc, t + 256, 8, 8);
        average(v, sc, t + 256 + 64, 8, 8);
    }
    for (int n = 0; n < 6; n++)
        if (cbp & (32 >> n) && !inter_block(d, b, n < 4 ? y + (n >> 1) * 8 * sy + (n & 1) * 8 : n == 4 ? u : v,
                                            n < 4 ? sy : sc))
            return false;
    return true;
}

/* ------------------------------------------------------------ video packets, concealment */

/* Macroblocks from..to-1 that couldn't be decoded: the reference's (no motion), else grey. */
static void conceal(Mpeg4 *d, int from, int to) {
    Pic *p = &d->pic[d->cur];
    const Pic *r = d->fwd >= 0 ? &d->pic[d->fwd] : NULL;
    for (int mb = from; mb < to; mb++) {
        int mbx = mb % d->mbw, mby = mb / d->mbw, z[4] = {0};
        if (d->type != VOP_B) {
            d->mbf[mb] = 0;
            d->mbq[mb] = (int8_t)d->q;
            set_mvs(d, mbx, mby, z, z);
        }
        uint8_t *y = p->y + (size_t)mby * 16 * d->sy + mbx * 16, *u = p->u + (size_t)mby * 8 * d->sc + mbx * 8,
                *v = p->v + (size_t)mby * 8 * d->sc + mbx * 8;
        if (r) predict(d, r, mbx, mby, false, z, z, 0, y, u, v, d->sy, d->sc);
        else {
            for (int j = 0; j < 16; j++) memset(y + j * d->sy, 128, 16);
            for (int j = 0; j < 8; j++) memset(u + j * d->sc, 128, 8), memset(v + j * d->sc, 128, 8);
        }
        p->concealed++;
    }
}

/* The resync marker's zeros (before its 1) for this VOP (6.3.5.2) */
static int prefix_len(const Mpeg4 *d) {
    if (d->type == VOP_I) return 16;
    if (d->type != VOP_B) return 15 + d->fcode;
    int m = d->fcode > d->bcode ? d->fcode : d->bcode;
    return 15 + (m > 2 ? m : 2);
}

/* At a macroblock's start: stuffing to the byte (a 0, then 1s) and a resync marker? */
static bool at_resync(const Mpeg4 *d, const Br *b) {
    int n = 8 - (int)(b->pos & 7);
    if (b->pos + (size_t)n + 24 > b->end || show(b, n) != (1u << (n - 1)) - 1) return false;
    Br t = *b;
    skip(&t, n);
    int len = 0;
    while (len < 32 && !bit(&t)) len++;
    return len == prefix_len(d);
}

/* 6.2.5.2 video_packet_header from its resync marker (byte aligned): the macroblock it starts at, -1 if that
   makes no sense. Its quantiser becomes the running one. */
static int packet(Mpeg4 *d, Br *b) {
    int len = 0;
    while (len < 32 && !bit(b)) len++;
    if (len != prefix_len(d)) return -1;
    int bits = 1;
    while ((1 << bits) < d->nmb) bits++;
    int mb = (int)get(b, bits), q = (int)get(b, 5);
    if (bit(b)) { /* header_extension_code: the VOP header's times and codes again */
        for (int guard = 0; bit(b) && guard < 64; guard++) {}
        skip(b, 1 + d->inc_bits + 1 + 2 + 3);
        if (d->type != VOP_I) skip(b, 3);
        if (d->type == VOP_B) skip(b, 3);
    }
    if (overrun(b) || mb <= 0 || mb >= d->nmb) return -1;
    if (q) d->q = q;
    return mb;
}

/* After damage: the next video packet that starts past macroblock `after`, b left at its first macroblock. */
static int resync_search(Mpeg4 *d, Br *b, int after) {
    if (!d->resync) return -1;
    for (size_t byte = (b->pos >> 3) + 1; byte * 8 + 40 <= b->end; byte++) {
        if (b->buf[byte] || b->buf[byte + 1]) continue; /* (a marker starts with at least 16 zeros) */
        Br t = {b->buf, byte * 8, b->end};
        int save = d->q, mb = packet(d, &t);
        if (mb > after) {
            *b = t;
            return mb;
        }
        d->q = save;
    }
    return -1;
}

static void decode_mbs(Mpeg4 *d, Br *b) {
    d->pkt = 0;
    memset(d->bmv, 0, sizeof d->bmv);
    for (int mb = 0, fresh = 1; mb < d->nmb;) {
        if (!fresh && d->resync && at_resync(d, b)) {
            Br t = *b;
            skip(&t, 8 - (int)(t.pos & 7));
            int save = d->q, next = packet(d, &t);
            /* a packet that starts further on: the macroblocks before it take no bits (B-VOPs' where the newer
               reference skipped) - decoded as such; one that should take bits is lost, and patched up */
            bool ahead = next > mb && d->type == VOP_B && d->colf[mb] & MB_SKIP;
            if (next >= mb && !ahead) {
                conceal(d, mb, next);
                mb = d->pkt = next, *b = t;
                memset(d->bmv, 0, sizeof d->bmv);
            } else d->q = save;
        }
        fresh = 0;
        d->err = false;
        if ((d->type == VOP_B ? mb_b(d, b, mb) : mb_ip(d, b, mb)) && !overrun(b)) {
            mb++;
            continue;
        }
        int next = resync_search(d, b, mb);
        conceal(d, mb, next < 0 ? d->nmb : next);
        if (next < 0) return;
        mb = d->pkt = next, fresh = 1;
        memset(d->bmv, 0, sizeof d->bmv);
    }
}

/* ------------------------------------------------------------ VOPs (6.2.5) */

/* The header: 1 = a coded VOP to decode, 0 = not coded, -1 = damaged or using what we don't decode.
   The times move either way (as FFmpeg's do). */
static int vop_header(Mpeg4 *d, Br *b) {
    if (!d->vol) return -1;
    int type = (int)get(b, 2), incr = 0;
    while (bit(b))
        if (++incr > 63 || overrun(b)) return -1;
    skip(b, 1);
    int64_t inc = get(b, d->inc_bits);
    skip(b, 1);
    if (type != VOP_B) {
        d->last_time_base = d->time_base;
        d->time_base += incr;
        int64_t t = d->time_base * d->res + inc;
        d->pp = t - d->last_nonb;
        d->last_nonb = t;
    } else {
        int64_t t = (d->last_time_base + incr) * d->res + inc;
        d->pb = d->pp - (d->last_nonb - t);
    }
    if (!bit(b)) return 0; /* vop_coded */
    if (d->unsup || (type == VOP_S && !d->gmc0)) return -1;
    d->type = type;
    d->rc = type == VOP_P || type == VOP_S ? bit(b) : 0;
    d->dcthr = (int)get(b, 3);
    d->q = (int)get(b, 5);
    d->fcode = type != VOP_I ? (int)get(b, 3) : 1;
    d->bcode = type == VOP_B ? (int)get(b, 3) : 1;
    return overrun(b) || !d->q || !d->fcode || !d->bcode ? -1 : 1;
}

static void vop_decode(Mpeg4 *d, Br *b) {
    if (d->type == VOP_B) {
        if (d->ref[0] < 0 || d->ref[1] < 0) { /* its references are gone (a seek): dropped, its time too */
            pts_pop(d);
            return;
        }
        d->fwd = d->ref[0], d->bwd = d->ref[1];
    } else d->fwd = d->ref[1], d->bwd = -1;
    d->cur = free_pic(d, -1);
    if (d->cur < 0) {
        pts_pop(d);
        return;
    }
    if (d->type != VOP_I && d->fwd < 0) { /* a P-VOP first: predicted from grey */
        int g = free_pic(d, d->cur);
        if (g < 0) {
            pts_pop(d);
            return;
        }
        Pic *gp = &d->pic[g];
        memset(gp->y, 128, (size_t)d->sy * (size_t)d->mbh * 16);
        memset(gp->u, 128, (size_t)d->sc * (size_t)d->mbh * 8);
        memset(gp->v, 128, (size_t)d->sc * (size_t)d->mbh * 8);
        d->fwd = g;
    }
    Pic *p = &d->pic[d->cur];
    p->type = "IPBS"[d->type];
    p->concealed = 0;
    decode_mbs(d, b);
    if (d->type == VOP_B) {
        show_pic(d, d->cur);
        return;
    }
    if (d->held >= 0) show_pic(d, d->held);
    d->held = d->cur;
    d->ref[0] = d->ref[1], d->ref[1] = d->cur;
    int16_t(*t)[2] = d->colmv; /* this VOP's vectors and kinds become the next B-VOPs' co-located ones */
    d->colmv = d->mv, d->mv = t;
    uint8_t *f = d->colf;
    d->colf = d->mbf, d->mbf = f;
}

static void gov(Mpeg4 *d, Br *b) {
    int hh = (int)get(b, 5), mm = (int)get(b, 6);
    skip(b, 1);
    int ss = (int)get(b, 6);
    if (!overrun(b)) d->time_base = ss + 60 * (mm + 60 * hh);
}

/* the byte after the next 00 00 01 at or past `from`, or n */
static size_t next_start(const uint8_t *p, size_t n, size_t from) {
    for (size_t i = from; i + 2 < n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i + 3;
    return n;
}

int mpeg4_decode(Mpeg4 *d, const uint8_t *data, size_t n, int64_t pts) {
    if (n + PAD > d->cap) {
        uint8_t *in = realloc(d->in, n + PAD);
        if (!in) return -1;
        d->in = in, d->cap = n + PAD;
    }
    if (n) memcpy(d->in, data, n);
    memset(d->in + n, 0, PAD);
    int coded = 0;
    bool empty = false;
    for (size_t i = next_start(d->in, n, 0); i < n;) {
        size_t next = next_start(d->in, n, i + 1), end = next < n ? next - 3 : n;
        uint8_t code = d->in[i];
        Br b = {d->in, (i + 1) * 8, end * 8};
        if (code >= 0x20 && code <= 0x2F) vol(d, &b);
        else if (code == 0xB2) user_data(d, d->in + i + 1, end - i - 1);
        else if (code == 0xB3) gov(d, &b);
        else if (code == 0xB6) {
            int r = vop_header(d, &b);
            if (r <= 0) empty = true;
            else {
                if (!coded++) pts_push(d, pts); /* a block's time is its first VOP's */
                else d->debt++;                 /* (packed: the next empty block brings this one's) */
                vop_decode(d, &b);
            }
        }
        i = next;
    }
    if (!coded && empty && d->debt > 0) pts_push(d, pts), d->debt--;
    return d->nq;
}

bool mpeg4_frame(Mpeg4 *d, Mpeg4Frame *out) {
    if (!d->nq) return false;
    const Pic *p = &d->pic[d->queue[0]];
    *out = (Mpeg4Frame){p->y, p->u, p->v, d->sy, d->sc, d->w, d->h, d->par_num, d->par_den,
                        d->fixed_inc ? d->res : 0, d->fixed_inc, p->type, d->qpts[0], p->concealed};
    d->nq--;
    memmove(d->queue, d->queue + 1, (size_t)d->nq * sizeof *d->queue);
    memmove(d->qpts, d->qpts + 1, (size_t)d->nq * sizeof *d->qpts);
    return true;
}

void mpeg4_flush(Mpeg4 *d) {
    if (d->held >= 0) show_pic(d, d->held);
    d->held = -1;
}

void mpeg4_reset(Mpeg4 *d) {
    d->ref[0] = d->ref[1] = d->held = -1;
    d->nq = d->npts = d->debt = 0;
    d->pp = d->pb = 0;
}

bool mpeg4_info(const Mpeg4 *d, int *width, int *height) {
    if (!d->vol) return false;
    if (width) *width = d->w;
    if (height) *height = d->h;
    return true;
}

const char *mpeg4_unsupported(const Mpeg4 *d) { return d->unsup; }

/* For the tests: is every code table prefix-free? *bad names the first that isn't. */
static bool prefix_free(const Code *c, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (i == j || c[i].len > c[j].len) continue;
            if (c[j].code >> (c[j].len - c[i].len) == c[i].code) return false;
        }
    return true;
}
bool mpeg4_tables_ok_for_test(const char **bad);
bool mpeg4_tables_ok_for_test(const char **bad) {
    static const struct {
        const char *name;
        const Code *c;
        int n;
    } T[] = {{"mcbpc I", MCBPC_I, 9}, {"mcbpc P", MCBPC_P, 21}, {"cbpy", CBPY, 16},          {"dc lum", DC_LUM, 13},
             {"dc chrom", DC_CHROM, 13}, {"mvd", MVD, 33},      {"intra", TC_INTRA, 103}, {"inter", TC_INTER, 103}};
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (!prefix_free(T[i].c, T[i].n)) {
            *bad = T[i].name;
            return false;
        }
    return true;
}
