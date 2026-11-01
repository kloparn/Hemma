/* modules/av/hevc_dsp.c — HEVC sample work: the inverse transforms (8.6.4), intra prediction (8.4.4.2), motion
   compensation with weighted prediction (8.5.3.3), the deblocking filter (8.7.2) and SAO (8.7.3). See hevc.h. */
#include "hevc_int.h"

/* ---- inverse transforms ---- */

/* the 32-point DCT (8.6.4.2: transMatrix); an N-point transform uses every (32/N)-th row */
static const int8_t DCT[32][32] = {
    {64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64},
    {90, 90, 88, 85, 82, 78, 73, 67, 61, 54, 46, 38, 31, 22, 13, 4, -4, -13, -22, -31, -38, -46, -54, -61, -67, -73, -78, -82, -85, -88, -90, -90},
    {90, 87, 80, 70, 57, 43, 25, 9, -9, -25, -43, -57, -70, -80, -87, -90, -90, -87, -80, -70, -57, -43, -25, -9, 9, 25, 43, 57, 70, 80, 87, 90},
    {90, 82, 67, 46, 22, -4, -31, -54, -73, -85, -90, -88, -78, -61, -38, -13, 13, 38, 61, 78, 88, 90, 85, 73, 54, 31, 4, -22, -46, -67, -82, -90},
    {89, 75, 50, 18, -18, -50, -75, -89, -89, -75, -50, -18, 18, 50, 75, 89, 89, 75, 50, 18, -18, -50, -75, -89, -89, -75, -50, -18, 18, 50, 75, 89},
    {88, 67, 31, -13, -54, -82, -90, -78, -46, -4, 38, 73, 90, 85, 61, 22, -22, -61, -85, -90, -73, -38, 4, 46, 78, 90, 82, 54, 13, -31, -67, -88},
    {87, 57, 9, -43, -80, -90, -70, -25, 25, 70, 90, 80, 43, -9, -57, -87, -87, -57, -9, 43, 80, 90, 70, 25, -25, -70, -90, -80, -43, 9, 57, 87},
    {85, 46, -13, -67, -90, -73, -22, 38, 82, 88, 54, -4, -61, -90, -78, -31, 31, 78, 90, 61, 4, -54, -88, -82, -38, 22, 73, 90, 67, 13, -46, -85},
    {83, 36, -36, -83, -83, -36, 36, 83, 83, 36, -36, -83, -83, -36, 36, 83, 83, 36, -36, -83, -83, -36, 36, 83, 83, 36, -36, -83, -83, -36, 36, 83},
    {82, 22, -54, -90, -61, 13, 78, 85, 31, -46, -90, -67, 4, 73, 88, 38, -38, -88, -73, -4, 67, 90, 46, -31, -85, -78, -13, 61, 90, 54, -22, -82},
    {80, 9, -70, -87, -25, 57, 90, 43, -43, -90, -57, 25, 87, 70, -9, -80, -80, -9, 70, 87, 25, -57, -90, -43, 43, 90, 57, -25, -87, -70, 9, 80},
    {78, -4, -82, -73, 13, 85, 67, -22, -88, -61, 31, 90, 54, -38, -90, -46, 46, 90, 38, -54, -90, -31, 61, 88, 22, -67, -85, -13, 73, 82, 4, -78},
    {75, -18, -89, -50, 50, 89, 18, -75, -75, 18, 89, 50, -50, -89, -18, 75, 75, -18, -89, -50, 50, 89, 18, -75, -75, 18, 89, 50, -50, -89, -18, 75},
    {73, -31, -90, -22, 78, 67, -38, -90, -13, 82, 61, -46, -88, -4, 85, 54, -54, -85, 4, 88, 46, -61, -82, 13, 90, 38, -67, -78, 22, 90, 31, -73},
    {70, -43, -87, 9, 90, 25, -80, -57, 57, 80, -25, -90, -9, 87, 43, -70, -70, 43, 87, -9, -90, -25, 80, 57, -57, -80, 25, 90, 9, -87, -43, 70},
    {67, -54, -78, 38, 85, -22, -90, 4, 90, 13, -88, -31, 82, 46, -73, -61, 61, 73, -46, -82, 31, 88, -13, -90, -4, 90, 22, -85, -38, 78, 54, -67},
    {64, -64, -64, 64, 64, -64, -64, 64, 64, -64, -64, 64, 64, -64, -64, 64, 64, -64, -64, 64, 64, -64, -64, 64, 64, -64, -64, 64, 64, -64, -64, 64},
    {61, -73, -46, 82, 31, -88, -13, 90, -4, -90, 22, 85, -38, -78, 54, 67, -67, -54, 78, 38, -85, -22, 90, 4, -90, 13, 88, -31, -82, 46, 73, -61},
    {57, -80, -25, 90, -9, -87, 43, 70, -70, -43, 87, 9, -90, 25, 80, -57, -57, 80, 25, -90, 9, 87, -43, -70, 70, 43, -87, -9, 90, -25, -80, 57},
    {54, -85, -4, 88, -46, -61, 82, 13, -90, 38, 67, -78, -22, 90, -31, -73, 73, 31, -90, 22, 78, -67, -38, 90, -13, -82, 61, 46, -88, 4, 85, -54},
    {50, -89, 18, 75, -75, -18, 89, -50, -50, 89, -18, -75, 75, 18, -89, 50, 50, -89, 18, 75, -75, -18, 89, -50, -50, 89, -18, -75, 75, 18, -89, 50},
    {46, -90, 38, 54, -90, 31, 61, -88, 22, 67, -85, 13, 73, -82, 4, 78, -78, -4, 82, -73, -13, 85, -67, -22, 88, -61, -31, 90, -54, -38, 90, -46},
    {43, -90, 57, 25, -87, 70, 9, -80, 80, -9, -70, 87, -25, -57, 90, -43, -43, 90, -57, -25, 87, -70, -9, 80, -80, 9, 70, -87, 25, 57, -90, 43},
    {38, -88, 73, -4, -67, 90, -46, -31, 85, -78, 13, 61, -90, 54, 22, -82, 82, -22, -54, 90, -61, -13, 78, -85, 31, 46, -90, 67, 4, -73, 88, -38},
    {36, -83, 83, -36, -36, 83, -83, 36, 36, -83, 83, -36, -36, 83, -83, 36, 36, -83, 83, -36, -36, 83, -83, 36, 36, -83, 83, -36, -36, 83, -83, 36},
    {31, -78, 90, -61, 4, 54, -88, 82, -38, -22, 73, -90, 67, -13, -46, 85, -85, 46, 13, -67, 90, -73, 22, 38, -82, 88, -54, -4, 61, -90, 78, -31},
    {25, -70, 90, -80, 43, 9, -57, 87, -87, 57, -9, -43, 80, -90, 70, -25, -25, 70, -90, 80, -43, -9, 57, -87, 87, -57, 9, 43, -80, 90, -70, 25},
    {22, -61, 85, -90, 73, -38, -4, 46, -78, 90, -82, 54, -13, -31, 67, -88, 88, -67, 31, 13, -54, 82, -90, 78, -46, 4, 38, -73, 90, -85, 61, -22},
    {18, -50, 75, -89, 89, -75, 50, -18, -18, 50, -75, 89, -89, 75, -50, 18, 18, -50, 75, -89, 89, -75, 50, -18, -18, 50, -75, 89, -89, 75, -50, 18},
    {13, -38, 61, -78, 88, -90, 85, -73, 54, -31, 4, 22, -46, 67, -82, 90, -90, 82, -67, 46, -22, -4, 31, -54, 73, -85, 90, -88, 78, -61, 38, -13},
    {9, -25, 43, -57, 70, -80, 87, -90, 90, -87, 80, -70, 57, -43, 25, -9, -9, 25, -43, 57, -70, 80, -87, 90, -90, 87, -80, 70, -57, 43, -25, 9},
    {4, -13, 22, -31, 38, -46, 54, -61, 67, -73, 78, -82, 85, -88, 90, -90, 90, -90, 88, -85, 82, -78, 73, -67, 61, -54, 46, -38, 31, -22, 13, -4}};

static const int DST4[4][4] = {{29, 55, 74, 84}, {74, 74, 0, -74}, {84, -29, -74, 55}, {55, -84, 74, -29}};

/* one 1-D inverse transform of n points: out[i] = sum over k < lim of T[k][i] * in[k] (even and odd halves) */
static void idct1(int n, const int32_t *in, int lim, int64_t *out) {
    int h = n >> 1, step = 32 / n;
    for (int i = 0; i < h; i++) {
        int64_t e = 0, o = 0;
        for (int k = 0; k < lim; k += 2) e += (int64_t)DCT[k * step][i] * in[k];
        for (int k = 1; k < lim; k += 2) o += (int64_t)DCT[k * step][i] * in[k];
        out[i] = e + o, out[n - 1 - i] = e - o;
    }
}

void hv_transform(int32_t *res, const int16_t *coef, int log2, bool dst, int depth) {
    int n = 1 << log2;
    /* the last row and column holding a coefficient: the rest adds nothing */
    int maxr = 0, maxc = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (coef[y * n + x]) {
                if (y > maxr) maxr = y;
                if (x > maxc) maxc = x;
            }
    int32_t mid[32 * 32], in[32];
    int64_t out[32];
    for (int x = 0; x <= maxc; x++) { /* columns first (vertical), clipped to 16 bits */
        for (int k = 0; k <= maxr; k++) in[k] = coef[k * n + x];
        if (dst) {
            for (int i = 0; i < 4; i++) {
                int64_t s = 0;
                for (int k = 0; k <= maxr; k++) s += (int64_t)DST4[k][i] * in[k];
                out[i] = s;
            }
        } else {
            idct1(n, in, maxr + 1, out);
        }
        for (int i = 0; i < n; i++) mid[i * n + x] = (int32_t)CLIP3(-32768, 32767, (out[i] + 64) >> 7);
    }
    int sh = 20 - depth;
    int64_t rnd = (int64_t)1 << (sh - 1);
    for (int y = 0; y < n; y++) {
        if (dst) {
            for (int i = 0; i < 4; i++) {
                int64_t s = 0;
                for (int k = 0; k <= maxc; k++) s += (int64_t)DST4[k][i] * mid[y * n + k];
                out[i] = s;
            }
        } else {
            idct1(n, mid + y * n, maxc + 1, out);
        }
        for (int i = 0; i < n; i++) res[y * n + i] = (int32_t)((out[i] + rnd) >> sh);
    }
}

void hv_transform_dc(int32_t *res, int dc, int log2, int depth) {
    int g = CLIP3(-32768, 32767, (64 * dc + 64) >> 7), sh = 20 - depth;
    int v = (int)(((int64_t)64 * g + ((int64_t)1 << (sh - 1))) >> sh);
    for (int i = 0; i < (1 << (2 * log2)); i++) res[i] = v;
}

/* ---- intra prediction ---- */

static const int8_t ANGLE[33] = {32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26, -32,
                                 -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32};
static const int16_t INV_ANGLE[15] = {-4096, -1638, -910, -630, -482, -390, -315, -256,
                                      -315, -390, -482, -630, -910, -1638, -4096};

static bool intra_avail(Hevc *d, int xl, int yl, int xn, int yn) {
    if (!hv_avail(d, xl, yl, xn, yn)) return false;
    if (d->p->constrained_intra && d->cur->mv[(size_t)(yn >> 2) * d->w4 + (xn >> 2)].pred) return false;
    return true;
}

void hv_intra(Hevc *d, int x0, int y0, int log2, int c, int mode) {
    HvPic *pic = d->cur;
    int n = 1 << log2, st = pic->stride[c], sc = c ? 1 : 0, u = c ? 2 : 4;
    int depth = d->s->depth, maxv = (1 << depth) - 1;
    px *dst = pic->plane[c] + (size_t)y0 * st + x0;
    int xl = x0 << sc, yl = y0 << sc;
    /* the 4n+1 reference samples, from p[-1][2n-1] up the left side to the corner, then along the top */
    int ref[4 * 64 + 1];
    bool av[4 * 64 + 1];
    int nr = 4 * n + 1, found = -1;
    for (int y = 0; y < 2 * n; y += u) {
        bool a = intra_avail(d, xl, yl, (x0 - 1) * (1 << sc), (y0 + y) << sc);
        for (int k = 0; k < u && y + k < 2 * n; k++) {
            int i = 2 * n - 1 - (y + k);
            av[i] = a;
            if (a) ref[i] = dst[(ptrdiff_t)(y + k) * st - 1];
        }
    }
    av[2 * n] = intra_avail(d, xl, yl, (x0 - 1) * (1 << sc), (y0 - 1) * (1 << sc));
    if (av[2 * n]) ref[2 * n] = dst[-st - 1];
    for (int x = 0; x < 2 * n; x += u) {
        bool a = intra_avail(d, xl, yl, (x0 + x) << sc, (y0 - 1) * (1 << sc));
        for (int k = 0; k < u && x + k < 2 * n; k++) {
            int i = 2 * n + 1 + x + k;
            av[i] = a;
            if (a) ref[i] = dst[-st + x + k];
        }
    }
    for (int i = 0; i < nr && found < 0; i++)
        if (av[i]) found = i;
    if (found < 0) {
        for (int i = 0; i < nr; i++) ref[i] = 1 << (depth - 1);
    } else {
        if (!av[0]) ref[0] = ref[found];
        for (int i = 1; i < nr; i++)
            if (!av[i]) ref[i] = ref[i - 1];
    }
    /* smoothing (luma only for 4:2:0) */
    if (!c && mode != 1 && n != 4) {
        int dist = abs(mode - 26) < abs(mode - 10) ? abs(mode - 26) : abs(mode - 10);
        int thres = n == 8 ? 7 : n == 16 ? 1 : 0;
        if (dist > thres) {
            int f[4 * 64 + 1];
            int corner = ref[2 * n], bl = ref[0], tr = ref[4 * n];
            if (d->s->strong && n == 32 && abs(corner + tr - 2 * ref[2 * n + n]) < (1 << (depth - 5)) &&
                abs(corner + bl - 2 * ref[2 * n - n]) < (1 << (depth - 5))) {
                f[2 * n] = corner, f[0] = bl, f[4 * n] = tr;
                for (int y = 0; y < 63; y++) f[2 * n - 1 - y] = ((63 - y) * corner + (y + 1) * bl + 32) >> 6;
                for (int x = 0; x < 63; x++) f[2 * n + 1 + x] = ((63 - x) * corner + (x + 1) * tr + 32) >> 6;
            } else {
                f[0] = ref[0], f[nr - 1] = ref[nr - 1];
                for (int i = 1; i < nr - 1; i++) f[i] = (ref[i - 1] + 2 * ref[i] + ref[i + 1] + 2) >> 2;
            }
            memcpy(ref, f, sizeof(int) * (size_t)nr);
        }
    }
#define LEFT(y) ref[2 * n - 1 - (y)] /* p[-1][y], y = -1 .. 2n-1 */
#define TOP(x) ref[2 * n + 1 + (x)]  /* p[x][-1], x = -1 .. 2n-1 */
    if (mode == 0) {
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                dst[(size_t)y * st + x] = (px)(((n - 1 - x) * LEFT(y) + (x + 1) * TOP(n) + (n - 1 - y) * TOP(x) +
                                                (y + 1) * LEFT(n) + n) >> (log2 + 1));
    } else if (mode == 1) {
        int sum = n;
        for (int i = 0; i < n; i++) sum += TOP(i) + LEFT(i);
        int dc = sum >> (log2 + 1);
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) dst[(size_t)y * st + x] = (px)dc;
        if (!c && n < 32) {
            dst[0] = (px)((LEFT(0) + 2 * dc + TOP(0) + 2) >> 2);
            for (int x = 1; x < n; x++) dst[x] = (px)((TOP(x) + 3 * dc + 2) >> 2);
            for (int y = 1; y < n; y++) dst[(size_t)y * st] = (px)((LEFT(y) + 3 * dc + 2) >> 2);
        }
    } else {
        int angle = ANGLE[mode - 2];
        int buf[3 * 64 + 2], *rm = buf + 64 + 1;
        bool vert = mode >= 18;
        for (int x = 0; x <= n; x++) rm[x] = vert ? TOP(x - 1) : LEFT(x - 1);
        if (angle < 0) {
            int last = (n * angle) >> 5;
            if (last < -1) {
                int inv = INV_ANGLE[mode - 11];
                for (int x = last; x <= -1; x++) {
                    int k = -1 + ((x * inv + 128) >> 8);
                    rm[x] = vert ? LEFT(k) : TOP(k);
                }
            }
        } else {
            for (int x = n + 1; x <= 2 * n; x++) rm[x] = vert ? TOP(x - 1) : LEFT(x - 1);
        }
        for (int j = 0; j < n; j++) {
            int idx = ((j + 1) * angle) >> 5, fr = ((j + 1) * angle) & 31;
            for (int i = 0; i < n; i++) {
                int v = fr ? ((32 - fr) * rm[i + idx + 1] + fr * rm[i + idx + 2] + 16) >> 5 : rm[i + idx + 1];
                if (vert) dst[(size_t)j * st + i] = (px)v;  /* j = y, i = x */
                else dst[(size_t)i * st + j] = (px)v;       /* j = x, i = y */
            }
        }
        if (!c && n < 32) {
            if (mode == 26)
                for (int y = 0; y < n; y++) dst[(size_t)y * st] = (px)CLIP3(0, maxv, TOP(0) + ((LEFT(y) - LEFT(-1)) >> 1));
            if (mode == 10)
                for (int x = 0; x < n; x++) dst[x] = (px)CLIP3(0, maxv, LEFT(0) + ((TOP(x) - TOP(-1)) >> 1));
        }
    }
#undef LEFT
#undef TOP
}

/* ---- motion compensation (8.5.3.3.3) ---- */

static const int8_t QPEL[4][8] = {{0, 0, 0, 64, 0, 0, 0, 0},
                                  {-1, 4, -10, 58, 17, -5, 1, 0},
                                  {-1, 4, -11, 40, 40, -11, 4, -1},
                                  {0, 1, -5, 17, 58, -10, 4, -1}};
static const int8_t EPEL[8][4] = {{0, 64, 0, 0},     {-2, 58, 10, -2}, {-4, 54, 16, -2}, {-6, 46, 28, -4},
                                  {-4, 36, 36, -4}, {-4, 28, 46, -6}, {-2, 16, 54, -4}, {-2, 10, 58, -2}};

/* a block's reference samples with `before` and `after` taps around it: the picture itself where its padding
   reaches, else a copy with the coordinates clamped (the same samples the padding would repeat) */
static const px *window(const HvPic *ref, int c, int x, int y, int w, int h, int before, int after, px *buf, int *stride) {
    int pad = c ? HV_PAD / 2 : HV_PAD, pw = c ? ref->cw : ref->w, ph = c ? ref->ch : ref->h, st = ref->stride[c];
    if (x - before >= -pad && y - before >= -pad && x + w + after <= pw + pad && y + h + after <= ph + pad) {
        *stride = st;
        return ref->plane[c] + (ptrdiff_t)y * st + x;
    }
    int bw = w + before + after, bh = h + before + after;
    for (int j = 0; j < bh; j++) {
        int yy = CLIP3(0, ph - 1, y - before + j);
        for (int i = 0; i < bw; i++) buf[j * bw + i] = ref->plane[c][(size_t)yy * st + CLIP3(0, pw - 1, x - before + i)];
    }
    *stride = bw;
    return buf + before * bw + before;
}

/* the prediction at 14-bit precision into out (stride w) */
static void mc(const HvPic *ref, int c, int x, int y, int w, int h, int mvx, int mvy, int depth, int16_t *out) {
    int taps = c ? 4 : 8, before = c ? 1 : 3, after = c ? 2 : 4;
    int fx = c ? mvx & 7 : mvx & 3, fy = c ? mvy & 7 : mvy & 3;
    int xi = x + (c ? mvx >> 3 : mvx >> 2), yi = y + (c ? mvy >> 3 : mvy >> 2);
    px buf[(64 + 7) * (64 + 7)];
    int st;
    const px *s = window(ref, c, xi, yi, w, h, before, after, buf, &st);
    int sh1 = depth - 8, sh3 = 14 - depth;
    const int8_t *hf = c ? EPEL[fx] : QPEL[fx], *vf = c ? EPEL[fy] : QPEL[fy];
    if (!fx && !fy) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) out[j * w + i] = (int16_t)(s[(ptrdiff_t)j * st + i] << sh3);
    } else if (!fy) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                const px *p = s + (ptrdiff_t)j * st + i - before;
                int v = 0;
                for (int k = 0; k < taps; k++) v += hf[k] * p[k];
                out[j * w + i] = (int16_t)(v >> sh1);
            }
    } else if (!fx) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                const px *p = s + (ptrdiff_t)(j - before) * st + i;
                int v = 0;
                for (int k = 0; k < taps; k++) v += vf[k] * p[(ptrdiff_t)k * st];
                out[j * w + i] = (int16_t)(v >> sh1);
            }
    } else {
        int16_t tmp[(64 + 7) * 64];
        int th = h + taps - 1;
        for (int j = 0; j < th; j++)
            for (int i = 0; i < w; i++) {
                const px *p = s + (ptrdiff_t)(j - before) * st + i - before;
                int v = 0;
                for (int k = 0; k < taps; k++) v += hf[k] * p[k];
                tmp[j * w + i] = (int16_t)(v >> sh1);
            }
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                int v = 0;
                for (int k = 0; k < taps; k++) v += vf[k] * tmp[(j + k) * w + i];
                out[j * w + i] = (int16_t)(v >> 6);
            }
    }
}

void hv_inter(Hevc *d, int x, int y, int w, int h, const HvMv *mv) {
    const HvSlice *sh = &d->sh;
    int depth = d->s->depth, maxv = (1 << depth) - 1;
    bool weighted = (sh->type == SLICE_P && d->p->weighted_p) || (sh->type == SLICE_B && d->p->weighted_b);
    int16_t pred[2][64 * 64];
    for (int c = 0; c < 3; c++) {
        int cx = c ? x >> 1 : x, cy = c ? y >> 1 : y, cw = c ? w >> 1 : w, ch = c ? h >> 1 : h;
        int nl = 0, lists[2];
        for (int l = 0; l < 2; l++)
            if (mv->pred & (1 << l)) {
                mc(d->refpic[l][mv->ref[l]], c, cx, cy, cw, ch, mv->mv[l][0], mv->mv[l][1], depth, pred[nl]);
                lists[nl++] = l;
            }
        HvPic *pic = d->cur;
        px *dst = pic->plane[c] + (size_t)cy * pic->stride[c] + cx;
        int st = pic->stride[c], sh1 = 14 - depth;
        if (!weighted) {
            if (nl == 1) {
                int rnd = 1 << (sh1 - 1);
                for (int j = 0; j < ch; j++)
                    for (int i = 0; i < cw; i++) dst[(size_t)j * st + i] = (px)CLIP3(0, maxv, (pred[0][j * cw + i] + rnd) >> sh1);
            } else {
                int sh2 = sh1 + 1, rnd = 1 << (sh2 - 1);
                for (int j = 0; j < ch; j++)
                    for (int i = 0; i < cw; i++)
                        dst[(size_t)j * st + i] = (px)CLIP3(0, maxv, (pred[0][j * cw + i] + pred[1][j * cw + i] + rnd) >> sh2);
            }
            continue;
        }
        int denom = c ? sh->cw_denom : sh->lw_denom, log2wd = denom + sh1, ws[2], os[2];
        for (int k = 0; k < nl; k++) {
            int l = lists[k], r = mv->ref[l];
            ws[k] = c ? sh->cw[l][r][c - 1] : sh->lw[l][r];
            os[k] = (c ? sh->co[l][r][c - 1] : sh->lo[l][r]) * (1 << (depth - 8));
        }
        if (nl == 1) {
            for (int j = 0; j < ch; j++)
                for (int i = 0; i < cw; i++) {
                    int v = pred[0][j * cw + i] * ws[0];
                    v = log2wd >= 1 ? ((v + (1 << (log2wd - 1))) >> log2wd) + os[0] : v + os[0];
                    dst[(size_t)j * st + i] = (px)CLIP3(0, maxv, v);
                }
        } else {
            for (int j = 0; j < ch; j++)
                for (int i = 0; i < cw; i++) {
                    int v = (pred[0][j * cw + i] * ws[0] + pred[1][j * cw + i] * ws[1] + (os[0] + os[1] + 1) * (1 << log2wd)) >> (log2wd + 1);
                    dst[(size_t)j * st + i] = (px)CLIP3(0, maxv, v);
                }
        }
    }
}

/* ---- deblocking (8.7.2) ---- */

static const uint8_t BETA[52] = {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  6,  7,
                                 8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24, 26, 28, 30, 32,
                                 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64};
static const uint8_t TC[54] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,  1,  1,  1,  1,  1,  1,  1,  1,
                               2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 22, 24};

static const HvSliceInfo *info_at(const Hevc *d, int x, int y) {
    int i = d->ctb_info[(y >> d->s->log2_ctb) * d->wctb + (x >> d->s->log2_ctb)];
    return &d->info[i >= 0 && i < d->ninfo ? i : 0];
}

/* the luma edge segment of 4 lines at q0 = s: xs = the step across the edge, ls = the step along it */
static void luma_edge(px *s, ptrdiff_t xs, ptrdiff_t ls, int bs, int qp, const HvSliceInfo *in, int depth, bool nop, bool noq) {
    int beta = BETA[CLIP3(0, 51, qp + in->beta)] * (1 << (depth - 8));
    int tc = TC[CLIP3(0, 53, qp + 2 * (bs - 1) + in->tc)] * (1 << (depth - 8));
    if (!tc && !beta) return;
#define P(i, k) s[(k) * ls - ((i) + 1) * xs]
#define Q(i, k) s[(k) * ls + (i) * xs]
    int dp0 = abs(P(2, 0) - 2 * P(1, 0) + P(0, 0)), dp3 = abs(P(2, 3) - 2 * P(1, 3) + P(0, 3));
    int dq0 = abs(Q(2, 0) - 2 * Q(1, 0) + Q(0, 0)), dq3 = abs(Q(2, 3) - 2 * Q(1, 3) + Q(0, 3));
    int dpq0 = dp0 + dq0, dpq3 = dp3 + dq3, dp = dp0 + dp3, dq = dq0 + dq3;
    if (dpq0 + dpq3 >= beta) return;
    int maxv = (1 << depth) - 1;
    bool strong0 = 2 * dpq0 < (beta >> 2) && abs(P(3, 0) - P(0, 0)) + abs(Q(0, 0) - Q(3, 0)) < (beta >> 3) &&
                   abs(P(0, 0) - Q(0, 0)) < ((5 * tc + 1) >> 1);
    bool strong3 = 2 * dpq3 < (beta >> 2) && abs(P(3, 3) - P(0, 3)) + abs(Q(0, 3) - Q(3, 3)) < (beta >> 3) &&
                   abs(P(0, 3) - Q(0, 3)) < ((5 * tc + 1) >> 1);
    bool dep = dp < ((beta + (beta >> 1)) >> 3), deq = dq < ((beta + (beta >> 1)) >> 3);
    for (int k = 0; k < 4; k++) {
        int p0 = P(0, k), p1 = P(1, k), p2 = P(2, k), p3 = P(3, k), q0 = Q(0, k), q1 = Q(1, k), q2 = Q(2, k), q3 = Q(3, k);
        if (strong0 && strong3) {
            int t2 = 2 * tc;
            if (!nop) {
                P(0, k) = (px)CLIP3(p0 - t2, p0 + t2, (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                P(1, k) = (px)CLIP3(p1 - t2, p1 + t2, (p2 + p1 + p0 + q0 + 2) >> 2);
                P(2, k) = (px)CLIP3(p2 - t2, p2 + t2, (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            }
            if (!noq) {
                Q(0, k) = (px)CLIP3(q0 - t2, q0 + t2, (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                Q(1, k) = (px)CLIP3(q1 - t2, q1 + t2, (p0 + q0 + q1 + q2 + 2) >> 2);
                Q(2, k) = (px)CLIP3(q2 - t2, q2 + t2, (p0 + q0 + q1 + 3 * q2 + 2 * q3 + 4) >> 3);
            }
        } else {
            int delta = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
            if (abs(delta) >= tc * 10) continue;
            delta = CLIP3(-tc, tc, delta);
            if (!nop) {
                P(0, k) = (px)CLIP3(0, maxv, p0 + delta);
                if (dep) P(1, k) = (px)CLIP3(0, maxv, p1 + CLIP3(-(tc >> 1), tc >> 1, (((p2 + p0 + 1) >> 1) - p1 + delta) >> 1));
            }
            if (!noq) {
                Q(0, k) = (px)CLIP3(0, maxv, q0 - delta);
                if (deq) Q(1, k) = (px)CLIP3(0, maxv, q1 + CLIP3(-(tc >> 1), tc >> 1, (((q2 + q0 + 1) >> 1) - q1 - delta) >> 1));
            }
        }
    }
}

static void chroma_edge(px *s, ptrdiff_t xs, ptrdiff_t ls, int qp, int off, const HvSliceInfo *in, int depth, bool nop, bool noq) {
    static const uint8_t QPC[14] = {29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37};
    int qpi = qp + off, qpc = qpi < 30 ? qpi : qpi > 43 ? qpi - 6 : QPC[qpi - 30];
    int tc = TC[CLIP3(0, 53, qpc + 2 + in->tc)] * (1 << (depth - 8));
    if (!tc) return;
    int maxv = (1 << depth) - 1;
    for (int k = 0; k < 4; k++) {
        int p0 = P(0, k), p1 = P(1, k), q0 = Q(0, k), q1 = Q(1, k);
        int delta = CLIP3(-tc, tc, ((((q0 - p0) * 4) + p1 - q1 + 4) >> 3));
        if (!nop) P(0, k) = (px)CLIP3(0, maxv, p0 + delta);
        if (!noq) Q(0, k) = (px)CLIP3(0, maxv, q0 - delta);
    }
#undef P
#undef Q
}

static void deblock_dir(Hevc *d, bool vertical) {
    const HvSps *s = d->s;
    HvPic *pic = d->cur;
    int bw = (s->width + 7) >> 3, depth = s->depth;
    for (int y = 0; y < s->height; y += 4)
        for (int x = 0; x < s->width; x += 4) {
            int bs;
            if (vertical) {
                if (x & 7) continue;
                bs = d->bs_v[(y >> 2) * bw + (x >> 3)];
            } else {
                if (y & 7) continue;
                bs = d->bs_h[(y >> 3) * d->w4 + (x >> 2)];
            }
            if (!bs) continue;
            int xp = vertical ? x - 1 : x, yp = vertical ? y : y - 1;
            size_t ap = (size_t)(yp >> 2) * d->w4 + (xp >> 2), aq = (size_t)(y >> 2) * d->w4 + (x >> 2);
            bool nop = d->flags4[ap] & F4_NOFILTER, noq = d->flags4[aq] & F4_NOFILTER;
            int qp = (d->qp4[ap] + d->qp4[aq] + 1) >> 1;
            const HvSliceInfo *in = info_at(d, x, y);
            ptrdiff_t st = pic->stride[0];
            luma_edge(pic->plane[0] + (ptrdiff_t)y * st + x, vertical ? 1 : st, vertical ? st : 1, bs, qp, in, depth, nop, noq);
            /* chroma: edges on the chroma 8x8 grid, 4 chroma lines = this luma segment and the next */
            if (bs == 2 && ((vertical ? x : y) & 15) == 0 && ((vertical ? y : x) & 7) == 0)
                for (int c = 1; c < 3; c++) {
                    ptrdiff_t cs = pic->stride[c];
                    px *p = pic->plane[c] + (ptrdiff_t)(y >> 1) * cs + (x >> 1);
                    chroma_edge(p, vertical ? 1 : cs, vertical ? cs : 1, qp, c == 1 ? d->p->cb_off : d->p->cr_off, in, depth, nop, noq);
                }
        }
}

void hv_deblock(Hevc *d) {
    deblock_dir(d, true);
    deblock_dir(d, false);
}

/* ---- SAO (8.7.3) ---- */

static const int8_t EO[4][2][2] = {{{-1, 0}, {1, 0}}, {{0, -1}, {0, 1}}, {{-1, -1}, {1, 1}}, {{1, -1}, {-1, 1}}};

/* may SAO at a sample of CTB `rs` look at a sample of CTB `rn` (both decoded)? */
static bool sao_ok(const Hevc *d, int rs, int rn) {
    if (rs == rn) return true;
    if (d->ctb_addr[rn] < 0) return false;
    if (!d->p->lf_tiles && d->tile_id[d->rs2ts[rs]] != d->tile_id[d->rs2ts[rn]]) return false;
    if (d->ctb_addr[rs] != d->ctb_addr[rn]) {
        const HvSliceInfo *later = d->rs2ts[rn] < d->rs2ts[rs] ? &d->info[d->ctb_info[rs]] : &d->info[d->ctb_info[rn]];
        if (!later->lf_slices) return false;
    }
    return true;
}

void hv_sao(Hevc *d) {
    const HvSps *s = d->s;
    if (!s->sao) return;
    HvPic *pic = d->cur;
    int depth = s->depth, maxv = (1 << depth) - 1;
    bool any = false;
    for (int i = 0; i < d->nctb && !any; i++) any = d->ctb_addr[i] >= 0 && (d->sao[i].type[0] || d->sao[i].type[1] || d->sao[i].type[2]);
    if (!any) return;
    for (int c = 0; c < 3; c++) {
        int pw = c ? pic->cw : pic->w, ph = c ? pic->ch : pic->h, st = pic->stride[c], sc = c ? 1 : 0;
        px *plane = pic->plane[c], *t = d->tmp;
        for (int y = 0; y < ph; y++) memcpy(t + (size_t)y * pw, plane + (size_t)y * st, (size_t)pw * sizeof(px));
        int cs = d->ctb >> sc;
        for (int rs = 0; rs < d->nctb; rs++) {
            const HvSao *o = &d->sao[rs];
            if (d->ctb_addr[rs] < 0 || !o->type[c]) continue;
            int cx = rs % d->wctb, cy = rs / d->wctb, x0 = cx * cs, y0 = cy * cs;
            int x1 = x0 + cs < pw ? x0 + cs : pw, y1 = y0 + cs < ph ? y0 + cs : ph;
            if (o->type[c] == 1) {
                int table[32] = {0}, sh = depth - 5;
                for (int k = 0; k < 4; k++) table[(k + o->band[c]) & 31] = o->off[c][k + 1];
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) {
                        if (d->flags4[(size_t)((y << sc) >> 2) * d->w4 + ((x << sc) >> 2)] & F4_NOFILTER) continue;
                        int v = t[(size_t)y * pw + x];
                        plane[(size_t)y * st + x] = (px)CLIP3(0, maxv, v + table[v >> sh]);
                    }
                continue;
            }
            const int8_t (*e)[2] = EO[o->eo[c]];
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    int ax = x + e[0][0], ay = y + e[0][1], bx = x + e[1][0], by = y + e[1][1];
                    if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= pw || bx >= pw || ay >= ph || by >= ph) continue;
                    if (d->flags4[(size_t)((y << sc) >> 2) * d->w4 + ((x << sc) >> 2)] & F4_NOFILTER) continue;
                    if (x == x0 || y == y0 || x == x1 - 1 || y == y1 - 1) { /* a neighbour in another CTB? */
                        int ra = ((ay << sc) >> s->log2_ctb) * d->wctb + ((ax << sc) >> s->log2_ctb);
                        int rb = ((by << sc) >> s->log2_ctb) * d->wctb + ((bx << sc) >> s->log2_ctb);
                        if (!sao_ok(d, rs, ra) || !sao_ok(d, rs, rb)) continue;
                    }
                    int v = t[(size_t)y * pw + x], a = t[(size_t)ay * pw + ax], b = t[(size_t)by * pw + bx];
                    int edge = 2 + (v > a) - (v < a) + (v > b) - (v < b);
                    if (edge < 3) edge = edge == 2 ? 0 : edge + 1;
                    plane[(size_t)y * st + x] = (px)CLIP3(0, maxv, v + o->off[c][edge]);
                }
        }
    }
}
