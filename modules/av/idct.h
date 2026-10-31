/* modules/av/idct.h — the 8x8 inverse DCT the video decoders share (MPEG-2, MPEG-4 Part 2).
 *
 * Fast integer IDCT after Chen & Wang, as in the MPEG Software Simulation Group
 * decoder: 12-bit coefficients in, IEEE 1180-1990 accurate (tested in
 * mpeg2_test.c). Output is clipped to [-256, 255]. Header-only: each decoder
 * gets its own static copy (pure library, libc only). */
#ifndef HEMMA_AV_IDCT_H
#define HEMMA_AV_IDCT_H

#include <stdint.h>

#define IDCT_W1 2841 /* 2048 * sqrt(2) * cos(1 * pi / 16) */
#define IDCT_W2 2676 /* 2048 * sqrt(2) * cos(2 * pi / 16) */
#define IDCT_W3 2408 /* 2048 * sqrt(2) * cos(3 * pi / 16) */
#define IDCT_W5 1609 /* 2048 * sqrt(2) * cos(5 * pi / 16) */
#define IDCT_W6 1108 /* 2048 * sqrt(2) * cos(6 * pi / 16) */
#define IDCT_W7 565  /* 2048 * sqrt(2) * cos(7 * pi / 16) */

static inline int idct_clip(int v) { return v < -256 ? -256 : v > 255 ? 255 : v; }

static inline void idct_row(int16_t *b) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = b[4] * 2048) | (x2 = b[6]) | (x3 = b[2]) | (x4 = b[1]) | (x5 = b[7]) | (x6 = b[5]) | (x7 = b[3]))) {
        int16_t v = (int16_t)(b[0] * 8);
        b[0] = b[1] = b[2] = b[3] = b[4] = b[5] = b[6] = b[7] = v;
        return;
    }
    x0 = b[0] * 2048 + 128;
    x8 = IDCT_W7 * (x4 + x5);
    x4 = x8 + (IDCT_W1 - IDCT_W7) * x4;
    x5 = x8 - (IDCT_W1 + IDCT_W7) * x5;
    x8 = IDCT_W3 * (x6 + x7);
    x6 = x8 - (IDCT_W3 - IDCT_W5) * x6;
    x7 = x8 - (IDCT_W3 + IDCT_W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = IDCT_W6 * (x3 + x2);
    x2 = x1 - (IDCT_W2 + IDCT_W6) * x2;
    x3 = x1 + (IDCT_W2 - IDCT_W6) * x3;
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

static inline void idct_col(int16_t *b) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = b[32] * 256) | (x2 = b[48]) | (x3 = b[16]) | (x4 = b[8]) | (x5 = b[56]) | (x6 = b[40]) |
          (x7 = b[24]))) {
        int16_t v = (int16_t)idct_clip((b[0] + 32) >> 6);
        b[0] = b[8] = b[16] = b[24] = b[32] = b[40] = b[48] = b[56] = v;
        return;
    }
    x0 = b[0] * 256 + 8192;
    x8 = IDCT_W7 * (x4 + x5) + 4;
    x4 = (x8 + (IDCT_W1 - IDCT_W7) * x4) >> 3;
    x5 = (x8 - (IDCT_W1 + IDCT_W7) * x5) >> 3;
    x8 = IDCT_W3 * (x6 + x7) + 4;
    x6 = (x8 - (IDCT_W3 - IDCT_W5) * x6) >> 3;
    x7 = (x8 - (IDCT_W3 + IDCT_W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = IDCT_W6 * (x3 + x2) + 4;
    x2 = (x1 - (IDCT_W2 + IDCT_W6) * x2) >> 3;
    x3 = (x1 + (IDCT_W2 - IDCT_W6) * x3) >> 3;
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
    b[0] = (int16_t)idct_clip((x7 + x1) >> 14);
    b[8] = (int16_t)idct_clip((x3 + x2) >> 14);
    b[16] = (int16_t)idct_clip((x0 + x4) >> 14);
    b[24] = (int16_t)idct_clip((x8 + x6) >> 14);
    b[32] = (int16_t)idct_clip((x8 - x6) >> 14);
    b[40] = (int16_t)idct_clip((x0 - x4) >> 14);
    b[48] = (int16_t)idct_clip((x3 - x2) >> 14);
    b[56] = (int16_t)idct_clip((x7 - x1) >> 14);
}

/* rows: bit r set when row r has a non-zero coefficient (other rows are skipped) */
static inline void idct(int16_t *b, int rows) {
    for (int r = 0; r < 8; r++)
        if (rows & (1 << r)) idct_row(b + 8 * r);
    for (int c = 0; c < 8; c++) idct_col(b + c);
}

#endif
