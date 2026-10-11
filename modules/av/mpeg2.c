/* modules/av/mpeg2.c — see mpeg2.h. Clause and table numbers refer to
 * ISO/IEC 13818-2 (MPEG-2 video); MPEG-1 differences follow ISO/IEC 11172-2. */
#include "mpeg2.h"

#include <stdlib.h>
#include <string.h>

#define PAD 1024 /* zero bytes after the input copy: the bit reader may look past the end */
#define NPIC 8   /* frame buffers: 2 references + the current picture + queued/held frames */
#define QMAX 4   /* frames waiting in display order */

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
    uint32_t v = show(b, n);
    b->pos += (size_t)n;
    return v;
}
static inline int bit(Br *b) { return (int)get(b, 1); }
static inline bool overrun(const Br *b) { return b->pos > b->end; }

/* ------------------------------------------------------------ VLC tables */

typedef struct {
    int16_t sym;
    uint8_t len; /* 0 = not a valid code */
} Vlc;

typedef struct {
    uint16_t code;
    uint8_t len;
    int16_t sym;
} Code;

static void vlc_build(Vlc *t, int bits, const Code *c, int n) {
    memset(t, 0, sizeof(Vlc) << bits);
    for (int k = 0; k < n; k++) {
        int shift = bits - c[k].len;
        for (int i = 0; i < 1 << shift; i++) t[(c[k].code << shift) + i] = (Vlc){c[k].sym, c[k].len};
    }
}

enum { MB_QUANT = 1, MB_FWD = 2, MB_BWD = 4, MB_PAT = 8, MB_INTRA = 16 };
enum { MBA_ESCAPE = 34, MBA_STUFFING = 35 };
/* motion types: frame pictures 1 field, 2 frame, 3 dual prime; field pictures 1 field, 2 16x8, 3 dual prime */
enum { MT_FIELD = 1, MT_FRAME = 2, MT_16X8 = 2, MT_DMV = 3 };

/* B-1 macroblock_address_increment */
static const Code MBA_CODES[] = {
    {0x1, 1, 1},           {0x3, 3, 2},           {0x2, 3, 3},           {0x3, 4, 4},   {0x2, 4, 5},
    {0x3, 5, 6},           {0x2, 5, 7},           {0x7, 7, 8},           {0x6, 7, 9},   {0xb, 8, 10},
    {0xa, 8, 11},          {0x9, 8, 12},          {0x8, 8, 13},          {0x7, 8, 14},  {0x6, 8, 15},
    {0x17, 10, 16},        {0x16, 10, 17},        {0x15, 10, 18},        {0x14, 10, 19}, {0x13, 10, 20},
    {0x12, 10, 21},        {0x23, 11, 22},        {0x22, 11, 23},        {0x21, 11, 24}, {0x20, 11, 25},
    {0x1f, 11, 26},        {0x1e, 11, 27},        {0x1d, 11, 28},        {0x1c, 11, 29}, {0x1b, 11, 30},
    {0x1a, 11, 31},        {0x19, 11, 32},        {0x18, 11, 33},        {0x08, 11, MBA_ESCAPE},
    {0x0f, 11, MBA_STUFFING},
};

/* B-2, B-3, B-4 macroblock_type for I, P and B pictures */
static const Code MBT_I[] = {{1, 1, MB_INTRA}, {1, 2, MB_QUANT | MB_INTRA}};
static const Code MBT_P[] = {
    {1, 1, MB_FWD | MB_PAT},       {1, 2, MB_PAT},          {1, 3, MB_FWD},           {3, 5, MB_INTRA},
    {2, 5, MB_QUANT | MB_FWD | MB_PAT}, {1, 5, MB_QUANT | MB_PAT}, {1, 6, MB_QUANT | MB_INTRA},
};
static const Code MBT_B[] = {
    {2, 2, MB_FWD | MB_BWD},          {3, 2, MB_FWD | MB_BWD | MB_PAT}, {2, 3, MB_BWD},
    {3, 3, MB_BWD | MB_PAT},          {2, 4, MB_FWD},                   {3, 4, MB_FWD | MB_PAT},
    {3, 5, MB_INTRA},                 {2, 5, MB_QUANT | MB_FWD | MB_BWD | MB_PAT},
    {3, 6, MB_QUANT | MB_FWD | MB_PAT}, {2, 6, MB_QUANT | MB_BWD | MB_PAT}, {1, 6, MB_QUANT | MB_INTRA},
};

/* B-9 coded_block_pattern: {code, length} for each pattern 0..63 */
static const uint8_t CBP_CODES[64][2] = {
    {0x01, 9}, {0x0b, 5}, {0x09, 5}, {0x0d, 6}, {0x0d, 4}, {0x17, 7}, {0x13, 7}, {0x1f, 8}, {0x0c, 4}, {0x16, 7},
    {0x12, 7}, {0x1e, 8}, {0x13, 5}, {0x1b, 8}, {0x17, 8}, {0x13, 8}, {0x0b, 4}, {0x15, 7}, {0x11, 7}, {0x1d, 8},
    {0x11, 5}, {0x19, 8}, {0x15, 8}, {0x11, 8}, {0x0f, 6}, {0x0f, 8}, {0x0d, 8}, {0x03, 9}, {0x0f, 5}, {0x0b, 8},
    {0x07, 8}, {0x07, 9}, {0x0a, 4}, {0x14, 7}, {0x10, 7}, {0x1c, 8}, {0x0e, 6}, {0x0e, 8}, {0x0c, 8}, {0x02, 9},
    {0x10, 5}, {0x18, 8}, {0x14, 8}, {0x10, 8}, {0x0e, 5}, {0x0a, 8}, {0x06, 8}, {0x06, 9}, {0x12, 5}, {0x1a, 8},
    {0x16, 8}, {0x12, 8}, {0x0d, 5}, {0x09, 8}, {0x05, 8}, {0x05, 9}, {0x0c, 5}, {0x08, 8}, {0x04, 8}, {0x04, 9},
    {0x07, 3}, {0x0a, 5}, {0x08, 5}, {0x0c, 6},
};

/* B-10 motion_code magnitude 0..16 (a sign bit follows non-zero codes) */
static const uint8_t MV_CODES[17][2] = {
    {1, 1}, {1, 2}, {1, 3}, {1, 4}, {3, 6}, {5, 7}, {4, 7}, {3, 7}, {11, 9},
    {10, 9}, {9, 9}, {17, 10}, {16, 10}, {15, 10}, {14, 10}, {13, 10}, {12, 10},
};

/* B-12 / B-13 dct_dc_size_luminance / _chrominance */
static const Code DC_LUM[] = {
    {4, 3, 0}, {0, 2, 1}, {1, 2, 2}, {5, 3, 3}, {6, 3, 4}, {14, 4, 5},
    {30, 5, 6}, {62, 6, 7}, {126, 7, 8}, {254, 8, 9}, {510, 9, 10}, {511, 9, 11},
};
static const Code DC_CHROM[] = {
    {0, 2, 0}, {1, 2, 1}, {2, 2, 2}, {6, 3, 3}, {14, 4, 4}, {30, 5, 5},
    {62, 6, 6}, {126, 7, 7}, {254, 8, 8}, {510, 9, 9}, {1022, 10, 10}, {1023, 10, 11},
};

/* B-14 / B-15 DCT coefficients. Entries 0..110 are the (run, level) pairs in the
   order of DCT_RUNS (run 0 levels 1..40, run 1 levels 1..18, ...), then escape
   and end-of-block. Codes exclude the sign bit. B-14's "first coefficient" form
   ('1s' for run 0 level 1 in non-intra blocks) is handled in code. */
#define DCT_ENTRIES 113
static const uint8_t DCT_RUNS[32] = {40, 18, 5, 4, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2,
                                     2,  1,  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}; /* levels per run */
static const uint16_t DCT_B14[DCT_ENTRIES][2] = {
    /* run 0 */
    {0x3, 2}, {0x4, 4}, {0x5, 5}, {0x6, 7}, {0x26, 8}, {0x21, 8}, {0xa, 10}, {0x1d, 12}, {0x18, 12}, {0x13, 12},
    {0x10, 12}, {0x1a, 13}, {0x19, 13}, {0x18, 13}, {0x17, 13}, {0x1f, 14}, {0x1e, 14}, {0x1d, 14}, {0x1c, 14},
    {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14}, {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14},
    {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15}, {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15},
    {0x12, 15}, {0x11, 15}, {0x10, 15},
    /* run 1 */
    {0x3, 3}, {0x6, 6}, {0x25, 8}, {0xc, 10}, {0x1b, 12}, {0x16, 13}, {0x15, 13}, {0x1f, 15}, {0x1e, 15},
    {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16}, {0x11, 16}, {0x10, 16},
    /* runs 2..6 */
    {0x5, 4}, {0x4, 7}, {0xb, 10}, {0x14, 12}, {0x14, 13}, {0x7, 5}, {0x24, 8}, {0x1c, 12}, {0x13, 13},
    {0x6, 5}, {0xf, 10}, {0x12, 12}, {0x7, 6}, {0x9, 10}, {0x12, 13}, {0x5, 6}, {0x1e, 12}, {0x14, 16},
    /* runs 7..16 */
    {0x4, 6}, {0x15, 12}, {0x7, 7}, {0x11, 12}, {0x5, 7}, {0x11, 13}, {0x27, 8}, {0x10, 13}, {0x23, 8},
    {0x1a, 16}, {0x22, 8}, {0x19, 16}, {0x20, 8}, {0x18, 16}, {0xe, 10}, {0x17, 16}, {0xd, 10}, {0x16, 16},
    {0x8, 10}, {0x15, 16},
    /* runs 17..31 */
    {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13}, {0x1c, 13},
    {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16},
    /* escape, end of block */
    {0x1, 6}, {0x2, 2},
};
static const uint16_t DCT_B15[DCT_ENTRIES][2] = {
    /* run 0 */
    {0x2, 2}, {0x6, 3}, {0x7, 4}, {0x1c, 5}, {0x1d, 5}, {0x5, 6}, {0x4, 6}, {0x7b, 7}, {0x7c, 7}, {0x23, 8},
    {0x22, 8}, {0xfa, 8}, {0xfb, 8}, {0xfe, 8}, {0xff, 8}, {0x1f, 14}, {0x1e, 14}, {0x1d, 14}, {0x1c, 14},
    {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14}, {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14},
    {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15}, {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15},
    {0x12, 15}, {0x11, 15}, {0x10, 15},
    /* run 1 */
    {0x2, 3}, {0x6, 5}, {0x79, 7}, {0x27, 8}, {0x20, 8}, {0x16, 13}, {0x15, 13}, {0x1f, 15}, {0x1e, 15},
    {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16}, {0x11, 16}, {0x10, 16},
    /* runs 2..6 */
    {0x5, 5}, {0x7, 7}, {0xfc, 8}, {0xc, 10}, {0x14, 13}, {0x7, 5}, {0x26, 8}, {0x1c, 12}, {0x13, 13},
    {0x6, 6}, {0xfd, 8}, {0x12, 12}, {0x7, 6}, {0x4, 9}, {0x12, 13}, {0x6, 7}, {0x1e, 12}, {0x14, 16},
    /* runs 7..16 */
    {0x4, 7}, {0x15, 12}, {0x5, 7}, {0x11, 12}, {0x78, 7}, {0x11, 13}, {0x7a, 7}, {0x10, 13}, {0x21, 8},
    {0x1a, 16}, {0x25, 8}, {0x19, 16}, {0x24, 8}, {0x18, 16}, {0x5, 9}, {0x17, 16}, {0x7, 9}, {0x16, 16},
    {0xd, 10}, {0x15, 16},
    /* runs 17..31 */
    {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13}, {0x1c, 13},
    {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16},
    /* escape, end of block */
    {0x1, 6}, {0x6, 4},
};

enum { DCT_EOB = 64, DCT_ESC = 65, DCT_LONG = 66 };
typedef struct {
    uint8_t run, level, len; /* len 0 = invalid */
} Dct;
typedef struct {
    Dct l1[256];    /* by the first 8 bits */
    Dct l2[4][256]; /* codes longer than 8 bits all start with 000000xx: by the next 8 */
} DctTable;

static void dct_build(DctTable *t, const uint16_t codes[DCT_ENTRIES][2]) {
    memset(t, 0, sizeof *t);
    for (int i = 0; i < 4; i++) t->l1[i].run = DCT_LONG;
    int run = 0, level = 1;
    for (int k = 0; k < DCT_ENTRIES; k++) {
        Dct e = {k < 111 ? (uint8_t)run : k == 111 ? DCT_ESC : DCT_EOB, k < 111 ? (uint8_t)level : 0,
                 (uint8_t)codes[k][1]};
        if (k < 111 && ++level > DCT_RUNS[run]) run++, level = 1;
        int code = codes[k][0], len = codes[k][1];
        if (len <= 8) {
            for (int i = 0; i < 1 << (8 - len); i++) t->l1[(code << (8 - len)) + i] = e;
        } else {
            int hi = code >> (len - 8), rest = code & ((1 << (len - 8)) - 1);
            if (hi > 3) continue; /* cannot happen with the tables above */
            for (int i = 0; i < 1 << (16 - len); i++) t->l2[hi][(rest << (16 - len)) + i] = e;
        }
    }
}

static inline const Dct *dct_get(const DctTable *t, uint32_t v16) {
    const Dct *e = &t->l1[v16 >> 8];
    return e->run == DCT_LONG ? &t->l2[v16 >> 8][v16 & 255] : e;
}

/* scan orders: scan position -> raster index (7.3) */
static const uint8_t ZIGZAG[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
static const uint8_t ALT_SCAN[64] = {
    0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,  11,
    4,  12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44,
    52, 60, 37, 45, 53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

static const uint8_t DEFAULT_INTRA[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34,
    34, 38, 22, 22, 26, 27, 29, 34, 37, 40, 22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32,
    35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};
static const uint8_t NONLINEAR_Q[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                        24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};
static const int RATES[9][2] = {{0, 1}, {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001},
                                {30, 1}, {50, 1},       {60000, 1001}, {60, 1}};
/* MPEG-1 pel_aspect_ratio as height/width x 10000 */
static const int MPEG1_PAR[15] = {0,    10000, 6735,  7031,  7615,  8055,  8437, 8935,
                                  9157, 9815,  10255, 10695, 10950, 11575, 12015};

/* ------------------------------------------------------------------ IDCT */
/* Fast integer 8x8 IDCT after Chen & Wang, as in the MPEG Software Simulation
   Group decoder: 12-bit coefficients in, IEEE 1180-1990 accurate (tested in
   mpeg2_test.c). Output is clipped to [-256, 255]. */

#define W1 2841 /* 2048 * sqrt(2) * cos(1 * pi / 16) */
#define W2 2676 /* 2048 * sqrt(2) * cos(2 * pi / 16) */
#define W3 2408 /* 2048 * sqrt(2) * cos(3 * pi / 16) */
#define W5 1609 /* 2048 * sqrt(2) * cos(5 * pi / 16) */
#define W6 1108 /* 2048 * sqrt(2) * cos(6 * pi / 16) */
#define W7 565  /* 2048 * sqrt(2) * cos(7 * pi / 16) */

static inline int iclip(int v) { return v < -256 ? -256 : v > 255 ? 255 : v; }

static void idct_row(int16_t *b) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = b[4] * 2048) | (x2 = b[6]) | (x3 = b[2]) | (x4 = b[1]) | (x5 = b[7]) | (x6 = b[5]) | (x7 = b[3]))) {
        int16_t v = (int16_t)(b[0] * 8);
        b[0] = b[1] = b[2] = b[3] = b[4] = b[5] = b[6] = b[7] = v;
        return;
    }
    x0 = b[0] * 2048 + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (int)((181 * (int64_t)(x4 + x5) + 128) >> 8); /* 64-bit: damaged data can reach 2^32 here */
    x4 = (int)((181 * (int64_t)(x4 - x5) + 128) >> 8);
    b[0] = (int16_t)((x7 + x1) >> 8);
    b[1] = (int16_t)((x3 + x2) >> 8);
    b[2] = (int16_t)((x0 + x4) >> 8);
    b[3] = (int16_t)((x8 + x6) >> 8);
    b[4] = (int16_t)((x8 - x6) >> 8);
    b[5] = (int16_t)((x0 - x4) >> 8);
    b[6] = (int16_t)((x3 - x2) >> 8);
    b[7] = (int16_t)((x7 - x1) >> 8);
}

static void idct_col(int16_t *b) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = b[32] * 256) | (x2 = b[48]) | (x3 = b[16]) | (x4 = b[8]) | (x5 = b[56]) | (x6 = b[40]) |
          (x7 = b[24]))) {
        int16_t v = (int16_t)iclip((b[0] + 32) >> 6);
        b[0] = b[8] = b[16] = b[24] = b[32] = b[40] = b[48] = b[56] = v;
        return;
    }
    x0 = b[0] * 256 + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (int)((181 * (int64_t)(x4 + x5) + 128) >> 8);
    x4 = (int)((181 * (int64_t)(x4 - x5) + 128) >> 8);
    b[0] = (int16_t)iclip((x7 + x1) >> 14);
    b[8] = (int16_t)iclip((x3 + x2) >> 14);
    b[16] = (int16_t)iclip((x0 + x4) >> 14);
    b[24] = (int16_t)iclip((x8 + x6) >> 14);
    b[32] = (int16_t)iclip((x8 - x6) >> 14);
    b[40] = (int16_t)iclip((x0 - x4) >> 14);
    b[48] = (int16_t)iclip((x3 - x2) >> 14);
    b[56] = (int16_t)iclip((x7 - x1) >> 14);
}

/* rows: bit r set when row r has a non-zero coefficient (other rows are skipped) */
static void idct(int16_t *b, int rows) {
    for (int r = 0; r < 8; r++)
        if (rows & (1 << r)) idct_row(b + 8 * r);
    for (int c = 0; c < 8; c++) idct_col(b + c);
}

/* For the tests: the decoder's IDCT on a 12-bit coefficient block. */
void mpeg2_idct_for_test(int16_t block[64]);
void mpeg2_idct_for_test(int16_t block[64]) { idct(block, 0xFF); }

/* For the tests: is this code set prefix-free and within Kraft's bound? */
static bool prefix_free(const uint32_t *code, const int *len, int n) {
    double kraft = 0;
    for (int i = 0; i < n; i++) {
        kraft += 1.0 / (double)(1u << len[i]);
        for (int j = 0; j < n; j++)
            if (j != i && len[j] >= len[i] && code[j] >> (len[j] - len[i]) == code[i]) return false;
    }
    return kraft <= 1.0;
}

static inline uint8_t clip8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static void put_block(uint8_t *d, int ds, const int16_t *b) {
    for (int y = 0; y < 8; y++, d += ds, b += 8)
        for (int x = 0; x < 8; x++) d[x] = clip8(b[x]);
}

static void add_block(uint8_t *d, int ds, const int16_t *b) {
    for (int y = 0; y < 8; y++, d += ds, b += 8)
        for (int x = 0; x < 8; x++) d[x] = clip8(d[x] + b[x]);
}

/* ---------------------------------------------------- motion compensation */
/* mode: bit 0 horizontal half-pel, bit 1 vertical half-pel, bit 2 average
   with what is already in dst (second direction, dual prime). 7.6.4 */

static inline __attribute__((always_inline)) void mc_kernel(uint8_t *restrict d, int ds, const uint8_t *restrict s,
                                                            int ss, int mode, int w, int h) {
    switch (mode) {
    case 0:
        for (int y = 0; y < h; y++, d += ds, s += ss) memcpy(d, s, (size_t)w);
        break;
    case 1:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < w; x++) d[x] = (uint8_t)((s[x] + s[x + 1] + 1) >> 1);
        break;
    case 2:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < w; x++) d[x] = (uint8_t)((s[x] + s[x + ss] + 1) >> 1);
        break;
    case 3:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < w; x++) d[x] = (uint8_t)((s[x] + s[x + 1] + s[x + ss] + s[x + ss + 1] + 2) >> 2);
        break;
    case 4:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < w; x++) d[x] = (uint8_t)((d[x] + s[x] + 1) >> 1);
        break;
    case 5:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < w; x++) d[x] = (uint8_t)((d[x] + ((s[x] + s[x + 1] + 1) >> 1) + 1) >> 1);
        break;
    case 6:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < w; x++) d[x] = (uint8_t)((d[x] + ((s[x] + s[x + ss] + 1) >> 1) + 1) >> 1);
        break;
    default:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < w; x++)
                d[x] = (uint8_t)((d[x] + ((s[x] + s[x + 1] + s[x + ss] + s[x + ss + 1] + 2) >> 2) + 1) >> 1);
        break;
    }
}

static void mc16(uint8_t *d, int ds, const uint8_t *s, int ss, int mode, int h) { mc_kernel(d, ds, s, ss, mode, 16, h); }
static void mc8(uint8_t *d, int ds, const uint8_t *s, int ss, int mode, int h) { mc_kernel(d, ds, s, ss, mode, 8, h); }

/* A w x h prediction from plane (pw x ph, line step ps) at integer position
   (x, y) + half-pel flags in mode. Vectors pointing outside the plane (not
   allowed, but damaged or sloppy streams have them) repeat the edge pixels. */
static void mc_plane(uint8_t *dst, int ds, const uint8_t *plane, int ps, int pw, int ph, int x, int y, int mode, int w,
                     int h) {
    int hx = mode & 1, hy = mode >> 1 & 1;
    uint8_t tmp[17 * 17];
    const uint8_t *s;
    int ss;
    if (x < 0 || y < 0 || x + w + hx > pw || y + h + hy > ph) {
        for (int j = 0; j < h + hy; j++) {
            int yy = y + j < 0 ? 0 : y + j >= ph ? ph - 1 : y + j;
            for (int i = 0; i < w + hx; i++) {
                int xx = x + i < 0 ? 0 : x + i >= pw ? pw - 1 : x + i;
                tmp[j * 17 + i] = plane[(size_t)yy * (size_t)ps + (size_t)xx];
            }
        }
        s = tmp;
        ss = 17;
    } else {
        s = plane + (size_t)y * (size_t)ps + (size_t)x;
        ss = ps;
    }
    if (w == 16) mc16(dst, ds, s, ss, mode, h);
    else mc8(dst, ds, s, ss, mode, h);
}

/* ------------------------------------------------------------- the decoder */

typedef struct {
    uint8_t *mem;
    uint8_t *y, *u, *v;
    int64_t pts;
    char type;
    int tref, first_parity, concealed;
    bool tff, rff, progressive_frame;
    bool ref, queued, held, shown;
} Pic;

typedef struct {
    int v[2][4][2]; /* [direction][vector][x, y] in half-pels; dual prime adds vectors 2 and 3 */
    int sel[2][2];  /* motion_vertical_field_select per direction and vector */
} MbMv;

struct Mpeg2 {
    /* sequence */
    bool have_seq, mpeg1, unsupported, progressive_seq;
    int hsize, vsize, aspect, rate_code, rate_n, rate_d, disp_w, disp_h, matrix;
    uint8_t intra_q[64], inter_q[64]; /* raster order */
    /* configured geometry */
    int width, height, mbw, mbh, coded_w, coded_h, stride, cstride;
    /* references and output */
    bool closed_gop;
    int refs_ok; /* reference frames decoded since the start, a reset or a broken link */
    Pic pics[NPIC];
    Pic *fwd, *bwd, *cur, *pending; /* pending: a frame still waiting for its second field */
    Pic *queue[QMAX];
    int nq;
    /* the picture being decoded */
    bool pic_header, pic_started, skip_pic, second_field;
    int ptype, tref, fcode[2][2], dc_prec, structure;
    bool full_pel[2], tff, frame_pred_dct, concealment, qscale_type, intra_vlc, alt_scan, rff, progressive_frame;
    int64_t pts_next, pic_pts;
    uint8_t *mbdone;
    /* input copy (+ PAD zero bytes) */
    uint8_t *in;
    size_t in_cap;
    /* tables */
    Vlc mba[1 << 11], mbt_i[1 << 2], mbt_p[1 << 6], mbt_b[1 << 6], cbp[1 << 9], mv[1 << 10], dcl[1 << 9],
        dcc[1 << 10];
    DctTable b14, b15;
};

typedef struct {
    Br b;
    int qscale;
    int dc[3];
    int pmv[2][2][2]; /* motion vector predictors [r][direction][x, y] */
    int prev_flags, prev_mtype; /* the last coded macroblock, repeated by skipped B macroblocks */
    MbMv prev;
    int16_t blk[64];
} Slice;

Mpeg2 *mpeg2_new(void) {
    Mpeg2 *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    vlc_build(d->mba, 11, MBA_CODES, (int)(sizeof MBA_CODES / sizeof *MBA_CODES));
    vlc_build(d->mbt_i, 2, MBT_I, 2);
    vlc_build(d->mbt_p, 6, MBT_P, 7);
    vlc_build(d->mbt_b, 6, MBT_B, 11);
    Code c[64];
    for (int i = 0; i < 64; i++) c[i] = (Code){CBP_CODES[i][0], CBP_CODES[i][1], (int16_t)i};
    vlc_build(d->cbp, 9, c, 64);
    for (int i = 0; i < 17; i++) c[i] = (Code){MV_CODES[i][0], MV_CODES[i][1], (int16_t)i};
    vlc_build(d->mv, 10, c, 17);
    vlc_build(d->dcl, 9, DC_LUM, 12);
    vlc_build(d->dcc, 10, DC_CHROM, 12);
    dct_build(&d->b14, DCT_B14);
    dct_build(&d->b15, DCT_B15);
    memcpy(d->intra_q, DEFAULT_INTRA, 64);
    memset(d->inter_q, 16, 64);
    d->pts_next = d->pic_pts = MPEG2_NOPTS;
    return d;
}

void mpeg2_free(Mpeg2 *d) {
    if (!d) return;
    for (int i = 0; i < NPIC; i++) free(d->pics[i].mem);
    free(d->mbdone);
    free(d->in);
    free(d);
}

static void unhold(Mpeg2 *d) {
    for (int i = 0; i < NPIC; i++) d->pics[i].held = false;
}

static void enqueue(Mpeg2 *d, Pic *p) {
    if (p->shown) return;
    p->shown = true;
    if (d->nq == QMAX) { /* nobody is reading: drop the oldest */
        d->queue[0]->queued = false;
        memmove(d->queue, d->queue + 1, (QMAX - 1) * sizeof *d->queue);
        d->nq--;
    }
    p->queued = true;
    d->queue[d->nq++] = p;
}

/* Forget every reference and queued frame (the picture being parsed stays). */
static void drop_frames(Mpeg2 *d) {
    for (int i = 0; i < NPIC; i++) d->pics[i].ref = d->pics[i].queued = d->pics[i].held = false;
    d->nq = 0;
    d->fwd = d->bwd = d->cur = d->pending = NULL;
    d->refs_ok = 0;
}

void mpeg2_reset(Mpeg2 *d) {
    drop_frames(d);
    d->pic_header = d->pic_started = d->skip_pic = false;
    d->pts_next = d->pic_pts = MPEG2_NOPTS;
}

/* Geometry from the sequence header; reallocates (and drops every frame) when it changes. */
static bool configure(Mpeg2 *d) {
    int w = d->hsize, h = d->vsize;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    int mbw = (w + 15) / 16, mbh = d->mpeg1 || d->progressive_seq ? (h + 15) / 16 : 2 * ((h + 31) / 32);
    if (w == d->width && h == d->height && mbw == d->mbw && mbh == d->mbh && d->mbdone) return true;
    uint8_t *done = malloc((size_t)mbw * (size_t)mbh);
    if (!done) return false;
    drop_frames(d);
    for (int i = 0; i < NPIC; i++) {
        free(d->pics[i].mem);
        d->pics[i].mem = NULL;
    }
    free(d->mbdone);
    d->mbdone = done;
    d->width = w;
    d->height = h;
    d->mbw = mbw;
    d->mbh = mbh;
    d->coded_w = 16 * mbw;
    d->coded_h = 16 * mbh;
    d->stride = d->coded_w;
    d->cstride = d->coded_w / 2;
    return true;
}

static Pic *alloc_pic(Mpeg2 *d) {
    Pic *p = NULL;
    for (int i = 0; i < NPIC && !p; i++) {
        Pic *q = &d->pics[i];
        if (q->mem && !q->ref && !q->queued && !q->held && q != d->cur && q != d->pending) p = q;
    }
    for (int i = 0; i < NPIC && !p; i++) {
        Pic *q = &d->pics[i];
        if (q->mem) continue;
        size_t ys = (size_t)d->stride * (size_t)d->coded_h, cs = (size_t)d->cstride * (size_t)(d->coded_h / 2);
        if (!(q->mem = malloc(ys + 2 * cs))) return NULL;
        q->y = q->mem;
        q->u = q->mem + ys;
        q->v = q->u + cs;
        p = q;
    }
    if (!p) return NULL;
    p->ref = p->queued = p->held = p->shown = false;
    p->concealed = 0;
    return p;
}

/* -------------------------------------------------------------- headers */

static void load_matrix(Br *b, uint8_t *q) {
    for (int i = 0; i < 64; i++) {
        int v = (int)get(b, 8);
        q[ZIGZAG[i]] = (uint8_t)(v ? v : 1);
    }
}

static void sequence_header(Mpeg2 *d, Br *b) {
    int w = (int)get(b, 12), h = (int)get(b, 12), aspect = (int)get(b, 4), rate = (int)get(b, 4);
    skip(b, 18 + 1 + 10 + 1); /* bit rate, marker, vbv buffer size, constrained parameters */
    uint8_t intra[64], inter[64];
    if (bit(b)) load_matrix(b, intra);
    else memcpy(intra, DEFAULT_INTRA, 64);
    if (bit(b)) load_matrix(b, inter);
    else memset(inter, 16, 64);
    if (overrun(b) || !w || !h) return;
    memcpy(d->intra_q, intra, 64);
    memcpy(d->inter_q, inter, 64);
    d->hsize = w;
    d->vsize = h;
    d->aspect = aspect;
    d->rate_code = rate < 9 ? rate : 0;
    d->rate_n = d->rate_d = 0;
    d->disp_w = d->disp_h = 0;
    d->mpeg1 = true; /* until a sequence extension says otherwise */
    d->progressive_seq = true;
    d->unsupported = false;
    d->have_seq = true;
}

static void gop_header(Mpeg2 *d, Br *b) {
    skip(b, 25); /* time code */
    d->closed_gop = bit(b);
    if (bit(b)) d->refs_ok = 0; /* broken link: the B pictures after the next I can't be decoded */
}

static void picture_header(Mpeg2 *d, Br *b) {
    d->tref = (int)get(b, 10);
    d->ptype = (int)get(b, 3);
    skip(b, 16); /* vbv_delay */
    int f[2] = {7, 7};
    d->full_pel[0] = d->full_pel[1] = false;
    if (d->ptype == 2 || d->ptype == 3) {
        d->full_pel[0] = bit(b);
        f[0] = (int)get(b, 3);
    }
    if (d->ptype == 3) {
        d->full_pel[1] = bit(b);
        f[1] = (int)get(b, 3);
    }
    /* MPEG-1 meaning; an MPEG-2 picture coding extension replaces it */
    for (int s = 0; s < 2; s++) d->fcode[s][0] = d->fcode[s][1] = f[s];
    d->dc_prec = 0;
    d->structure = 3;
    d->tff = d->rff = false;
    d->frame_pred_dct = d->progressive_frame = true;
    d->concealment = d->qscale_type = d->intra_vlc = d->alt_scan = false;
    d->pic_header = d->have_seq && !overrun(b);
    d->skip_pic = d->ptype < 1 || d->ptype > 3 || d->unsupported; /* D pictures are skipped */
    d->pic_pts = d->pts_next;
    d->pts_next = MPEG2_NOPTS;
}

static void extension(Mpeg2 *d, Br *b) {
    switch (get(b, 4)) {
    case 1: { /* sequence extension */
        skip(b, 8);
        bool progressive = bit(b);
        int chroma = (int)get(b, 2), hx = (int)get(b, 2), vx = (int)get(b, 2);
        skip(b, 12 + 1 + 8 + 1); /* bit rate ext, marker, vbv ext, low delay */
        int rn = (int)get(b, 2), rd = (int)get(b, 5);
        if (overrun(b)) return;
        d->mpeg1 = false;
        d->progressive_seq = progressive;
        d->unsupported = chroma != 1; /* 4:2:2 / 4:4:4 */
        d->hsize = (d->hsize & 0xFFF) | hx << 12;
        d->vsize = (d->vsize & 0xFFF) | vx << 12;
        d->rate_n = rn;
        d->rate_d = rd;
        break;
    }
    case 2: /* sequence display extension */
        skip(b, 3);
        if (bit(b)) {
            skip(b, 16);
            d->matrix = (int)get(b, 8);
        }
        d->disp_w = (int)get(b, 14);
        skip(b, 1);
        d->disp_h = (int)get(b, 14);
        if (overrun(b)) d->disp_w = d->disp_h = 0;
        break;
    case 3: /* quant matrix extension (the chroma matrices only matter for 4:2:2) */
        if (bit(b)) load_matrix(b, d->intra_q);
        if (bit(b)) load_matrix(b, d->inter_q);
        break;
    case 8: /* picture coding extension */
        if (d->pic_started) break;
        for (int s = 0; s < 2; s++)
            for (int t = 0; t < 2; t++) d->fcode[s][t] = (int)get(b, 4);
        d->dc_prec = (int)get(b, 2);
        d->structure = (int)get(b, 2);
        d->tff = bit(b);
        d->frame_pred_dct = bit(b);
        d->concealment = bit(b);
        d->qscale_type = bit(b);
        d->intra_vlc = bit(b);
        d->alt_scan = bit(b);
        d->rff = bit(b);
        skip(b, 1); /* chroma_420_type */
        d->progressive_frame = bit(b);
        d->full_pel[0] = d->full_pel[1] = false;
        if (!d->structure || overrun(b)) d->skip_pic = true;
        break;
    }
}

/* ------------------------------------------------------ pictures and frames */

/* Copy the lines of the decoded field into the missing one. */
static void double_field(Mpeg2 *d, Pic *p, int have) {
    for (int y = 0; y < d->coded_h; y += 2)
        memcpy(p->y + (size_t)(y + !have) * d->stride, p->y + (size_t)(y + have) * d->stride, (size_t)d->coded_w);
    for (int y = 0; y < d->coded_h / 2; y += 2) {
        memcpy(p->u + (size_t)(y + !have) * d->cstride, p->u + (size_t)(y + have) * d->cstride, (size_t)d->coded_w / 2);
        memcpy(p->v + (size_t)(y + !have) * d->cstride, p->v + (size_t)(y + have) * d->cstride, (size_t)d->coded_w / 2);
    }
}

/* The second field never came: make the frame whole from the first. */
static void lonely_field(Mpeg2 *d) {
    Pic *p = d->pending;
    d->pending = NULL;
    double_field(d, p, p->first_parity);
    p->concealed += d->mbw * d->mbh / 2;
    if (p->type == 'B') enqueue(d, p);
}

static void start_picture(Mpeg2 *d) {
    if (!configure(d)) {
        d->skip_pic = true;
        return;
    }
    bool field = d->structure != 3, is_b = d->ptype == 3;
    int parity = d->structure == 2;
    if (d->pending) {
        Pic *p = d->pending;
        if (field && parity != p->first_parity && is_b == (p->type == 'B')) { /* its second field */
            d->pending = NULL;
            d->cur = p;
            d->second_field = true;
            goto ready;
        }
        lonely_field(d);
    }
    d->second_field = false;
    /* references available? (8.3: after a seek or a broken link the leading B pictures go) */
    if (d->ptype == 2 && !d->bwd) {
        d->skip_pic = true;
        return;
    }
    if (is_b && (!d->bwd || d->refs_ok < (d->closed_gop ? 1 : 2) || (!d->fwd && !d->closed_gop))) {
        d->skip_pic = true;
        return;
    }
    Pic *p = alloc_pic(d);
    if (!p) {
        d->skip_pic = true;
        return;
    }
    p->type = "?IPB"[d->ptype];
    p->tref = d->tref;
    p->pts = d->pic_pts;
    p->tff = d->tff;
    p->rff = d->rff;
    p->progressive_frame = d->progressive_frame;
    p->first_parity = parity;
    if (!is_b) { /* a new reference: the previous one can be shown now (display order) */
        if (d->bwd) enqueue(d, d->bwd);
        if (d->fwd) d->fwd->ref = false;
        d->fwd = d->bwd;
        d->bwd = p;
        p->ref = true;
        d->refs_ok++;
    }
    d->cur = p;
ready:
    memset(d->mbdone, 0, (size_t)d->mbw * (size_t)(field ? d->mbh / 2 : d->mbh));
    d->pic_started = true;
}

/* Patch up macroblocks no slice delivered: copy them from the nearest reference. */
static void conceal(Mpeg2 *d) {
    Pic *cur = d->cur;
    bool field = d->structure != 3;
    int rows = field ? d->mbh / 2 : d->mbh, par = d->structure == 2, f = field ? 2 : 1;
    const Pic *src = d->ptype == 3 ? (d->fwd ? d->fwd : d->bwd) : d->fwd;
    if (src == cur) src = NULL;
    size_t S = (size_t)d->stride, C = (size_t)d->cstride;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < d->mbw; x++) {
            if (d->mbdone[y * d->mbw + x]) continue;
            cur->concealed++;
            for (int j = 0; j < 16; j++) {
                size_t o = (size_t)par * S + (size_t)((16 * y + j) * f) * S + (size_t)(16 * x);
                if (src) memcpy(cur->y + o, src->y + o, 16);
                else memset(cur->y + o, 128, 16);
            }
            for (int j = 0; j < 8; j++) {
                size_t o = (size_t)par * C + (size_t)((8 * y + j) * f) * C + (size_t)(8 * x);
                if (src) {
                    memcpy(cur->u + o, src->u + o, 8);
                    memcpy(cur->v + o, src->v + o, 8);
                } else {
                    memset(cur->u + o, 128, 8);
                    memset(cur->v + o, 128, 8);
                }
            }
        }
    }
}

static void finish_picture(Mpeg2 *d) {
    d->pic_header = false;
    if (!d->pic_started) return;
    d->pic_started = false;
    conceal(d);
    Pic *p = d->cur;
    d->cur = NULL;
    if (d->structure != 3 && !d->second_field) {
        d->pending = p; /* the other field follows */
        return;
    }
    if (p->type == 'B') enqueue(d, p);
}

/* ---------------------------------------------------------- macroblocks */

static int qscale(const Mpeg2 *d, int code) { return d->mpeg1 ? code : d->qscale_type ? NONLINEAR_Q[code] : 2 * code; }

static void reset_dc(const Mpeg2 *d, Slice *s) { s->dc[0] = s->dc[1] = s->dc[2] = d->mpeg1 ? 128 : 128 << d->dc_prec; }

/* One motion vector component (7.6.3.1): VLC + residual, wrapped into range. */
static bool mv_component(const Mpeg2 *d, Br *b, int fcode, int pred, int *out) {
    const Vlc *v = &d->mv[show(b, 10)];
    if (!v->len) return false;
    skip(b, v->len);
    int code = v->sym;
    if (code && bit(b)) code = -code;
    int r = fcode - 1;
    if (r < 0 || r > 8) return false;
    int delta = code;
    if (r && code) {
        int a = code < 0 ? -code : code;
        delta = (a - 1) * (1 << r) + (int)get(b, r) + 1;
        if (code < 0) delta = -delta;
    }
    int f = 1 << r, v2 = pred + delta;
    if (v2 < -16 * f) v2 += 32 * f;
    else if (v2 > 16 * f - 1) v2 -= 32 * f;
    *out = v2;
    return true;
}

static int dmvector(Br *b) { return bit(b) ? (bit(b) ? -1 : 1) : 0; }

/* motion_vectors(s) for one direction (6.2.5.2, 7.6.3), updating the predictors. */
static bool read_mvs(const Mpeg2 *d, Slice *s, int dir, int mtype, MbMv *m) {
    Br *b = &s->b;
    int fx = d->fcode[dir][0], fy = d->fcode[dir][1], (*pmv)[2][2] = s->pmv, x = 0, y = 0;
    int scale = d->full_pel[dir] ? 2 : 1; /* MPEG-1 full-pel vectors */
    if (d->structure == 3) {
        if (mtype == MT_FRAME) {
            if (!mv_component(d, b, fx, pmv[0][dir][0], &x) || !mv_component(d, b, fy, pmv[0][dir][1], &y)) return false;
            pmv[0][dir][0] = pmv[1][dir][0] = x;
            pmv[0][dir][1] = pmv[1][dir][1] = y;
            m->v[dir][0][0] = x * scale;
            m->v[dir][0][1] = y * scale;
        } else if (mtype == MT_FIELD) {
            for (int r = 0; r < 2; r++) {
                m->sel[dir][r] = bit(b);
                if (!mv_component(d, b, fx, pmv[r][dir][0], &x) || !mv_component(d, b, fy, pmv[r][dir][1] >> 1, &y))
                    return false;
                pmv[r][dir][0] = x;
                pmv[r][dir][1] = y * 2;
                m->v[dir][r][0] = x;
                m->v[dir][r][1] = y;
            }
        } else { /* dual prime (7.6.3.6) */
            if (!mv_component(d, b, fx, pmv[0][dir][0], &x)) return false;
            int dmx = dmvector(b);
            if (!mv_component(d, b, fy, pmv[0][dir][1] >> 1, &y)) return false;
            int dmy = dmvector(b);
            pmv[0][dir][0] = pmv[1][dir][0] = x;
            pmv[0][dir][1] = pmv[1][dir][1] = y * 2;
            m->v[dir][0][0] = x;
            m->v[dir][0][1] = y;
            int k = d->tff ? 1 : 3; /* temporal distance to the opposite-parity field */
            m->v[dir][2][0] = ((x * k + (x > 0)) >> 1) + dmx;
            m->v[dir][2][1] = ((y * k + (y > 0)) >> 1) + dmy - 1;
            k = 4 - k;
            m->v[dir][3][0] = ((x * k + (x > 0)) >> 1) + dmx;
            m->v[dir][3][1] = ((y * k + (y > 0)) >> 1) + dmy + 1;
        }
    } else {
        if (mtype == MT_FIELD || mtype == MT_16X8) {
            int n = mtype == MT_FIELD ? 1 : 2;
            for (int r = 0; r < n; r++) {
                m->sel[dir][r] = bit(b);
                if (!mv_component(d, b, fx, pmv[r][dir][0], &x) || !mv_component(d, b, fy, pmv[r][dir][1], &y))
                    return false;
                pmv[r][dir][0] = x;
                pmv[r][dir][1] = y;
                m->v[dir][r][0] = x;
                m->v[dir][r][1] = y;
            }
            if (n == 1) pmv[1][dir][0] = x, pmv[1][dir][1] = y;
        } else { /* dual prime */
            if (!mv_component(d, b, fx, pmv[0][dir][0], &x)) return false;
            int dmx = dmvector(b);
            if (!mv_component(d, b, fy, pmv[0][dir][1], &y)) return false;
            int dmy = dmvector(b);
            pmv[0][dir][0] = pmv[1][dir][0] = x;
            pmv[0][dir][1] = pmv[1][dir][1] = y;
            m->v[dir][0][0] = x;
            m->v[dir][0][1] = y;
            m->v[dir][2][0] = ((x + (x > 0)) >> 1) + dmx;
            m->v[dir][2][1] = ((y + (y > 0)) >> 1) + dmy + (d->structure == 1 ? -1 : 1);
        }
    }
    return true;
}

/* One prediction: luma lw=16 x lh at (lx, ly) of the current picture's frame
   (dsel < 0) or field dsel, from the frame (rsel < 0) or field rsel of ref,
   displaced by (mvx, mvy) half-pels; chroma follows at half size (7.6.3.7). */
static void pred(Mpeg2 *d, const Pic *ref, int rsel, int dsel, int lx, int ly, int lh, int mvx, int mvy, bool avg) {
    int S = d->stride, C = d->cstride, fs = rsel >= 0 ? 2 : 1, fd = dsel >= 0 ? 2 : 1;
    int ph = d->coded_h / fs;
    Pic *cur = d->cur;
    int mode = (mvx & 1) | (mvy & 1) << 1 | (avg ? 4 : 0);
    uint8_t *dst = cur->y + (dsel > 0 ? S : 0) + (size_t)ly * (size_t)(S * fd) + (size_t)lx;
    mc_plane(dst, S * fd, ref->y + (rsel > 0 ? S : 0), S * fs, d->coded_w, ph, lx + (mvx >> 1), ly + (mvy >> 1), mode,
             16, lh);
    int cmx = mvx / 2, cmy = mvy / 2, cx = lx / 2, cy = ly / 2;
    mode = (cmx & 1) | (cmy & 1) << 1 | (avg ? 4 : 0);
    size_t co = (size_t)cy * (size_t)(C * fd) + (size_t)cx + (dsel > 0 ? (size_t)C : 0);
    int cxs = cx + (cmx >> 1), cys = cy + (cmy >> 1);
    mc_plane(cur->u + co, C * fd, ref->u + (rsel > 0 ? C : 0), C * fs, d->coded_w / 2, ph / 2, cxs, cys, mode, 8,
             lh / 2);
    mc_plane(cur->v + co, C * fd, ref->v + (rsel > 0 ? C : 0), C * fs, d->coded_w / 2, ph / 2, cxs, cys, mode, 8,
             lh / 2);
}

/* Reference frame for a direction, falling back to whatever exists. */
static const Pic *pick(const Mpeg2 *d, int dir) {
    const Pic *r = dir ? d->bwd : d->fwd;
    if (!r) r = dir ? d->fwd : d->bwd;
    return r ? r : d->cur;
}

/* Field pictures: the second field of a P frame may predict from the first. */
static const Pic *field_ref(const Mpeg2 *d, int dir, int sel) {
    if (!dir && d->ptype == 2 && d->second_field && sel != (d->structure == 2)) return d->cur;
    return pick(d, dir);
}

static void predict(Mpeg2 *d, int x, int y, int flags, int mtype, const MbMv *m) {
    bool first = true;
    int cp = d->structure == 2;
    for (int dir = 0; dir < 2; dir++) {
        if (!(flags & (dir ? MB_BWD : MB_FWD))) continue;
        bool avg = !first;
        first = false;
        const int(*v)[2] = m->v[dir];
        if (d->structure == 3) {
            const Pic *ref = pick(d, dir);
            if (mtype == MT_FRAME) {
                pred(d, ref, -1, -1, 16 * x, 16 * y, 16, v[0][0], v[0][1], avg);
            } else if (mtype == MT_FIELD) {
                for (int r = 0; r < 2; r++) pred(d, ref, m->sel[dir][r], r, 16 * x, 8 * y, 8, v[r][0], v[r][1], avg);
            } else { /* dual prime: same parity, then averaged with the opposite parity */
                pred(d, ref, 0, 0, 16 * x, 8 * y, 8, v[0][0], v[0][1], false);
                pred(d, ref, 1, 1, 16 * x, 8 * y, 8, v[0][0], v[0][1], false);
                pred(d, ref, 1, 0, 16 * x, 8 * y, 8, v[2][0], v[2][1], true);
                pred(d, ref, 0, 1, 16 * x, 8 * y, 8, v[3][0], v[3][1], true);
            }
        } else {
            if (mtype == MT_FIELD) {
                int s = m->sel[dir][0];
                pred(d, field_ref(d, dir, s), s, cp, 16 * x, 16 * y, 16, v[0][0], v[0][1], avg);
            } else if (mtype == MT_16X8) {
                for (int r = 0; r < 2; r++) {
                    int s = m->sel[dir][r];
                    pred(d, field_ref(d, dir, s), s, cp, 16 * x, 16 * y + 8 * r, 8, v[r][0], v[r][1], avg);
                }
            } else {
                pred(d, field_ref(d, 0, cp), cp, cp, 16 * x, 16 * y, 16, v[0][0], v[0][1], false);
                pred(d, field_ref(d, 0, !cp), !cp, cp, 16 * x, 16 * y, 16, v[2][0], v[2][1], true);
            }
        }
    }
}

static int escape_level(const Mpeg2 *d, Br *b) {
    if (!d->mpeg1) {
        int l = (int)get(b, 12);
        if (l >= 2048) l -= 4096;
        return l == -2048 ? 0 : l; /* 0 and -2048 are forbidden */
    }
    int l = (int)get(b, 8);
    if (l == 0) return (int)get(b, 8);
    if (l == 128) return (int)get(b, 8) - 256;
    return l > 128 ? l - 256 : l;
}

/* Coefficients of one block (7.2.2, 7.4): inverse scan, inverse quantisation,
   saturation and mismatch control into s->blk. rows: rows with coefficients. */
static bool block(Mpeg2 *d, Slice *s, bool intra, int comp, int *rows) {
    Br *b = &s->b;
    int16_t *blk = s->blk;
    const uint8_t *scan = d->alt_scan ? ALT_SCAN : ZIGZAG;
    const uint8_t *w = intra ? d->intra_q : d->inter_q;
    const DctTable *t = intra && d->intra_vlc ? &d->b15 : &d->b14;
    const int q = s->qscale;
    const bool m1 = d->mpeg1;
    int i, parity = 0, mask = 0;
    if (intra) {
        const Vlc *v = comp ? &d->dcc[show(b, 10)] : &d->dcl[show(b, 9)];
        if (!v->len) return false;
        skip(b, v->len);
        int size = v->sym, diff = 0;
        if (size) {
            diff = (int)get(b, size);
            if (!(diff >> (size - 1))) diff -= (1 << size) - 1;
        }
        int dc = s->dc[comp] + diff;
        s->dc[comp] = dc;
        int val = m1 ? dc * 8 : dc * (8 >> d->dc_prec);
        val = val < -2048 ? -2048 : val > 2047 ? 2047 : val;
        blk[0] = (int16_t)val;
        parity = val;
        mask = 1;
        i = 0;
    } else {
        i = -1;
        if (show(b, 1)) { /* first coefficient '1s': run 0, level 1 */
            skip(b, 1);
            int sign = bit(b);
            int m = ((3 * q * w[scan[0]]) >> (m1 ? 4 : 5));
            if (m1 && m && !(m & 1)) m--;
            int c = sign ? -m : m;
            blk[scan[0]] = (int16_t)c;
            parity = c;
            mask = 1 << (scan[0] >> 3);
            i = 0;
        }
    }
    for (;;) {
        const Dct *e = dct_get(t, show(b, 16));
        int run, level;
        if (e->run < 64) {
            if (!e->len) return false;
            skip(b, e->len);
            run = e->run;
            level = bit(b) ? -e->level : e->level;
        } else if (e->run == DCT_EOB) {
            skip(b, e->len);
            break;
        } else {
            skip(b, 6); /* escape */
            run = (int)get(b, 6);
            level = escape_level(d, b);
            if (!level) return false;
        }
        i += run + 1;
        if (i > 63) return false;
        int pos = scan[i], a = level < 0 ? -level : level, m;
        if (intra) m = (a * q * w[pos]) >> (m1 ? 3 : 4);
        else m = ((2 * a + 1) * q * w[pos]) >> (m1 ? 4 : 5);
        if (m1 && m && !(m & 1)) m--; /* MPEG-1 oddification */
        if (m > 2047) m = level < 0 ? (m > 2048 ? 2048 : m) : 2047;
        int c = level < 0 ? -m : m;
        blk[pos] = (int16_t)c;
        parity ^= c;
        mask |= 1 << (pos >> 3);
        if (overrun(b)) return false;
    }
    if (!m1 && !(parity & 1)) { /* mismatch control (7.4.4) */
        blk[63] ^= 1;
        mask |= 0x80;
    }
    *rows = mask;
    return true;
}

/* Destination of the macroblock's luma/chroma in the current picture. */
static void mb_dest(const Mpeg2 *d, int x, int y, uint8_t **py, uint8_t **pu, uint8_t **pv, int *ys, int *cs) {
    int S = d->stride, C = d->cstride;
    if (d->structure == 3) {
        *ys = S;
        *cs = C;
        *py = d->cur->y + (size_t)(16 * y) * S + 16 * x;
        *pu = d->cur->u + (size_t)(8 * y) * C + 8 * x;
        *pv = d->cur->v + (size_t)(8 * y) * C + 8 * x;
    } else {
        int par = d->structure == 2;
        *ys = 2 * S;
        *cs = 2 * C;
        *py = d->cur->y + (size_t)par * S + (size_t)(16 * y) * (2 * S) + 16 * x;
        *pu = d->cur->u + (size_t)par * C + (size_t)(8 * y) * (2 * C) + 8 * x;
        *pv = d->cur->v + (size_t)par * C + (size_t)(8 * y) * (2 * C) + 8 * x;
    }
}

static bool residual(Mpeg2 *d, Slice *s, int x, int y, bool intra, bool field_dct, int cbp) {
    uint8_t *py, *pu, *pv;
    int ys, cs;
    mb_dest(d, x, y, &py, &pu, &pv, &ys, &cs);
    for (int k = 0; k < 6; k++) {
        if (!(cbp & (32 >> k))) continue;
        int rows = 0;
        if (!block(d, s, intra, k < 4 ? 0 : k - 3, &rows)) {
            memset(s->blk, 0, sizeof s->blk);
            return false;
        }
        idct(s->blk, rows);
        uint8_t *dst;
        int st;
        if (k < 4) {
            if (field_dct) dst = py + (k & 1) * 8 + (size_t)(k >> 1) * ys, st = 2 * ys;
            else dst = py + (k & 1) * 8 + (size_t)(k >> 1) * 8 * ys, st = ys;
        } else {
            dst = k == 4 ? pu : pv;
            st = cs;
        }
        if (intra) put_block(dst, st, s->blk);
        else add_block(dst, st, s->blk);
        memset(s->blk, 0, sizeof s->blk);
    }
    return true;
}

static bool decode_mb(Mpeg2 *d, Slice *s, int x, int y) {
    Br *b = &s->b;
    const Vlc *t = d->ptype == 1 ? &d->mbt_i[show(b, 2)] : d->ptype == 2 ? &d->mbt_p[show(b, 6)] : &d->mbt_b[show(b, 6)];
    if (!t->len) return false;
    skip(b, t->len);
    int flags = t->sym, mtype = 0;
    bool frame_pic = d->structure == 3;
    if (flags & (MB_FWD | MB_BWD)) {
        mtype = d->mpeg1 || (frame_pic && d->frame_pred_dct) ? MT_FRAME : (int)get(b, 2);
        if (!mtype || (mtype == MT_DMV && d->ptype != 2)) return false;
    }
    bool field_dct = !d->mpeg1 && frame_pic && !d->frame_pred_dct && (flags & (MB_INTRA | MB_PAT)) && bit(b);
    if (flags & MB_QUANT) s->qscale = qscale(d, (int)get(b, 5));
    MbMv mv;
    memset(&mv, 0, sizeof mv);
    if (flags & MB_INTRA) {
        if (d->concealment) { /* concealment vectors: parsed for the predictors, not used */
            if (!read_mvs(d, s, 0, frame_pic ? MT_FRAME : MT_FIELD, &mv)) return false;
            skip(b, 1);
        } else {
            memset(s->pmv, 0, sizeof s->pmv);
        }
        s->prev_flags = MB_INTRA;
        return !overrun(b) && residual(d, s, x, y, true, field_dct, 63);
    }
    reset_dc(d, s);
    if ((flags & MB_FWD) && !read_mvs(d, s, 0, mtype, &mv)) return false;
    if ((flags & MB_BWD) && !read_mvs(d, s, 1, mtype, &mv)) return false;
    if (d->ptype == 2 && !(flags & MB_FWD)) { /* P macroblock without MC: zero vector, predictors reset */
        memset(s->pmv, 0, sizeof s->pmv);
        flags |= MB_FWD;
        mtype = frame_pic ? MT_FRAME : MT_FIELD;
        mv.sel[0][0] = d->structure == 2;
    }
    int cbp = 0;
    if (flags & MB_PAT) {
        const Vlc *c = &d->cbp[show(b, 9)];
        if (!c->len) return false;
        skip(b, c->len);
        cbp = c->sym;
    }
    if (overrun(b)) return false;
    predict(d, x, y, flags, mtype, &mv);
    s->prev_flags = flags;
    s->prev_mtype = mtype;
    s->prev = mv;
    return residual(d, s, x, y, false, field_dct, cbp);
}

/* 7.6.6: skipped macroblocks */
static bool skipped_mb(Mpeg2 *d, Slice *s, int x, int y) {
    reset_dc(d, s);
    if (d->ptype == 2) {
        MbMv mv;
        memset(&mv, 0, sizeof mv);
        memset(s->pmv, 0, sizeof s->pmv);
        mv.sel[0][0] = d->structure == 2;
        predict(d, x, y, MB_FWD, d->structure == 3 ? MT_FRAME : MT_FIELD, &mv);
        return true;
    }
    if (d->ptype != 3 || (s->prev_flags & MB_INTRA) || !(s->prev_flags & (MB_FWD | MB_BWD))) return false;
    predict(d, x, y, s->prev_flags & (MB_FWD | MB_BWD), s->prev_mtype, &s->prev);
    return true;
}

static void decode_slice(Mpeg2 *d, int code, const Br *start) {
    bool field = d->structure != 3;
    int rows = field ? d->mbh / 2 : d->mbh;
    Slice s;
    memset(&s, 0, sizeof s);
    s.b = *start;
    int y = code - 1;
    if (d->vsize > 2800) y += (int)get(&s.b, 3) << 7;
    if (y >= rows) return;
    s.qscale = qscale(d, (int)get(&s.b, 5));
    if (!d->mpeg1 && show(&s.b, 1)) skip(&s.b, 9); /* intra_slice_flag, intra_slice, reserved */
    while (bit(&s.b)) {                             /* extra_information_slice */
        skip(&s.b, 8);
        if (overrun(&s.b)) return;
    }
    reset_dc(d, &s);
    int x = 0, limit = d->mbw * rows;
    for (int n = 0;; n++) {
        int inc = 0;
        for (;;) {
            const Vlc *v = &d->mba[show(&s.b, 11)];
            if (!v->len || overrun(&s.b)) return;
            skip(&s.b, v->len);
            if (v->sym == MBA_STUFFING) continue;
            if (v->sym == MBA_ESCAPE) {
                if ((inc += 33) > limit) return;
                continue;
            }
            inc += v->sym;
            break;
        }
        if (n == 0) {
            x = inc - 1;
        } else {
            for (int k = 1; k < inc; k++) {
                if (++x >= d->mbw) {
                    if (!d->mpeg1) return; /* MPEG-2 slices stay in their row */
                    x = 0;
                    y++;
                }
                if (y >= rows || !skipped_mb(d, &s, x, y)) return;
                d->mbdone[y * d->mbw + x] = 1;
            }
            x++;
        }
        while (x >= d->mbw) {
            if (!d->mpeg1) return;
            x -= d->mbw;
            y++;
        }
        if (y >= rows || !decode_mb(d, &s, x, y)) return;
        d->mbdone[y * d->mbw + x] = 1;
        if (overrun(&s.b) || !show(&s.b, 23)) return; /* 23 zero bits: next start code */
    }
}

/* ------------------------------------------------------------------ API */

static void unit(Mpeg2 *d, int code, const uint8_t *p, size_t n) {
    Br b = {p, 0, n * 8};
    if (code >= 0x01 && code <= 0xAF) {
        if (!d->pic_header || d->skip_pic) return;
        if (!d->pic_started) start_picture(d);
        if (d->pic_started) decode_slice(d, code, &b);
        return;
    }
    switch (code) {
    case 0x00: finish_picture(d), picture_header(d, &b); break;
    case 0xB3: finish_picture(d), sequence_header(d, &b); break;
    case 0xB5: extension(d, &b); break;
    case 0xB8: finish_picture(d), gop_header(d, &b); break;
    case 0xB7: finish_picture(d); break; /* sequence end */
    }
}

static size_t next_start(const uint8_t *p, size_t from, size_t n) {
    for (size_t i = from; i + 3 <= n; i++) {
        if (p[i + 2] > 1) {
            i += 2;
            continue;
        }
        if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i;
    }
    return n;
}

int mpeg2_decode(Mpeg2 *d, const uint8_t *data, size_t n, int64_t pts) {
    unhold(d);
    if (n + PAD > d->in_cap) {
        size_t cap = d->in_cap ? d->in_cap : 65536;
        while (cap < n + PAD) cap *= 2;
        uint8_t *in = realloc(d->in, cap);
        if (!in) return -1;
        d->in = in;
        d->in_cap = cap;
    }
    if (n) memcpy(d->in, data, n);
    memset(d->in + n, 0, PAD);
    d->pts_next = pts;
    for (size_t s = next_start(d->in, 0, n); s + 4 <= n;) {
        size_t e = next_start(d->in, s + 3, n);
        unit(d, d->in[s + 3], d->in + s + 4, e - (s + 4));
        s = e;
    }
    finish_picture(d); /* whole pictures: the last one is complete */
    return d->nq;
}

static int gcd(int a, int b) {
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

bool mpeg2_frame(Mpeg2 *d, Mpeg2Frame *f) {
    if (!d->nq) return false;
    Pic *p = d->queue[0];
    memmove(d->queue, d->queue + 1, (size_t)(d->nq - 1) * sizeof *d->queue);
    d->nq--;
    p->queued = false;
    p->held = true;
    memset(f, 0, sizeof *f);
    f->y = p->y;
    f->u = p->u;
    f->v = p->v;
    f->stride_y = d->stride;
    f->stride_c = d->cstride;
    f->width = d->width;
    f->height = d->height;
    f->coded_width = d->coded_w;
    f->coded_height = d->coded_h;
    f->disp_width = d->disp_w ? d->disp_w : d->width;
    f->disp_height = d->disp_h ? d->disp_h : d->height;
    int an = f->disp_width, ad = f->disp_height;
    if (d->mpeg1) {
        int par = d->aspect > 0 && d->aspect < 15 ? MPEG1_PAR[d->aspect] : 10000;
        an = d->width * 10, ad = (int)((int64_t)d->height * par / 1000);
    } else if (d->aspect == 2) an = 4, ad = 3;
    else if (d->aspect == 3) an = 16, ad = 9;
    else if (d->aspect == 4) an = 221, ad = 100;
    int g = gcd(an, ad);
    f->dar_num = an / g;
    f->dar_den = ad / g;
    f->rate_num = RATES[d->rate_code][0] * (d->rate_n + 1);
    f->rate_den = RATES[d->rate_code][1] * (d->rate_d + 1);
    f->type = p->type;
    f->mpeg1 = d->mpeg1;
    f->progressive_sequence = d->progressive_seq;
    f->progressive_frame = p->progressive_frame;
    f->top_field_first = p->tff;
    f->repeat_first_field = p->rff;
    f->temporal_reference = p->tref;
    f->matrix = d->matrix;
    f->pts = p->pts;
    f->concealed = p->concealed;
    return true;
}

void mpeg2_flush(Mpeg2 *d) {
    unhold(d);
    finish_picture(d);
    if (d->pending) lonely_field(d);
    if (d->bwd) enqueue(d, d->bwd);
}

/* For the tests: every VLC table is prefix-free and the lookup tables decode
   each code back to its symbol. NULL = all good, else the broken table. */
static bool check_vlc(const Vlc *t, int bits, const Code *c, int n) {
    uint32_t code[64];
    int len[64];
    for (int i = 0; i < n; i++) {
        const Vlc *e = &t[c[i].code << (bits - c[i].len)];
        if (e->sym != c[i].sym || e->len != c[i].len) return false;
        code[i] = c[i].code;
        len[i] = c[i].len;
    }
    return prefix_free(code, len, n);
}

static bool check_dct(const DctTable *t, const uint16_t codes[DCT_ENTRIES][2]) {
    uint32_t code[DCT_ENTRIES];
    int len[DCT_ENTRIES], run = 0, level = 1;
    for (int k = 0; k < DCT_ENTRIES; k++) {
        code[k] = codes[k][0];
        len[k] = codes[k][1];
        const Dct *e = dct_get(t, code[k] << (16 - len[k]));
        int want_run = k < 111 ? run : k == 111 ? DCT_ESC : DCT_EOB, want_level = k < 111 ? level : 0;
        if (e->run != want_run || e->level != want_level || e->len != len[k]) return false;
        if (k < 111 && ++level > DCT_RUNS[run]) run++, level = 1;
    }
    return prefix_free(code, len, DCT_ENTRIES);
}

const char *mpeg2_selftest(void);
const char *mpeg2_selftest(void) {
    Mpeg2 *d = mpeg2_new();
    if (!d) return "out of memory";
    Code c[64];
    const char *bad = NULL;
    if (!check_vlc(d->mba, 11, MBA_CODES, (int)(sizeof MBA_CODES / sizeof *MBA_CODES))) bad = "macroblock_address_increment";
    else if (!check_vlc(d->mbt_i, 2, MBT_I, 2) || !check_vlc(d->mbt_p, 6, MBT_P, 7) || !check_vlc(d->mbt_b, 6, MBT_B, 11))
        bad = "macroblock_type";
    else if (!check_vlc(d->dcl, 9, DC_LUM, 12) || !check_vlc(d->dcc, 10, DC_CHROM, 12)) bad = "dct_dc_size";
    if (!bad) {
        for (int i = 0; i < 64; i++) c[i] = (Code){CBP_CODES[i][0], CBP_CODES[i][1], (int16_t)i};
        if (!check_vlc(d->cbp, 9, c, 64)) bad = "coded_block_pattern";
    }
    if (!bad) {
        for (int i = 0; i < 17; i++) c[i] = (Code){MV_CODES[i][0], MV_CODES[i][1], (int16_t)i};
        if (!check_vlc(d->mv, 10, c, 17)) bad = "motion_code";
    }
    if (!bad && !check_dct(&d->b14, DCT_B14)) bad = "DCT table B-14";
    if (!bad && !check_dct(&d->b15, DCT_B15)) bad = "DCT table B-15";
    mpeg2_free(d);
    return bad;
}

bool mpeg2_info(const Mpeg2 *d, int *width, int *height, int *rate_num, int *rate_den) {
    if (!d->have_seq) return false;
    if (width) *width = d->hsize;
    if (height) *height = d->vsize;
    if (rate_num) *rate_num = RATES[d->rate_code][0] * (d->rate_n + 1);
    if (rate_den) *rate_den = RATES[d->rate_code][1] * (d->rate_d + 1);
    return true;
}
