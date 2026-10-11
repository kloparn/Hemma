/* modules/av/flac_test.c — FLAC encoder: CRC and MD5 vectors, and lossless
 * round trips through a small independent decoder written here from RFC 9639
 * (noise, sine, silence, square waves, 24-bit, mono, 5.1, odd block sizes and
 * sample rates, wasted bits, escapes). Chrome's decoder is the second opinion
 * (tools/muxcheck.c). */
#include "../../tests/test.h"
#include "flac.h"

#include <math.h>

/* ------------------------------------------------------ reference decoder */

typedef struct {
    const uint8_t *p;
    size_t n, pos; /* pos in bits */
    bool err;
} Br;

static uint32_t rd(Br *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (b->pos >= b->n * 8) {
            b->err = true;
            return 0;
        }
        v = v << 1 | (uint32_t)(b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1);
        b->pos++;
    }
    return v;
}

static int32_t rds(Br *b, int n) { /* signed, n <= 32 */
    if (!n) return 0;
    uint32_t v = rd(b, n);
    if (n < 32 && (v >> (n - 1)) & 1) v |= ~0u << n;
    return (int32_t)v;
}

static uint32_t unary(Br *b) {
    uint32_t q = 0;
    while (!b->err && !rd(b, 1)) q++;
    return q;
}

static bool residual(Br *b, int n, int order, int32_t *r) {
    int method = (int)rd(b, 2);
    if (method > 1) return false;
    int po = (int)rd(b, 4), pbits = method ? 5 : 4;
    uint32_t esc = method ? 31 : 15;
    int np = 1 << po, sz = n >> po;
    if ((n & (np - 1)) || sz < order) return false;
    int i = order;
    for (int p = 0; p < np; p++) {
        int cnt = p ? sz : sz - order;
        uint32_t k = rd(b, pbits);
        if (k == esc) {
            int raw = (int)rd(b, 5);
            for (int j = 0; j < cnt; j++) r[i++] = rds(b, raw);
        } else {
            for (int j = 0; j < cnt; j++) {
                uint32_t q = unary(b);
                uint32_t u = (q << k) | rd(b, (int)k);
                r[i++] = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            }
        }
    }
    return !b->err;
}

static bool subframe(Br *b, int n, int bps, int32_t *x) {
    if (rd(b, 1)) return false;
    int type = (int)rd(b, 6), wasted = 0;
    if (rd(b, 1)) wasted = (int)unary(b) + 1;
    bps -= wasted;
    if (type == 0) {
        int32_t v = rds(b, bps);
        for (int i = 0; i < n; i++) x[i] = v;
    } else if (type == 1) {
        for (int i = 0; i < n; i++) x[i] = rds(b, bps);
    } else if (type >= 8 && type <= 12) {
        int order = type - 8;
        for (int i = 0; i < order; i++) x[i] = rds(b, bps);
        if (!residual(b, n, order, x)) return false;
        for (int i = order; i < n; i++) {
            int64_t p = 0;
            switch (order) {
            case 1: p = x[i - 1]; break;
            case 2: p = 2LL * x[i - 1] - x[i - 2]; break;
            case 3: p = 3LL * x[i - 1] - 3LL * x[i - 2] + x[i - 3]; break;
            case 4: p = 4LL * x[i - 1] - 6LL * x[i - 2] + 4LL * x[i - 3] - x[i - 4]; break;
            }
            x[i] = (int32_t)(x[i] + p);
        }
    } else if (type >= 32) {
        int order = type - 31;
        for (int i = 0; i < order; i++) x[i] = rds(b, bps);
        int prec = (int)rd(b, 4) + 1;
        int shift = rds(b, 5);
        if (prec == 16 || shift < 0) return false;
        int32_t c[32];
        for (int i = 0; i < order; i++) c[i] = rds(b, prec);
        if (!residual(b, n, order, x)) return false;
        for (int i = order; i < n; i++) {
            int64_t s = 0;
            for (int j = 0; j < order; j++) s += (int64_t)c[j] * x[i - 1 - j];
            x[i] = (int32_t)(x[i] + (s >> shift));
        }
    } else return false;
    if (wasted)
        for (int i = 0; i < n; i++) x[i] = (int32_t)((uint32_t)x[i] << wasted);
    return !b->err;
}

/* One frame at p; interleaved samples out. Returns its size, or 0. */
static size_t decode_frame(const uint8_t *p, size_t len, int stream_bps, int32_t *out, int *ns, int *nch, int *rate_code) {
    Br b = {p, len, 0, false};
    if (rd(&b, 14) != 0x3FFE || rd(&b, 1) || rd(&b, 1)) return 0;
    int bsc = (int)rd(&b, 4), rc = (int)rd(&b, 4), ca = (int)rd(&b, 4), sc = (int)rd(&b, 3);
    if (rd(&b, 1)) return 0;
    uint32_t c0 = rd(&b, 8); /* UTF-8 frame number */
    int extra = c0 < 0x80 ? 0 : c0 < 0xE0 ? 1 : c0 < 0xF0 ? 2 : c0 < 0xF8 ? 3 : c0 < 0xFC ? 4 : c0 < 0xFE ? 5 : 6;
    for (int i = 0; i < extra; i++)
        if ((rd(&b, 8) & 0xC0) != 0x80) return 0;
    int n = bsc == 1 ? 192 : bsc <= 5 ? 576 << (bsc - 2) : bsc == 6 ? (int)rd(&b, 8) + 1 : bsc == 7 ? (int)rd(&b, 16) + 1 : 256 << (bsc - 8);
    if (rc == 12) rd(&b, 8);
    else if (rc == 13 || rc == 14) rd(&b, 16);
    size_t hdr = b.pos / 8;
    if (rd(&b, 8) != flac_crc8(p, hdr)) return 0;
    static const int bits_of[8] = {0, 8, 12, 0, 16, 20, 24, 32};
    int bps = sc ? bits_of[sc] : stream_bps;
    int ch = ca < 8 ? ca + 1 : 2;
    static int32_t x[8][65536];
    for (int c = 0; c < ch; c++) {
        int sb = bps + ((ca == 8 && c == 1) || (ca == 9 && c == 0) || (ca == 10 && c == 1));
        if (!subframe(&b, n, sb, x[c])) return 0;
    }
    if (b.pos & 7) b.pos += 8 - (b.pos & 7);
    size_t body = b.pos / 8;
    if (rd(&b, 16) != flac_crc16(p, body) || b.err) return 0;
    for (int i = 0; i < n; i++) {
        int32_t l = x[0][i], r = ch > 1 ? x[1][i] : 0;
        if (ca == 8) r = l - r;           /* left/side */
        else if (ca == 9) l = l + r;      /* side/right: x0 = side */
        else if (ca == 10) {              /* mid/side */
            int32_t mid = (int32_t)((uint32_t)l << 1) | (r & 1), side = r;
            l = (mid + side) >> 1;
            r = (mid - side) >> 1;
        }
        for (int c = 0; c < ch; c++) out[(size_t)i * (size_t)ch + (size_t)c] = c == 0 ? l : c == 1 ? r : x[c][i];
    }
    *ns = n, *nch = ch;
    if (rate_code) *rate_code = rc;
    return body + 2;
}

/* ------------------------------------------------------------ test kit */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    int frames;
} Out;

static void sink(void *ctx, const uint8_t *frame, size_t n, int samples) {
    Out *o = ctx;
    (void)samples;
    if (o->len + n > o->cap) {
        o->cap = (o->len + n) * 2;
        o->p = realloc(o->p, o->cap);
    }
    memcpy(o->p + o->len, frame, n);
    o->len += n;
    o->frames++;
}

static uint32_t rng = 12345;
static int32_t noise(int bits) {
    rng = rng * 1103515245u + 12345u;
    uint32_t v = (rng >> 1) ^ (rng << 13);
    return (int32_t)(v % (1u << bits)) - (1 << (bits - 1));
}

/* Encode pcm (n per channel), decode it back, compare; returns compressed bytes or 0. */
static size_t round_trip(const int32_t *pcm, int n, int ch, int bits, int rate, int block, int lpc, int *rate_code) {
    FlacConfig c = {rate, ch, bits, block, lpc};
    Flac *f = flac_new(&c);
    if (!f) return 0;
    Out o = {0};
    if (!flac_write(f, pcm, n, sink, &o) || !flac_finish(f, sink, &o)) {
        flac_free(f);
        return 0;
    }
    uint8_t si[FLAC_STREAMINFO_BYTES];
    flac_streaminfo(f, si);
    uint64_t total = (uint64_t)(si[13] & 0xF) << 32 | (uint64_t)si[14] << 24 | (uint64_t)si[15] << 16 | (uint64_t)si[16] << 8 | si[17];
    int si_rate = si[10] << 12 | si[11] << 4 | si[12] >> 4, si_ch = (si[12] >> 1 & 7) + 1, si_bits = ((si[12] & 1) << 4 | si[13] >> 4) + 1;
    /* MD5 of the input as little-endian bytes */
    int bw = (bits + 7) / 8;
    uint8_t *bytes = malloc((size_t)n * (size_t)ch * (size_t)bw + 1);
    size_t nb = 0;
    for (size_t i = 0; i < (size_t)n * (size_t)ch; i++)
        for (int k = 0; k < bw; k++) bytes[nb++] = (uint8_t)((uint32_t)pcm[i] >> (8 * k));
    uint8_t md5[16];
    flac_md5(bytes, nb, md5);
    free(bytes);
    bool ok = total == (uint64_t)n && si_rate == rate && si_ch == ch && si_bits == bits && memcmp(md5, si + 18, 16) == 0;
    int32_t *dec = malloc((size_t)block * (size_t)ch * sizeof *dec);
    size_t pos = 0;
    int got = 0;
    while (ok && pos < o.len) {
        int fn = 0, fch = 0;
        size_t used = decode_frame(o.p + pos, o.len - pos, bits, dec, &fn, &fch, rate_code);
        if (!used || fch != ch || got + fn > n) ok = false;
        else {
            if (memcmp(dec, pcm + (size_t)got * (size_t)ch, (size_t)fn * (size_t)ch * sizeof *dec) != 0) ok = false;
            got += fn;
            pos += used;
        }
    }
    ok = ok && got == n;
    size_t size = o.len;
    free(dec);
    free(o.p);
    flac_free(f);
    return ok ? size : 0;
}

/* ----------------------------------------------------------------- tests */

TEST(flac_crc_and_md5_vectors) {
    const uint8_t *s = (const uint8_t *)"123456789";
    CHECK_INT(flac_crc8(s, 9), 0xF4);    /* CRC-8 (poly 0x07, init 0) */
    CHECK_INT(flac_crc16(s, 9), 0xFEE8); /* CRC-16/UMTS (poly 0x8005, init 0) */
    static const struct {
        const char *in, *hex;
    } md5[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"The quick brown fox jumps over the lazy dog", "9e107d9d372bb6826bd81d3542a419d6"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    for (int i = 0; i < (int)countof(md5); i++) {
        uint8_t d[16];
        char hex[33];
        flac_md5(md5[i].in, strlen(md5[i].in), d);
        for (int k = 0; k < 16; k++) snprintf(hex + 2 * k, 3, "%02x", d[k]);
        CHECK_STR(hex, md5[i].hex);
    }
}

TEST(flac_round_trips_are_bit_exact) {
    enum { N = 4096 * 5 + 1234 };
    static int32_t pcm[N * 6];
    /* white noise, 16-bit stereo: incompressible, must still survive */
    for (int i = 0; i < N * 2; i++) pcm[i] = noise(16);
    size_t sz = round_trip(pcm, N, 2, 16, 48000, 4096, 0, NULL);
    CHECK(sz > 0);
    CHECK(sz <= (size_t)N * 2 * 2 + 4000); /* never much bigger than the raw samples */
    /* a sine at -6 dB, slightly different in each channel (mid/side territory) */
    for (int i = 0; i < N; i++) {
        double t = (double)i / 48000.0;
        pcm[2 * i] = (int32_t)lrint(16000.0 * sin(2 * M_PI * 440 * t));
        pcm[2 * i + 1] = (int32_t)lrint(16000.0 * sin(2 * M_PI * 440 * t + 0.01)) + (noise(4) >> 2);
    }
    sz = round_trip(pcm, N, 2, 16, 48000, 4096, 0, NULL);
    CHECK(sz > 0);
    CHECK(sz < (size_t)N * 2 * 2 * 4 / 10); /* a clean tone compresses a lot */
    /* the same with FIXED predictors only */
    CHECK(round_trip(pcm, N, 2, 16, 48000, 4096, -1, NULL) > 0);
    /* silence: constant subframes */
    memset(pcm, 0, sizeof pcm);
    sz = round_trip(pcm, N, 2, 16, 48000, 4096, 0, NULL);
    CHECK(sz > 0 && sz < 200);
    /* square waves (big steps, long runs) */
    for (int i = 0; i < N; i++) pcm[2 * i] = (i / 50) & 1 ? 20000 : -20000, pcm[2 * i + 1] = (i / 37) & 1 ? 32767 : -32768;
    CHECK(round_trip(pcm, N, 2, 16, 44100, 4096, 0, NULL) > 0);
    /* 24-bit: tone + noise, mono */
    for (int i = 0; i < N; i++) pcm[i] = (int32_t)lrint(4000000.0 * sin(i * 0.013)) + noise(10);
    CHECK(round_trip(pcm, N, 1, 24, 96000, 4096, 0, NULL) > 0);
    /* 24-bit full-scale noise in stereo: side needs 25 bits, residuals go big */
    for (int i = 0; i < N * 2; i++) pcm[i] = noise(24);
    CHECK(round_trip(pcm, N, 2, 24, 48000, 4096, 0, NULL) > 0);
    /* 5.1 at 16 bits, each channel different */
    for (int i = 0; i < N; i++)
        for (int c = 0; c < 6; c++) pcm[i * 6 + c] = (int32_t)lrint(8000.0 * sin(i * (0.01 + 0.007 * c))) + noise(6);
    CHECK(round_trip(pcm, N, 6, 16, 48000, 4096, 0, NULL) > 0);
}

TEST(flac_handles_odd_block_sizes_rates_and_wasted_bits) {
    enum { N = 9000 };
    static int32_t pcm[N * 2];
    for (int i = 0; i < N; i++) pcm[2 * i] = (int32_t)lrint(12000.0 * sin(i * 0.05)), pcm[2 * i + 1] = noise(12);
    static const struct {
        int block, rate, code;
    } cases[] = {
        {1536, 48000, 10}, {192, 44100, 9},  {4608, 32000, 8}, {1000, 22050, 6},
        {256, 11025, 13},  {300, 96000, 11}, {4096, 7350, 13}, {4096, 192000, 3},
        {4096, 100000, 12}, {4096, 352800, 14},
    };
    for (int i = 0; i < (int)countof(cases); i++) {
        int rc = -1;
        if (!round_trip(pcm, N, 2, 16, cases[i].rate, cases[i].block, 0, &rc) || rc != cases[i].code) {
            test_fail(__FILE__, __LINE__, "case %d (block %d, rate %d): rate code %d", i, cases[i].block, cases[i].rate, rc);
            return;
        }
    }
    /* 16-bit sound stored in 24-bit samples: every value a multiple of 256 (wasted bits) */
    for (int i = 0; i < N * 2; i++) pcm[i] = (int32_t)lrint(8000.0 * sin(i * 0.02)) * 256;
    size_t wasted = round_trip(pcm, N, 2, 24, 48000, 4096, 0, NULL);
    for (int i = 0; i < N * 2; i++) pcm[i] /= 256;
    size_t plain = round_trip(pcm, N, 2, 16, 48000, 4096, 0, NULL);
    CHECK(wasted > 0 && plain > 0);
    CHECK(wasted < plain + plain / 20); /* the 8 zero bits cost (almost) nothing */
    /* one sample, and a stream shorter than a block */
    CHECK(round_trip(pcm, 1, 2, 16, 48000, 4096, 0, NULL) > 0);
    CHECK(round_trip(pcm, 777, 2, 16, 48000, 4096, 0, NULL) > 0);
    /* 8-bit and 12-bit */
    for (int i = 0; i < N * 2; i++) pcm[i] = noise(8);
    CHECK(round_trip(pcm, N, 2, 8, 8000, 4096, 0, NULL) > 0);
    for (int i = 0; i < N * 2; i++) pcm[i] = (int32_t)lrint(1500.0 * sin(i * 0.1));
    CHECK(round_trip(pcm, N, 2, 12, 16000, 4096, 0, NULL) > 0);
}

TEST(flac_escapes_and_rice2_survive_extreme_residuals) {
    enum { N = 8192 };
    static int32_t pcm[N];
    /* long silences broken by full-scale spikes: zero-width escapes next to huge values */
    for (int i = 0; i < N; i++) pcm[i] = (i % 2048) < 1500 ? 0 : noise(24);
    CHECK(round_trip(pcm, N, 1, 24, 48000, 4096, 0, NULL) > 0);
    /* alternating extremes: worst case for every predictor */
    for (int i = 0; i < N; i++) pcm[i] = i & 1 ? 8388607 : -8388608;
    CHECK(round_trip(pcm, N, 1, 24, 48000, 4096, 0, NULL) > 0);
    /* a slow, loud 24-bit ramp: LPC with big Rice parameters (Rice2 territory) */
    for (int i = 0; i < N; i++) pcm[i] = (int32_t)lrint(8000000.0 * sin(i * 0.0007)) + noise(18);
    CHECK(round_trip(pcm, N, 1, 24, 48000, 4096, 0, NULL) > 0);
}

TEST(flac_header_and_bad_input) {
    FlacConfig bad[] = {{0, 2, 16, 0, 0}, {48000, 0, 16, 0, 0}, {48000, 9, 16, 0, 0}, {48000, 2, 3, 0, 0},
                        {48000, 2, 25, 0, 0}, {48000, 2, 16, 8, 0}, {48000, 2, 16, 70000, 0}, {48000, 2, 16, 0, 40}};
    for (int i = 0; i < (int)countof(bad); i++) CHECK(flac_new(&bad[i]) == NULL);
    FlacConfig c = {44100, 2, 16, 0, 0};
    Flac *f = flac_new(&c);
    CHECK(f != NULL);
    uint8_t h[FLAC_HEADER_BYTES];
    flac_header(f, h);
    CHECK(memcmp(h, "fLaC\x80\x00\x00\x22", 8) == 0);
    CHECK(h[8] == 0x10 && h[9] == 0x00 && h[10] == 0x10 && h[11] == 0x00); /* block size 4096 */
    CHECK_INT(flac_samples(f), 0);
    int32_t loud[4] = {40000, 0, 0, 0}; /* doesn't fit 16 bits */
    uint8_t out[256];
    CHECK_INT(flac_encode_frame(f, loud, 2, out, sizeof out), -1);
    CHECK_INT(flac_encode_frame(f, loud + 1, 0, out, sizeof out), -1);
    int32_t ok[4] = {1, 2, 3, 4};
    long n = flac_encode_frame(f, ok, 2, out, sizeof out);
    CHECK(n > 0);
    CHECK_INT(flac_samples(f), 2);
    flac_free(f);
}

/* ------------------------------------------------- real sound, when given */

#include "../../core/plat.h"
#include "ac3.h"
#include "mkv.h"

static bool rd_file(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

TEST(flac_real_dvd_sound_round_trips) {
    const char *path = plat_getenv("HEMMA_TEST_MKV");
    if (!path || !*path) return;
    int64_t size = 0;
    CHECK(path_stat(path, &size, NULL, NULL));
    FILE *fp = file_open(path, "rb");
    MkvReader r = {fp, size, rd_file};
    char err[128];
    Mkv *m = mkv_open(&r, err, sizeof err);
    CHECK(m);
    const MkvTrack *at = mkv_find_track(m, MKV_AUDIO);
    if (!at || strcmp(at->codec, "A_AC3") != 0) {
        mkv_close(m);
        fclose(fp);
        return;
    }
    Ac3Dec *dec = ac3_open();
    ac3_set_layout(dec, AC3_LAYOUT_WAV);
    static float buf[AC3_SAMPLES * AC3_MAX_CHANNELS];
    static int16_t s16[AC3_SAMPLES * AC3_MAX_CHANNELS];
    enum { SECONDS = 30 };
    int32_t *pcm = malloc((size_t)48000 * SECONDS * AC3_MAX_CHANNELS * sizeof *pcm);
    size_t n = 0;
    int ch = 0, rate = 0;
    MkvFrame f;
    while (mkv_next(m, &f) == 1 && (!rate || n / (size_t)ch < (size_t)rate * SECONDS - AC3_SAMPLES)) {
        if (f.track != at->number) continue;
        int c = 0;
        Ac3Info info;
        if (ac3_decode(dec, f.data, f.size, buf, &c, &info) < 0 && !c) continue;
        if (ch && c != ch) break;
        ch = c, rate = info.sample_rate;
        ac3_to_s16(buf, s16, AC3_SAMPLES * (size_t)c);
        for (size_t i = 0; i < AC3_SAMPLES * (size_t)c; i++) pcm[n + i] = s16[i];
        n += AC3_SAMPLES * (size_t)c;
    }
    CHECK(ch > 0 && n > 0);
    size_t sz = round_trip(pcm, (int)(n / (size_t)ch), ch, 16, rate, 4096, 0, NULL);
    CHECK(sz > 0);
    printf("    real sound: %d s of %d ch at %d Hz -> %zu bytes FLAC (%.0f%% of the PCM)\n", (int)(n / (size_t)ch / (size_t)rate), ch,
           rate, sz, 100.0 * (double)sz / ((double)n * 2));
    free(pcm);
    ac3_close(dec);
    mkv_close(m);
    fclose(fp);
}
