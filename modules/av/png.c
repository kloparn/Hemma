/* modules/av/png.c — png_indexed() (declared in subpic.h): an indexed-colour PNG of a subtitle picture.
 *
 * Signature, IHDR (colour type 3, 8 bits), PLTE, tRNS (alpha per entry, only as far as the last entry that
 * isn't opaque), one IDAT, IEND; a CRC-32 per chunk. IDAT is zlib (0x78 0x01) around ONE deflate block
 * with the fixed Huffman codes, whose only matches are runs (distance 1) - a subtitle picture is mostly
 * long runs of clear pixels, so that's most of what deflate would find - then the Adler-32. Each row
 * starts with filter byte 0 (none). Pure C, no globals. */
#include "subpic.h"

#include <stdlib.h>
#include <string.h>

#define PNG_MAX_SIDE 16384

typedef struct {
    uint8_t *p;
    size_t n;
    uint32_t bits; /* not yet written, the oldest in the low bits */
    int nbits;
} Bits;

static void put_bits(Bits *b, uint32_t v, int n) { /* deflate's order: least significant bit first */
    b->bits |= v << b->nbits;
    b->nbits += n;
    while (b->nbits >= 8) {
        b->p[b->n++] = (uint8_t)b->bits;
        b->bits >>= 8;
        b->nbits -= 8;
    }
}

static void put_code(Bits *b, uint32_t code, int n) { /* a Huffman code: its most significant bit first */
    uint32_t r = 0;
    for (int i = 0; i < n; i++) r = r << 1 | (code >> i & 1);
    put_bits(b, r, n);
}

/* A literal/length symbol in the fixed code: 0-143 8 bits from 0x30, 144-255 9 bits from 0x190,
   256-279 7 bits from 0, 280-287 8 bits from 0xC0 */
static void put_sym(Bits *b, int v) {
    if (v < 144) put_code(b, 0x30u + (uint32_t)v, 8);
    else if (v < 256) put_code(b, 0x190u + (uint32_t)(v - 144), 9);
    else if (v < 280) put_code(b, (uint32_t)(v - 256), 7);
    else put_code(b, 0xC0u + (uint32_t)(v - 280), 8);
}

/* A run: len (3..258) copies of the byte before, i.e. a match at distance 1 */
static void put_run(Bits *b, size_t len) {
    static const uint16_t base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    int k = 28;
    while (base[k] > len) k--;
    put_sym(b, 257 + k);
    put_bits(b, (uint32_t)(len - base[k]), extra[k]);
    put_code(b, 0, 5); /* distance code 0 = 1, no extra bits */
}

static void be32_at(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* A chunk of n data bytes already at out + at + 8: its length, type and CRC around them; the next offset */
static size_t chunk(uint8_t *out, size_t at, const char *type, size_t n, const uint32_t *crc_tab) {
    be32_at(out + at, (uint32_t)n);
    memcpy(out + at + 4, type, 4);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n + 4; i++) c = crc_tab[(c ^ out[at + 4 + i]) & 0xFF] ^ c >> 8;
    be32_at(out + at + 8 + n, c ^ 0xFFFFFFFFu);
    return at + 12 + n;
}

uint8_t *png_indexed(const uint8_t *px, int w, int h, const uint32_t *pal, int ncolors, size_t *len) {
    if (len) *len = 0;
    if (!px || !len || w < 1 || h < 1 || w > PNG_MAX_SIDE || h > PNG_MAX_SIDE) return NULL;
    size_t npx = (size_t)w * (size_t)h, raw_n = (size_t)h * ((size_t)w + 1);
    int n = ncolors < 1 ? 1 : ncolors > 256 ? 256 : ncolors;
    for (size_t i = 0; i < npx; i++) /* (every value in px needs its entry) */
        if (px[i] >= n) n = px[i] + 1;
    uint32_t ent[256];
    for (int i = 0; i < 256; i++) ent[i] = pal && i < ncolors ? pal[i] : 0;
    int ntrns = 0;
    for (int i = 0; i < n; i++)
        if ((ent[i] & 0xFF) != 0xFF) ntrns = i + 1;

    /* the rows, each after its filter byte */
    uint8_t *raw = malloc(raw_n);
    if (!raw) return NULL;
    for (size_t y = 0; y < (size_t)h; y++) {
        raw[y * ((size_t)w + 1)] = 0;
        memcpy(raw + y * ((size_t)w + 1) + 1, px + y * (size_t)w, (size_t)w);
    }
    /* at most 9 bits a byte (a literal), plus the block's 3 header bits and 7 for its end */
    size_t zmax = 2 + (raw_n * 9 + 10 + 7) / 8 + 1 + 4;
    size_t cap = 8 + (12 + 13) + (12 + 3 * (size_t)n) + (12 + (size_t)ntrns) + (12 + zmax) + 12;
    uint8_t *out = malloc(cap);
    if (!out) {
        free(raw);
        return NULL;
    }
    uint32_t crc_tab[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
        crc_tab[i] = c;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(out, sig, 8);
    size_t at = 8;
    uint8_t *d = out + at + 8; /* IHDR: size, 8 bits, colour type 3 (indexed), deflate, no filter, no interlace */
    be32_at(d, (uint32_t)w), be32_at(d + 4, (uint32_t)h);
    d[8] = 8, d[9] = 3, d[10] = 0, d[11] = 0, d[12] = 0;
    at = chunk(out, at, "IHDR", 13, crc_tab);
    d = out + at + 8;
    for (int i = 0; i < n; i++)
        d[3 * i] = (uint8_t)(ent[i] >> 24), d[3 * i + 1] = (uint8_t)(ent[i] >> 16), d[3 * i + 2] = (uint8_t)(ent[i] >> 8);
    at = chunk(out, at, "PLTE", 3 * (size_t)n, crc_tab);
    if (ntrns) {
        d = out + at + 8;
        for (int i = 0; i < ntrns; i++) d[i] = (uint8_t)ent[i];
        at = chunk(out, at, "tRNS", (size_t)ntrns, crc_tab);
    }
    /* IDAT: zlib header (deflate, 32 KB window; 0x7801 is a multiple of 31), one final fixed block */
    d = out + at + 8;
    d[0] = 0x78, d[1] = 0x01;
    Bits b = {d + 2, 0, 0, 0};
    put_bits(&b, 1, 1); /* BFINAL */
    put_bits(&b, 1, 2); /* BTYPE 01: fixed Huffman codes */
    for (size_t i = 0; i < raw_n;) {
        uint8_t v = raw[i++];
        put_sym(&b, v);
        size_t run = 0;
        while (i + run < raw_n && raw[i + run] == v) run++;
        while (run >= 3) { /* (fewer than 3 left: literals, next time round) */
            size_t m = run < 258 ? run : 258;
            put_run(&b, m);
            i += m, run -= m;
        }
    }
    put_sym(&b, 256); /* end of block */
    if (b.nbits) put_bits(&b, 0, 8 - b.nbits);
    uint32_t s1 = 1, s2 = 0; /* Adler-32 of the rows */
    for (size_t i = 0; i < raw_n;) {
        size_t k = raw_n - i < 5552 ? raw_n - i : 5552; /* (no overflow before the modulo) */
        for (size_t e = i + k; i < e; i++) s1 += raw[i], s2 += s1;
        s1 %= 65521, s2 %= 65521;
    }
    be32_at(b.p + b.n, s2 << 16 | s1);
    at = chunk(out, at, "IDAT", 2 + b.n + 4, crc_tab);
    at = chunk(out, at, "IEND", 0, crc_tab);
    free(raw);
    *len = at;
    return out;
}
