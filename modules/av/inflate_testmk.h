/* modules/av/inflate_testmk.h — tests only: a zlib stream of any bytes, made the simple way - one fixed-Huffman block
 * of literals, a run of one byte as matches one back - for inflate.h's own tests and for the test files whose tracks
 * are zlib-packed (mkv_test.c, conv_test.c: and so the fuzzers' seeds carry one). Plus the bit writer the tests use
 * to hand-make streams that must be refused. */
#ifndef HEMMA_AV_INFLATE_TESTMK_H
#define HEMMA_AV_INFLATE_TESTMK_H

#include "inflate.h"

typedef struct {
    uint8_t *p;
    size_t n, cap;
    uint32_t acc;
    int nb;
    bool full; /* (cap ran out) */
} ZtW;

static inline void zt_bits(ZtW *w, uint32_t v, int bits) { /* the stream's own order: the lowest bit first */
    for (int i = 0; i < bits; i++) {
        w->acc |= (v >> i & 1u) << w->nb;
        if (++w->nb < 8) continue;
        if (w->n < w->cap) w->p[w->n++] = (uint8_t)w->acc;
        else w->full = true;
        w->acc = 0, w->nb = 0;
    }
}

static inline void zt_code(ZtW *w, uint32_t code, int bits) { /* a Huffman code: its highest bit first */
    for (int i = bits - 1; i >= 0; i--) zt_bits(w, code >> i & 1u, 1);
}

static inline void zt_sym(ZtW *w, int sym) { /* the fixed code's literal/length symbol */
    if (sym < 144) zt_code(w, 0x30u + (uint32_t)sym, 8);
    else if (sym < 256) zt_code(w, 0x190u + (uint32_t)(sym - 144), 9);
    else if (sym < 280) zt_code(w, (uint32_t)(sym - 256), 7);
    else zt_code(w, 0xC0u + (uint32_t)(sym - 280), 8);
}

static inline void zt_match(ZtW *w, int len) { /* len (3..258) one back */
    static const short base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    int i = 28;
    while (base[i] > len) i--;
    zt_sym(w, 257 + i);
    zt_bits(w, (uint32_t)(len - base[i]), extra[i]);
    zt_code(w, 0, 5); /* distance code 0: one back */
}

/* The header, then (block) its bits; zt_end pads to the byte and adds the Adler-32 of data. */
static inline void zt_start(ZtW *w, uint8_t *out, size_t cap) {
    *w = (ZtW){.p = out, .cap = cap};
    zt_bits(w, 0x78, 8);
    zt_bits(w, 0x01, 8);
}

static inline size_t zt_end(ZtW *w, const void *data, size_t n) {
    if (w->nb) zt_bits(w, 0, 8 - w->nb);
    uint32_t a = infl_adler(data, n);
    for (int s = 24; s >= 0; s -= 8) zt_bits(w, a >> s & 0xFF, 8);
    return w->full ? 0 : w->n;
}

/* in[0..n) zlib-packed into out; its length, 0 when cap is too small */
static inline size_t zlib_test_pack(uint8_t *out, size_t cap, const void *in, size_t n) {
    const uint8_t *d = in;
    ZtW w;
    zt_start(&w, out, cap);
    zt_bits(&w, 1, 1); /* the last block */
    zt_bits(&w, 1, 2); /* fixed codes */
    for (size_t i = 0; i < n;) {
        size_t run = 1;
        while (i + run < n && d[i + run] == d[i]) run++;
        zt_sym(&w, d[i]);
        size_t left = run - 1;
        while (left >= 3) {
            size_t k = left > 258 ? 258 : left;
            if (left - k && left - k < 3) k -= 3; /* (what's left must still make a match) */
            zt_match(&w, (int)k);
            left -= k;
        }
        while (left--) zt_sym(&w, d[i]);
        i += run;
    }
    zt_sym(&w, 256);
    return zt_end(&w, in, n);
}

#endif
