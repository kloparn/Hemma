/* modules/av/aac.c — see aac.h
 *
 * One frame, start to end:
 *   1. a new 1024-sample block arrives; the transient detector marks which of
 *      its eight 128-sample sub-blocks hold an attack (high-passed energy 10x
 *      the recent average);
 *   2. frame m = [block m-1, block m] gets its window sequence from the attacks
 *      that fall inside its short-window span (block m-1 from 448, block m up
 *      to 576) and the next frame's, so a START always precedes EIGHT_SHORT;
 *   3. window + MDCT (long, or eight short ones grouped by energy);
 *   4. psychoacoustics per band: energy, Bark spreading, tonality, absolute
 *      threshold, pre-echo limit -> the noise each band may carry;
 *   5. M/S per band where it is cheaper (perceptual entropy), with S held to
 *      its own SNR so matrixed surround (Dolby Surround / Pro Logic) survives;
 *   6. a scalefactor per band that puts its noise at that allowance, then one
 *      global offset searched until the frame fits the bit budget (target rate
 *      + half the reservoir);
 *   7. codebooks by a trellis over bands (section headers vs. spectral bits),
 *      scalefactors as differences, and the bitstream.
 *
 * Bitstream syntax follows ISO/IEC 14496-3 (4.4.2 raw_data_block, SCE/CPE,
 * ics_info, section/scalefactor/spectral data) with the Huffman codebooks of
 * ISO/IEC 13818-7 Annex A; the MDCT and windows follow 14496-3 4.6.11. */
#include "aac.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define SF_OFFSET 100
#define MAX_Q 8191
#define MAX_UNITS 128 /* 8 groups x 15 short bands, or up to 51 long bands */
#define INF_BITS 0x3fffffff
#define NCB 12

enum { ONLY_LONG = 0, LONG_START = 1, EIGHT_SHORT = 2, LONG_STOP = 3 };
enum { ID_SCE = 0, ID_CPE = 1, ID_END = 7 };

/* The Huffman codebooks of ISO/IEC 13818-7 Table A.1-A.12 (spectrum 1-11, scalefactors).
   Checked: every code fits its length, each book is prefix-free and complete (Kraft sum 1). */
static const uint16_t cb1_code[81] = {
    0x07f8, 0x01f1, 0x07fd, 0x03f5, 0x0068, 0x03f0, 0x07f7, 0x01ec, 0x07f5, 0x03f1,
    0x0072, 0x03f4, 0x0074, 0x0011, 0x0076, 0x01eb, 0x006c, 0x03f6, 0x07fc, 0x01e1,
    0x07f1, 0x01f0, 0x0061, 0x01f6, 0x07f2, 0x01ea, 0x07fb, 0x01f2, 0x0069, 0x01ed,
    0x0077, 0x0017, 0x006f, 0x01e6, 0x0064, 0x01e5, 0x0067, 0x0015, 0x0062, 0x0012,
    0x0000, 0x0014, 0x0065, 0x0016, 0x006d, 0x01e9, 0x0063, 0x01e4, 0x006b, 0x0013,
    0x0071, 0x01e3, 0x0070, 0x01f3, 0x07fe, 0x01e7, 0x07f3, 0x01ef, 0x0060, 0x01ee,
    0x07f0, 0x01e2, 0x07fa, 0x03f3, 0x006a, 0x01e8, 0x0075, 0x0010, 0x0073, 0x01f4,
    0x006e, 0x03f7, 0x07f6, 0x01e0, 0x07f9, 0x03f2, 0x0066, 0x01f5, 0x07ff, 0x01f7,
    0x07f4,
};
static const uint8_t cb1_len[81] = {
    11, 9, 11, 10, 7, 10, 11, 9, 11, 10, 7, 10, 7, 5, 7, 9, 7, 10, 11, 9,
    11, 9, 7, 9, 11, 9, 11, 9, 7, 9, 7, 5, 7, 9, 7, 9, 7, 5, 7, 5,
    1, 5, 7, 5, 7, 9, 7, 9, 7, 5, 7, 9, 7, 9, 11, 9, 11, 9, 7, 9,
    11, 9, 11, 10, 7, 9, 7, 5, 7, 9, 7, 10, 11, 9, 11, 10, 7, 9, 11, 9,
    11,
};
static const uint16_t cb2_code[81] = {
    0x01f3, 0x006f, 0x01fd, 0x00eb, 0x0023, 0x00ea, 0x01f7, 0x00e8, 0x01fa, 0x00f2,
    0x002d, 0x0070, 0x0020, 0x0006, 0x002b, 0x006e, 0x0028, 0x00e9, 0x01f9, 0x0066,
    0x00f8, 0x00e7, 0x001b, 0x00f1, 0x01f4, 0x006b, 0x01f5, 0x00ec, 0x002a, 0x006c,
    0x002c, 0x000a, 0x0027, 0x0067, 0x001a, 0x00f5, 0x0024, 0x0008, 0x001f, 0x0009,
    0x0000, 0x0007, 0x001d, 0x000b, 0x0030, 0x00ef, 0x001c, 0x0064, 0x001e, 0x000c,
    0x0029, 0x00f3, 0x002f, 0x00f0, 0x01fc, 0x0071, 0x01f2, 0x00f4, 0x0021, 0x00e6,
    0x00f7, 0x0068, 0x01f8, 0x00ee, 0x0022, 0x0065, 0x0031, 0x0002, 0x0026, 0x00ed,
    0x0025, 0x006a, 0x01fb, 0x0072, 0x01fe, 0x0069, 0x002e, 0x00f6, 0x01ff, 0x006d,
    0x01f6,
};
static const uint8_t cb2_len[81] = {
    9, 7, 9, 8, 6, 8, 9, 8, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 9, 7,
    8, 8, 6, 8, 9, 7, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 6, 5, 6, 5,
    3, 5, 6, 5, 6, 8, 6, 7, 6, 5, 6, 8, 6, 8, 9, 7, 9, 8, 6, 8,
    8, 7, 9, 8, 6, 7, 6, 4, 6, 8, 6, 7, 9, 7, 9, 7, 6, 8, 9, 7,
    9,
};
static const uint16_t cb3_code[81] = {
    0x0000, 0x0009, 0x00ef, 0x000b, 0x0019, 0x00f0, 0x01eb, 0x01e6, 0x03f2, 0x000a,
    0x0035, 0x01ef, 0x0034, 0x0037, 0x01e9, 0x01ed, 0x01e7, 0x03f3, 0x01ee, 0x03ed,
    0x1ffa, 0x01ec, 0x01f2, 0x07f9, 0x07f8, 0x03f8, 0x0ff8, 0x0008, 0x0038, 0x03f6,
    0x0036, 0x0075, 0x03f1, 0x03eb, 0x03ec, 0x0ff4, 0x0018, 0x0076, 0x07f4, 0x0039,
    0x0074, 0x03ef, 0x01f3, 0x01f4, 0x07f6, 0x01e8, 0x03ea, 0x1ffc, 0x00f2, 0x01f1,
    0x0ffb, 0x03f5, 0x07f3, 0x0ffc, 0x00ee, 0x03f7, 0x7ffe, 0x01f0, 0x07f5, 0x7ffd,
    0x1ffb, 0x3ffa, 0xffff, 0x00f1, 0x03f0, 0x3ffc, 0x01ea, 0x03ee, 0x3ffb, 0x0ff6,
    0x0ffa, 0x7ffc, 0x07f2, 0x0ff5, 0xfffe, 0x03f4, 0x07f7, 0x7ffb, 0x0ff7, 0x0ff9,
    0x7ffa,
};
static const uint8_t cb3_len[81] = {
    1, 4, 8, 4, 5, 8, 9, 9, 10, 4, 6, 9, 6, 6, 9, 9, 9, 10, 9, 10,
    13, 9, 9, 11, 11, 10, 12, 4, 6, 10, 6, 7, 10, 10, 10, 12, 5, 7, 11, 6,
    7, 10, 9, 9, 11, 9, 10, 13, 8, 9, 12, 10, 11, 12, 8, 10, 15, 9, 11, 15,
    13, 14, 16, 8, 10, 14, 9, 10, 14, 12, 12, 15, 11, 12, 16, 10, 11, 15, 12, 12,
    15,
};
static const uint16_t cb4_code[81] = {
    0x0007, 0x0016, 0x00f6, 0x0018, 0x0008, 0x00ef, 0x01ef, 0x00f3, 0x07f8, 0x0019,
    0x0017, 0x00ed, 0x0015, 0x0001, 0x00e2, 0x00f0, 0x0070, 0x03f0, 0x01ee, 0x00f1,
    0x07fa, 0x00ee, 0x00e4, 0x03f2, 0x07f6, 0x03ef, 0x07fd, 0x0005, 0x0014, 0x00f2,
    0x0009, 0x0004, 0x00e5, 0x00f4, 0x00e8, 0x03f4, 0x0006, 0x0002, 0x00e7, 0x0003,
    0x0000, 0x006b, 0x00e3, 0x0069, 0x01f3, 0x00eb, 0x00e6, 0x03f6, 0x006e, 0x006a,
    0x01f4, 0x03ec, 0x01f0, 0x03f9, 0x00f5, 0x00ec, 0x07fb, 0x00ea, 0x006f, 0x03f7,
    0x07f9, 0x03f3, 0x0fff, 0x00e9, 0x006d, 0x03f8, 0x006c, 0x0068, 0x01f5, 0x03ee,
    0x01f2, 0x07f4, 0x07f7, 0x03f1, 0x0ffe, 0x03ed, 0x01f1, 0x07f5, 0x07fe, 0x03f5,
    0x07fc,
};
static const uint8_t cb4_len[81] = {
    4, 5, 8, 5, 4, 8, 9, 8, 11, 5, 5, 8, 5, 4, 8, 8, 7, 10, 9, 8,
    11, 8, 8, 10, 11, 10, 11, 4, 5, 8, 4, 4, 8, 8, 8, 10, 4, 4, 8, 4,
    4, 7, 8, 7, 9, 8, 8, 10, 7, 7, 9, 10, 9, 10, 8, 8, 11, 8, 7, 10,
    11, 10, 12, 8, 7, 10, 7, 7, 9, 10, 9, 11, 11, 10, 12, 10, 9, 11, 11, 10,
    11,
};
static const uint16_t cb5_code[81] = {
    0x1fff, 0x0ff7, 0x07f4, 0x07e8, 0x03f1, 0x07ee, 0x07f9, 0x0ff8, 0x1ffd, 0x0ffd,
    0x07f1, 0x03e8, 0x01e8, 0x00f0, 0x01ec, 0x03ee, 0x07f2, 0x0ffa, 0x0ff4, 0x03ef,
    0x01f2, 0x00e8, 0x0070, 0x00ec, 0x01f0, 0x03ea, 0x07f3, 0x07eb, 0x01eb, 0x00ea,
    0x001a, 0x0008, 0x0019, 0x00ee, 0x01ef, 0x07ed, 0x03f0, 0x00f2, 0x0073, 0x000b,
    0x0000, 0x000a, 0x0071, 0x00f3, 0x07e9, 0x07ef, 0x01ee, 0x00ef, 0x0018, 0x0009,
    0x001b, 0x00eb, 0x01e9, 0x07ec, 0x07f6, 0x03eb, 0x01f3, 0x00ed, 0x0072, 0x00e9,
    0x01f1, 0x03ed, 0x07f7, 0x0ff6, 0x07f0, 0x03e9, 0x01ed, 0x00f1, 0x01ea, 0x03ec,
    0x07f8, 0x0ff9, 0x1ffc, 0x0ffc, 0x0ff5, 0x07ea, 0x03f3, 0x03f2, 0x07f5, 0x0ffb,
    0x1ffe,
};
static const uint8_t cb5_len[81] = {
    13, 12, 11, 11, 10, 11, 11, 12, 13, 12, 11, 10, 9, 8, 9, 10, 11, 12, 12, 10,
    9, 8, 7, 8, 9, 10, 11, 11, 9, 8, 5, 4, 5, 8, 9, 11, 10, 8, 7, 4,
    1, 4, 7, 8, 11, 11, 9, 8, 5, 4, 5, 8, 9, 11, 11, 10, 9, 8, 7, 8,
    9, 10, 11, 12, 11, 10, 9, 8, 9, 10, 11, 12, 13, 12, 12, 11, 10, 10, 11, 12,
    13,
};
static const uint16_t cb6_code[81] = {
    0x07fe, 0x03fd, 0x01f1, 0x01eb, 0x01f4, 0x01ea, 0x01f0, 0x03fc, 0x07fd, 0x03f6,
    0x01e5, 0x00ea, 0x006c, 0x0071, 0x0068, 0x00f0, 0x01e6, 0x03f7, 0x01f3, 0x00ef,
    0x0032, 0x0027, 0x0028, 0x0026, 0x0031, 0x00eb, 0x01f7, 0x01e8, 0x006f, 0x002e,
    0x0008, 0x0004, 0x0006, 0x0029, 0x006b, 0x01ee, 0x01ef, 0x0072, 0x002d, 0x0002,
    0x0000, 0x0003, 0x002f, 0x0073, 0x01fa, 0x01e7, 0x006e, 0x002b, 0x0007, 0x0001,
    0x0005, 0x002c, 0x006d, 0x01ec, 0x01f9, 0x00ee, 0x0030, 0x0024, 0x002a, 0x0025,
    0x0033, 0x00ec, 0x01f2, 0x03f8, 0x01e4, 0x00ed, 0x006a, 0x0070, 0x0069, 0x0074,
    0x00f1, 0x03fa, 0x07ff, 0x03f9, 0x01f6, 0x01ed, 0x01f8, 0x01e9, 0x01f5, 0x03fb,
    0x07fc,
};
static const uint8_t cb6_len[81] = {
    11, 10, 9, 9, 9, 9, 9, 10, 11, 10, 9, 8, 7, 7, 7, 8, 9, 10, 9, 8,
    6, 6, 6, 6, 6, 8, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 7, 6, 4,
    4, 4, 6, 7, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 8, 6, 6, 6, 6,
    6, 8, 9, 10, 9, 8, 7, 7, 7, 7, 8, 10, 11, 10, 9, 9, 9, 9, 9, 10,
    11,
};
static const uint16_t cb7_code[64] = {
    0x0000, 0x0005, 0x0037, 0x0074, 0x00f2, 0x01eb, 0x03ed, 0x07f7, 0x0004, 0x000c,
    0x0035, 0x0071, 0x00ec, 0x00ee, 0x01ee, 0x01f5, 0x0036, 0x0034, 0x0072, 0x00ea,
    0x00f1, 0x01e9, 0x01f3, 0x03f5, 0x0073, 0x0070, 0x00eb, 0x00f0, 0x01f1, 0x01f0,
    0x03ec, 0x03fa, 0x00f3, 0x00ed, 0x01e8, 0x01ef, 0x03ef, 0x03f1, 0x03f9, 0x07fb,
    0x01ed, 0x00ef, 0x01ea, 0x01f2, 0x03f3, 0x03f8, 0x07f9, 0x07fc, 0x03ee, 0x01ec,
    0x01f4, 0x03f4, 0x03f7, 0x07f8, 0x0ffd, 0x0ffe, 0x07f6, 0x03f0, 0x03f2, 0x03f6,
    0x07fa, 0x07fd, 0x0ffc, 0x0fff,
};
static const uint8_t cb7_len[64] = {
    1, 3, 6, 7, 8, 9, 10, 11, 3, 4, 6, 7, 8, 8, 9, 9, 6, 6, 7, 8,
    8, 9, 9, 10, 7, 7, 8, 8, 9, 9, 10, 10, 8, 8, 9, 9, 10, 10, 10, 11,
    9, 8, 9, 9, 10, 10, 11, 11, 10, 9, 9, 10, 10, 11, 12, 12, 11, 10, 10, 10,
    11, 11, 12, 12,
};
static const uint16_t cb8_code[64] = {
    0x000e, 0x0005, 0x0010, 0x0030, 0x006f, 0x00f1, 0x01fa, 0x03fe, 0x0003, 0x0000,
    0x0004, 0x0012, 0x002c, 0x006a, 0x0075, 0x00f8, 0x000f, 0x0002, 0x0006, 0x0014,
    0x002e, 0x0069, 0x0072, 0x00f5, 0x002f, 0x0011, 0x0013, 0x002a, 0x0032, 0x006c,
    0x00ec, 0x00fa, 0x0071, 0x002b, 0x002d, 0x0031, 0x006d, 0x0070, 0x00f2, 0x01f9,
    0x00ef, 0x0068, 0x0033, 0x006b, 0x006e, 0x00ee, 0x00f9, 0x03fc, 0x01f8, 0x0074,
    0x0073, 0x00ed, 0x00f0, 0x00f6, 0x01f6, 0x01fd, 0x03fd, 0x00f3, 0x00f4, 0x00f7,
    0x01f7, 0x01fb, 0x01fc, 0x03ff,
};
static const uint8_t cb8_len[64] = {
    5, 4, 5, 6, 7, 8, 9, 10, 4, 3, 4, 5, 6, 7, 7, 8, 5, 4, 4, 5,
    6, 7, 7, 8, 6, 5, 5, 6, 6, 7, 8, 8, 7, 6, 6, 6, 7, 7, 8, 9,
    8, 7, 6, 7, 7, 8, 8, 10, 9, 7, 7, 8, 8, 8, 9, 9, 10, 8, 8, 8,
    9, 9, 9, 10,
};
static const uint16_t cb9_code[169] = {
    0x0000, 0x0005, 0x0037, 0x00e7, 0x01de, 0x03ce, 0x03d9, 0x07c8, 0x07cd, 0x0fc8,
    0x0fdd, 0x1fe4, 0x1fec, 0x0004, 0x000c, 0x0035, 0x0072, 0x00ea, 0x00ed, 0x01e2,
    0x03d1, 0x03d3, 0x03e0, 0x07d8, 0x0fcf, 0x0fd5, 0x0036, 0x0034, 0x0071, 0x00e8,
    0x00ec, 0x01e1, 0x03cf, 0x03dd, 0x03db, 0x07d0, 0x0fc7, 0x0fd4, 0x0fe4, 0x00e6,
    0x0070, 0x00e9, 0x01dd, 0x01e3, 0x03d2, 0x03dc, 0x07cc, 0x07ca, 0x07de, 0x0fd8,
    0x0fea, 0x1fdb, 0x01df, 0x00eb, 0x01dc, 0x01e6, 0x03d5, 0x03de, 0x07cb, 0x07dd,
    0x07dc, 0x0fcd, 0x0fe2, 0x0fe7, 0x1fe1, 0x03d0, 0x01e0, 0x01e4, 0x03d6, 0x07c5,
    0x07d1, 0x07db, 0x0fd2, 0x07e0, 0x0fd9, 0x0feb, 0x1fe3, 0x1fe9, 0x07c4, 0x01e5,
    0x03d7, 0x07c6, 0x07cf, 0x07da, 0x0fcb, 0x0fda, 0x0fe3, 0x0fe9, 0x1fe6, 0x1ff3,
    0x1ff7, 0x07d3, 0x03d8, 0x03e1, 0x07d4, 0x07d9, 0x0fd3, 0x0fde, 0x1fdd, 0x1fd9,
    0x1fe2, 0x1fea, 0x1ff1, 0x1ff6, 0x07d2, 0x03d4, 0x03da, 0x07c7, 0x07d7, 0x07e2,
    0x0fce, 0x0fdb, 0x1fd8, 0x1fee, 0x3ff0, 0x1ff4, 0x3ff2, 0x07e1, 0x03df, 0x07c9,
    0x07d6, 0x0fca, 0x0fd0, 0x0fe5, 0x0fe6, 0x1feb, 0x1fef, 0x3ff3, 0x3ff4, 0x3ff5,
    0x0fe0, 0x07ce, 0x07d5, 0x0fc6, 0x0fd1, 0x0fe1, 0x1fe0, 0x1fe8, 0x1ff0, 0x3ff1,
    0x3ff8, 0x3ff6, 0x7ffc, 0x0fe8, 0x07df, 0x0fc9, 0x0fd7, 0x0fdc, 0x1fdc, 0x1fdf,
    0x1fed, 0x1ff5, 0x3ff9, 0x3ffb, 0x7ffd, 0x7ffe, 0x1fe7, 0x0fcc, 0x0fd6, 0x0fdf,
    0x1fde, 0x1fda, 0x1fe5, 0x1ff2, 0x3ffa, 0x3ff7, 0x3ffc, 0x3ffd, 0x7fff,
};
static const uint8_t cb9_len[169] = {
    1, 3, 6, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13, 3, 4, 6, 7, 8, 8, 9,
    10, 10, 10, 11, 12, 12, 6, 6, 7, 8, 8, 9, 10, 10, 10, 11, 12, 12, 12, 8,
    7, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 13, 9, 8, 9, 9, 10, 10, 11, 11,
    11, 12, 12, 12, 13, 10, 9, 9, 10, 11, 11, 11, 12, 11, 12, 12, 13, 13, 11, 9,
    10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 11, 10, 10, 11, 11, 12, 12, 13, 13,
    13, 13, 13, 13, 11, 10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 13, 14, 11, 10, 11,
    11, 12, 12, 12, 12, 13, 13, 14, 14, 14, 12, 11, 11, 12, 12, 12, 13, 13, 13, 14,
    14, 14, 15, 12, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 15, 15, 13, 12, 12, 12,
    13, 13, 13, 13, 14, 14, 14, 14, 15,
};
static const uint16_t cb10_code[169] = {
    0x0022, 0x0008, 0x001d, 0x0026, 0x005f, 0x00d3, 0x01cf, 0x03d0, 0x03d7, 0x03ed,
    0x07f0, 0x07f6, 0x0ffd, 0x0007, 0x0000, 0x0001, 0x0009, 0x0020, 0x0054, 0x0060,
    0x00d5, 0x00dc, 0x01d4, 0x03cd, 0x03de, 0x07e7, 0x001c, 0x0002, 0x0006, 0x000c,
    0x001e, 0x0028, 0x005b, 0x00cd, 0x00d9, 0x01ce, 0x01dc, 0x03d9, 0x03f1, 0x0025,
    0x000b, 0x000a, 0x000d, 0x0024, 0x0057, 0x0061, 0x00cc, 0x00dd, 0x01cc, 0x01de,
    0x03d3, 0x03e7, 0x005d, 0x0021, 0x001f, 0x0023, 0x0027, 0x0059, 0x0064, 0x00d8,
    0x00df, 0x01d2, 0x01e2, 0x03dd, 0x03ee, 0x00d1, 0x0055, 0x0029, 0x0056, 0x0058,
    0x0062, 0x00ce, 0x00e0, 0x00e2, 0x01da, 0x03d4, 0x03e3, 0x07eb, 0x01c9, 0x005e,
    0x005a, 0x005c, 0x0063, 0x00ca, 0x00da, 0x01c7, 0x01ca, 0x01e0, 0x03db, 0x03e8,
    0x07ec, 0x01e3, 0x00d2, 0x00cb, 0x00d0, 0x00d7, 0x00db, 0x01c6, 0x01d5, 0x01d8,
    0x03ca, 0x03da, 0x07ea, 0x07f1, 0x01e1, 0x00d4, 0x00cf, 0x00d6, 0x00de, 0x00e1,
    0x01d0, 0x01d6, 0x03d1, 0x03d5, 0x03f2, 0x07ee, 0x07fb, 0x03e9, 0x01cd, 0x01c8,
    0x01cb, 0x01d1, 0x01d7, 0x01df, 0x03cf, 0x03e0, 0x03ef, 0x07e6, 0x07f8, 0x0ffa,
    0x03eb, 0x01dd, 0x01d3, 0x01d9, 0x01db, 0x03d2, 0x03cc, 0x03dc, 0x03ea, 0x07ed,
    0x07f3, 0x07f9, 0x0ff9, 0x07f2, 0x03ce, 0x01e4, 0x03cb, 0x03d8, 0x03d6, 0x03e2,
    0x03e5, 0x07e8, 0x07f4, 0x07f5, 0x07f7, 0x0ffb, 0x07fa, 0x03ec, 0x03df, 0x03e1,
    0x03e4, 0x03e6, 0x03f0, 0x07e9, 0x07ef, 0x0ff8, 0x0ffe, 0x0ffc, 0x0fff,
};
static const uint8_t cb10_len[169] = {
    6, 5, 6, 6, 7, 8, 9, 10, 10, 10, 11, 11, 12, 5, 4, 4, 5, 6, 7, 7,
    8, 8, 9, 10, 10, 11, 6, 4, 5, 5, 6, 6, 7, 8, 8, 9, 9, 10, 10, 6,
    5, 5, 5, 6, 7, 7, 8, 8, 9, 9, 10, 10, 7, 6, 6, 6, 6, 7, 7, 8,
    8, 9, 9, 10, 10, 8, 7, 6, 7, 7, 7, 8, 8, 8, 9, 10, 10, 11, 9, 7,
    7, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11, 9, 8, 8, 8, 8, 8, 9, 9, 9,
    10, 10, 11, 11, 9, 8, 8, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11, 10, 9, 9,
    9, 9, 9, 9, 10, 10, 10, 11, 11, 12, 10, 9, 9, 9, 9, 10, 10, 10, 10, 11,
    11, 11, 12, 11, 10, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 11, 10, 10, 10,
    10, 10, 10, 11, 11, 12, 12, 12, 12,
};
static const uint16_t cb11_code[289] = {
    0x0000, 0x0006, 0x0019, 0x003d, 0x009c, 0x00c6, 0x01a7, 0x0390, 0x03c2, 0x03df,
    0x07e6, 0x07f3, 0x0ffb, 0x07ec, 0x0ffa, 0x0ffe, 0x038e, 0x0005, 0x0001, 0x0008,
    0x0014, 0x0037, 0x0042, 0x0092, 0x00af, 0x0191, 0x01a5, 0x01b5, 0x039e, 0x03c0,
    0x03a2, 0x03cd, 0x07d6, 0x00ae, 0x0017, 0x0007, 0x0009, 0x0018, 0x0039, 0x0040,
    0x008e, 0x00a3, 0x00b8, 0x0199, 0x01ac, 0x01c1, 0x03b1, 0x0396, 0x03be, 0x03ca,
    0x009d, 0x003c, 0x0015, 0x0016, 0x001a, 0x003b, 0x0044, 0x0091, 0x00a5, 0x00be,
    0x0196, 0x01ae, 0x01b9, 0x03a1, 0x0391, 0x03a5, 0x03d5, 0x0094, 0x009a, 0x0036,
    0x0038, 0x003a, 0x0041, 0x008c, 0x009b, 0x00b0, 0x00c3, 0x019e, 0x01ab, 0x01bc,
    0x039f, 0x038f, 0x03a9, 0x03cf, 0x0093, 0x00bf, 0x003e, 0x003f, 0x0043, 0x0045,
    0x009e, 0x00a7, 0x00b9, 0x0194, 0x01a2, 0x01ba, 0x01c3, 0x03a6, 0x03a7, 0x03bb,
    0x03d4, 0x009f, 0x01a0, 0x008f, 0x008d, 0x0090, 0x0098, 0x00a6, 0x00b6, 0x00c4,
    0x019f, 0x01af, 0x01bf, 0x0399, 0x03bf, 0x03b4, 0x03c9, 0x03e7, 0x00a8, 0x01b6,
    0x00ab, 0x00a4, 0x00aa, 0x00b2, 0x00c2, 0x00c5, 0x0198, 0x01a4, 0x01b8, 0x038c,
    0x03a4, 0x03c4, 0x03c6, 0x03dd, 0x03e8, 0x00ad, 0x03af, 0x0192, 0x00bd, 0x00bc,
    0x018e, 0x0197, 0x019a, 0x01a3, 0x01b1, 0x038d, 0x0398, 0x03b7, 0x03d3, 0x03d1,
    0x03db, 0x07dd, 0x00b4, 0x03de, 0x01a9, 0x019b, 0x019c, 0x01a1, 0x01aa, 0x01ad,
    0x01b3, 0x038b, 0x03b2, 0x03b8, 0x03ce, 0x03e1, 0x03e0, 0x07d2, 0x07e5, 0x00b7,
    0x07e3, 0x01bb, 0x01a8, 0x01a6, 0x01b0, 0x01b2, 0x01b7, 0x039b, 0x039a, 0x03ba,
    0x03b5, 0x03d6, 0x07d7, 0x03e4, 0x07d8, 0x07ea, 0x00ba, 0x07e8, 0x03a0, 0x01bd,
    0x01b4, 0x038a, 0x01c4, 0x0392, 0x03aa, 0x03b0, 0x03bc, 0x03d7, 0x07d4, 0x07dc,
    0x07db, 0x07d5, 0x07f0, 0x00c1, 0x07fb, 0x03c8, 0x03a3, 0x0395, 0x039d, 0x03ac,
    0x03ae, 0x03c5, 0x03d8, 0x03e2, 0x03e6, 0x07e4, 0x07e7, 0x07e0, 0x07e9, 0x07f7,
    0x0190, 0x07f2, 0x0393, 0x01be, 0x01c0, 0x0394, 0x0397, 0x03ad, 0x03c3, 0x03c1,
    0x03d2, 0x07da, 0x07d9, 0x07df, 0x07eb, 0x07f4, 0x07fa, 0x0195, 0x07f8, 0x03bd,
    0x039c, 0x03ab, 0x03a8, 0x03b3, 0x03b9, 0x03d0, 0x03e3, 0x03e5, 0x07e2, 0x07de,
    0x07ed, 0x07f1, 0x07f9, 0x07fc, 0x0193, 0x0ffd, 0x03dc, 0x03b6, 0x03c7, 0x03cc,
    0x03cb, 0x03d9, 0x03da, 0x07d3, 0x07e1, 0x07ee, 0x07ef, 0x07f5, 0x07f6, 0x0ffc,
    0x0fff, 0x019d, 0x01c2, 0x00b5, 0x00a1, 0x0096, 0x0097, 0x0095, 0x0099, 0x00a0,
    0x00a2, 0x00ac, 0x00a9, 0x00b1, 0x00b3, 0x00bb, 0x00c0, 0x018f, 0x0004,
};
static const uint8_t cb11_len[289] = {
    4, 5, 6, 7, 8, 8, 9, 10, 10, 10, 11, 11, 12, 11, 12, 12, 10, 5, 4, 5,
    6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 10, 11, 8, 6, 5, 5, 6, 7, 7,
    8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 8, 7, 6, 6, 6, 7, 7, 8, 8, 8,
    9, 9, 9, 10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9,
    10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10,
    10, 8, 9, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 8, 9,
    8, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 10, 8, 10, 9, 8, 8,
    9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 8, 10, 9, 9, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 11, 11, 8, 11, 9, 9, 9, 9, 9, 9, 10, 10, 10,
    10, 10, 11, 10, 11, 11, 8, 11, 10, 9, 9, 10, 9, 10, 10, 10, 10, 10, 11, 11,
    11, 11, 11, 8, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11,
    9, 11, 10, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 11, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 12, 10, 10, 10, 10,
    10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 9, 9, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 9, 5,
};
static const uint32_t sf_code[121] = {
    0x3ffe8, 0x3ffe6, 0x3ffe7, 0x3ffe5, 0x7fff5, 0x7fff1, 0x7ffed, 0x7fff6,
    0x7ffee, 0x7ffef, 0x7fff0, 0x7fffc, 0x7fffd, 0x7ffff, 0x7fffe, 0x7fff7,
    0x7fff8, 0x7fffb, 0x7fff9, 0x3ffe4, 0x7fffa, 0x3ffe3, 0x1ffef, 0x1fff0,
    0x0fff5, 0x1ffee, 0x0fff2, 0x0fff3, 0x0fff4, 0x0fff1, 0x07ff6, 0x07ff7,
    0x03ff9, 0x03ff5, 0x03ff7, 0x03ff3, 0x03ff6, 0x03ff2, 0x01ff7, 0x01ff5,
    0x00ff9, 0x00ff7, 0x00ff6, 0x007f9, 0x00ff4, 0x007f8, 0x003f9, 0x003f7,
    0x003f5, 0x001f8, 0x001f7, 0x000fa, 0x000f8, 0x000f6, 0x00079, 0x0003a,
    0x00038, 0x0001a, 0x0000b, 0x00004, 0x00000, 0x0000a, 0x0000c, 0x0001b,
    0x00039, 0x0003b, 0x00078, 0x0007a, 0x000f7, 0x000f9, 0x001f6, 0x001f9,
    0x003f4, 0x003f6, 0x003f8, 0x007f5, 0x007f4, 0x007f6, 0x007f7, 0x00ff5,
    0x00ff8, 0x01ff4, 0x01ff6, 0x01ff8, 0x03ff8, 0x03ff4, 0x0fff0, 0x07ff4,
    0x0fff6, 0x07ff5, 0x3ffe2, 0x7ffd9, 0x7ffda, 0x7ffdb, 0x7ffdc, 0x7ffdd,
    0x7ffde, 0x7ffd8, 0x7ffd2, 0x7ffd3, 0x7ffd4, 0x7ffd5, 0x7ffd6, 0x7fff2,
    0x7ffdf, 0x7ffe7, 0x7ffe8, 0x7ffe9, 0x7ffea, 0x7ffeb, 0x7ffe6, 0x7ffe0,
    0x7ffe1, 0x7ffe2, 0x7ffe3, 0x7ffe4, 0x7ffe5, 0x7ffd7, 0x7ffec, 0x7fff4,
    0x7fff3,
};
static const uint8_t sf_len[121] = {
    18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 18,
    19, 18, 17, 17, 16, 17, 16, 16, 16, 16, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13,
    12, 12, 12, 11, 12, 11, 10, 10, 10, 9, 9, 8, 8, 8, 7, 6, 6, 5, 4, 3,
    1, 4, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12,
    12, 13, 13, 13, 14, 14, 16, 15, 16, 15, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19,
};

static const AacCodebook books[NCB] = {
    {0, 0, 0, false, NULL, NULL},
    {81, 4, 1, false, cb1_code, cb1_len},
    {81, 4, 1, false, cb2_code, cb2_len},
    {81, 4, 2, true, cb3_code, cb3_len},
    {81, 4, 2, true, cb4_code, cb4_len},
    {81, 2, 4, false, cb5_code, cb5_len},
    {81, 2, 4, false, cb6_code, cb6_len},
    {64, 2, 7, true, cb7_code, cb7_len},
    {64, 2, 7, true, cb8_code, cb8_len},
    {169, 2, 12, true, cb9_code, cb9_len},
    {169, 2, 12, true, cb10_code, cb10_len},
    {289, 2, 16, true, cb11_code, cb11_len},
};

/* Codeword index of an all-zero quad/pair per book (to price silent bands inside a section). */
static const int zero_index[NCB] = {0, 40, 40, 0, 0, 40, 40, 0, 0, 0, 0, 0};

const AacCodebook *aac_codebook(int cb) { return cb >= 1 && cb <= 11 ? &books[cb] : NULL; }

void aac_sf_codebook(const uint32_t **code, const uint8_t **len) {
    *code = sf_code;
    *len = sf_len;
}

/* Scalefactor bands (ISO/IEC 14496-3 Tables 4.129-4.140). */
static const uint16_t swb_long_48[50] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  48,  56,
                                         64,  72,  80,  88,  96,  108, 120, 132, 144, 160, 176, 196, 216,
                                         240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608,
                                         640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 1024};
static const uint16_t swb_long_32[52] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  48,  56,
                                         64,  72,  80,  88,  96,  108, 120, 132, 144, 160, 176, 196, 216,
                                         240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608,
                                         640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992, 1024};
static const uint16_t swb_long_24[48] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,
                                         52,  60,  68,  76,  84,  92,  100, 108, 116, 124, 136, 148,
                                         160, 172, 188, 204, 220, 240, 260, 284, 308, 336, 364, 396,
                                         432, 468, 508, 552, 600, 652, 704, 768, 832, 896, 960, 1024};
static const uint16_t swb_long_16[44] = {0,   8,   16,  24,  32,  40,  48,  56,  64,  72,  80,
                                         88,  100, 112, 124, 136, 148, 160, 172, 184, 196, 212,
                                         228, 244, 260, 280, 300, 320, 344, 368, 396, 424, 456,
                                         492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024};
static const uint16_t swb_short_48[15] = {0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128};
static const uint16_t swb_short_24[16] = {0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128};
static const uint16_t swb_short_16[16] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128};

static const int rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

int aac_rate_index(int sample_rate) {
    for (int i = 0; i < 13; i++)
        if (rates[i] == sample_rate) return i;
    return -1;
}

const uint16_t *aac_swb_offsets(int sample_rate, bool short_window, int *nbands) {
    switch (sample_rate) {
    case 48000:
    case 44100: *nbands = short_window ? 14 : 49; return short_window ? swb_short_48 : swb_long_48;
    case 32000: *nbands = short_window ? 14 : 51; return short_window ? swb_short_48 : swb_long_32;
    case 24000:
    case 22050: *nbands = short_window ? 15 : 47; return short_window ? swb_short_24 : swb_long_24;
    case 16000:
    case 12000:
    case 11025: *nbands = short_window ? 15 : 43; return short_window ? swb_short_16 : swb_long_16;
    default: *nbands = 0; return NULL;
    }
}

/* ------------------------------------------------------------------ state */

typedef struct {
    int start, width; /* coefficients [start, start+width) of the coding-order spectrum */
    int band, group;
} Unit;

typedef struct {
    float spec[AAC_FRAME]; /* coding order: long = bins; short = per group, band, window */
    float x34[AAC_FRAME];  /* |spec|^(3/4) */
    int16_t q[AAC_FRAME];  /* quantised values */
    float en[MAX_UNITS], thr[MAX_UNITS]; /* band energy, noise it may carry */
    float sf0[MAX_UNITS];  /* scalefactor (real) putting the noise at thr; < -999 = band left out */
    int sfmin[MAX_UNITS];  /* smallest scalefactor keeping |q| <= 8191 */
    int sf[MAX_UNITS], maxq[MAX_UNITS];
    uint8_t cb[MAX_UNITS];
    int cost[MAX_UNITS][NCB]; /* spectral bits of a unit per codebook (INF_BITS = not allowed) */
    float thr_prev[64];       /* long-window thresholds of the previous frame (pre-echo limit) */
    bool have_prev;
    float hp_prev, hist[8]; /* transient detector: high-pass memory, recent sub-block energies */
    int global_gain;
} Chan;

struct AacEnc {
    AacConfig cfg;
    int rate_index, channels, shape;
    const uint16_t *swb_l, *swb_s;
    int nswb_l, nswb_s;
    int lim_l, lim_s; /* bands kept (bandwidth) */
    int mean_bits, reservoir, reservoir_max, max_frame_bits;
    /* input: three blocks per channel, oldest first (frame = first two, look-ahead = third) */
    float in[AAC_MAX_CHANNELS][3 * AAC_FRAME];
    uint8_t attack[3]; /* per buffered block: sub-blocks (bit s) holding an attack */
    long long pushed, blocks, units_out;
    int prev_seq, last_delta;
    /* this frame */
    int seq, ngroups, glen[8], max_sfb, ms_mode;
    Unit unit[MAX_UNITS];
    int nunits, per_group;
    bool ms[MAX_UNITS];
    Chan ch[AAC_MAX_CHANNELS];
    /* transforms */
    float win_l[2][1024], win_s[2][128];
    float fft_c[256], fft_s[256]; /* cos / sin (2 pi k / 512) */
    uint16_t rev512[512], rev64[64];
    float tw_l[4 * 512], tw_s[4 * 64]; /* DCT-IV pre- and post-twiddles */
    /* quantisation */
    float pow43[MAX_Q + 1], step[256], qscale[256];
    /* psychoacoustics */
    float bark_l[64], bark_s[16], ath_l[64], ath_s[16];
    float spread_l[64 * 64], spread_s[16 * 16];
    AacStats st;
};

/* --------------------------------------------------------------- transforms */

static void fft(const AacEnc *e, float *re, float *im, int m) {
    const uint16_t *rev = m == 512 ? e->rev512 : e->rev64;
    for (int i = 0; i < m; i++) {
        int j = rev[i];
        if (j > i) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= m; len <<= 1) {
        int half = len >> 1, step = 512 / len;
        for (int i = 0; i < m; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = e->fft_c[k * step], wi = -e->fft_s[k * step]; /* e^(-2 pi i k / len) */
                int a = i + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

/* DCT-IV y[k] = sum_n v[n] cos(pi/N (n + 1/2)(k + 1/2)) for N = 1024 or 128 through
   an N/2-point complex FFT. Pairing v[2m] + i v[N-1-2m] = c_m, every output pair is
   Z[p] = sum_m c_m e^(-i pi/N (2m + 1/2)(2p + 1/2)) with y[2p] = Re Z, y[N-1-2p] = -Im Z;
   expanding the exponent leaves a pre-twiddle e^(-i pi m/N), an FFT and a post-twiddle
   e^(-i pi (p + 1/4)/N). */
static void dct4(const AacEnc *e, const float *v, float *y, int n) {
    int m = n / 2;
    const float *tw = n == 1024 ? e->tw_l : e->tw_s;
    float re[512], im[512];
    for (int j = 0; j < m; j++) {
        float a = v[2 * j], b = v[n - 1 - 2 * j], c = tw[2 * j], s = tw[2 * j + 1];
        re[j] = a * c + b * s;
        im[j] = b * c - a * s;
    }
    fft(e, re, im, m);
    const float *post = tw + 2 * m;
    for (int p = 0; p < m; p++) {
        float c = post[2 * p], s = post[2 * p + 1];
        y[2 * p] = re[p] * c + im[p] * s;
        y[n - 1 - 2 * p] = re[p] * s - im[p] * c;
    }
}

/* MDCT of 2N inputs (quarters a b c d) = 2 x DCT-IV of (-c_rev - d, a - b_rev). */
void aac_mdct(const AacEnc *e, const float *z, float *x, int n) {
    int N = n / 2, Q = N / 2;
    float v[1024];
    for (int j = 0; j < Q; j++) {
        v[j] = -z[3 * Q - 1 - j] - z[3 * Q + j];
        v[Q + j] = z[j] - z[2 * Q - 1 - j];
    }
    dct4(e, v, x, N);
    for (int k = 0; k < N; k++) x[k] *= 2.0f;
}

/* IMDCT: u = DCT-IV(X) / N gives back the folded input; unfolding it yields the
   time-aliased (u2, -u2_rev, -u1_rev, -u1) that windowing + overlap-add cancels. */
void aac_imdct(const AacEnc *e, const float *x, float *y, int n) {
    int N = n / 2, Q = N / 2;
    float u[1024];
    dct4(e, x, u, N);
    float s = 1.0f / (float)N;
    for (int j = 0; j < Q; j++) {
        y[j] = u[Q + j] * s;
        y[Q + j] = -u[2 * Q - 1 - j] * s;
        y[2 * Q + j] = -u[Q - 1 - j] * s;
        y[3 * Q + j] = -u[j] * s;
    }
}

const float *aac_window(const AacEnc *e, bool short_window, int shape) {
    shape = shape ? 1 : 0;
    return short_window ? e->win_s[shape] : e->win_l[shape];
}

static double bessel_i0(double x) {
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 64; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-15) break;
    }
    return sum;
}

/* Rising half of a Kaiser-Bessel-derived window of length n (4.6.11.3.2). */
static void kbd_half(float *w, int n, double alpha) {
    double wp[1025], total = 0, acc = 0;
    int half = n / 2;
    for (int p = 0; p <= half; p++) {
        double r = (p - n / 4.0) / (n / 4.0);
        wp[p] = bessel_i0(PI * alpha * sqrt(fmax(0.0, 1.0 - r * r)));
        total += wp[p];
    }
    for (int i = 0; i < half; i++) {
        acc += wp[i];
        w[i] = (float)sqrt(acc / total);
    }
}

/* The 2048-sample analysis window of a long-type sequence (constant shape). */
static float long_window(const AacEnc *e, int seq, int n) {
    const float *wl = e->win_l[e->shape], *ws = e->win_s[e->shape];
    if (n < 1024) {
        if (seq != LONG_STOP) return wl[n];
        if (n < 448) return 0.0f;
        if (n < 576) return ws[n - 448];
        return 1.0f;
    }
    if (seq != LONG_START) return wl[2047 - n];
    if (n < 1472) return 1.0f;
    if (n < 1600) return ws[127 - (n - 1472)];
    return 0.0f;
}

/* ------------------------------------------------------------------ setup */

static double ath_db(double f) { /* Terhardt's threshold in quiet, dB SPL */
    double k = fmax(f, 20.0) / 1000.0;
    return 3.64 * pow(k, -0.8) - 6.5 * exp(-0.6 * (k - 3.3) * (k - 3.3)) + 0.001 * k * k * k * k;
}

static double bark(double f) { return 13.0 * atan(0.00076 * f) + 3.5 * atan((f / 7500.0) * (f / 7500.0)); }

/* Energy of a full-scale (32768) 1 kHz sine in one windowed transform of length n. */
static double full_scale_energy(const AacEnc *e, int n, int rate) {
    float z[2048], x[1024];
    for (int i = 0; i < n; i++) {
        float w = n == 2048 ? (i < 1024 ? e->win_l[0][i] : e->win_l[0][2047 - i])
                            : (i < 128 ? e->win_s[0][i] : e->win_s[0][255 - i]);
        z[i] = w * 32768.0f * (float)sin(2 * PI * 1000.0 * i / rate + 0.3);
    }
    aac_mdct(e, z, x, n);
    double s = 0;
    for (int k = 0; k < n / 2; k++) s += (double)x[k] * x[k];
    return s;
}

static void psy_tables(AacEnc *e, bool sh) {
    int nb = sh ? e->nswb_s : e->nswb_l, n = sh ? 256 : 2048;
    const uint16_t *swb = sh ? e->swb_s : e->swb_l;
    float *bk = sh ? e->bark_s : e->bark_l, *ath = sh ? e->ath_s : e->ath_l, *sp = sh ? e->spread_s : e->spread_l;
    int stride = sh ? 16 : 64;
    double hz = (double)e->cfg.sample_rate / n, ref = full_scale_energy(e, n, e->cfg.sample_rate);
    for (int b = 0; b < nb; b++) {
        bk[b] = (float)bark(hz * (swb[b] + swb[b + 1]) / 2.0);
        double lo = 1e9;
        for (int k = swb[b]; k < swb[b + 1]; k++) lo = fmin(lo, ath_db(hz * (k + 0.5)));
        /* full scale ~ 105 dB SPL: generous for TV dialogue turned up loud */
        ath[b] = (float)(ref * pow(10.0, (lo - 105.0) / 10.0));
    }
    for (int i = 0; i < nb; i++) {
        for (int j = 0; j < nb; j++) {
            double dz = bk[i] - bk[j], att = dz >= 0 ? 15.0 * dz : -27.0 * dz; /* masker below / above */
            sp[i * stride + j] = att > 60 ? 0.0f : (float)pow(10.0, -att / 10.0);
        }
    }
}

AacEnc *aac_open(const AacConfig *cfg, char *err, size_t errcap) {
    int nl = 0, ns = 0;
    const uint16_t *l = aac_swb_offsets(cfg->sample_rate, false, &nl), *s = aac_swb_offsets(cfg->sample_rate, true, &ns);
    if (!l) {
        if (err && errcap) snprintf(err, errcap, "AAC: %d Hz is not supported", cfg->sample_rate);
        return NULL;
    }
    if (cfg->channels < 1 || cfg->channels > AAC_MAX_CHANNELS) {
        if (err && errcap) snprintf(err, errcap, "AAC: %d channels (1 or 2 supported)", cfg->channels);
        return NULL;
    }
    AacEnc *e = calloc(1, sizeof *e);
    if (!e) {
        if (err && errcap) snprintf(err, errcap, "out of memory");
        return NULL;
    }
    e->cfg = *cfg;
    e->channels = cfg->channels;
    e->rate_index = aac_rate_index(cfg->sample_rate);
    e->shape = cfg->window_shape ? 1 : 0;
    e->swb_l = l;
    e->swb_s = s;
    e->nswb_l = nl;
    e->nswb_s = ns;
    int bitrate = cfg->bitrate > 0 ? cfg->bitrate : 80000 * cfg->channels;
    bitrate = bitrate < 8000 * cfg->channels ? 8000 * cfg->channels : bitrate;
    if (bitrate > 6144 * cfg->channels * cfg->sample_rate / 1024) bitrate = 6144 * cfg->channels * cfg->sample_rate / 1024;
    e->cfg.bitrate = bitrate;
    e->mean_bits = (int)((long long)bitrate * AAC_FRAME / cfg->sample_rate);
    e->max_frame_bits = 6144 * cfg->channels;
    e->reservoir_max = e->max_frame_bits - e->mean_bits;
    if (e->reservoir_max < 0) e->reservoir_max = 0;

    /* bandwidth from the bitrate per channel (Hz kept), unless given */
    int per_ch = bitrate / cfg->channels, bw = cfg->bandwidth;
    if (bw <= 0)
        bw = per_ch >= 112000 ? 20000
           : per_ch >= 96000  ? 19000
           : per_ch >= 80000  ? 17500
           : per_ch >= 64000  ? 16000
           : per_ch >= 48000  ? 14000
           : per_ch >= 32000  ? 11000
                              : 8000;
    if (bw > cfg->sample_rate / 2) bw = cfg->sample_rate / 2;
    e->lim_l = nl;
    while (e->lim_l > 1 && (double)l[e->lim_l - 1] * cfg->sample_rate / 2048.0 >= bw) e->lim_l--;
    e->lim_s = ns;
    while (e->lim_s > 1 && (double)s[e->lim_s - 1] * cfg->sample_rate / 256.0 >= bw) e->lim_s--;
    e->st.bandwidth = (int)((double)l[e->lim_l] * cfg->sample_rate / 2048.0);

    /* windows */
    for (int i = 0; i < 1024; i++) e->win_l[0][i] = (float)sin(PI / 2048.0 * (i + 0.5));
    for (int i = 0; i < 128; i++) e->win_s[0][i] = (float)sin(PI / 256.0 * (i + 0.5));
    kbd_half(e->win_l[1], 2048, 4.0);
    kbd_half(e->win_s[1], 256, 6.0);
    /* FFT + DCT-IV twiddles */
    for (int k = 0; k < 256; k++) {
        e->fft_c[k] = (float)cos(2 * PI * k / 512.0);
        e->fft_s[k] = (float)sin(2 * PI * k / 512.0);
    }
    for (int i = 0; i < 512; i++) {
        int r = 0;
        for (int b = 0; b < 9; b++) r |= ((i >> b) & 1) << (8 - b);
        e->rev512[i] = (uint16_t)r;
    }
    for (int i = 0; i < 64; i++) {
        int r = 0;
        for (int b = 0; b < 6; b++) r |= ((i >> b) & 1) << (5 - b);
        e->rev64[i] = (uint16_t)r;
    }
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? 128 : 1024, m = n / 2;
        float *tw = pass ? e->tw_s : e->tw_l;
        for (int j = 0; j < m; j++) {
            tw[2 * j] = (float)cos(PI * j / n);
            tw[2 * j + 1] = (float)sin(PI * j / n);
            tw[2 * m + 2 * j] = (float)cos(PI * (j + 0.25) / n);
            tw[2 * m + 2 * j + 1] = (float)sin(PI * (j + 0.25) / n);
        }
    }
    /* quantiser */
    for (int q = 0; q <= MAX_Q; q++) e->pow43[q] = (float)pow((double)q, 4.0 / 3.0);
    for (int sf = 0; sf < 256; sf++) {
        e->step[sf] = (float)pow(2.0, 0.25 * (sf - SF_OFFSET));
        e->qscale[sf] = (float)pow(2.0, -0.1875 * (sf - SF_OFFSET));
    }
    psy_tables(e, false);
    psy_tables(e, true);
    e->prev_seq = ONLY_LONG;
    e->st.bits_min = INF_BITS;
    return e;
}

void aac_close(AacEnc *e) { free(e); }

const AacStats *aac_stats(const AacEnc *e) { return &e->st; }

int aac_specific_config(const AacEnc *e, uint8_t out[2]) {
    unsigned v = (2u << 11) | ((unsigned)e->rate_index << 7) | ((unsigned)e->channels << 3); /* AOT 2, 1024, no ext */
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)v;
    return 2;
}

int aac_adts_header(int sample_rate, int channels, int payload_bytes, uint8_t hdr[7]) {
    int sr = aac_rate_index(sample_rate), len = payload_bytes + 7;
    if (sr < 0 || channels < 1 || channels > 7 || len > 8191) return 0;
    hdr[0] = 0xFF;
    hdr[1] = 0xF1; /* MPEG-4, layer 0, no CRC */
    hdr[2] = (uint8_t)((1 << 6) | (sr << 2) | (channels >> 2)); /* profile LC */
    hdr[3] = (uint8_t)(((channels & 3) << 6) | (len >> 11));
    hdr[4] = (uint8_t)(len >> 3);
    hdr[5] = (uint8_t)(((len & 7) << 5) | 0x1F); /* buffer fullness 0x7FF: variable rate */
    hdr[6] = 0xFC;
    return 7;
}

/* ------------------------------------------------------------- bit writer */

typedef struct {
    uint8_t *p;
    int pos, cap; /* bits */
} BitW;

static void put(BitW *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (w->pos < w->cap) {
            if ((v >> i) & 1) w->p[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
        }
        w->pos++;
    }
}

/* ---------------------------------------------------------- noiseless code */

static int esc_bits(int v) { /* escape sequence of book 11 for |v| >= 16 */
    int n = 0;
    while ((v >> (n + 5)) > 0) n++; /* 2^(n+4) <= v < 2^(n+5) */
    return 2 * n + 5;
}

/* Spectral bits of n values (a multiple of the book's dimension) in book cb; the
   caller guarantees |q| <= lav (book 11 escapes anything larger). */
static int unit_bits(const int16_t *q, int n, int cb) {
    const AacCodebook *b = &books[cb];
    const uint8_t *len = b->len;
    int bits = 0;
    switch (cb) {
    case 1:
    case 2:
        for (int i = 0; i < n; i += 4) bits += len[27 * (q[i] + 1) + 9 * (q[i + 1] + 1) + 3 * (q[i + 2] + 1) + q[i + 3] + 1];
        break;
    case 3:
    case 4:
        for (int i = 0; i < n; i += 4) {
            int a = abs(q[i]), c = abs(q[i + 1]), d = abs(q[i + 2]), f = abs(q[i + 3]);
            bits += len[27 * a + 9 * c + 3 * d + f] + (a != 0) + (c != 0) + (d != 0) + (f != 0);
        }
        break;
    case 5:
    case 6:
        for (int i = 0; i < n; i += 2) bits += len[9 * (q[i] + 4) + q[i + 1] + 4];
        break;
    case 7:
    case 8:
        for (int i = 0; i < n; i += 2) {
            int a = abs(q[i]), c = abs(q[i + 1]);
            bits += len[8 * a + c] + (a != 0) + (c != 0);
        }
        break;
    case 9:
    case 10:
        for (int i = 0; i < n; i += 2) {
            int a = abs(q[i]), c = abs(q[i + 1]);
            bits += len[13 * a + c] + (a != 0) + (c != 0);
        }
        break;
    default: /* 11 */
        for (int i = 0; i < n; i += 2) {
            int a = abs(q[i]), c = abs(q[i + 1]);
            bits += len[17 * (a < 16 ? a : 16) + (c < 16 ? c : 16)] + (a != 0) + (c != 0);
            if (a >= 16) bits += esc_bits(a);
            if (c >= 16) bits += esc_bits(c);
        }
        break;
    }
    return bits;
}

static void put_esc(BitW *w, int v) {
    int n = 0;
    while ((v >> (n + 5)) > 0) n++;
    put(w, (1u << n) - 1u, n); /* n ones */
    put(w, 0, 1);
    put(w, (uint32_t)(v - (1 << (n + 4))), n + 4);
}

static void put_unit(BitW *w, const int16_t *q, int n, int cb) {
    const AacCodebook *b = &books[cb];
    if (b->dim == 4) {
        for (int i = 0; i < n; i += 4) {
            int idx;
            if (b->is_unsigned) idx = 27 * abs(q[i]) + 9 * abs(q[i + 1]) + 3 * abs(q[i + 2]) + abs(q[i + 3]);
            else idx = 27 * (q[i] + 1) + 9 * (q[i + 1] + 1) + 3 * (q[i + 2] + 1) + q[i + 3] + 1;
            put(w, b->code[idx], b->len[idx]);
            if (b->is_unsigned)
                for (int k = 0; k < 4; k++)
                    if (q[i + k]) put(w, q[i + k] < 0, 1);
        }
        return;
    }
    for (int i = 0; i < n; i += 2) {
        int a = q[i], c = q[i + 1], idx;
        if (cb == 5 || cb == 6) idx = 9 * (a + 4) + c + 4;
        else if (cb <= 8) idx = 8 * abs(a) + abs(c);
        else if (cb <= 10) idx = 13 * abs(a) + abs(c);
        else idx = 17 * (abs(a) < 16 ? abs(a) : 16) + (abs(c) < 16 ? abs(c) : 16);
        put(w, b->code[idx], b->len[idx]);
        if (b->is_unsigned) {
            if (a) put(w, a < 0, 1);
            if (c) put(w, c < 0, 1);
        }
        if (cb == 11) {
            if (abs(a) >= 16) put_esc(w, abs(a));
            if (abs(c) >= 16) put_esc(w, abs(c));
        }
    }
}

/* -------------------------------------------------------- frame analysis */

/* Attacks in one new block: high-passed (first difference) energy of each 128-sample
   sub-block against the average of the eight before it. */
static uint8_t find_attacks(AacEnc *e, int c, const float *x) {
    Chan *ch = &e->ch[c];
    uint8_t mask = 0;
    float prev = ch->hp_prev;
    for (int s = 0; s < 8; s++) {
        float en = 0;
        for (int i = 0; i < 128; i++) {
            float d = x[s * 128 + i] - prev;
            prev = x[s * 128 + i];
            en += d * d;
        }
        float avg = 0;
        for (int h = 0; h < 8; h++) avg += ch->hist[h];
        avg *= 0.125f;
        if (en > 10.0f * avg && en > 1.4e5f) mask |= (uint8_t)(1 << s);
        memmove(ch->hist, ch->hist + 1, 7 * sizeof(float));
        ch->hist[7] = en;
    }
    ch->hp_prev = prev;
    return mask;
}

/* Window sequence of the frame [block 0, block 1] given the attack masks of the three
   buffered blocks: short when an attack falls in its short-window span (block 0 from
   sample 448 = sub-blocks 3-7, block 1 up to 576 = sub-blocks 0-4). */
static int choose_sequence(AacEnc *e) {
    bool now = (e->attack[0] & 0xF8) || (e->attack[1] & 0x1F);
    bool next = (e->attack[1] & 0xF8) || (e->attack[2] & 0x1F);
    int prev = e->prev_seq;
    if (now) return EIGHT_SHORT; /* the previous frame was made a START (or SHORT) for this */
    if (prev == EIGHT_SHORT || prev == LONG_START) return next ? EIGHT_SHORT : LONG_STOP;
    return next ? LONG_START : ONLY_LONG;
}

static void transform(AacEnc *e, int c, float *wm /* window-major 1024 */) {
    const float *x = e->in[c];
    float z[2048];
    if (e->seq != EIGHT_SHORT) {
        for (int n = 0; n < 2048; n++) z[n] = x[n] * long_window(e, e->seq, n);
        aac_mdct(e, z, wm, 2048);
        return;
    }
    const float *ws = e->win_s[e->shape];
    for (int w = 0; w < 8; w++) {
        const float *s = x + 448 + 128 * w;
        for (int j = 0; j < 128; j++) {
            z[j] = s[j] * ws[j];
            z[128 + j] = s[128 + j] * ws[127 - j];
        }
        aac_mdct(e, z, wm + 128 * w, 256);
    }
}

/* Short windows: groups of consecutive windows with similar energy share scalefactors. */
static void group_windows(AacEnc *e, float wm[][AAC_FRAME]) {
    float en[8];
    for (int w = 0; w < 8; w++) {
        en[w] = 1e-3f;
        for (int c = 0; c < e->channels; c++)
            for (int k = 0; k < 128; k++) en[w] += wm[c][128 * w + k] * wm[c][128 * w + k];
    }
    e->ngroups = 1;
    e->glen[0] = 1;
    for (int w = 1; w < 8; w++) {
        float r = en[w] > en[w - 1] ? en[w] / en[w - 1] : en[w - 1] / en[w];
        if (r > 8.0f) e->glen[e->ngroups++] = 1;
        else e->glen[e->ngroups - 1]++;
    }
}

static void build_units(AacEnc *e, float wm[][AAC_FRAME]) {
    e->nunits = 0;
    if (e->seq != EIGHT_SHORT) {
        e->ngroups = 1;
        e->glen[0] = 1;
        e->per_group = e->lim_l;
        for (int b = 0; b < e->lim_l; b++)
            e->unit[e->nunits++] = (Unit){e->swb_l[b], e->swb_l[b + 1] - e->swb_l[b], b, 0};
        for (int c = 0; c < e->channels; c++) memcpy(e->ch[c].spec, wm[c], sizeof(float) * AAC_FRAME);
        return;
    }
    e->per_group = e->lim_s;
    int pos = 0, w0 = 0;
    for (int g = 0; g < e->ngroups; g++) {
        for (int b = 0; b < e->lim_s; b++) {
            int bw = e->swb_s[b + 1] - e->swb_s[b];
            e->unit[e->nunits++] = (Unit){pos, bw * e->glen[g], b, g};
            for (int c = 0; c < e->channels; c++)
                for (int w = w0; w < w0 + e->glen[g]; w++)
                    memcpy(e->ch[c].spec + pos + (w - w0) * bw, wm[c] + 128 * w + e->swb_s[b], sizeof(float) * (size_t)bw);
            pos += bw * e->glen[g];
        }
        w0 += e->glen[g];
    }
    for (int c = 0; c < e->channels; c++)
        if (pos < AAC_FRAME) memset(e->ch[c].spec + pos, 0, sizeof(float) * (size_t)(AAC_FRAME - pos));
}

/* Psychoacoustics, long window: the noise each band may carry. */
static void psy_long(AacEnc *e, Chan *ch, const float *x) {
    int nb = e->nswb_l;
    float en[64], ton[64], dens = 0;
    for (int b = 0; b < nb; b++) {
        int s = e->swb_l[b], t = e->swb_l[b + 1];
        float sum = 0;
        for (int k = s; k < t; k++) sum += x[k] * x[k];
        en[b] = sum;
        if (sum / (float)(t - s) > dens) dens = sum / (float)(t - s);
        /* tonality: how much of the energy around the band (8 bins either side) sits in
           its three strongest bins - a tone smears over 2-4 MDCT bins, noise over all */
        int a = s - 8 < 0 ? 0 : s - 8, z = t + 8 > AAC_FRAME ? AAC_FRAME : t + 8;
        float m1 = 0, m2 = 0, m3 = 0, tot = 0;
        for (int k = a; k < z; k++) {
            float p = x[k] * x[k];
            tot += p;
            if (p > m1) m3 = m2, m2 = m1, m1 = p;
            else if (p > m2) m3 = m2, m2 = p;
            else if (p > m3) m3 = p;
        }
        float r3 = tot > 0 ? (m1 + m2 + m3) / tot : 0.0f;
        ton[b] = r3 <= 0.4f ? 0.0f : r3 >= 0.85f ? 1.0f : (r3 - 0.4f) / 0.45f;
    }
    float floor_dens = dens * 3.1622777e-8f; /* 75 dB below the loudest band: never worth bits */
    for (int b = 0; b < nb; b++) {
        float es = 0;
        const float *sp = e->spread_l + b * 64;
        for (int j = 0; j < nb; j++) es += en[j] * sp[j];
        float tonal = 18.0f + e->bark_l[b];
        if (tonal > 30.0f) tonal = 30.0f;
        float off = ton[b] * tonal + (1.0f - ton[b]) * 6.0f;
        float thr = es * expf(-off * 0.2302585f); /* 10^(-off/10) */
        if (thr < e->ath_l[b]) thr = e->ath_l[b];
        float fl = floor_dens * (float)(e->swb_l[b + 1] - e->swb_l[b]);
        if (thr < fl) thr = fl;
        if (ch->have_prev && thr > 2.0f * ch->thr_prev[b]) thr = 2.0f * ch->thr_prev[b]; /* pre-echo */
        ch->thr_prev[b] = thr;
        if (b < e->lim_l) {
            ch->en[b] = en[b];
            ch->thr[b] = thr;
        }
    }
    ch->have_prev = true;
}

/* Psychoacoustics, eight short windows: noise-like masking (8 dB) per window; a group
   may carry glen times the smallest allowance of its windows. */
static void psy_short(AacEnc *e, Chan *ch, const float *wm) {
    int nb = e->nswb_s;
    float en[8][16], thr[8][16], dens = 0;
    for (int w = 0; w < 8; w++)
        for (int b = 0; b < nb; b++) {
            float sum = 0;
            for (int k = e->swb_s[b]; k < e->swb_s[b + 1]; k++) sum += wm[128 * w + k] * wm[128 * w + k];
            en[w][b] = sum;
            if (sum / (float)(e->swb_s[b + 1] - e->swb_s[b]) > dens) dens = sum / (float)(e->swb_s[b + 1] - e->swb_s[b]);
        }
    float floor_dens = dens * 3.1622777e-8f; /* 75 dB below the loudest band of the frame */
    for (int w = 0; w < 8; w++)
        for (int b = 0; b < nb; b++) {
            float es = 0;
            for (int j = 0; j < nb; j++) es += en[w][j] * e->spread_s[b * 16 + j];
            /* tonality as for long windows (three strongest bins of the band +-2) */
            const float *x = wm + 128 * w;
            int a = e->swb_s[b] - 2 < 0 ? 0 : e->swb_s[b] - 2, z = e->swb_s[b + 1] + 2 > 128 ? 128 : e->swb_s[b + 1] + 2;
            float m1 = 0, m2 = 0, m3 = 0, tot = 0;
            for (int k = a; k < z; k++) {
                float p = x[k] * x[k];
                tot += p;
                if (p > m1) m3 = m2, m2 = m1, m1 = p;
                else if (p > m2) m3 = m2, m2 = p;
                else if (p > m3) m3 = p;
            }
            float r3 = tot > 0 ? (m1 + m2 + m3) / tot : 0.0f, ton = r3 <= 0.5f ? 0.0f : r3 >= 0.9f ? 1.0f : (r3 - 0.5f) / 0.4f;
            float tonal = 18.0f + e->bark_s[b];
            if (tonal > 30.0f) tonal = 30.0f;
            float t = es * expf(-(ton * tonal + (1.0f - ton) * 8.0f) * 0.2302585f);
            float fl = floor_dens * (float)(e->swb_s[b + 1] - e->swb_s[b]);
            if (t < fl) t = fl;
            thr[w][b] = t > e->ath_s[b] ? t : e->ath_s[b];
        }
    int u = 0, w0 = 0;
    for (int g = 0; g < e->ngroups; g++) {
        for (int b = 0; b < e->lim_s; b++, u++) {
            float sum = 0, mn = 1e30f;
            for (int w = w0; w < w0 + e->glen[g]; w++) {
                sum += en[w][b];
                if (thr[w][b] < mn) mn = thr[w][b];
            }
            ch->en[u] = sum;
            ch->thr[u] = mn * (float)e->glen[g];
        }
        w0 += e->glen[g];
    }
    ch->have_prev = false; /* no pre-echo limit right after short windows */
}

static float pe(float en, float thr, int width) { return en > thr ? 0.5f * (float)width * log2f(en / thr) : 0.0f; }

/* Mid/side per band where it needs fewer bits. Both M and S may carry half the stricter
   L/R allowance (noise in L and R is then N_M + N_S <= min). When S has content of its
   own (matrixed surround: S is the surround channel a Pro Logic decoder steers to the
   back), it is also held to 12 dB SNR in either coding, so it survives. */
static void decide_ms(AacEnc *e) {
    Chan *L = &e->ch[0], *R = &e->ch[1];
    const float guard = 0.0630957f; /* 10^(-1.2) */
    for (int u = 0; u < e->nunits; u++) {
        const Unit *un = &e->unit[u];
        float *l = L->spec + un->start, *r = R->spec + un->start;
        float em = 0, es = 0;
        for (int k = 0; k < un->width; k++) {
            float m = 0.5f * (l[k] + r[k]), s = 0.5f * (l[k] - r[k]);
            em += m * m;
            es += s * s;
        }
        float el = L->en[u], er = R->en[u], tl = L->thr[u], tr = R->thr[u];
        bool protect = es > 1e-4f * (el + er);
        float tlg = tl, trg = tr, tm = 0.5f * (tl < tr ? tl : tr), ts = tm;
        if (protect) {
            float g = 2.0f * es * guard;
            if (tlg > g) tlg = g;
            if (trg > g) trg = g;
            if (ts > es * guard) ts = es * guard;
        }
        float pe_lr = pe(el, tlg, un->width) + pe(er, trg, un->width);
        float pe_ms = pe(em, tm, un->width) + pe(es, ts, un->width);
        e->ms[u] = pe_ms < pe_lr;
        if (e->ms[u]) {
            for (int k = 0; k < un->width; k++) {
                float m = 0.5f * (l[k] + r[k]), s = 0.5f * (l[k] - r[k]);
                l[k] = m;
                r[k] = s;
            }
            L->en[u] = em;
            R->en[u] = es;
            L->thr[u] = tm;
            R->thr[u] = ts;
        } else {
            L->thr[u] = tlg;
            R->thr[u] = trg;
        }
    }
}

/* Per band: |x|^(3/4), the scalefactor that would put the noise at the allowance (noise of
   the 3/4-power quantiser ~ 4/27 step^1.5 sum sqrt|x|) and the floor that keeps |q| <= 8191. */
static void prepare(AacEnc *e, Chan *ch) {
    for (int u = 0; u < e->nunits; u++) {
        const Unit *un = &e->unit[u];
        float sum_sqrt = 0, mx = 0;
        for (int k = un->start; k < un->start + un->width; k++) {
            float a = fabsf(ch->spec[k]), r = sqrtf(a);
            ch->x34[k] = sqrtf(a * r);
            sum_sqrt += r;
            if (a > mx) mx = a;
        }
        if (mx <= 0 || ch->en[u] <= ch->thr[u]) {
            ch->sf0[u] = -1000.0f; /* below the allowance: leave it out */
            ch->sfmin[u] = 0;
            continue;
        }
        ch->sf0[u] = SF_OFFSET + (8.0f / 3.0f) * log2f(6.75f * ch->thr[u] / sum_sqrt);
        int lo = (int)ceilf(SF_OFFSET + 4.0f * log2f(mx) - 69.33f);
        ch->sfmin[u] = lo < 0 ? 0 : lo > 255 ? 255 : lo;
    }
}

/* --------------------------------------------------------- rate control */

static void quantize_unit(const AacEnc *e, Chan *ch, int u) {
    const Unit *un = &e->unit[u];
    float sc = e->qscale[ch->sf[u]];
    int mx = 0;
    for (int k = un->start; k < un->start + un->width; k++) {
        float qf = ch->x34[k] * sc + 0.4054f;
        int q = qf >= (float)MAX_Q ? MAX_Q : (int)qf; /* clamp before converting: never overflows */
        if (q > mx) mx = q;
        ch->q[k] = (int16_t)(ch->spec[k] < 0 ? -q : q);
    }
    ch->maxq[u] = mx;
}

/* Scalefactors and quantised values at global offset delta; neighbours stay within the
   +-60 the scalefactor code can express. */
static void quantize_channel(AacEnc *e, Chan *ch, int delta) {
    int last = -1;
    for (int u = 0; u < e->nunits; u++) {
        const Unit *un = &e->unit[u];
        if (ch->sf0[u] < -999.0f) {
            ch->maxq[u] = 0;
            memset(ch->q + un->start, 0, sizeof(int16_t) * (size_t)un->width);
            continue;
        }
        int sf = (int)lrintf(ch->sf0[u]) + delta;
        if (sf < ch->sfmin[u]) sf = ch->sfmin[u];
        if (sf > 255) sf = 255;
        if (last >= 0 && sf > last + 60) sf = last + 60;
        if (last >= 0 && sf < last - 60) sf = last - 60;
        ch->sf[u] = sf;
        quantize_unit(e, ch, u);
        if (ch->maxq[u]) last = sf;
    }
}

static int candidate_lo(int maxq) {
    return maxq == 0 ? 0 : maxq <= 1 ? 1 : maxq <= 2 ? 3 : maxq <= 4 ? 5 : maxq <= 7 ? 7 : maxq <= 12 ? 9 : 11;
}

/* Codebooks for one channel by a trellis over the bands of each group (a section header
   costs 4 + 5 bits long, 4 + 3 short); returns the channel's bits after global_gain. */
static int code_channel(AacEnc *e, Chan *ch) {
    bool sh = e->seq == EIGHT_SHORT;
    int sect_bits = sh ? 3 : 5, esc = sh ? 7 : 31, hdr = 4 + sect_bits, nb = e->max_sfb;
    for (int g = 0; g < e->ngroups; g++) {
        for (int i = 0; i < nb; i++) {
            int u = g * e->per_group + i, mq = ch->maxq[u];
            const Unit *un = &e->unit[u];
            for (int cb = 0; cb < NCB; cb++) ch->cost[u][cb] = INF_BITS;
            if (mq == 0) {
                ch->cost[u][0] = 0;
                for (int cb = 1; cb < NCB; cb++) /* inside a section: zeros + a 1-bit "same scalefactor" */
                    ch->cost[u][cb] = books[cb].len[zero_index[cb]] * (un->width / books[cb].dim) + 1;
                continue;
            }
            int lo = candidate_lo(mq);
            for (int cb = lo; cb < NCB && cb < lo + (lo >= 9 ? 3 : 4); cb++)
                ch->cost[u][cb] = unit_bits(ch->q + un->start, un->width, cb);
            if (ch->cost[u][11] == INF_BITS) ch->cost[u][11] = unit_bits(ch->q + un->start, un->width, 11);
        }
    }
    int bits = 0;
    for (int g = 0; g < e->ngroups; g++) {
        int best[64][NCB];
        uint8_t from[64][NCB];
        for (int i = 0; i < nb; i++) {
            int u = g * e->per_group + i, pmin = INF_BITS, parg = 0;
            if (i > 0)
                for (int cb = 0; cb < NCB; cb++)
                    if (best[i - 1][cb] < pmin) pmin = best[i - 1][cb], parg = cb;
            for (int cb = 0; cb < NCB; cb++) {
                int c = ch->cost[u][cb];
                if (c >= INF_BITS) {
                    best[i][cb] = INF_BITS;
                    continue;
                }
                if (i == 0) {
                    best[i][cb] = c + hdr;
                    from[i][cb] = (uint8_t)cb;
                } else {
                    int stay = best[i - 1][cb], sw = pmin + hdr;
                    if (stay <= sw) {
                        best[i][cb] = c + stay;
                        from[i][cb] = (uint8_t)cb;
                    } else {
                        best[i][cb] = c + sw;
                        from[i][cb] = (uint8_t)parg;
                    }
                }
            }
        }
        if (nb > 0) {
            int cb = 0, mn = INF_BITS;
            for (int c = 0; c < NCB; c++)
                if (best[nb - 1][c] < mn) mn = best[nb - 1][c], cb = c;
            for (int i = nb - 1; i >= 0; i--) {
                ch->cb[g * e->per_group + i] = (uint8_t)cb;
                cb = from[i][cb];
            }
        }
        /* exact section bits */
        for (int i = 0; i < nb;) {
            int j = i;
            while (j < nb && ch->cb[g * e->per_group + j] == ch->cb[g * e->per_group + i]) j++;
            bits += 4 + sect_bits * ((j - i) / esc + 1);
            i = j;
        }
    }
    /* scalefactors (silent bands inside a section repeat the last one, or take the first
       real band's before it: either way a 1-bit "no change") + spectral bits */
    int last = -1, lead = SF_OFFSET;
    for (int u = 0; u < e->nunits; u++)
        if (ch->maxq[u] && e->unit[u].band < nb) {
            lead = ch->sf[u];
            break;
        }
    ch->global_gain = lead;
    for (int g = 0; g < e->ngroups; g++)
        for (int i = 0; i < nb; i++) {
            int u = g * e->per_group + i;
            if (!ch->cb[u]) continue;
            if (ch->maxq[u] == 0) ch->sf[u] = last >= 0 ? last : lead;
            if (last < 0) ch->global_gain = last = ch->sf[u];
            bits += sf_len[ch->sf[u] - last + 60];
            last = ch->sf[u];
            bits += ch->cost[u][ch->cb[u]] - (ch->maxq[u] == 0); /* the 1 sf bit is counted above */
        }
    return bits + 3; /* pulse, tns, gain control flags */
}

static int ms_mode(const AacEnc *e) {
    int on = 0, n = 0;
    for (int g = 0; g < e->ngroups; g++)
        for (int i = 0; i < e->max_sfb; i++, n++) on += e->ms[g * e->per_group + i];
    return on == 0 ? 0 : on == n ? 2 : 1;
}

static int frame_bits(AacEnc *e, int delta) {
    int mx = 0;
    for (int c = 0; c < e->channels; c++) {
        quantize_channel(e, &e->ch[c], delta);
        for (int g = 0; g < e->ngroups; g++)
            for (int i = 0; i < e->per_group; i++)
                if (e->ch[c].maxq[g * e->per_group + i] && i + 1 > mx) mx = i + 1;
    }
    e->max_sfb = mx;
    int ics_info = e->seq == EIGHT_SHORT ? 15 : 11;
    int bits = 3 + 4 + 3; /* element id + tag, END */
    if (e->channels == 2) {
        e->ms_mode = ms_mode(e);
        bits += 1 + ics_info + 2 + (e->ms_mode == 1 ? e->ngroups * e->max_sfb : 0);
    } else {
        bits += ics_info;
    }
    for (int c = 0; c < e->channels; c++) bits += 8 + code_channel(e, &e->ch[c]);
    return (bits + 7) & ~7;
}

/* ---------------------------------------------------------------- writing */

static void put_ics_info(const AacEnc *e, BitW *w) {
    put(w, 0, 1);
    put(w, (uint32_t)e->seq, 2);
    put(w, (uint32_t)e->shape, 1);
    if (e->seq == EIGHT_SHORT) {
        put(w, (uint32_t)e->max_sfb, 4);
        unsigned grouping = 0;
        int w0 = 0;
        for (int g = 0; g < e->ngroups; g++) {
            for (int k = 1; k < e->glen[g]; k++) grouping |= 1u << (6 - (w0 + k - 1)); /* window w0+k joins */
            w0 += e->glen[g];
        }
        put(w, grouping, 7);
    } else {
        put(w, (uint32_t)e->max_sfb, 6);
        put(w, 0, 1); /* predictor_data_present */
    }
}

static void put_ics(const AacEnc *e, const Chan *ch, BitW *w, bool common) {
    bool sh = e->seq == EIGHT_SHORT;
    int sect_bits = sh ? 3 : 5, esc = sh ? 7 : 31, nb = e->max_sfb;
    put(w, (uint32_t)ch->global_gain, 8);
    if (!common) put_ics_info(e, w);
    for (int g = 0; g < e->ngroups; g++) /* section data */
        for (int i = 0; i < nb;) {
            int cb = ch->cb[g * e->per_group + i], j = i;
            while (j < nb && ch->cb[g * e->per_group + j] == cb) j++;
            put(w, (uint32_t)cb, 4);
            int len = j - i;
            while (len >= esc) {
                put(w, (uint32_t)esc, sect_bits);
                len -= esc;
            }
            put(w, (uint32_t)len, sect_bits);
            i = j;
        }
    int last = ch->global_gain; /* scalefactor data */
    for (int g = 0; g < e->ngroups; g++)
        for (int i = 0; i < nb; i++) {
            int u = g * e->per_group + i;
            if (!ch->cb[u]) continue;
            int d = ch->sf[u] - last + 60;
            put(w, sf_code[d], sf_len[d]);
            last = ch->sf[u];
        }
    put(w, 0, 3); /* pulse_data_present, tns_data_present, gain_control_data_present */
    for (int g = 0; g < e->ngroups; g++) /* spectral data */
        for (int i = 0; i < nb; i++) {
            int u = g * e->per_group + i;
            if (ch->cb[u]) put_unit(w, ch->q + e->unit[u].start, e->unit[u].width, ch->cb[u]);
        }
}

/* ------------------------------------------------------------------ frame */

static int encode_frame(AacEnc *e, uint8_t *out, int cap) {
    static const int lo_delta = -16, hi_delta = 120;
    float wm[AAC_MAX_CHANNELS][AAC_FRAME];
    e->seq = choose_sequence(e);
    for (int c = 0; c < e->channels; c++) transform(e, c, wm[c]);
    if (e->seq == EIGHT_SHORT) group_windows(e, wm);
    build_units(e, wm);
    for (int c = 0; c < e->channels; c++) {
        if (e->seq == EIGHT_SHORT) psy_short(e, &e->ch[c], wm[c]);
        else psy_long(e, &e->ch[c], wm[c]);
    }
    memset(e->ms, 0, sizeof e->ms);
    if (e->channels == 2) decide_ms(e);
    for (int c = 0; c < e->channels; c++) prepare(e, &e->ch[c]);

    /* the budget: the mean plus up to half the reservoir, never past the decoder's buffer */
    int budget = e->mean_bits + e->reservoir / 2;
    if (budget > e->max_frame_bits) budget = e->max_frame_bits;
    /* smallest offset that fits, searched around the last frame's */
    int d = e->last_delta, bits = frame_bits(e, d);
    if (bits <= budget) {
        while (d > lo_delta) {
            int b = frame_bits(e, d - 2);
            if (b > budget) break;
            d -= 2;
            bits = b;
        }
        if (d > lo_delta && frame_bits(e, d - 1) <= budget) d -= 1;
    } else {
        int step = 2;
        while (d < hi_delta && bits > budget) {
            d = d + step > hi_delta ? hi_delta : d + step;
            bits = frame_bits(e, d);
            step = step < 16 ? step * 2 : 16;
        }
        while (d > lo_delta) { /* back off the overshoot */
            if (frame_bits(e, d - 1) > budget) break;
            d--;
        }
    }
    bits = frame_bits(e, d);
    while (bits > budget && e->nunits > 0) { /* only for pathological input: drop the top band */
        int top = e->max_sfb - 1;
        if (top < 0) break;
        for (int c = 0; c < e->channels; c++)
            for (int g = 0; g < e->ngroups; g++) e->ch[c].sf0[g * e->per_group + top] = -1000.0f;
        bits = frame_bits(e, d);
    }
    e->last_delta = d;

    if (cap < (bits >> 3)) return -1;
    memset(out, 0, (size_t)(bits >> 3));
    BitW w = {out, 0, cap * 8};
    if (e->channels == 1) {
        put(&w, ID_SCE, 3);
        put(&w, 0, 4);
        put_ics(e, &e->ch[0], &w, false);
    } else {
        put(&w, ID_CPE, 3);
        put(&w, 0, 4);
        put(&w, 1, 1); /* common_window */
        put_ics_info(e, &w);
        put(&w, (uint32_t)e->ms_mode, 2);
        if (e->ms_mode == 1)
            for (int g = 0; g < e->ngroups; g++)
                for (int i = 0; i < e->max_sfb; i++) put(&w, e->ms[g * e->per_group + i], 1);
        put_ics(e, &e->ch[0], &w, true);
        put_ics(e, &e->ch[1], &w, true);
    }
    put(&w, ID_END, 3);
    int bytes = (w.pos + 7) >> 3;
    if (bytes * 8 != bits || bytes > cap) return -1; /* the count and the writer must agree */

    e->reservoir += e->mean_bits - bits;
    if (e->reservoir > e->reservoir_max) e->reservoir = e->reservoir_max;
    if (e->reservoir < 0) e->reservoir = 0;
    e->prev_seq = e->seq;
    e->st.frames++;
    e->st.short_frames += e->seq == EIGHT_SHORT;
    if (e->channels == 2)
        for (int g = 0; g < e->ngroups; g++)
            for (int i = 0; i < e->max_sfb; i++) e->st.ms_bands += e->ms[g * e->per_group + i];
    if (bits < e->st.bits_min) e->st.bits_min = bits;
    if (bits > e->st.bits_max) e->st.bits_max = bits;
    e->st.bits_total += bits;
    e->st.reservoir = e->reservoir;
    return bytes;
}

/* Take one block (NULL = silence), then encode the frame it completes, if any. */
static int push(AacEnc *e, const float *pcm, uint8_t *out, int cap) {
    uint8_t mask = 0;
    for (int c = 0; c < e->channels; c++) {
        float *b = e->in[c];
        memmove(b, b + AAC_FRAME, sizeof(float) * 2 * AAC_FRAME);
        float *nb = b + 2 * AAC_FRAME;
        for (int i = 0; i < AAC_FRAME; i++) {
            float v = pcm ? pcm[i * e->channels + c] : 0.0f;
            if (!(v == v) || v > 1e30f || v < -1e30f) v = 0.0f; /* NaN, +-Inf */
            if (v > 1.5f) v = 1.5f;
            if (v < -1.5f) v = -1.5f;
            nb[i] = v * 32768.0f;
        }
        mask |= find_attacks(e, c, nb);
    }
    e->attack[0] = e->attack[1];
    e->attack[1] = e->attack[2];
    e->attack[2] = mask;
    e->pushed++;
    if (e->pushed < 2) return 0;
    int n = encode_frame(e, out, cap);
    if (n > 0) e->units_out++;
    return n;
}

int aac_encode(AacEnc *e, const float *pcm, uint8_t *out, int cap) {
    if (!e || !pcm || !out) return -1;
    e->blocks++;
    return push(e, pcm, out, cap);
}

int aac_flush(AacEnc *e, uint8_t *out, int cap) {
    if (!e || !out) return -1;
    while (e->units_out < e->blocks + 1) {
        int n = push(e, NULL, out, cap);
        if (n != 0) return n;
    }
    return 0;
}
