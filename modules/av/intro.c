/* modules/av/intro.c — a series' intro found by its sound (see intro.h). */
#include "intro.h"

#include "aacdec.h"
#include "ac3.h"
#include "dts.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NB 33           /* bands: 32 bits from the differences of neighbours */
#define LO_HZ 300.0
#define HI_HZ 2000.0
#define WIN_S 0.37      /* the analysis window (Haitsma & Kalker's) */
#define NFFT 2048
#define TARGET_HZ 5000  /* filtered and thinned out to 5-8 kHz: 2 kHz is all a print looks at */
#define MAX_TAPS 401
#define CHUNK 4096
#define QUIET 1e-6      /* mean square under -60 dBFS: silence, matches nothing (and so does a steady sound) */

/* --------------------------------------------------------------- the FFT */

/* A real NFFT-point transform as an NFFT/2-point complex one (even samples real, odd imaginary), split after. */
#define M2 (NFFT / 2)

typedef struct {
    uint16_t rev[M2];
    float cs[M2 / 2], sn[M2 / 2]; /* the half-size transform's twiddles */
    float tc[M2], ts[M2];         /* e^{-2 pi i k / NFFT}, for the split */
} Fft;

static void fft_init(Fft *f) {
    int bits = 0;
    while ((1 << bits) < M2) bits++;
    for (int i = 0; i < M2; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        f->rev[i] = (uint16_t)r;
    }
    for (int k = 0; k < M2 / 2; k++) f->cs[k] = (float)cos(2 * M_PI * k / M2), f->sn[k] = (float)-sin(2 * M_PI * k / M2);
    for (int k = 0; k < M2; k++) f->tc[k] = (float)cos(2 * M_PI * k / NFFT), f->ts[k] = (float)-sin(2 * M_PI * k / NFFT);
}

static void fft(const Fft *f, float *re, float *im) { /* M2 points, e^{-2 pi i nk / M2} */
    for (int i = 0; i < M2; i++) {
        int j = f->rev[i];
        if (j > i) {
            float t = re[i];
            re[i] = re[j], re[j] = t;
            t = im[i], im[i] = im[j], im[j] = t;
        }
    }
    for (int len = 2; len <= M2; len <<= 1) {
        int half = len / 2, step = M2 / len;
        for (int i = 0; i < M2; i += len)
            for (int j = 0; j < half; j++) {
                float wr = f->cs[j * step], wi = f->sn[j * step];
                float *ar = &re[i + j], *ai = &im[i + j], *br = &re[i + j + half], *bi = &im[i + j + half];
                float xr = *br * wr - *bi * wi, xi = *br * wi + *bi * wr;
                *br = *ar - xr, *bi = *ai - xi;
                *ar += xr, *ai += xi;
            }
    }
}

/* ------------------------------------------------------------ the printer */

struct IntroPrinter {
    double rate, F; /* the input's rate, the thinned-out one */
    int d, taps, L;
    float h[MAX_TAPS];
    float work[MAX_TAPS + CHUNK];
    int64_t in_count;
    float *buf;   /* thinned-out samples from absolute index base */
    int nbuf, capbuf;
    int64_t base, frame;
    float win[NFFT], re[M2], im[M2], pw[M2];
    int edge[NB + 1];
    double prev[NB];
    uint32_t *v;
    uint8_t *loud;
    int n, cap;
    Fft fft;
};

IntroPrinter *intro_printer_open(int sample_rate) {
    if (sample_rate < 4000 || sample_rate > 192000) return NULL;
    IntroPrinter *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->rate = sample_rate;
    p->d = sample_rate / TARGET_HZ < 1 ? 1 : sample_rate / TARGET_HZ;
    p->F = p->rate / p->d;
    p->taps = p->d == 1 ? 1 : 8 * p->d + 1;
    if (p->taps > MAX_TAPS) p->taps = MAX_TAPS | 1;
    if (p->taps == 1) {
        p->h[0] = 1;
    } else { /* windowed sinc low-pass at 0.45 of the new rate */
        double fc = 0.45 * p->F / p->rate, sum = 0;
        int mid = p->taps / 2;
        for (int t = 0; t < p->taps; t++) {
            double x = t - mid, s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
            double w = 0.54 - 0.46 * cos(2 * M_PI * t / (p->taps - 1));
            p->h[t] = (float)(s * w);
            sum += s * w;
        }
        for (int t = 0; t < p->taps; t++) p->h[t] = (float)(p->h[t] / sum);
    }
    p->L = (int)(WIN_S * p->F + 0.5);
    if (p->L > NFFT) p->L = NFFT;
    for (int i = 0; i < p->L; i++) p->win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / (p->L - 1)));
    for (int m = 0; m <= NB; m++) {
        double hz = LO_HZ * pow(HI_HZ / LO_HZ, (double)m / NB);
        p->edge[m] = (int)(hz * NFFT / p->F + 0.5);
    }
    fft_init(&p->fft);
    return p;
}

void intro_printer_close(IntroPrinter *p) {
    if (!p) return;
    free(p->buf);
    free(p->v);
    free(p->loud);
    free(p);
}

static bool push(IntroPrinter *p, float x) {
    if (p->nbuf == p->capbuf) {
        int cap = p->capbuf ? p->capbuf * 2 : 8192;
        float *nb = realloc(p->buf, sizeof(float) * (size_t)cap);
        if (!nb) return false;
        p->buf = nb, p->capbuf = cap;
    }
    p->buf[p->nbuf++] = x;
    return true;
}

static int64_t frame_start(const IntroPrinter *p, int64_t k) { return (int64_t)floor((double)k * p->F / INTRO_FPS); }

static bool one_frame(IntroPrinter *p, const float *x) {
    double ms = 0;
    for (int i = 0; i < p->L; i++) ms += (double)x[i] * x[i];
    ms /= p->L;
    for (int n = 0; n < M2; n++) { /* even samples real, odd imaginary; the window, zeros past it */
        p->re[n] = 2 * n < p->L ? x[2 * n] * p->win[2 * n] : 0;
        p->im[n] = 2 * n + 1 < p->L ? x[2 * n + 1] * p->win[2 * n + 1] : 0;
    }
    fft(&p->fft, p->re, p->im);
    const Fft *f = &p->fft;
    int top = p->edge[NB] < M2 ? p->edge[NB] : M2 - 1;
    for (int k = p->edge[0]; k < top; k++) { /* X[k] = E[k] + W^k O[k] from the half-size transform */
        int j = (M2 - k) & (M2 - 1);
        float zr = p->re[k], zi = p->im[k], cr = p->re[j], ci = -p->im[j];
        float er = 0.5f * (zr + cr), ei = 0.5f * (zi + ci), orr = 0.5f * (zi - ci), oi = -0.5f * (zr - cr);
        float xr = er + f->tc[k] * orr - f->ts[k] * oi, xi = ei + f->tc[k] * oi + f->ts[k] * orr;
        p->pw[k] = xr * xr + xi * xi;
    }
    double e[NB];
    for (int m = 0; m < NB; m++) {
        double s = 0;
        for (int k = p->edge[m]; k < p->edge[m + 1] && k < top; k++) s += p->pw[k];
        e[m] = s;
    }
    uint32_t bits = 0;
    for (int m = 0; m < NB - 1; m++)
        if ((e[m] - e[m + 1]) - (p->prev[m] - p->prev[m + 1]) > 0) bits |= 1u << m;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 1024;
        uint32_t *nv = realloc(p->v, sizeof(uint32_t) * (size_t)cap);
        if (!nv) return false;
        p->v = nv;
        uint8_t *nl = realloc(p->loud, (size_t)cap);
        if (!nl) return false;
        p->loud = nl, p->cap = cap;
    }
    p->v[p->n] = bits;
    int set = __builtin_popcount(bits); /* (a steady tone or hum changes nothing: an all-0 print would match any other) */
    p->loud[p->n] = p->n > 0 && ms > QUIET && set >= 3 && set <= 29;
    p->n++;
    memcpy(p->prev, e, sizeof e);
    return true;
}

static bool frames(IntroPrinter *p) {
    for (;;) {
        int64_t s = frame_start(p, p->frame);
        if (s + p->L > p->base + p->nbuf) break;
        if (!one_frame(p, p->buf + (s - p->base))) return false;
        p->frame++;
        int64_t next = frame_start(p, p->frame), drop = next - p->base;
        if (drop > 0) {
            if (drop > p->nbuf) drop = p->nbuf;
            memmove(p->buf, p->buf + drop, sizeof(float) * (size_t)(p->nbuf - drop));
            p->nbuf -= (int)drop;
            p->base += drop;
        }
    }
    return true;
}

bool intro_printer_feed(IntroPrinter *p, const float *x, int n) {
    int keep = p->taps - 1;
    while (n > 0) {
        int c = n < CHUNK ? n : CHUNK;
        memcpy(p->work + keep, x, sizeof(float) * (size_t)c);
        int64_t r = p->in_count % p->d;
        for (int i = r ? (int)(p->d - r) : 0; i < c; i += p->d) {
            const float *e = p->work + keep + i;
            float acc = 0;
            for (int t = 0; t < p->taps; t++) acc += p->h[t] * e[-t];
            if (!push(p, acc)) return false;
        }
        p->in_count += c;
        if (keep) memmove(p->work, p->work + c, sizeof(float) * (size_t)keep);
        x += c, n -= c;
        if (!frames(p)) return false;
    }
    return true;
}

bool intro_printer_finish(IntroPrinter *p, IntroPrint *out) {
    memset(out, 0, sizeof *out);
    if (!p) return false;
    out->v = p->v, out->loud = p->loud, out->n = p->n;
    out->start_ms = (int64_t)(1000.0 * (p->L / 2.0) / p->F + 0.5); /* frame 0's centre, after the first sample */
    p->v = NULL, p->loud = NULL;
    intro_printer_close(p);
    return true;
}

void intro_print_free(IntroPrint *p) {
    if (!p) return;
    free(p->v);
    free(p->loud);
    memset(p, 0, sizeof *p);
}

/* -------------------------------------------------------- reading a file */

#define NS 1000000000LL

typedef struct {
    IntroPrinter *pr;
    int rate;
    float mono[AACDEC_FRAME > DTS_SAMPLES ? AACDEC_FRAME : DTS_SAMPLES];
    float pcm[DTS_SAMPLES * DTS_MAX_CHANNELS > AACDEC_FRAME * AACDEC_MAX_CHANNELS ? DTS_SAMPLES * DTS_MAX_CHANNELS
                                                                                  : AACDEC_FRAME * AACDEC_MAX_CHANNELS];
    int64_t first_ns, next_ns; /* the first block's time, where the sound fed so far ends */
    int64_t fed;               /* samples fed */
    int64_t limit;             /* samples wanted */
} Feed;

static bool feed_pcm(Feed *f, int rate, const float *mono, int n) {
    if (!f->pr) {
        f->pr = intro_printer_open(rate);
        if (!f->pr) return false;
        f->rate = rate;
    }
    if (rate != f->rate) return true; /* a frame at another rate: not ours to mix in (left out) */
    if (n > f->limit - f->fed) n = (int)(f->limit - f->fed);
    if (n <= 0) return true;
    f->fed += n;
    return intro_printer_feed(f->pr, mono, n);
}

static bool feed_silence(Feed *f, int64_t samples) {
    static const float zero[1024];
    while (samples > 0 && f->fed < f->limit) {
        int n = samples > 1024 ? 1024 : (int)samples;
        if (!feed_pcm(f, f->rate, zero, n)) return false;
        samples -= n;
    }
    return true;
}

static const char *codec_words(const char *id) {
    static const struct {
        const char *id, *words;
    } names[] = {{"A_MPEG/L3", "MP3"},   {"A_MPEG/L2", "MP2"},   {"A_OPUS", "Opus"},          {"A_VORBIS", "Vorbis"},
                 {"A_FLAC", "FLAC"},     {"A_TRUEHD", "TrueHD"}, {"A_MLP", "MLP"},            {"A_PCM", "PCM"},
                 {"A_ALAC", "ALAC"},     {"A_REAL", "RealAudio"}, {"A_MS/ACM", "an AVI sound"}, {"A_MP4/", "an MP4 sound"}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strncmp(id, names[i].id, strlen(names[i].id))) return names[i].words;
    return id;
}

int intro_read(const MkvReader *rd, const IntroReadOpts *o, IntroPrint *out, char *why, size_t whycap) {
    memset(out, 0, sizeof *out);
    if (why && whycap) why[0] = 0;
    char err[160] = "";
    Mkv *m = mkv_open(rd, err, sizeof err);
    if (!m) {
        if (o->stop && o->stop(o->ctx)) return INTRO_READ_STOPPED;
        if (why) snprintf(why, whycap, "not a readable video (%s)", err);
        return INTRO_READ_BROKEN;
    }
    const MkvTrack *t = mkv_find_track(m, MKV_AUDIO);
    if (!t) {
        mkv_close(m);
        if (why) snprintf(why, whycap, "it has no sound");
        return INTRO_READ_NO_SOUND;
    }
    AacDec *aac = NULL;
    Ac3Dec *ac3 = NULL;
    DtsDec *dts = NULL;
    if (!t->compressed && !strncmp(t->codec, "A_AAC", 5)) {
        uint8_t asc[2];
        if (t->priv && t->priv_len) aac = aacdec_open(t->priv, t->priv_len, err, sizeof err);
        else if (aacdec_make_config((int)t->rate, t->channels, asc)) aac = aacdec_open(asc, 2, err, sizeof err);
        else snprintf(err, sizeof err, "an AAC track without its configuration");
        if (aac) aacdec_set_layout(aac, AACDEC_LAYOUT_MONO);
    } else if (!t->compressed && (!strncmp(t->codec, "A_AC3", 5) || !strncmp(t->codec, "A_EAC3", 6))) {
        ac3 = ac3_open();
        if (ac3) ac3_set_layout(ac3, AC3_LAYOUT_STEREO);
    } else if (!t->compressed && !strncmp(t->codec, "A_DTS", 5)) {
        dts = dts_open();
        if (dts) dts_set_layout(dts, DTS_LAYOUT_STEREO);
    } else {
        if (why) snprintf(why, whycap, "its sound is %s", t->compressed ? "compressed inside the file" : codec_words(t->codec));
        mkv_close(m);
        return INTRO_READ_CODEC;
    }
    if (!aac && !ac3 && !dts) {
        if (why) snprintf(why, whycap, "its sound won't decode (%s)", err[0] ? err : "out of memory");
        mkv_close(m);
        return INTRO_READ_BROKEN;
    }
    Feed *f = calloc(1, sizeof *f);
    int rc = INTRO_READ_OK;
    if (!f) rc = INTRO_READ_STOPPED;
    int64_t seconds = o->seconds > 0 ? o->seconds : 600;
    if (f) f->first_ns = f->next_ns = MKV_NOPTS, f->limit = INT64_MAX;
    long blocks = 0, good = 0, bad = 0;
    MkvFrame fr;
    while (rc == INTRO_READ_OK) {
        if ((++blocks & 63) == 0 && o->stop && o->stop(o->ctx)) {
            rc = INTRO_READ_STOPPED;
            break;
        }
        int got = mkv_next(m, &fr);
        if (got < 0) {
            rc = INTRO_READ_STOPPED;
            break;
        }
        if (got == 0) break;
        if (fr.track != t->number) continue;
        if (f->first_ns == MKV_NOPTS) {
            if (fr.pts == MKV_NOPTS) continue; /* (a lace's later frames before any time) */
            f->first_ns = f->next_ns = fr.pts;
        }
        if (fr.pts != MKV_NOPTS && fr.pts - f->first_ns >= seconds * NS) break;
        if (f->pr && f->fed >= f->limit) break;
        if (f->pr && fr.pts != MKV_NOPTS && f->next_ns != MKV_NOPTS && fr.pts > f->next_ns + NS / 10) {
            /* a hole in the sound: silence keeps the times (at most 10 s - a bigger jump is a broken time) */
            int64_t gap = fr.pts - f->next_ns;
            if (gap > 10 * NS) gap = 10 * NS;
            if (!feed_silence(f, gap * f->rate / NS)) rc = INTRO_READ_STOPPED;
        }
        const uint8_t *p = fr.data;
        size_t left = fr.size;
        int64_t samples = 0;
        int rate = 0;
        if (aac) {
            int ch = 0, r = aacdec_decode(aac, p, left, f->pcm, &ch);
            rate = aacdec_info(aac)->sample_rate;
            if (r == AACDEC_OK) good++;
            else bad++;
            if (!feed_pcm(f, rate, f->pcm, AACDEC_FRAME)) rc = INTRO_READ_STOPPED;
            samples = AACDEC_FRAME;
        }
        while (ac3 && left >= AC3_HEADER_BYTES && rc == INTRO_READ_OK) {
            int size = ac3_frame_size(p, left), ch = 0;
            if (size <= 0 || (size_t)size > left) break;
            Ac3Info in = {0};
            int r = ac3_decode(ac3, p, (size_t)size, f->pcm, &ch, &in);
            p += size, left -= (size_t)size;
            if (r == AC3_SKIP) continue;
            if (in.sample_rate <= 0 || in.samples <= 0 || in.samples > AC3_SAMPLES) {
                bad++;
                continue;
            }
            rate = in.sample_rate;
            for (int i = 0; i < in.samples; i++) f->mono[i] = ch == 2 ? 0.5f * (f->pcm[2 * i] + f->pcm[2 * i + 1]) : 0;
            if (r == AC3_OK) good++;
            else bad++;
            if (!feed_pcm(f, rate, f->mono, in.samples)) rc = INTRO_READ_STOPPED;
            samples += in.samples;
        }
        while (dts && left >= DTS_HEADER_BYTES && rc == INTRO_READ_OK) {
            int size = dts_frame_size(p, left), ch = 0;
            if (size <= 0 || (size_t)size > left) break;
            DtsInfo in = {0};
            int r = dts_decode(dts, p, (size_t)size, f->pcm, &ch, &in);
            p += size, left -= (size_t)size;
            if (in.sample_rate <= 0 || in.samples <= 0 || in.samples > DTS_SAMPLES) {
                bad++;
                continue;
            }
            rate = in.sample_rate;
            for (int i = 0; i < in.samples; i++) f->mono[i] = ch == 2 ? 0.5f * (f->pcm[2 * i] + f->pcm[2 * i + 1]) : 0;
            if (r == DTS_OK) good++;
            else bad++;
            if (!feed_pcm(f, rate, f->mono, in.samples)) rc = INTRO_READ_STOPPED;
            samples += in.samples;
        }
        if (f->pr && f->limit == INT64_MAX) f->limit = seconds * f->rate;
        if (rate > 0) {
            int64_t took = samples * NS / rate;
            if (fr.pts != MKV_NOPTS) f->next_ns = fr.pts + took;
            else if (f->next_ns != MKV_NOPTS) f->next_ns += took;
        }
    }
    if (rc == INTRO_READ_OK && (!f->pr || good == 0 || bad > good)) {
        if (why) snprintf(why, whycap, good ? "most of its sound wouldn't decode (%ld of %ld frames)" : "its sound wouldn't decode", bad, good + bad);
        rc = INTRO_READ_BROKEN;
    }
    if (rc == INTRO_READ_OK) {
        intro_printer_finish(f->pr, out);
        f->pr = NULL;
        out->start_ms += f->first_ns / 1000000;
    }
    if (rc == INTRO_READ_STOPPED && why && !why[0]) snprintf(why, whycap, "stopped");
    if (f) intro_printer_close(f->pr);
    free(f);
    aacdec_close(aac);
    if (ac3) ac3_close(ac3);
    if (dts) dts_close(dts);
    mkv_close(m);
    return rc;
}

/* -------------------------------------------------------------- matching */

#define WIN INTRO_FPS         /* frames averaged: one second */
#define GOOD_SUM (11 * WIN)   /* at most 11 of 32 bits apart on average over it (random sound: 16) */
#define GAP (3 * INTRO_FPS)   /* up to three seconds that don't agree inside a stretch at one offset (a door, a line) */
#define EDGE 4                /* frames a stretch's edges are trimmed by */
#define EDGE_SUM (11 * EDGE)
#define VOTE_MIN 4            /* equal 16-bit pieces needed around an offset to look at it closely */
#define CANDIDATES 24         /* offsets looked at closely per pair */
#define BUCKET_MAX 48         /* a piece that common (the print of silence or a hum) votes for nothing */
#define STRONG_COST 750       /* a stretch only two episodes share counts when it agrees this well (7.5 bits) */

static inline int cost(const IntroPrint *a, int i, const IntroPrint *b, int j) {
    if (!a->loud[i] || !b->loud[j]) return 32;
    return __builtin_popcount(a->v[i] ^ b->v[j]);
}

/* Trim the run's ends to where a few frames in a row agree. */
static void trim(const IntroPrint *a, const IntroPrint *b, int d, IntroRun *r) {
    while (r->a_to - r->a_from > EDGE) {
        int s = 0;
        for (int k = 0; k < EDGE; k++) s += cost(a, r->a_from + k, b, r->a_from + k - d);
        if (s <= EDGE_SUM) break;
        r->a_from++;
    }
    while (r->a_to - r->a_from > EDGE) {
        int s = 0;
        for (int k = 1; k <= EDGE; k++) s += cost(a, r->a_to - k, b, r->a_to - k - d);
        if (s <= EDGE_SUM) break;
        r->a_to--;
    }
    long sum = 0;
    for (int i = r->a_from; i < r->a_to; i++) sum += cost(a, i, b, i - d);
    r->cost = (int)(sum * 100 / (r->a_to - r->a_from > 0 ? r->a_to - r->a_from : 1));
    r->b_from = r->a_from - d, r->b_to = r->a_to - d;
}

static int run_len(const IntroRun *r) { return r->a_to - r->a_from; }

static void consider(IntroRun *runs, int *nr, int max, const IntroRun *r) {
    int len = run_len(r), d = r->a_from - r->b_from;
    for (int k = 0; k < *nr; k++) { /* the same stretch at a neighbouring offset: keep the longer */
        int dk = runs[k].a_from - runs[k].b_from;
        if (abs(dk - d) > 2 * INTRO_FPS || runs[k].a_from >= r->a_to || r->a_from >= runs[k].a_to) continue;
        if (len > run_len(&runs[k]) || (len == run_len(&runs[k]) && r->cost < runs[k].cost)) {
            runs[k] = *r;
            for (; k > 0 && run_len(&runs[k]) > run_len(&runs[k - 1]); k--) {
                IntroRun t = runs[k];
                runs[k] = runs[k - 1], runs[k - 1] = t;
            }
        }
        return;
    }
    int at;
    if (*nr < max) at = (*nr)++;
    else if (len > run_len(&runs[max - 1])) at = max - 1; /* full: only a longer one gets in */
    else return;
    runs[at] = *r;
    for (; at > 0 && run_len(&runs[at]) > run_len(&runs[at - 1]); at--) {
        IntroRun t = runs[at];
        runs[at] = runs[at - 1], runs[at - 1] = t;
    }
}

/* One offset - frame i of a against frame i - d of b -: its stretches worth keeping into runs (c: room for the
   costs). */
static void scan(const IntroPrint *a, const IntroPrint *b, int d, int min_frames, int *c, IntroRun *runs, int *nr, int max) {
    int i0 = d > 0 ? d : 0, i1 = a->n < b->n + d ? a->n : b->n + d, len = i1 - i0;
    if (len < min_frames || len < WIN) return;
    for (int i = i0; i < i1; i++) c[i - i0] = cost(a, i, b, i - d);
    int sum = 0, run_from = -1, last_good = -1;
    for (int k = 0; k < WIN; k++) sum += c[k];
    for (int w = 0; w + WIN <= len; w++) { /* window [w, w + WIN) */
        if (w) sum += c[w + WIN - 1] - c[w - 1];
        bool good = sum <= GOOD_SUM;
        if (good) {
            if (run_from < 0) run_from = w;
            last_good = w;
        }
        if (run_from >= 0 && ((!good && w - last_good > GAP) || w + WIN == len)) {
            int from = run_from, to = last_good + WIN;
            if (to - from >= min_frames) {
                IntroRun r = {i0 + from, i0 + to, 0, 0, 0};
                trim(a, b, d, &r);
                if (run_len(&r) >= min_frames) consider(runs, nr, max, &r);
            }
            run_from = -1;
        }
    }
}

/* Each loud frame's print as four 16-bit pieces (the low and high halves, the even and odd bits), sorted: two
   episodes' equal pieces vote for the offset between them, and only the offsets with votes are scanned. A
   stretch that agrees to ~5 bits in 32 has a few in 100 of its pieces exactly equal; random frames one in 65,536. */
typedef struct {
    uint32_t key; /* (piece kind << 16) | its bits */
    int i;
} Piece;

typedef struct {
    Piece *p;
    int n;
} Pieces;

static uint32_t squeeze(uint32_t v) {
    uint32_t r = 0;
    for (int b = 0; b < 16; b++) r |= ((v >> (2 * b)) & 1u) << b;
    return r;
}

static int cmp_piece(const void *x, const void *y) {
    const Piece *a = x, *b = y;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return (a->i > b->i) - (a->i < b->i);
}

static bool pieces_make(const IntroPrint *p, Pieces *out) {
    out->n = 0;
    out->p = malloc(sizeof(Piece) * 4 * (size_t)(p->n > 0 ? p->n : 1));
    if (!out->p) return false;
    for (int i = 0; i < p->n; i++) {
        if (!p->loud[i]) continue;
        uint32_t v = p->v[i], k[4] = {v & 0xFFFFu, v >> 16, squeeze(v), squeeze(v >> 1)};
        for (uint32_t j = 0; j < 4; j++) out->p[out->n++] = (Piece){j << 16 | k[j], i};
    }
    qsort(out->p, (size_t)out->n, sizeof(Piece), cmp_piece);
    return true;
}

static int match(const IntroPrint *a, const Pieces *pa, const IntroPrint *b, const Pieces *pb, int min_frames, IntroRun *runs,
                 int max_runs) {
    if (!a || !b || a->n < WIN || b->n < WIN || max_runs <= 0) return 0;
    if (min_frames < WIN) min_frames = WIN;
    int na = a->n, nb = b->n, span = na + nb - 1; /* offset d = i - j in -(nb - 1) .. na - 1, at index d + nb - 1 */
    int *votes = calloc((size_t)span, sizeof(int)), *c = malloc(sizeof(int) * (size_t)(na > nb ? na : nb));
    if (!votes || !c) {
        free(votes);
        free(c);
        return 0;
    }
    for (int x = 0, y = 0; x < pa->n && y < pb->n;) { /* equal pieces, by walking both sorted lists */
        uint32_t key = pa->p[x].key;
        if (key < pb->p[y].key) {
            x++;
            continue;
        }
        if (pb->p[y].key < key) {
            y++;
            continue;
        }
        int x1 = x, y1 = y;
        while (x1 < pa->n && pa->p[x1].key == key) x1++;
        while (y1 < pb->n && pb->p[y1].key == key) y1++;
        if (x1 - x <= BUCKET_MAX && y1 - y <= BUCKET_MAX)
            for (int u = x; u < x1; u++)
                for (int w = y; w < y1; w++) votes[pa->p[u].i - pb->p[w].i + nb - 1]++;
        x = x1, y = y1;
    }
    int cand[CANDIDATES], cs[CANDIDATES], ncand = 0;
    for (int k = 0; k < span; k++) { /* the offsets whose neighbourhood (+-1) has the most votes, local peaks only */
        int s = votes[k] + (k > 0 ? votes[k - 1] : 0) + (k + 1 < span ? votes[k + 1] : 0);
        if (s < VOTE_MIN) continue;
        bool peak = true;
        for (int q = k - 3; q <= k + 3 && peak; q++) {
            if (q == k || q < 1 || q + 1 >= span) continue;
            int t = votes[q] + votes[q - 1] + votes[q + 1];
            if (t > s || (t == s && q < k)) peak = false;
        }
        if (!peak) continue;
        int at;
        if (ncand < CANDIDATES) at = ncand++;
        else if (s > cs[CANDIDATES - 1]) at = CANDIDATES - 1;
        else continue;
        for (; at > 0 && cs[at - 1] < s; at--) cand[at] = cand[at - 1], cs[at] = cs[at - 1];
        cand[at] = k, cs[at] = s;
    }
    int nr = 0;
    for (int q = 0; q < ncand; q++)
        for (int dd = -2; dd <= 2; dd++) scan(a, b, cand[q] - (nb - 1) + dd, min_frames, c, runs, &nr, max_runs);
    free(votes);
    free(c);
    return nr;
}

int intro_match(const IntroPrint *a, const IntroPrint *b, int min_frames, IntroRun *runs, int max_runs) {
    if (!a || !b) return 0;
    Pieces pa = {0}, pb = {0};
    int nr = 0;
    if (pieces_make(a, &pa) && pieces_make(b, &pb)) nr = match(a, &pa, b, &pb, min_frames, runs, max_runs);
    free(pa.p);
    free(pb.p);
    return nr;
}

/* ------------------------------------------------------------ a season */

typedef struct {
    int from, to, partner, cost; /* frames of its own episode */
} Cand;

static int overlap(int a0, int a1, int b0, int b1) {
    int lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
    return hi > lo ? hi - lo : 0;
}

static bool same_stretch(const Cand *x, const Cand *y) {
    int o = overlap(x->from, x->to, y->from, y->to), sx = x->to - x->from, sy = y->to - y->from;
    return o * 2 >= (sx < sy ? sx : sy);
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

void intro_find(const IntroPrint *const *prints, int n, const IntroFindOpts *opt, IntroFound *out) {
    IntroFindOpts o = opt ? *opt : (IntroFindOpts){0};
    if (o.min_ms <= 0) o.min_ms = 15000;
    if (o.max_ms <= 0) o.max_ms = 150000;
    if (o.partners <= 0) o.partners = 6;
    for (int i = 0; i < n; i++) out[i] = (IntroFound){-1, -1, 0};
    int usable = 0;
    for (int i = 0; i < n; i++) usable += prints[i] && prints[i]->n > 0;
    if (usable < 2) return;
    int min_frames = (int)(o.min_ms * INTRO_FPS / 1000), max_frames = (int)(o.max_ms * INTRO_FPS / 1000);
    enum { RUNS = 3, VOTERS = 64 };
    int capc = 2 * o.partners * RUNS + 2;
    Cand *cands = calloc((size_t)n * (size_t)capc, sizeof *cands);
    int *nc = calloc((size_t)n, sizeof *nc);
    Pieces *pc = calloc((size_t)n, sizeof *pc);
    bool ok = cands && nc && pc;
    for (int i = 0; ok && i < n; i++)
        if (prints[i] && prints[i]->n > 0) ok = pieces_make(prints[i], &pc[i]);
    for (int i = 0; ok && i < n; i++) {
        if (!prints[i] || !prints[i]->n) continue;
        for (int k = 1, found = 0; found < o.partners && i + k < n; k++) { /* the next `partners` readable episodes */
            const IntroPrint *b = prints[i + k];
            if (!b || !b->n) continue;
            found++;
            IntroRun runs[RUNS];
            int nr = match(prints[i], &pc[i], b, &pc[i + k], min_frames, runs, RUNS);
            for (int r = 0; r < nr; r++) {
                if (run_len(&runs[r]) > max_frames) continue; /* longer than any intro: a recap, a rerun */
                if (nc[i] < capc) cands[i * capc + nc[i]++] = (Cand){runs[r].a_from, runs[r].a_to, i + k, runs[r].cost};
                if (nc[i + k] < capc) cands[(i + k) * capc + nc[i + k]++] = (Cand){runs[r].b_from, runs[r].b_to, i, runs[r].cost};
            }
        }
    }
    for (int i = 0; ok && i < n; i++) {
        const Cand *c = cands + i * capc;
        int best = -1, best_votes = 0, best_len = 0, best_cost = 0;
        for (int x = 0; x < nc[i]; x++) { /* the stretch the most other episodes share */
            int votes = 0, voters[VOTERS], low = c[x].cost;
            for (int y = 0; y < nc[i]; y++) {
                if (!same_stretch(&c[x], &c[y])) continue;
                bool seen = false;
                for (int v = 0; v < votes; v++) seen |= voters[v] == c[y].partner;
                if (!seen && votes < VOTERS) voters[votes++] = c[y].partner;
                if (c[y].cost < low) low = c[y].cost;
            }
            int len = c[x].to - c[x].from;
            if (votes > best_votes || (votes == best_votes && len > best_len)) best = x, best_votes = votes, best_len = len, best_cost = low;
        }
        /* two episodes or more share it - or only one, and then it must agree well (a season with two versions
           of its intro: That 70s Show season 7, whose episodes 5 and 9 have the other) */
        if (best < 0 || (best_votes < 2 && best_cost > STRONG_COST)) continue;
        int froms[VOTERS * RUNS], tos[VOTERS * RUNS], m = 0;
        for (int y = 0; y < nc[i] && m < VOTERS * RUNS; y++)
            if (same_stretch(&c[best], &c[y])) froms[m] = c[y].from, tos[m] = c[y].to, m++;
        qsort(froms, (size_t)m, sizeof(int), cmp_int);
        qsort(tos, (size_t)m, sizeof(int), cmp_int);
        int from = m >= 3 ? froms[1] : froms[0], to = m >= 3 ? tos[m - 2] : tos[m - 1]; /* the outermost but one */
        if (to - from < min_frames || to - from > max_frames) continue;
        const IntroPrint *p = prints[i];
        int64_t hop = 1000 / INTRO_FPS;
        out[i].from_ms = p->start_ms + (int64_t)from * 1000 / INTRO_FPS - hop / 2;
        out[i].to_ms = p->start_ms + (int64_t)(to - 1) * 1000 / INTRO_FPS + hop / 2;
        if (out[i].from_ms < 0) out[i].from_ms = 0;
        out[i].votes = best_votes;
    }
    for (int i = 0; pc && i < n; i++) free(pc[i].p);
    free(pc);
    free(cands);
    free(nc);
}
