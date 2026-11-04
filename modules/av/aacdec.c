/* modules/av/aacdec.c — AAC-LC decoder (see aacdec.h). Written from ISO/IEC 14496-3 subpart 4: the
 * AudioSpecificConfig and program_config_element, raw_data_block's elements, individual_channel_stream
 * (ics_info, section_data, scale_factor_data, pulse_data, tns_data, spectral_data), dequantisation, PNS,
 * M/S and intensity stereo, TNS, the filterbank with sine and KBD windows. The spectral and scalefactor
 * Huffman codebooks are the encoder's (aac.h: checked against the standard's tables by aac_test.c). The
 * inverse MDCT is a DCT-IV over a complex FFT of a quarter of the window, unfolded. */
#include "aacdec.h"

#include "aac.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ tables */

static const int RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

/* Scalefactor band offsets (Tables 4.129-4.147: n bands = n + 1 offsets). */
static const uint16_t L96[42] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,  48,  52,
                                 56,  64,  72,  80,  88,  96,  108, 120, 132, 144, 156, 172, 188, 212,
                                 240, 276, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024};
static const uint16_t L64[48] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,  48,  52,  56,  64,
                                 72,  80,  88,  100, 112, 124, 140, 156, 172, 192, 216, 240, 268, 304, 344, 384,
                                 424, 464, 504, 544, 584, 624, 664, 704, 744, 784, 824, 864, 904, 944, 984, 1024};
static const uint16_t L48[50] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  48,  56,  64,  72,  80,  88,
                                 96,  108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448,
                                 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 1024};
static const uint16_t L32[52] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  48,  56,  64,  72,  80,  88, 96,
                                 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512,
                                 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992, 1024};
static const uint16_t L24[48] = {0,   4,   8,   12,  16,  20,  24,  28,  32,  36,  40,  44,  52,  60,  68,  76,
                                 84,  92,  100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220, 240, 260, 284,
                                 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704, 768, 832, 896, 960, 1024};
static const uint16_t L16[44] = {0,   8,   16,  24,  32,  40,  48,  56,  64,  72,  80,  88,  100, 112, 124,
                                 136, 148, 160, 172, 184, 196, 212, 228, 244, 260, 280, 300, 320, 344, 368,
                                 396, 424, 456, 492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024};
static const uint16_t L8[41] = {0,   12,  24,  36,  48,  60,  72,  84,  96,  108, 120, 132, 144, 156,
                                172, 188, 204, 220, 236, 252, 268, 288, 308, 328, 348, 372, 396, 420,
                                448, 476, 508, 544, 580, 620, 664, 712, 764, 820, 880, 944, 1024};
static const uint16_t S96[13] = {0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128};
static const uint16_t S48[15] = {0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128};
static const uint16_t S24[16] = {0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128};
static const uint16_t S16[16] = {0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128};
static const uint16_t S8[16] = {0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128};

static const uint16_t *const SWB_L[13] = {L96, L96, L64, L48, L48, L32, L24, L24, L16, L16, L16, L8, L8};
static const uint8_t NSWB_L[13] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40, 40};
static const uint16_t *const SWB_S[13] = {S96, S96, S96, S48, S48, S48, S24, S24, S16, S16, S16, S8, S8};
static const uint8_t NSWB_S[13] = {12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15};
/* TNS_MAX_BANDS for AAC-LC (Table 4.156) */
static const uint8_t TNS_MAX_L[13] = {31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39};
static const uint8_t TNS_MAX_S[13] = {9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14};
#define TNS_ORDER_L 12
#define TNS_ORDER_S 7

/* Channels of each channelConfiguration (0 = a program config element says; 11, 12 and 14 from later editions). */
static const int8_t CONFIG_CH[16] = {0, 1, 2, 3, 4, 5, 6, 8, -1, -1, -1, 7, 8, -1, 8, -1};

enum { CB_ZERO = 0, CB_NOISE = 13, CB_IS2 = 14, CB_IS = 15 };
enum { SEQ_LONG = 0, SEQ_START = 1, SEQ_SHORT = 2, SEQ_STOP = 3 };
enum { EL_SCE = 0, EL_CPE = 1, EL_CCE = 2, EL_LFE = 3, EL_DSE = 4, EL_PCE = 5, EL_FIL = 6, EL_END = 7 };

/* The rate index whose tables a rate uses (exact, else the ranges of Table 4.82 for explicit rates). */
static int table_index(int rate) {
    for (int i = 0; i < 13; i++)
        if (RATES[i] == rate) return i;
    static const int at_least[12] = {92017, 75132, 55426, 46009, 37566, 27713, 23004, 18783, 13856, 11502, 9391, 1};
    for (int i = 0; i < 12; i++)
        if (rate >= at_least[i]) return i;
    return -1;
}

/* -------------------------------------------------------------- bit reader */

typedef struct {
    const uint8_t *p;
    size_t bytes, pos, n; /* pos and n in bits */
    bool over;
} Br;

static uint32_t peek32(const Br *r) {
    size_t b = r->pos >> 3;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | (b + (size_t)i < r->bytes ? r->p[b + (size_t)i] : 0u);
    return v << (r->pos & 7); /* at least 25 good bits on top */
}

static unsigned getbits(Br *r, int k) { /* 0..25 bits */
    if (k <= 0) return 0;
    if (r->over || r->pos + (size_t)k > r->n) {
        r->over = true;
        r->pos = r->n;
        return 0;
    }
    uint32_t v = peek32(r) >> (32 - k);
    r->pos += (size_t)k;
    return v;
}

static void skipbits(Br *r, size_t k) {
    if (r->over || k > r->n - r->pos) {
        r->over = true;
        r->pos = r->n;
    } else {
        r->pos += k;
    }
}

static void byte_align(Br *r) { skipbits(r, (8 - (r->pos & 7)) & 7); }

/* ------------------------------------------------------------- Huffman */

typedef struct {
    int16_t child[2], sym;
} HNode;

typedef struct {
    HNode node[2 * 289];
    int n;
} HTree;

static bool tree_add(HTree *t, uint32_t code, int len, int sym) {
    int at = 0;
    for (int i = len - 1; i >= 0; i--) {
        int b = (int)((code >> i) & 1u);
        if (t->node[at].sym >= 0) return false; /* a code inside another */
        if (t->node[at].child[b] < 0) {
            if (t->n >= (int)(sizeof t->node / sizeof t->node[0])) return false;
            int nn = t->n++;
            t->node[nn] = (HNode){{-1, -1}, -1};
            t->node[at].child[b] = (int16_t)nn;
        }
        at = t->node[at].child[b];
    }
    t->node[at].sym = (int16_t)sym;
    return true;
}

static bool tree_build(HTree *t, int cb) {
    t->n = 1;
    t->node[0] = (HNode){{-1, -1}, -1};
    if (cb == 0) {
        const uint32_t *code;
        const uint8_t *len;
        aac_sf_codebook(&code, &len);
        for (int i = 0; i < 121; i++)
            if (!tree_add(t, code[i], len[i], i)) return false;
        return true;
    }
    const AacCodebook *b = aac_codebook(cb);
    if (!b) return false;
    for (int i = 0; i < b->size; i++)
        if (!tree_add(t, b->code[i], b->len[i], i)) return false;
    return true;
}

static int huff(Br *r, const HTree *t) {
    if (r->over || r->pos >= r->n) {
        r->over = true;
        return -1;
    }
    uint32_t v = peek32(r);
    int at = 0, len = 0;
    while (t->node[at].sym < 0) {
        if (len >= 25) return -1;
        int nx = t->node[at].child[v >> 31];
        v <<= 1, len++;
        if (nx < 0) return -1;
        at = nx;
    }
    if (r->pos + (size_t)len > r->n) {
        r->over = true;
        return -1;
    }
    r->pos += (size_t)len;
    return t->node[at].sym;
}

/* ----------------------------------------------------------------- FFT */

#define FFT_MAX 512

typedef struct {
    int n;
    uint16_t rev[FFT_MAX];
    float cs[FFT_MAX / 2], sn[FFT_MAX / 2];
} Fft;

static void fft_init(Fft *f, int n) {
    f->n = n;
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        f->rev[i] = (uint16_t)r;
    }
    for (int k = 0; k < n / 2; k++) {
        f->cs[k] = (float)cos(2 * M_PI * k / n);
        f->sn[k] = (float)sin(2 * M_PI * k / n);
    }
}

/* In place, e^{+i 2 pi mk/n}, no scaling. */
static void fft_inv(const Fft *f, float *re, float *im) {
    int n = f->n;
    for (int i = 0; i < n; i++) {
        int j = f->rev[i];
        if (j > i) {
            float t = re[i];
            re[i] = re[j], re[j] = t;
            t = im[i], im[i] = im[j], im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len / 2, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < half; j++) {
                float wr = f->cs[j * step], wi = f->sn[j * step];
                float *ar = &re[i + j], *ai = &im[i + j], *br = &re[i + j + half], *bi = &im[i + j + half];
                float xr = *br * wr - *bi * wi, xi = *br * wi + *bi * wr;
                *br = *ar - xr, *bi = *ai - xi;
                *ar += xr, *ai += xi;
            }
    }
}

/* ------------------------------------------------------------- decoder */

typedef struct {
    int seq, shape, max_sfb, ngroups, glen[8], nwin, num_swb;
    const uint16_t *swb;
} IcsInfo;

typedef struct {
    int nfilt[8], length[8][3], order[8][3], dir[8][3];
    float refl[8][3][TNS_ORDER_L];
} Tns;

typedef struct {
    IcsInfo in;
    uint8_t cb[8 * 64];
    int sf[8 * 64];
    bool tns_on;
    Tns tns;
    float coef[1024];
    int q[1024]; /* long windows: quantised values, for pulses */
} Chan;

typedef struct {
    float pre_c[FFT_MAX], pre_s[FFT_MAX], post_c[FFT_MAX], post_s[FFT_MAX]; /* L = M/2 entries */
    Fft fft;
    int M;
} Dct4;

struct AacDec {
    AacDecInfo info;
    AacDecLayout layout;
    AacDecStats stats;
    int ti; /* table index */
    const uint16_t *swb_l, *swb_s;
    int nl, ns, tns_l, tns_s;
    HTree tree[12];
    float pow43[8192];
    float gain[256];       /* 2^((sf - 100) / 4) */
    float wl[2][1024];     /* rising halves: [shape] long */
    float ws[2][128];      /* short */
    Dct4 dl, ds;           /* M = 1024, 128 */
    float overlap[AACDEC_MAX_CHANNELS][1024];
    int prev_shape[AACDEC_MAX_CHANNELS];
    float pcm[AACDEC_MAX_CHANNELS][1024];
    char roles[AACDEC_MAX_CHANNELS + 1]; /* of the last good frame: 'C' SCE, 'L'/'R' CPE, 'F' LFE */
    int nout; /* channels of the last good frame */
    uint32_t rnd;
    bool ms[8 * 64];
    Chan ch[2];
};

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

/* program_config_element: how many channels it names (-1 = broken). */
static int read_pce(Br *r) {
    getbits(r, 4); /* element_instance_tag */
    getbits(r, 2); /* object_type */
    getbits(r, 4); /* sampling_frequency_index */
    int nf = (int)getbits(r, 4), ns = (int)getbits(r, 4), nb = (int)getbits(r, 4), nl = (int)getbits(r, 2);
    int na = (int)getbits(r, 3), nc = (int)getbits(r, 4);
    if (getbits(r, 1)) getbits(r, 4); /* mono mixdown */
    if (getbits(r, 1)) getbits(r, 4); /* stereo mixdown */
    if (getbits(r, 1)) getbits(r, 3); /* matrix mixdown */
    int ch = 0;
    for (int i = 0; i < nf + ns + nb; i++) {
        bool cpe = getbits(r, 1);
        getbits(r, 4);
        ch += cpe ? 2 : 1;
    }
    for (int i = 0; i < nl; i++) getbits(r, 4), ch++;
    for (int i = 0; i < na; i++) getbits(r, 4);
    for (int i = 0; i < nc; i++) getbits(r, 5);
    byte_align(r);
    int cmt = (int)getbits(r, 8);
    skipbits(r, (size_t)cmt * 8);
    return r->over ? -1 : ch;
}

static int read_aot(Br *r) {
    int aot = (int)getbits(r, 5);
    return aot == 31 ? 32 + (int)getbits(r, 6) : aot;
}

static int read_rate(Br *r) {
    int sfi = (int)getbits(r, 4);
    if (sfi == 15) return (int)getbits(r, 24);
    return sfi < 13 ? RATES[sfi] : 0;
}

bool aacdec_config(const uint8_t *asc, size_t n, AacDecInfo *out, char *err, size_t errcap) {
    AacDecInfo info = {0};
    Br r = {asc, n, 0, n * 8, false};
    if (!asc || n < 2) {
        set_err(err, errcap, "no AAC configuration");
        if (out) *out = info;
        return false;
    }
    int aot = read_aot(&r);
    info.object_type = aot;
    info.sample_rate = info.ext_rate = read_rate(&r);
    info.config = (int)getbits(&r, 4);
    if (aot == 5 || aot == 29) {
        info.sbr = true;
        info.ext_rate = read_rate(&r);
        aot = read_aot(&r);
    }
    bool ok = !r.over && aot == 2;
    if (!ok) set_err(err, errcap, r.over ? "a cut AAC configuration" : aot == 1 ? "AAC Main" : aot == 4 ? "AAC LTP" : aot == 3 ? "AAC SSR" : "an AAC object type other than LC");
    if (ok && (info.sample_rate < 1000 || info.sample_rate > 192000 || table_index(info.sample_rate) < 0)) {
        set_err(err, errcap, "an AAC sample rate out of range");
        ok = false;
    }
    if (ok) { /* GASpecificConfig */
        if (getbits(&r, 1)) {
            set_err(err, errcap, "AAC with 960-sample frames");
            ok = false;
        }
        if (getbits(&r, 1)) getbits(&r, 14); /* dependsOnCoreCoder: coreCoderDelay */
        getbits(&r, 1);                       /* extensionFlag: nothing more for AAC-LC */
        if (info.config == 0) {
            info.channels = read_pce(&r);
        } else {
            info.channels = CONFIG_CH[info.config & 15];
        }
        if (ok && (r.over || info.channels < 1 || info.channels > AACDEC_MAX_CHANNELS)) {
            set_err(err, errcap, info.channels > AACDEC_MAX_CHANNELS ? "AAC with more than 8 channels" : "an AAC channel configuration we can't read");
            ok = false;
        }
    }
    if (ok && !info.sbr && r.n - r.pos >= 16) { /* backward-compatible signalling of SBR after the config */
        if (getbits(&r, 11) == 0x2b7 && read_aot(&r) == 5 && getbits(&r, 1) && !r.over) {
            int ext = read_rate(&r);
            if (!r.over && ext > 0) info.sbr = true, info.ext_rate = ext, info.object_type = 5;
        }
    }
    if (out) *out = info;
    return ok;
}

int aacdec_make_config(int sample_rate, int channels, uint8_t out[2]) {
    int sfi = -1;
    for (int i = 0; i < 13; i++)
        if (RATES[i] == sample_rate) sfi = i;
    if (sfi < 0 || channels < 1 || channels > 8 || channels == 7) return 0; /* no configuration has 7 */
    int cfg = channels <= 6 ? channels : 7;
    out[0] = (uint8_t)((2 << 3) | (sfi >> 1));
    out[1] = (uint8_t)(((sfi & 1) << 7) | (cfg << 3));
    return 2;
}

static void kbd(float *w, int N, double alpha) {
    int half = N / 2;
    double t[1025], sum = 0;
    for (int p = 0; p <= half; p++) {
        double x = (p - N / 4.0) / (N / 4.0), a = M_PI * alpha * sqrt(fmax(0.0, 1.0 - x * x)), term = 1, s = 1;
        for (int k = 1; k < 60 && term > 1e-14 * s; k++) { /* I0 */
            term *= (a / 2 / k) * (a / 2 / k);
            s += term;
        }
        t[p] = s;
        sum += s;
    }
    double acc = 0;
    for (int n = 0; n < half; n++) {
        acc += t[n];
        w[n] = (float)sqrt(acc / sum);
    }
}

static void dct4_init(Dct4 *t, int M) {
    int L = M / 2;
    t->M = M;
    fft_init(&t->fft, L);
    for (int m = 0; m < L; m++) {
        t->pre_c[m] = (float)cos(M_PI * m / M), t->pre_s[m] = (float)sin(M_PI * m / M);
        t->post_c[m] = (float)cos(M_PI * (m + 0.25) / M), t->post_s[m] = (float)sin(M_PI * (m + 0.25) / M);
    }
}

/* DCT-IV: y[k] = sum X[n] cos(pi/M (n + 1/2)(k + 1/2)), M/2-point complex FFT. */
static void dct4(const Dct4 *t, const float *X, float *y) {
    int M = t->M, L = M / 2;
    float re[FFT_MAX], im[FFT_MAX];
    for (int m = 0; m < L; m++) { /* (a - ib) e^{i pi m / M} */
        float a = X[2 * m], b = X[M - 1 - 2 * m], c = t->pre_c[m], s = t->pre_s[m];
        re[m] = a * c + b * s;
        im[m] = a * s - b * c;
    }
    fft_inv(&t->fft, re, im);
    for (int p = 0; p < L; p++) { /* W e^{i pi (p + 1/4) / M} */
        float c = t->post_c[p], s = t->post_s[p];
        y[2 * p] = re[p] * c - im[p] * s;
        y[M - 1 - 2 * p] = re[p] * s + im[p] * c;
    }
}

/* x[n] = 2/N sum X[k] cos(2 pi/N (n + n0)(k + 1/2)), N = 2M outputs (times `scale`). */
static void imdct(const Dct4 *t, const float *X, float *x, float scale) {
    int M = t->M, N = 2 * M;
    float y[1024];
    dct4(t, X, y);
    float s = scale * 2.0f / (float)N;
    for (int n = 0; n < N; n++) {
        int u = n + M / 2;
        float v = u < M ? y[u] : u < 2 * M ? -y[2 * M - 1 - u] : -y[u - 2 * M];
        x[n] = s * v;
    }
}

AacDec *aacdec_open(const uint8_t *asc, size_t n, char *err, size_t errcap) {
    AacDecInfo info;
    if (!aacdec_config(asc, n, &info, err, errcap)) return NULL;
    AacDec *d = calloc(1, sizeof *d);
    if (!d) {
        set_err(err, errcap, "out of memory");
        return NULL;
    }
    d->info = info;
    d->layout = AACDEC_LAYOUT_NATIVE;
    d->ti = table_index(info.sample_rate);
    d->swb_l = SWB_L[d->ti], d->swb_s = SWB_S[d->ti];
    d->nl = NSWB_L[d->ti], d->ns = NSWB_S[d->ti];
    d->tns_l = TNS_MAX_L[d->ti], d->tns_s = TNS_MAX_S[d->ti];
    for (int cb = 0; cb < 12; cb++)
        if (!tree_build(&d->tree[cb], cb)) {
            set_err(err, errcap, "the AAC codebooks didn't build");
            free(d);
            return NULL;
        }
    for (int i = 0; i < 8192; i++) d->pow43[i] = (float)pow((double)i, 4.0 / 3.0);
    for (int i = 0; i < 256; i++) d->gain[i] = (float)pow(2.0, 0.25 * (i - 100));
    for (int i = 0; i < 1024; i++) d->wl[0][i] = (float)sin(M_PI / 2048 * (i + 0.5));
    for (int i = 0; i < 128; i++) d->ws[0][i] = (float)sin(M_PI / 256 * (i + 0.5));
    kbd(d->wl[1], 2048, 4.0);
    kbd(d->ws[1], 256, 6.0);
    dct4_init(&d->dl, 1024);
    dct4_init(&d->ds, 128);
    d->rnd = 0x1f2e3d4c;
    return d;
}

void aacdec_close(AacDec *d) { free(d); }

void aacdec_reset(AacDec *d) {
    memset(d->overlap, 0, sizeof d->overlap);
    memset(d->prev_shape, 0, sizeof d->prev_shape);
}

void aacdec_set_layout(AacDec *d, AacDecLayout layout) { d->layout = layout; }
const AacDecInfo *aacdec_info(const AacDec *d) { return &d->info; }
const AacDecStats *aacdec_stats(const AacDec *d) { return &d->stats; }

/* ------------------------------------------------- individual channel stream */

static bool ics_info(const AacDec *d, Br *r, IcsInfo *in) {
    if (getbits(r, 1)) return false; /* ics_reserved_bit */
    in->seq = (int)getbits(r, 2);
    in->shape = (int)getbits(r, 1);
    in->ngroups = 1;
    in->glen[0] = 1;
    if (in->seq == SEQ_SHORT) {
        in->max_sfb = (int)getbits(r, 4);
        unsigned grouping = getbits(r, 7);
        for (int w = 1; w < 8; w++) {
            if (grouping & (1u << (7 - w))) in->glen[in->ngroups - 1]++;
            else in->glen[in->ngroups++] = 1;
        }
        in->nwin = 8;
        in->num_swb = d->ns;
        in->swb = d->swb_s;
    } else {
        in->max_sfb = (int)getbits(r, 6);
        if (getbits(r, 1)) return false; /* predictor_data_present: not in AAC-LC */
        in->nwin = 1;
        in->num_swb = d->nl;
        in->swb = d->swb_l;
    }
    return !r->over && in->max_sfb <= in->num_swb;
}

static int unpack(const AacDec *d, Br *r, int cb, int *v) { /* one codeword -> 4 or 2 values; 0 = error */
    int sym = huff(r, &d->tree[cb]);
    if (sym < 0) return 0;
    int n;
    switch (cb) {
    case 1:
    case 2: v[0] = sym / 27 - 1, v[1] = sym / 9 % 3 - 1, v[2] = sym / 3 % 3 - 1, v[3] = sym % 3 - 1, n = 4; break;
    case 3:
    case 4: v[0] = sym / 27, v[1] = sym / 9 % 3, v[2] = sym / 3 % 3, v[3] = sym % 3, n = 4; break;
    case 5:
    case 6: v[0] = sym / 9 - 4, v[1] = sym % 9 - 4, n = 2; break;
    case 7:
    case 8: v[0] = sym / 8, v[1] = sym % 8, n = 2; break;
    case 9:
    case 10: v[0] = sym / 13, v[1] = sym % 13, n = 2; break;
    default: v[0] = sym / 17, v[1] = sym % 17, n = 2; break;
    }
    if (cb >= 3 && cb != 5 && cb != 6) /* unsigned books: a sign bit for each value that isn't 0 */
        for (int i = 0; i < n; i++)
            if (v[i] && getbits(r, 1)) v[i] = -v[i];
    if (cb == 11)
        for (int i = 0; i < n; i++) {
            if (abs(v[i]) != 16) continue;
            int ones = 0;
            while (getbits(r, 1)) {
                if (++ones > 8 || r->over) return 0;
            }
            int mag = (1 << (ones + 4)) + (int)getbits(r, ones + 4);
            v[i] = v[i] < 0 ? -mag : mag;
        }
    return r->over ? 0 : n;
}

static bool read_tns(Br *r, const IcsInfo *in, Tns *t) {
    bool sh = in->seq == SEQ_SHORT;
    int maxo = sh ? TNS_ORDER_S : TNS_ORDER_L;
    for (int w = 0; w < in->nwin; w++) {
        t->nfilt[w] = (int)getbits(r, sh ? 1 : 2);
        if (!t->nfilt[w]) continue;
        int res = (int)getbits(r, 1) + 3; /* coef_res: 3 or 4 bits */
        for (int f = 0; f < t->nfilt[w]; f++) {
            t->length[w][f] = (int)getbits(r, sh ? 4 : 6);
            t->order[w][f] = (int)getbits(r, sh ? 3 : 5);
            if (t->order[w][f] > maxo) return false;
            if (!t->order[w][f]) continue;
            t->dir[w][f] = (int)getbits(r, 1);
            int compress = (int)getbits(r, 1), bits = res - compress;
            double iq = ((1 << (res - 1)) - 0.5) / (M_PI / 2), iqm = ((1 << (res - 1)) + 0.5) / (M_PI / 2);
            for (int i = 0; i < t->order[w][f]; i++) {
                int v = (int)getbits(r, bits);
                if (v & (1 << (bits - 1))) v -= 1 << bits; /* two's complement */
                t->refl[w][f][i] = (float)sin(v / (v >= 0 ? iq : iqm));
            }
        }
    }
    return !r->over;
}

/* individual_channel_stream into c (its ics_info already there when `common`). */
static int read_ics(AacDec *d, Br *r, Chan *c, bool common) {
    IcsInfo *in = &c->in;
    int gg = (int)getbits(r, 8);
    if (!common && !ics_info(d, r, in)) return AACDEC_ERR_BITSTREAM;
    bool sh = in->seq == SEQ_SHORT;
    int sbits = sh ? 3 : 5, esc = (1 << sbits) - 1;
    memset(c->cb, 0, sizeof c->cb);
    for (int g = 0; g < in->ngroups; g++) { /* section_data */
        int k = 0;
        while (k < in->max_sfb) {
            int cbk = (int)getbits(r, 4), len = 0, inc;
            if (cbk == 12) return AACDEC_ERR_BITSTREAM;
            while ((inc = (int)getbits(r, sbits)) == esc && !r->over) len += esc;
            len += inc;
            if (r->over || k + len > in->max_sfb) return AACDEC_ERR_BITSTREAM;
            for (int i = k; i < k + len; i++) c->cb[g * 64 + i] = (uint8_t)cbk;
            k += len;
            if (len == 0 && r->pos >= r->n) return AACDEC_ERR_BITSTREAM;
        }
    }
    int sf = gg, nrg = gg - 90, is = 0; /* scale_factor_data */
    bool first_noise = true;
    for (int g = 0; g < in->ngroups; g++)
        for (int i = 0; i < in->max_sfb; i++) {
            int cbk = c->cb[g * 64 + i], *out = &c->sf[g * 64 + i];
            if (cbk == CB_ZERO) {
                *out = 0;
            } else if (cbk == CB_IS || cbk == CB_IS2) {
                int dd = huff(r, &d->tree[0]);
                if (dd < 0) return AACDEC_ERR_BITSTREAM;
                is += dd - 60;
                *out = is;
            } else if (cbk == CB_NOISE) {
                if (first_noise) {
                    nrg += (int)getbits(r, 9) - 256;
                    first_noise = false;
                } else {
                    int dd = huff(r, &d->tree[0]);
                    if (dd < 0) return AACDEC_ERR_BITSTREAM;
                    nrg += dd - 60;
                }
                *out = nrg;
            } else {
                int dd = huff(r, &d->tree[0]);
                if (dd < 0) return AACDEC_ERR_BITSTREAM;
                sf += dd - 60;
                if (sf < 0 || sf > 255) return AACDEC_ERR_BITSTREAM;
                *out = sf;
            }
        }
    int npulse = 0, ppos[4], pamp[4];
    if (getbits(r, 1)) { /* pulse_data: long windows only */
        if (sh) return AACDEC_ERR_BITSTREAM;
        npulse = (int)getbits(r, 2) + 1;
        int start = (int)getbits(r, 6);
        if (start >= in->num_swb) return AACDEC_ERR_BITSTREAM;
        int at = in->swb[start];
        for (int i = 0; i < npulse; i++) {
            at += (int)getbits(r, 5);
            if (at >= 1024) return AACDEC_ERR_BITSTREAM;
            ppos[i] = at;
            pamp[i] = (int)getbits(r, 4);
        }
        d->stats.pulse++;
    }
    c->tns_on = getbits(r, 1);
    if (c->tns_on) {
        if (!read_tns(r, in, &c->tns)) return AACDEC_ERR_BITSTREAM;
        d->stats.tns++;
    }
    if (getbits(r, 1)) return AACDEC_ERR_UNSUPPORTED; /* gain_control_data: AAC SSR */
    if (r->over) return AACDEC_ERR_BITSTREAM;
    /* spectral_data */
    memset(c->coef, 0, sizeof c->coef);
    if (!sh) memset(c->q, 0, sizeof c->q);
    bool noise = false;
    int w0 = 0;
    for (int g = 0; g < in->ngroups; g++) {
        for (int i = 0; i < in->max_sfb; i++) {
            int cbk = c->cb[g * 64 + i], lo = in->swb[i], bw = in->swb[i + 1] - lo;
            if (cbk == CB_ZERO || cbk == CB_IS || cbk == CB_IS2) continue;
            if (cbk == CB_NOISE) {
                noise = true;
                int e = c->sf[g * 64 + i];
                e = e < -100 ? -100 : e > 155 ? 155 : e;
                float scale = (float)pow(2.0, 0.25 * e);
                for (int w = w0; w < w0 + in->glen[g]; w++) {
                    float *cf = c->coef + 128 * w * sh + lo, energy = 0;
                    for (int k = 0; k < bw; k++) {
                        d->rnd = d->rnd * 1664525u + 1013904223u;
                        cf[k] = (float)(int32_t)d->rnd;
                        energy += cf[k] * cf[k];
                    }
                    float s = energy > 0 ? scale / sqrtf(energy) : 0;
                    for (int k = 0; k < bw; k++) cf[k] *= s;
                }
                continue;
            }
            int vals[4], n = bw * in->glen[g];
            for (int j = 0; j < n;) {
                int got = unpack(d, r, cbk, vals);
                if (!got) return AACDEC_ERR_BITSTREAM;
                for (int k = 0; k < got && j < n; k++, j++) {
                    int w = w0 + j / bw, bin = lo + j % bw, v = vals[k];
                    if (!sh) {
                        c->q[bin] = v;
                    } else {
                        float m = d->pow43[abs(v) & 8191] * d->gain[c->sf[g * 64 + i]];
                        c->coef[128 * w + bin] = v < 0 ? -m : m;
                    }
                }
            }
        }
        w0 += in->glen[g];
    }
    if (noise) d->stats.pns++;
    if (!sh) { /* pulses onto the quantised values, then dequantise */
        for (int p = 0; p < npulse; p++) {
            int b = 0;
            while (b < in->max_sfb && in->swb[b + 1] <= ppos[p]) b++;
            if (b >= in->max_sfb) continue;
            int cbk = c->cb[b];
            if (cbk == CB_ZERO || cbk >= CB_NOISE) continue;
            c->q[ppos[p]] += c->q[ppos[p]] > 0 ? pamp[p] : -pamp[p];
        }
        for (int i = 0; i < in->max_sfb; i++) {
            int cbk = c->cb[i];
            if (cbk == CB_ZERO || cbk >= CB_NOISE) continue;
            float gn = d->gain[c->sf[i]];
            for (int k = in->swb[i]; k < in->swb[i + 1]; k++) {
                int v = c->q[k];
                float m = d->pow43[abs(v) & 8191] * gn;
                c->coef[k] = v < 0 ? -m : m;
            }
        }
    }
    if (sh) d->stats.short_frames++;
    return r->over ? AACDEC_ERR_BITSTREAM : AACDEC_OK;
}

static void apply_tns(const AacDec *d, Chan *c) {
    const IcsInfo *in = &c->in;
    bool sh = in->seq == SEQ_SHORT;
    int mmm = sh ? d->tns_s : d->tns_l;
    if (mmm > in->max_sfb) mmm = in->max_sfb;
    for (int w = 0; w < in->nwin; w++) {
        int bottom = in->num_swb;
        for (int f = 0; f < c->tns.nfilt[w]; f++) {
            int top = bottom, order = c->tns.order[w][f];
            bottom = top - c->tns.length[w][f] < 0 ? 0 : top - c->tns.length[w][f];
            if (!order) continue;
            float a[TNS_ORDER_L + 1], b[TNS_ORDER_L + 1]; /* reflection -> LPC (tns_decode_coef) */
            a[0] = 1;
            for (int m = 1; m <= order; m++) {
                float k = c->tns.refl[w][f][m - 1];
                for (int i = 1; i < m; i++) b[i] = a[i] + k * a[m - i];
                for (int i = 1; i < m; i++) a[i] = b[i];
                a[m] = k;
            }
            int start = in->swb[bottom < mmm ? bottom : mmm], end = in->swb[top < mmm ? top : mmm], size = end - start;
            if (size <= 0) continue;
            int inc = 1;
            if (c->tns.dir[w][f]) inc = -1, start = end - 1;
            float *x = c->coef + 128 * w * sh, state[TNS_ORDER_L] = {0};
            for (int i = 0; i < size; i++, start += inc) { /* y = x - a1 y(n-1) - ... (tns_ar_filter) */
                float y = x[start];
                for (int j = 0; j < order; j++) y -= a[j + 1] * state[j];
                for (int j = order - 1; j > 0; j--) state[j] = state[j - 1];
                state[0] = y;
                x[start] = y;
            }
        }
    }
}

/* The filterbank: coefficients -> 1024 samples of output channel oc. */
static void synth(AacDec *d, int oc, const Chan *c) {
    const IcsInfo *in = &c->in;
    const float *lp = d->wl[d->prev_shape[oc]], *lc = d->wl[in->shape];
    const float *sp = d->ws[d->prev_shape[oc]], *sc = d->ws[in->shape];
    float z[2048];
    const float scale = 1.0f / 32768.0f;
    if (in->seq != SEQ_SHORT) {
        imdct(&d->dl, c->coef, z, scale);
        switch (in->seq) {
        case SEQ_START:
            for (int n = 0; n < 1024; n++) z[n] *= lp[n];
            for (int n = 1472; n < 1600; n++) z[n] *= sc[1599 - n];
            for (int n = 1600; n < 2048; n++) z[n] = 0;
            break;
        case SEQ_STOP:
            for (int n = 0; n < 448; n++) z[n] = 0;
            for (int n = 448; n < 576; n++) z[n] *= sp[n - 448];
            for (int n = 1024; n < 2048; n++) z[n] *= lc[2047 - n];
            break;
        default:
            for (int n = 0; n < 1024; n++) z[n] *= lp[n];
            for (int n = 1024; n < 2048; n++) z[n] *= lc[2047 - n];
            break;
        }
    } else {
        memset(z, 0, sizeof z);
        for (int w = 0; w < 8; w++) {
            float y[256];
            const float *left = w == 0 ? sp : sc;
            imdct(&d->ds, c->coef + 128 * w, y, scale);
            for (int j = 0; j < 128; j++) {
                z[448 + 128 * w + j] += y[j] * left[j];
                z[448 + 128 * w + 128 + j] += y[128 + j] * sc[127 - j];
            }
        }
    }
    for (int n = 0; n < 1024; n++) d->pcm[oc][n] = d->overlap[oc][n] + z[n];
    memcpy(d->overlap[oc], z + 1024, sizeof(float) * 1024);
    d->prev_shape[oc] = in->shape;
}

static void stereo(AacDec *d, int ms_present) {
    Chan *l = &d->ch[0], *r = &d->ch[1];
    const IcsInfo *in = &l->in;
    bool sh = in->seq == SEQ_SHORT, any_ms = false, any_is = false;
    int w0 = 0;
    for (int g = 0; g < in->ngroups; g++) {
        for (int i = 0; i < in->max_sfb; i++) {
            int idx = g * 64 + i, cl = l->cb[idx], cr = r->cb[idx];
            int lo = in->swb[i], hi = in->swb[i + 1];
            if (cr == CB_IS || cr == CB_IS2) { /* intensity: right = left scaled */
                int p = r->sf[idx];
                p = p < -155 ? -155 : p > 100 ? 100 : p;
                float s = (float)pow(2.0, -0.25 * p) * (cr == CB_IS ? 1.0f : -1.0f);
                if (ms_present == 1 && d->ms[idx]) s = -s;
                for (int w = w0; w < w0 + in->glen[g]; w++)
                    for (int k = lo; k < hi; k++) r->coef[128 * w * sh + k] = l->coef[128 * w * sh + k] * s;
                any_is = true;
            } else if (d->ms[idx] && cl < CB_NOISE && cr < CB_NOISE) { /* L = M + S, R = M - S */
                for (int w = w0; w < w0 + in->glen[g]; w++)
                    for (int k = lo; k < hi; k++) {
                        float *a = &l->coef[128 * w * sh + k], *b = &r->coef[128 * w * sh + k], m = *a, s = *b;
                        *a = m + s, *b = m - s;
                    }
                any_ms = true;
            }
        }
        w0 += in->glen[g];
    }
    d->stats.ms += any_ms;
    d->stats.is += any_is;
}

static void silence(AacDec *d, float *out, int *channels) {
    int ch = d->nout;
    if (d->layout == AACDEC_LAYOUT_STEREO && ch) ch = 2;
    if (d->layout == AACDEC_LAYOUT_MONO && ch) ch = 1;
    memset(out, 0, sizeof(float) * AACDEC_FRAME * (size_t)(ch ? ch : 1));
    *channels = ch;
}

/* The decoded channels (d->pcm, roles in d->roles) into out, in the chosen layout. */
static int emit(AacDec *d, int n, float *out) {
    const char *ro = d->roles;
    if (d->layout == AACDEC_LAYOUT_MONO) {
        int k = 0;
        for (int c = 0; c < n; c++) k += ro[c] != 'F';
        float s = k ? 1.0f / (float)k : 0;
        for (int i = 0; i < AACDEC_FRAME; i++) {
            float v = 0;
            for (int c = 0; c < n; c++)
                if (ro[c] != 'F') v += d->pcm[c][i];
            out[i] = v * s;
        }
        return 1;
    }
    if (d->layout == AACDEC_LAYOUT_STEREO) {
        /* first SCE = centre, first CPE = front, later pairs and lone channels = surrounds, LFE dropped */
        float wl[AACDEC_MAX_CHANNELS] = {0}, wr[AACDEC_MAX_CHANNELS] = {0}, sum = 1;
        int pairs = 0;
        bool centre = false;
        const float k3 = 0.70710678f;
        for (int c = 0; c < n; c++) {
            if (ro[c] == 'L') {
                float w = pairs == 0 ? 1.0f : k3;
                wl[c] = w, wr[c + 1] = w;
                if (pairs++) sum += k3;
                c++;
            } else if (ro[c] == 'C') {
                float w = !centre && pairs == 0 ? k3 : k3 * k3;
                if (n == 1) w = 1;
                wl[c] = wr[c] = w;
                centre = true;
                if (n > 1) sum += w;
            }
        }
        if (pairs == 0 && n > 1) sum = 1; /* lone channels only */
        float s = 1.0f / sum;
        for (int i = 0; i < AACDEC_FRAME; i++) {
            float a = 0, b = 0;
            for (int c = 0; c < n; c++) a += wl[c] * d->pcm[c][i], b += wr[c] * d->pcm[c][i];
            out[2 * i] = a * s, out[2 * i + 1] = b * s;
        }
        return 2;
    }
    int map[AACDEC_MAX_CHANNELS];
    for (int c = 0; c < n; c++) map[c] = c;
    if (d->layout == AACDEC_LAYOUT_WAV) {
        if (!strcmp(ro, "CLR")) map[0] = 1, map[1] = 2, map[2] = 0;                      /* L R C */
        else if (!strcmp(ro, "CLRC")) map[0] = 1, map[1] = 2, map[2] = 0, map[3] = 3;    /* L R C Cs */
        else if (!strcmp(ro, "CLRLR")) map[0] = 1, map[1] = 2, map[2] = 0, map[3] = 3, map[4] = 4;
        else if (!strcmp(ro, "CLRLRF")) map[0] = 1, map[1] = 2, map[2] = 0, map[3] = 5, map[4] = 3, map[5] = 4;
    }
    for (int i = 0; i < AACDEC_FRAME; i++)
        for (int c = 0; c < n; c++) out[i * n + c] = d->pcm[map[c]][i];
    return n;
}

int aacdec_decode(AacDec *d, const uint8_t *au, size_t n, float *out, int *channels) {
    int dummy;
    if (!channels) channels = &dummy;
    if (au && n >= 7 && au[0] == 0xFF && (au[1] & 0xF6) == 0xF0) { /* an ADTS header */
        size_t len = ((size_t)(au[3] & 3) << 11) | ((size_t)au[4] << 3) | (au[5] >> 5), hdr = (au[1] & 1) ? 7 : 9;
        if (len == n && n > hdr && (au[6] & 3) == 0) au += hdr, n -= hdr;
    }
    Br r = {au, n, 0, n * 8, false};
    int nout = 0, rc = AACDEC_OK, elements = 0;
    char roles[AACDEC_MAX_CHANNELS + 1] = {0};
    d->stats.frames++;
    while (rc == AACDEC_OK) {
        int id = (int)getbits(&r, 3);
        if (r.over) {
            rc = AACDEC_ERR_BITSTREAM;
            break;
        }
        if (id == EL_END) break;
        if (++elements > 64) {
            rc = AACDEC_ERR_BITSTREAM;
            break;
        }
        switch (id) {
        case EL_SCE:
        case EL_LFE: {
            getbits(&r, 4);
            if (nout + 1 > AACDEC_MAX_CHANNELS) {
                rc = AACDEC_ERR_UNSUPPORTED;
                break;
            }
            Chan *c = &d->ch[0];
            rc = read_ics(d, &r, c, false);
            if (rc) break;
            if (c->tns_on) apply_tns(d, c);
            roles[nout] = id == EL_LFE ? 'F' : 'C';
            synth(d, nout++, c);
            break;
        }
        case EL_CPE: {
            getbits(&r, 4);
            if (nout + 2 > AACDEC_MAX_CHANNELS) {
                rc = AACDEC_ERR_UNSUPPORTED;
                break;
            }
            bool common = getbits(&r, 1);
            int ms_present = 0;
            memset(d->ms, 0, sizeof d->ms);
            if (common) {
                if (!ics_info(d, &r, &d->ch[0].in)) {
                    rc = AACDEC_ERR_BITSTREAM;
                    break;
                }
                d->ch[1].in = d->ch[0].in;
                ms_present = (int)getbits(&r, 2);
                if (ms_present == 3) {
                    rc = AACDEC_ERR_BITSTREAM;
                    break;
                }
                const IcsInfo *in = &d->ch[0].in;
                for (int g = 0; g < in->ngroups; g++)
                    for (int i = 0; i < in->max_sfb; i++) d->ms[g * 64 + i] = ms_present == 2 || (ms_present == 1 && getbits(&r, 1));
            }
            rc = read_ics(d, &r, &d->ch[0], common);
            if (!rc) rc = read_ics(d, &r, &d->ch[1], common);
            if (rc) break;
            if (common) stereo(d, ms_present);
            for (int k = 0; k < 2; k++) {
                if (d->ch[k].tns_on) apply_tns(d, &d->ch[k]);
                roles[nout] = k ? 'R' : 'L';
                synth(d, nout++, &d->ch[k]);
            }
            break;
        }
        case EL_DSE: {
            getbits(&r, 4);
            bool align = getbits(&r, 1);
            size_t cnt = getbits(&r, 8);
            if (cnt == 255) cnt += getbits(&r, 8);
            if (align) byte_align(&r);
            skipbits(&r, cnt * 8);
            break;
        }
        case EL_PCE:
            if (read_pce(&r) < 0) rc = AACDEC_ERR_BITSTREAM;
            break;
        case EL_FIL: {
            size_t cnt = getbits(&r, 4);
            if (cnt == 15) cnt += getbits(&r, 8) - 1;
            if (cnt > 0 && r.pos + 4 <= r.n) {
                unsigned type = peek32(&r) >> 28;
                if (type == 13 || type == 14) { /* EXT_SBR_DATA(_CRC): HE-AAC signalled implicitly */
                    d->stats.fill_sbr++;
                    if (!d->info.sbr) {
                        d->info.sbr = true;
                        d->info.ext_rate = d->info.sample_rate * 2;
                        if (d->info.object_type == 2) d->info.object_type = 5;
                    }
                }
            }
            skipbits(&r, cnt * 8);
            break;
        }
        default: /* CCE */
            rc = AACDEC_ERR_UNSUPPORTED;
            break;
        }
        if (rc == AACDEC_OK && r.over) rc = AACDEC_ERR_BITSTREAM;
    }
    if (rc == AACDEC_OK && nout == 0) rc = AACDEC_ERR_BITSTREAM;
    if (rc != AACDEC_OK) {
        d->stats.errors++;
        silence(d, out, channels);
        return rc;
    }
    roles[nout] = 0;
    memcpy(d->roles, roles, sizeof roles);
    d->nout = nout;
    *channels = emit(d, nout, out);
    return AACDEC_OK;
}

/* ---------------------------------------------------------------- tests */

const uint16_t *aacdec_swb(int sample_rate, bool short_window, int *nbands) {
    int ti = table_index(sample_rate);
    if (ti < 0 || sample_rate < 1000 || sample_rate > 192000) return NULL;
    if (nbands) *nbands = short_window ? NSWB_S[ti] : NSWB_L[ti];
    return short_window ? SWB_S[ti] : SWB_L[ti];
}

const float *aacdec_test_spectrum(const AacDec *d, int k) { return d->ch[k ? 1 : 0].coef; }

void aacdec_test_imdct(const AacDec *d, const float *in, float *out, int n) { imdct(n == 256 ? &d->ds : &d->dl, in, out, 1.0f); }

const float *aacdec_test_window(const AacDec *d, bool short_window, int shape) {
    return short_window ? d->ws[shape ? 1 : 0] : d->wl[shape ? 1 : 0];
}
