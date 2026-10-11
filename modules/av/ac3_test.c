#include "../../tests/test.h"
#include "ac3.h"
#include "ac3_testenc.h"

#include <math.h>

#define PI 3.14159265358979323846

/* ------------------------------------------------------------- helpers */

typedef struct {
    uint8_t *p;
    int pos, cap; /* cap in bytes: bits past it are counted, not written */
} BitW;           /* MSB-first bit writer over a zeroed buffer */

static void put(BitW *w, unsigned v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (((v >> i) & 1) && w->pos < w->cap * 8) w->p[w->pos >> 3] |= (uint8_t)(0x80 >> (w->pos & 7));
        w->pos++;
    }
}

/* syncinfo + bsi for a 48 kHz frame of frmsizecod; crc1 left 0 */
static void put_header(BitW *w, int frmsizecod, int acmod, bool lfe, int dialnorm) {
    put(w, 0x0B77, 16);
    put(w, 0, 16);         /* crc1 */
    put(w, 0, 2);          /* fscod 48 kHz */
    put(w, (unsigned)frmsizecod, 6);
    put(w, 8, 5);          /* bsid */
    put(w, 0, 3);          /* bsmod */
    put(w, (unsigned)acmod, 3);
    if ((acmod & 1) && acmod != 1) put(w, 1, 2); /* cmixlev -4.5 dB */
    if (acmod & 4) put(w, 0, 2);                 /* surmixlev -3 dB */
    if (acmod == 2) put(w, 2, 2);                /* dsurmod: Dolby Surround */
    put(w, lfe, 1);
    put(w, (unsigned)dialnorm, 5);
    put(w, 0, 3); /* compre, langcode, audprodie */
    if (acmod == 0) {
        put(w, 20, 5); /* dialnorm2 */
        put(w, 0, 3);
    }
    put(w, 0, 2); /* copyrightb, origbs */
    put(w, 0, 3); /* timecod1e, timecod2e, addbsie */
}

/* make both CRCs of a frame valid (crc1 by search: it sits at the start of what it covers) */
static void seal(uint8_t *f, int size) {
    int words = size / 2, f58 = 2 * ((words >> 1) + (words >> 3));
    uint16_t c2 = ac3_crc16(0, f + f58, (size_t)(size - f58 - 2));
    f[size - 2] = (uint8_t)(c2 >> 8);
    f[size - 1] = (uint8_t)c2;
    for (unsigned c = 0; c < 65536; c++) {
        f[2] = (uint8_t)(c >> 8);
        f[3] = (uint8_t)c;
        if (ac3_crc16(0, f + 2, (size_t)f58 - 2) == 0) return;
    }
}

/* A/52 forward transform (the encoder's definition): windowed 512 samples -> 256 coefficients */
static void forward(const double *x, bool short_block, double *X) {
    if (!short_block) {
        for (int k = 0; k < 256; k++) {
            double s = 0;
            for (int n = 0; n < 512; n++)
                s += x[n] * cos(2 * PI / 2048 * (2 * n + 1) * (2 * k + 1) + PI / 4 * (2 * k + 1));
            X[k] = -2.0 / 512 * s;
        }
        return;
    }
    for (int half = 0; half < 2; half++) { /* alpha = -1 on the first 256, +1 on the second */
        for (int k = 0; k < 128; k++) {
            double s = 0;
            for (int n = 0; n < 256; n++)
                s += x[256 * half + n] * cos(2 * PI / 1024 * (2 * n + 1) * (2 * k + 1) + PI / 4 * (2 * k + 1) * (2 * half));
            X[2 * k + half] = -2.0 / 256 * s;
        }
    }
}

static double win512(const float *w, int n) { return n < 256 ? w[n] : w[511 - n]; }

/* ------------------------------------------------------------- framing */

TEST(ac3_crc16_matches_the_standard_check_value) {
    CHECK_INT(ac3_crc16(0, (const uint8_t *)"123456789", 9), 0xFEE8); /* CRC-16/UMTS */
}

TEST(ac3_frame_sizes_follow_table_5_18) {
    uint8_t h[8] = {0x0B, 0x77, 0, 0, 0, 8 << 3, 0, 0};
    h[4] = 0; /* 48 kHz, 32 kbit/s */
    CHECK_INT(ac3_frame_size(h, 8), 128);
    h[4] = 30; /* 448 kbit/s */
    CHECK_INT(ac3_frame_size(h, 8), 1792);
    h[4] = 20; /* 192 kbit/s */
    CHECK_INT(ac3_frame_size(h, 8), 768);
    h[4] = 0x40 | 0; /* 44.1 kHz: padding word on odd codes */
    CHECK_INT(ac3_frame_size(h, 8), 138);
    h[4] = 0x40 | 1;
    CHECK_INT(ac3_frame_size(h, 8), 140);
    h[4] = 0x40 | 37;
    CHECK_INT(ac3_frame_size(h, 8), 2788);
    h[4] = 0x80 | 37; /* 32 kHz, 640 kbit/s: the largest frame */
    CHECK_INT(ac3_frame_size(h, 8), AC3_MAX_FRAME);
    h[4] = 0xC0; /* reserved fscod */
    CHECK_INT(ac3_frame_size(h, 8), 0);
    h[4] = 38; /* reserved frmsizecod */
    CHECK_INT(ac3_frame_size(h, 8), 0);
    h[4] = 0;
    h[5] = 16 << 3; /* E-AC-3 */
    CHECK_INT(ac3_frame_size(h, 8), 0);
    h[5] = 8 << 3;
    h[1] = 0x78;
    CHECK_INT(ac3_frame_size(h, 8), 0);
    CHECK_INT(ac3_frame_size(h, 3), 0);
}

TEST(ac3_header_fields) {
    uint8_t f[64] = {0};
    BitW w = {f, 0, (int)sizeof f};
    put_header(&w, 30, 7, true, 27);
    Ac3Info in;
    CHECK(ac3_parse_header(f, sizeof f, &in));
    CHECK_INT(in.sample_rate, 48000);
    CHECK_INT(in.bitrate, 448);
    CHECK_INT(in.frame_bytes, 1792);
    CHECK_INT(in.fscod, 0);
    CHECK_INT(in.frmsizecod, 30);
    CHECK_INT(in.acmod, 7);
    CHECK(in.lfe);
    CHECK_INT(in.channels, 6);
    CHECK_INT(in.dialnorm, 27);
    CHECK(fabsf(in.clev - 0.5946f) < 1e-3f);
    CHECK(fabsf(in.slev - 0.7071f) < 1e-3f);
    CHECK_STR(ac3_acmod_name(in.acmod, in.lfe), "5.1");

    memset(f, 0, sizeof f);
    w = (BitW){f, 0, (int)sizeof f};
    put_header(&w, 20, 2, false, 0); /* dialnorm 0 is reserved: -31 */
    CHECK(ac3_parse_header(f, sizeof f, &in));
    CHECK_INT(in.channels, 2);
    CHECK_INT(in.dsurmod, 2);
    CHECK_INT(in.dialnorm, 31);
    CHECK_STR(ac3_acmod_name(2, false), "2.0");

    memset(f, 0, sizeof f);
    w = (BitW){f, 0, (int)sizeof f};
    put_header(&w, 20, 0, false, 24); /* 1+1 carries a second set of fields */
    CHECK(ac3_parse_header(f, sizeof f, &in));
    CHECK_INT(in.acmod, 0);
    CHECK_INT(in.channels, 2);
    CHECK_STR(ac3_channel_name(0, false, AC3_LAYOUT_NATIVE, 1), "Ch2");

    f[5] = (uint8_t)((16 << 3) | (f[5] & 7)); /* E-AC-3 */
    CHECK(!ac3_parse_header(f, sizeof f, &in));
}

TEST(ac3_channel_layouts) {
    CHECK_INT(ac3_layout_channels(7, true, AC3_LAYOUT_NATIVE), 6);
    CHECK_INT(ac3_layout_channels(7, true, AC3_LAYOUT_STEREO), 2);
    const char *native[6], *wav[6];
    for (int i = 0; i < 6; i++) {
        native[i] = ac3_channel_name(7, true, AC3_LAYOUT_NATIVE, i);
        wav[i] = ac3_channel_name(7, true, AC3_LAYOUT_WAV, i);
    }
    CHECK_STR(native[0], "L");
    CHECK_STR(native[1], "C");
    CHECK_STR(native[2], "R");
    CHECK_STR(native[5], "LFE");
    CHECK_STR(wav[0], "L");
    CHECK_STR(wav[1], "R");
    CHECK_STR(wav[2], "C");
    CHECK_STR(wav[3], "LFE");
    CHECK_STR(wav[4], "Ls");
    CHECK_STR(wav[5], "Rs");
    CHECK_STR(ac3_channel_name(4, true, AC3_LAYOUT_WAV, 2), "LFE"); /* 2/1: L R LFE S */
    CHECK_STR(ac3_channel_name(4, true, AC3_LAYOUT_WAV, 3), "S");
    CHECK_STR(ac3_channel_name(1, false, AC3_LAYOUT_WAV, 0), "C");
    CHECK_STR(ac3_channel_name(2, false, AC3_LAYOUT_NATIVE, 2), "");
}

/* ------------------------------------------------------- building blocks */

TEST(ac3_exponents_unpack_d15_d25_d45) {
    /* deltas (+1,-1,0) = code 25*3+5*1+2 = 82, then (-2,+2,+2) = 0+20+4 = 24 */
    uint8_t groups[2] = {82, 24};
    int8_t out[24];
    CHECK(ac3_expand_exponents(1, 10, groups, 2, out));
    int8_t want15[6] = {11, 10, 10, 8, 10, 12};
    for (int i = 0; i < 6; i++) CHECK_INT(out[i], want15[i]);
    CHECK(ac3_expand_exponents(2, 10, groups, 2, out));
    for (int i = 0; i < 12; i++) CHECK_INT(out[i], want15[i / 2]);
    CHECK(ac3_expand_exponents(3, 10, groups, 2, out));
    for (int i = 0; i < 24; i++) CHECK_INT(out[i], want15[i / 4]);

    uint8_t bad = 125;
    CHECK(!ac3_expand_exponents(1, 10, &bad, 1, out)); /* groups only go to 124 */
    uint8_t down = 0;                                  /* -2 -2 -2 */
    CHECK(!ac3_expand_exponents(1, 4, &down, 1, out)); /* below 0 */
    uint8_t up = 124;                                  /* +2 +2 +2 */
    CHECK(!ac3_expand_exponents(1, 20, &up, 1, out));  /* above 24 */
    CHECK(!ac3_expand_exponents(0, 10, groups, 2, out));
}

static Ac3BitAlloc params(int csnr, int fsnr, int fgaincode, int floor, int dbknee) {
    static const int fastgain[8] = {0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400};
    return (Ac3BitAlloc){.fscod = 0,
                         .sdecay = 0x13,
                         .fdecay = 0x53,
                         .sgain = 0x4d8,
                         .dbknee = dbknee,
                         .floor = floor,
                         .fgain = fastgain[fgaincode],
                         .snroffset = ((csnr - 15) * 16 + fsnr) * 4,
                         .dba_mode = 2};
}

TEST(ac3_bit_allocation_hand_computed) {
    int8_t exps[256];
    uint8_t bap[256];
    /* LFE channel, flat spectrum: psd 2560, excitation 2560 - fgain(0x100) = 2304 above the
       hearing threshold, mask (2304 - 0x1f0) & 0x1fe0 + 0x1f0 = 2288: address (2560-2288)>>5 = 8 -> bap 3 */
    memset(exps, 4, sizeof exps);
    Ac3BitAlloc p = params(15, 0, 1, 0x1f0, 0x700);
    p.sgain = 0x540;
    CHECK(ac3_bit_allocation(&p, exps, 0, 7, bap));
    for (int i = 0; i < 7; i++) CHECK_INT(bap[i], 3);

    /* a silent channel (all exponents 24): the mask is the hearing threshold (Table 7.15),
       so each bap follows from hth alone: ((hth + 816) & ~31) - 2048 against psd 0 */
    memset(exps, 24, sizeof exps);
    p = params(34, 4, 4, -2048, 0);
    CHECK(ac3_bit_allocation(&p, exps, 0, 73, bap));
    static const uint8_t want[28] = {0, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
                                     3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5};
    for (int i = 0; i < 28; i++) CHECK_INT(bap[i], want[i]);

    /* csnroffst = fsnroffst = 0: nothing is transmitted */
    p = params(0, 0, 4, 0x1f0, 0x700);
    memset(exps, 3, sizeof exps);
    CHECK(ac3_bit_allocation(&p, exps, 0, 253, bap));
    for (int i = 0; i < 253; i++) CHECK_INT(bap[i], 0);

    /* delta bit allocation: reaching past the 50 bands is refused ... */
    p = params(30, 0, 4, 0x1f0, 0x700);
    p.dba_mode = 1;
    p.dba_nseg = 1;
    p.dba_offst[0] = 41;
    p.dba_len[0] = 15;
    CHECK(!ac3_bit_allocation(&p, exps, 0, 253, bap));
    /* ... and code 7 raises the mask by (7 - 3) * 128 on bands 10..13: fewer bits there only */
    p.dba_offst[0] = 10;
    p.dba_len[0] = 4;
    p.dba_ba[0] = 7;
    uint8_t ref[256];
    Ac3BitAlloc q = p;
    q.dba_mode = 2;
    CHECK(ac3_bit_allocation(&q, exps, 0, 253, ref));
    CHECK(ac3_bit_allocation(&p, exps, 0, 253, bap));
    for (int i = 0; i < 253; i++) {
        if (i >= 10 && i <= 13) CHECK(bap[i] <= ref[i]);
        else CHECK_INT(bap[i], ref[i]);
    }
}

TEST(ac3_window_is_kbd_alpha_5) {
    Ac3Dec *d = ac3_open();
    CHECK(d);
    const float *w = ac3_window(d);
    /* first entries of A/52 Table 7.33 */
    CHECK(fabs(w[0] - 0.00014) < 0.000006);
    CHECK(fabs(w[1] - 0.00024) < 0.000006);
    CHECK(fabs(w[2] - 0.00037) < 0.000006);
    for (int n = 0; n < 256; n++) {
        CHECK(fabs((double)w[n] * w[n] + (double)w[255 - n] * w[255 - n] - 1.0) < 1e-5); /* Princen-Bradley */
        if (n) CHECK(w[n] >= w[n - 1]);
    }
    ac3_close(d);
}

TEST(ac3_imdct_matches_the_definition) {
    Ac3Dec *d = ac3_open();
    float X[256], y[512];
    uint32_t seed = 12345;
    for (int k = 0; k < 256; k++) {
        seed = seed * 1664525u + 1013904223u;
        X[k] = (float)((int32_t)seed / 2147483648.0);
    }
    for (int sh = 0; sh < 2; sh++) {
        ac3_imdct(d, X, sh, y);
        double worst = 0;
        for (int n = 0; n < 512; n++) {
            double ref = 0;
            if (!sh) {
                for (int k = 0; k < 256; k++)
                    ref += X[k] * cos(2 * PI / 2048 * (2 * n + 1) * (2 * k + 1) + PI / 4 * (2 * k + 1));
            } else {
                int half = n / 256, m = n % 256;
                for (int k = 0; k < 128; k++)
                    ref += X[2 * k + half] *
                           cos(2 * PI / 1024 * (2 * m + 1) * (2 * k + 1) + PI / 4 * (2 * k + 1) * (2 * half));
            }
            if (fabs(ref - y[n]) > worst) worst = fabs(ref - y[n]);
        }
        CHECK(worst < 1e-3); /* sums of 256 terms in float */
    }
    ac3_close(d);
}

/* Forward transform each block (with block switching), inverse with ac3_imdct, window and
   overlap-add the way the decoder does (gain -2): the input comes back exactly (TDAC). */
TEST(ac3_transform_reconstructs_across_block_switches) {
    Ac3Dec *d = ac3_open();
    const float *w = ac3_window(d);
    enum { BLOCKS = 10 };
    static const bool sw[BLOCKS] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 0};
    double sig[256 * (BLOCKS + 1)], delay[256] = {0}, worst = 0;
    uint32_t seed = 99;
    for (int i = 0; i < 256 * (BLOCKS + 1); i++) {
        seed = seed * 1664525u + 1013904223u;
        sig[i] = (int32_t)seed / 4294967296.0;
    }
    for (int j = 0; j < BLOCKS; j++) { /* block j covers sig[256j .. 256j+512) */
        double x[512], X[256];
        float Xf[256], y[512];
        for (int n = 0; n < 512; n++) x[n] = sig[256 * j + n] * win512(w, n);
        forward(x, sw[j], X);
        for (int k = 0; k < 256; k++) Xf[k] = (float)X[k];
        ac3_imdct(d, Xf, sw[j], y);
        for (int n = 0; n < 256; n++) {
            double out = -2.0 * y[n] * win512(w, n) + delay[n];
            if (j > 0 && fabs(out - sig[256 * j + n]) > worst) worst = fabs(out - sig[256 * j + n]);
            delay[n] = -2.0 * y[256 + n] * win512(w, 256 + n);
        }
    }
    CHECK(worst < 1e-4);
    ac3_close(d);
}

/* ------------------------------------------------ a tiny encoder: round trip */

/* Encode 48 kHz frames the way an AC-3 encoder would (D15 exponents in every block, no
   coupling, fixed bit allocation parameters, the largest csnroffst that fits), so the
   whole decoder can be checked end to end: syntax, exponents, bit allocation, grouped
   mantissas across channels, transforms with block switching, layouts and downmix. */
typedef struct {
    int bw;         /* chbwcod */
    int kbps;       /* 0 = 192 */
    int acmod;      /* encode_frame_ch only (encode_frame is mono) */
    bool lfe;
    bool shorts[6]; /* blocks using the short transform (full-bandwidth channels) */
    bool dither;
} EncOpts;

static const uint8_t NFULL_CH[8] = {2, 1, 2, 3, 3, 4, 4, 5};
static const int QBITS_T[16] = {0, 0, 0, 0, 0, 0, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};

/* what the last encoded frame transmitted: coefficients as an ideal decoder rebuilds them
   (A/52 7.3: symmetric levels (2q - (L-1)) / L, two's complement fractions, times 2^-exp) */
static double g_sent[6][6][256];
static bool g_short[6][6];

static double dequantise(int code, int bap) {
    static const int levels[6] = {0, 3, 5, 7, 11, 15};
    if (bap == 0) return 0;
    if (bap <= 5) return (2.0 * code - (levels[bap] - 1)) / levels[bap];
    int b = QBITS_T[bap], v = code >= 1 << (b - 1) ? code - (1 << b) : code;
    return v / (double)(1 << (b - 1));
}

/* reference synthesis of one channel-block straight from the A/52 7.9.4 definition (double):
   y[n] = sum X[k] cos(2pi/4N (2n+1)(2k+1) + pi/4 (2k+1)(1+alpha)), window, overlap-add */
static void ideal_block(const double *X, bool short_block, const float *w, double *delay, double *out) {
    double y[512];
    for (int n = 0; n < 512; n++) {
        double acc = 0;
        if (!short_block) {
            for (int k = 0; k < 256; k++) acc += X[k] * cos(2 * PI / 2048 * (2 * n + 1) * (2 * k + 1) + PI / 4 * (2 * k + 1));
        } else {
            int half = n / 256, m = n % 256;
            for (int k = 0; k < 128; k++)
                acc += X[2 * k + half] * cos(2 * PI / 1024 * (2 * m + 1) * (2 * k + 1) + PI / 4 * (2 * k + 1) * (2 * half));
        }
        y[n] = acc;
    }
    for (int n = 0; n < 256; n++) {
        out[n] = -2.0 * y[n] * (double)w[n] + delay[n];
        delay[n] = -2.0 * y[256 + n] * (double)w[255 - n];
    }
}

static void quantise(double m, int bap, int *code) {
    static const int levels[6] = {0, 3, 5, 7, 11, 15};
    if (bap >= 1 && bap <= 5) {
        int L = levels[bap];
        int q = (int)floor((m * L + (L - 1)) / 2.0 + 0.5);
        *code = q < 0 ? 0 : q > L - 1 ? L - 1 : q;
    } else if (bap >= 6) {
        int b = QBITS_T[bap], lim = 1 << (b - 1);
        int q = (int)floor(m * lim + 0.5);
        q = q < -lim ? -lim : q > lim - 1 ? lim - 1 : q;
        *code = q & ((1 << b) - 1);
    } else {
        *code = 0;
    }
}

/* one block of one channel: D15-legal exponents and normalised mantissas */
static void analyse_block(const double *s, const float *w, bool short_block, int end, int8_t *exps, double *mant) {
    double x[512], X[256];
    for (int n = 0; n < 512; n++) x[n] = s[n] * win512(w, n);
    forward(x, short_block, X);
    int e[256];
    for (int k = 0; k < end; k++) {
        double a = fabs(X[k]);
        int v = 0;
        if (a == 0) v = 24;
        else
            while (v < 24 && a * ldexp(1.0, v + 1) < 1.0) v++;
        e[k] = v;
    }
    if (e[0] > 15) e[0] = 15;
    for (int k = 1; k < end; k++)
        if (e[k] > e[k - 1] + 2) e[k] = e[k - 1] + 2;
    for (int k = end - 2; k >= 0; k--)
        if (e[k] > e[k + 1] + 2) e[k] = e[k + 1] + 2;
    for (int k = 0; k < end; k++) {
        exps[k] = (int8_t)e[k];
        mant[k] = X[k] * ldexp(1.0, e[k]);
    }
}

static void put_exponents(BitW *w, const int8_t *exps, int end) {
    put(w, (unsigned)exps[0], 4);
    for (int g = 0; g < (end - 1) / 3; g++) {
        int d1 = exps[3 * g + 1] - exps[3 * g], d2 = exps[3 * g + 2] - exps[3 * g + 1],
            d3 = exps[3 * g + 3] - exps[3 * g + 2];
        put(w, (unsigned)(25 * (d1 + 2) + 5 * (d2 + 2) + (d3 + 2)), 7);
    }
}

/* sig[ch]: 256*7 samples (the six blocks of the frame plus the half before), full-bandwidth
   channels in stream order, then the LFE. Returns the csnroffst used, -1 if nothing fits. */
static int encode_frame_ch(double *const *sig, const EncOpts *o, uint8_t *frame) {
    static const int rates[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
    int kbps = o->kbps ? o->kbps : 192, frmsizecod = 0;
    while (rates[frmsizecod >> 1] != kbps) frmsizecod += 2;
    const int size = 4 * kbps, nf = NFULL_CH[o->acmod], nch = nf + o->lfe; /* 48 kHz: 4 bytes per kbit/s */
    int end[6];
    static int8_t exps[6][6][256];
    static double mant[6][6][256];
    Ac3Dec *tmp = ac3_open();
    for (int ch = 0; ch < nch; ch++) {
        end[ch] = ch < nf ? 73 + 3 * o->bw : 7;
        for (int b = 0; b < 6; b++)
            analyse_block(sig[ch] + 256 * b, ac3_window(tmp), ch < nf && o->shorts[b], end[ch], exps[ch][b],
                          mant[ch][b]);
    }
    ac3_close(tmp);

    for (int csnr = 63; csnr >= 0; csnr--) {
        memset(frame, 0, (size_t)size);
        BitW wr = {frame, 0, size};
        put_header(&wr, frmsizecod, o->acmod, o->lfe, 27);
        Ac3BitAlloc p = params(csnr, 0, 4, -2048, 0x900);
        for (int b = 0; b < 6; b++) {
            for (int ch = 0; ch < nf; ch++) put(&wr, o->shorts[b], 1); /* blksw */
            for (int ch = 0; ch < nf; ch++) put(&wr, o->dither, 1);    /* dithflag */
            put(&wr, 0, o->acmod == 0 ? 2 : 1);                       /* dynrnge (+ dynrng2e) */
            put(&wr, b == 0, 1);                                      /* cplstre */
            if (b == 0) put(&wr, 0, 1);                               /* cplinu */
            if (o->acmod == 2) {                                      /* rematstr + four flags off */
                put(&wr, b == 0, 1);
                if (b == 0) put(&wr, 0, 4);
            }
            for (int ch = 0; ch < nf; ch++) put(&wr, 1, 2); /* chexpstr D15 */
            if (o->lfe) put(&wr, 1, 1);                     /* lfeexpstr */
            for (int ch = 0; ch < nf; ch++) put(&wr, (unsigned)o->bw, 6);
            for (int ch = 0; ch < nf; ch++) {
                put_exponents(&wr, exps[ch][b], end[ch]);
                put(&wr, 0, 2); /* gainrng */
            }
            if (o->lfe) put_exponents(&wr, exps[nf][b], 7);
            put(&wr, b == 0, 1);
            if (b == 0) put(&wr, (2 << 9) | (1 << 7) | (1 << 5) | (2 << 3) | 7, 11); /* sdcy fdcy sgain dbpb floor */
            put(&wr, b == 0, 1);
            if (b == 0) {
                put(&wr, (unsigned)csnr, 6);
                for (int ch = 0; ch < nch; ch++) put(&wr, 4, 7); /* fsnroffst 0, fgaincod 4 */
            }
            put(&wr, 0, 1); /* deltbaie */
            put(&wr, 0, 1); /* skiple */
            /* mantissas in transmission order; bap 1/2/4 groups run across channels */
            static int tbap[6 * 256], tcode[6 * 256];
            int nt = 0;
            for (int ch = 0; ch < nch; ch++) {
                uint8_t bap[256];
                ac3_bit_allocation(&p, exps[ch][b], 0, end[ch], bap);
                for (int k = 0; k < 256; k++) g_sent[ch][b][k] = 0;
                for (int k = 0; k < end[ch]; k++, nt++) {
                    tbap[nt] = bap[k];
                    quantise(mant[ch][b][k], bap[k], &tcode[nt]);
                    g_sent[ch][b][k] = dequantise(tcode[nt], bap[k]) * ldexp(1.0, -exps[ch][b][k]);
                }
                g_short[ch][b] = ch < nf && o->shorts[b];
            }
            int pending[5] = {0};
            for (int k = 0; k < nt; k++) {
                int a = tbap[k];
                if (a == 1 || a == 2 || a == 4) {
                    if (pending[a]) {
                        pending[a]--;
                        continue;
                    }
                    int per = a == 4 ? 2 : 3, L = a == 1 ? 3 : a == 2 ? 5 : 11, v = 0, got = 0;
                    for (int j = k; j < nt && got < per; j++) {
                        if (tbap[j] != a) continue;
                        v = v * L + tcode[j];
                        got++;
                    }
                    for (; got < per; got++) v = v * L + (L - 1) / 2;
                    put(&wr, (unsigned)v, a == 1 ? 5 : 7);
                    pending[a] = per - 1;
                } else if (a == 3) {
                    put(&wr, (unsigned)tcode[k], 3);
                } else if (a == 5) {
                    put(&wr, (unsigned)tcode[k], 4);
                } else if (a >= 6) {
                    put(&wr, (unsigned)tcode[k], QBITS_T[a]);
                }
            }
        }
        if (wr.pos <= size * 8 - 18) {
            seal(frame, size);
            return csnr;
        }
    }
    return -1;
}

static int encode_frame(const double *sig, const EncOpts *o, Ac3Dec *unused, uint8_t *frame) { /* mono */
    (void)unused;
    EncOpts m = *o;
    m.acmod = 1;
    m.lfe = false;
    double *one[1] = {(double *)sig};
    return encode_frame_ch(one, &m, frame);
}

static double snr_db(const double *ref, const float *got, int n) {
    double s = 0, e = 0;
    for (int i = 0; i < n; i++) {
        s += ref[i] * ref[i];
        e += (ref[i] - got[i]) * (ref[i] - got[i]);
    }
    return e > 0 ? 10 * log10(s / e) : 200;
}

/* Largest difference between a decoded frame (nch interleaved native channels) and the
   reference synthesis of what the encoder sent; dl carries the reference overlap tails. */
static double frame_error(const float *pcm, int nch, const float *w, double dl[][256]) {
    double worst = 0, ref[256];
    for (int b = 0; b < 6; b++)
        for (int ch = 0; ch < nch; ch++) {
            ideal_block(g_sent[ch][b], g_short[ch][b], w, dl[ch], ref);
            for (int n = 0; n < 256; n++) {
                double e = fabs(pcm[(b * 256 + n) * nch + ch] - ref[n]);
                if (e > worst) worst = e;
            }
        }
    return worst;
}

#define EXACT 2e-5 /* float decoder vs double reference, signals up to ~0.8 */

static void round_trip(const EncOpts *o, double *snr_out, int *csnr_out, long *baps) {
    enum { FRAMES = 4 };
    static double sig[256 * (6 * FRAMES + 2)];
    int total = 256 * (6 * FRAMES + 2);
    for (int i = 0; i < total; i++) {
        double t = (i - 256) / 48000.0;
        sig[i] = i < 256 ? 0 : 0.5 * sin(2 * PI * 1000 * t) + 0.25 * sin(2 * PI * 3150 * t + 1) +
                                   (i % 2000 == 700 ? 0.2 : 0); /* tones + clicks for the short blocks */
    }
    Ac3Dec *d = ac3_open();
    static float out[AC3_SAMPLES * FRAMES];
    static uint8_t frame[2560];
    int size = 4 * (o->kbps ? o->kbps : 192);
    *snr_out = 0;
    memset(baps, 0, sizeof(long) * 16);
    double dl[1][256] = {{0}};
    for (int f = 0; f < FRAMES; f++) {
        *csnr_out = encode_frame(sig + 256 * 6 * f, o, d, frame);
        if (*csnr_out < 0) break;
        CHECK(ac3_crc_ok(frame, (size_t)size));
        int ch = 0;
        Ac3Info in;
        CHECK_INT(ac3_decode(d, frame, (size_t)size, out + AC3_SAMPLES * f, &ch, &in), AC3_OK);
        CHECK_INT(ch, 1);
        CHECK(ac3_stats(d)->tail_zero);
        CHECK(frame_error(out + AC3_SAMPLES * f, 1, ac3_window(d), dl) < EXACT); /* exactly what was sent */
        for (int k = 0; k < 16; k++) baps[k] += ac3_stats(d)->bap[k];
    }
    /* sig[0..256) is the silent half before the first frame, so out[n] reproduces sig[n] */
    *snr_out = snr_db(sig + 256, out + 256, AC3_SAMPLES * FRAMES - 256);
    ac3_close(d);
}

TEST(ac3_encode_decode_round_trip_long_blocks) {
    EncOpts o = {.bw = 60, .kbps = 640}; /* full bandwidth: only bins 253..255 are never coded */
    double snr;
    int csnr;
    long baps[16];
    round_trip(&o, &snr, &csnr, baps);
    CHECK(csnr > 30); /* 640 kbit/s mono leaves plenty of room */
    if (snr < 55) test_fail(__FILE__, __LINE__, "round trip SNR %.1f dB (csnroffst %d)", snr, csnr);
    if (getenv("AC3_VERBOSE")) printf("    SNR %.1f dB at csnroffst %d\n", snr, csnr);
}

TEST(ac3_encode_decode_round_trip_with_short_blocks) {
    EncOpts o = {.bw = 60, .kbps = 640, .shorts = {0, 1, 1, 0, 0, 1}};
    double snr;
    int csnr;
    long baps[16];
    round_trip(&o, &snr, &csnr, baps);
    if (snr < 42) test_fail(__FILE__, __LINE__, "round trip SNR %.1f dB (csnroffst %d)", snr, csnr);
    if (getenv("AC3_VERBOSE")) printf("    SNR %.1f dB at csnroffst %d\n", snr, csnr);
}

/* few bits: the grouped and symmetric quantisers (bap 1..5) carry most of the signal */
TEST(ac3_encode_decode_round_trip_low_bitrate) {
    EncOpts o = {.bw = 20, .kbps = 96, .shorts = {0, 0, 0, 1, 1, 0}}; /* exponents take ~2/3 of the frame */
    double snr;
    int csnr;
    long baps[16];
    round_trip(&o, &snr, &csnr, baps);
    long low = baps[1] + baps[2] + baps[3] + baps[4] + baps[5], all = -baps[0]; /* of the coded ones */
    for (int k = 0; k < 16; k++) all += baps[k];
    if (getenv("AC3_VERBOSE"))
        printf("    SNR %.1f dB at csnroffst %d, bap 1-5: %ld of %ld coded mantissas\n", snr, csnr, low, all);
    CHECK(low * 4 > all); /* a quarter or more of the coded mantissas */
    if (snr < 15) test_fail(__FILE__, __LINE__, "round trip SNR %.1f dB (csnroffst %d)", snr, csnr);
}

/* 5.1: each channel carries its own tone. Pass 0 switches every full-bandwidth channel to
   short blocks for a while and checks the parse, the WAV order and the Lo/Ro downmix; pass 1
   (long blocks: no spectral spread past the coded bandwidth) checks each channel's tone. */
TEST(ac3_encode_decode_round_trip_5_1) {
    enum { FRAMES = 3, N = 256 * (6 * FRAMES + 2) };
    static double sig[6][N];
    static const double freq[6] = {440, 660, 880, 1200, 1500, 300}; /* L C R Ls Rs LFE (7 bins: < 656 Hz) */
    for (int ch = 0; ch < 6; ch++)
        for (int i = 0; i < N; i++) sig[ch][i] = i < 256 ? 0 : 0.3 * sin(2 * PI * freq[ch] * (i - 256) / 48000.0 + ch);
    static float out_n[AC3_SAMPLES * 6 * FRAMES], out_w[AC3_SAMPLES * 6], out_s[AC3_SAMPLES * 2];
    static uint8_t frame[2560];
    static const int wav_from_native[6] = {0, 2, 1, 5, 3, 4}; /* L R C LFE Ls Rs */
    for (int pass = 0; pass < 2; pass++) {
        EncOpts o = {.bw = 12, .kbps = 640, .acmod = 7, .lfe = true}; /* about 10 kHz */
        if (pass == 0) o.shorts[2] = o.shorts[3] = true;
        Ac3Dec *native = ac3_open(), *wav = ac3_open(), *stereo = ac3_open();
        ac3_set_layout(wav, AC3_LAYOUT_WAV);
        ac3_set_layout(stereo, AC3_LAYOUT_STEREO);
        static double dl[6][256];
        memset(dl, 0, sizeof dl);
        for (int f = 0; f < FRAMES; f++) {
            double *chans[6];
            for (int ch = 0; ch < 6; ch++) chans[ch] = sig[ch] + 256 * 6 * f;
            int csnr = encode_frame_ch(chans, &o, frame);
            CHECK(csnr > 15);
            if (getenv("AC3_VERBOSE") && f == 0) printf("    5.1 pass %d csnroffst %d\n", pass, csnr);
            int ch = 0;
            Ac3Info in;
            float *on = out_n + AC3_SAMPLES * 6 * f;
            CHECK_INT(ac3_decode(native, frame, sizeof frame, on, &ch, &in), AC3_OK);
            CHECK_INT(ch, 6);
            CHECK(ac3_stats(native)->tail_zero);
            CHECK_INT(ac3_stats(native)->blocks_short, pass == 0 ? 10 : 0);
            CHECK(frame_error(on, 6, ac3_window(native), dl) < EXACT); /* every channel, LFE included */
            CHECK_INT(ac3_decode(wav, frame, sizeof frame, out_w, &ch, NULL), AC3_OK);
            CHECK_INT(ac3_decode(stereo, frame, sizeof frame, out_s, &ch, NULL), AC3_OK);
            CHECK_INT(ch, 2);
            float norm = 1.0f + in.clev + in.slev;
            for (int n = 0; n < AC3_SAMPLES; n++) {
                const float *s = on + n * 6;
                for (int i = 0; i < 6; i++) CHECK(out_w[n * 6 + i] == s[wav_from_native[i]]);
                float lo = (s[0] + in.clev * s[1] + in.slev * s[3]) / norm;
                float ro = (s[2] + in.clev * s[1] + in.slev * s[4]) / norm;
                CHECK(fabsf(out_s[2 * n] - lo) < 1e-5f && fabsf(out_s[2 * n + 1] - ro) < 1e-5f);
            }
        }
        ac3_close(native);
        ac3_close(wav);
        ac3_close(stereo);
        if (pass == 0) continue;
        for (int ch = 0; ch < 6; ch++) {
            static double ref[AC3_SAMPLES * FRAMES];
            static float got[AC3_SAMPLES * FRAMES];
            for (int n = 0; n < AC3_SAMPLES * FRAMES; n++) ref[n] = sig[ch][n], got[n] = out_n[n * 6 + ch];
            double snr = snr_db(ref + 256, got + 256, AC3_SAMPLES * FRAMES - 256);
            /* the encoder's masking drops weak bins, so this is a sanity floor (a swapped
               channel scores ~0 dB); exactness is checked against frame_error above */
            if (snr < 25) test_fail(__FILE__, __LINE__, "5.1 channel %d: SNR %.1f dB", ch, snr);
            if (getenv("AC3_VERBOSE"))
                printf("    5.1 %-3s SNR %.1f dB\n", ac3_channel_name(7, true, AC3_LAYOUT_NATIVE, ch), snr);
        }
    }
}

size_t ac3_test_tone(uint8_t *out, size_t cap, int frames) {
    enum { MAXF = 64 };
    static double l[1536 * MAXF + 256], r[1536 * MAXF + 256];
    if (frames > MAXF) frames = MAXF;
    for (int i = 0; i < 1536 * frames + 256; i++) {
        double t = i / 48000.0;
        l[i] = 0.3 * sin(2 * PI * 440 * t);
        r[i] = 0.3 * sin(2 * PI * 660 * t);
    }
    EncOpts o = {.bw = 40, .kbps = 192, .acmod = 2};
    size_t n = 0;
    for (int f = 0; f < frames; f++) {
        double *chans[2] = {l + 1536 * f, r + 1536 * f};
        static uint8_t frame[4096];
        memset(frame, 0, sizeof frame);
        int size = encode_frame_ch(chans, &o, frame) >= 0 ? ac3_frame_size(frame, sizeof frame) : 0;
        if (size <= 0 || n + (size_t)size > cap) break;
        memcpy(out + n, frame, (size_t)size);
        n += (size_t)size;
    }
    return n;
}

/* 1+1 (two independent mono programmes, each with its own dynrng field) */
TEST(ac3_encode_decode_dual_mono) {
    enum { N = 256 * 8 };
    static double a[N], b[N];
    for (int i = 0; i < N; i++) {
        double t = (i - 256) / 48000.0;
        a[i] = i < 256 ? 0 : 0.4 * sin(2 * PI * 500 * t);
        b[i] = i < 256 ? 0 : 0.2 * sin(2 * PI * 2500 * t);
    }
    EncOpts o = {.bw = 30, .kbps = 256, .acmod = 0};
    double *chans[2] = {a, b};
    static uint8_t frame[1024];
    CHECK(encode_frame_ch(chans, &o, frame) > 15);
    Ac3Dec *d = ac3_open();
    static float out[AC3_SAMPLES * 2];
    int ch = 0;
    Ac3Info in;
    CHECK_INT(ac3_decode(d, frame, sizeof frame, out, &ch, &in), AC3_OK);
    CHECK_INT(ch, 2);
    CHECK_INT(in.acmod, 0);
    CHECK(ac3_stats(d)->tail_zero);
    static double dl[2][256];
    CHECK(frame_error(out, 2, ac3_window(d), dl) < EXACT);
    static double ra[AC3_SAMPLES], rb[AC3_SAMPLES];
    static float ga[AC3_SAMPLES], gb[AC3_SAMPLES];
    for (int n = 0; n < AC3_SAMPLES; n++) ra[n] = a[n], rb[n] = b[n], ga[n] = out[2 * n], gb[n] = out[2 * n + 1];
    CHECK(snr_db(ra + 256, ga + 256, AC3_SAMPLES - 256) > 40);
    CHECK(snr_db(rb + 256, gb + 256, AC3_SAMPLES - 256) > 40);
    ac3_close(d);
}

/* 5.1 -> mono: the old overlap tails must not bleed into the new layout */
TEST(ac3_layout_change_resets_the_overlap) {
    static double s[6][256 * 7];
    for (int ch = 0; ch < 6; ch++)
        for (int i = 0; i < 256 * 7; i++) s[ch][i] = 0.2 * sin(i * (0.01 + 0.003 * ch));
    static uint8_t five[2560], mono[768];
    EncOpts o51 = {.bw = 12, .kbps = 640, .acmod = 7, .lfe = true}, om = {.bw = 30};
    double *chans[6] = {s[0], s[1], s[2], s[3], s[4], s[5]};
    CHECK(encode_frame_ch(chans, &o51, five) >= 0);
    CHECK(encode_frame(s[0], &om, NULL, mono) >= 0);
    Ac3Dec *d = ac3_open(), *fresh = ac3_open();
    static float out[AC3_SAMPLES * 6], ref[AC3_SAMPLES * 6];
    int ch = 0;
    CHECK_INT(ac3_decode(d, five, sizeof five, out, &ch, NULL), AC3_OK);
    CHECK_INT(ch, 6);
    CHECK_INT(ac3_decode(d, mono, sizeof mono, out, &ch, NULL), AC3_OK);
    CHECK_INT(ch, 1);
    CHECK_INT(ac3_decode(fresh, mono, sizeof mono, ref, &ch, NULL), AC3_OK);
    for (int n = 0; n < AC3_SAMPLES; n++) CHECK(out[n] == ref[n]);
    ac3_close(d);
    ac3_close(fresh);
}

/* ---------------------------------------------------------- robustness */

TEST(ac3_damaged_frames_never_crash) {
    double sig[256 * 7];
    for (int i = 0; i < 256 * 7; i++) sig[i] = 0.3 * sin(i * 0.05) + 0.1 * sin(i * 0.31);
    EncOpts o = {.bw = 30, .shorts = {0, 0, 1}, .dither = true};
    Ac3Dec *d = ac3_open();
    uint8_t good[768], f[768 + 64];
    CHECK(encode_frame(sig, &o, d, good) >= 0);
    static float out[AC3_SAMPLES * AC3_MAX_CHANNELS];
    ac3_set_crc_check(d, false); /* let damage reach the parser */
    uint32_t seed = 7;
    int ok = 0, bad = 0;
    for (int iter = 0; iter < 3000; iter++) {
        memcpy(f, good, sizeof good);
        int flips = 1 + iter % 12;
        for (int k = 0; k < flips; k++) {
            seed = seed * 1664525u + 1013904223u;
            int bit = 32 + (int)(seed >> 8) % (768 * 8 - 32); /* keep the sync word */
            f[bit >> 3] ^= (uint8_t)(0x80 >> (bit & 7));
        }
        if (iter % 50 == 0) /* sometimes pure noise after the header */
            for (int k = 8; k < 768; k++) seed = seed * 1664525u + 1013904223u, f[k] = (uint8_t)(seed >> 24);
        int ch = 0;
        int rc = ac3_decode(d, f, sizeof good, out, &ch, NULL);
        CHECK(rc == AC3_OK || rc == AC3_ERR_BITSTREAM || rc == AC3_ERR_SYNC || rc == AC3_ERR_TRUNCATED ||
              rc == AC3_ERR_UNSUPPORTED);
        if (rc == AC3_OK) {
            ok++;
            for (int i = 0; i < AC3_SAMPLES * ch; i++) CHECK(isfinite(out[i]));
        } else {
            bad++;
        }
    }
    CHECK(ok > 0 && bad > 0);
    /* with the CRC check on, damage is caught and the output is silence */
    ac3_set_crc_check(d, true);
    memcpy(f, good, sizeof good);
    f[400] ^= 0x10;
    int ch = 0;
    CHECK_INT(ac3_decode(d, f, sizeof good, out, &ch, NULL), AC3_ERR_CRC);
    CHECK_INT(ch, 1);
    for (int i = 0; i < AC3_SAMPLES; i++) CHECK(out[i] == 0.0f);
    /* truncated and foreign input */
    CHECK_INT(ac3_decode(d, good, 700, out, &ch, NULL), AC3_ERR_TRUNCATED);
    CHECK_INT(ch, 0);
    CHECK_INT(ac3_decode(d, (const uint8_t *)"not ac3 at all", 14, out, &ch, NULL), AC3_ERR_SYNC);
    memcpy(f, good, sizeof good);
    f[5] = (uint8_t)((16 << 3) | (f[5] & 7));
    CHECK_INT(ac3_decode(d, f, sizeof good, out, &ch, NULL), AC3_ERR_UNSUPPORTED);
    /* finding frames in a stream with junk around them */
    uint8_t stream[2000];
    for (int k = 0; k < (int)sizeof stream; k++) {
        seed = seed * 1664525u + 1013904223u;
        stream[k] = (uint8_t)(seed >> 24);
        if (stream[k] == 0x0B) stream[k] = 0x0C;
    }
    memcpy(stream + 333, good, sizeof good);
    stream[100] = 0x0B, stream[101] = 0x77, stream[104] = 0, stream[105] = 8 << 3; /* a false 128-byte frame */
    CHECK_INT(ac3_find_frame(stream, sizeof stream, true), 333);
    CHECK_INT(ac3_find_frame(stream, 900, true), -1);
    CHECK_INT(ac3_find_frame(stream, 900, false), 333);
    ac3_close(d);
}

TEST(ac3_float_to_s16) {
    float in[6] = {0.0f, 0.5f, -0.5f, 1.5f, -1.5f, 0.99999f};
    int16_t out[6];
    ac3_to_s16(in, out, 6);
    CHECK_INT(out[0], 0);
    CHECK_INT(out[1], 16384);
    CHECK_INT(out[2], -16384);
    CHECK_INT(out[3], 32767);
    CHECK_INT(out[4], -32768);
    CHECK_INT(out[5], 32767);
}

/* ------------------------------------------------- real media (opt-in) */

/* HEMMA_TEST_AC3=<file with AC-3 in it: raw .ac3, .mkv, .vob ...> decodes up to 2000
   frames found by the sync+CRC scan and checks that every one parses exactly. */
TEST(ac3_real_media_from_env) {
    const char *path = getenv("HEMMA_TEST_AC3");
    if (!path || !*path) return;
    FILE *fp = fopen(path, "rb");
    CHECK(fp);
    size_t cap = 64u << 20;
    uint8_t *buf = malloc(cap);
    CHECK(buf);
    size_t n = fread(buf, 1, cap, fp);
    fclose(fp);
    Ac3Dec *d = ac3_open();
    static float out[AC3_SAMPLES * AC3_MAX_CHANNELS];
    long pos = 0;
    int frames = 0, exact = 0;
    while (frames < 2000) {
        long k = ac3_find_frame(buf + pos, n - (size_t)pos, true);
        if (k < 0) break;
        pos += k;
        int ch = 0, size = ac3_frame_size(buf + pos, n - (size_t)pos);
        int rc = ac3_decode(d, buf + pos, n - (size_t)pos, out, &ch, NULL);
        CHECK_INT(rc, AC3_OK);
        exact += ac3_stats(d)->tail_zero;
        for (int i = 0; i < AC3_SAMPLES * ch; i++) CHECK(isfinite(out[i]) && fabsf(out[i]) < 4.0f);
        frames++;
        pos += size;
    }
    CHECK(frames > 0);
    CHECK(exact * 100 >= frames * 99);
    free(buf);
    ac3_close(d);
}
