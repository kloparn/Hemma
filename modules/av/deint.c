/* modules/av/deint.c — see deint.h. The deinterlacing kernel follows yadif
 * (Michael Niedermayer's "yet another deinterlacing filter"); the inverse
 * telecine follows the field-matching + decimation approach of TIVTC
 * (TFM/TDecimate): match fields by the least combing, drop the most
 * duplicate-looking frame of every five. */
#include "deint.h"

#include <stdlib.h>
#include <string.h>

#define CTHRESH 9 /* a pixel sticking out of both lines of the other field by this much ... */
#define MI 80     /* ... counted per 16x16 block: this many make a frame "combed" (TFM's defaults) */
#define CYCLE 5
#define NONE INT64_MAX

typedef struct {
    uint8_t *buf, *y, *u, *v;
    int64_t pts;
    bool progressive, tff, rff, interpolated;
    int64_t diff; /* IVTC: biggest block difference to the previous matched frame */
} Pic;

struct Deint {
    int w, h, cw, ch, mode;
    Pic in[3]; /* the last three inputs: frame k lives in in[k % 3] */
    int64_t nin;
    Pic out[2];  /* YADIF / BOB / PASS results */
    Pic cyc[CYCLE], last; /* IVTC: the matched frames of this cycle, and the one before it */
    int ncyc;
    bool have_last;
    Pic *ready[CYCLE];
    int nready, iready;
    int64_t index;
    DeintCounters cnt;
};

static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

static bool pic_alloc(Pic *p, int w, int h, int cw, int ch) {
    p->buf = malloc((size_t)w * (size_t)h + 2 * (size_t)cw * (size_t)ch);
    if (!p->buf) return false;
    p->y = p->buf;
    p->u = p->y + (size_t)w * (size_t)h;
    p->v = p->u + (size_t)cw * (size_t)ch;
    return true;
}

static void pic_copy(Deint *d, Pic *dst, const Pic *src) {
    memcpy(dst->buf, src->buf, (size_t)d->w * (size_t)d->h + 2 * (size_t)d->cw * (size_t)d->ch);
    dst->pts = src->pts, dst->progressive = src->progressive, dst->tff = src->tff, dst->rff = src->rff;
    dst->interpolated = false;
}

Deint *deint_new(int width, int height, int mode) {
    if (width < 4 || height < 4 || width > 8192 || height > 8192 || mode < DEINT_YADIF || mode > DEINT_PASS) return NULL;
    Deint *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->w = width, d->h = height, d->cw = (width + 1) / 2, d->ch = (height + 1) / 2, d->mode = mode;
    bool ok = true;
    for (int i = 0; i < 3; i++) ok = ok && pic_alloc(&d->in[i], d->w, d->h, d->cw, d->ch);
    if (mode == DEINT_IVTC) {
        for (int i = 0; i < CYCLE; i++) ok = ok && pic_alloc(&d->cyc[i], d->w, d->h, d->cw, d->ch);
        ok = ok && pic_alloc(&d->last, d->w, d->h, d->cw, d->ch);
    } else {
        for (int i = 0; i < 2; i++) ok = ok && pic_alloc(&d->out[i], d->w, d->h, d->cw, d->ch);
    }
    if (!ok) {
        deint_free(d);
        return NULL;
    }
    return d;
}

void deint_free(Deint *d) {
    if (!d) return;
    for (int i = 0; i < 3; i++) free(d->in[i].buf);
    for (int i = 0; i < 2; i++) free(d->out[i].buf);
    for (int i = 0; i < CYCLE; i++) free(d->cyc[i].buf);
    free(d->last.buf);
    free(d);
}

const DeintCounters *deint_counters(const Deint *d) { return &d->cnt; }

void deint_rate(int mode, int in_num, int in_den, int *out_num, int *out_den) {
    long long n = in_num, m = in_den;
    if (mode == DEINT_IVTC) n *= 4, m *= 5;
    else if (mode == DEINT_BOB) n *= 2;
    long long a = n, b = m;
    while (b) {
        long long t = a % b;
        a = b, b = t;
    }
    if (a <= 0) a = 1;
    *out_num = (int)(n / a), *out_den = (int)(m / a);
}

/* ------------------------------------------------------------ measuring */

/* Worst 16x16 block of combed pixels in the frame woven from top's even rows
   and bottom's odd rows. A pixel is combed when it sticks out of both
   neighbouring rows (the other field) and the 5-tap vertical test agrees,
   which leaves real vertical detail alone. */
int deint_comb(const uint8_t *top, const uint8_t *bottom, int stride, int w, int h) {
    int best = 0;
    int bw = (w + 15) / 16;
    int counts[512];
    if (bw > 512) bw = 512;
    for (int by = 0; by < h; by += 16) {
        memset(counts, 0, sizeof counts);
        int y1 = imin(by + 16, h - 2);
        for (int y = imax(by, 2); y < y1; y++) {
            const uint8_t *s0 = (y & 1 ? bottom : top), *s1 = (y & 1 ? top : bottom); /* this row's field, the other */
            const uint8_t *c = s0 + (size_t)y * stride, *a = s1 + (size_t)(y - 1) * stride, *b = s1 + (size_t)(y + 1) * stride;
            const uint8_t *a2 = s0 + (size_t)(y - 2) * stride, *b2 = s0 + (size_t)(y + 2) * stride;
            for (int bx = 0; bx * 16 < w; bx++) { /* a block's pixels without branches: vectorises (~4x) */
                int x1 = imin(bx * 16 + 16, w), n = 0;
                for (int x = bx * 16; x < x1; x++) {
                    int d1 = c[x] - a[x], d2 = c[x] - b[x];
                    int v = a2[x] + 4 * c[x] + b2[x] - 3 * (a[x] + b[x]);
                    n += (((d1 > CTHRESH) & (d2 > CTHRESH)) | ((d1 < -CTHRESH) & (d2 < -CTHRESH))) &
                         ((v > CTHRESH * 6) | (v < -CTHRESH * 6));
                }
                counts[imin(bx, bw - 1)] += n;
            }
        }
        for (int i = 0; i < bw; i++)
            if (counts[i] > best) best = counts[i];
    }
    return best;
}

/* Biggest 16x16-block luma difference between two frames (sum of |a-b|). */
static int64_t block_diff(const Deint *d, const Pic *a, const Pic *b) {
    int64_t best = 0;
    int bw = (d->w + 15) / 16;
    int64_t sums[512];
    if (bw > 512) bw = 512;
    for (int by = 0; by < d->h; by += 16) {
        memset(sums, 0, sizeof sums);
        int y1 = imin(by + 16, d->h);
        for (int y = by; y < y1; y++) {
            const uint8_t *p = a->y + (size_t)y * d->w, *q = b->y + (size_t)y * d->w;
            for (int x = 0; x < d->w; x++) sums[imin(x >> 4, bw - 1)] += iabs(p[x] - q[x]);
        }
        for (int i = 0; i < bw; i++)
            if (sums[i] > best) best = sums[i];
    }
    return best;
}

/* -------------------------------------------------------------- yadif */

/* One plane. Rows of parity `keep` (0 = top) are copied from cur; the others
   are interpolated. `first`: the kept field is the earlier one in time, so the
   missing field's neighbours in time are prev and cur (else cur and next). */
static void yadif_plane(uint8_t *dst, const uint8_t *prev, const uint8_t *cur, const uint8_t *next, int w, int h, int keep,
                        bool first) {
    const uint8_t *prev2 = first ? prev : cur, *next2 = first ? cur : next;
    for (int y = 0; y < h; y++) {
        uint8_t *o = dst + (size_t)y * w;
        if ((y & 1) == keep) {
            memcpy(o, cur + (size_t)y * w, (size_t)w);
            continue;
        }
        int ym = y > 0 ? y - 1 : y + 1, yp = y + 1 < h ? y + 1 : y - 1; /* the kept field's rows around y */
        if (ym < 0 || ym >= h) ym = yp;
        if (yp < 0 || yp >= h) yp = ym;
        const uint8_t *c = cur + (size_t)ym * w, *e = cur + (size_t)yp * w;
        const uint8_t *pm = prev + (size_t)ym * w, *pp = prev + (size_t)yp * w;
        const uint8_t *nm = next + (size_t)ym * w, *np = next + (size_t)yp * w;
        const uint8_t *p2 = prev2 + (size_t)y * w, *n2 = next2 + (size_t)y * w;
        bool spatial = y >= 2 && y + 2 < h;
        const uint8_t *p2m = spatial ? prev2 + (size_t)(y - 2) * w : NULL, *n2m = spatial ? next2 + (size_t)(y - 2) * w : NULL;
        const uint8_t *p2p = spatial ? prev2 + (size_t)(y + 2) * w : NULL, *n2p = spatial ? next2 + (size_t)(y + 2) * w : NULL;
        for (int x = 0; x < w; x++) {
            int cv = c[x], ev = e[x];
            int dv = (p2[x] + n2[x]) >> 1;
            int t0 = iabs(p2[x] - n2[x]);
            int t1 = (iabs(pm[x] - cv) + iabs(pp[x] - ev)) >> 1;
            int t2 = (iabs(nm[x] - cv) + iabs(np[x] - ev)) >> 1;
            int diff = imax(imax(t0 >> 1, t1), t2);
            int pred = (cv + ev) >> 1;
            if (x >= 3 && x + 3 < w) { /* edge-directed: follow a diagonal when it matches better */
                int score = iabs(c[x - 1] - e[x - 1]) + iabs(cv - ev) + iabs(c[x + 1] - e[x + 1]) - 1;
                int s = iabs(c[x - 2] - e[x]) + iabs(c[x - 1] - e[x + 1]) + iabs(c[x] - e[x + 2]);
                if (s < score) {
                    score = s, pred = (c[x - 1] + e[x + 1]) >> 1;
                    s = iabs(c[x - 3] - e[x + 1]) + iabs(c[x - 2] - e[x + 2]) + iabs(c[x - 1] - e[x + 3]);
                    if (s < score) score = s, pred = (c[x - 2] + e[x + 2]) >> 1;
                }
                s = iabs(c[x] - e[x - 2]) + iabs(c[x + 1] - e[x - 1]) + iabs(c[x + 2] - e[x]);
                if (s < score) {
                    score = s, pred = (c[x + 1] + e[x - 1]) >> 1;
                    s = iabs(c[x + 1] - e[x - 3]) + iabs(c[x + 2] - e[x - 2]) + iabs(c[x + 3] - e[x - 1]);
                    if (s < score) score = s, pred = (c[x + 2] + e[x - 2]) >> 1;
                }
            }
            if (spatial && diff > 2) { /* moving: don't let the temporal clamp hide real vertical detail.
                                         (Still areas skip this - unlike stock yadif - so static
                                         fine lines weave back exactly instead of being smoothed.) */
                int b = (p2m[x] + n2m[x]) >> 1, f = (p2p[x] + n2p[x]) >> 1;
                int mx = imax(imax(dv - ev, dv - cv), imin(b - cv, f - ev));
                int mn = imin(imin(dv - ev, dv - cv), imax(b - cv, f - ev));
                diff = imax(imax(diff, mn), -mx);
            }
            if (pred > dv + diff) pred = dv + diff;
            else if (pred < dv - diff) pred = dv - diff;
            o[x] = (uint8_t)(pred < 0 ? 0 : pred > 255 ? 255 : pred);
        }
    }
}

static void yadif(Deint *d, Pic *dst, const Pic *prev, const Pic *cur, const Pic *next, int keep, bool first) {
    yadif_plane(dst->y, prev->y, cur->y, next->y, d->w, d->h, keep, first);
    yadif_plane(dst->u, prev->u, cur->u, next->u, d->cw, d->ch, keep, first);
    yadif_plane(dst->v, prev->v, cur->v, next->v, d->cw, d->ch, keep, first);
    dst->pts = cur->pts, dst->tff = cur->tff, dst->rff = false, dst->progressive = true;
    dst->interpolated = true;
}

/* -------------------------------------------------------------- IVTC */

static void weave_plane(uint8_t *dst, const uint8_t *top, const uint8_t *bottom, int w, int h) {
    for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * w, (y & 1 ? bottom : top) + (size_t)y * w, (size_t)w);
}

/* Match frame k (with k-1 and k+1 when there are such) into the cycle. */
static void ivtc_match(Deint *d, int64_t k, bool have_next) {
    Pic *cur = &d->in[k % 3], *prev = k > 0 ? &d->in[(k - 1) % 3] : cur, *next = have_next ? &d->in[(k + 1) % 3] : cur;
    Pic *out = &d->cyc[d->ncyc];
    bool tff = cur->tff || cur->progressive;
    /* keep the first field, look for the other one in prev / cur / next */
    const Pic *cand[3] = {cur, prev, next};
    int metric[3] = {0, 0, 0}, best = 0;
    for (int i = 0; i < 3; i++) {
        if (i && cand[i] == cur) {
            metric[i] = 1 << 30;
            continue;
        }
        const uint8_t *top = tff ? cur->y : cand[i]->y, *bot = tff ? cand[i]->y : cur->y;
        metric[i] = deint_comb(top, bot, d->w, d->w, d->h);
        if (metric[i] < metric[best]) best = i;
    }
    const Pic *m = cand[best];
    const Pic *t = tff ? cur : m, *b = tff ? m : cur;
    weave_plane(out->y, t->y, b->y, d->w, d->h);
    weave_plane(out->u, t->u, b->u, d->cw, d->ch);
    weave_plane(out->v, t->v, b->v, d->cw, d->ch);
    out->pts = cur->pts, out->tff = cur->tff, out->rff = false, out->progressive = true, out->interpolated = false;
    if (best == 0) d->cnt.match_c++;
    else if (best == 1) d->cnt.match_p++;
    else d->cnt.match_n++;
    if (metric[best] > MI) { /* an orphan field (cut, video-shot title): deinterlace it */
        yadif(d, out, prev, cur, next, tff ? 0 : 1, true);
        d->cnt.interpolated++;
    }
    const Pic *before = d->ncyc ? &d->cyc[d->ncyc - 1] : d->have_last ? &d->last : NULL;
    out->diff = before ? block_diff(d, out, before) : NONE;
    d->ncyc++;
}

static void ivtc_emit(Deint *d, bool decimate) {
    int drop = -1;
    if (decimate && d->ncyc == CYCLE) {
        int64_t lo = NONE;
        for (int i = 0; i < CYCLE; i++)
            if (d->cyc[i].diff < lo) lo = d->cyc[i].diff, drop = i;
        if (drop >= 0) d->cnt.dropped++;
    }
    for (int i = 0; i < d->ncyc; i++)
        if (i != drop) d->ready[d->nready++] = &d->cyc[i];
    if (d->ncyc) {
        pic_copy(d, &d->last, &d->cyc[d->ncyc - 1]);
        d->have_last = true;
    }
    d->ncyc = 0;
}

/* ---------------------------------------------------------------- API */

static void begin_turn(Deint *d) { d->nready = d->iready = 0; } /* earlier results are given up */

/* YADIF / BOB / PASS: produce frame k's output(s), next = k + 1 if present. */
static void plain(Deint *d, int64_t k, bool have_next) {
    Pic *cur = &d->in[k % 3], *prev = k > 0 ? &d->in[(k - 1) % 3] : cur, *next = have_next ? &d->in[(k + 1) % 3] : cur;
    bool tff = cur->tff;
    if (cur->progressive || d->mode == DEINT_PASS) {
        /* PASS keeps every picture whose fields belong together, whatever the flags say */
        bool keep = cur->progressive || deint_comb(cur->y, cur->y, d->w, d->w, d->h) <= MI;
        if (keep) {
            pic_copy(d, &d->out[0], cur);
            d->out[0].interpolated = false;
        } else {
            yadif(d, &d->out[0], prev, cur, next, tff ? 0 : 1, true);
            d->cnt.interpolated++;
        }
        d->ready[d->nready++] = &d->out[0];
        if (d->mode == DEINT_BOB) d->ready[d->nready++] = &d->out[0]; /* same picture, twice */
        return;
    }
    yadif(d, &d->out[0], prev, cur, next, tff ? 0 : 1, true); /* the first field */
    d->cnt.interpolated++;
    d->ready[d->nready++] = &d->out[0];
    if (d->mode == DEINT_BOB) {
        yadif(d, &d->out[1], prev, cur, next, tff ? 1 : 0, false); /* the second field */
        d->ready[d->nready++] = &d->out[1];
    }
}

int deint_push(Deint *d, const DeintIn *in) {
    if (!in || !in->y || !in->u || !in->v || in->stride_y < d->w || in->stride_c < d->cw) return -1;
    begin_turn(d);
    Pic *p = &d->in[d->nin % 3];
    for (int y = 0; y < d->h; y++) memcpy(p->y + (size_t)y * d->w, in->y + (size_t)y * in->stride_y, (size_t)d->w);
    for (int y = 0; y < d->ch; y++) {
        memcpy(p->u + (size_t)y * d->cw, in->u + (size_t)y * in->stride_c, (size_t)d->cw);
        memcpy(p->v + (size_t)y * d->cw, in->v + (size_t)y * in->stride_c, (size_t)d->cw);
    }
    p->pts = in->pts, p->progressive = in->progressive, p->tff = in->tff, p->rff = in->rff;
    d->nin++;
    d->cnt.in++;
    if (d->nin >= 2) {
        int64_t k = d->nin - 2;
        if (d->mode == DEINT_IVTC) {
            ivtc_match(d, k, true);
            if (d->ncyc == CYCLE) ivtc_emit(d, true);
        } else {
            plain(d, k, true);
        }
    }
    return d->nready;
}

int deint_flush(Deint *d) {
    begin_turn(d);
    if (d->nin >= 1) {
        int64_t k = d->nin - 1;
        if (d->mode == DEINT_IVTC) {
            ivtc_match(d, k, false);
            ivtc_emit(d, d->ncyc == CYCLE);
        } else {
            plain(d, k, false);
        }
        d->nin = 0; /* nothing more to give */
    }
    return d->nready;
}

bool deint_pull(Deint *d, DeintOut *out) {
    if (d->iready >= d->nready) return false;
    const Pic *p = d->ready[d->iready++];
    out->y = p->y, out->u = p->u, out->v = p->v;
    out->stride_y = d->w, out->stride_c = d->cw, out->width = d->w, out->height = d->h;
    out->pts = p->pts;
    out->index = d->index++;
    out->fields = d->mode == DEINT_BOB ? 1 : d->mode == DEINT_PASS && p->rff ? 3 : 2;
    out->interpolated = p->interpolated;
    d->cnt.out++;
    return true;
}

/* -------------------------------------------------------------- analysis */

static int popcount10(uint32_t v) {
    int n = 0;
    for (int i = 0; i < 10; i++) n += (int)(v >> i & 1);
    return n;
}

/* Motion: a thumbnail of block averages, compared with the previous frame's.
   Averaging over whole blocks hides compression noise; anything real moving
   shifts some block by several levels. */
static bool moved(DeintStats *s, const DeintIn *in, int w, int h) {
    uint8_t t[DEINT_THUMB_W * DEINT_THUMB_H];
    for (int by = 0; by < DEINT_THUMB_H; by++)
        for (int bx = 0; bx < DEINT_THUMB_W; bx++) {
            int x0 = bx * w / DEINT_THUMB_W, x1 = (bx + 1) * w / DEINT_THUMB_W;
            int y0 = by * h / DEINT_THUMB_H, y1 = (by + 1) * h / DEINT_THUMB_H;
            uint32_t sum = 0, cnt = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) sum += in->y[(size_t)y * in->stride_y + x], cnt++;
            t[by * DEINT_THUMB_W + bx] = (uint8_t)(cnt ? sum / cnt : 0);
        }
    bool m = false;
    if (s->have_thumb)
        for (int i = 0; i < DEINT_THUMB_W * DEINT_THUMB_H && !m; i++) m = iabs(t[i] - s->thumb[i]) >= 4;
    memcpy(s->thumb, t, sizeof t);
    s->have_thumb = true;
    return m;
}

void deint_analyze(DeintStats *s, const DeintIn *in, int width, int height) {
    s->frames++;
    s->progressive += in->progressive;
    s->rff += in->rff;
    bool combed = !in->progressive && deint_comb(in->y, in->y, in->stride_y, width, height) > MI;
    bool mv = moved(s, in, width, height);
    s->combed += combed;
    s->moving += mv;
    s->hist = (s->hist << 1 | (combed ? 1u : 0u)) & 0x3FF;
    s->mhist = (s->mhist << 1 | (mv ? 1u : 0u)) & 0x3FF;
    if (++s->nhist % 10) return;
    uint32_t h1 = s->hist & 31, h2 = s->hist >> 5 & 31;
    int n = popcount10(s->hist), m = popcount10(s->mhist);
    bool pair = h1 == 3 || h1 == 6 || h1 == 12 || h1 == 24 || h1 == 17; /* two neighbours (cyclic) in five */
    if (!n) {
        if (m >= 5) s->clean++;
        else s->still++;
    } else if (n >= 8) s->video++;
    else if (h1 == h2 && pair) s->cadence++;
    else s->mixed++;
}

int deint_kind(const DeintStats *s) {
    if (!s->frames) return DEINT_KIND_UNKNOWN;
    if (s->progressive * 20 >= s->frames * 19) return s->rff * 5 >= s->frames ? DEINT_KIND_SOFT_TELECINE : DEINT_KIND_PROGRESSIVE;
    int64_t windows = s->still + s->video + s->cadence + s->mixed + s->clean, active = windows - s->still;
    if (!active || s->clean * 5 >= active * 4) return DEINT_KIND_PROGRESSIVE; /* motion, yet whole pictures */
    if (s->cadence >= 2 * s->video && s->cadence * 20 >= windows) return DEINT_KIND_HARD_TELECINE;
    return DEINT_KIND_INTERLACED;
}

const char *deint_kind_name(int kind) {
    switch (kind) {
    case DEINT_KIND_PROGRESSIVE: return "progressive pictures";
    case DEINT_KIND_SOFT_TELECINE: return "film, soft telecine (progressive frames + RFF)";
    case DEINT_KIND_HARD_TELECINE: return "film, hard telecine (3:2 pulldown in the fields)";
    case DEINT_KIND_INTERLACED: return "interlaced video";
    default: return "unknown";
    }
}

int deint_mode_for(int kind) {
    switch (kind) {
    case DEINT_KIND_PROGRESSIVE:
    case DEINT_KIND_SOFT_TELECINE: return DEINT_PASS;
    case DEINT_KIND_HARD_TELECINE: return DEINT_IVTC;
    default: return DEINT_YADIF;
    }
}
