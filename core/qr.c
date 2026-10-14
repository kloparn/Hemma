/* core/qr.c — QR code encoder (byte mode, versions 1-10), after ISO/IEC 18004 and Project
 * Nayuki's well-known reference implementation: data + Reed-Solomon codewords, placed in the
 * zigzag around the function patterns, under the mask that scores best. */
#include "qr.h"
#include "html.h"

/* codewords of error correction per block, and blocks, for each level (L M Q H) and version */
static const uint8_t ECC_PER_BLOCK[4][QR_MAX_VERSION + 1] = {
    {0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18},
    {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26},
    {0, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24},
    {0, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28},
};
static const uint8_t BLOCKS[4][QR_MAX_VERSION + 1] = {
    {0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4},
    {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5},
    {0, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8},
    {0, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8},
};
static const int FORMAT_LEVEL[4] = {1, 0, 3, 2}; /* how the format bits name L M Q H */

#define QR_MAX_CODEWORDS 346 /* version 10's */

typedef struct {
    QrCode *q;
    uint8_t fn[QR_MAX_SIZE][QR_MAX_SIZE]; /* 1 = a function module (never data, never masked) */
} Grid;

static void put(Grid *g, int x, int y, bool dark) { /* x = column, y = row */
    g->q->m[y][x] = dark;
    g->fn[y][x] = 1;
}

/* modules left for data and error correction */
static int raw_modules(int ver) {
    int n = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int na = ver / 7 + 2;
        n -= (25 * na - 10) * na - 55;
        if (ver >= 7) n -= 36;
    }
    return n;
}

static int data_codewords(int ver, int ecl) {
    return raw_modules(ver) / 8 - ECC_PER_BLOCK[ecl][ver] * BLOCKS[ecl][ver];
}

static int align_positions(int ver, int out[7]) {
    if (ver == 1) return 0;
    int na = ver / 7 + 2, step = (ver * 8 + na * 3 + 5) / (na * 4 - 4) * 2;
    out[0] = 6;
    for (int i = na - 1, pos = ver * 4 + 10; i >= 1; i--, pos -= step) out[i] = pos;
    return na;
}

static void format_bits(Grid *g, int ecl, int mask) {
    int size = g->q->size, data = FORMAT_LEVEL[ecl] << 3 | mask, rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
#define BIT(i) (((bits >> (i)) & 1) != 0)
    for (int i = 0; i <= 5; i++) put(g, 8, i, BIT(i)); /* by the top-left finder */
    put(g, 8, 7, BIT(6));
    put(g, 8, 8, BIT(7));
    put(g, 7, 8, BIT(8));
    for (int i = 9; i < 15; i++) put(g, 14 - i, 8, BIT(i));
    for (int i = 0; i < 8; i++) put(g, size - 1 - i, 8, BIT(i)); /* and the copy by the other two */
    for (int i = 8; i < 15; i++) put(g, 8, size - 15 + i, BIT(i));
#undef BIT
    put(g, 8, size - 8, true); /* always dark */
}

static void function_patterns(Grid *g, int ecl) {
    int size = g->q->size, ver = g->q->version;
    for (int i = 0; i < size; i++) { /* timing */
        put(g, 6, i, i % 2 == 0);
        put(g, i, 6, i % 2 == 0);
    }
    const int fx[3] = {3, size - 4, 3}, fy[3] = {3, 3, size - 4};
    for (int f = 0; f < 3; f++) /* finders, with their light separators */
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int x = fx[f] + dx, y = fy[f] + dy, d = MAX(abs(dx), abs(dy));
                if (x >= 0 && x < size && y >= 0 && y < size) put(g, x, y, d != 2 && d != 4);
            }
    int pos[7], na = align_positions(ver, pos);
    for (int i = 0; i < na; i++)
        for (int j = 0; j < na; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue; /* the finders' */
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++) put(g, pos[i] + dx, pos[j] + dy, MAX(abs(dx), abs(dy)) != 1);
        }
    format_bits(g, ecl, 0); /* (reserves the spots; the real bits come with the mask) */
    if (ver >= 7) {
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = (long)ver << 12 | rem;
        for (int i = 0; i < 18; i++) {
            bool dark = ((bits >> i) & 1) != 0;
            int a = size - 11 + i % 3, b = i / 3;
            put(g, a, b, dark);
            put(g, b, a, dark);
        }
    }
}

/* Reed-Solomon over GF(2^8) with the polynomial 0x11D */
static uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

static void rs_divisor(int degree, uint8_t *out) {
    memset(out, 0, (size_t)degree);
    out[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            out[j] = gf_mul(out[j], root);
            if (j + 1 < degree) out[j] ^= out[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_remainder(const uint8_t *data, int len, const uint8_t *div, int degree, uint8_t *out) {
    memset(out, 0, (size_t)degree);
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ out[0];
        memmove(out, out + 1, (size_t)degree - 1);
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++) out[j] ^= gf_mul(div[j], factor);
    }
}

/* the data codewords, then each block's error correction, interleaved as the standard says */
static int codewords(int ver, int ecl, const uint8_t *data, uint8_t *out) {
    int nb = BLOCKS[ecl][ver], ecc = ECC_PER_BLOCK[ecl][ver], raw = raw_modules(ver) / 8;
    int nshort = nb - raw % nb, short_len = raw / nb;
    uint8_t blocks[8][QR_MAX_CODEWORDS + 1], div[32];
    rs_divisor(ecc, div);
    for (int i = 0, k = 0; i < nb; i++) {
        int dlen = short_len - ecc + (i < nshort ? 0 : 1), at = dlen;
        memcpy(blocks[i], data + k, (size_t)dlen);
        k += dlen;
        if (i < nshort) blocks[i][at++] = 0; /* (a placeholder: short blocks line up with long ones) */
        rs_remainder(blocks[i], dlen, div, ecc, blocks[i] + at);
    }
    int n = 0;
    for (int i = 0; i <= short_len; i++)
        for (int j = 0; j < nb; j++)
            if (i != short_len - ecc || j >= nshort) out[n++] = blocks[j][i];
    return n;
}

static void place(Grid *g, const uint8_t *cw, int n) {
    int size = g->q->size, bit = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5; /* (the vertical timing line) */
        bool up = ((right + 1) & 2) == 0;
        for (int v = 0; v < size; v++)
            for (int j = 0; j < 2; j++) {
                int x = right - j, y = up ? size - 1 - v : v;
                if (g->fn[y][x] || bit >= n * 8) continue;
                g->q->m[y][x] = (cw[bit >> 3] >> (7 - (bit & 7))) & 1;
                bit++;
            }
    }
}

static void apply_mask(Grid *g, int mask) {
    int size = g->q->size;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            bool flip;
            switch (mask) {
            case 0: flip = (x + y) % 2 == 0; break;
            case 1: flip = y % 2 == 0; break;
            case 2: flip = x % 3 == 0; break;
            case 3: flip = (x + y) % 3 == 0; break;
            case 4: flip = (x / 3 + y / 2) % 2 == 0; break;
            case 5: flip = x * y % 2 + x * y % 3 == 0; break;
            case 6: flip = (x * y % 2 + x * y % 3) % 2 == 0; break;
            default: flip = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
            }
            if (flip && !g->fn[y][x]) g->q->m[y][x] ^= 1;
        }
}

/* How hard the code is to read under a mask (the standard's four rules): long runs, 2x2 blocks,
   finder look-alikes, and dark/light out of balance. Lowest wins. */
static long penalty(const QrCode *q) {
    int size = q->size;
    long score = 0;
    for (int pass = 0; pass < 2; pass++) /* rows, then columns */
        for (int a = 0; a < size; a++) {
            int run = 0;
            for (int b = 0; b < size; b++) {
                int here = pass ? q->m[b][a] : q->m[a][b];
                int prev = b ? (pass ? q->m[b - 1][a] : q->m[a][b - 1]) : -1;
                run = here == prev ? run + 1 : 1;
                if (run == 5) score += 3;
                else if (run > 5) score += 1;
                /* 1:1:3:1:1 with four light on one side (past the edge counts as light) */
                if (b >= 6) {
                    static const uint8_t pat[7] = {1, 0, 1, 1, 1, 0, 1};
                    bool hit = true;
                    for (int k = 0; k < 7 && hit; k++) hit = (pass ? q->m[b - 6 + k][a] : q->m[a][b - 6 + k]) == pat[k];
                    if (hit) {
                        bool before = true, after = true;
                        for (int k = 1; k <= 4; k++) {
                            int i = b - 6 - k, j = b + k;
                            if (i >= 0 && (pass ? q->m[i][a] : q->m[a][i])) before = false;
                            if (j < size && (pass ? q->m[j][a] : q->m[a][j])) after = false;
                        }
                        if (before || after) score += 40;
                    }
                }
            }
        }
    int dark = 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            dark += q->m[y][x];
            if (x && y && q->m[y][x] == q->m[y - 1][x] && q->m[y][x] == q->m[y][x - 1] && q->m[y][x] == q->m[y - 1][x - 1])
                score += 3;
        }
    long total = (long)size * size;
    long k = (labs(dark * 20L - total * 10) + total - 1) / total - 1;
    return score + k * 10;
}

bool qr_make(QrCode *q, const void *data, size_t len, int ecl, int mask) {
    if (ecl < QR_L || ecl > QR_H || mask < -1 || mask > 7) return false;
    int ver = 1;
    for (; ver <= QR_MAX_VERSION; ver++) /* mode (4 bits) + count (8, or 16 from version 10) + the bytes */
        if ((long)len * 8 + 4 + (ver < 10 ? 8 : 16) <= (long)data_codewords(ver, ecl) * 8 && len < (ver < 10 ? 256u : 65536u))
            break;
    if (ver > QR_MAX_VERSION) return false;
    int cap = data_codewords(ver, ecl), nbits = 0;
    uint8_t dc[QR_MAX_CODEWORDS] = {0}, cw[QR_MAX_CODEWORDS];
#define PUSH(v, n)                                                                                                     \
    for (int i_ = (n) - 1; i_ >= 0; i_--, nbits++)                                                                     \
        if ((((unsigned long)(v)) >> i_) & 1) dc[nbits >> 3] |= (uint8_t)(0x80 >> (nbits & 7));
    PUSH(4, 4); /* byte mode */
    PUSH(len, ver < 10 ? 8 : 16);
    for (size_t i = 0; i < len; i++) PUSH(((const uint8_t *)data)[i], 8);
    int end = MIN(4, cap * 8 - nbits); /* terminator */
    nbits += end;
    nbits = (nbits + 7) / 8 * 8;
#undef PUSH
    for (int i = nbits / 8, pad = 0xEC; i < cap; i++, pad ^= 0xEC ^ 0x11) dc[i] = (uint8_t)pad;
    int n = codewords(ver, ecl, dc, cw);

    memset(q, 0, sizeof *q);
    q->version = ver;
    q->size = ver * 4 + 17;
    Grid *g = xmalloc(sizeof *g);
    memset(g, 0, sizeof *g);
    g->q = q;
    function_patterns(g, ecl);
    place(g, cw, n);
    if (mask < 0) {
        long best = -1;
        for (int m = 0; m < 8; m++) {
            apply_mask(g, m);
            format_bits(g, ecl, m);
            long p = penalty(q);
            if (best < 0 || p < best) {
                best = p;
                mask = m;
            }
            apply_mask(g, m); /* (XOR again: undone) */
        }
    }
    apply_mask(g, mask);
    format_bits(g, ecl, mask);
    q->mask = mask;
    free(g);
    return true;
}

void qr_svg(Buf *b, const QrCode *q, const char *label) {
    int side = q->size + 8;
    buf_printf(b, "<svg class=\"qr\" viewBox=\"-4 -4 %d %d\" role=\"img\" aria-label=\"", side, side);
    html_esc(b, label ? label : "QR code");
    buf_printf(b, "\" shape-rendering=\"crispEdges\"><rect x=\"-4\" y=\"-4\" width=\"%d\" height=\"%d\" fill=\"#fff\"/>"
                  "<path fill=\"#000\" d=\"",
               side, side);
    for (int y = 0; y < q->size; y++)
        for (int x = 0; x < q->size;) {
            if (!q->m[y][x]) {
                x++;
                continue;
            }
            int run = 1;
            while (x + run < q->size && q->m[y][x + run]) run++;
            buf_printf(b, "M%d %dh%dv1h-%dz", x, y, run, run);
            x += run;
        }
    buf_puts(b, "\"/></svg>");
}
