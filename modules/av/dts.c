/* modules/av/dts.c — DTS Coherent Acoustics core decoder (ETSI TS 102 114 clause 5, Annex C, Annex D).
 * See dts.h. The frame: a header, then subframes - each its side information, the high-frequency vectors,
 * the LFE samples and up to 4 subsubframes of 8 subband samples per subband - then optional data and the
 * extensions (left alone). Each subframe's subband samples go through the 32-band synthesis filter bank. */
#include "dts.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/ffmpeg/dts_vq.h" /* (FFmpeg's two vector tables: LGPL 2.1+, see its README) */
#include "dts_tables.h"

#define BANDS 32
#define PCH 5         /* primary channels the core carries (more come in extensions) */
#define SSF_MAX 4     /* subsubframes in a subframe */
#define SF_SAMPLES 32 /* subband samples per subband in a subframe: 4 x 8 */
#define FULL_SCALE (1.0f / 8388608.0f) /* the core works in 24-bit sample units */
/* The synthesis below (Annex C.3.6's) leaves its output 1/(128 sqrt 2) of the PCM's: its rScale, which the
   spec doesn't give. Measured against FFmpeg's decode of real tracks: 181.01932, every channel, every frame. */
#define QMF_GAIN 181.01933598375617f

static const int RATES[16] = {0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0};
static const int KBPS[32] = {32,  56,  64,  96,  112, 128, 192, 224, 256, 320, 384,
                             448, 512, 576, 640, 768, 960, 1024, 1152, 1280, 1344, 1408,
                             1411, 1472, 1536, -1, -1, -1, -1, 0, -1, 0}; /* (-1 invalid, 0 open or lossless - 31, which
                                                                             picks the lossless step sizes; 1411.2 as 1411) */
static const int AMODE_CHANNELS[10] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5};
static const float ADJ[4] = {1.0f, 1.125f, 1.25f, 1.4375f};

/* ------------------------------------------------------------------ bits */

typedef struct {
    const uint8_t *p;
    long pos, end; /* in bits */
    bool over;     /* read past the end */
} Bits;

static uint32_t getb(Bits *b, int n) {
    if (n <= 0) return 0;
    if (b->pos + n > b->end) {
        b->over = true;
        b->pos = b->end;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->pos++) v = (v << 1) | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u);
    return v;
}

static int huff(Bits *b, const DtsHuff *h) {
    int n = 0;
    for (int depth = 0; depth < 24 && !b->over; depth++) {
        int v = h->node[n][getb(b, 1)];
        if (v < 0) return -v - 1 - DTS_LEAF;
        n = v;
    }
    b->over = true; /* (a complete code book can't run deeper: damage or the end) */
    return 0;
}

/* ---------------------------------------------------------------- header */

typedef struct {
    int ftype, deficit, cpf, nblks, fsize, amode, sfreq, rate, dynf, timef, auxf, hdcd, ext_id, ext_audio, aspf,
        lff, hflag, filts, vernum, chist, pcmr, sumf, sums;
} Hdr;

static bool read_header(Bits *b, Hdr *h) {
    if (getb(b, 32) != 0x7FFE8001u) return false;
    h->ftype = (int)getb(b, 1);
    h->deficit = (int)getb(b, 5);
    h->cpf = (int)getb(b, 1);
    h->nblks = (int)getb(b, 7);
    h->fsize = (int)getb(b, 14);
    h->amode = (int)getb(b, 6);
    h->sfreq = (int)getb(b, 4);
    h->rate = (int)getb(b, 5);
    getb(b, 1); /* FixedBit */
    h->dynf = (int)getb(b, 1);
    h->timef = (int)getb(b, 1);
    h->auxf = (int)getb(b, 1);
    h->hdcd = (int)getb(b, 1);
    h->ext_id = (int)getb(b, 3);
    h->ext_audio = (int)getb(b, 1);
    h->aspf = (int)getb(b, 1);
    h->lff = (int)getb(b, 2);
    h->hflag = (int)getb(b, 1);
    if (h->cpf) getb(b, 16); /* HCRC: never tested (5.4.2) */
    h->filts = (int)getb(b, 1);
    h->vernum = (int)getb(b, 4);
    h->chist = (int)getb(b, 2);
    h->pcmr = (int)getb(b, 3);
    h->sumf = (int)getb(b, 1);
    h->sums = (int)getb(b, 1);
    getb(b, 4); /* DIALNORM / UNSPEC */
    if (b->over) return false;
    /* values the spec rules out: a sync word in other data, most likely */
    if (h->nblks < 5 || h->fsize < 95 || !RATES[h->sfreq] || KBPS[h->rate] < 0 || h->lff == 3) return false;
    if (h->ftype == 1 && h->deficit != 31) return false;
    return true;
}

static void fill_info(const Hdr *h, DtsInfo *info) {
    memset(info, 0, sizeof *info);
    info->sample_rate = RATES[h->sfreq];
    info->bitrate = KBPS[h->rate];
    info->frame_bytes = h->fsize + 1;
    info->blocks = h->nblks + 1;
    info->samples = info->blocks * 32;
    info->amode = h->amode;
    info->lfe = h->lff != 0;
    info->lfe_factor = h->lff == 1 ? 128 : h->lff == 2 ? 64 : 0;
    info->channels = (h->amode < 10 ? AMODE_CHANNELS[h->amode] : 0) + info->lfe;
    info->crc = h->cpf;
    info->dynrange = h->dynf;
    info->ext_audio = h->ext_audio;
    info->ext_audio_id = h->ext_id;
    info->perfect_qmf = h->filts;
    info->version = h->vernum;
    info->pcm_bits = h->pcmr < 2 ? 16 : h->pcmr < 4 ? 20 : 24;
}

int dts_frame_size(const uint8_t *p, size_t n) {
    if (!p || n < DTS_HEADER_BYTES) return 0;
    Bits b = {p, 0, DTS_HEADER_BYTES * 8, false};
    Hdr h;
    return read_header(&b, &h) ? h.fsize + 1 : 0;
}

bool dts_parse_header(const uint8_t *p, size_t n, DtsInfo *info) {
    if (!p || n < DTS_HEADER_BYTES) return false;
    Bits b = {p, 0, DTS_HEADER_BYTES * 8, false};
    Hdr h;
    if (!read_header(&b, &h)) return false;
    if (info) fill_info(&h, info);
    return true;
}

long dts_find_frame(const uint8_t *p, size_t n, bool complete) {
    for (size_t i = 0; p && i + DTS_HEADER_BYTES <= n; i++) {
        if (p[i] != 0x7F || p[i + 1] != 0xFE || p[i + 2] != 0x80 || p[i + 3] != 0x01) continue;
        int size = dts_frame_size(p + i, n - i);
        if (size <= 0) continue;
        if ((size_t)size > n - i) {
            if (complete) continue;
            return (long)i;
        }
        if ((size_t)size + 4 > n - i) return (long)i; /* ends the buffer */
        const uint8_t *q = p + i + size;
        if ((q[0] == 0x7F && q[1] == 0xFE && q[2] == 0x80 && q[3] == 0x01) || /* the next core frame */
            (q[0] == 0x64 && q[1] == 0x58 && q[2] == 0x20 && q[3] == 0x25))   /* a DTS-HD substream */
            return (long)i;
    }
    return -1;
}

/* -------------------------------------------------------------- decoder */

struct DtsDec {
    DtsLayout layout;
    float drc;
    DtsStats st;
    float cosmod[544];             /* the synthesis bank's cosine modulation (Annex C.3.6) */
    float hist[PCH][BANDS][4];     /* ADPCM history: each subband's last 4 samples, oldest first */
    float qx[PCH][512], qz[PCH][64]; /* the synthesis bank's state per channel */
    float lfe_hist[8];             /* the last decimated LFE samples, newest first */
    float sub[PCH][BANDS][SF_SAMPLES]; /* one subframe's subband samples */
    float pcm[DTS_MAX_CHANNELS][DTS_SAMPLES]; /* the frame's PCM: primary channels, then LFE */
};

DtsDec *dts_open(void) {
    DtsDec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->layout = DTS_LAYOUT_NATIVE;
    const double pi = 3.14159265358979323846;
    int j = 0;
    for (int k = 0; k < 16; k++)
        for (int i = 0; i < 16; i++) d->cosmod[j++] = (float)cos((2 * i + 1) * (2 * k + 1) * pi / 64);
    for (int k = 0; k < 16; k++)
        for (int i = 0; i < 16; i++) d->cosmod[j++] = (float)cos(i * (2 * k + 1) * pi / 32);
    for (int k = 0; k < 16; k++) d->cosmod[j++] = (float)(0.25 / (2 * cos((2 * k + 1) * pi / 128)));
    for (int k = 0; k < 16; k++) d->cosmod[j++] = (float)(-0.25 / (2 * sin((2 * k + 1) * pi / 128)));
    return d;
}

void dts_close(DtsDec *d) { free(d); }

void dts_reset(DtsDec *d) {
    memset(d->hist, 0, sizeof d->hist);
    memset(d->qx, 0, sizeof d->qx);
    memset(d->qz, 0, sizeof d->qz);
    memset(d->lfe_hist, 0, sizeof d->lfe_hist);
}

void dts_set_layout(DtsDec *d, DtsLayout layout) { d->layout = layout; }
void dts_set_drc(DtsDec *d, float scale) { d->drc = scale; }
const DtsStats *dts_stats(const DtsDec *d) { return &d->st; }

/* the 32-band synthesis (Annex C.3.6): one subband sample per band in, 32 PCM samples out */
static void synthesize(DtsDec *d, int ch, const float *coef, const float xin[BANDS], float *out) {
    const float *cm = d->cosmod;
    float a[16], b[16], *x = d->qx[ch], *z = d->qz[ch];
    int j = 0;
    for (int k = 0; k < 16; k++) {
        float s = 0;
        for (int i = 0; i < 16; i++) s += (xin[2 * i] + xin[2 * i + 1]) * cm[j++];
        a[k] = s;
    }
    for (int k = 0; k < 16; k++) {
        float s = 0;
        for (int i = 0; i < 16; i++) s += (i ? xin[2 * i] + xin[2 * i - 1] : xin[0]) * cm[j++];
        b[k] = s;
    }
    for (int k = 0; k < 16; k++) x[k] = cm[j++] * (a[k] + b[k]);
    for (int k = 0; k < 16; k++) x[31 - k] = cm[j++] * (a[k] - b[k]);
    for (int i = 0, k = 31; i < 32; i++, k--)
        for (int t = 0; t < 512; t += 64) z[i] += coef[i + t] * (x[i + t] - x[t + k]);
    for (int i = 0, k = 31; i < 32; i++, k--)
        for (int t = 0; t < 512; t += 64) z[32 + i] += coef[32 + i + t] * (-x[i + t] - x[t + k]);
    for (int i = 0; i < 32; i++) out[i] = z[i] * QMF_GAIN;
    memmove(x + 32, x, 480 * sizeof *x);
    memmove(z, z + 32, 32 * sizeof *z);
    memset(z + 32, 0, 32 * sizeof *z);
}

/* the LFE: n decimated samples, each becoming `factor` (64 or 128) through the 512-tap filter (Annex C.3.7) */
static void lfe_interpolate(DtsDec *d, const float *dec, int n, int factor, float *out) {
    const float *coef = factor == 128 ? DTS_LFE128 : DTS_LFE64;
    int taps = 512 / factor;
    for (int i = 0; i < n; i++) {
        memmove(d->lfe_hist + 1, d->lfe_hist, 7 * sizeof *d->lfe_hist);
        d->lfe_hist[0] = dec[i];
        for (int k = 0; k < factor; k++) {
            float s = 0;
            for (int t = 0; t < taps; t++) s += d->lfe_hist[t] * coef[k + t * factor];
            *out++ = s;
        }
    }
}

/* everything a frame's coding header says about one channel */
typedef struct {
    int subs, vqsub, joinx, thuff, shuff, bhuff;
    int sel[10];
    float adj[10];
} ChHdr;

#define BAD(d, why) ((d)->st.error_line = __LINE__, (void)(why), false)

static bool read_scale(Bits *b, int shuff, int *sum) {
    if (shuff < 5) *sum += huff(b, &DTS_SCALES[shuff]);
    else *sum = (int)getb(b, shuff == 5 ? 6 : 7);
    return !b->over && *sum >= 0 && *sum < (shuff == 6 ? 125 : 63);
}

/* One quantisation index group of a subband: 8 samples (Table 5-26) */
static bool read_audio(DtsDec *d, Bits *b, int abits, int sel, int out[8]) {
    if (abits == 0) {
        memset(out, 0, 8 * sizeof *out);
        return true;
    }
    if (abits <= 10 && sel < DTS_QUANT_BOOKS[abits - 1]) { /* Huffman */
        for (int m = 0; m < 8; m++) out[m] = huff(b, &DTS_QUANT[abits - 1][sel]);
        d->st.huffman_codes++;
        return !b->over;
    }
    if (abits <= 7) { /* block code: 4 samples in one */
        int levels = DTS_QUANT_LEVELS[abits - 1];
        static const int BITS[7] = {7, 10, 12, 13, 15, 17, 19};
        for (int blk = 0; blk < 2; blk++) {
            int code = (int)getb(b, BITS[abits - 1]), off = (levels - 1) / 2;
            for (int m = 0; m < 4; m++) {
                out[blk * 4 + m] = code % levels - off;
                code /= levels;
            }
            if (code) return false; /* a code past the book */
        }
        d->st.block_codes++;
        return !b->over;
    }
    int n = abits - 3; /* plain two's complement: 5 bits for ABITS 8 ... 23 for 26 */
    for (int m = 0; m < 8; m++) {
        uint32_t v = getb(b, n);
        out[m] = (int)(v ^ (1u << (n - 1))) - (1 << (n - 1));
    }
    d->st.linear_codes++;
    return !b->over;
}

static void mix_out(DtsDec *d, const Hdr *h, int nsamp, float *out, int *channels);

int dts_decode(DtsDec *d, const uint8_t *frame, size_t n, float *out, int *channels, DtsInfo *info) {
    *channels = 0;
    if (!frame || n < DTS_HEADER_BYTES) return DTS_ERR_SYNC;
    Bits b = {frame, 0, (long)n * 8, false};
    Hdr h;
    if (!read_header(&b, &h)) return DTS_ERR_SYNC;
    if (info) fill_info(&h, info);
    if ((size_t)h.fsize + 1 > n) return DTS_ERR_TRUNCATED;
    if (h.amode > 9) return DTS_ERR_UNSUPPORTED;
    b.end = (long)(h.fsize + 1) * 8;
    memset(&d->st, 0, sizeof d->st);
    const int samples = (h.nblks + 1) * 32, nch = AMODE_CHANNELS[h.amode];
    const int lfe_factor = h.lff == 1 ? 128 : 64;
    const int32_t *steps = h.rate == 0x1F ? DTS_STEP_LOSSLESS : DTS_STEP_LOSSY;
    const float *qmf = h.filts ? DTS_QMF_PERFECT : DTS_QMF_LOSSY;
    if (!h.hflag) memset(d->hist, 0, sizeof d->hist); /* an entry point: no history from the last frame */

    /* primary audio coding header (Table 5-21) */
    ChHdr c[PCH];
    memset(c, 0, sizeof c);
    int nsubfs = (int)getb(&b, 4) + 1, npch = (int)getb(&b, 3) + 1;
    bool ok = npch == nch;
    if (!ok) d->st.error_line = __LINE__;
    for (int ch = 0; ok && ch < npch; ch++) c[ch].subs = (int)getb(&b, 5) + 2;
    for (int ch = 0; ok && ch < npch; ch++) c[ch].vqsub = (int)getb(&b, 5) + 1;
    for (int ch = 0; ok && ch < npch; ch++) c[ch].joinx = (int)getb(&b, 3);
    for (int ch = 0; ok && ch < npch; ch++) c[ch].thuff = (int)getb(&b, 2);
    for (int ch = 0; ok && ch < npch; ch++) c[ch].shuff = (int)getb(&b, 3);
    for (int ch = 0; ok && ch < npch; ch++) c[ch].bhuff = (int)getb(&b, 3);
    for (int q = 0; ok && q < 10; q++)
        for (int ch = 0; ch < npch; ch++) c[ch].sel[q] = (int)getb(&b, q == 0 ? 1 : q < 5 ? 2 : 3);
    for (int q = 0; ok && q < 10; q++)
        for (int ch = 0; ch < npch; ch++) {
            c[ch].adj[q] = 1.0f;
            if (c[ch].sel[q] < DTS_QUANT_BOOKS[q]) c[ch].adj[q] = ADJ[getb(&b, 2)]; /* (sent for Huffman books only) */
        }
    if (ok && h.cpf) getb(&b, 16); /* AHCRC */
    for (int ch = 0; ok && ch < npch; ch++) {
        const ChHdr *k = &c[ch];
        if (k->subs > BANDS || k->vqsub > BANDS || k->shuff == 7 || k->bhuff == 7 ||
            (k->joinx && (k->joinx - 1 >= npch || k->joinx - 1 == ch)))
            ok = BAD(d, "a coding header value out of range");
    }
    if (b.over) ok = false;

    int done = 0; /* subband samples per band so far: (NBLKS + 1) when the frame is whole */
    for (int sf = 0; ok && sf < nsubfs; sf++) {
        /* side information (Table 5-28) */
        int nssc = (int)getb(&b, 2) + 1, psc = (int)getb(&b, 3);
        if (psc) { /* a partial subsubframe: termination frames only, never met */
            ok = BAD(d, "partial subsubframe");
            break;
        }
        int nsamp = nssc * 8;
        if (done + nsamp > h.nblks + 1) {
            ok = BAD(d, "more subband samples than the frame's blocks");
            break;
        }
        uint8_t pmode[PCH][BANDS] = {{0}}, abits[PCH][BANDS] = {{0}}, tmode[PCH][BANDS] = {{0}};
        int16_t pvq[PCH][BANDS] = {{0}};
        float scale[PCH][BANDS][2], joint[PCH][BANDS];
        memset(scale, 0, sizeof scale);
        for (int ch = 0; ch < npch; ch++)
            for (int k = 0; k < c[ch].subs; k++) pmode[ch][k] = (uint8_t)getb(&b, 1);
        for (int ch = 0; ch < npch; ch++)
            for (int k = 0; k < c[ch].subs; k++)
                if (pmode[ch][k]) pvq[ch][k] = (int16_t)getb(&b, 12);
        for (int ch = 0; ch < npch; ch++)
            for (int k = 0; k < c[ch].vqsub; k++) {
                int a = c[ch].bhuff < 5 ? huff(&b, &DTS_BITALLOC[c[ch].bhuff]) : (int)getb(&b, c[ch].bhuff == 5 ? 4 : 5);
                if (a > 26) ok = BAD(d, "bit allocation index past 26");
                abits[ch][k] = (uint8_t)a;
            }
        if (nssc > 1)
            for (int ch = 0; ch < npch; ch++)
                for (int k = 0; k < c[ch].vqsub; k++)
                    if (abits[ch][k]) tmode[ch][k] = (uint8_t)huff(&b, &DTS_TMODE[c[ch].thuff]);
        for (int ch = 0; ok && ch < npch; ch++) { /* the bands with bits (two scales with a transient), then the vectors' */
            const int32_t *tab = c[ch].shuff == 6 ? DTS_SCALE7 : DTS_SCALE6;
            int sum = 0;
            for (int k = 0; ok && k < c[ch].vqsub; k++) {
                if (!abits[ch][k]) continue;
                if (!read_scale(&b, c[ch].shuff, &sum)) ok = BAD(d, "scale factor index");
                else scale[ch][k][0] = (float)tab[sum];
                if (ok && tmode[ch][k]) {
                    if (!read_scale(&b, c[ch].shuff, &sum)) ok = BAD(d, "scale factor index");
                    else scale[ch][k][1] = (float)tab[sum];
                }
            }
            for (int k = c[ch].vqsub; ok && k < c[ch].subs; k++) {
                if (!read_scale(&b, c[ch].shuff, &sum)) ok = BAD(d, "scale factor index");
                else scale[ch][k][0] = (float)tab[sum];
            }
        }
        int jshuff[PCH] = {0};
        for (int ch = 0; ch < npch; ch++)
            if (c[ch].joinx) jshuff[ch] = (int)getb(&b, 3);
        for (int ch = 0; ok && ch < npch; ch++) {
            if (!c[ch].joinx) continue;
            int src = c[ch].joinx - 1;
            if (jshuff[ch] == 7) ok = BAD(d, "joint scale code book");
            for (int k = c[ch].subs; ok && k < c[src].subs; k++) {
                int v = jshuff[ch] < 5 ? huff(&b, &DTS_SCALES[jshuff[ch]]) : (int)getb(&b, jshuff[ch] == 5 ? 6 : 7);
                v += 64;
                if (v < 0 || v > 128) ok = BAD(d, "joint scale index");
                else joint[ch][k] = DTS_JOINT[v];
            }
        }
        float range = 1.0f;
        if (h.dynf) {
            int r = (int)getb(&b, 8);
            if (d->drc > 0) range = powf(10.0f, d->drc * (-31.75f + 0.25f * (float)r) / 20.0f);
        }
        if (h.cpf) getb(&b, 16); /* SICRC */
        if (!ok || b.over) {
            ok = ok ? BAD(d, "side information ran past the frame") : false;
            break;
        }
        for (int ch = 0; ch < npch; ch++) {
            for (int k = 0; k < c[ch].subs; k++)
                if (pmode[ch][k] && k < c[ch].vqsub) d->st.subbands_predicted++;
            for (int k = c[ch].vqsub; k < c[ch].subs; k++) d->st.subbands_vq++;
            for (int k = 0; k < c[ch].vqsub; k++) {
                d->st.transients += tmode[ch][k] != 0;
                d->st.abits[abits[ch][k]] += nsamp;
            }
        }

        /* the high-frequency vectors (Table 5-29) */
        memset(d->sub, 0, sizeof d->sub);
        for (int ch = 0; ch < npch; ch++)
            for (int k = c[ch].vqsub; k < c[ch].subs; k++) {
                int v = (int)getb(&b, 10);
                const int8_t *e = DTS_HF_VQ[v];
                for (int m = 0; m < nsamp; m++) d->sub[ch][k][m] = scale[ch][k][0] * (float)e[m] * (1.0f / 16.0f);
            }
        /* the LFE */
        float lfe_dec[2 * 2 * SSF_MAX];
        int nlfe = h.lff ? 2 * h.lff * nssc : 0;
        if (nlfe) {
            int8_t raw[2 * 2 * SSF_MAX];
            for (int i = 0; i < nlfe; i++) raw[i] = (int8_t)getb(&b, 8);
            int si = (int)getb(&b, 8);
            if (si > 124) {
                ok = BAD(d, "LFE scale index");
                break;
            }
            float s = (float)DTS_SCALE7[si] * 0.035f;
            for (int i = 0; i < nlfe; i++) lfe_dec[i] = (float)raw[i] * s;
        }
        /* the subsubframes */
        for (int ss = 0; ok && ss < nssc; ss++) {
            for (int ch = 0; ok && ch < npch; ch++)
                for (int k = 0; ok && k < c[ch].vqsub; k++) {
                    int a = abits[ch][k], q[8];
                    int sel = a >= 1 && a <= 10 ? c[ch].sel[a - 1] : 0;
                    if (!read_audio(d, &b, a, sel, q)) {
                        ok = BAD(d, "audio codes");
                        break;
                    }
                    float *x = d->sub[ch][k] + ss * 8;
                    if (a) {
                        int t = tmode[ch][k] ? tmode[ch][k] : nssc;
                        float s = (float)steps[a] * (1.0f / 4194304.0f) * scale[ch][k][ss < t ? 0 : 1] *
                                  (a <= 10 ? c[ch].adj[a - 1] : 1.0f);
                        for (int m = 0; m < 8; m++) x[m] = s * (float)q[m];
                    }
                    if (pmode[ch][k]) { /* inverse ADPCM (Annex C.3.3) */
                        const int16_t *v = DTS_ADPCM_VQ[pvq[ch][k]];
                        float *hh = d->hist[ch][k];
                        for (int m = 0; m < 8; m++) {
                            float p = 0;
                            for (int i = 0; i < 4; i++) {
                                float prev = m - i - 1 >= 0 ? x[m - i - 1] : hh[4 + (m - i - 1)];
                                p += (float)v[i] * (1.0f / 8192.0f) * prev;
                            }
                            x[m] += p;
                        }
                    }
                    for (int i = 0; i < 4; i++) d->hist[ch][k][i] = x[4 + i]; /* (every subband keeps its last 4) */
                }
            if (ok && (ss == nssc - 1 || h.aspf) && getb(&b, 16) != 0xFFFF) ok = BAD(d, "DSYNC");
            if (b.over) ok = ok ? BAD(d, "audio ran past the frame") : false;
        }
        if (!ok) break;
        /* joint intensity (C.3.4): bands past this channel's own, copied from the source channel */
        int active[PCH];
        for (int ch = 0; ch < npch; ch++) {
            active[ch] = c[ch].subs;
            if (!c[ch].joinx) continue;
            int src = c[ch].joinx - 1;
            for (int k = c[ch].subs; k < c[src].subs; k++) {
                for (int m = 0; m < nsamp; m++) d->sub[ch][k][m] = joint[ch][k] * d->sub[src][k][m];
                d->st.subbands_joint++;
            }
            if (c[src].subs > active[ch]) active[ch] = c[src].subs;
        }
        /* the synthesis filter bank, then the LFE */
        for (int ch = 0; ch < npch; ch++) {
            float xin[BANDS];
            for (int m = 0; m < nsamp; m++) {
                for (int k = 0; k < BANDS; k++) xin[k] = k < active[ch] ? d->sub[ch][k][m] : 0.0f;
                synthesize(d, ch, qmf, xin, d->pcm[ch] + (done + m) * 32);
            }
            if (range != 1.0f)
                for (int i = 0; i < nsamp * 32; i++) d->pcm[ch][done * 32 + i] *= range;
        }
        if (nlfe) lfe_interpolate(d, lfe_dec, nlfe, lfe_factor, d->pcm[npch] + done * 32);
        done += nsamp;
    }
    if (ok && done != h.nblks + 1) ok = BAD(d, "fewer subband samples than the frame's blocks");
    d->st.bits_used = (int)b.pos;
    int nout = dts_layout_channels(h.amode, h.lff != 0, d->layout);
    if (!ok) {
        memset(out, 0, (size_t)samples * (size_t)nout * sizeof *out);
        dts_reset(d);
        *channels = nout;
        return DTS_ERR_BITSTREAM;
    }
    /* sum/difference coding of the front and surround pairs (C.3.5; AMODE 3 is one) */
    int l = h.amode == 5 || h.amode == 7 || h.amode == 9 ? 1 : 0;
    if ((h.sumf || h.amode == 3) && nch >= 2)
        for (int i = 0; i < samples; i++) {
            float a = d->pcm[l][i], r = d->pcm[l + 1][i];
            d->pcm[l][i] = a + r, d->pcm[l + 1][i] = a - r;
        }
    if (h.sums && (h.amode == 8 || h.amode == 9))
        for (int i = 0; i < samples; i++) {
            float a = d->pcm[nch - 2][i], r = d->pcm[nch - 1][i];
            d->pcm[nch - 2][i] = a + r, d->pcm[nch - 1][i] = a - r;
        }
    mix_out(d, &h, samples, out, channels);
    return DTS_OK;
}

/* ---------------------------------------------------------------- output */

/* Where each primary channel of an AMODE sits: L R C S SL SR (-1 = not one of them) */
enum { CL, CR, CC, CS, CSL, CSR };
static const int8_t ROLE[10][5] = {
    {CC, -1, -1, -1, -1},       {CL, CR, -1, -1, -1},       {CL, CR, -1, -1, -1},     {CL, CR, -1, -1, -1},
    {CL, CR, -1, -1, -1},       {CC, CL, CR, -1, -1},       {CL, CR, CS, -1, -1},     {CC, CL, CR, CS, -1},
    {CL, CR, CSL, CSR, -1},     {CC, CL, CR, CSL, CSR},
};

int dts_layout_channels(int amode, bool lfe, DtsLayout layout) {
    if (amode < 0 || amode > 9) return 0;
    return layout == DTS_LAYOUT_STEREO ? 2 : AMODE_CHANNELS[amode] + (lfe ? 1 : 0);
}

/* WAVE order (L R C LFE S|SL SR) -> which native channel; npch = the LFE's place */
static int wav_source(int amode, bool lfe, int i) {
    static const int ORDER[6] = {CL, CR, CC, -2 /* LFE */, CSL, CSR};
    int nch = AMODE_CHANNELS[amode], k = 0;
    if (amode == 0) return i; /* mono (+ LFE) */
    if (amode == 1) return i; /* dual mono */
    for (int o = 0; o < 6; o++) {
        int want = ORDER[o];
        int src = -1;
        if (want == -2) src = lfe ? nch : -1;
        else
            for (int ch = 0; ch < nch; ch++)
                if (ROLE[amode][ch] == want || (want == CSL && ROLE[amode][ch] == CS)) src = ch;
        if (src < 0) continue;
        if (k++ == i) return src;
    }
    return 0;
}

const char *dts_channel_name(int amode, bool lfe, DtsLayout layout, int i) {
    static const char *const NAME[6] = {"L", "R", "C", "S", "SL", "SR"};
    if (amode < 0 || amode > 9 || i < 0 || i >= dts_layout_channels(amode, lfe, layout)) return "";
    if (layout == DTS_LAYOUT_STEREO) return i ? "R" : "L";
    int nch = AMODE_CHANNELS[amode];
    int src = layout == DTS_LAYOUT_WAV ? wav_source(amode, lfe, i) : i;
    if (src >= nch) return "LFE";
    if (amode == 1) return src ? "Ch2" : "Ch1";
    if (amode == 4) return src ? "Rt" : "Lt";
    return NAME[ROLE[amode][src]];
}

const char *dts_amode_name(int amode, bool lfe) {
    static const char *const plain[10] = {"1.0", "1+1", "2.0", "2.0", "2.0", "3.0", "2/1", "3/1", "2/2", "5.0"};
    static const char *const with_lfe[10] = {"1.1", "1+1.1", "2.1", "2.1", "2.1", "3.1", "2/1.1", "3/1.1", "2/2.1", "5.1"};
    if (amode < 0 || amode > 9) return "?";
    return lfe ? with_lfe[amode] : plain[amode];
}

static void mix_out(DtsDec *d, const Hdr *h, int nsamp, float *out, int *channels) {
    int nch = AMODE_CHANNELS[h->amode], lfe = h->lff != 0;
    if (d->layout == DTS_LAYOUT_STEREO) { /* Lo/Ro: centre and surrounds at -3 dB, normalised as the Dolby one is */
        const float c3 = 0.70710678f;
        float wl[PCH] = {0}, wr[PCH] = {0}, sl = 0, sr = 0;
        for (int ch = 0; ch < nch; ch++) {
            switch (h->amode == 1 ? (ch ? CR : CL) : ROLE[h->amode][ch]) {
            case CL: wl[ch] = 1; break;
            case CR: wr[ch] = 1; break;
            case CC: wl[ch] = wr[ch] = h->amode == 0 ? 1.0f : c3; break;
            case CS: wl[ch] = wr[ch] = c3 * c3; break;
            case CSL: wl[ch] = c3; break;
            case CSR: wr[ch] = c3; break;
            }
        }
        for (int ch = 0; ch < nch; ch++) sl += wl[ch], sr += wr[ch];
        for (int ch = 0; ch < nch; ch++) wl[ch] *= FULL_SCALE / sl, wr[ch] *= FULL_SCALE / sr;
        for (int n = 0; n < nsamp; n++) {
            float a = 0, b = 0;
            for (int ch = 0; ch < nch; ch++) a += wl[ch] * d->pcm[ch][n], b += wr[ch] * d->pcm[ch][n];
            out[2 * n] = a, out[2 * n + 1] = b;
        }
        *channels = 2;
        return;
    }
    int nout = nch + lfe;
    for (int i = 0; i < nout; i++) {
        int src = d->layout == DTS_LAYOUT_WAV ? wav_source(h->amode, lfe, i) : i;
        const float *p = d->pcm[src];
        for (int n = 0; n < nsamp; n++) out[n * nout + i] = p[n] * FULL_SCALE;
    }
    *channels = nout;
}
