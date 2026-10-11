/* modules/av/flac.c — see flac.h. Section numbers refer to RFC 9639 (FLAC). */
#include "flac.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif

#define MAX_ORDER 32
#define MAX_PORDER 8 /* up to 256 partitions per residual */
#define DEFAULT_BLOCK 4096
#define DEFAULT_LPC 12

enum { SUB_CONST, SUB_VERBATIM, SUB_FIXED, SUB_LPC };

/* ------------------------------------------------------------------- MD5 */

typedef struct {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
    size_t nbuf;
} Md5;

static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(Md5 *m, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint8_t R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) & 15;
        else f = c ^ (b | ~d), g = (7 * i) & 15;
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    m->h[0] += a, m->h[1] += b, m->h[2] += c, m->h[3] += d;
}

static void md5_init(Md5 *m) {
    memset(m, 0, sizeof *m);
    m->h[0] = 0x67452301, m->h[1] = 0xefcdab89, m->h[2] = 0x98badcfe, m->h[3] = 0x10325476;
}

static void md5_update(Md5 *m, const uint8_t *p, size_t n) {
    m->len += n;
    while (n) {
        if (!m->nbuf && n >= 64) {
            md5_block(m, p);
            p += 64, n -= 64;
            continue;
        }
        size_t k = 64 - m->nbuf < n ? 64 - m->nbuf : n;
        memcpy(m->buf + m->nbuf, p, k);
        m->nbuf += k, p += k, n -= k;
        if (m->nbuf == 64) {
            md5_block(m, m->buf);
            m->nbuf = 0;
        }
    }
}

static void md5_final(Md5 m, uint8_t out[16]) { /* by value: finishing a copy leaves the running sum alone */
    uint64_t bits = m.len * 8;
    uint8_t pad[72] = {0x80};
    size_t padn = (m.nbuf < 56 ? 56 : 120) - m.nbuf;
    for (int i = 0; i < 8; i++) pad[padn + (size_t)i] = (uint8_t)(bits >> (8 * i));
    md5_update(&m, pad, padn + 8);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(m.h[i / 4] >> (8 * (i % 4)));
}

void flac_md5(const void *p, size_t n, uint8_t out[16]) {
    Md5 m;
    md5_init(&m);
    md5_update(&m, p, n);
    md5_final(m, out);
}

/* -------------------------------------------------------------------- CRC */

uint8_t flac_crc8(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

uint16_t flac_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
    }
    return c;
}

/* ------------------------------------------------------------- bit writer */

typedef struct {
    uint8_t *p;
    size_t cap, len;
    uint64_t acc;
    int n; /* pending bits in acc (< 8 between calls) */
} Bw;

static inline uint32_t lowmask(int n) { return n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1; }

static inline void put(Bw *w, uint32_t v, int n) { /* n = 0..32 */
    if (!n) return;
    w->acc = (w->acc << n) | (v & lowmask(n));
    w->n += n;
    while (w->n >= 8) {
        w->n -= 8;
        if (w->len < w->cap) w->p[w->len] = (uint8_t)(w->acc >> w->n);
        w->len++;
    }
}

static inline void put_signed(Bw *w, int32_t v, int n) { put(w, (uint32_t)v, n); }

static void put_unary_zeros(Bw *w, uint32_t q) { /* q zeros, then a 1 */
    while (q >= 32) {
        put(w, 0, 32);
        q -= 32;
    }
    put(w, 1, (int)q + 1);
}

static inline void put_rice(Bw *w, uint32_t u, int k) {
    uint32_t q = u >> k;
    if (q + 1 + (uint32_t)k <= 32) put(w, (1u << k) | (u & lowmask(k)), (int)q + 1 + k);
    else {
        put_unary_zeros(w, q);
        put(w, u, k);
    }
}

static void align(Bw *w) {
    if (w->n) put(w, 0, 8 - w->n);
}

/* --------------------------------------------------------------- encoder */

typedef struct {
    int type, order, wasted, bps; /* bps: bits of the (shifted) samples this subframe codes */
    int prec, shift;
    int32_t coef[MAX_ORDER];
    int porder, method;           /* method 0: 4-bit Rice parameters, 1: 5-bit (Rice2) */
    uint8_t param[1 << MAX_PORDER];
    int8_t raw[1 << MAX_PORDER]; /* escaped partitions: raw bits per sample, else -1 */
    uint64_t bits;               /* whole subframe */
} Sub;

struct Flac {
    FlacConfig c;
    int block, max_order;
    uint64_t frame_no, total;
    uint32_t min_frame, max_frame;
    Md5 md5;
    int32_t *pend; /* flac_write buffer */
    int npend;
    uint8_t *fbuf;
    int32_t *ch[FLAC_MAX_CHANNELS + 2]; /* deinterleaved; [channels] = mid, [channels+1] = side */
    int32_t *shifted;                   /* a channel without its wasted bits */
    int32_t *res;
    double *win, *wx;
    uint8_t *bytes; /* one frame's samples as little-endian bytes, for the MD5 */
    Sub sub[FLAC_MAX_CHANNELS + 2];
};

static int ilog2_u64(uint64_t v) { /* floor(log2(v)), v > 0 */
    int r = 0;
    while (v >>= 1) r++;
    return r;
}

static int bitlen(uint32_t v) { /* bits to hold v: 0 for 0 */
    int r = 0;
    while (v) r++, v >>= 1;
    return r;
}

static inline uint32_t zigzag(int32_t r) { return ((uint32_t)r << 1) ^ (uint32_t)(r >> 31); }

/* --- Rice planning (9.2.7) */

/* Cheapest parameter for `cnt` values summing to `sum` (estimate: cnt*(k+1) + sum>>k). */
static int rice_k_estimate(uint64_t sum, uint32_t cnt, int kmax, uint64_t *bits) {
    int k0 = 0;
    if (cnt && sum > cnt) k0 = ilog2_u64(sum / cnt);
    int best = 0;
    uint64_t bb = UINT64_MAX;
    for (int k = k0 > 0 ? k0 - 1 : 0; k <= k0 + 1 && k <= kmax; k++) {
        uint64_t b = (uint64_t)cnt * (uint64_t)(k + 1) + (sum >> k);
        if (b < bb) bb = b, best = k;
    }
    *bits = bb;
    return best;
}

/* Exact bits of one partition with parameter k. */
static uint64_t rice_exact(const int32_t *r, uint32_t cnt, int k) {
    uint64_t b = (uint64_t)cnt * (uint64_t)(k + 1);
    for (uint32_t i = 0; i < cnt; i++) b += zigzag(r[i]) >> k;
    return b;
}

/* Plan the residual res[order..n): partition order, method, parameters and
   escapes. Returns its size in bits (coding method and order fields included). */
static uint64_t rice_plan(const int32_t *res, int n, int order, Sub *s) {
    int pmax = MAX_PORDER;
    while (pmax > 0 && ((n & ((1 << pmax) - 1)) || (n >> pmax) <= order)) pmax--;
    uint64_t sum[1 << MAX_PORDER];
    uint32_t mx[1 << MAX_PORDER];
    int parts = 1 << pmax, psize = n >> pmax;
    for (int p = 0; p < parts; p++) {
        int a = p ? p * psize : order, z = (p + 1) * psize;
        uint64_t sm = 0;
        uint32_t m = 0;
        for (int i = a; i < z; i++) {
            uint32_t u = zigzag(res[i]);
            sm += u;
            if (u > m) m = u;
        }
        sum[p] = sm, mx[p] = m;
    }
    uint64_t best_bits = UINT64_MAX;
    int best_p = 0;
    for (int po = pmax; po >= 0; po--) {
        int np = 1 << po, sz = n >> po;
        uint64_t bits = 2 + 4;
        bool wide = false;
        for (int p = 0; p < np; p++) {
            uint32_t cnt = (uint32_t)(p ? sz : sz - order);
            uint64_t pb;
            int k = rice_k_estimate(sum[p], cnt, 30, &pb);
            int rb = bitlen(mx[p]);
            uint64_t raw = (uint64_t)rb * cnt + 5;
            if (rb <= 31 && raw < pb) pb = raw;
            else if (k > 14) wide = true;
            bits += pb;
        }
        bits += (uint64_t)np * (wide ? 5 : 4);
        if (bits < best_bits) best_bits = bits, best_p = po;
        if (po) /* merge pairs for the next coarser order */
            for (int p = 0; p < np / 2; p++) {
                sum[p] = sum[2 * p] + sum[2 * p + 1];
                mx[p] = mx[2 * p] > mx[2 * p + 1] ? mx[2 * p] : mx[2 * p + 1];
            }
    }
    /* exact pass for the chosen partition order */
    s->porder = best_p;
    s->method = 0;
    int np = 1 << best_p, sz = n >> best_p;
    uint64_t bits = 2 + 4;
    for (int p = 0; p < np; p++) {
        int a = p ? p * sz : order;
        uint32_t cnt = (uint32_t)((p + 1) * sz - a);
        const int32_t *r = res + a;
        uint64_t sm = 0;
        uint32_t m = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t u = zigzag(r[i]);
            sm += u;
            if (u > m) m = u;
        }
        uint64_t est;
        int k0 = rice_k_estimate(sm, cnt, 30, &est), bk = k0;
        uint64_t bb = UINT64_MAX;
        for (int k = k0 > 0 ? k0 - 1 : 0; k <= k0 + 1 && k <= 30; k++) {
            uint64_t b = rice_exact(r, cnt, k);
            if (b < bb) bb = b, bk = k;
        }
        int rb = bitlen(m);
        if (rb <= 31 && (uint64_t)rb * cnt + 5 <= bb) { /* escape: raw two's complement samples */
            s->raw[p] = (int8_t)rb;
            s->param[p] = 0;
            bits += (uint64_t)rb * cnt + 5;
        } else {
            s->raw[p] = -1;
            s->param[p] = (uint8_t)bk;
            if (bk > 14) s->method = 1;
            bits += bb;
        }
    }
    bits += (uint64_t)np * (s->method ? 5 : 4);
    return bits;
}

/* --- prediction */

static void fixed_residual(const int32_t *x, int n, int order, int32_t *r) {
    switch (order) {
    case 0: memcpy(r, x, (size_t)n * sizeof *r); break;
    case 1:
        for (int i = 1; i < n; i++) r[i] = x[i] - x[i - 1];
        break;
    case 2:
        for (int i = 2; i < n; i++) r[i] = x[i] - 2 * x[i - 1] + x[i - 2];
        break;
    case 3:
        for (int i = 3; i < n; i++) r[i] = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3];
        break;
    default:
        for (int i = 4; i < n; i++) r[i] = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4];
        break;
    }
}

/* Sum of |residual| of fixed orders 0..4 in one pass (to pick the order). */
static int best_fixed_order(const int32_t *x, int n) {
    uint64_t e[5] = {0};
    if (n < 5) return n > 1 ? 1 : 0;
    int64_t p1 = x[3] - x[2], p2 = p1 - (x[2] - x[1]), p3 = p2 - ((x[2] - x[1]) - (x[1] - x[0]));
    for (int i = 4; i < n; i++) {
        int64_t e0 = x[i], e1 = e0 - x[i - 1], e2 = e1 - p1, e3 = e2 - p2, e4 = e3 - p3;
        e[0] += (uint64_t)(e0 < 0 ? -e0 : e0);
        e[1] += (uint64_t)(e1 < 0 ? -e1 : e1);
        e[2] += (uint64_t)(e2 < 0 ? -e2 : e2);
        e[3] += (uint64_t)(e3 < 0 ? -e3 : e3);
        e[4] += (uint64_t)(e4 < 0 ? -e4 : e4);
        p1 = e1, p2 = e2, p3 = e3;
    }
    int best = 0;
    for (int k = 1; k < 5; k++)
        if (e[k] < e[best]) best = k;
    return best;
}

static bool lpc_residual(const int32_t *x, int n, const int32_t *coef, int order, int shift, int32_t *r) {
    for (int i = order; i < n; i++) {
        int64_t sm = 0;
        for (int j = 0; j < order; j++) sm += (int64_t)coef[j] * x[i - 1 - j];
        int64_t v = (int64_t)x[i] - (sm >> shift);
        if (v < INT32_MIN || v > INT32_MAX) return false;
        r[i] = (int32_t)v;
    }
    return true;
}

/* Levinson-Durbin: lp[k][j] predicts x[i] from x[i-1-j] with order k+1. */
static int levinson(const double *autoc, int max_order, double lp[][MAX_ORDER], double *err) {
    double lpc[MAX_ORDER], e = autoc[0];
    for (int i = 0; i < max_order; i++) {
        double r = -autoc[i + 1];
        for (int j = 0; j < i; j++) r -= lpc[j] * autoc[i - j];
        r /= e;
        lpc[i] = r;
        int j = 0;
        for (; j < i / 2; j++) {
            double t = lpc[j];
            lpc[j] += r * lpc[i - 1 - j];
            lpc[i - 1 - j] += r * t;
        }
        if (i & 1) lpc[j] += lpc[j] * r;
        e *= 1.0 - r * r;
        for (j = 0; j <= i; j++) lp[i][j] = -lpc[j];
        err[i] = e;
        if (e <= 0.0) return i + 1;
    }
    return max_order;
}

/* Quantise to `prec` signed bits with error feedback; false when hopeless. */
static bool quantize(const double *c, int order, int prec, int32_t *q, int *shift) {
    double cmax = 0;
    for (int i = 0; i < order; i++)
        if (fabs(c[i]) > cmax) cmax = fabs(c[i]);
    if (cmax <= 0) return false;
    int qmax = (1 << (prec - 1)) - 1, qmin = -(1 << (prec - 1));
    int log2cmax;
    frexp(cmax, &log2cmax);
    log2cmax--;
    int sh = prec - 1 - log2cmax - 1;
    if (sh > 15) sh = 15;
    double scale;
    if (sh >= 0) scale = (double)(1 << sh);
    else { /* decoders don't allow a negative shift: scale the coefficients down instead */
        scale = 1.0 / (double)(1 << -sh);
        sh = 0;
    }
    double err = 0;
    for (int i = 0; i < order; i++) {
        err += c[i] * scale;
        long v = lround(err);
        if (v > qmax) v = qmax;
        else if (v < qmin) v = qmin;
        err -= (double)v;
        q[i] = (int32_t)v;
    }
    *shift = sh;
    return true;
}

static int qlp_precision(int bps, int n) {
    if (bps < 16) return 2 + bps / 2 > 5 ? 2 + bps / 2 : 5;
    if (bps == 16) return n <= 192 ? 7 : n <= 384 ? 8 : n <= 576 ? 9 : n <= 1152 ? 10 : n <= 2304 ? 11 : n <= 4608 ? 12 : 13;
    return n <= 384 ? 13 : n <= 1152 ? 14 : 15;
}

static void tukey(double *w, int n) {
    int np = (int)(0.5 / 2.0 * n) - 1;
    for (int i = 0; i < n; i++) w[i] = 1.0;
    if (np > 0)
        for (int i = 0; i <= np; i++) {
            w[i] = 0.5 - 0.5 * cos(M_PI * i / np);
            w[n - np - 1 + i] = 0.5 - 0.5 * cos(M_PI * (i + np) / np);
        }
}

/* --- one channel: choose the cheapest subframe */

static void analyse(Flac *f, const int32_t *x, int n, int bps, Sub *s) {
    memset(s, 0, sizeof *s);
    s->bps = bps;
    /* wasted bits: trailing zeros shared by every sample */
    uint32_t orv = 0;
    for (int i = 0; i < n; i++) orv |= (uint32_t)x[i];
    if (!orv) { /* digital silence */
        s->type = SUB_CONST;
        s->bits = 8 + (uint64_t)bps;
        return;
    }
    int w = 0;
    while (!(orv & 1)) orv >>= 1, w++;
    if (w >= bps) w = 0;
    s->wasted = w;
    int b = bps - w;
    const int32_t *v = x;
    if (w) {
        for (int i = 0; i < n; i++) f->shifted[i] = x[i] >> w;
        v = f->shifted;
    }
    uint64_t head = 8 + (uint64_t)w; /* header byte + unary wasted count */
    bool constant = true;
    for (int i = 1; i < n && constant; i++) constant = v[i] == v[0];
    if (constant) {
        s->type = SUB_CONST;
        s->bps = b;
        s->bits = head + (uint64_t)b;
        return;
    }
    s->type = SUB_VERBATIM;
    s->bps = b;
    s->bits = head + (uint64_t)n * (uint64_t)b;

    Sub t = *s;
    /* FIXED: order by the cheap one-pass estimate, then plan exactly */
    int fo = best_fixed_order(v, n);
    if (fo < n) {
        fixed_residual(v, n, fo, f->res);
        t.type = SUB_FIXED;
        t.order = fo;
        t.bits = head + (uint64_t)fo * (uint64_t)b + rice_plan(f->res, n, fo, &t);
        if (t.bits < s->bits) *s = t;
    }
    /* LPC */
    int maxo = f->max_order < n - 1 ? f->max_order : n - 1;
    if (maxo >= 1 && n >= 32) {
        double *wx = f->wx;
        if (n == f->block)
            for (int i = 0; i < n; i++) wx[i] = (double)v[i] * f->win[i];
        else { /* a short last block gets its own window */
            tukey(wx, n);
            for (int i = 0; i < n; i++) wx[i] *= (double)v[i];
        }
        double autoc[MAX_ORDER + 1];
        for (int k = 0; k <= maxo; k++) {
            double sm = 0;
            for (int i = k; i < n; i++) sm += wx[i] * wx[i - k];
            autoc[k] = sm;
        }
        if (autoc[0] > 0) {
            double lp[MAX_ORDER][MAX_ORDER], err[MAX_ORDER];
            int got = levinson(autoc, maxo, lp, err);
            int prec = qlp_precision(b, n);
            /* expected bits per order (libFLAC's estimate), then try the best and its neighbours */
            double best_est = 1e300;
            int best_o = 1;
            for (int o = 1; o <= got; o++) {
                double e = err[o - 1], bpsamp = 0;
                if (e > 0) {
                    bpsamp = 0.5 * log(0.5 / n * e) / M_LN2;
                    if (bpsamp < 0) bpsamp = 0;
                } else if (e < 0) bpsamp = 1e32;
                double est = bpsamp * (n - o) + (double)o * (prec + b);
                if (est < best_est) best_est = est, best_o = o;
            }
            for (int o = best_o - 1; o <= best_o + 1; o++) {
                if (o < 1 || o > got) continue;
                Sub u = *s;
                u.type = SUB_LPC;
                u.order = o;
                u.prec = prec;
                if (!quantize(lp[o - 1], o, prec, u.coef, &u.shift)) continue;
                if (!lpc_residual(v, n, u.coef, o, u.shift, f->res)) continue;
                u.bits = head + (uint64_t)o * (uint64_t)b + 4 + 5 + (uint64_t)o * (uint64_t)prec + rice_plan(f->res, n, o, &u);
                if (u.bits < s->bits) *s = u;
            }
        }
    }
}

static void write_residual(Bw *w, const int32_t *res, int n, const Sub *s) {
    put(w, (uint32_t)s->method, 2);
    put(w, (uint32_t)s->porder, 4);
    int np = 1 << s->porder, sz = n >> s->porder, pbits = s->method ? 5 : 4;
    uint32_t esc = s->method ? 31 : 15;
    for (int p = 0; p < np; p++) {
        int a = p ? p * sz : s->order, z = (p + 1) * sz;
        if (s->raw[p] >= 0) {
            put(w, esc, pbits);
            put(w, (uint32_t)s->raw[p], 5);
            for (int i = a; i < z; i++) put_signed(w, res[i], s->raw[p]);
        } else {
            int k = s->param[p];
            put(w, (uint32_t)k, pbits);
            for (int i = a; i < z; i++) put_rice(w, zigzag(res[i]), k);
        }
    }
}

static void write_subframe(Flac *f, Bw *w, const int32_t *x, int n, const Sub *s) {
    const int32_t *v = x;
    if (s->wasted && s->type != SUB_CONST) {
        for (int i = 0; i < n; i++) f->shifted[i] = x[i] >> s->wasted;
        v = f->shifted;
    }
    int type = s->type == SUB_CONST ? 0 : s->type == SUB_VERBATIM ? 1 : s->type == SUB_FIXED ? 8 + s->order : 32 + s->order - 1;
    put(w, 0, 1);
    put(w, (uint32_t)type, 6);
    put(w, s->wasted ? 1 : 0, 1);
    if (s->wasted) put_unary_zeros(w, (uint32_t)(s->wasted - 1));
    switch (s->type) {
    case SUB_CONST: put_signed(w, s->wasted ? x[0] >> s->wasted : x[0], s->bps); break;
    case SUB_VERBATIM:
        for (int i = 0; i < n; i++) put_signed(w, v[i], s->bps);
        break;
    case SUB_FIXED:
        for (int i = 0; i < s->order; i++) put_signed(w, v[i], s->bps);
        fixed_residual(v, n, s->order, f->res);
        write_residual(w, f->res, n, s);
        break;
    default:
        for (int i = 0; i < s->order; i++) put_signed(w, v[i], s->bps);
        put(w, (uint32_t)(s->prec - 1), 4);
        put_signed(w, s->shift, 5);
        for (int i = 0; i < s->order; i++) put_signed(w, s->coef[i], s->prec);
        lpc_residual(v, n, s->coef, s->order, s->shift, f->res);
        write_residual(w, f->res, n, s);
        break;
    }
}

/* --- frame header (9.1) */

static int blocksize_code(int n, int *extra) {
    *extra = 0;
    if (n == 192) return 1;
    if (n == 576) return 2;
    if (n == 1152) return 3;
    if (n == 2304) return 4;
    if (n == 4608) return 5;
    for (int k = 8; k <= 15; k++)
        if (n == 256 << (k - 8)) return k;
    if (n <= 256) {
        *extra = 8;
        return 6;
    }
    *extra = 16;
    return 7;
}

static int rate_code(int rate, int *extra) {
    static const int std[] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    *extra = 0;
    for (int k = 1; k < 12; k++)
        if (rate == std[k]) return k;
    if (rate % 1000 == 0 && rate / 1000 <= 255) return *extra = 8, 12;
    if (rate <= 65535) return *extra = 16, 13;
    if (rate % 10 == 0 && rate / 10 <= 65535) return *extra = 16, 14;
    return 0; /* "see STREAMINFO" */
}

static int bits_code(int bits) {
    switch (bits) {
    case 8: return 1;
    case 12: return 2;
    case 16: return 4;
    case 20: return 5;
    case 24: return 6;
    default: return 0;
    }
}

static void put_utf8(Bw *w, uint64_t v) {
    if (v < 0x80) {
        put(w, (uint32_t)v, 8);
        return;
    }
    int nb = v < 0x800 ? 2 : v < 0x10000 ? 3 : v < 0x200000 ? 4 : v < 0x4000000 ? 5 : v < 0x80000000ULL ? 6 : 7;
    uint32_t lead = nb == 7 ? 0xFE : (0xFF00u >> nb) & 0xFF;
    int first_bits = nb == 7 ? 0 : 7 - nb;
    put(w, lead | ((uint32_t)(v >> (6 * (nb - 1))) & lowmask(first_bits)), 8);
    for (int i = nb - 2; i >= 0; i--) put(w, 0x80 | ((uint32_t)(v >> (6 * i)) & 0x3F), 8);
}

/* ------------------------------------------------------------------- API */

Flac *flac_new(const FlacConfig *cfg) {
    FlacConfig c = *cfg;
    if (!c.block_size) c.block_size = DEFAULT_BLOCK;
    if (c.rate < 1 || c.rate > 655350 || c.channels < 1 || c.channels > FLAC_MAX_CHANNELS || c.bits < 4 || c.bits > 24 ||
        c.block_size < 16 || c.block_size > 65535 || c.max_lpc_order < -1 || c.max_lpc_order > MAX_ORDER)
        return NULL;
    Flac *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->c = c;
    f->block = c.block_size;
    f->max_order = c.max_lpc_order == 0 ? DEFAULT_LPC : c.max_lpc_order < 0 ? 0 : c.max_lpc_order;
    size_t n = (size_t)f->block;
    bool ok = true;
    for (int i = 0; i < c.channels + 2; i++) ok = ok && (f->ch[i] = malloc(n * sizeof(int32_t)));
    ok = ok && (f->shifted = malloc(n * sizeof(int32_t))) && (f->res = malloc(n * sizeof(int32_t))) &&
         (f->win = malloc(n * sizeof(double))) && (f->wx = malloc(n * sizeof(double))) &&
         (f->bytes = malloc(n * (size_t)c.channels * 3)) && (f->pend = malloc(n * (size_t)c.channels * sizeof(int32_t)));
    if (!ok) {
        flac_free(f);
        return NULL;
    }
    tukey(f->win, f->block);
    md5_init(&f->md5);
    return f;
}

void flac_free(Flac *f) {
    if (!f) return;
    for (int i = 0; i < FLAC_MAX_CHANNELS + 2; i++) free(f->ch[i]);
    free(f->shifted);
    free(f->res);
    free(f->win);
    free(f->wx);
    free(f->bytes);
    free(f->pend);
    free(f->fbuf);
    free(f);
}

size_t flac_frame_bound(const Flac *f) {
    size_t per = 1 + 4 + ((size_t)f->block * (size_t)(f->c.bits + 1) + 7) / 8;
    return 32 + (size_t)f->c.channels * per;
}

uint64_t flac_samples(const Flac *f) { return f->total; }

long flac_encode_frame(Flac *f, const int32_t *pcm, int n, uint8_t *out, size_t cap) {
    int nch = f->c.channels, bps = f->c.bits;
    if (n < 1 || n > f->block) return -1;
    int32_t lo = -(1 << (bps - 1)), hi = (1 << (bps - 1)) - 1;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < nch; c++) {
            int32_t v = pcm[(size_t)i * (size_t)nch + (size_t)c];
            if (v < lo || v > hi) return -1;
            f->ch[c][i] = v;
        }
    /* MD5 over little-endian samples of (bits+7)/8 bytes */
    int bw = (bps + 7) / 8;
    size_t nb = 0;
    for (int i = 0; i < n * nch; i++)
        for (int k = 0; k < bw; k++) f->bytes[nb++] = (uint8_t)((uint32_t)pcm[i] >> (8 * k));
    md5_update(&f->md5, f->bytes, nb);

    /* subframes; stereo picks the cheapest of L/R, L/S, S/R, M/S */
    int assign = nch - 1;
    const int32_t *src[FLAC_MAX_CHANNELS];
    Sub *use[FLAC_MAX_CHANNELS];
    if (nch == 2) {
        int32_t *l = f->ch[0], *r = f->ch[1], *m = f->ch[2], *s = f->ch[3];
        for (int i = 0; i < n; i++) {
            s[i] = l[i] - r[i];
            m[i] = (l[i] + r[i]) >> 1; /* arithmetic shift: floor, as the decoder expects */
        }
        analyse(f, l, n, bps, &f->sub[0]);
        analyse(f, r, n, bps, &f->sub[1]);
        analyse(f, m, n, bps, &f->sub[2]);
        analyse(f, s, n, bps + 1, &f->sub[3]);
        uint64_t lr = f->sub[0].bits + f->sub[1].bits, ls = f->sub[0].bits + f->sub[3].bits,
                 rs = f->sub[3].bits + f->sub[1].bits, ms = f->sub[2].bits + f->sub[3].bits;
        assign = 1, src[0] = l, src[1] = r, use[0] = &f->sub[0], use[1] = &f->sub[1];
        uint64_t best = lr;
        if (ls < best) best = ls, assign = 8, src[0] = l, src[1] = s, use[0] = &f->sub[0], use[1] = &f->sub[3];
        if (rs < best) best = rs, assign = 9, src[0] = s, src[1] = r, use[0] = &f->sub[3], use[1] = &f->sub[1];
        if (ms < best) best = ms, assign = 10, src[0] = m, src[1] = s, use[0] = &f->sub[2], use[1] = &f->sub[3];
    } else {
        for (int c = 0; c < nch; c++) {
            analyse(f, f->ch[c], n, bps, &f->sub[c]);
            src[c] = f->ch[c], use[c] = &f->sub[c];
        }
    }

    Bw w = {out, cap, 0, 0, 0};
    int bsx, rx;
    int bsc = blocksize_code(n, &bsx), rc = rate_code(f->c.rate, &rx);
    put(&w, 0x3FFE, 14); /* sync */
    put(&w, 0, 1);       /* reserved */
    put(&w, 0, 1);       /* fixed block size */
    put(&w, (uint32_t)bsc, 4);
    put(&w, (uint32_t)rc, 4);
    put(&w, (uint32_t)assign, 4);
    put(&w, (uint32_t)bits_code(bps), 3);
    put(&w, 0, 1);
    put_utf8(&w, f->frame_no);
    if (bsx) put(&w, (uint32_t)(n - 1), bsx);
    if (rx == 8) put(&w, (uint32_t)(f->c.rate / 1000), 8);
    else if (rx == 16) put(&w, (uint32_t)(rc == 14 ? f->c.rate / 10 : f->c.rate), 16);
    if (w.len > cap) return -1;
    put(&w, flac_crc8(out, w.len), 8);
    for (int c = 0; c < nch; c++) write_subframe(f, &w, src[c], n, use[c]);
    align(&w);
    if (w.len + 2 > cap) return -1;
    uint16_t crc = flac_crc16(out, w.len);
    put(&w, crc, 16);
    uint32_t size = (uint32_t)w.len;
    if (!f->min_frame || size < f->min_frame) f->min_frame = size;
    if (size > f->max_frame) f->max_frame = size;
    f->frame_no++;
    f->total += (uint64_t)n;
    return (long)size;
}

bool flac_write(Flac *f, const int32_t *pcm, int n, FlacSink sink, void *ctx) {
    int nch = f->c.channels;
    if (!f->fbuf && !(f->fbuf = malloc(flac_frame_bound(f)))) return false;
    while (n > 0) {
        int take = f->block - f->npend < n ? f->block - f->npend : n;
        memcpy(f->pend + (size_t)f->npend * (size_t)nch, pcm, (size_t)take * (size_t)nch * sizeof *pcm);
        f->npend += take, pcm += (size_t)take * (size_t)nch, n -= take;
        if (f->npend == f->block) {
            long sz = flac_encode_frame(f, f->pend, f->block, f->fbuf, flac_frame_bound(f));
            if (sz < 0) return false;
            sink(ctx, f->fbuf, (size_t)sz, f->block);
            f->npend = 0;
        }
    }
    return true;
}

bool flac_finish(Flac *f, FlacSink sink, void *ctx) {
    if (!f->npend) return true;
    if (!f->fbuf && !(f->fbuf = malloc(flac_frame_bound(f)))) return false;
    long sz = flac_encode_frame(f, f->pend, f->npend, f->fbuf, flac_frame_bound(f));
    if (sz < 0) return false;
    sink(ctx, f->fbuf, (size_t)sz, f->npend);
    f->npend = 0;
    return true;
}

void flac_streaminfo(const Flac *f, uint8_t o[FLAC_STREAMINFO_BYTES]) {
    uint32_t bs = (uint32_t)f->block;
    o[0] = (uint8_t)(bs >> 8), o[1] = (uint8_t)bs; /* min block size (the last frame doesn't count) */
    o[2] = (uint8_t)(bs >> 8), o[3] = (uint8_t)bs;
    o[4] = (uint8_t)(f->min_frame >> 16), o[5] = (uint8_t)(f->min_frame >> 8), o[6] = (uint8_t)f->min_frame;
    o[7] = (uint8_t)(f->max_frame >> 16), o[8] = (uint8_t)(f->max_frame >> 8), o[9] = (uint8_t)f->max_frame;
    uint32_t rate = (uint32_t)f->c.rate, ch = (uint32_t)(f->c.channels - 1), bps = (uint32_t)(f->c.bits - 1);
    uint64_t total = f->total & 0xFFFFFFFFFULL;
    o[10] = (uint8_t)(rate >> 12);
    o[11] = (uint8_t)(rate >> 4);
    o[12] = (uint8_t)((rate & 0xF) << 4 | ch << 1 | bps >> 4);
    o[13] = (uint8_t)((bps & 0xF) << 4 | (uint32_t)(total >> 32));
    o[14] = (uint8_t)(total >> 24), o[15] = (uint8_t)(total >> 16), o[16] = (uint8_t)(total >> 8), o[17] = (uint8_t)total;
    if (f->total) md5_final(f->md5, o + 18);
    else memset(o + 18, 0, 16);
}

void flac_header(const Flac *f, uint8_t out[FLAC_HEADER_BYTES]) {
    memcpy(out, "fLaC", 4);
    out[4] = 0x80; /* last metadata block, type 0 = STREAMINFO */
    out[5] = 0, out[6] = 0, out[7] = FLAC_STREAMINFO_BYTES;
    flac_streaminfo(f, out + 8);
}
