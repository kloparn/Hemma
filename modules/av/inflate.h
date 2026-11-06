/* modules/av/inflate.h — zlib's format undone: RFC 1950 around RFC 1951's deflate (2.9.1). Header-only (static
 * functions, as idct.h is): mkv.c includes it.
 *
 * Why: Matroska lets a muxer pack a track's frames with zlib (ContentCompression algorithm 0) - mkvmerge did that to
 * subtitle tracks by default for years. A Blu-ray release's PGS track came so (2026-11-06) and Hemma left it out
 * without a word: the upload said its subtitles would go along, none did. mkv.c now unpacks such frames with this,
 * so everything reading a Matroska file (the converter here and in the browser, the subtitle job) sees them plain.
 *
 * Ours, after the RFCs: stored, fixed and dynamic Huffman blocks; canonical codes decoded from their counts per
 * length, a bit at a time (the way Mark Adler's puff.c explains it) - frames are small: a subtitle's display set.
 * Code sets are checked as zlib checks them: the code-length code complete, a literal/length or distance code
 * incomplete only as a lone one-bit code. Refused: a distance before the output's start, codes 286/287 and distances
 * 30/31, a stored block whose NLEN isn't LEN's complement, a preset dictionary, a wrong Adler-32, a stream cut short.
 * The output grows by realloc up to `max` bytes: a stream that would make more is refused, so a "zip bomb" costs
 * `max` at most. Every loop eats input, so damaged data ends. Pure: libc only, no mutable globals. */
#ifndef HEMMA_AV_INFLATE_H
#define HEMMA_AV_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *in;
    size_t n, at;  /* the input; the next byte to read */
    uint32_t bits; /* read ahead, the next bit lowest */
    int nbits;     /* (< 8 between calls) */
    uint8_t *out;
    size_t len, cap, max;
    bool bad; /* the input ended */
} Infl;

typedef struct {
    short count[16];   /* codes of each length (count[0]: symbols without one) */
    short symbol[288]; /* the symbols in code order */
} InflCode;

static inline uint32_t infl_bits(Infl *s, int need) { /* 0..16 bits, the first read lowest */
    while (s->nbits < need) {
        if (s->at >= s->n) {
            s->bad = true;
            return 0;
        }
        s->bits |= (uint32_t)s->in[s->at++] << s->nbits;
        s->nbits += 8;
    }
    uint32_t v = s->bits & ((1u << need) - 1);
    s->bits >>= need;
    s->nbits -= need;
    return v;
}

static inline bool infl_room(Infl *s, size_t more) {
    if (more > s->max - s->len) return false;
    if (more <= s->cap - s->len) return true;
    size_t c = s->cap ? s->cap : 4096;
    while (c - s->len < more) c = c > s->max / 2 ? s->max : c * 2;
    uint8_t *p = realloc(s->out, c);
    if (!p) return false;
    s->out = p;
    s->cap = c;
    return true;
}

/* The code of lens[0..n) (each 0..15): 0 = complete, > 0 = incomplete, < 0 = over-subscribed. No codes at all
   counts as complete - nothing decodes then. */
static inline int infl_build(InflCode *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = left * 2 - h->count[l];
        if (left < 0) return left;
    }
    short at[16];
    at[1] = 0;
    for (int l = 1; l < 15; l++) at[l + 1] = (short)(at[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (lens[i]) h->symbol[at[lens[i]]++] = (short)i;
    return left;
}

static inline int infl_decode(Infl *s, const InflCode *h) { /* the next symbol, -1 = none (or the input ended) */
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)infl_bits(s, 1);
        if (s->bad) return -1;
        int count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) * 2;
        code *= 2;
    }
    return -1;
}

static inline bool infl_stored(Infl *s) {
    s->bits = 0, s->nbits = 0; /* (to the byte) */
    if (s->n - s->at < 4) return false;
    const uint8_t *p = s->in + s->at;
    size_t len = (size_t)(p[0] | p[1] << 8), nlen = (size_t)(p[2] | p[3] << 8);
    s->at += 4;
    if (len != (~nlen & 0xFFFF) || s->n - s->at < len || !infl_room(s, len)) return false;
    if (len) memcpy(s->out + s->len, s->in + s->at, len);
    s->at += len;
    s->len += len;
    return true;
}

static inline bool infl_codes(Infl *s, const InflCode *lit, const InflCode *dist) {
    static const short base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const unsigned short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,    65,    97,    129,
                                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dextra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = infl_decode(s, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (!infl_room(s, 1)) return false;
            s->out[s->len++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        size_t len = (size_t)base[sym] + infl_bits(s, extra[sym]);
        int d = infl_decode(s, dist);
        if (d < 0 || d >= 30) return false;
        size_t back = (size_t)dbase[d] + infl_bits(s, dextra[d]);
        if (s->bad || back > s->len || !infl_room(s, len)) return false;
        uint8_t *o = s->out + s->len;
        for (size_t i = 0; i < len; i++) o[i] = o[(ptrdiff_t)i - (ptrdiff_t)back]; /* (may overlap: a run) */
        s->len += len;
    }
}

static inline bool infl_fixed(Infl *s) {
    uint8_t lens[288];
    InflCode lit, dist;
    for (int i = 0; i < 288; i++) lens[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    infl_build(&lit, lens, 288);
    for (int i = 0; i < 30; i++) lens[i] = 5;
    infl_build(&dist, lens, 30);
    return infl_codes(s, &lit, &dist);
}

static inline bool infl_dynamic(Infl *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = (int)infl_bits(s, 5) + 257, ndist = (int)infl_bits(s, 5) + 1, ncode = (int)infl_bits(s, 4) + 4;
    if (s->bad || nlen > 286 || ndist > 30) return false;
    uint8_t lens[286 + 30] = {0};
    for (int i = 0; i < ncode; i++) lens[order[i]] = (uint8_t)infl_bits(s, 3);
    InflCode lc, lit, dist;
    if (s->bad || infl_build(&lc, lens, 19)) return false; /* (complete or nothing) */
    for (int i = 0; i < nlen + ndist;) {
        int sym = infl_decode(s, &lc), rep;
        uint8_t v = 0;
        if (sym < 0) return false;
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (!i) return false;
            v = lens[i - 1];
            rep = 3 + (int)infl_bits(s, 2);
        } else if (sym == 17) rep = 3 + (int)infl_bits(s, 3);
        else rep = 11 + (int)infl_bits(s, 7);
        if (s->bad || rep > nlen + ndist - i) return false;
        while (rep--) lens[i++] = v;
    }
    if (!lens[256]) return false; /* no end-of-block code */
    int e = infl_build(&lit, lens, nlen);
    if (e < 0 || (e > 0 && nlen != lit.count[0] + lit.count[1])) return false;
    e = infl_build(&dist, lens + nlen, ndist);
    if (e < 0 || (e > 0 && ndist != dist.count[0] + dist.count[1])) return false;
    return infl_codes(s, &lit, &dist);
}

static inline uint32_t infl_adler(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552; /* (the most that can't overflow before the modulo) */
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* in[0..n) as a zlib stream -> *out, grown by realloc (*out and *cap stay the caller's to reuse and free, whatever the
   answer; *out is never left NULL on success), *len = how much it made. False: not a zlib stream, damaged, cut short,
   or more than max bytes. */
static inline bool inflate_zlib(const uint8_t *in, size_t n, uint8_t **out, size_t *cap, size_t max, size_t *len) {
    Infl s = {.in = in, .n = n, .at = 2, .out = *out, .cap = *cap, .max = max};
    *len = 0;
    bool ok = n >= 6 && (in[0] & 0x0F) == 8 && in[0] >> 4 <= 7 && (in[0] << 8 | in[1]) % 31 == 0 && !(in[1] & 0x20);
    if (ok && !s.cap) { /* (an empty result still points somewhere) */
        ok = (s.out = malloc(4096)) != NULL;
        s.cap = ok ? 4096 : 0;
    }
    for (bool last = false; ok && !last;) {
        last = infl_bits(&s, 1);
        int type = (int)infl_bits(&s, 2);
        ok = !s.bad && (type == 0 ? infl_stored(&s) : type == 1 ? infl_fixed(&s) : type == 2 && infl_dynamic(&s));
    }
    if (ok) { /* the Adler-32 of what came out, big-endian, from the next whole byte */
        const uint8_t *a = in + s.at;
        ok = n - s.at >= 4 && ((uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]) == infl_adler(s.out, s.len);
    }
    *out = s.out;
    *cap = s.cap;
    if (ok) *len = s.len;
    return ok;
}

#endif
