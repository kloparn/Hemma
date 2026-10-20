/* core/ed25519.c — Ed25519 (RFC 8032) for signed releases (issue #73). Ours, written after TweetNaCl
 * (Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers; public domain): small and plain - signing a
 * release in CI and checking one on a Pi, not speed. Two differences: the field's constants are worked
 * out once at first use instead of typed in (d = -121665/121666, sqrt(-1), the base point from y = 4/5),
 * and carries multiply instead of shifting negative numbers left (undefined in C; the debug build traps).
 *
 * Field elements are 16 limbs of 16 bits (in int64_t, so sums and products never overflow before a
 * carry); points are extended coordinates (X, Y, Z, T). */
#include "ed25519.h"
#include "codec.h"

typedef int64_t gf[16];

static gf gf0, gf1, D, D2, I, BX, BY;
static bool g_ready;

static void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += 65536;
        int64_t c = o[i] >> 16; /* (arithmetic: c may be below 1) */
        if (i < 15) o[i + 1] += c - 1;
        else o[0] += 38 * (c - 1); /* 2^256 = 38 (mod 2^255 - 19) */
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int b) { /* swap p and q when b is 1, in constant time */
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    set25519(t, n);
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) { /* subtract p once more if that stays above zero */
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static bool same32(const uint8_t *x, const uint8_t *y) {
    unsigned d = 0;
    for (int i = 0; i < 32; i++) d |= (unsigned)(x[i] ^ y[i]);
    return d == 0;
}

static bool eq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return same32(c, d);
}

static int par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

/* a^e for e given as 32 little-endian bytes */
static void pow25519(gf o, const gf a, const uint8_t e[32]) {
    gf c;
    set25519(c, gf1);
    for (int bit = 255; bit >= 0; bit--) {
        S(c, c);
        if ((e[bit / 8] >> (bit & 7)) & 1) M(c, c, a);
    }
    set25519(o, c);
}

static void inv25519(gf o, const gf i) { /* i^(p - 2) */
    gf c;
    set25519(c, i);
    for (int a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    set25519(o, c);
}

static void pow2523(gf o, const gf i) { /* i^((p - 5) / 8) */
    gf c;
    set25519(c, i);
    for (int a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    set25519(o, c);
}

static void add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gf p[4], gf q[4], int b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pack(uint8_t *r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static void scalarmult(gf p[4], gf q[4], const uint8_t *s) {
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; i--) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(gf p[4], const uint8_t *s) {
    gf q[4];
    set25519(q[0], BX);
    set25519(q[1], BY);
    set25519(q[2], gf1);
    M(q[3], BX, BY);
    scalarmult(p, q, s);
}

/* The point an encoding names, negated (x of the other parity): -A for checking a signature. */
static bool unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);
    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);
    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);
    S(chk, r[0]);
    M(chk, chk, den);
    if (!eq25519(chk, num)) M(r[0], r[0], I);
    S(chk, r[0]);
    M(chk, chk, den);
    if (!eq25519(chk, num)) return false; /* not on the curve */
    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return true;
}

static void init_constants(void) {
    if (g_ready) return;
    memset(gf0, 0, sizeof gf0);
    memset(gf1, 0, sizeof gf1);
    gf1[0] = 1;
    gf a = {0xdb41, 1}, b = {0xdb42, 1}; /* 121665, 121666 */
    gf ib;
    inv25519(ib, b);
    M(D, a, ib);
    Z(D, gf0, D); /* d = -121665/121666 */
    A(D2, D, D);
    static const uint8_t quarter[32] = {0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f}; /* (p - 1) / 4 */
    gf two = {2};
    pow25519(I, two, quarter); /* sqrt(-1) = 2^((p - 1) / 4) */
    uint8_t base[32]; /* the base point's encoding: y = 4/5, x even */
    base[0] = 0x58;
    memset(base + 1, 0x66, 31);
    gf q[4];
    unpackneg(q, base);
    Z(BX, gf0, q[0]);
    set25519(BY, q[1]);
    g_ready = true;
}

/* ------------------------------------------------------- scalars mod L */

static const int64_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                              0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                              0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

static void modL(uint8_t *r, int64_t x[64]) {
    int64_t carry;
    for (int i = 63; i >= 32; i--) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++) x[j] -= carry * L[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = r[i];
    memset(r, 0, 64);
    modL(r, x);
}

static bool below_L(const uint8_t s[32]) { /* little-endian compare, from the top byte */
    for (int i = 31; i >= 0; i--) {
        if (s[i] < L[i]) return true;
        if (s[i] > L[i]) return false;
    }
    return false; /* (equal) */
}

/* ------------------------------------------------------------- the API */

static void expand(const uint8_t seed[32], uint8_t h[64]) {
    sha512(seed, 32, h);
    h[0] &= 248;
    h[31] &= 127;
    h[31] |= 64;
}

void ed25519_public(const uint8_t seed[32], uint8_t pub[32]) {
    init_constants();
    uint8_t h[64];
    gf p[4];
    expand(seed, h);
    scalarbase(p, h);
    pack(pub, p);
}

void ed25519_sign(const uint8_t seed[32], const void *msg, size_t n, uint8_t sig[64]) {
    init_constants();
    uint8_t h[64], pub[32], r[64], k[64];
    gf p[4];
    expand(seed, h);
    scalarbase(p, h);
    pack(pub, p);
    Sha512 s;
    sha512_init(&s); /* r = H(the seed's second half, msg) */
    sha512_update(&s, h + 32, 32);
    sha512_update(&s, msg, n);
    sha512_final(&s, r);
    reduce(r);
    scalarbase(p, r);
    pack(sig, p); /* R */
    sha512_init(&s); /* k = H(R, A, msg) */
    sha512_update(&s, sig, 32);
    sha512_update(&s, pub, 32);
    sha512_update(&s, msg, n);
    sha512_final(&s, k);
    reduce(k);
    int64_t x[64] = {0}; /* S = r + k * a (mod L) */
    for (int i = 0; i < 32; i++) x[i] = r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++) x[i + j] += (int64_t)k[i] * h[j];
    modL(sig + 32, x);
}

bool ed25519_verify(const uint8_t pub[32], const void *msg, size_t n, const uint8_t sig[64]) {
    init_constants();
    gf p[4], q[4];
    if (!below_L(sig + 32) || !unpackneg(q, pub)) return false;
    uint8_t k[64], t[32];
    Sha512 s;
    sha512_init(&s);
    sha512_update(&s, sig, 32);
    sha512_update(&s, pub, 32);
    sha512_update(&s, msg, n);
    sha512_final(&s, k);
    reduce(k);
    scalarmult(p, q, k); /* -k*A */
    scalarbase(q, sig + 32); /* S*B */
    add(p, q);
    pack(t, p);
    return same32(sig, t); /* = R */
}
