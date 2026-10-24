/* modules/av/dts_testenc.h — a DTS core frame writer for tests (dts_test.c, conv_test.c): frames that use
 * every tool the decoder knows - Huffman, block and plain codes for the subband samples, Huffman and linear
 * scale factors and bit allocations, transients, ADPCM prediction, high-frequency vectors, joint intensity,
 * the LFE, dynamic range, CRC words, a check word per subsubframe - with random contents, recording what it
 * wrote; or a plain tone, a band per channel (a steady value in band 2 ch + 1: a tone at that band's top
 * edge, (ch + 1) x 1500 Hz - a steady subband value comes out at an edge, the top one in odd bands). Not an encoder for sound: subband samples are chosen, never
 * analysed. Header-only, static, test builds only. */
#ifndef HEMMA_AV_DTS_TESTENC_H
#define HEMMA_AV_DTS_TESTENC_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dts.h"
#include "dts_tables.h"

typedef struct {
    uint8_t *p;
    size_t cap;
    long pos; /* bits */
} TencBits;

static void tenc_put(TencBits *b, int n, uint32_t v) {
    for (int i = n - 1; i >= 0; i--, b->pos++) {
        if ((size_t)(b->pos >> 3) >= b->cap) return;
        if ((v >> i) & 1u) b->p[b->pos >> 3] |= (uint8_t)(0x80u >> (b->pos & 7));
    }
}

/* a symbol's code in a book: found by walking its tree */
static bool tenc_find(const DtsHuff *h, int node, int sym, uint32_t prefix, int depth, uint32_t *code, int *len) {
    for (int bit = 0; bit < 2; bit++) {
        int v = h->node[node][bit];
        uint32_t c = prefix << 1 | (uint32_t)bit;
        if (v < 0) {
            if (-v - 1 - DTS_LEAF == sym) {
                *code = c, *len = depth + 1;
                return true;
            }
        } else if (tenc_find(h, v, sym, c, depth + 1, code, len)) {
            return true;
        }
    }
    return false;
}

static void tenc_huff(TencBits *b, const DtsHuff *h, int sym) {
    uint32_t code = 0;
    int len = 0;
    if (tenc_find(h, 0, sym, 0, 0, &code, &len)) tenc_put(b, len, code);
}

static uint32_t tenc_rand(uint32_t *s) {
    *s ^= *s << 13, *s ^= *s >> 17, *s ^= *s << 5;
    return *s;
}
static int tenc_range(uint32_t *s, int lo, int hi) { return lo + (int)(tenc_rand(s) % (uint32_t)(hi - lo + 1)); }

/* a scale factor index: a difference from the last in a Huffman book (SHUFF 0-4), or itself in 6 or 7 bits */
static void tenc_scale(TencBits *b, uint32_t *s, int shuff, int *sum, bool random) {
    int t = random ? tenc_range(s, 20, 60) : 50;
    if (shuff < 5) tenc_huff(b, &DTS_SCALES[shuff], t - *sum);
    else tenc_put(b, shuff == 5 ? 6 : 7, (uint32_t)t);
    *sum = t;
}

typedef struct {
    int amode;      /* 2 (L R) or 9 (C L R SL SR) */
    bool lfe;       /* LFF 2: 64x */
    int nblks;      /* NBLKS: blocks - 1 (15 = 512-sample frames) */
    int subframes;  /* their subsubframes split the blocks evenly: (nblks + 1) / 8 / subframes each */
    bool random;    /* every tool, random contents; else a tone: (ch + 1) x 1500 Hz in channel ch */
    bool aspf, cpf, dynf, sumf, sums;
    uint32_t seed;
} TencOpts;

typedef struct { /* what the frame used, counted as the decoder's dts_stats() counts it */
    int predicted, vq, joint, transients, huffman, block, linear;
    long abits[32];
} TencRecord;

/* One frame into out (zeroed here), cap >= 16384; its size in bytes */
static size_t dts_test_frame(const TencOpts *o, uint8_t *out, size_t cap, TencRecord *rec) {
    memset(out, 0, cap);
    if (rec) memset(rec, 0, sizeof *rec);
    TencBits b = {out, cap, 0};
    uint32_t s = o->seed ? o->seed : 1;
    const int nch = o->amode == 9 ? 5 : 2, lff = o->lfe ? 2 : 0;
    const int ssc = (o->nblks + 1) / 8 / o->subframes;
    /* frame header (Table 5-1) */
    tenc_put(&b, 32, 0x7FFE8001u);
    tenc_put(&b, 1, 1);             /* FTYPE: normal */
    tenc_put(&b, 5, 31);            /* SHORT */
    tenc_put(&b, 1, o->cpf);        /* CPF */
    tenc_put(&b, 7, (uint32_t)o->nblks);
    long fsize_at = b.pos;
    tenc_put(&b, 14, 0);            /* FSIZE: filled in at the end */
    tenc_put(&b, 6, (uint32_t)o->amode);
    tenc_put(&b, 4, 13);            /* 48 kHz */
    tenc_put(&b, 5, 24);            /* 1536 kbit/s */
    tenc_put(&b, 1, 0);             /* FixedBit */
    tenc_put(&b, 1, o->dynf);
    tenc_put(&b, 4, 0);             /* TIMEF AUXF HDCD, and EXT_AUDIO_ID's first bit */
    tenc_put(&b, 2, 0);             /* EXT_AUDIO_ID's rest */
    tenc_put(&b, 1, 0);             /* EXT_AUDIO */
    tenc_put(&b, 1, o->aspf);
    tenc_put(&b, 2, (uint32_t)lff);
    tenc_put(&b, 1, 1);             /* HFLAG */
    if (o->cpf) tenc_put(&b, 16, 0x1234);
    tenc_put(&b, 1, 0);             /* FILTS */
    tenc_put(&b, 4, 7);             /* VERNUM */
    tenc_put(&b, 2, 0);             /* CHIST */
    tenc_put(&b, 3, 6);             /* PCMR: 24 bits */
    tenc_put(&b, 1, o->sumf);
    tenc_put(&b, 1, o->sums);
    tenc_put(&b, 4, 0);             /* DIALNORM */
    /* coding header (Table 5-21) */
    int subs[5], vqsub[5], joinx[5] = {0}, thuff[5], shuff[5], bhuff[5], sel[5][10];
    for (int ch = 0; ch < nch; ch++) {
        if (o->random) {
            subs[ch] = tenc_range(&s, 8, 32);
            vqsub[ch] = tenc_range(&s, 4, subs[ch]);
            thuff[ch] = tenc_range(&s, 0, 3);
            shuff[ch] = tenc_range(&s, 0, 6);
            bhuff[ch] = tenc_range(&s, 0, 6);
            for (int q = 0; q < 10; q++) sel[ch][q] = tenc_range(&s, 0, q == 0 ? 1 : q < 5 ? 3 : 7);
        } else {
            subs[ch] = vqsub[ch] = 2 * ch + 2; /* the tone's band (2 ch + 1), the ones below it at nothing */
            thuff[ch] = 0, shuff[ch] = 5, bhuff[ch] = 6;
            for (int q = 0; q < 10; q++) sel[ch][q] = q == 0 ? 1 : q < 5 ? 3 : 7; /* block / plain codes */
        }
    }
    if (o->random && nch > 1) { /* the last channel copies its top bands from the first (joint intensity) */
        int ch = nch - 1;
        if (subs[ch] > 8) {
            if (subs[0] <= subs[ch]) subs[0] = subs[ch] + 1 > 32 ? 32 : subs[ch] + 1;
            subs[ch] = subs[0] - 3 > 2 ? subs[0] - 3 : 2;
            if (vqsub[ch] > subs[ch]) vqsub[ch] = subs[ch];
            if (vqsub[0] > subs[0]) vqsub[0] = subs[0];
            joinx[ch] = 1;
        }
    }
    tenc_put(&b, 4, (uint32_t)(o->subframes - 1));
    tenc_put(&b, 3, (uint32_t)(nch - 1));
    for (int ch = 0; ch < nch; ch++) tenc_put(&b, 5, (uint32_t)(subs[ch] - 2));
    for (int ch = 0; ch < nch; ch++) tenc_put(&b, 5, (uint32_t)(vqsub[ch] - 1));
    for (int ch = 0; ch < nch; ch++) tenc_put(&b, 3, (uint32_t)joinx[ch]);
    for (int ch = 0; ch < nch; ch++) tenc_put(&b, 2, (uint32_t)thuff[ch]);
    for (int ch = 0; ch < nch; ch++) tenc_put(&b, 3, (uint32_t)shuff[ch]);
    for (int ch = 0; ch < nch; ch++) tenc_put(&b, 3, (uint32_t)bhuff[ch]);
    for (int q = 0; q < 10; q++)
        for (int ch = 0; ch < nch; ch++) tenc_put(&b, q == 0 ? 1 : q < 5 ? 2 : 3, (uint32_t)sel[ch][q]);
    for (int q = 0; q < 10; q++)
        for (int ch = 0; ch < nch; ch++)
            if (sel[ch][q] < DTS_QUANT_BOOKS[q]) tenc_put(&b, 2, (uint32_t)tenc_range(&s, 0, 3)); /* ADJ */
    if (o->cpf) tenc_put(&b, 16, 0);
    /* subframes */
    for (int sf = 0; sf < o->subframes; sf++) {
        int abits[5][32] = {{0}}, tmode[5][32] = {{0}}, pmode[5][32] = {{0}};
        tenc_put(&b, 2, (uint32_t)(ssc - 1));
        tenc_put(&b, 3, 0); /* PSC */
        for (int ch = 0; ch < nch; ch++)
            for (int k = 0; k < subs[ch]; k++) {
                pmode[ch][k] = o->random && tenc_rand(&s) % 4 == 0;
                tenc_put(&b, 1, (uint32_t)pmode[ch][k]);
                if (rec && pmode[ch][k] && k < vqsub[ch]) rec->predicted++;
            }
        for (int ch = 0; ch < nch; ch++)
            for (int k = 0; k < subs[ch]; k++)
                if (pmode[ch][k]) tenc_put(&b, 12, (uint32_t)tenc_range(&s, 0, 4095));
        for (int ch = 0; ch < nch; ch++)
            for (int k = 0; k < vqsub[ch]; k++) {
                int a;
                if (!o->random) a = k == 2 * ch + 1 ? 9 : 0; /* 6-bit plain codes */
                else if (bhuff[ch] < 5) a = tenc_range(&s, 1, 12);
                else a = tenc_range(&s, 0, bhuff[ch] == 5 ? 15 : 24);
                abits[ch][k] = a;
                if (bhuff[ch] < 5) tenc_huff(&b, &DTS_BITALLOC[bhuff[ch]], a);
                else tenc_put(&b, bhuff[ch] == 5 ? 4 : 5, (uint32_t)a);
                if (rec) rec->abits[a] += ssc * 8;
            }
        if (ssc > 1)
            for (int ch = 0; ch < nch; ch++)
                for (int k = 0; k < vqsub[ch]; k++)
                    if (abits[ch][k]) {
                        tmode[ch][k] = o->random ? tenc_range(&s, 0, 3) : 0;
                        tenc_huff(&b, &DTS_TMODE[thuff[ch]], tmode[ch][k]);
                        if (rec && tmode[ch][k]) rec->transients++;
                    }
        for (int ch = 0; ch < nch; ch++) { /* scale factors: the bands with bits (two with a transient), then the vectors' */
            int sum = 0;
            for (int k = 0; k < vqsub[ch]; k++)
                for (int i = 0; abits[ch][k] && i < 1 + (tmode[ch][k] != 0); i++) tenc_scale(&b, &s, shuff[ch], &sum, o->random);
            for (int k = vqsub[ch]; k < subs[ch]; k++) tenc_scale(&b, &s, shuff[ch], &sum, o->random);
        }
        int jshuff[5] = {0};
        for (int ch = 0; ch < nch; ch++)
            if (joinx[ch]) {
                jshuff[ch] = tenc_range(&s, 0, 6);
                tenc_put(&b, 3, (uint32_t)jshuff[ch]);
            }
        for (int ch = 0; ch < nch; ch++)
            if (joinx[ch])
                for (int k = subs[ch]; k < subs[joinx[ch] - 1]; k++) {
                    if (jshuff[ch] < 5) tenc_huff(&b, &DTS_SCALES[jshuff[ch]], tenc_range(&s, -20, 20));
                    else tenc_put(&b, jshuff[ch] == 5 ? 6 : 7, (uint32_t)tenc_range(&s, 0, 40)); /* (read back + 64) */
                    if (rec) rec->joint++;
                }
        if (o->dynf) tenc_put(&b, 8, (uint32_t)tenc_range(&s, 0, 255));
        if (o->cpf) tenc_put(&b, 16, 0);
        for (int ch = 0; ch < nch; ch++)
            for (int k = vqsub[ch]; k < subs[ch]; k++) {
                tenc_put(&b, 10, (uint32_t)tenc_range(&s, 0, 1023));
                if (rec) rec->vq++;
            }
        if (lff) {
            for (int i = 0; i < 2 * lff * ssc; i++) tenc_put(&b, 8, (uint32_t)(uint8_t)(int8_t)(o->random ? tenc_range(&s, -100, 100) : 50));
            tenc_put(&b, 8, (uint32_t)(o->random ? tenc_range(&s, 0, 124) : 60));
        }
        for (int ss = 0; ss < ssc; ss++) {
            for (int ch = 0; ch < nch; ch++)
                for (int k = 0; k < vqsub[ch]; k++) {
                    int a = abits[ch][k];
                    if (!a) continue;
                    int sl = a <= 10 ? sel[ch][a - 1] : 0;
                    if (a <= 10 && sl < DTS_QUANT_BOOKS[a - 1]) {
                        int half = (DTS_QUANT_LEVELS[a - 1] - 1) / 2;
                        for (int m = 0; m < 8; m++) tenc_huff(&b, &DTS_QUANT[a - 1][sl], tenc_range(&s, -half, half));
                        if (rec) rec->huffman++;
                    } else if (a <= 7) {
                        static const int BITS[7] = {7, 10, 12, 13, 15, 17, 19};
                        int levels = DTS_QUANT_LEVELS[a - 1];
                        for (int blk = 0; blk < 2; blk++) {
                            uint32_t code = 0, mul = 1;
                            for (int m = 0; m < 4; m++, mul *= (uint32_t)levels) code += (uint32_t)tenc_range(&s, 0, levels - 1) * mul;
                            tenc_put(&b, BITS[a - 1], code);
                        }
                        if (rec) rec->block++;
                    } else {
                        int n = a - 3, lim = n > 8 ? 100 : (1 << (n - 1)) - 1;
                        for (int m = 0; m < 8; m++) {
                            int v = o->random ? tenc_range(&s, -lim, lim) : 20;
                            tenc_put(&b, n, (uint32_t)v & ((1u << n) - 1));
                        }
                        if (rec) rec->linear++;
                    }
                }
            if (ss == ssc - 1 || o->aspf) tenc_put(&b, 16, 0xFFFF);
        }
    }
    /* whole bytes, at least 96 (FSIZE >= 95), and the size in the header */
    size_t bytes = (size_t)((b.pos + 7) >> 3);
    if (bytes < 96) bytes = 96;
    long end = b.pos;
    b.pos = fsize_at;
    tenc_put(&b, 14, (uint32_t)(bytes - 1));
    b.pos = end;
    return bytes;
}

/* frames of a tone: n frames of 512 samples (5.1 + LFE when surround, else stereo) into out, one after another */
static size_t dts_test_tone(uint8_t *out, size_t cap, int frames, bool surround) {
    size_t at = 0;
    static uint8_t frame[16384];
    for (int f = 0; f < frames; f++) {
        TencOpts o = {.amode = surround ? 9 : 2, .lfe = surround, .nblks = 15, .subframes = 1, .seed = (uint32_t)f + 1};
        size_t n = dts_test_frame(&o, frame, sizeof frame, NULL);
        if (at + n > cap) break;
        memcpy(out + at, frame, n);
        at += n;
    }
    return at;
}

#endif
