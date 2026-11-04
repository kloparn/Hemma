/* The AAC decoder (aacdec.c): its transform and windows against the standard's definitions, our encoder's
 * streams back through it (and through the encoder's own small test decoder - the two must agree), and
 * frames written here by hand for every tool our encoder never uses: PNS, intensity stereo, pulses, TNS,
 * grouped short windows, 5.1, data/fill/program elements, ADTS, HE-AAC's signalling. FFmpeg is the check
 * on top for real files (HEMMA_TEST_AAC, at the end; never part of the suite). */
#include "../../core/plat.h"
#include "../../tests/test.h"
#include "aac.h"
#include "aac_testdec.h"
#include "aacdec.h"
#include "mkv.h"

#include <math.h>
#include <stdio.h>

#define PI 3.14159265358979323846

/* ------------------------------------------------------- a frame writer */

typedef struct {
    uint8_t b[8192];
    int pos; /* bits */
} Wr;

static void put(Wr *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if ((v >> i) & 1u) w->b[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7));
        w->pos++;
    }
}

static int wr_bytes(Wr *w) { return (w->pos + 7) / 8; }

static void put_sf(Wr *w, int diff) {
    const uint32_t *code;
    const uint8_t *len;
    aac_sf_codebook(&code, &len);
    put(w, code[diff + 60], len[diff + 60]);
}

static void put_values(Wr *w, int cb, const int *v) { /* one codeword: 4 or 2 values */
    const AacCodebook *b = aac_codebook(cb);
    int sym, n = b->dim;
    switch (cb) {
    case 1:
    case 2: sym = (v[0] + 1) * 27 + (v[1] + 1) * 9 + (v[2] + 1) * 3 + (v[3] + 1); break;
    case 3:
    case 4: sym = abs(v[0]) * 27 + abs(v[1]) * 9 + abs(v[2]) * 3 + abs(v[3]); break;
    case 5:
    case 6: sym = (v[0] + 4) * 9 + (v[1] + 4); break;
    case 7:
    case 8: sym = abs(v[0]) * 8 + abs(v[1]); break;
    case 9:
    case 10: sym = abs(v[0]) * 13 + abs(v[1]); break;
    default: sym = (abs(v[0]) > 15 ? 16 : abs(v[0])) * 17 + (abs(v[1]) > 15 ? 16 : abs(v[1])); break;
    }
    put(w, b->code[sym], b->len[sym]);
    if (b->is_unsigned)
        for (int i = 0; i < n; i++)
            if (v[i]) put(w, v[i] < 0, 1);
    if (cb == 11)
        for (int i = 0; i < n; i++) {
            int m = abs(v[i]);
            if (m < 16) continue;
            int bits = 0;
            while ((1 << (bits + 1)) <= m) bits++; /* m in [2^bits, 2^(bits+1)) */
            int ones = bits - 4;
            for (int k = 0; k < ones; k++) put(w, 1, 1);
            put(w, 0, 1);
            put(w, (uint32_t)(m - (1 << bits)), bits);
        }
}

/* One individual channel stream as data. */
typedef struct {
    int seq, shape, max_sfb, grouping, gg;
    int cb[8][64], sf[8][64]; /* per group and band: normal = scalefactor, 13 = noise energy, 14/15 = IS position */
    int q[1024];              /* long: q[bin]; short: q[128 * w + bin] */
    int npulse, pstart, poff[4], pamp[4];
    int tns_n, tns_res, tns_len, tns_order, tns_dir, tns_compress, tns_coef[12]; /* one filter, long windows */
} TIcs;

static int t_groups(const TIcs *s, int *glen) {
    if (s->seq != 2) return glen[0] = 1, 1;
    int ng = 1;
    glen[0] = 1;
    for (int w = 1; w < 8; w++) {
        if (s->grouping & (1 << (7 - w))) glen[ng - 1]++;
        else glen[ng++] = 1;
    }
    return ng;
}

static void put_info(Wr *w, const TIcs *s) {
    put(w, 0, 1);
    put(w, (uint32_t)s->seq, 2);
    put(w, (uint32_t)s->shape, 1);
    if (s->seq == 2) {
        put(w, (uint32_t)s->max_sfb, 4);
        put(w, (uint32_t)s->grouping, 7);
    } else {
        put(w, (uint32_t)s->max_sfb, 6);
        put(w, 0, 1);
    }
}

static void put_ics(Wr *w, const TIcs *s, bool common, int rate) {
    bool sh = s->seq == 2;
    int glen[8], ng = t_groups(s, glen), nb;
    const uint16_t *swb = aacdec_swb(rate, sh, &nb);
    put(w, (uint32_t)s->gg, 8);
    if (!common) put_info(w, s);
    int sbits = sh ? 3 : 5, esc = (1 << sbits) - 1;
    for (int g = 0; g < ng; g++)
        for (int i = 0; i < s->max_sfb;) {
            int j = i;
            while (j < s->max_sfb && s->cb[g][j] == s->cb[g][i]) j++;
            int len = j - i;
            put(w, (uint32_t)s->cb[g][i], 4);
            while (len >= esc) put(w, (uint32_t)esc, sbits), len -= esc;
            put(w, (uint32_t)len, sbits);
            i = j;
        }
    int sf = s->gg, nrg = s->gg - 90, is = 0;
    bool first = true;
    for (int g = 0; g < ng; g++)
        for (int i = 0; i < s->max_sfb; i++) {
            int cb = s->cb[g][i], v = s->sf[g][i];
            if (cb == 0) continue;
            if (cb == 14 || cb == 15) put_sf(w, v - is), is = v;
            else if (cb == 13) {
                if (first) put(w, (uint32_t)(v - nrg + 256), 9), first = false;
                else put_sf(w, v - nrg);
                nrg = v;
            } else put_sf(w, v - sf), sf = v;
        }
    put(w, s->npulse > 0, 1);
    if (s->npulse) {
        put(w, (uint32_t)(s->npulse - 1), 2);
        put(w, (uint32_t)s->pstart, 6);
        for (int i = 0; i < s->npulse; i++) put(w, (uint32_t)s->poff[i], 5), put(w, (uint32_t)s->pamp[i], 4);
    }
    put(w, s->tns_n > 0, 1);
    if (s->tns_n) { /* long: n_filt 2 bits */
        put(w, 1, 2);
        put(w, (uint32_t)s->tns_res, 1);
        put(w, (uint32_t)s->tns_len, 6);
        put(w, (uint32_t)s->tns_order, 5);
        put(w, (uint32_t)s->tns_dir, 1);
        put(w, (uint32_t)s->tns_compress, 1);
        int bits = s->tns_res + 3 - s->tns_compress;
        for (int i = 0; i < s->tns_order; i++) put(w, (uint32_t)s->tns_coef[i] & ((1u << bits) - 1), bits);
    }
    put(w, 0, 1); /* gain control */
    int w0 = 0;
    for (int g = 0; g < ng; g++) {
        for (int i = 0; i < s->max_sfb; i++) {
            int cb = s->cb[g][i];
            if (cb == 0 || cb >= 13) continue;
            int bw = swb[i + 1] - swb[i], n = bw * glen[g], dim = aac_codebook(cb)->dim, v[4];
            for (int j = 0; j < n; j += dim) {
                for (int k = 0; k < dim; k++) {
                    int jj = j + k, win = w0 + jj / bw, bin = swb[i] + jj % bw;
                    v[k] = s->q[(sh ? 128 * win : 0) + bin];
                }
                put_values(w, cb, v);
            }
        }
        w0 += glen[g];
    }
}

static void put_sce(Wr *w, const TIcs *s, int rate, bool lfe) {
    put(w, lfe ? 3 : 0, 3);
    put(w, 0, 4);
    put_ics(w, s, false, rate);
}

static void put_cpe(Wr *w, const TIcs *l, const TIcs *r, int rate, int ms_present, const bool *ms) {
    put(w, 1, 3);
    put(w, 0, 4);
    put(w, 1, 1); /* common window */
    put_info(w, l);
    put(w, (uint32_t)ms_present, 2);
    if (ms_present == 1) {
        int glen[8], ng = t_groups(l, glen);
        for (int g = 0; g < ng; g++)
            for (int i = 0; i < l->max_sfb; i++) put(w, ms[g * 64 + i], 1);
    }
    put_ics(w, l, true, rate);
    put_ics(w, r, true, rate);
}

static void put_end(Wr *w) {
    put(w, 7, 3);
    w->pos = (w->pos + 7) & ~7;
}

static TIcs *new_ics(int max_sfb) {
    TIcs *s = calloc(1, sizeof *s);
    s->max_sfb = max_sfb;
    s->gg = 120;
    return s;
}

/* A value codebook cb can carry (escapes past 15 only in book 11). */
static int fits(int cb, int k) {
    const AacCodebook *b = aac_codebook(cb);
    int lav = cb == 11 ? (k % 13 == 0 ? 300 + k : 15) : b->lav, span = 2 * lav + 1;
    return (k * 7) % span - lav;
}

static AacDec *open_lc(int rate, int ch) {
    uint8_t asc[2];
    if (!aacdec_make_config(rate, ch, asc)) return NULL;
    return aacdec_open(asc, 2, NULL, 0);
}

static double pow43(int v) { return pow(abs(v), 4.0 / 3.0) * (v < 0 ? -1 : 1); }
static double gainof(int sf) { return pow(2.0, 0.25 * (sf - 100)); }

/* ---------------------------------------------------------------- tests */

TEST(aacdec_imdct_matches_the_definition) {
    AacDec *d = open_lc(48000, 2);
    CHECK(d);
    for (int n = 256; n <= 2048; n *= 8) {
        int m = n / 2;
        float *X = malloc(sizeof(float) * (size_t)m), *x = malloc(sizeof(float) * (size_t)n);
        uint32_t s = 12345;
        for (int k = 0; k < m; k++) s = s * 1103515245u + 12345u, X[k] = (float)((int)(s >> 16) % 2000 - 1000);
        aacdec_test_imdct(d, X, x, n);
        double n0 = n / 4.0 + 0.5, worst = 0;
        for (int i = 0; i < n; i += (n == 2048 ? 7 : 1)) {
            double sum = 0;
            for (int k = 0; k < m; k++) sum += X[k] * cos(2 * PI / n * (i + n0) * (k + 0.5));
            sum *= 2.0 / n;
            worst = fmax(worst, fabs(sum - x[i]));
        }
        free(X);
        free(x);
        CHECK(worst < 0.02); /* values ~1000 * sqrt(m): float rounding only */
    }
    aacdec_close(d);
}

TEST(aacdec_windows_and_bands_match_the_encoders) {
    AacDec *d = open_lc(48000, 2);
    CHECK(d);
    AacConfig c = {48000, 2, 0, 0, 0};
    AacEnc *e = aac_open(&c, NULL, 0);
    CHECK(e);
    for (int sh = 0; sh < 2; sh++)
        for (int shape = 0; shape < 2; shape++) {
            const float *a = aacdec_test_window(d, sh, shape), *b = aac_window(e, sh, shape);
            for (int i = 0; i < (sh ? 128 : 1024); i++) CHECK(fabsf(a[i] - b[i]) < 1e-6f);
        }
    aac_close(e);
    aacdec_close(d);
    static const int rates[] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025};
    for (int i = 0; i < 8; i++)
        for (int sh = 0; sh < 2; sh++) {
            int na, nb;
            const uint16_t *a = aacdec_swb(rates[i], sh, &na), *b = aac_swb_offsets(rates[i], sh, &nb);
            CHECK(a && b);
            CHECK_INT(na, nb);
            for (int k = 0; k <= na; k++) CHECK_INT(a[k], b[k]);
        }
    static const int all[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; i++) /* every table rises and ends at the window's end */
        for (int sh = 0; sh < 2; sh++) {
            int n;
            const uint16_t *a = aacdec_swb(all[i], sh, &n);
            CHECK(a);
            CHECK_INT(a[0], 0);
            CHECK_INT(a[n], sh ? 128 : 1024);
            for (int k = 0; k < n; k++) CHECK(a[k + 1] > a[k] && (a[k + 1] - a[k]) % 4 == 0);
        }
}

TEST(aacdec_reads_configs) {
    AacDecInfo in;
    uint8_t lc[2];
    CHECK_INT(aacdec_make_config(44100, 2, lc), 2);
    CHECK(aacdec_config(lc, 2, &in, NULL, 0));
    CHECK_INT(in.object_type, 2);
    CHECK_INT(in.sample_rate, 44100);
    CHECK_INT(in.channels, 2);
    CHECK(!in.sbr);
    CHECK_INT(aacdec_make_config(48000, 7, lc), 0);
    /* HE-AAC, explicit: AOT 5, core 24 kHz stereo, extension 48 kHz, core AOT 2 */
    Wr w = {0};
    put(&w, 5, 5), put(&w, 6, 4), put(&w, 2, 4), put(&w, 3, 4), put(&w, 2, 5), put(&w, 0, 3);
    CHECK(aacdec_config(w.b, (size_t)wr_bytes(&w), &in, NULL, 0));
    CHECK(in.sbr);
    CHECK_INT(in.object_type, 5);
    CHECK_INT(in.sample_rate, 24000);
    CHECK_INT(in.ext_rate, 48000);
    /* HE-AAC v2 (29) */
    memset(&w, 0, sizeof w);
    put(&w, 29, 5), put(&w, 6, 4), put(&w, 1, 4), put(&w, 3, 4), put(&w, 2, 5), put(&w, 0, 3);
    CHECK(aacdec_config(w.b, (size_t)wr_bytes(&w), &in, NULL, 0));
    CHECK_INT(in.object_type, 29);
    CHECK_INT(in.channels, 1);
    /* backward compatible: an LC config, then 0x2b7, AOT 5, present, 48 kHz */
    memset(&w, 0, sizeof w);
    put(&w, 2, 5), put(&w, 6, 4), put(&w, 2, 4), put(&w, 0, 3);
    put(&w, 0x2b7, 11), put(&w, 5, 5), put(&w, 1, 1), put(&w, 3, 4);
    CHECK(aacdec_config(w.b, (size_t)wr_bytes(&w), &in, NULL, 0));
    CHECK(in.sbr);
    CHECK_INT(in.sample_rate, 24000);
    CHECK_INT(in.ext_rate, 48000);
    /* an explicit rate (index 15) uses the nearest table */
    memset(&w, 0, sizeof w);
    put(&w, 2, 5), put(&w, 15, 4), put(&w, 47000, 24), put(&w, 1, 4), put(&w, 0, 3);
    CHECK(aacdec_config(w.b, (size_t)wr_bytes(&w), &in, NULL, 0));
    CHECK_INT(in.sample_rate, 47000);
    /* config 0 + a program config element: 5.1 */
    memset(&w, 0, sizeof w);
    put(&w, 2, 5), put(&w, 3, 4), put(&w, 0, 4), put(&w, 0, 3);
    put(&w, 0, 4), put(&w, 1, 2), put(&w, 3, 4);                 /* tag, object, rate */
    put(&w, 2, 4), put(&w, 0, 4), put(&w, 1, 4), put(&w, 1, 2); /* 2 front, 0 side, 1 back, 1 lfe */
    put(&w, 0, 3), put(&w, 0, 4), put(&w, 0, 1), put(&w, 0, 1), put(&w, 0, 1);
    put(&w, 0, 1), put(&w, 0, 4), put(&w, 1, 1), put(&w, 1, 4); /* C, L R */
    put(&w, 1, 1), put(&w, 2, 4);                               /* Ls Rs */
    put(&w, 0, 4);                                              /* LFE */
    w.pos = (w.pos + 7) & ~7;
    put(&w, 0, 8); /* no comment */
    CHECK(aacdec_config(w.b, (size_t)wr_bytes(&w), &in, NULL, 0));
    CHECK_INT(in.channels, 6);
    char err[96] = "";
    /* refused: Main, LTP, 960-sample frames, nothing */
    uint8_t main_[2] = {0x09, 0x90}, ltp[2] = {0x21, 0x90}, f960[2] = {0x11, 0x94};
    CHECK(!aacdec_config(main_, 2, &in, err, sizeof err));
    CHECK_HAS(err, "Main");
    CHECK(!aacdec_config(ltp, 2, &in, err, sizeof err));
    CHECK(!aacdec_config(f960, 2, &in, err, sizeof err));
    CHECK_HAS(err, "960");
    CHECK(!aacdec_config(NULL, 0, &in, NULL, 0));
    CHECK(!aacdec_open(main_, 2, NULL, 0));
}

typedef struct {
    uint8_t *data;
    int *size;
    int n;
} Units;

/* Our encoder's units for n samples per channel of pcm. */
static bool encode_units(int rate, int ch, int shape, const float *pcm, int n, Units *u) {
    AacConfig c = {rate, ch, 0, 0, shape};
    AacEnc *e = aac_open(&c, NULL, 0);
    if (!e) return false;
    int blocks = (n + AAC_FRAME - 1) / AAC_FRAME, bytes = 0;
    u->data = malloc((size_t)(blocks + 3) * AAC_MAX_AU);
    u->size = malloc(sizeof(int) * (size_t)(blocks + 3));
    u->n = 0;
    float blk[AAC_FRAME * 2];
    for (int b = 0; b <= blocks + 2; b++) {
        int got;
        if (b < blocks) {
            for (int i = 0; i < AAC_FRAME * ch; i++) {
                long at = (long)b * AAC_FRAME * ch + i;
                blk[i] = at < (long)n * ch ? pcm[at] : 0;
            }
            got = aac_encode(e, blk, u->data + bytes, AAC_MAX_AU);
        } else {
            got = aac_flush(e, u->data + bytes, AAC_MAX_AU);
        }
        if (got > 0) u->size[u->n++] = got, bytes += got;
    }
    aac_close(e);
    return u->n > 0;
}

TEST(aacdec_our_encoders_streams_round_trip) {
    static const int rates[] = {48000, 44100, 32000, 24000, 16000, 11025};
    for (int ri = 0; ri < 6; ri++)
        for (int ch = 1; ch <= 2; ch++) {
            int rate = rates[ri], n = rate * 2;
            float *pcm = malloc(sizeof(float) * (size_t)n * (size_t)ch);
            uint32_t s = 7;
            for (int i = 0; i < n; i++)
                for (int c = 0; c < ch; c++) { /* tones, a click every half second (short windows), a little noise */
                    s = s * 1664525u + 1013904223u;
                    double t = (double)i / rate, v = 0.3 * sin(2 * PI * (440 + 220 * c) * t) + 0.1 * sin(2 * PI * 1234 * t);
                    if (i % (rate / 2) < 40) v += 0.6 * ((i & 1) ? 1 : -1);
                    pcm[(size_t)i * ch + c] = (float)(v + ((int)(s >> 20) - 2048) / 2048.0 * 0.01);
                }
            Units u;
            CHECK(encode_units(rate, ch, ri & 1, pcm, n, &u));
            AacDec *d = open_lc(rate, ch);
            Dec ref;
            CHECK(d && dec_open(&ref, rate, ch));
            float a[AACDEC_FRAME * AACDEC_MAX_CHANNELS], b[AAC_FRAME * 2];
            double err = 0, sig = 0, diff = 0;
            int off = 0, got = 0;
            for (int k = 0; k < u.n; k++) {
                CHECK_INT(aacdec_decode(d, u.data + off, (size_t)u.size[k], a, &got), AACDEC_OK);
                CHECK_INT(got, ch);
                CHECK(dec_au(&ref, u.data + off, u.size[k], b));
                off += u.size[k];
                for (int i = 0; i < AAC_FRAME * ch; i++) {
                    diff = fmax(diff, fabs(a[i] - b[i]));
                    long at = (long)(k - 1) * AAC_FRAME * ch + i; /* unit k = input block k - 1 */
                    if (k >= 2 && at < (long)n * ch) err += (a[i] - pcm[at]) * (a[i] - pcm[at]), sig += pcm[at] * pcm[at];
                }
            }
            CHECK(diff < 1e-4);                     /* the same as the encoder's test decoder */
            CHECK(10 * log10(sig / err) > 15.0);    /* and the sound that went in */
            CHECK(aacdec_stats(d)->short_frames > 0); /* the clicks were short windows */
            dec_close(&ref);
            aacdec_close(d);
            free(u.data);
            free(u.size);
            free(pcm);
        }
}

TEST(aacdec_noise_substitution_has_its_energy) {
    AacDec *d = open_lc(48000, 1);
    CHECK(d);
    TIcs *s = new_ics(20);
    s->cb[0][5] = 13, s->sf[0][5] = 60;  /* noise: 2^(60/4) amplitude */
    s->cb[0][9] = 13, s->sf[0][9] = 44;
    s->cb[0][12] = 1, s->sf[0][12] = 110; /* and a normal band around them */
    int lo, n;
    const uint16_t *swb = aacdec_swb(48000, false, &n);
    lo = swb[12];
    s->q[lo] = 1, s->q[lo + 1] = -1;
    Wr w = {0};
    put_sce(&w, s, 48000, false);
    put_end(&w);
    float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
    int ch;
    CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &ch), AACDEC_OK);
    const float *sp = aacdec_test_spectrum(d, 0);
    double e5 = 0, e9 = 0, e0 = 0;
    for (int k = swb[5]; k < swb[6]; k++) e5 += sp[k] * sp[k];
    for (int k = swb[9]; k < swb[10]; k++) e9 += sp[k] * sp[k];
    for (int k = swb[0]; k < swb[5]; k++) e0 += sp[k] * sp[k];
    CHECK(fabs(e5 / pow(2.0, 60 / 2.0) - 1) < 1e-3);
    CHECK(fabs(e9 / pow(2.0, 44 / 2.0) - 1) < 1e-3);
    CHECK(e0 == 0);
    CHECK(fabs(sp[lo] - gainof(110)) < 1e-3 * gainof(110) && fabs(sp[lo + 1] + gainof(110)) < 1e-3 * gainof(110));
    CHECK_INT(aacdec_stats(d)->pns, 1);
    free(s);
    aacdec_close(d);
}

TEST(aacdec_intensity_and_mid_side) {
    AacDec *d = open_lc(44100, 2);
    CHECK(d);
    int nb;
    const uint16_t *swb = aacdec_swb(44100, false, &nb);
    TIcs *l = new_ics(12), *r = new_ics(12);
    for (int b = 0; b < 12; b++) {
        l->cb[0][b] = 5, l->sf[0][b] = 100 + b;
        for (int k = swb[b]; k < swb[b + 1]; k++) l->q[k] = (k % 7) - 3;
    }
    /* right: band 2 IS in phase (15) pos 8, band 3 out of phase (14) pos -4, band 4 IS + ms_used (inverted),
       band 6 M/S with real values, the rest zero */
    r->cb[0][2] = 15, r->sf[0][2] = 8;
    r->cb[0][3] = 14, r->sf[0][3] = -4;
    r->cb[0][4] = 15, r->sf[0][4] = -4;
    r->cb[0][6] = 1, r->sf[0][6] = 100;
    for (int k = swb[6]; k < swb[7]; k++) r->q[k] = (k & 1) ? 1 : -1;
    bool ms[8 * 64] = {0};
    ms[4] = true, ms[6] = true;
    Wr w = {0};
    put_cpe(&w, l, r, 44100, 1, ms);
    put_end(&w);
    float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
    int ch;
    CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &ch), AACDEC_OK);
    CHECK_INT(ch, 2);
    const float *L = aacdec_test_spectrum(d, 0), *R = aacdec_test_spectrum(d, 1);
    for (int k = swb[2]; k < swb[3]; k++) CHECK(fabs(R[k] - L[k] * pow(2.0, -2.0)) < 1e-3 * (1 + fabs(L[k])));
    for (int k = swb[3]; k < swb[4]; k++) CHECK(fabs(R[k] + L[k] * 2.0) < 1e-3 * (1 + fabs(L[k])));
    for (int k = swb[4]; k < swb[5]; k++) CHECK(fabs(R[k] + L[k] * 2.0) < 1e-3 * (1 + fabs(L[k]))); /* inverted */
    for (int k = swb[6]; k < swb[7]; k++) { /* L = M + S, R = M - S */
        double m = pow43(l->q[k]) * gainof(106), sd = pow43(r->q[k]) * gainof(100);
        CHECK(fabs(L[k] - (m + sd)) < 1e-3 * (1 + fabs(m)));
        CHECK(fabs(R[k] - (m - sd)) < 1e-3 * (1 + fabs(m)));
    }
    for (int k = swb[7]; k < swb[8]; k++) CHECK(R[k] == 0);
    CHECK_INT(aacdec_stats(d)->is, 1);
    CHECK_INT(aacdec_stats(d)->ms, 1);
    free(l);
    free(r);
    aacdec_close(d);
}

TEST(aacdec_pulses_and_escapes) {
    AacDec *d = open_lc(48000, 1);
    CHECK(d);
    int nb;
    const uint16_t *swb = aacdec_swb(48000, false, &nb);
    TIcs *s = new_ics(30);
    for (int b = 0; b < 30; b++) s->cb[0][b] = 11, s->sf[0][b] = 100;
    s->q[3] = 5, s->q[40] = -17, s->q[41] = 300, s->q[100] = 8191, s->q[101] = -4096, s->q[200] = 0;
    /* pulses from band 20: +3 at its start, +2 four further on, -(1) seven further on an empty value */
    s->npulse = 3, s->pstart = 20, s->poff[0] = 0, s->pamp[0] = 3, s->poff[1] = 4, s->pamp[1] = 2, s->poff[2] = 7, s->pamp[2] = 1;
    int p0 = swb[20];
    s->q[p0] = 4, s->q[p0 + 4] = -6;
    Wr w = {0};
    put_sce(&w, s, 48000, false);
    put_end(&w);
    float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
    int ch;
    CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &ch), AACDEC_OK);
    const float *sp = aacdec_test_spectrum(d, 0);
    int at[] = {3, 40, 41, 100, 101};
    for (int i = 0; i < 5; i++) CHECK(fabs(sp[at[i]] - pow43(s->q[at[i]])) < 1e-4 * fabs(pow43(s->q[at[i]])));
    CHECK(fabs(sp[p0] - pow43(7)) < 1e-3);
    CHECK(fabs(sp[p0 + 4] - pow43(-8)) < 1e-3);
    CHECK(fabs(sp[p0 + 11] - pow43(-1)) < 1e-3);
    CHECK_INT(aacdec_stats(d)->pulse, 1);
    free(s);
    aacdec_close(d);
}

TEST(aacdec_tns_filters_the_spectrum) {
    for (int dir = 0; dir < 2; dir++) {
        AacDec *d = open_lc(48000, 1);
        CHECK(d);
        int nb;
        const uint16_t *swb = aacdec_swb(48000, false, &nb);
        TIcs *s = new_ics(40);
        for (int b = 0; b < 40; b++) s->cb[0][b] = 1, s->sf[0][b] = 100;
        /* one filter over the top 10 bands of the 49 (TNS stops at max_sfb = 40: bands 39..40 -> none above 40),
           so length 20 covers bands 29..48, clipped to 29..39 */
        int lo = swb[29], hi = swb[40], imp = dir ? hi - 1 : lo;
        s->q[imp] = 1;
        s->tns_n = 1, s->tns_res = 1, s->tns_len = 20, s->tns_order = 1, s->tns_dir = dir, s->tns_compress = 0;
        s->tns_coef[0] = 5; /* 4-bit: 5 -> sin(5 / iq), iq = 7.5 / (pi/2) */
        Wr w = {0};
        put_sce(&w, s, 48000, false);
        put_end(&w);
        float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
        int ch;
        CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &ch), AACDEC_OK);
        const float *sp = aacdec_test_spectrum(d, 0);
        double k = sin(5 / (7.5 / (PI / 2))), want = 1;
        for (int i = 0; i < hi - lo; i++) { /* y(n) = x(n) - k y(n-1): an impulse rings as (-k)^n */
            int at = dir ? imp - i : imp + i;
            CHECK(fabs(sp[at] - want) < 1e-4);
            want *= -k;
        }
        CHECK(sp[dir ? lo - 1 : hi] == 0); /* not past the region */
        CHECK_INT(aacdec_stats(d)->tns, 1);
        free(s);
        aacdec_close(d);
    }
}

TEST(aacdec_grouped_short_windows) {
    AacDec *d = open_lc(32000, 1);
    CHECK(d);
    int nb;
    const uint16_t *swb = aacdec_swb(32000, true, &nb);
    TIcs *s = new_ics(10);
    s->seq = 2;
    s->grouping = 0x5A; /* 1011010: windows 0 | 1 2 | 3 4 | 5 | 6 7 ... as the bits say */
    int glen[8], ng = t_groups(s, glen), w0 = 0;
    for (int g = 0; g < ng; g++) {
        for (int b = 0; b < 10; b++) s->cb[g][b] = 7, s->sf[g][b] = 90 + g * 3 + b;
        for (int win = w0; win < w0 + glen[g]; win++)
            for (int k = 0; k < swb[10]; k++) s->q[128 * win + k] = (win + k) % 6;
        w0 += glen[g];
    }
    Wr w = {0};
    put_sce(&w, s, 32000, false);
    put_end(&w);
    float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
    int ch;
    CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &ch), AACDEC_OK);
    const float *sp = aacdec_test_spectrum(d, 0);
    w0 = 0;
    for (int g = 0; g < ng; g++) {
        for (int win = w0; win < w0 + glen[g]; win++)
            for (int b = 0; b < 10; b++)
                for (int k = swb[b]; k < swb[b + 1]; k++) {
                    double want = pow43(s->q[128 * win + k]) * gainof(90 + g * 3 + b);
                    CHECK(fabs(sp[128 * win + k] - want) < 1e-4 * (1 + fabs(want)));
                }
        w0 += glen[g];
    }
    CHECK_INT(aacdec_stats(d)->short_frames, 1);
    free(s);
    aacdec_close(d);
}

TEST(aacdec_five_one_layouts_and_other_elements) {
    AacDec *d = open_lc(48000, 6);
    CHECK(d);
    TIcs *z = new_ics(0), *c = new_ics(4), *lfe = new_ics(2);
    for (int b = 0; b < 4; b++) c->cb[0][b] = 1, c->sf[0][b] = 140;
    for (int k = 0; k < 16; k++) c->q[k] = 1;
    lfe->cb[0][0] = 1, lfe->sf[0][0] = 130, lfe->q[0] = 1;
    bool ms[8 * 64] = {0};
    for (int pass = 0; pass < 4; pass++) {
        Wr w = {0};
        put(&w, 4, 3), put(&w, 0, 4), put(&w, 1, 1), put(&w, 3, 8); /* a data stream element, byte aligned */
        w.pos = (w.pos + 7) & ~7;
        put(&w, 0xABCDEF, 24);
        put_sce(&w, c, 48000, false);
        put_cpe(&w, z, z, 48000, 0, ms);
        put_cpe(&w, z, z, 48000, 0, ms);
        put_sce(&w, lfe, 48000, true);
        put(&w, 6, 3), put(&w, 2, 4), put(&w, 0, 8), put(&w, 0, 8); /* fill: 2 bytes */
        put_end(&w);
        AacDecLayout lay = pass == 0 ? AACDEC_LAYOUT_NATIVE : pass == 1 ? AACDEC_LAYOUT_WAV : pass == 2 ? AACDEC_LAYOUT_STEREO : AACDEC_LAYOUT_MONO;
        aacdec_set_layout(d, lay);
        aacdec_reset(d);
        float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
        int ch;
        CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &ch), AACDEC_OK);
        double e[8] = {0};
        for (int i = 0; i < AACDEC_FRAME; i++)
            for (int k = 0; k < ch; k++) e[k] += out[i * ch + k] * out[i * ch + k];
        if (pass == 0) { /* C L R Ls Rs LFE */
            CHECK_INT(ch, 6);
            CHECK(e[0] > 0 && e[5] > 0 && e[1] == 0 && e[2] == 0 && e[3] == 0 && e[4] == 0);
        } else if (pass == 1) { /* L R C LFE Ls Rs */
            CHECK_INT(ch, 6);
            CHECK(e[2] > 0 && e[3] > 0 && e[0] == 0 && e[1] == 0 && e[4] == 0 && e[5] == 0);
        } else if (pass == 2) { /* the centre in both, the LFE gone */
            CHECK_INT(ch, 2);
            CHECK(e[0] > 0 && fabs(e[0] - e[1]) < 1e-9 * e[0]);
        } else {
            CHECK_INT(ch, 1);
            CHECK(e[0] > 0);
        }
    }
    free(z);
    free(c);
    free(lfe);
    aacdec_close(d);
}

TEST(aacdec_adts_sbr_fill_and_damage) {
    AacDec *d = open_lc(24000, 1);
    CHECK(d);
    TIcs *s = new_ics(8);
    for (int b = 0; b < 8; b++) s->cb[0][b] = 3, s->sf[0][b] = 120;
    for (int k = 0; k < 40; k++) s->q[k] = k % 3;
    Wr w = {0};
    put_sce(&w, s, 24000, false);
    put(&w, 6, 3), put(&w, 3, 4), put(&w, 13, 4), put(&w, 0, 20); /* fill with EXT_SBR_DATA: implicit HE-AAC */
    put_end(&w);
    float a[AACDEC_FRAME * AACDEC_MAX_CHANNELS], b[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
    int ch;
    CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), a, &ch), AACDEC_OK);
    CHECK(aacdec_info(d)->sbr);
    CHECK_INT(aacdec_info(d)->ext_rate, 48000);
    /* the same unit behind an ADTS header decodes alike */
    uint8_t adts[2048];
    CHECK_INT(aac_adts_header(24000, 1, wr_bytes(&w), adts), 7);
    memcpy(adts + 7, w.b, (size_t)wr_bytes(&w));
    aacdec_reset(d);
    CHECK_INT(aacdec_decode(d, w.b, (size_t)wr_bytes(&w), a, &ch), AACDEC_OK);
    aacdec_reset(d);
    CHECK_INT(aacdec_decode(d, adts, (size_t)wr_bytes(&w) + 7, b, &ch), AACDEC_OK);
    CHECK(!memcmp(a, b, sizeof(float) * AACDEC_FRAME));
    /* damage: every cut and random bytes come back as silence of the last channel count, never a crash */
    for (int cut = 0; cut < wr_bytes(&w) - 1; cut++) {
        int rc = aacdec_decode(d, w.b, (size_t)cut, a, &ch);
        CHECK(rc == AACDEC_OK || rc == AACDEC_ERR_BITSTREAM);
        if (rc) CHECK_INT(ch, 1);
    }
    uint32_t x = 99;
    uint8_t junk[512];
    for (int t = 0; t < 3000; t++) {
        int n = (int)(x % 500) + 1;
        for (int i = 0; i < n; i++) x = x * 1664525u + 1013904223u, junk[i] = (uint8_t)(x >> 24);
        aacdec_decode(d, junk, (size_t)n, a, &ch);
        CHECK(ch <= AACDEC_MAX_CHANNELS);
    }
    CHECK(aacdec_stats(d)->errors > 0);
    CHECK(aacdec_decode(d, NULL, 0, a, &ch) < 0);
    free(s);
    aacdec_close(d);
}

/* HEMMA_TEST_AAC=<a video or audio file> [HEMMA_TEST_AAC_S=seconds] [HEMMA_TEST_AAC_OUT=<file.f32>] [HEMMA_TEST_AAC_WAV=1]:
   decode its first AAC track's start and write float samples, native order (or WAV order) (compare with ffmpeg -f f32le -c:a pcm_f32le). */
static bool file_read_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread((FILE *)ctx, off, dst, n); }

TEST(aacdec_real_file_when_given) {
    const char *path = getenv("HEMMA_TEST_AAC");
    if (!path || !*path) return;
    int64_t size = 0;
    CHECK(path_stat(path, &size, NULL, NULL));
    FILE *f = file_open(path, "rb");
    CHECK(f);
    MkvReader rd = {f, size, file_read_cb};
    char err[128] = "";
    Mkv *m = mkv_open(&rd, err, sizeof err);
    CHECK(m);
    const MkvTrack *t = NULL;
    for (int i = 0; i < mkv_track_count(m); i++)
        if (!strncmp(mkv_track(m, i)->codec, "A_AAC", 5)) {
            t = mkv_track(m, i);
            break;
        }
    CHECK(t);
    AacDec *d = aacdec_open(t->priv, t->priv_len, err, sizeof err);
    if (!d) printf("aac: %s\n", err);
    CHECK(d);
    if (getenv("HEMMA_TEST_AAC_WAV")) aacdec_set_layout(d, AACDEC_LAYOUT_WAV);
    const AacDecInfo *in = aacdec_info(d);
    double secs = getenv("HEMMA_TEST_AAC_S") ? atof(getenv("HEMMA_TEST_AAC_S")) : 30;
    const char *outp = getenv("HEMMA_TEST_AAC_OUT");
    FILE *o = outp ? fopen(outp, "wb") : NULL;
    MkvFrame fr;
    long frames = 0, max = (long)(secs * in->sample_rate / AACDEC_FRAME);
    float pcm[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
    int ch = 0, bad = 0;
    while (frames < max && mkv_next(m, &fr) == 1) {
        if (fr.track != t->number) continue;
        if (aacdec_decode(d, fr.data, fr.size, pcm, &ch) != AACDEC_OK) bad++;
        if (o && ch) fwrite(pcm, sizeof(float), (size_t)AACDEC_FRAME * (size_t)ch, o);
        frames++;
    }
    const AacDecStats *st = aacdec_stats(d);
    printf("aac: %s, object %d, core %d Hz (plays at %d), config %d, %d channels; %ld units, %d bad; short %d tns %d pns %d is %d ms %d pulse %d sbr fills %d\n",
           path, in->object_type, in->sample_rate, in->ext_rate, in->config, ch, frames, bad, st->short_frames, st->tns, st->pns, st->is, st->ms,
           st->pulse, st->fill_sbr);
    if (o) fclose(o);
    aacdec_close(d);
    mkv_close(m);
    fclose(f);
    CHECK_INT(bad, 0);
}

/* HEMMA_FUZZ_RAW=<dir> ./build/test aacdec_fuzz_seeds: .bin files in <dir>/aac for tools/avfuzz.c -DAVFUZZ_AAC - [options][config
   length][config][2-byte length + unit]...: our encoder's streams and the hand-written frames above (every tool). */
typedef struct {
    uint8_t b[65536];
    size_t n;
} Seed;

static void seed_start(Seed *s, uint8_t opt, const uint8_t *asc, size_t alen) {
    s->n = 0;
    s->b[s->n++] = opt;
    s->b[s->n++] = (uint8_t)alen;
    memcpy(s->b + s->n, asc, alen);
    s->n += alen;
}

static void seed_unit(Seed *s, const uint8_t *u, size_t len) {
    if (s->n + 2 + len > sizeof s->b) return;
    s->b[s->n++] = (uint8_t)(len >> 8), s->b[s->n++] = (uint8_t)len;
    memcpy(s->b + s->n, u, len);
    s->n += len;
}

static bool seed_save(const char *dir, const char *name, const Seed *s) {
    if (!dir) return true;
    char path[1100];
    snprintf(path, sizeof path, "%s/aac/%s.bin", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(s->b, 1, s->n, f) == s->n;
    return fclose(f) == 0 && ok;
}

TEST(aacdec_fuzz_seeds) {
    const char *raw = getenv("HEMMA_FUZZ_RAW");
    if (raw && !*raw) raw = NULL;
    if (raw) {
        char path[1024];
        snprintf(path, sizeof path, "%s/aac", raw);
        CHECK(dir_make(path));
    }
    Seed *s = malloc(sizeof *s);
    CHECK(s);
    uint8_t asc[2];
    for (int k = 0; k < 2; k++) { /* our encoder: stereo 48 kHz with clicks (short windows), mono 24 kHz */
        int rate = k ? 24000 : 48000, ch = k ? 1 : 2, n = rate / 2;
        float *pcm = malloc(sizeof(float) * (size_t)n * (size_t)ch);
        for (int i = 0; i < n * ch; i++) pcm[i] = (float)(0.3 * sin(i * 0.05) + (i % 4000 < 30 ? 0.5 : 0));
        Units u;
        CHECK(encode_units(rate, ch, k, pcm, n, &u));
        CHECK(aacdec_make_config(rate, ch, asc) == 2);
        seed_start(s, (uint8_t)(k ? 0x13 : 0x00), asc, 2);
        for (int i = 0, off = 0; i < u.n; off += u.size[i], i++) seed_unit(s, u.data + off, (size_t)u.size[i]);
        CHECK(seed_save(raw, k ? "ours-mono24" : "ours-stereo48", s));
        free(u.data);
        free(u.size);
        free(pcm);
    }
    /* hand-written frames, each with its config */
    TIcs *a = new_ics(20), *b = new_ics(20);
    int nb;
    const uint16_t *swb = aacdec_swb(48000, false, &nb);
    for (int i = 0; i < 20; i++) {
        a->cb[0][i] = (i % 11) + 1, a->sf[0][i] = 100 + i;
        for (int k = swb[i]; k < swb[i + 1]; k++) a->q[k] = fits(a->cb[0][i], k);
    }
    a->cb[0][3] = 13, a->sf[0][3] = 50, a->cb[0][4] = 13, a->sf[0][4] = 61;
    a->npulse = 2, a->pstart = 6, a->poff[0] = 1, a->pamp[0] = 5, a->poff[1] = 3, a->pamp[1] = 2;
    a->tns_n = 1, a->tns_res = 1, a->tns_len = 12, a->tns_order = 4, a->tns_dir = 1, a->tns_compress = 0;
    a->tns_coef[0] = 3, a->tns_coef[1] = -2, a->tns_coef[2] = 7, a->tns_coef[3] = -8;
    *b = *a;
    b->npulse = 0, b->tns_n = 0;
    b->cb[0][8] = 15, b->sf[0][8] = 6, b->cb[0][9] = 14, b->sf[0][9] = -3, b->cb[0][10] = 13, b->sf[0][10] = 40;
    bool ms[8 * 64] = {0};
    for (int i = 0; i < 20; i += 3) ms[i] = true;
    for (int v = 0; v < 4; v++) {
        Wr w = {0};
        int ch = v == 3 ? 6 : v == 1 ? 1 : 2;
        if (v == 1) put_sce(&w, a, 48000, false);
        if (v == 0 || v == 2) {
            TIcs al = *a; /* a pair shares one window: no pulses or TNS of its own needed */
            put_cpe(&w, &al, b, 48000, v == 2 ? 2 : 1, ms);
        }
        if (v == 3) {
            put(&w, 4, 3), put(&w, 1, 4), put(&w, 0, 1), put(&w, 2, 8), put(&w, 0xBEEF, 16);
            put_sce(&w, a, 48000, false);
            put_cpe(&w, a, b, 48000, 1, ms);
            put_cpe(&w, b, a, 48000, 0, ms);
            put_sce(&w, a, 48000, true);
            put(&w, 6, 3), put(&w, 3, 4), put(&w, 13, 4), put(&w, 0, 20);
        }
        put_end(&w);
        CHECK(aacdec_make_config(48000, ch, asc) == 2);
        AacDec *d = aacdec_open(asc, 2, NULL, 0); /* a seed the decoder takes whole */
        float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
        int got;
        CHECK(d && aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &got) == AACDEC_OK && got == ch);
        aacdec_close(d);
        seed_start(s, (uint8_t)(v * 0x21), asc, 2);
        for (int r = 0; r < 4; r++) seed_unit(s, w.b, (size_t)wr_bytes(&w));
        static const char *const names[] = {"tools-cpe-ms", "tools-sce", "tools-cpe-msall", "tools-51-dse-fil"};
        CHECK(seed_save(raw, names[v], s));
    }
    { /* grouped short windows, at 32 kHz, behind ADTS headers, with an HE-AAC (explicit) config */
        TIcs *sh = new_ics(10);
        sh->seq = 2, sh->grouping = 0x2D;
        int glen[8], ng = t_groups(sh, glen), w0 = 0;
        const uint16_t *ss = aacdec_swb(32000, true, &nb);
        for (int g = 0; g < ng; g++) {
            for (int i = 0; i < 10; i++) sh->cb[g][i] = 1 + (g + i) % 11, sh->sf[g][i] = 95 + g;
            for (int win = w0; win < w0 + glen[g]; win++)
                for (int i = 0; i < 10; i++)
                    for (int k = ss[i]; k < ss[i + 1]; k++) sh->q[128 * win + k] = fits(sh->cb[g][i], win * 3 + k);
            w0 += glen[g];
        }
        Wr w = {0};
        put_sce(&w, sh, 32000, false);
        put_end(&w);
        Wr c = {0};
        put(&c, 5, 5), put(&c, 5, 4), put(&c, 1, 4), put(&c, 3, 4), put(&c, 2, 5), put(&c, 0, 3);
        AacDec *d = aacdec_open(c.b, (size_t)wr_bytes(&c), NULL, 0);
        float out[AACDEC_FRAME * AACDEC_MAX_CHANNELS];
        int got;
        CHECK(d && aacdec_decode(d, w.b, (size_t)wr_bytes(&w), out, &got) == AACDEC_OK && got == 1);
        aacdec_close(d);
        seed_start(s, 0x42, c.b, (size_t)wr_bytes(&c));
        uint8_t unit[4096];
        CHECK(aac_adts_header(32000, 1, wr_bytes(&w), unit) == 7);
        memcpy(unit + 7, w.b, (size_t)wr_bytes(&w));
        for (int r = 0; r < 3; r++) seed_unit(s, unit, (size_t)wr_bytes(&w) + 7);
        CHECK(seed_save(raw, "short-adts-he", s));
        free(sh);
    }
    free(a);
    free(b);
    free(s);
}
