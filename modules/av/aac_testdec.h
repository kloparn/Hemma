/* modules/av/aac_testdec.h — a small AAC-LC decoder written from ISO/IEC 14496-3 (raw_data_block,
 * SCE/CPE, ics_info, sections, scalefactors, spectral data with every codebook and escapes,
 * M/S, IMDCT with every window sequence and shape). Only for checking the encoder: the
 * tests (aac_test.c) and tools/aaccheck.c include it; it is not part of the app. It uses
 * the encoder's exposed tables and transforms, which the tests check against their
 * definitions, and Chrome's decoder checks the whole on top. */
#ifndef HEMMA_AV_AAC_TESTDEC_H
#define HEMMA_AV_AAC_TESTDEC_H

#include "aac.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ bit reader */

typedef struct {
    const uint8_t *p;
    int pos, n; /* bits */
    bool over;
} BitR;

static unsigned getb(BitR *r, int k) {
    unsigned v = 0;
    while (k-- > 0) {
        unsigned bit = 0;
        if (r->pos < r->n) bit = (r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u;
        else r->over = true;
        v = (v << 1) | bit;
        r->pos++;
    }
    return v;
}

/* ------------------------------------------------------- Huffman decoding */

typedef struct {
    int16_t child[2], sym;
} Node;
typedef struct {
    Node node[700];
    int n;
} Tree;

static Tree trees[12]; /* [0] = scalefactors, [1..11] = spectrum */

static void tree_add(Tree *t, uint32_t code, int len, int sym) {
    int at = 0;
    for (int i = len - 1; i >= 0; i--) {
        int b = (int)((code >> i) & 1u);
        if (t->node[at].child[b] < 0) {
            int nn = t->n++;
            t->node[nn] = (Node){{-1, -1}, -1};
            t->node[at].child[b] = (int16_t)nn;
        }
        at = t->node[at].child[b];
    }
    t->node[at].sym = (int16_t)sym;
}

static void build_trees(void) {
    static bool done;
    if (done) return;
    done = true;
    for (int cb = 0; cb < 12; cb++) {
        Tree *t = &trees[cb];
        t->n = 1;
        t->node[0] = (Node){{-1, -1}, -1};
        if (cb == 0) {
            const uint32_t *code;
            const uint8_t *len;
            aac_sf_codebook(&code, &len);
            for (int i = 0; i < 121; i++) tree_add(t, code[i], len[i], i);
        } else {
            const AacCodebook *b = aac_codebook(cb);
            for (int i = 0; i < b->size; i++) tree_add(t, b->code[i], b->len[i], i);
        }
    }
}

static int huff(BitR *r, const Tree *t) {
    int at = 0;
    while (t->node[at].sym < 0) {
        int nx = t->node[at].child[getb(r, 1)];
        if (nx < 0 || r->over) return -1;
        at = nx;
    }
    return t->node[at].sym;
}

/* --------------------------------------------------------- the decoder */

typedef struct {
    AacEnc *tx; /* windows and the inverse MDCT */
    int sr, channels;
    const uint16_t *swb_l, *swb_s;
    int nl, ns;
    float overlap[2][1024];
    int prev_shape[2];
    /* the access unit being decoded */
    int seq, shape, max_sfb, ngroups, glen[8], ms_mode;
    int cb[2][8][64], sf[2][8][64];
    bool ms[8][64];
    float coef[2][1024]; /* window-major */
    int short_units, frames;
} Dec;

static bool dec_open(Dec *d, int sr, int ch) {
    memset(d, 0, sizeof *d);
    AacConfig c = {sr, ch, 0, 0, 0};
    d->tx = aac_open(&c, NULL, 0);
    d->sr = sr;
    d->channels = ch;
    d->swb_l = aac_swb_offsets(sr, false, &d->nl);
    d->swb_s = aac_swb_offsets(sr, true, &d->ns);
    build_trees();
    return d->tx && d->swb_l;
}

static void dec_close(Dec *d) { aac_close(d->tx); }

static bool ics_info(Dec *d, BitR *r) {
    if (getb(r, 1)) return false; /* ics_reserved_bit */
    d->seq = (int)getb(r, 2);
    d->shape = (int)getb(r, 1);
    d->ngroups = 1;
    d->glen[0] = 1;
    if (d->seq == 2) {
        d->max_sfb = (int)getb(r, 4);
        unsigned grouping = getb(r, 7);
        for (int w = 1; w < 8; w++) {
            if (grouping & (1u << (6 - (w - 1)))) d->glen[d->ngroups - 1]++;
            else d->glen[d->ngroups++] = 1;
        }
        return d->max_sfb <= d->ns;
    }
    d->max_sfb = (int)getb(r, 6);
    if (getb(r, 1)) return false; /* predictor_data_present: not in AAC-LC */
    return d->max_sfb <= d->nl;
}

static int unpack(BitR *r, int cb, int *v) { /* one codeword -> 4 or 2 values; false-y on error */
    int sym = huff(r, &trees[cb]);
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
    if (aac_codebook(cb)->is_unsigned)
        for (int i = 0; i < n; i++)
            if (v[i] && getb(r, 1)) v[i] = -v[i];
    if (cb == 11)
        for (int i = 0; i < n; i++) {
            if (abs(v[i]) != 16) continue;
            int ones = 0;
            while (getb(r, 1)) {
                if (++ones > 8 || r->over) return 0;
            }
            int mag = (1 << (ones + 4)) + (int)getb(r, ones + 4);
            v[i] = v[i] < 0 ? -mag : mag;
        }
    return r->over ? 0 : n;
}

static bool ics(Dec *d, BitR *r, int c, bool common) {
    int gg = (int)getb(r, 8);
    if (!common && !ics_info(d, r)) return false;
    bool sh = d->seq == 2;
    int sbits = sh ? 3 : 5, esc = sh ? 7 : 31;
    const uint16_t *sw = sh ? d->swb_s : d->swb_l;
    for (int g = 0; g < d->ngroups; g++) { /* section data */
        int k = 0;
        while (k < d->max_sfb) {
            int cbk = (int)getb(r, 4), len = 0, inc;
            if (cbk > 11) return false; /* no PNS / intensity in our streams */
            while ((inc = (int)getb(r, sbits)) == esc && !r->over) len += esc;
            len += inc;
            if (len == 0 || k + len > d->max_sfb || r->over) return false;
            for (int i = k; i < k + len; i++) d->cb[c][g][i] = cbk;
            k += len;
        }
    }
    int sfv = gg; /* scalefactor data */
    for (int g = 0; g < d->ngroups; g++)
        for (int i = 0; i < d->max_sfb; i++) {
            if (!d->cb[c][g][i]) continue;
            int dd = huff(r, &trees[0]);
            if (dd < 0) return false;
            sfv += dd - 60;
            if (sfv < 0 || sfv > 255) return false;
            d->sf[c][g][i] = sfv;
        }
    if (getb(r, 1) || getb(r, 1) || getb(r, 1)) return false; /* pulse, TNS, gain control */
    memset(d->coef[c], 0, sizeof d->coef[c]);
    int w0 = 0;
    for (int g = 0; g < d->ngroups; g++) { /* spectral data */
        for (int i = 0; i < d->max_sfb; i++) {
            int cbk = d->cb[c][g][i], bw = sw[i + 1] - sw[i], n = bw * d->glen[g];
            if (!cbk) continue;
            float gain = (float)pow(2.0, 0.25 * (d->sf[c][g][i] - 100));
            int vals[4];
            for (int j = 0; j < n;) {
                int got = unpack(r, cbk, vals);
                if (!got) return false;
                for (int k = 0; k < got; k++, j++) {
                    int w = w0 + j / bw, bin = sw[i] + j % bw;
                    float mag = (float)pow(fabs((double)vals[k]), 4.0 / 3.0) * gain;
                    d->coef[c][(sh ? 128 * w : 0) + bin] = vals[k] < 0 ? -mag : mag;
                }
            }
        }
        w0 += d->glen[g];
    }
    return !r->over;
}

static float dwin(const Dec *d, int c, int n) { /* long-type synthesis windows (4.6.11.3.2) */
    const float *lp = aac_window(d->tx, false, d->prev_shape[c]), *lc = aac_window(d->tx, false, d->shape);
    const float *sp = aac_window(d->tx, true, d->prev_shape[c]), *sc = aac_window(d->tx, true, d->shape);
    switch (d->seq) {
    case 1: return n < 1024 ? lp[n] : n < 1472 ? 1.0f : n < 1600 ? sc[1599 - n] : 0.0f;
    case 3: return n < 448 ? 0.0f : n < 576 ? sp[n - 448] : n < 1024 ? 1.0f : lc[2047 - n];
    default: return n < 1024 ? lp[n] : lc[2047 - n];
    }
}

static void synth(Dec *d, int c, float *out, int stride) {
    float z[2048];
    if (d->seq != 2) {
        aac_imdct(d->tx, d->coef[c], z, 2048);
        for (int n = 0; n < 2048; n++) z[n] *= dwin(d, c, n);
    } else {
        memset(z, 0, sizeof z);
        const float *sc = aac_window(d->tx, true, d->shape);
        for (int w = 0; w < 8; w++) {
            float y[256];
            const float *left = aac_window(d->tx, true, w == 0 ? d->prev_shape[c] : d->shape);
            aac_imdct(d->tx, d->coef[c] + 128 * w, y, 256);
            for (int j = 0; j < 128; j++) {
                z[448 + 128 * w + j] += y[j] * left[j];
                z[448 + 128 * w + 128 + j] += y[128 + j] * sc[127 - j];
            }
        }
    }
    for (int n = 0; n < 1024; n++) out[n * stride] = (d->overlap[c][n] + z[n]) / 32768.0f;
    memcpy(d->overlap[c], z + 1024, sizeof(float) * 1024);
    d->prev_shape[c] = d->shape;
}

/* One raw access unit -> 1024 interleaved samples per channel. */
static bool dec_au(Dec *d, const uint8_t *au, int bytes, float *out) {
    BitR r = {au, 0, bytes * 8, false};
    int seen = 0;
    for (;;) {
        int id = (int)getb(&r, 3);
        if (r.over) return false;
        if (id == 7) break; /* END */
        getb(&r, 4);        /* element_instance_tag */
        if (id == 0 && d->channels == 1) {
            if (!ics(d, &r, 0, false)) return false;
            d->ms_mode = 0;
        } else if (id == 1 && d->channels == 2) {
            bool common = getb(&r, 1);
            memset(d->ms, 0, sizeof d->ms);
            d->ms_mode = 0;
            if (common) {
                if (!ics_info(d, &r)) return false;
                d->ms_mode = (int)getb(&r, 2);
                if (d->ms_mode == 3) return false;
                for (int g = 0; g < d->ngroups; g++)
                    for (int i = 0; i < d->max_sfb; i++) d->ms[g][i] = d->ms_mode == 2 || (d->ms_mode == 1 && getb(&r, 1));
            }
            if (!ics(d, &r, 0, common) || !ics(d, &r, 1, common)) return false;
            if (d->ms_mode) { /* L = M + S, R = M - S */
                bool sh = d->seq == 2;
                const uint16_t *sw = sh ? d->swb_s : d->swb_l;
                int w0 = 0;
                for (int g = 0; g < d->ngroups; g++) {
                    for (int i = 0; i < d->max_sfb; i++) {
                        if (!d->ms[g][i]) continue;
                        for (int w = w0; w < w0 + d->glen[g]; w++)
                            for (int k = sw[i]; k < sw[i + 1]; k++) {
                                float *l = &d->coef[0][(sh ? 128 * w : 0) + k], *rr = &d->coef[1][(sh ? 128 * w : 0) + k];
                                float m = *l, s = *rr;
                                *l = m + s;
                                *rr = m - s;
                            }
                    }
                    w0 += d->glen[g];
                }
            }
        } else {
            return false;
        }
        seen++;
    }
    if (seen != 1 || r.pos > r.n || (r.pos + 7) / 8 != bytes) return false; /* exactly one element, no junk */
    for (int c = 0; c < d->channels; c++) synth(d, c, out + c, d->channels);
    d->short_units += d->seq == 2;
    d->frames++;
    return true;
}

#endif
