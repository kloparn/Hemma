/* modules/av/ac3.c — see ac3.h. Section and table numbers refer to ATSC A/52. */
#include "ac3.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NFULL 5 /* most full-bandwidth channels */
#define CPL 5   /* the coupling channel's slot in per-channel arrays */
#define LFE 6   /* the LFE channel's slot */
#define NSLOTS 7
#define NBANDS 50
#define PI 3.14159265358979323846
#define SQRT1_2 0.70710678f

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ================================================================== tables */

static const uint16_t KBPS[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
static const int SAMPLE_RATE[3] = {48000, 44100, 32000};
static const uint8_t NFCHANS[8] = {2, 1, 2, 3, 3, 4, 4, 5};
static const float CMIXLEV[4] = {0.70710678f, 0.59460356f, 0.5f, 0.59460356f}; /* 3 = reserved: -4.5 dB */
static const float SURMIXLEV[4] = {0.70710678f, 0.5f, 0.0f, 0.5f};            /* 3 = reserved: -6 dB */

static const uint16_t CRC16[256] = {
    0x0000, 0x8005, 0x800f, 0x000a, 0x801b, 0x001e, 0x0014, 0x8011, 0x8033, 0x0036, 0x003c, 0x8039, 0x0028, 0x802d,
    0x8027, 0x0022, 0x8063, 0x0066, 0x006c, 0x8069, 0x0078, 0x807d, 0x8077, 0x0072, 0x0050, 0x8055, 0x805f, 0x005a,
    0x804b, 0x004e, 0x0044, 0x8041, 0x80c3, 0x00c6, 0x00cc, 0x80c9, 0x00d8, 0x80dd, 0x80d7, 0x00d2, 0x00f0, 0x80f5,
    0x80ff, 0x00fa, 0x80eb, 0x00ee, 0x00e4, 0x80e1, 0x00a0, 0x80a5, 0x80af, 0x00aa, 0x80bb, 0x00be, 0x00b4, 0x80b1,
    0x8093, 0x0096, 0x009c, 0x8099, 0x0088, 0x808d, 0x8087, 0x0082, 0x8183, 0x0186, 0x018c, 0x8189, 0x0198, 0x819d,
    0x8197, 0x0192, 0x01b0, 0x81b5, 0x81bf, 0x01ba, 0x81ab, 0x01ae, 0x01a4, 0x81a1, 0x01e0, 0x81e5, 0x81ef, 0x01ea,
    0x81fb, 0x01fe, 0x01f4, 0x81f1, 0x81d3, 0x01d6, 0x01dc, 0x81d9, 0x01c8, 0x81cd, 0x81c7, 0x01c2, 0x0140, 0x8145,
    0x814f, 0x014a, 0x815b, 0x015e, 0x0154, 0x8151, 0x8173, 0x0176, 0x017c, 0x8179, 0x0168, 0x816d, 0x8167, 0x0162,
    0x8123, 0x0126, 0x012c, 0x8129, 0x0138, 0x813d, 0x8137, 0x0132, 0x0110, 0x8115, 0x811f, 0x011a, 0x810b, 0x010e,
    0x0104, 0x8101, 0x8303, 0x0306, 0x030c, 0x8309, 0x0318, 0x831d, 0x8317, 0x0312, 0x0330, 0x8335, 0x833f, 0x033a,
    0x832b, 0x032e, 0x0324, 0x8321, 0x0360, 0x8365, 0x836f, 0x036a, 0x837b, 0x037e, 0x0374, 0x8371, 0x8353, 0x0356,
    0x035c, 0x8359, 0x0348, 0x834d, 0x8347, 0x0342, 0x03c0, 0x83c5, 0x83cf, 0x03ca, 0x83db, 0x03de, 0x03d4, 0x83d1,
    0x83f3, 0x03f6, 0x03fc, 0x83f9, 0x03e8, 0x83ed, 0x83e7, 0x03e2, 0x83a3, 0x03a6, 0x03ac, 0x83a9, 0x03b8, 0x83bd,
    0x83b7, 0x03b2, 0x0390, 0x8395, 0x839f, 0x039a, 0x838b, 0x038e, 0x0384, 0x8381, 0x0280, 0x8285, 0x828f, 0x028a,
    0x829b, 0x029e, 0x0294, 0x8291, 0x82b3, 0x02b6, 0x02bc, 0x82b9, 0x02a8, 0x82ad, 0x82a7, 0x02a2, 0x82e3, 0x02e6,
    0x02ec, 0x82e9, 0x02f8, 0x82fd, 0x82f7, 0x02f2, 0x02d0, 0x82d5, 0x82df, 0x02da, 0x82cb, 0x02ce, 0x02c4, 0x82c1,
    0x8243, 0x0246, 0x024c, 0x8249, 0x0258, 0x825d, 0x8257, 0x0252, 0x0270, 0x8275, 0x827f, 0x027a, 0x826b, 0x026e,
    0x0264, 0x8261, 0x0220, 0x8225, 0x822f, 0x022a, 0x823b, 0x023e, 0x0234, 0x8231, 0x8213, 0x0216, 0x021c, 0x8219,
    0x0208, 0x820d, 0x8207, 0x0202};

/* ---- bit allocation (7.2.2) ---- */
static const uint8_t SLOWDEC[4] = {0x0f, 0x11, 0x13, 0x15};        /* Table 7.6 */
static const uint8_t FASTDEC[4] = {0x3f, 0x53, 0x67, 0x7b};        /* Table 7.7 */
static const uint16_t SLOWGAIN[4] = {0x540, 0x4d8, 0x478, 0x410};  /* Table 7.8 */
static const uint16_t DBPBTAB[4] = {0x000, 0x700, 0x900, 0xb00};   /* Table 7.9 */
static const int16_t FLOORTAB[8] = {0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, -0x800}; /* Table 7.10 */
static const uint16_t FASTGAIN[8] = {0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400}; /* Table 7.11 */

/* first bin of each bit allocation band, plus the end (Table 7.13) */
static const uint8_t BNDTAB[NBANDS + 1] = {0,  1,  2,  3,  4,  5,  6,  7,  8,   9,   10,  11,  12,  13,  14,  15,  16,
                                           17, 18, 19, 20, 21, 22, 23, 24, 25,  26,  27,  28,  31,  34,  37,  40,  43,
                                           46, 49, 55, 61, 67, 73, 79, 85, 97, 109, 121, 133, 157, 181, 205, 229, 253};

static const uint8_t LATAB[256] = { /* log-addition (Table 7.14) */
    0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x34, 0x33, 0x32,
    0x31, 0x30, 0x2f, 0x2f, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x29, 0x29, 0x28, 0x27, 0x26, 0x26,
    0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x21, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c,
    0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x15,
    0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f,
    0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint16_t HTH[NBANDS][3] = { /* hearing threshold per band, columns fscod 48/44.1/32 kHz (Table 7.15) */
    {0x04d0, 0x04f0, 0x0580}, {0x04d0, 0x04f0, 0x0580}, {0x0440, 0x0460, 0x04b0}, {0x0400, 0x0410, 0x0450},
    {0x03e0, 0x03e0, 0x0420}, {0x03c0, 0x03d0, 0x03f0}, {0x03b0, 0x03c0, 0x03e0}, {0x03b0, 0x03b0, 0x03d0},
    {0x03a0, 0x03b0, 0x03c0}, {0x03a0, 0x03a0, 0x03b0}, {0x03a0, 0x03a0, 0x03b0}, {0x03a0, 0x03a0, 0x03b0},
    {0x03a0, 0x03a0, 0x03a0}, {0x0390, 0x03a0, 0x03a0}, {0x0390, 0x0390, 0x03a0}, {0x0390, 0x0390, 0x03a0},
    {0x0380, 0x0390, 0x03a0}, {0x0380, 0x0380, 0x03a0}, {0x0370, 0x0380, 0x03a0}, {0x0370, 0x0380, 0x03a0},
    {0x0360, 0x0370, 0x0390}, {0x0360, 0x0370, 0x0390}, {0x0350, 0x0360, 0x0390}, {0x0350, 0x0360, 0x0380},
    {0x0340, 0x0350, 0x0380}, {0x0340, 0x0340, 0x0380}, {0x0330, 0x0340, 0x0380}, {0x0320, 0x0340, 0x0370},
    {0x0310, 0x0320, 0x0360}, {0x0300, 0x0310, 0x0350}, {0x02f0, 0x0300, 0x0340}, {0x02f0, 0x02f0, 0x0330},
    {0x02f0, 0x02f0, 0x0320}, {0x02f0, 0x02f0, 0x0310}, {0x0300, 0x02f0, 0x0300}, {0x0310, 0x0300, 0x02f0},
    {0x0340, 0x0320, 0x02f0}, {0x0390, 0x0350, 0x02f0}, {0x03e0, 0x0390, 0x0300}, {0x0420, 0x03e0, 0x0310},
    {0x0460, 0x0420, 0x0330}, {0x0490, 0x0450, 0x0350}, {0x04a0, 0x04a0, 0x03c0}, {0x0460, 0x0490, 0x0410},
    {0x0440, 0x0460, 0x0470}, {0x0440, 0x0440, 0x04a0}, {0x0520, 0x0480, 0x0460}, {0x0800, 0x0630, 0x0440},
    {0x0840, 0x0840, 0x0450}, {0x0840, 0x0840, 0x04e0}};

static const uint8_t BAPTAB[64] = { /* Table 7.16 */
    0,  1,  1,  1,  1,  1,  2,  2,  3,  3,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,
    7,  8,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13,
    13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15};

/* ---- quantisation (7.3) ---- */
#define SYM(m, levels) ((2.0f * (m) - ((levels)-1)) / (levels)) /* symmetric quantiser level m */
/* sized for every code the bitstream can carry; out-of-range codes extend the formula like other decoders */
static const float Q1[4] = {SYM(0, 3), SYM(1, 3), SYM(2, 3), SYM(3, 3)};
static const float Q2[6] = {SYM(0, 5), SYM(1, 5), SYM(2, 5), SYM(3, 5), SYM(4, 5), SYM(5, 5)};
static const float Q3[8] = {SYM(0, 7), SYM(1, 7), SYM(2, 7), SYM(3, 7), SYM(4, 7), SYM(5, 7), SYM(6, 7), SYM(7, 7)};
static const float Q4[12] = {SYM(0, 11), SYM(1, 11), SYM(2, 11), SYM(3, 11), SYM(4, 11),  SYM(5, 11),
                             SYM(6, 11), SYM(7, 11), SYM(8, 11), SYM(9, 11), SYM(10, 11), SYM(11, 11)};
static const float Q5[16] = {SYM(0, 15),  SYM(1, 15),  SYM(2, 15),  SYM(3, 15), SYM(4, 15),  SYM(5, 15),
                             SYM(6, 15),  SYM(7, 15),  SYM(8, 15),  SYM(9, 15), SYM(10, 15), SYM(11, 15),
                             SYM(12, 15), SYM(13, 15), SYM(14, 15), SYM(15, 15)};
static const uint8_t QBITS[16] = {0, 0, 0, 0, 0, 0, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16}; /* bap 6..15 (Table 7.18) */

#define P2(n) (1.0f / (float)(1L << (n)))
static const float EXPSCALE[25] = {P2(0),  P2(1),  P2(2),  P2(3),  P2(4),  P2(5),  P2(6),  P2(7),  P2(8),
                                   P2(9),  P2(10), P2(11), P2(12), P2(13), P2(14), P2(15), P2(16), P2(17),
                                   P2(18), P2(19), P2(20), P2(21), P2(22), P2(23), P2(24)};

static const uint8_t REMATBND[5] = {13, 25, 37, 61, 253}; /* rematrixing bands (7.5.2) */

/* ============================================================ bit reading */

typedef struct {
    const uint8_t *p; /* zero-padded by at least 4 bytes past `end` */
    int pos, end;     /* bit positions; pos > end once the data ran out */
} Bits;

static inline unsigned getb(Bits *b, int n) { /* 1 <= n <= 24 */
    if (b->pos + n > b->end) {
        b->pos = b->end + 1;
        return 0;
    }
    const uint8_t *q = b->p + (b->pos >> 3);
    uint32_t v = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
    unsigned r = (unsigned)((v << (b->pos & 7)) >> (32 - n));
    b->pos += n;
    return r;
}

/* ================================================================ framing */

uint16_t ac3_crc16(uint16_t crc, const uint8_t *p, size_t n) {
    while (n--) crc = (uint16_t)((crc << 8) ^ CRC16[((crc >> 8) ^ *p++) & 0xff]);
    return crc;
}

static int frame_words(int fscod, int frmsizecod) {
    int kbps = KBPS[frmsizecod >> 1];
    if (fscod == 0) return kbps * 2;
    if (fscod == 1) return kbps * 320 / 147 + (frmsizecod & 1); /* 44.1 kHz: odd codes carry a padding word */
    return kbps * 3;
}

int ac3_frame_size(const uint8_t *p, size_t n) {
    if (!p || n < 6 || p[0] != 0x0B || p[1] != 0x77) return 0;
    int fscod = p[4] >> 6, frmsizecod = p[4] & 63, bsid = p[5] >> 3;
    if (fscod == 3 || frmsizecod > 37 || bsid > 8) return 0;
    return 2 * frame_words(fscod, frmsizecod);
}

bool ac3_crc_ok(const uint8_t *f, size_t n) {
    int size = ac3_frame_size(f, n);
    if (!size || (size_t)size > n) return false;
    int words = size / 2, f58 = 2 * ((words >> 1) + (words >> 3)); /* first 5/8 of the frame */
    return ac3_crc16(0, f + 2, (size_t)f58 - 2) == 0 && ac3_crc16(0, f + f58, (size_t)(size - f58)) == 0;
}

long ac3_find_frame(const uint8_t *p, size_t n, bool complete) {
    for (size_t i = 0; i + AC3_HEADER_BYTES <= n; i++) {
        if (p[i] != 0x0B || p[i + 1] != 0x77) continue;
        int size = ac3_frame_size(p + i, n - i);
        if (!size) continue;
        if (i + (size_t)size > n) {
            if (complete) continue;
            return (long)i;
        }
        if (ac3_crc_ok(p + i, n - i)) return (long)i;
    }
    return -1;
}

typedef struct {
    int fscod, frmsizecod, bsid, bsmod, acmod, nfchans, lfeon, dialnorm, dsurmod;
    float clev, slev;
} Hdr;

/* syncinfo + bsi (5.3.1, 5.3.2); leaves b after the bsi. With whole = false
   the additional bsi may lie beyond the data (only the fields matter). */
static bool parse_bsi(Bits *b, Hdr *h, bool whole) {
    if (getb(b, 16) != 0x0B77) return false;
    getb(b, 16); /* crc1 */
    h->fscod = (int)getb(b, 2);
    h->frmsizecod = (int)getb(b, 6);
    h->bsid = (int)getb(b, 5);
    if (h->fscod == 3 || h->frmsizecod > 37 || h->bsid > 8) return false;
    h->bsmod = (int)getb(b, 3);
    h->acmod = (int)getb(b, 3);
    h->nfchans = NFCHANS[h->acmod];
    h->clev = h->slev = 0;
    h->dsurmod = 0;
    if ((h->acmod & 1) && h->acmod != 1) h->clev = CMIXLEV[getb(b, 2)];
    if (h->acmod & 4) h->slev = SURMIXLEV[getb(b, 2)];
    if (h->acmod == 2) h->dsurmod = (int)getb(b, 2);
    h->lfeon = (int)getb(b, 1);
    h->dialnorm = (int)getb(b, 5);
    if (!h->dialnorm) h->dialnorm = 31; /* 0 is reserved: treat as -31 dB */
    for (int i = 0; i <= (h->acmod == 0); i++) { /* second set for 1+1 */
        if (i) getb(b, 5);                        /* dialnorm2 */
        if (getb(b, 1)) getb(b, 8);               /* compre, compr */
        if (getb(b, 1)) getb(b, 8);               /* langcode, langcod */
        if (getb(b, 1)) getb(b, 7);               /* audprodie, mixlevel + roomtyp */
    }
    getb(b, 2);                  /* copyrightb, origbs */
    if (getb(b, 1)) getb(b, 14); /* timecod1 / xbsi1 */
    if (getb(b, 1)) getb(b, 14); /* timecod2 / xbsi2 */
    if (getb(b, 1)) {            /* addbsi */
        int len = (int)getb(b, 6) + 1;
        if (b->pos > b->end) return false;
        if (!whole) return true;
        b->pos += 8 * len;
    }
    return b->pos <= b->end;
}

static void fill_info(const Hdr *h, Ac3Info *info) {
    info->sample_rate = SAMPLE_RATE[h->fscod];
    info->bitrate = KBPS[h->frmsizecod >> 1];
    info->frame_bytes = 2 * frame_words(h->fscod, h->frmsizecod);
    info->bsid = h->bsid;
    info->bsmod = h->bsmod;
    info->acmod = h->acmod;
    info->lfe = h->lfeon;
    info->channels = h->nfchans + h->lfeon;
    info->dialnorm = h->dialnorm;
    info->dsurmod = h->dsurmod;
    info->clev = h->clev;
    info->slev = h->slev;
    info->fscod = h->fscod;
    info->frmsizecod = h->frmsizecod;
}

bool ac3_parse_header(const uint8_t *p, size_t n, Ac3Info *info) {
    uint8_t tmp[72] = {0};
    size_t k = n < 64 ? n : 64; /* the fields before the additional bsi fit in ~20 bytes */
    if (!p || k < 6) return false;
    memcpy(tmp, p, k);
    Bits b = {tmp, 0, (int)k * 8};
    Hdr h;
    if (!parse_bsi(&b, &h, false)) return false;
    if (info) fill_info(&h, info);
    return true;
}

/* ============================================================ the decoder */

typedef struct { /* grouped mantissas left over (7.3.5): groups span channels within a block */
    int n1, n2, n4;
    uint8_t q1[2], q2[2], q4;
} Groups;

struct Ac3Dec {
    Ac3Layout layout;
    bool crc_check;
    float drc_scale;
    float drc[256]; /* gain per dynrng code, with drc_scale applied */
    uint32_t seed;  /* dither */
    float window[256], wgain[256];
    float pre_c[2][128], pre_s[2][128], post_c[2][128], post_s[2][128]; /* DCT-IV twiddles: [0] K=256, [1] K=128 */
    float tw_c[64], tw_s[64];                                           /* exp(2 pi i j / 128) */
    uint8_t rev7[128], rev6[64];
    float delay[AC3_MAX_CHANNELS][256];
    int prev_acmod, prev_lfe; /* layout the overlap tails belong to */
    Ac3Stats stats;
    uint8_t buf[AC3_MAX_FRAME + 16];

    /* the frame being decoded */
    Hdr h;
    /* audio block state: values carry over from block to block within one frame */
    bool blksw[NFULL], dithflag[NFULL];
    float dynrng[2];
    bool cplinu, chincpl[NFULL], phsflginu, phsflg[18];
    int cplbegf, cplendf, ncplbnd, cplstrtmant, cplendmant;
    int cplbnd_end[18];
    float cplco[NFULL][18];
    bool cplco_ok[NFULL];
    int nrematbnd;
    bool rematflg[4];
    int endmant[NSLOTS];
    int exp_end[NSLOTS]; /* end bin of the exponents on hand (0 = none yet) */
    int exp_start_cpl;
    bool exp_cpl[NFULL]; /* exponents were decoded while the channel was coupled */
    int8_t exps[NSLOTS][256];
    int sdecay, fdecay, sgain, dbknee, floor, csnroffst, cplfleak, cplsleak;
    int fsnroffst[NSLOTS], fgain[NSLOTS];
    int dba_mode[NSLOTS], dba_nseg[NSLOTS];
    uint8_t dba_offst[NSLOTS][8], dba_len[NSLOTS][8], dba_ba[NSLOTS][8];
    uint8_t bap[NSLOTS][256];
    float coef[NSLOTS][256];
    bool cpl_dith[256]; /* coupling bins carrying dither (bap 0) */
    float pcm[AC3_MAX_CHANNELS][256];
};

static void kbd_window(float *w, int n, double alpha) { /* Kaiser-Bessel derived (7.9.4.1) */
    double cum[256], sum = 0, a2 = (alpha * PI / n) * (alpha * PI / n);
    for (int i = 0; i < n; i++) {
        double t = i * (double)(n - i) * a2, bessel = 1.0; /* I0 by its power series */
        for (int j = 50; j > 0; j--) bessel = bessel * t / ((double)j * j) + 1.0;
        sum += bessel;
        cum[i] = sum;
    }
    sum += 1.0;
    for (int i = 0; i < n; i++) w[i] = (float)sqrt(cum[i] / sum);
}

static unsigned bitrev(unsigned v, int bits) {
    unsigned r = 0;
    for (int i = 0; i < bits; i++) r = r << 1 | ((v >> i) & 1);
    return r;
}

Ac3Dec *ac3_open(void) {
    Ac3Dec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->layout = AC3_LAYOUT_NATIVE;
    d->crc_check = true;
    d->seed = 0x2545F491u;
    d->prev_acmod = -1;
    ac3_set_drc(d, 1.0f);
    kbd_window(d->window, 256, 5.0);
    for (int n = 0; n < 256; n++) d->wgain[n] = -2.0f * d->window[n]; /* gain for perfect reconstruction */
    for (int t = 0; t < 2; t++) {
        int K = t ? 128 : 256;
        for (int n = 0; n < K / 2; n++) {
            double a = PI * (n + 0.25) / K, c = PI * n / K;
            d->pre_c[t][n] = (float)cos(a);
            d->pre_s[t][n] = (float)sin(a);
            d->post_c[t][n] = (float)cos(c);
            d->post_s[t][n] = (float)sin(c);
        }
    }
    for (int j = 0; j < 64; j++) {
        d->tw_c[j] = (float)cos(2 * PI * j / 128);
        d->tw_s[j] = (float)sin(2 * PI * j / 128);
    }
    for (unsigned i = 0; i < 128; i++) d->rev7[i] = (uint8_t)bitrev(i, 7);
    for (unsigned i = 0; i < 64; i++) d->rev6[i] = (uint8_t)bitrev(i, 6);
    return d;
}

void ac3_close(Ac3Dec *d) { free(d); }
void ac3_reset(Ac3Dec *d) { memset(d->delay, 0, sizeof d->delay); }
void ac3_set_layout(Ac3Dec *d, Ac3Layout layout) { d->layout = layout; }
void ac3_set_crc_check(Ac3Dec *d, bool on) { d->crc_check = on; }
const Ac3Stats *ac3_stats(const Ac3Dec *d) { return &d->stats; }
const float *ac3_window(const Ac3Dec *d) { return d->window; }

void ac3_set_drc(Ac3Dec *d, float scale) { /* 7.7.1: gain = 2^X * (1 + Y/32), X signed 3 bits, Y 5 bits */
    d->drc_scale = scale < 0 ? 0 : scale;
    for (int code = 0; code < 256; code++) {
        int x = (code >> 5) - ((code & 0x80) ? 8 : 0);
        double g = ldexp(1.0 + (code & 31) / 32.0, x);
        d->drc[code] = (float)pow(g, d->drc_scale);
    }
}

static inline float dither(Ac3Dec *d) { /* uniform, +-0.707 (7.3.4) */
    d->seed = d->seed * 1664525u + 1013904223u;
    return (float)(int32_t)d->seed * (SQRT1_2 / 2147483648.0f);
}

/* ============================================================== exponents */

bool ac3_expand_exponents(int strategy, int absexp, const uint8_t *groups, int ngrps, int8_t *out) {
    if (strategy < 1 || strategy > 3) return false;
    int reps = strategy == 3 ? 4 : strategy, prev = absexp, n = 0;
    for (int g = 0; g < ngrps; g++) {
        int v = groups[g];
        if (v > 124) return false;
        int dexp[3] = {v / 25, (v % 25) / 5, v % 5};
        for (int j = 0; j < 3; j++) {
            prev += dexp[j] - 2;
            if (prev < 0 || prev > 24) return false;
            for (int r = 0; r < reps; r++) out[n++] = (int8_t)prev;
        }
    }
    return true;
}

/* ========================================================= bit allocation */

static inline int logadd(int a, int b) {
    int c = a - b, addr = imin((c < 0 ? -c : c) >> 1, 255);
    return c >= 0 ? a + LATAB[addr] : b + LATAB[addr];
}

static inline int calc_lowcomp(int a, int b0, int b1, int bin) {
    if (bin < 7) {
        if (b0 + 256 == b1) a = 384;
        else if (b0 > b1) a = imax(0, a - 64);
    } else if (bin < 20) {
        if (b0 + 256 == b1) a = 320;
        else if (b0 > b1) a = imax(0, a - 64);
    } else {
        a = imax(0, a - 128);
    }
    return a;
}

static int band_of(int bin) {
    int k = 0;
    while (k < NBANDS - 1 && BNDTAB[k + 1] <= bin) k++;
    return k;
}

bool ac3_bit_allocation(const Ac3BitAlloc *p, const int8_t *exps, int start, int end, uint8_t *bap) {
    int psd[256], bndpsd[NBANDS + 1], excite[NBANDS], mask[NBANDS];
    if (end <= start || end > 253 || start < 0) return end == start;
    if (p->snroffset == -960) { /* csnroffst and fsnroffst both 0: no mantissas at all */
        memset(bap + start, 0, (size_t)(end - start));
        return true;
    }
    for (int bin = start; bin < end; bin++) psd[bin] = 3072 - (exps[bin] << 7);

    /* integrate the PSD over each band (7.2.2.3) */
    int j = start, k = band_of(start), lastbin;
    do {
        lastbin = imin(BNDTAB[k + 1], end);
        int v = psd[j++];
        for (; j < lastbin; j++) v = logadd(v, psd[j]);
        bndpsd[k++] = v;
    } while (end > lastbin);

    /* excitation function (7.2.2.4) */
    int bndstrt = band_of(start), bndend = band_of(end - 1) + 1;
    bool lfe = bndend == 7;
    int begin, lowcomp = 0, fastleak = 0, slowleak = 0;
    if (bndstrt == 0) { /* full-bandwidth and LFE channels */
        lowcomp = calc_lowcomp(lowcomp, bndpsd[0], bndpsd[1], 0);
        excite[0] = bndpsd[0] - p->fgain - lowcomp;
        lowcomp = calc_lowcomp(lowcomp, bndpsd[1], bndpsd[2], 1);
        excite[1] = bndpsd[1] - p->fgain - lowcomp;
        begin = 7;
        for (int bin = 2; bin < 7; bin++) {
            if (!(lfe && bin == 6)) lowcomp = calc_lowcomp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = bndpsd[bin] - p->fgain;
            slowleak = bndpsd[bin] - p->sgain;
            excite[bin] = fastleak - lowcomp;
            if (!(lfe && bin == 6) && bndpsd[bin] <= bndpsd[bin + 1]) {
                begin = bin + 1;
                break;
            }
        }
        for (int bin = begin; bin < imin(bndend, 22); bin++) {
            if (!(lfe && bin == 6)) lowcomp = calc_lowcomp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = imax(fastleak - p->fdecay, bndpsd[bin] - p->fgain);
            slowleak = imax(slowleak - p->sdecay, bndpsd[bin] - p->sgain);
            excite[bin] = imax(fastleak - lowcomp, slowleak);
        }
        begin = 22;
    } else { /* coupling channel */
        begin = bndstrt;
        fastleak = (p->cplfleak << 8) + 768;
        slowleak = (p->cplsleak << 8) + 768;
    }
    for (int bin = begin; bin < bndend; bin++) {
        fastleak = imax(fastleak - p->fdecay, bndpsd[bin] - p->fgain);
        slowleak = imax(slowleak - p->sdecay, bndpsd[bin] - p->sgain);
        excite[bin] = imax(fastleak, slowleak);
    }

    /* masking curve (7.2.2.5) */
    for (int bin = bndstrt; bin < bndend; bin++) {
        if (bndpsd[bin] < p->dbknee) excite[bin] += (p->dbknee - bndpsd[bin]) >> 2;
        mask[bin] = imax(excite[bin], HTH[bin][p->fscod]);
    }

    /* delta bit allocation (7.2.2.6) */
    if (p->dba_mode == 0 || p->dba_mode == 1) {
        int band = 0;
        for (int seg = 0; seg < p->dba_nseg && seg < 8; seg++) {
            band += p->dba_offst[seg];
            if (band >= NBANDS || p->dba_len[seg] > NBANDS - band) return false;
            int delta = p->dba_ba[seg] >= 4 ? (p->dba_ba[seg] - 3) * 128 : (p->dba_ba[seg] - 4) * 128;
            for (int i = 0; i < p->dba_len[seg]; i++) mask[band++] += delta;
        }
    }

    /* bit allocation pointers (7.2.2.7) */
    int i = start;
    j = bndstrt;
    do {
        lastbin = imin(BNDTAB[j + 1], end);
        int m = mask[j] - p->snroffset - p->floor;
        if (m < 0) m = 0;
        m = (m & 0x1fe0) + p->floor;
        for (; i < lastbin; i++) {
            int a = psd[i] - m;
            a = a < 0 ? 0 : a >> 5;
            bap[i] = BAPTAB[a > 63 ? 63 : a];
        }
        j++;
    } while (end > lastbin);
    return true;
}

static bool allocate(Ac3Dec *d, int slot, int start, int end) {
    Ac3BitAlloc p = {
        .fscod = d->h.fscod,
        .sdecay = d->sdecay,
        .fdecay = d->fdecay,
        .sgain = d->sgain,
        .dbknee = d->dbknee,
        .floor = d->floor,
        .fgain = d->fgain[slot],
        .snroffset = ((d->csnroffst - 15) * 16 + d->fsnroffst[slot]) * 4,
        .cplfleak = d->cplfleak,
        .cplsleak = d->cplsleak,
        .dba_mode = slot == LFE ? 2 : d->dba_mode[slot],
        .dba_nseg = d->dba_nseg[slot],
    };
    memcpy(p.dba_offst, d->dba_offst[slot], 8);
    memcpy(p.dba_len, d->dba_len[slot], 8);
    memcpy(p.dba_ba, d->dba_ba[slot], 8);
    if ((p.dba_mode == 0 || p.dba_mode == 1) && p.dba_nseg) d->stats.blocks_dba++;
    return ac3_bit_allocation(&p, d->exps[slot], start, end, d->bap[slot]);
}

/* ============================================================== mantissas */

static void mantissas(Ac3Dec *d, Bits *b, Groups *g, int slot, int start, int end, bool dith, bool *dith_mask) {
    const int8_t *e = d->exps[slot];
    const uint8_t *bap = d->bap[slot];
    float *c = d->coef[slot];
    for (int bin = start; bin < end; bin++) {
        int a = bap[bin];
        float m;
        d->stats.bap[a]++;
        switch (a) {
        case 0:
            m = dith ? dither(d) : 0.0f;
            if (dith_mask) dith_mask[bin] = true;
            break;
        case 1:
            if (g->n1) m = Q1[g->q1[--g->n1]];
            else {
                unsigned v = getb(b, 5);
                g->q1[1] = (uint8_t)((v % 9) / 3);
                g->q1[0] = (uint8_t)(v % 3);
                g->n1 = 2;
                m = Q1[v / 9];
            }
            break;
        case 2:
            if (g->n2) m = Q2[g->q2[--g->n2]];
            else {
                unsigned v = getb(b, 7);
                g->q2[1] = (uint8_t)((v % 25) / 5);
                g->q2[0] = (uint8_t)(v % 5);
                g->n2 = 2;
                m = Q2[v / 25];
            }
            break;
        case 3: m = Q3[getb(b, 3)]; break;
        case 4:
            if (g->n4) {
                m = Q4[g->q4];
                g->n4 = 0;
            } else {
                unsigned v = getb(b, 7);
                g->q4 = (uint8_t)(v % 11);
                g->n4 = 1;
                m = Q4[v / 11];
            }
            break;
        case 5: m = Q5[getb(b, 4)]; break;
        default: { /* asymmetric: two's complement fraction */
            int q = QBITS[a];
            int v = (int)getb(b, q);
            if (v >= 1 << (q - 1)) v -= 1 << q;
            m = (float)v * EXPSCALE[q - 1];
        }
        }
        if (dith_mask && a) dith_mask[bin] = false;
        c[bin] = m * EXPSCALE[e[bin]];
    }
}

/* ============================================================ audio block */

static void read_dba(Bits *b, Ac3Dec *d, int slot) {
    d->dba_nseg[slot] = (int)getb(b, 3) + 1;
    for (int s = 0; s < d->dba_nseg[slot]; s++) {
        d->dba_offst[slot][s] = (uint8_t)getb(b, 5);
        d->dba_len[slot][s] = (uint8_t)getb(b, 4);
        d->dba_ba[slot][s] = (uint8_t)getb(b, 3);
    }
}

/* every rejection records where it happened (ac3_stats: error_block / error_line) */
#define FAIL (d->stats.error_block = blk, d->stats.error_line = __LINE__, AC3_ERR_BITSTREAM)

/* Parse audio block blk (5.4.3) and leave its frequency coefficients in d->coef. */
static int audio_block(Ac3Dec *d, Bits *b, int blk) {
    int nf = d->h.nfchans, acmod = d->h.acmod;
    for (int ch = 0; ch < nf; ch++) d->blksw[ch] = getb(b, 1);
    for (int ch = 0; ch < nf; ch++) d->dithflag[ch] = getb(b, 1);
    for (int i = 0; i <= (acmod == 0); i++) { /* dynrng, dynrng2 */
        if (getb(b, 1)) d->dynrng[i] = d->drc[getb(b, 8)];
        else if (blk == 0) d->dynrng[i] = 1.0f;
    }

    /* coupling strategy */
    if (getb(b, 1)) {
        d->cplinu = getb(b, 1);
        if (d->cplinu) {
            if (acmod < 2) return FAIL;
            for (int ch = 0; ch < nf; ch++) d->chincpl[ch] = getb(b, 1);
            d->phsflginu = acmod == 2 && getb(b, 1);
            d->cplbegf = (int)getb(b, 4);
            d->cplendf = (int)getb(b, 4);
            int nsub = 3 + d->cplendf - d->cplbegf;
            if (nsub < 1) return FAIL;
            d->cplstrtmant = 37 + 12 * d->cplbegf;
            d->cplendmant = 37 + 12 * (d->cplendf + 3);
            int nb = 0;
            for (int sb = 0; sb < nsub; sb++) {
                if (sb == 0 || !getb(b, 1)) nb++; /* cplbndstrc: 1 = merge with the previous sub-band */
                d->cplbnd_end[nb - 1] = d->cplstrtmant + 12 * (sb + 1);
            }
            d->ncplbnd = nb;
        } else {
            memset(d->chincpl, 0, sizeof d->chincpl);
        }
        for (int ch = 0; ch < nf; ch++)
            if (!d->chincpl[ch]) d->cplco_ok[ch] = false; /* rejoining coupling needs fresh coordinates */
    } else if (blk == 0) {
        return FAIL;
    }

    /* coupling coordinates and phase flags */
    if (d->cplinu) {
        bool fresh = false;
        for (int ch = 0; ch < nf; ch++) {
            if (!d->chincpl[ch]) continue;
            if (getb(b, 1)) {
                int mstr = 3 * (int)getb(b, 2);
                for (int bnd = 0; bnd < d->ncplbnd; bnd++) {
                    int e = (int)getb(b, 4), m = (int)getb(b, 4);
                    float v = e == 15 ? (float)m / 16.0f : (float)(m + 16) / 32.0f;
                    d->cplco[ch][bnd] = 8.0f * v * EXPSCALE[e + mstr];
                }
                d->cplco_ok[ch] = true;
                fresh = true;
            } else if (!d->cplco_ok[ch]) {
                return FAIL;
            }
        }
        if (acmod == 2 && fresh)
            for (int bnd = 0; bnd < d->ncplbnd; bnd++) d->phsflg[bnd] = d->phsflginu && getb(b, 1);
    }

    /* rematrixing */
    if (acmod == 2) {
        if (getb(b, 1)) {
            d->nrematbnd = !d->cplinu || d->cplbegf > 2 ? 4 : d->cplbegf > 0 ? 3 : 2;
            for (int i = 0; i < d->nrematbnd; i++) d->rematflg[i] = getb(b, 1);
        } else if (blk == 0) {
            d->nrematbnd = 0;
        }
    }

    /* exponent strategies and bandwidth */
    int cplexpstr = 0, chexpstr[NFULL] = {0}, lfeexpstr = 0;
    if (d->cplinu) cplexpstr = (int)getb(b, 2);
    for (int ch = 0; ch < nf; ch++) chexpstr[ch] = (int)getb(b, 2);
    if (d->h.lfeon) lfeexpstr = (int)getb(b, 1);
    for (int ch = 0; ch < nf; ch++) {
        if (!chexpstr[ch]) continue;
        if (d->chincpl[ch]) {
            d->endmant[ch] = d->cplstrtmant;
        } else {
            int bw = (int)getb(b, 6);
            if (bw > 60) return FAIL;
            d->endmant[ch] = 73 + 3 * bw;
        }
    }

    /* exponents */
    uint8_t grp[84];
    if (d->cplinu) {
        d->stats.exp_strategy[cplexpstr]++;
        if (cplexpstr) {
            int absexp = (int)getb(b, 4) << 1;
            int ngrps = (d->cplendmant - d->cplstrtmant) / (3 << (cplexpstr - 1));
            for (int g = 0; g < ngrps; g++) grp[g] = (uint8_t)getb(b, 7);
            if (!ac3_expand_exponents(cplexpstr, absexp, grp, ngrps, d->exps[CPL] + d->cplstrtmant)) return FAIL;
            d->exp_start_cpl = d->cplstrtmant;
            d->exp_end[CPL] = d->cplendmant;
        } else if (d->exp_start_cpl != d->cplstrtmant || d->exp_end[CPL] != d->cplendmant) {
            return FAIL;
        }
    }
    for (int ch = 0; ch < nf; ch++) {
        d->stats.exp_strategy[chexpstr[ch]]++;
        if (chexpstr[ch]) {
            int gs = 3 << (chexpstr[ch] - 1);
            int ngrps = (d->endmant[ch] + gs - 4) / gs;
            d->exps[ch][0] = (int8_t)getb(b, 4);
            for (int g = 0; g < ngrps; g++) grp[g] = (uint8_t)getb(b, 7);
            if (!ac3_expand_exponents(chexpstr[ch], d->exps[ch][0], grp, ngrps, d->exps[ch] + 1)) return FAIL;
            getb(b, 2); /* gainrng: only matters to fixed-point decoders */
            d->exp_end[ch] = d->endmant[ch];
            d->exp_cpl[ch] = d->chincpl[ch];
        } else if (!d->exp_end[ch] || d->exp_cpl[ch] != d->chincpl[ch] ||
                   (d->chincpl[ch] && d->exp_end[ch] != d->cplstrtmant)) {
            return FAIL;
        }
    }
    if (d->h.lfeon) {
        if (lfeexpstr) {
            d->exps[LFE][0] = (int8_t)getb(b, 4);
            grp[0] = (uint8_t)getb(b, 7);
            grp[1] = (uint8_t)getb(b, 7);
            if (!ac3_expand_exponents(1, d->exps[LFE][0], grp, 2, d->exps[LFE] + 1)) return FAIL;
            d->exp_end[LFE] = 7;
        } else if (!d->exp_end[LFE]) {
            return FAIL;
        }
        d->endmant[LFE] = 7;
    }

    /* bit allocation parameters */
    if (getb(b, 1)) {
        d->sdecay = SLOWDEC[getb(b, 2)];
        d->fdecay = FASTDEC[getb(b, 2)];
        d->sgain = SLOWGAIN[getb(b, 2)];
        d->dbknee = DBPBTAB[getb(b, 2)];
        d->floor = FLOORTAB[getb(b, 3)];
    } else if (blk == 0) {
        return FAIL;
    }
    if (getb(b, 1)) {
        d->csnroffst = (int)getb(b, 6);
        if (d->cplinu) {
            d->fsnroffst[CPL] = (int)getb(b, 4);
            d->fgain[CPL] = FASTGAIN[getb(b, 3)];
        }
        for (int ch = 0; ch < nf; ch++) {
            d->fsnroffst[ch] = (int)getb(b, 4);
            d->fgain[ch] = FASTGAIN[getb(b, 3)];
        }
        if (d->h.lfeon) {
            d->fsnroffst[LFE] = (int)getb(b, 4);
            d->fgain[LFE] = FASTGAIN[getb(b, 3)];
        }
    } else if (blk == 0) {
        return FAIL;
    }
    if (d->cplinu) {
        if (getb(b, 1)) {
            d->cplfleak = (int)getb(b, 3);
            d->cplsleak = (int)getb(b, 3);
        } else if (blk == 0) {
            return FAIL;
        }
    }
    if (getb(b, 1)) { /* delta bit allocation */
        if (d->cplinu) d->dba_mode[CPL] = (int)getb(b, 2);
        for (int ch = 0; ch < nf; ch++) d->dba_mode[ch] = (int)getb(b, 2);
        if (d->cplinu && d->dba_mode[CPL] == 3) return FAIL;
        for (int ch = 0; ch < nf; ch++)
            if (d->dba_mode[ch] == 3) return FAIL;
        if (d->cplinu && d->dba_mode[CPL] == 1) read_dba(b, d, CPL);
        for (int ch = 0; ch < nf; ch++)
            if (d->dba_mode[ch] == 1) read_dba(b, d, ch);
    } else if (blk == 0) {
        for (int s = 0; s < NSLOTS; s++) d->dba_mode[s] = 2;
    }
    if (getb(b, 1)) { /* skip field */
        int skipl = (int)getb(b, 9);
        b->pos += 8 * skipl;
    }
    if (b->pos > b->end) return FAIL;

    /* bit allocation */
    if (d->cplinu && !allocate(d, CPL, d->cplstrtmant, d->cplendmant)) return FAIL;
    for (int ch = 0; ch < nf; ch++)
        if (!allocate(d, ch, 0, d->endmant[ch])) return FAIL;
    if (d->h.lfeon && !allocate(d, LFE, 0, 7)) return FAIL;

    /* mantissas, in transmission order */
    Groups g = {0};
    bool got_cpl = false;
    for (int ch = 0; ch < nf; ch++) {
        mantissas(d, b, &g, ch, 0, d->endmant[ch], d->dithflag[ch], NULL);
        if (d->cplinu && d->chincpl[ch] && !got_cpl) {
            mantissas(d, b, &g, CPL, d->cplstrtmant, d->cplendmant, true, d->cpl_dith);
            got_cpl = true;
        }
    }
    if (d->h.lfeon) mantissas(d, b, &g, LFE, 0, 7, false, NULL);
    if (b->pos > b->end) return FAIL;

    /* decoupling (7.4) */
    for (int ch = 0; ch < nf; ch++) {
        float *c = d->coef[ch];
        int top = d->endmant[ch];
        if (d->cplinu && d->chincpl[ch]) {
            int bin = d->cplstrtmant;
            for (int bnd = 0; bnd < d->ncplbnd; bnd++) {
                float k = d->cplco[ch][bnd];
                if (acmod == 2 && ch == 1 && d->phsflg[bnd]) k = -k;
                for (; bin < d->cplbnd_end[bnd]; bin++)
                    c[bin] = (d->cpl_dith[bin] && !d->dithflag[ch] ? 0.0f : d->coef[CPL][bin]) * k;
            }
            top = d->cplendmant;
        }
        memset(c + top, 0, sizeof(float) * (size_t)(256 - top));
    }
    if (d->h.lfeon) memset(d->coef[LFE] + 7, 0, sizeof(float) * 249);

    /* rematrixing (7.5) */
    if (acmod == 2) {
        int end = imin(d->endmant[0], d->endmant[1]);
        bool any = false;
        for (int bnd = 0; bnd < d->nrematbnd; bnd++) {
            if (!d->rematflg[bnd]) continue;
            any = true;
            for (int i = REMATBND[bnd]; i < imin(end, REMATBND[bnd + 1]); i++) {
                float l = d->coef[0][i], r = d->coef[1][i];
                d->coef[0][i] = l + r;
                d->coef[1][i] = l - r;
            }
        }
        d->stats.blocks_remat += any;
    }

    /* dynamic range (7.7) */
    for (int ch = 0; ch < nf + d->h.lfeon; ch++) {
        int slot = ch < nf ? ch : LFE;
        float gain = acmod == 0 && ch == 1 ? d->dynrng[1] : d->dynrng[0];
        if (gain != 1.0f)
            for (int i = 0; i < 256; i++) d->coef[slot][i] *= gain;
    }

    d->stats.blocks_cpl += d->cplinu;
    if (d->cplinu && acmod == 2)
        for (int bnd = 0; bnd < d->ncplbnd; bnd++)
            if (d->phsflg[bnd]) {
                d->stats.blocks_phase++;
                break;
            }
    for (int ch = 0; ch < nf; ch++) {
        d->stats.blocks_short += d->blksw[ch];
        d->stats.blocks_dither += d->dithflag[ch];
        if (d->blksw[ch]) d->stats.short_mask[blk] |= (uint8_t)(1u << ch);
    }
    return AC3_OK;
}

/* ============================================================== transform */

/* Complex FFT of 2^bits points (bits 7 or 6), positive exponent, unnormalised. */
static void fft(const Ac3Dec *d, float *re, float *im, int bits) {
    int n = 1 << bits;
    const uint8_t *rev = bits == 7 ? d->rev7 : d->rev6;
    for (int i = 0; i < n; i++) {
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
    for (int size = 2; size <= n; size <<= 1) {
        int half = size >> 1, step = 128 / size;
        for (int k = 0; k < half; k++) {
            float wr = d->tw_c[k * step], wi = d->tw_s[k * step];
            for (int a = k; a < n; a += size) {
                int c = a + half;
                float tr = re[c] * wr - im[c] * wi, ti = re[c] * wi + im[c] * wr;
                re[c] = re[a] - tr;
                im[c] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

/* DCT-IV of K = 256 or 128 values: u[m] = sum_k x[k] cos(pi/K (m+1/2)(k+1/2)),
   through a K/2-point complex FFT. */
static void dct4(const Ac3Dec *d, const float *x, float *u, int K) {
    int M = K / 2, t = K == 256 ? 0 : 1;
    float re[128], im[128];
    for (int n = 0; n < M; n++) {
        float a = x[2 * n], b = -x[K - 1 - 2 * n], c = d->pre_c[t][n], s = d->pre_s[t][n];
        re[n] = a * c - b * s;
        im[n] = a * s + b * c;
    }
    fft(d, re, im, K == 256 ? 7 : 6);
    for (int k = 0; k < M; k++) {
        float c = d->post_c[t][k], s = d->post_s[t][k];
        u[2 * k] = re[k] * c - im[k] * s;
        u[K - 1 - 2 * k] = re[k] * s + im[k] * c;
    }
}

void ac3_imdct(Ac3Dec *d, const float coef[256], bool short_block, float y[512]) {
    if (!short_block) { /* N = 512, alpha = 0: y[n] = u(n + 128) with the DCT-IV symmetries */
        float u[256];
        dct4(d, coef, u, 256);
        for (int n = 0; n < 128; n++) y[n] = u[n + 128];
        for (int n = 128; n < 384; n++) y[n] = -u[383 - n];
        for (int n = 384; n < 512; n++) y[n] = -u[n - 384];
    } else { /* two N = 256 transforms on the even (alpha = -1) and odd (alpha = +1) coefficients */
        float x1[128], x2[128], u1[128], u2[128];
        for (int k = 0; k < 128; k++) {
            x1[k] = coef[2 * k];
            x2[k] = coef[2 * k + 1];
        }
        dct4(d, x1, u1, 128);
        dct4(d, x2, u2, 128);
        for (int n = 0; n < 128; n++) y[n] = u1[n];
        for (int n = 128; n < 256; n++) y[n] = -u1[255 - n];
        for (int n = 0; n < 128; n++) y[256 + n] = -u2[127 - n];
        for (int n = 128; n < 256; n++) y[256 + n] = -u2[n - 128];
    }
}

/* window + overlap-add (7.9.4.1 steps 5-6): 256 new PCM samples */
static void synth(Ac3Dec *d, const float *coef, bool short_block, float *delay, float *pcm) {
    float y[512];
    ac3_imdct(d, coef, short_block, y);
    for (int n = 0; n < 256; n++) pcm[n] = y[n] * d->wgain[n] + delay[n];
    for (int n = 0; n < 256; n++) delay[n] = y[256 + n] * d->wgain[255 - n];
}

/* ================================================================= output */

static const char *const NAMES_NATIVE[8][5] = {
    {"Ch1", "Ch2"}, {"C"}, {"L", "R"}, {"L", "C", "R"}, {"L", "R", "S"}, {"L", "C", "R", "S"},
    {"L", "R", "Ls", "Rs"}, {"L", "C", "R", "Ls", "Rs"}};

int ac3_layout_channels(int acmod, bool lfe, Ac3Layout layout) {
    if (acmod < 0 || acmod > 7) return 0;
    return layout == AC3_LAYOUT_STEREO ? 2 : NFCHANS[acmod] + (lfe ? 1 : 0);
}

/* output channel i -> native channel index (nfchans = LFE) for the WAV order */
static int wav_source(int acmod, bool lfe, int i) {
    int nf = NFCHANS[acmod], order[6], n = 0;
    bool centre = (acmod & 1) && acmod != 1;
    if (acmod < 2) {
        for (int c = 0; c < nf; c++) order[n++] = c;
        if (lfe) order[n++] = nf;
    } else {
        order[n++] = 0;
        order[n++] = centre ? 2 : 1;
        if (centre) order[n++] = 1;
        if (lfe) order[n++] = nf;
        for (int c = centre ? 3 : 2; c < nf; c++) order[n++] = c;
    }
    return i >= 0 && i < n ? order[i] : -1;
}

const char *ac3_channel_name(int acmod, bool lfe, Ac3Layout layout, int i) {
    if (acmod < 0 || acmod > 7 || i < 0 || i >= ac3_layout_channels(acmod, lfe, layout)) return "";
    if (layout == AC3_LAYOUT_STEREO) return i ? "R" : "L";
    int src = layout == AC3_LAYOUT_WAV ? wav_source(acmod, lfe, i) : i;
    return src == NFCHANS[acmod] ? "LFE" : NAMES_NATIVE[acmod][src];
}

const char *ac3_acmod_name(int acmod, bool lfe) {
    static const char *const plain[8] = {"1+1", "1.0", "2.0", "3.0", "2/1", "3/1", "2/2", "5.0"};
    static const char *const with_lfe[8] = {"1+1.1", "1.1", "2.1", "3.1", "2/1.1", "3/1.1", "2/2.1", "5.1"};
    if (acmod < 0 || acmod > 7) return "?";
    return lfe ? with_lfe[acmod] : plain[acmod];
}

static void downmix_coefs(const Hdr *h, float l[NFULL], float r[NFULL]) { /* Lo/Ro (7.8.2), normalised */
    memset(l, 0, sizeof(float) * NFULL);
    memset(r, 0, sizeof(float) * NFULL);
    float c = h->clev, s = h->slev;
    switch (h->acmod) {
    case 0:
    case 2: l[0] = r[1] = 1; break;
    case 1: l[0] = r[0] = SQRT1_2; break;
    case 3: l[0] = r[2] = 1, l[1] = r[1] = c; break;
    case 4: l[0] = r[1] = 1, l[2] = r[2] = s * SQRT1_2; break;
    case 5: l[0] = r[2] = 1, l[1] = r[1] = c, l[3] = r[3] = s * SQRT1_2; break;
    case 6: l[0] = r[1] = 1, l[2] = r[3] = s; break;
    case 7: l[0] = r[2] = 1, l[1] = r[1] = c, l[3] = r[4] = s; break;
    }
    float sl = 0, sr = 0;
    for (int i = 0; i < NFULL; i++) sl += l[i], sr += r[i];
    for (int i = 0; i < NFULL; i++) l[i] /= sl, r[i] /= sr;
}

static void emit(Ac3Dec *d, int blk, float *out, int nout) {
    int nf = d->h.nfchans, lfe = d->h.lfeon;
    float *o = out + (size_t)blk * 256 * (size_t)nout;
    if (d->layout == AC3_LAYOUT_STEREO) {
        float l[NFULL], r[NFULL];
        downmix_coefs(&d->h, l, r);
        for (int n = 0; n < 256; n++) {
            float a = 0, b = 0;
            for (int ch = 0; ch < nf; ch++) a += l[ch] * d->pcm[ch][n], b += r[ch] * d->pcm[ch][n];
            o[2 * n] = a;
            o[2 * n + 1] = b;
        }
        return;
    }
    for (int i = 0; i < nf + lfe; i++) {
        int src = d->layout == AC3_LAYOUT_WAV ? wav_source(d->h.acmod, lfe, i) : i;
        const float *p = d->pcm[src];
        for (int n = 0; n < 256; n++) o[n * nout + i] = p[n];
    }
}

void ac3_to_s16(const float *in, int16_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float v = in[i] * 32768.0f;
        v += v < 0 ? -0.5f : 0.5f;
        out[i] = (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)v);
    }
}

/* ================================================================= decode */

static void tail_stats(Ac3Dec *d, const Bits *b) {
    int total = b->end, aux_end = total - 18; /* auxdatae, crcrsv and crc2 close the frame */
    Bits t = {d->buf, aux_end, total};
    int aux_start = aux_end;
    if (getb(&t, 1)) { /* auxdatae: auxdatal (14 bits) precedes it, the aux bits precede that */
        Bits a = {d->buf, aux_end - 14, total};
        d->stats.aux_bits = (int)getb(&a, 14);
        aux_start = aux_end - 14 - d->stats.aux_bits;
    }
    d->stats.bits_used = b->pos;
    d->stats.bits_free = aux_start - b->pos;
    bool zero = d->stats.bits_free >= 0;
    for (int p = b->pos; zero && p < aux_start;) {
        int k = imin(24, aux_start - p);
        Bits z = {d->buf, p, total};
        if (getb(&z, k)) zero = false;
        p += k;
    }
    d->stats.tail_zero = zero;
}

int ac3_decode(Ac3Dec *d, const uint8_t *frame, size_t n, float *out, int *channels, Ac3Info *info) {
    *channels = 0;
    memset(&d->stats, 0, sizeof d->stats);
    if (!frame || n < 6 || frame[0] != 0x0B || frame[1] != 0x77) return AC3_ERR_SYNC;
    int bsid = frame[5] >> 3;
    if (bsid > 8) return bsid <= 16 ? AC3_ERR_UNSUPPORTED : AC3_ERR_SYNC;
    int size = ac3_frame_size(frame, n);
    if (!size) return AC3_ERR_SYNC;
    if ((size_t)size > n) return AC3_ERR_TRUNCATED;

    memcpy(d->buf, frame, (size_t)size);
    memset(d->buf + size, 0, 16);
    Bits b = {d->buf, 0, size * 8};
    Hdr *h = &d->h;
    if (!parse_bsi(&b, h, true)) return AC3_ERR_SYNC;
    Ac3Info tmp;
    fill_info(h, info ? info : &tmp);
    int nout = ac3_layout_channels(h->acmod, h->lfeon, d->layout);
    *channels = nout;

    int err = AC3_OK;
    if (d->crc_check && !ac3_crc_ok(d->buf, (size_t)size)) err = AC3_ERR_CRC;
    if (h->acmod != d->prev_acmod || h->lfeon != d->prev_lfe) { /* e.g. 2.0 adverts around a 5.1 film */
        ac3_reset(d);
        d->prev_acmod = h->acmod;
        d->prev_lfe = h->lfeon;
    }

    /* a new frame starts from scratch: block 0 must carry everything */
    memset(d->exp_end, 0, sizeof d->exp_end);
    memset(d->cplco_ok, 0, sizeof d->cplco_ok);
    memset(d->dba_nseg, 0, sizeof d->dba_nseg);
    d->cplinu = false;
    d->nrematbnd = 0;
    d->exp_start_cpl = 0;
    d->cplfleak = d->cplsleak = 0;
    for (int s = 0; s < NSLOTS; s++) d->fgain[s] = FASTGAIN[0];

    for (int blk = 0; blk < 6 && !err; blk++) {
        err = audio_block(d, &b, blk);
        if (err) break;
        for (int ch = 0; ch < h->nfchans; ch++) synth(d, d->coef[ch], d->blksw[ch], d->delay[ch], d->pcm[ch]);
        if (h->lfeon) synth(d, d->coef[LFE], false, d->delay[h->nfchans], d->pcm[h->nfchans]);
        emit(d, blk, out, nout);
    }
    if (err) {
        memset(out, 0, sizeof(float) * AC3_SAMPLES * (size_t)nout);
        ac3_reset(d);
        return err;
    }
    tail_stats(d, &b);
    return AC3_OK;
}
