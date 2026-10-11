/* modules/av/conv_test.c — the converter on MKVs written here in memory: DVD-style
 * MPEG-2 re-encoded to H.264 with AAC copied, H.264 copied untouched, what it
 * refuses (and says so), stopping, progress. The same code runs in browsers as
 * WebAssembly (wasm/avconv.c), so this guards both. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include <math.h>

#include "aac.h"
#include "ac3.h"
#include "ac3_testenc.h"
#include "conv.h"
#include "h264enc.h"

/* ------------------------------------------------------------ bytes in memory */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} W;

static void w_put(W *w, const void *d, size_t n) {
    if (w->len + n > w->cap) {
        w->cap = (w->len + n) * 2 + 256;
        w->p = xrealloc(w->p, w->cap);
    }
    if (n) memcpy(w->p + w->len, d, n);
    w->len += n;
}

static void w_byte(W *w, int b) {
    uint8_t c = (uint8_t)b;
    w_put(w, &c, 1);
}

static void w_id(W *w, uint32_t id) {
    int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
    for (int i = n - 1; i >= 0; i--) w_byte(w, (int)(id >> (8 * i)));
}

static void w_size8(W *w, uint64_t v) {
    w_byte(w, 0x01);
    for (int i = 6; i >= 0; i--) w_byte(w, (int)(v >> (8 * i)));
}

static size_t w_open(W *w, uint32_t id) {
    w_id(w, id);
    size_t at = w->len;
    w_size8(w, 0);
    return at;
}

static void w_close(W *w, size_t at) {
    uint64_t n = w->len - at - 8;
    for (int i = 1; i < 8; i++) w->p[at + i] = (uint8_t)(n >> (8 * (7 - i)));
}

static void w_uint(W *w, uint32_t id, uint64_t v) {
    int n = 1;
    while (n < 8 && v >> (8 * n)) n++;
    w_id(w, id);
    w_byte(w, 0x80 | n);
    for (int i = n - 1; i >= 0; i--) w_byte(w, (int)(v >> (8 * i)));
}

static void w_bin(W *w, uint32_t id, const void *d, size_t n) {
    w_id(w, id);
    w_size8(w, n);
    w_put(w, d, n);
}

static void w_float(W *w, uint32_t id, double v) {
    uint64_t u;
    memcpy(&u, &v, 8);
    w_id(w, id);
    w_byte(w, 0x88);
    for (int i = 7; i >= 0; i--) w_byte(w, (int)(u >> (8 * i)));
}

static void w_simple(W *w, int track, int ms, bool key, const void *d, size_t n) {
    w_id(w, 0xA3);
    w_size8(w, 4 + n);
    w_byte(w, 0x80 | track);
    w_byte(w, (ms >> 8) & 0xFF);
    w_byte(w, ms & 0xFF);
    w_byte(w, key ? 0x80 : 0);
    w_put(w, d, n);
}

static bool mem_read(void *ctx, int64_t off, void *dst, size_t n) {
    const W *w = ctx;
    if (off < 0 || (uint64_t)off + n > w->len) return false;
    memcpy(dst, w->p + off, n);
    return true;
}

static bool mem_write(void *ctx, const void *p, size_t n) {
    w_put(ctx, p, n);
    return true;
}

static bool mem_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    W *w = ctx;
    if (off < 0 || (uint64_t)off + n > w->len) return false;
    memcpy(w->p + off, p, n);
    return true;
}

/* ------------------------------------------------- MPEG-2 pictures, bit by bit */

typedef struct {
    uint8_t b[4096];
    size_t n; /* bits */
} Bw;

static void put(Bw *w, uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--, w->n++)
        if (v >> i & 1) w->b[w->n >> 3] |= (uint8_t)(0x80 >> (w->n & 7));
}

static void start_code(Bw *w, int code) {
    w->n = (w->n + 7) & ~(size_t)7;
    put(w, 1, 24);
    put(w, (uint32_t)code, 8);
}

static void dc(Bw *w, int comp, int diff) {
    static const uint16_t lum[12][2] = {{4, 3}, {0, 2}, {1, 2}, {5, 3}, {6, 3}, {14, 4},
                                        {30, 5}, {62, 6}, {126, 7}, {254, 8}, {510, 9}, {511, 9}};
    static const uint16_t chrom[12][2] = {{0, 2}, {1, 2}, {2, 2}, {6, 3}, {14, 4}, {30, 5},
                                          {62, 6}, {126, 7}, {254, 8}, {510, 9}, {1022, 10}, {1023, 10}};
    int a = abs(diff), size = 0;
    while (a >> size) size++;
    if (comp) put(w, chrom[size][0], chrom[size][1]);
    else put(w, lum[size][0], lum[size][1]);
    if (size) put(w, (uint32_t)(diff > 0 ? diff : diff + (1 << size) - 1), size);
}

/* One progressive 48x32 I picture (sequence header and GOP in front, like a DVD's
   keyframes) of flat macroblocks whose brightness moves with `k`. */
static size_t mpeg2_picture(uint8_t *out, int k) {
    static Bw w;
    memset(&w, 0, sizeof w);
    start_code(&w, 0xB3); /* sequence header: 48x32, square, 29.97 */
    put(&w, 48, 12), put(&w, 32, 12), put(&w, 1, 4), put(&w, 4, 4), put(&w, 0x3FFFF, 18), put(&w, 1, 1);
    put(&w, 112, 10), put(&w, 0, 3);
    start_code(&w, 0xB5); /* sequence extension: MP@ML, progressive, 4:2:0 */
    put(&w, 1, 4), put(&w, 0x48, 8), put(&w, 1, 1), put(&w, 1, 2), put(&w, 0, 4), put(&w, 0, 12), put(&w, 1, 1);
    put(&w, 0, 8), put(&w, 0, 1), put(&w, 0, 7);
    start_code(&w, 0xB8); /* GOP, closed */
    put(&w, 0, 25), put(&w, 1, 1), put(&w, 0, 1);
    start_code(&w, 0x00); /* picture: I */
    put(&w, 0, 10), put(&w, 1, 3), put(&w, 0xFFFF, 16), put(&w, 0, 1);
    start_code(&w, 0xB5); /* picture coding extension: frame, frame DCT, progressive */
    put(&w, 8, 4);
    for (int i = 0; i < 4; i++) put(&w, 15, 4);
    put(&w, 3, 2), put(&w, 3, 2), put(&w, 0, 1), put(&w, 1, 1), put(&w, 0, 4), put(&w, 0, 1), put(&w, 1, 1);
    put(&w, 1, 1), put(&w, 0, 1);
    for (int row = 0; row < 2; row++) {
        start_code(&w, row + 1);
        put(&w, 8, 5), put(&w, 0, 1);
        int pred[3] = {1024, 1024, 1024};
        for (int x = 0; x < 3; x++) {
            put(&w, 1, 1); /* macroblock address increment 1 */
            put(&w, 1, 1); /* intra */
            int lv[3] = {40 + (k * 3 + x * 50 + row * 70) % 180, 128, 128};
            for (int b = 0; b < 6; b++) {
                int c = b < 4 ? 0 : b - 3, v = 8 * lv[c] + 1;
                dc(&w, c, v - pred[c]);
                pred[c] = v;
                put(&w, 2, 2); /* end of block */
            }
        }
    }
    size_t n = (w.n + 7) / 8;
    memcpy(out, w.b, n);
    return n;
}

/* ------------------------------------------------------------ test files */

static int frame_ms(int k) { return (int)((int64_t)k * 1001 / 30); }
static int au_ms(int k) { return (int)((int64_t)k * 1024 * 1000 / 48000); }

/* AAC access units: `seconds` of a 440 Hz tone */
static int make_aac(uint8_t aus[][AAC_MAX_AU], int *sizes, int cap, double seconds, uint8_t asc[2]) {
    AacConfig ac = {48000, 2, 128000, 0, 0};
    char err[100];
    AacEnc *e = aac_open(&ac, err, sizeof err);
    aac_specific_config(e, asc);
    float pcm[AAC_FRAME * 2];
    int n = 0, blocks = (int)(seconds * 48000 / AAC_FRAME);
    for (int b = 0; b < blocks && n < cap; b++) {
        for (int i = 0; i < AAC_FRAME; i++)
            pcm[2 * i] = pcm[2 * i + 1] = 0.3f * (float)sin(2 * 3.14159265 * 440 * (b * AAC_FRAME + i) / 48000.0);
        int got = aac_encode(e, pcm, aus[n], AAC_MAX_AU);
        if (got > 0) sizes[n++] = got;
    }
    for (int got; n < cap && (got = aac_flush(e, aus[n], AAC_MAX_AU)) > 0;) sizes[n++] = got;
    aac_close(e);
    return n;
}

typedef struct {
    const char *vcodec;
    const uint8_t *vpriv;
    size_t vpriv_len;
    int width, height;
    const char *acodec; /* NULL: no sound */
    const uint8_t *apriv;
    size_t apriv_len;
} Tracks;

static size_t start_file(W *w, const Tracks *t, double ms) {
    memset(w, 0, sizeof *w);
    size_t e = w_open(w, 0x1A45DFA3);
    w_uint(w, 0x4286, 1);
    w_bin(w, 0x4282, "matroska", 8);
    w_uint(w, 0x4287, 4);
    w_close(w, e);
    size_t seg = w_open(w, 0x18538067);
    size_t info = w_open(w, 0x1549A966);
    w_uint(w, 0x2AD7B1, 1000000);
    w_float(w, 0x4489, ms);
    w_close(w, info);
    size_t tracks = w_open(w, 0x1654AE6B);
    size_t tr = w_open(w, 0xAE);
    w_uint(w, 0xD7, 1);
    w_uint(w, 0x83, 1);
    w_bin(w, 0x86, t->vcodec, strlen(t->vcodec));
    if (t->vpriv) w_bin(w, 0x63A2, t->vpriv, t->vpriv_len);
    w_uint(w, 0x23E383, 33366667);
    size_t v = w_open(w, 0xE0);
    w_uint(w, 0xB0, (uint64_t)t->width);
    w_uint(w, 0xBA, (uint64_t)t->height);
    w_close(w, v);
    w_close(w, tr);
    if (t->acodec) {
        tr = w_open(w, 0xAE);
        w_uint(w, 0xD7, 2);
        w_uint(w, 0x83, 2);
        w_bin(w, 0x86, t->acodec, strlen(t->acodec));
        if (t->apriv) w_bin(w, 0x63A2, t->apriv, t->apriv_len);
        w_bin(w, 0x22B59C, "swe", 3);
        size_t a = w_open(w, 0xE1);
        w_float(w, 0xB5, 48000.0);
        w_uint(w, 0x9F, 2);
        w_close(w, a);
        w_close(w, tr);
    }
    w_close(w, tracks);
    return seg;
}

/* ------------------------------------------------------------ checking the MP4 */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static size_t box(const W *w, const char *type) { /* offset of a top-level box, or 0 */
    for (size_t at = 0; at + 8 <= w->len;) {
        uint32_t n = be32(w->p + at);
        if (!memcmp(w->p + at + 4, type, 4)) return at;
        if (n < 8) break;
        at += n;
    }
    return 0;
}

/* sample counts of the first two tracks (their stsz boxes, in order) */
static void counts(const W *w, int out[2]) {
    out[0] = out[1] = -1;
    size_t moov = box(w, "moov");
    if (!moov) return;
    size_t end = moov + be32(w->p + moov);
    for (size_t at = moov, k = 0; at + 20 <= end && k < 2; at++)
        if (!memcmp(w->p + at, "stsz", 4)) out[k++] = (int)be32(w->p + at + 12);
}

typedef struct {
    int calls, last;
    bool rising;
} Seen;

static void seen(void *ctx, int pm) {
    Seen *s = ctx;
    if (pm < s->last) s->rising = false;
    s->last = pm;
    s->calls++;
}

static bool run(W *in, W *out, AvConvJob *j, Seen *s) {
    memset(out, 0, sizeof *out);
    j->in = (MkvReader){in, (int64_t)in->len, mem_read};
    j->out = (Mp4Out){out, mem_write, mem_write_at};
    memset(s, 0, sizeof *s);
    s->rising = true;
    j->progress = seen;
    j->progress_ctx = s;
    return av_convert(j);
}

/* ------------------------------------------------------------------- tests */

TEST(conv_reencodes_dvd_video_and_copies_aac) {
    static uint8_t aus[200][AAC_MAX_AU];
    int sizes[200];
    uint8_t asc[2];
    int naus = make_aac(aus, sizes, 200, 3.0, asc);
    CHECK(naus > 130);
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_AAC", asc, 2};
    size_t seg = start_file(&in, &t, 3000);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t pic[4096];
    int frames = 90;
    for (int k = 0, a = 0; k < frames || a < naus;) { /* interleaved by time, like a real file */
        if (k < frames && (a >= naus || frame_ms(k) <= au_ms(a))) {
            size_t n = mpeg2_picture(pic, k);
            w_simple(&in, 1, frame_ms(k), true, pic, n);
            k++;
        } else {
            w_simple(&in, 2, au_ms(a), true, aus[a], (size_t)sizes[a]);
            a++;
        }
    }
    w_close(&in, cl);
    w_close(&in, seg);

    W out;
    AvConvJob j = {0};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_STR(j.err, "");
    CHECK_HAS(j.what, "progressive");
    CHECK_HAS(j.what, "→ H.264");
    CHECK_HAS(j.what, "AAC copied");
    CHECK(!j.unsupported);
    CHECK(j.out_bytes == (int64_t)out.len);
    CHECK(s.calls >= 3 && s.rising && s.last == 1000);
    CHECK(j.permille == 1000);
    /* a faststart MP4: ftyp, the index, then the media */
    CHECK(out.len > 16 && !memcmp(out.p + 4, "ftyp", 4));
    size_t moov = box(&out, "moov"), mdat = box(&out, "mdat");
    CHECK(moov && mdat && moov < mdat);
    int n[2];
    counts(&out, n);
    CHECK_INT(n[0], frames); /* every picture, progressive: nothing dropped or doubled */
    CHECK_INT(n[1], naus);   /* every sound block, untouched */

    /* the same file twice: the same bytes (the browser's copy matches the server's) */
    W again;
    AvConvJob j2 = {0};
    CHECK(run(&in, &again, &j2, &s));
    CHECK(again.len == out.len && !memcmp(again.p, out.p, out.len));

    /* a preview: only the first second */
    W part;
    AvConvJob j3 = {.limit_ns = 1000000000LL};
    CHECK(run(&in, &part, &j3, &s));
    counts(&part, n);
    CHECK(n[0] >= 30 && n[0] < 40);

    /* stopped: fails, not "unsupported" */
    volatile int stop = 1;
    W none;
    AvConvJob j4 = {.stop = &stop};
    CHECK(!run(&in, &none, &j4, &s));
    CHECK_STR(j4.err, "Stopped");
    CHECK(!j4.unsupported);
    free(in.p), free(out.p), free(again.p), free(part.p), free(none.p);
}

TEST(conv_copies_h264_as_it_is) {
    H264Params hp = {.width = 64, .height = 48, .fps_num = 30000, .fps_den = 1001, .keyint = 10, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    CHECK(e != NULL);
    uint8_t avcc[256];
    size_t avcc_len = h264enc_avcc(e, avcc, sizeof avcc);
    CHECK(avcc_len > 8);
    W in;
    Tracks t = {"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, NULL, NULL, 0};
    size_t seg = start_file(&in, &t, 1000);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t y[64 * 48], u[32 * 24], v[32 * 24];
    static uint8_t first[8192];
    size_t first_len = 0;
    for (int k = 0; k < 30; k++) {
        for (int i = 0; i < 64 * 48; i++) y[i] = (uint8_t)((i % 64 + i / 64 + k * 4) & 0xFF);
        memset(u, 100, sizeof u), memset(v, 150, sizeof v);
        H264Picture p = {y, u, v, 64, 32, 32, k};
        H264Packet pk;
        CHECK(h264enc_encode(e, &p, &pk));
        if (k == 0) memcpy(first, pk.data, first_len = pk.size);
        w_simple(&in, 1, frame_ms(k), pk.idr, pk.data, pk.size);
    }
    h264enc_free(e);
    w_close(&in, cl);
    w_close(&in, seg);

    W out;
    AvConvJob j = {0};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_STR(j.what, "H.264 copied");
    int n[2];
    counts(&out, n);
    CHECK_INT(n[0], 30);
    CHECK_INT(n[1], -1); /* no sound track */
    /* the pictures are the original bytes: the first one sits in the mdat as it was */
    size_t mdat = box(&out, "mdat");
    CHECK(mdat && first_len && out.len > mdat + 8 + first_len);
    CHECK(!memcmp(out.p + mdat + 8, first, first_len) || !memcmp(out.p + mdat + 16, first, first_len)); /* (64-bit size) */
    free(in.p), free(out.p);
}

TEST(conv_says_what_it_cannot_convert) {
    W in, out;
    AvConvJob j = {0};
    Seen s;
    Tracks hevc = {"V_MPEGH/ISO/HEVC", NULL, 0, 64, 48, NULL, NULL, 0};
    size_t seg = start_file(&in, &hevc, 1000);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "V_MPEGH/ISO/HEVC");
    CHECK_INT((int)out.len, 0); /* nothing written */
    free(in.p), free(out.p);

    Tracks dts = {"V_MPEG2", NULL, 0, 48, 32, "A_DTS", NULL, 0};
    seg = start_file(&in, &dts, 1000);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "A_DTS sound");
    free(in.p), free(out.p);

    memset(&in, 0, sizeof in); /* not Matroska at all */
    w_put(&in, "RIFF....AVI LIST", 16);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Not a readable MKV file");
    free(in.p), free(out.p);
}

/* sizes of the k-th track's samples (its stsz box), added up */
static int64_t track_bytes(const W *w, int k) {
    size_t moov = box(w, "moov");
    if (!moov) return -1;
    size_t end = moov + be32(w->p + moov);
    int seen = 0;
    for (size_t at = moov; at + 20 <= end; at++) {
        if (memcmp(w->p + at, "stsz", 4) || seen++ != k) continue;
        uint32_t fixed = be32(w->p + at + 8), count = be32(w->p + at + 12);
        if (fixed) return (int64_t)fixed * count;
        int64_t sum = 0;
        for (uint32_t i = 0; i < count && at + 16 + 4 * (size_t)i + 4 <= end; i++) sum += be32(w->p + at + 16 + 4 * i);
        return sum;
    }
    return -1;
}

/* DVD sound: Dolby AC-3 (32 ms frames) becomes AAC; a hole in it (a damaged disc) is filled
   with silence, so the sound after it stays in step with the picture. */
TEST(conv_turns_dolby_into_aac) {
    static uint8_t ac3[64 * 800];
    size_t bytes = ac3_test_tone(ac3, sizeof ac3, 24); /* 0.77 s: the test encoder is slow */
    int fsize = ac3_frame_size(ac3, bytes), nf = fsize > 0 ? (int)(bytes / (size_t)fsize) : 0;
    CHECK_INT(nf, 24);
    int aac[2] = {0, 0};
    for (int hole = 0; hole < 2; hole++) {
        W in;
        Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_AC3", NULL, 0};
        size_t seg = start_file(&in, &t, 768);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        static uint8_t pic[4096];
        for (int k = 0, a = 0; k < 23 || a < nf;) {
            if (k < 23 && (a >= nf || frame_ms(k) <= a * 32)) {
                size_t n = mpeg2_picture(pic, k);
                w_simple(&in, 1, frame_ms(k), true, pic, n);
                k++;
            } else {
                if (!hole || a < 8 || a >= 14) w_simple(&in, 2, a * 32, true, ac3 + (size_t)a * fsize, (size_t)fsize);
                a++;
            }
        }
        w_close(&in, cl);
        w_close(&in, seg);
        W out;
        AvConvJob j = {0};
        Seen s;
        CHECK(run(&in, &out, &j, &s));
        CHECK_STR(j.err, "");
        CHECK_HAS(j.what, "Dolby → AAC");
        int n[2];
        counts(&out, n);
        CHECK_INT(n[0], 23);
        aac[hole] = n[1];
        CHECK(n[1] >= nf * 3 / 2 && n[1] <= nf * 3 / 2 + 3); /* 1536 samples a Dolby frame, 1024 an AAC one */
        if (!hole) CHECK(track_bytes(&out, 1) / n[1] > 40); /* a tone, not silence */
        free(in.p), free(out.p);
    }
    CHECK_INT(aac[1], aac[0]); /* the hole became silence of the same length */
}
