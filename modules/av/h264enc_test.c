/* modules/av/h264enc_test.c — the H.264 encoder against the standard.
 *
 * Code words: every CAVLC table is prefix-free, a textbook block encodes to
 * its published bits, and random blocks (escapes, all nC classes, chroma DC)
 * come back out of a reader written here from clause 9.2. The inverse
 * transform and the deblocking filter are checked on hand-worked values,
 * motion vector prediction in every neighbour situation of 8.4.1. Whole
 * streams: parameter sets read back field by field, IDR spacing, odd sizes,
 * length-prefixed packets and avcC, determinism, the bitrate cap.
 *
 * The decisive check — a real decoder showing exactly the encoder's
 * reconstruction — runs outside the suite: tools/h264check writes a stream
 * plus its reconstruction, and a browser decodes it with WebCodecs (Chrome's
 * software and hardware decoders matched bit for bit). With HEMMA_TEST_MKV
 * pointing at a DVD rip, real MPEG-2 frames are re-encoded here too. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "h264enc.h"
#include "mkv.h"
#include "mpeg2.h"

#include <math.h>

size_t h264enc_cavlc_for_test(const int16_t *coef, int n, int nc, uint8_t *out, size_t cap);
uint32_t h264enc_code_for_test(int kind, int i, int j, int *len);
void h264enc_mvpred_for_test(const int (*st)[3], int mbw, int mbh, int mbx, int mby, int shape, const int first[2],
                             int out[2]);
void h264enc_filter_for_test(uint8_t line[8], int bs, int qp, bool chroma);
void h264enc_idct_for_test(const int32_t d[16], uint8_t out[16]);

/* ------------------------------------------------------------ bit reader */

typedef struct {
    const uint8_t *p;
    size_t n, pos; /* pos in bits */
} Rd;

static int rd_bit(Rd *r) {
    int b = r->pos < r->n * 8 ? (r->p[r->pos >> 3] >> (7 - (r->pos & 7))) & 1 : 0;
    r->pos++;
    return b;
}
static uint32_t rd_bits(Rd *r, int n) {
    uint32_t v = 0;
    while (n-- > 0) v = v << 1 | (uint32_t)rd_bit(r);
    return v;
}
static uint32_t rd_ue(Rd *r) {
    int z = 0;
    while (!rd_bit(r) && z < 32) z++;
    return ((1u << z) - 1) + rd_bits(r, z);
}

/* NAL payload without the emulation prevention bytes (7.4.1.1). */
static size_t unescape(const uint8_t *nal, size_t n, uint8_t *out) {
    size_t w = 0;
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        out[w++] = nal[i];
        zeros = nal[i] ? 0 : zeros + 1;
    }
    return w;
}

/* ---------------------------------------------------------------- CAVLC */

/* One code word of a table, found by reading bit by bit. */
static bool rd_code(Rd *r, int kind, int imin, int imax, int jmax, int *oi, int *oj) {
    uint32_t v = 0;
    for (int len = 1; len <= 16; len++) {
        v = v << 1 | (uint32_t)rd_bit(r);
        for (int i = imin; i <= imax; i++)
            for (int j = 0; j <= jmax; j++) {
                int l;
                uint32_t c = h264enc_code_for_test(kind, i, j, &l);
                if (l == len && c == v) {
                    *oi = i;
                    *oj = j;
                    return true;
                }
            }
    }
    return false;
}

/* residual_block_cavlc (7.3.5.3.2 and 9.2), written from the standard. */
static bool read_block(Rd *r, int n, int nc, int16_t *out) {
    int tc, t1;
    int kind = nc == -1 ? 4 : nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3;
    if (!rd_code(r, kind, 0, n, 3, &tc, &t1)) return false;
    memset(out, 0, sizeof *out * (size_t)n);
    if (!tc) return true;
    int level[16], run[16];
    for (int i = 0; i < t1; i++) level[i] = rd_bit(r) ? -1 : 1;
    int sl = tc > 10 && t1 < 3 ? 1 : 0;
    for (int i = t1; i < tc; i++) {
        int prefix = 0;
        while (!rd_bit(r))
            if (++prefix > 32) return false;
        int code = (prefix < 15 ? prefix : 15) << sl;
        int ssize = prefix == 14 && sl == 0 ? 4 : prefix >= 15 ? prefix - 3 : sl;
        if (ssize) code += (int)rd_bits(r, ssize);
        if (prefix >= 15 && sl == 0) code += 15;
        if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
        if (i == t1 && t1 < 3) code += 2;
        level[i] = code % 2 == 0 ? (code + 2) >> 1 : (-code - 1) >> 1;
        if (!sl) sl = 1;
        if (abs(level[i]) > (3 << (sl - 1)) && sl < 6) sl++;
    }
    int tz = 0, j;
    if (tc < n && !rd_code(r, n == 4 ? 6 : 5, tc, tc, 16 - tc, &j, &tz)) return false;
    int left = tz;
    for (int i = 0; i < tc - 1; i++) {
        run[i] = 0;
        if (left > 0) {
            int zl;
            if (!rd_code(r, 7, left < 7 ? left : 7, left < 7 ? left : 7, 14, &zl, &run[i])) return false;
            left -= run[i];
        }
    }
    run[tc - 1] = left;
    int cn = -1;
    for (int i = tc - 1; i >= 0; i--) {
        cn += run[i] + 1;
        if (cn >= n) return false;
        out[cn] = (int16_t)level[i];
    }
    return true;
}

TEST(h264enc_cavlc_tables_are_prefix_free) {
    static const struct {
        int kind, imin, imax, jmax;
    } groups[] = {{0, 0, 16, 3}, {1, 0, 16, 3}, {2, 0, 16, 3}, {3, 0, 16, 3}, {4, 0, 4, 3}};
    for (size_t g = 0; g < countof(groups); g++) {
        uint32_t code[80];
        int len[80], n = 0;
        double kraft = 0;
        for (int i = groups[g].imin; i <= groups[g].imax; i++)
            for (int j = 0; j <= groups[g].jmax; j++) {
                int l;
                uint32_t c = h264enc_code_for_test(groups[g].kind, i, j, &l);
                if (!l) continue;
                code[n] = c;
                len[n++] = l;
                kraft += 1.0 / (double)(1u << l);
            }
        CHECK(kraft <= 1.0 + 1e-12);
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++)
                if (a != b && len[a] <= len[b] && code[b] >> (len[b] - len[a]) == code[a])
                    test_fail(__FILE__, __LINE__, "coeff_token kind %d: a code is a prefix of another", groups[g].kind);
    }
    for (int kind = 5; kind <= 7; kind++) /* one table per TotalCoeff / zerosLeft */
        for (int i = 1; i <= (kind == 5 ? 15 : kind == 6 ? 3 : 7); i++) {
            int n = 0, len[17];
            uint32_t code[17];
            double kraft = 0;
            for (int j = 0; j <= 16; j++) {
                int l;
                uint32_t c = h264enc_code_for_test(kind, i, j, &l);
                if (!l) continue;
                code[n] = c;
                len[n++] = l;
                kraft += 1.0 / (double)(1u << l);
            }
            CHECK(n >= 2 && kraft <= 1.0 + 1e-12);
            for (int a = 0; a < n; a++)
                for (int b = 0; b < n; b++)
                    if (a != b && len[a] <= len[b] && code[b] >> (len[b] - len[a]) == code[a])
                        test_fail(__FILE__, __LINE__, "table kind %d row %d: a code is a prefix of another", kind, i);
        }
}

TEST(h264enc_cavlc_matches_the_textbook_example) {
    /* Richardson, "H.264 and MPEG-4 Video Compression", CAVLC example 1: the
       4x4 block {0 3 -1 0 / 0 -1 1 0 / 1 0 0 0 / 0 0 0 0}, zig-zag 0 3 0 1 -1 -1 0 1,
       nC = 0, codes as 000010001110010111101101. */
    int16_t c[16] = {0, 3, 0, 1, -1, -1, 0, 1};
    uint8_t out[8];
    size_t bits = h264enc_cavlc_for_test(c, 16, 0, out, sizeof out);
    CHECK_INT(bits, 24);
    CHECK_INT(out[0], 0x08);
    CHECK_INT(out[1], 0xE5);
    CHECK_INT(out[2], 0xED);
}

TEST(h264enc_cavlc_blocks_read_back) {
    uint32_t seed = 4242;
    static const int ncs[] = {0, 1, 2, 3, 4, 7, 8, 16};
    for (int iter = 0; iter < 4000; iter++) {
        int n = iter % 3 == 0 ? 16 : iter % 3 == 1 ? 15 : 4;
        int nc = n == 4 ? -1 : ncs[iter % 8];
        int16_t c[16] = {0}, back[16];
        seed = seed * 1103515245u + 12345u;
        int density = (int)(seed >> 28) + 1; /* 1..16 */
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            if ((int)(seed >> 28) >= density) continue;
            seed = seed * 1103515245u + 12345u;
            int kind = (int)(seed >> 29), v;
            seed = seed * 1103515245u + 12345u;
            if (kind < 4) v = 1;
            else if (kind < 6) v = 2 + (int)(seed >> 29);
            else if (kind < 7) v = 10 + (int)(seed >> 24) % 300;
            else v = 1 + (int)(seed >> 20) % 2063; /* up to the largest Baseline level */
            c[i] = (int16_t)(seed & 0x100 ? -v : v);
        }
        uint8_t buf[256];
        size_t bits = h264enc_cavlc_for_test(c, n, nc, buf, sizeof buf);
        CHECK(bits > 0);
        Rd r = {buf, (bits + 7) / 8, 0};
        if (!read_block(&r, n, nc, back) || r.pos != bits || memcmp(c, back, sizeof c[0] * (size_t)n) != 0) {
            test_fail(__FILE__, __LINE__, "block %d (n %d, nC %d) did not read back (%zu bits, read %zu)", iter, n, nc, bits,
                      r.pos);
            return;
        }
    }
}

/* ------------------------------------------------- transform & deblocking */

TEST(h264enc_inverse_transform_follows_the_standard) {
    uint32_t seed = 99;
    for (int iter = 0; iter < 2000; iter++) {
        int32_t d[16];
        for (int i = 0; i < 16; i++) {
            seed = seed * 1103515245u + 12345u;
            d[i] = (int32_t)((seed >> 16) % 4001) - 2000;
        }
        uint8_t got[16];
        h264enc_idct_for_test(d, got);
        /* 8.5.12.2 literally: rows, then columns, then (x + 32) >> 6 */
        int f[4][4], h[4][4];
        for (int i = 0; i < 4; i++) {
            int e0 = d[i * 4] + d[i * 4 + 2], e1 = d[i * 4] - d[i * 4 + 2];
            int e2 = (d[i * 4 + 1] >> 1) - d[i * 4 + 3], e3 = d[i * 4 + 1] + (d[i * 4 + 3] >> 1);
            f[i][0] = e0 + e3, f[i][1] = e1 + e2, f[i][2] = e1 - e2, f[i][3] = e0 - e3;
        }
        for (int j = 0; j < 4; j++) {
            int g0 = f[0][j] + f[2][j], g1 = f[0][j] - f[2][j], g2 = (f[1][j] >> 1) - f[3][j], g3 = f[1][j] + (f[3][j] >> 1);
            h[0][j] = g0 + g3, h[1][j] = g1 + g2, h[2][j] = g1 - g2, h[3][j] = g0 - g3;
        }
        for (int i = 0; i < 16; i++) {
            int v = 128 + ((h[i / 4][i % 4] + 32) >> 6);
            v = v < 0 ? 0 : v > 255 ? 255 : v;
            if (got[i] != v) {
                test_fail(__FILE__, __LINE__, "block %d sample %d: %d, expected %d", iter, i, got[i], v);
                return;
            }
        }
    }
}

static bool filtered(uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7,
                     int bs, int qp, bool chroma, const uint8_t want[8]) {
    uint8_t line[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    h264enc_filter_for_test(line, bs, qp, chroma);
    return memcmp(line, want, 8) == 0;
}

TEST(h264enc_deblocking_filter_hand_worked) {
    /* QP 30: alpha 25, beta 8, tc0 = 1 / 1 / 2 for bS 1 / 2 / 3 (Tables 8-16, 8-17) */
    /* bS 1 on a step of 10: tc = 1 + 1 + 1 = 3, delta clipped from 4 to 3; p1/q1 move by tc0 */
    CHECK(filtered(60, 60, 60, 60, 70, 70, 70, 70, 1, 30, false, (const uint8_t[]){60, 60, 61, 63, 67, 69, 70, 70}));
    /* bS 4, step too large for the strong filter (10 >= alpha/4 + 2 = 8): only p0/q0 */
    CHECK(filtered(60, 60, 60, 60, 70, 70, 70, 70, 4, 30, false, (const uint8_t[]){60, 60, 60, 63, 68, 70, 70, 70}));
    /* bS 4, small step: the strong filter reshapes three samples a side */
    CHECK(filtered(60, 60, 60, 60, 66, 66, 66, 66, 4, 30, false, (const uint8_t[]){60, 61, 62, 62, 64, 65, 65, 66}));
    /* a real edge (40 >= alpha): left alone */
    CHECK(filtered(60, 60, 60, 60, 100, 100, 100, 100, 3, 30, false, (const uint8_t[]){60, 60, 60, 60, 100, 100, 100, 100}));
    /* chroma bS 1: tc = tc0 + 1 = 2, only p0/q0 */
    CHECK(filtered(60, 60, 60, 60, 70, 70, 70, 70, 1, 30, true, (const uint8_t[]){60, 60, 60, 62, 68, 70, 70, 70}));
    /* QP 15: alpha is 0, nothing is filtered */
    CHECK(filtered(60, 60, 60, 60, 62, 62, 62, 62, 4, 15, false, (const uint8_t[]){60, 60, 60, 60, 62, 62, 62, 62}));
}

/* -------------------------------------------- motion vector prediction */

static void mvp(int st[9][3], int mbx, int mby, int shape, int fx, int fy, int *x, int *y) {
    int first[2] = {fx, fy}, out[2] = {-999, -999};
    h264enc_mvpred_for_test((const int(*)[3])st, 3, 3, mbx, mby, shape, first, out);
    *x = out[0];
    *y = out[1];
}

TEST(h264enc_motion_vector_prediction_cases) {
    /* a 3x3 macroblock picture; st[i] = {ref (-1 intra), mvx, mvy} */
    int x, y;
    int base[9][3] = {{0, 100, -8}, {0, 8, 4}, {0, -4, 12}, {0, 4, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    int st[9][3];
#define RESET() memcpy(st, base, sizeof st)
    RESET(); /* median of A (4,0), B (8,4), C (-4,12) */
    mvp(st, 1, 1, 0, 0, 0, &x, &y);
    CHECK(x == 4 && y == 4);
    RESET(); /* right edge: C is outside, D (above-left) stands in: A (0,0)=MB4, B (8,4)=MB2, D (100,-8)=MB1... */
    st[4][1] = 4, st[4][2] = 0, st[1][1] = 100, st[1][2] = -8;
    mvp(st, 2, 1, 0, 0, 0, &x, &y); /* A = MB4 (4,0), B = MB2 (-4,12), D = MB1 (100,-8) */
    CHECK(x == 4 && y == 0);
    RESET(); /* only A refers to the reference: its vector, not the median */
    st[1][0] = -1, st[2][0] = -1;
    mvp(st, 1, 1, 0, 0, 0, &x, &y);
    CHECK(x == 4 && y == 0);
    RESET(); /* top row: B and C unavailable, so both copy A */
    st[0][1] = 6, st[0][2] = -2;
    mvp(st, 1, 0, 0, 0, 0, &x, &y);
    CHECK(x == 6 && y == -2);
    RESET(); /* first macroblock: nothing around, zero */
    mvp(st, 0, 0, 0, 0, 0, &x, &y);
    CHECK(x == 0 && y == 0);
    RESET(); /* 16x8 top: B directly */
    mvp(st, 1, 1, 1, 0, 0, &x, &y);
    CHECK(x == 8 && y == 4);
    RESET(); /* 16x8 bottom: A directly */
    mvp(st, 1, 1, 2, 20, 20, &x, &y);
    CHECK(x == 4 && y == 0);
    RESET(); /* 16x8 bottom, A intra: B is the top partition (20,20), C is to the right (not yet coded),
                D is the intra left MB: only B refers -> B */
    st[3][0] = -1;
    mvp(st, 1, 1, 2, 20, 20, &x, &y);
    CHECK(x == 20 && y == 20);
    RESET(); /* 8x16 left: A directly */
    mvp(st, 1, 1, 3, 0, 0, &x, &y);
    CHECK(x == 4 && y == 0);
    RESET(); /* 8x16 right: C (above-right MB) directly */
    mvp(st, 1, 1, 4, 20, 20, &x, &y);
    CHECK(x == -4 && y == 12);
    RESET(); /* 8x16 right, C intra: median of A = left partition (20,20), B (8,4), C intra (0,0) */
    st[2][0] = -1;
    mvp(st, 1, 1, 4, 20, 20, &x, &y);
    CHECK(x == 8 && y == 4);
    /* P_Skip (8.4.1.1) */
    RESET(); /* left column: zero */
    mvp(st, 0, 1, -1, 0, 0, &x, &y);
    CHECK(x == 0 && y == 0);
    RESET(); /* A has a zero vector: zero */
    st[3][1] = 0, st[3][2] = 0;
    mvp(st, 1, 1, -1, 0, 0, &x, &y);
    CHECK(x == 0 && y == 0);
    RESET(); /* otherwise the 16x16 prediction */
    mvp(st, 1, 1, -1, 0, 0, &x, &y);
    CHECK(x == 4 && y == 4);
    RESET(); /* A intra, B zero: zero */
    st[3][0] = -1, st[1][1] = 0, st[1][2] = 0;
    mvp(st, 1, 1, -1, 0, 0, &x, &y);
    CHECK(x == 0 && y == 0);
    RESET(); /* A intra (counts as 0,0 with no reference), B (8,4), C (-4,12): median */
    st[3][0] = -1;
    mvp(st, 1, 1, -1, 0, 0, &x, &y);
    CHECK(x == 0 && y == 4);
#undef RESET
}

/* -------------------------------------------------------------- streams */

typedef struct {
    int w, h;
    uint8_t *y, *u, *v;
} Pic;

static void pic_make(Pic *p, int w, int h) {
    p->w = w, p->h = h;
    p->y = xmalloc((size_t)w * h * 3 / 2);
    p->u = p->y + (size_t)w * h;
    p->v = p->u + (size_t)w * h / 4;
}

static void pic_draw(Pic *p, int t, uint32_t noise) { /* gradient, a moving block, stripes, optional noise */
    uint32_t s = 777u + (uint32_t)t;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            int v = 30 + x * 150 / p->w + y * 40 / p->h;
            if (x >= t * 2 % p->w && x < t * 2 % p->w + 24 && y >= 8 && y < 40) v = 210;
            if (((x + t) / 6) & 1 && y > p->h / 2) v += 20;
            s = s * 1103515245u + 12345u;
            if (noise) v += (int)((s >> 16) % (noise * 2 + 1)) - (int)noise;
            p->y[(size_t)y * p->w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    for (int i = 0; i < p->w * p->h / 4; i++) {
        p->u[i] = (uint8_t)(110 + (i + t) % 30);
        p->v[i] = (uint8_t)(140 - (i / p->w) % 25);
    }
}

static H264Picture as_picture(const Pic *p, int64_t pts) {
    H264Picture pic = {p->y, p->u, p->v, p->w, p->w / 2, p->w / 2, pts};
    return pic;
}

static double recon_psnr(const H264Enc *e, const Pic *p) {
    const uint8_t *y, *u, *v;
    int sy, sc;
    h264enc_recon(e, &y, &u, &v, &sy, &sc);
    double sse = 0;
    for (int j = 0; j < p->h; j++)
        for (int i = 0; i < p->w; i++) {
            int d = y[(size_t)j * sy + i] - p->y[(size_t)j * p->w + i];
            sse += d * d;
        }
    return sse ? 10 * log10(255.0 * 255.0 * p->w * p->h / sse) : 99;
}

TEST(h264enc_parameter_sets_read_back) {
    H264Params hp = {.width = 720, .height = 480, .fps_num = 30000, .fps_den = 1001, .sar_w = 8, .sar_h = 9, .annexb = true};
    H264Enc *e = h264enc_new(&hp);
    CHECK(e);
    size_t n;
    const uint8_t *sps = h264enc_sps(e, &n);
    uint8_t raw[64];
    CHECK(n > 4 && n < sizeof raw);
    CHECK_INT(sps[0], 0x67); /* nal_ref_idc 3, type 7 */
    Rd r = {raw, unescape(sps + 1, n - 1, raw), 0};
    CHECK_INT(rd_bits(&r, 8), 66);   /* Baseline */
    CHECK_INT(rd_bits(&r, 8), 0xC0); /* constraint_set0 + 1: Constrained Baseline */
    CHECK_INT(rd_bits(&r, 8), 30);   /* level 3.0 for SD at 30 fps */
    CHECK_INT(rd_ue(&r), 0);
    uint32_t log2fn = rd_ue(&r) + 4;
    CHECK(log2fn >= 6); /* frame_num must not wrap within a 60-frame GOP */
    CHECK_INT(rd_ue(&r), 2); /* pic_order_cnt_type: no reordering */
    CHECK_INT(rd_ue(&r), 1); /* one reference frame */
    CHECK_INT(rd_bit(&r), 0);
    CHECK_INT(rd_ue(&r), 44); /* 45 x 30 macroblocks */
    CHECK_INT(rd_ue(&r), 29);
    CHECK_INT(rd_bit(&r), 1); /* frame_mbs_only */
    CHECK_INT(rd_bit(&r), 1); /* direct_8x8_inference */
    CHECK_INT(rd_bit(&r), 0); /* no cropping */
    CHECK_INT(rd_bit(&r), 1); /* VUI */
    CHECK_INT(rd_bit(&r), 1);
    CHECK_INT(rd_bits(&r, 8), 255); /* Extended_SAR */
    CHECK_INT(rd_bits(&r, 16), 8);
    CHECK_INT(rd_bits(&r, 16), 9);
    CHECK_INT(rd_bit(&r), 0); /* overscan */
    CHECK_INT(rd_bit(&r), 1); /* video signal type */
    CHECK_INT(rd_bits(&r, 3), 5);
    CHECK_INT(rd_bit(&r), 0); /* limited range */
    CHECK_INT(rd_bit(&r), 1);
    CHECK_INT(rd_bits(&r, 8), 6); /* SMPTE 170M (BT.601) for SD */
    CHECK_INT(rd_bits(&r, 8), 6);
    CHECK_INT(rd_bits(&r, 8), 6);
    CHECK_INT(rd_bit(&r), 0);
    CHECK_INT(rd_bit(&r), 1); /* timing: 1001 / 60000 = 29.97 fps */
    CHECK_INT(rd_bits(&r, 32), 1001);
    CHECK_INT(rd_bits(&r, 32), 60000);
    CHECK_INT(rd_bit(&r), 1);
    CHECK_INT(rd_bit(&r), 0);
    CHECK_INT(rd_bit(&r), 0);
    CHECK_INT(rd_bit(&r), 0);
    CHECK_INT(rd_bit(&r), 1); /* bitstream restriction */
    CHECK_INT(rd_bit(&r), 1);
    rd_ue(&r);
    rd_ue(&r);
    rd_ue(&r);
    rd_ue(&r);
    CHECK_INT(rd_ue(&r), 0); /* max_num_reorder_frames: decoders show frames at once */
    CHECK_INT(rd_ue(&r), 1);
    CHECK_INT(rd_bit(&r), 1); /* rbsp_stop_one_bit */
    const uint8_t *pps = h264enc_pps(e, &n);
    CHECK_INT(pps[0], 0x68);
    char codec[16];
    h264enc_codec(e, codec);
    CHECK_STR(codec, "avc1.42C01E");
    uint8_t avcc[128];
    size_t an = h264enc_avcc(e, avcc, sizeof avcc);
    size_t sn, pn;
    h264enc_sps(e, &sn);
    h264enc_pps(e, &pn);
    CHECK_INT(an, 11 + sn + pn);
    CHECK(avcc[0] == 1 && avcc[1] == 66 && avcc[2] == 0xC0 && avcc[3] == 30 && avcc[4] == 0xFF && avcc[5] == 0xE1);
    CHECK_INT(avcc[6] << 8 | avcc[7], sn);
    CHECK_INT(avcc[8 + sn], 1);
    CHECK_INT(h264enc_avcc(e, avcc, 10), 0);
    h264enc_free(e);

    /* odd size: cropping, square pixels, 1080p gets BT.709 and a level 4.0 */
    H264Params odd = {.width = 50, .height = 30, .fps_num = 25, .fps_den = 1};
    e = h264enc_new(&odd);
    CHECK(e);
    sps = h264enc_sps(e, &n);
    r = (Rd){raw, unescape(sps + 1, n - 1, raw), 0};
    rd_bits(&r, 24);
    rd_ue(&r);
    rd_ue(&r);
    rd_ue(&r);
    rd_ue(&r);
    rd_bit(&r);
    CHECK_INT(rd_ue(&r), 3); /* 64 x 32 coded */
    CHECK_INT(rd_ue(&r), 1);
    rd_bits(&r, 2);
    CHECK_INT(rd_bit(&r), 1); /* cropped to 50 x 30: right 7, bottom 1 (in pairs of samples) */
    CHECK_INT(rd_ue(&r), 0);
    CHECK_INT(rd_ue(&r), 7);
    CHECK_INT(rd_ue(&r), 0);
    CHECK_INT(rd_ue(&r), 1);
    h264enc_free(e);
    H264Params hd = {.width = 1920, .height = 1080, .fps_num = 30, .fps_den = 1};
    e = h264enc_new(&hd);
    CHECK(e);
    h264enc_codec(e, codec);
    CHECK_STR(codec, "avc1.42C028");
    h264enc_free(e);
}

TEST(h264enc_refuses_impossible_sizes) {
    static const int bad[][2] = {{0, 0}, {15, 16}, {16, 15}, {17, 16}, {16, 33}, {5000, 16}, {16, 4098}, {-16, 16}};
    for (size_t i = 0; i < countof(bad); i++) {
        H264Params hp = {.width = bad[i][0], .height = bad[i][1]};
        H264Enc *e = h264enc_new(&hp);
        if (e) {
            h264enc_free(e);
            test_fail(__FILE__, __LINE__, "accepted %dx%d", bad[i][0], bad[i][1]);
        }
    }
}

/* Encode `frames` pictures; returns total bytes, fills sizes/idr when given. */
static size_t encode_run(const H264Params *hp, int frames, uint32_t noise, size_t *sizes, bool *idr, double *min_psnr,
                         Buf *all) {
    H264Enc *e = h264enc_new(hp);
    if (!e) return 0;
    Pic p;
    pic_make(&p, hp->width, hp->height);
    size_t total = 0;
    if (min_psnr) *min_psnr = 99;
    for (int i = 0; i < frames; i++) {
        pic_draw(&p, i, noise);
        H264Picture pic = as_picture(&p, i);
        H264Packet pk;
        if (!h264enc_encode(e, &pic, &pk)) break;
        if (sizes) sizes[i] = pk.size;
        if (idr) idr[i] = pk.idr;
        if (all) buf_put(all, pk.data, pk.size);
        if (min_psnr) {
            double q = recon_psnr(e, &p);
            if (q < *min_psnr) *min_psnr = q;
        }
        total += pk.size;
    }
    free(p.y);
    h264enc_free(e);
    return total;
}

TEST(h264enc_streams_hold_together) {
    H264Params hp = {.width = 176, .height = 144, .fps_num = 25, .fps_den = 1, .quality = 26, .keyint = 5, .annexb = true};
    size_t sizes[16];
    bool idr[16];
    double q;
    Buf a = {0}, b = {0};
    CHECK(encode_run(&hp, 16, 3, sizes, idr, &q, &a) > 0);
    for (int i = 0; i < 16; i++) CHECK_INT(idr[i], i % 5 == 0);
    CHECK(q > 35); /* the reconstruction is close to the source */
    /* Annex-B: an IDR starts with SPS, PPS, then the IDR slice; a P frame is one slice */
    const uint8_t *s = (const uint8_t *)a.p;
    CHECK(s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 1 && s[4] == 0x67);
    size_t off = sizes[0] + sizes[1] + sizes[2] + sizes[3] + sizes[4];
    CHECK(!memcmp(s + sizes[0], "\0\0\0\1\x41", 5)); /* nal_ref_idc 2, non-IDR slice */
    CHECK(!memcmp(s + off, "\0\0\0\1\x67", 5));      /* frame 5: IDR again */
    /* the same input gives the same bytes */
    CHECK(encode_run(&hp, 16, 3, NULL, NULL, NULL, &b) == a.len);
    CHECK(!memcmp(a.p, b.p, a.len));
    buf_free(&a);
    buf_free(&b);

    /* both presets, odd sizes */
    static const int sz[][2] = {{18, 34}, {50, 30}, {16, 16}, {344, 208}};
    for (size_t i = 0; i < countof(sz); i++)
        for (int preset = 0; preset < 2; preset++) {
            H264Params o = {.width = sz[i][0], .height = sz[i][1], .quality = 24, .preset = (H264Preset)preset};
            CHECK(encode_run(&o, 12, 2, NULL, NULL, &q, NULL) > 0);
            if (q < 33) test_fail(__FILE__, __LINE__, "%dx%d preset %d: PSNR %.1f", sz[i][0], sz[i][1], preset, q);
        }
}

TEST(h264enc_length_prefixed_packets) {
    H264Params hp = {.width = 96, .height = 64, .quality = 28, .keyint = 4};
    H264Enc *e = h264enc_new(&hp);
    CHECK(e);
    Pic p;
    pic_make(&p, 96, 64);
    for (int i = 0; i < 8; i++) {
        pic_draw(&p, i, 2);
        H264Picture pic = as_picture(&p, i);
        H264Packet pk;
        CHECK(h264enc_encode(e, &pic, &pk));
        size_t at = 0;
        int nals = 0;
        while (at + 4 <= pk.size) {
            size_t len = (size_t)pk.data[at] << 24 | (size_t)pk.data[at + 1] << 16 | (size_t)pk.data[at + 2] << 8 | pk.data[at + 3];
            CHECK(len > 0 && at + 4 + len <= pk.size);
            int type = pk.data[at + 4] & 31;
            CHECK(type == (pk.idr ? 5 : 1)); /* only slices: SPS/PPS live in avcC */
            at += 4 + len;
            nals++;
        }
        CHECK(at == pk.size && nals == 1);
        CHECK_INT(pk.idr, i % 4 == 0);
        CHECK_INT(pk.pts, i);
    }
    free(p.y);
    h264enc_free(e);
}

TEST(h264enc_still_pictures_cost_almost_nothing) {
    /* the same picture over and over: P frames are all skipped macroblocks */
    H264Params hp = {.width = 320, .height = 240, .quality = 26};
    H264Enc *e = h264enc_new(&hp);
    CHECK(e);
    Pic p;
    pic_make(&p, 320, 240);
    pic_draw(&p, 3, 0);
    for (int i = 0; i < 10; i++) {
        H264Picture pic = as_picture(&p, i);
        H264Packet pk;
        CHECK(h264enc_encode(e, &pic, &pk));
        if (i) CHECK(pk.size < 24);
    }
    free(p.y);
    h264enc_free(e);
}

TEST(h264enc_bitrate_cap_holds_back) {
    H264Params free_run = {.width = 160, .height = 128, .fps_num = 25, .fps_den = 1, .quality = 16};
    H264Params capped = free_run;
    capped.max_kbps = 150;
    size_t a[60], b[60];
    CHECK(encode_run(&free_run, 60, 24, a, NULL, NULL, NULL) > 0);
    CHECK(encode_run(&capped, 60, 24, b, NULL, NULL, NULL) > 0);
    size_t late_a = 0, late_b = 0;
    for (int i = 40; i < 60; i++) late_a += a[i], late_b += b[i];
    CHECK(late_b * 2 < late_a); /* once the bucket fills, QP rises */
}

/* ------------------------------------------------------- a real DVD rip */

static bool read_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

TEST(h264enc_reencodes_a_real_dvd_rip) {
    const char *path = plat_getenv("HEMMA_TEST_MKV");
    if (!path || !*path) return; /* opt-in: HEMMA_TEST_MKV=/path/to/dvd-rip.mkv */
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    MkvReader r = {fp, size, read_cb};
    char err[100];
    Mkv *m = mkv_open(&r, err, sizeof err);
    CHECK(m);
    const MkvTrack *v = mkv_find_track(m, MKV_VIDEO);
    if (!v || strcmp(v->codec, "V_MPEG2") != 0) {
        mkv_close(m);
        fclose(fp);
        return;
    }
    Mpeg2 *d = mpeg2_new();
    mpeg2_decode(d, v->priv, v->priv_len, MPEG2_NOPTS);
    mkv_seek(m, 60000000000LL, v->number);
    mpeg2_reset(d);
    H264Enc *e = NULL;
    int frames = 0;
    size_t bytes = 0;
    double worst = 99;
    MkvFrame f;
    Mpeg2Frame fr;
    while (frames < 90 && mkv_next(m, &f) == 1) {
        if (f.track != v->number) continue;
        mpeg2_decode(d, f.data, f.size, f.pts);
        while (frames < 90 && mpeg2_frame(d, &fr)) {
            if (!e) {
                H264Params hp = {.width = fr.width, .height = fr.height, .fps_num = fr.rate_num, .fps_den = fr.rate_den,
                                 .quality = 26};
                e = h264enc_new(&hp);
                CHECK(e);
            }
            H264Picture pic = {fr.y, fr.u, fr.v, fr.stride_y, fr.stride_c, fr.stride_c, frames};
            H264Packet pk;
            CHECK(h264enc_encode(e, &pic, &pk));
            bytes += pk.size;
            Pic src = {fr.width, fr.height, NULL, NULL, NULL};
            /* PSNR against the decoded frame (luma), read through its stride */
            const uint8_t *ry, *ru, *rv;
            int sy, sc;
            h264enc_recon(e, &ry, &ru, &rv, &sy, &sc);
            double sse = 0;
            for (int j = 0; j < src.h; j++)
                for (int i = 0; i < src.w; i++) {
                    int dd = ry[(size_t)j * sy + i] - fr.y[(size_t)j * fr.stride_y + i];
                    sse += dd * dd;
                }
            double q = sse ? 10 * log10(255.0 * 255.0 * src.w * src.h / sse) : 99;
            if (q < worst) worst = q;
            frames++;
        }
    }
    CHECK_INT(frames, 90);
    CHECK(worst > 34);
    CHECK(bytes > 0);
    h264enc_free(e);
    mpeg2_free(d);
    mkv_close(m);
    fclose(fp);
}
