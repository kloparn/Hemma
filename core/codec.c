/* core/codec.c — see codec.h */
#include "codec.h"

/* ------------------------------------------------------------------ SHA-1 */

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e;
}

void sha1_init(Sha1 *s) {
    static const uint32_t iv[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->fill = 0;
}

void sha1_update(Sha1 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->total += n;
    while (n) {
        size_t take = MIN(n, 64 - s->fill);
        memcpy(s->block + s->fill, p, take);
        s->fill += take, p += take, n -= take;
        if (s->fill == 64) {
            sha1_block(s, s->block);
            s->fill = 0;
        }
    }
}

void sha1_final(Sha1 *s, uint8_t out[20]) {
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->fill != 56) sha1_update(s, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void sha1(const void *data, size_t n, uint8_t out[20]) {
    Sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, n);
    sha1_final(&s, out);
}

/* ---------------------------------------------------------------- SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static void sha256_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d;
    s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

void sha256_init(Sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->fill = 0;
}

void sha256_update(Sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->total += n;
    if (s->fill) {
        size_t take = MIN(n, 64 - s->fill);
        memcpy(s->block + s->fill, p, take);
        s->fill += take, p += take, n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->block);
            s->fill = 0;
        }
    }
    while (n >= 64) {
        sha256_block(s, p);
        p += 64, n -= 64;
    }
    if (n) {
        memcpy(s->block, p, n);
        s->fill = n;
    }
}

void sha256_final(Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->total * 8;
    uint8_t pad[72] = {0x80};
    size_t padlen = (s->fill < 56) ? 56 - s->fill : 120 - s->fill;
    for (int i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, pad, padlen + 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void sha256(const void *data, size_t n, uint8_t out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}

void sha256_hex(const void *data, size_t n, char out[65]) {
    uint8_t d[32];
    sha256(data, n, d);
    hex_encode(d, 32, out);
}

/* HMAC with the key's inner/outer states prepared once (PBKDF2 reuses them). */
typedef struct {
    Sha256 inner, outer;
} Hmac;

static void hmac_init(Hmac *h, const void *key, size_t klen) {
    uint8_t k[64] = {0}, pad[64];
    if (klen > 64) sha256(key, klen, k);
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&h->inner);
    sha256_update(&h->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&h->outer);
    sha256_update(&h->outer, pad, 64);
}

static void hmac_run(const Hmac *h, const void *m1, size_t n1, const void *m2, size_t n2, uint8_t out[32]) {
    Sha256 s = h->inner;
    sha256_update(&s, m1, n1);
    if (n2) sha256_update(&s, m2, n2);
    uint8_t ih[32];
    sha256_final(&s, ih);
    s = h->outer;
    sha256_update(&s, ih, 32);
    sha256_final(&s, out);
}

void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, uint8_t out[32]) {
    Hmac h;
    hmac_init(&h, key, klen);
    hmac_run(&h, msg, mlen, NULL, 0, out);
}

void pbkdf2_sha256(const void *pass, size_t plen, const void *salt, size_t slen, uint32_t iterations, uint8_t *out,
                   size_t outlen) {
    Hmac h;
    hmac_init(&h, pass, plen);
    for (uint32_t block = 1; outlen; block++) {
        uint8_t be[4] = {(uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8), (uint8_t)block};
        uint8_t u[32], t[32];
        hmac_run(&h, salt, slen, be, 4, u);
        memcpy(t, u, 32);
        for (uint32_t i = 1; i < iterations; i++) {
            hmac_run(&h, u, 32, NULL, 0, u);
            for (int k = 0; k < 32; k++) t[k] ^= u[k];
        }
        size_t take = MIN(outlen, 32);
        memcpy(out, t, take);
        out += take, outlen -= take;
    }
}

bool ct_equal(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff == 0;
}

/* ----------------------------------------------------------------- Base64 */

size_t base64_encode(const void *data, size_t n, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = data;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* -------------------------------------------------------------------- URL */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char *hex, uint8_t *out, size_t n) {
    if (!hex || strlen(hex) != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

size_t url_decode(const char *s, size_t n, char *out, bool plus_space) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out[o++] = (plus_space && c == '+') ? ' ' : c;
    }
    out[o] = '\0';
    return o;
}

void url_encode(Buf *b, const char *s, bool keep_slash) {
    static const char hx[] = "0123456789ABCDEF";
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || (keep_slash && c == '/')) {
            buf_putc(b, (char)c);
        } else {
            char e[3] = {'%', hx[c >> 4], hx[c & 15]};
            buf_put(b, e, 3);
        }
    }
}
