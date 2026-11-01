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
#include "dts.h"
#include "dts_testenc.h"
#include "h264enc.h"
#include "hevc_testenc.h"
#include "mp4.h"
#include "mpeg4_testenc.h"

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

static bool mem_write(void *ctx, const void *p, size_t n) { /* a "disk" of 64 MB: no test needs more */
    W *w = ctx;
    if (n > (64u << 20) - w->len) return false;
    w_put(w, p, n);
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

/* One w x h I picture (multiples of 16, at most 64x64; sequence header and GOP in front, like
   a DVD's keyframes) of flat macroblocks whose brightness moves with `k`. A progressive
   sequence, or (il) an interlaced one coded with field DCT whose bottom field is `comb`
   darker than the top: fields from different instants, as interlaced video shows them. */
static size_t mpeg2_picture_of(uint8_t *out, int k, int width, int height, bool il, int comb) {
    static Bw w;
    memset(&w, 0, sizeof w);
    start_code(&w, 0xB3); /* sequence header: square pixels, 29.97 */
    put(&w, (uint32_t)width, 12), put(&w, (uint32_t)height, 12), put(&w, 1, 4), put(&w, 4, 4), put(&w, 0x3FFFF, 18);
    put(&w, 1, 1), put(&w, 112, 10), put(&w, 0, 3);
    start_code(&w, 0xB5); /* sequence extension: MP@ML, (non-)progressive, 4:2:0 */
    put(&w, 1, 4), put(&w, 0x48, 8), put(&w, !il, 1), put(&w, 1, 2), put(&w, 0, 4), put(&w, 0, 12), put(&w, 1, 1);
    put(&w, 0, 8), put(&w, 0, 1), put(&w, 0, 7);
    start_code(&w, 0xB8); /* GOP, closed */
    put(&w, 0, 25), put(&w, 1, 1), put(&w, 0, 1);
    start_code(&w, 0x00); /* picture: I */
    put(&w, 0, 10), put(&w, 1, 3), put(&w, 0xFFFF, 16), put(&w, 0, 1);
    start_code(&w, 0xB5); /* picture coding extension: a frame; progressive with frame DCT, or top field first */
    put(&w, 8, 4);
    for (int i = 0; i < 4; i++) put(&w, 15, 4);
    put(&w, 3, 2), put(&w, 3, 2), put(&w, il, 1), put(&w, !il, 1), put(&w, 0, 4), put(&w, 0, 1), put(&w, 1, 1);
    put(&w, !il, 1), put(&w, 0, 1);
    for (int row = 0; row < height / 16; row++) {
        start_code(&w, row + 1);
        put(&w, 8, 5), put(&w, 0, 1);
        int pred[3] = {1024, 1024, 1024};
        for (int x = 0; x < width / 16; x++) {
            put(&w, 1, 1); /* macroblock address increment 1 */
            put(&w, 1, 1); /* intra */
            if (il) put(&w, 1, 1); /* dct_type: field - blocks 0, 1 hold the top field, 2, 3 the bottom */
            int top = 40 + (k * (il ? 9 : 3) + x * 50 + row * 70) % 180; /* (interlaced: enough to count as motion) */
            for (int b = 0; b < 6; b++) {
                int c = b < 4 ? 0 : b - 3, lv = c ? 128 : b >= 2 && il ? (top > comb + 16 ? top - comb : top + comb) : top;
                int v = 8 * lv + 1;
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

static size_t mpeg2_picture(uint8_t *out, int k) { return mpeg2_picture_of(out, k, 48, 32, false, 0); }

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

#define MANY_SUBS 34 /* subtitle tracks a test file may have: two more than the converter takes */

typedef struct { /* a subtitle track (codec NULL: none) */
    const char *codec, *lang, *name;
    bool forced;
    const void *priv; /* its CodecPrivate (VobSub: the .idx header) */
    size_t priv_len;
    const void *strip; /* header stripping: these bytes are left out of every block (ContentEncoding) */
    size_t strip_len;
    bool zlib; /* its blocks said to be zlib-compressed (ContentEncoding) */
} SubTrack;

typedef struct {
    const char *vcodec;
    const uint8_t *vpriv;
    size_t vpriv_len;
    int width, height;
    const char *acodec; /* NULL: no sound */
    const uint8_t *apriv;
    size_t apriv_len;
    SubTrack sub[MANY_SUBS]; /* tracks 3, 4, ... */
    int arate; /* the sound's SamplingFrequency (0: 48000) */
} Tracks;

/* a BlockGroup: a Block with its BlockDuration (subtitles come so) */
static void w_group(W *w, int track, int ms, int dur_ms, const void *d, size_t n) {
    size_t g = w_open(w, 0xA0);
    w_id(w, 0xA1);
    w_size8(w, 4 + n);
    w_byte(w, 0x80 | track);
    w_byte(w, (ms >> 8) & 0xFF);
    w_byte(w, ms & 0xFF);
    w_byte(w, 0);
    w_put(w, d, n);
    if (dur_ms) w_uint(w, 0x9B, (uint64_t)dur_ms);
    w_close(w, g);
}

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
    if (ms > 0) w_float(w, 0x4489, ms); /* (else: a length nobody wrote down) */
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
        w_float(w, 0xB5, t->arate ? t->arate : 48000.0);
        w_uint(w, 0x9F, 2);
        w_close(w, a);
        w_close(w, tr);
    }
    for (int k = 0; k < MANY_SUBS; k++) {
        if (!t->sub[k].codec) continue;
        tr = w_open(w, 0xAE);
        w_uint(w, 0xD7, (uint64_t)(3 + k));
        w_uint(w, 0x83, 0x11);
        w_bin(w, 0x86, t->sub[k].codec, strlen(t->sub[k].codec));
        if (t->sub[k].lang) w_bin(w, 0x22B59C, t->sub[k].lang, strlen(t->sub[k].lang));
        if (t->sub[k].name) w_bin(w, 0x536E, t->sub[k].name, strlen(t->sub[k].name));
        if (t->sub[k].forced) w_uint(w, 0x55AA, 1);
        if (t->sub[k].priv) w_bin(w, 0x63A2, t->sub[k].priv, t->sub[k].priv_len);
        if (t->sub[k].strip || t->sub[k].zlib) { /* ContentEncodings: one, header stripping (algorithm 3) or zlib (0) */
            size_t encs = w_open(w, 0x6D80), enc = w_open(w, 0x6240);
            w_uint(w, 0x5032, 1); /* scope: the frames */
            w_uint(w, 0x5033, 0); /* type: compression */
            size_t comp = w_open(w, 0x5034);
            w_uint(w, 0x4254, t->sub[k].zlib ? 0 : 3);
            if (t->sub[k].strip) w_bin(w, 0x4255, t->sub[k].strip, t->sub[k].strip_len);
            w_close(w, comp);
            w_close(w, enc);
            w_close(w, encs);
        }
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

/* 2.5 (the owner's Disney films: 35 of 54 MKVs with H.264 + MP3 went up as they were - "Its sound is MP3, which the
   converter can't change yet"): MP3 is copied into the MP4 as 'mp4a' with object type 0x6B (MPEG-1, 32-48 kHz) or 0x69
   (MPEG-2's half rates: Pinocchio's 24 kHz), one frame a sample - a block holding two frames gives two - each 1152 (or
   576) samples long at the track's rate. */
static size_t mp3_frame(uint8_t *f, bool half, uint8_t mark) { /* a header + a body (bytes as they come: copied) */
    static const uint8_t ONE[] = {0xFF, 0xFB, 0x90, 0x00}, TWO[] = {0xFF, 0xF3, 0x84, 0x00}; /* 128 kbps 44.1; 64 kbps 24 kHz */
    size_t n = half ? 72 * 64000 / 24000 : 144 * 128000 / 44100; /* 192, 417 */
    memset(f, mark, n);
    memcpy(f, half ? TWO : ONE, 4);
    return n;
}

TEST(conv_copies_mp3_sound) {
    for (int half = 0; half < 2; half++) {
        H264Params hp = {.width = 64, .height = 48, .fps_num = 25, .fps_den = 1, .keyint = 10, .annexb = false};
        H264Enc *e = h264enc_new(&hp);
        uint8_t avcc[256];
        size_t avcc_len = h264enc_avcc(e, avcc, sizeof avcc);
        W in;
        Tracks t = {"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_MPEG/L3", NULL, 0, .arate = half ? 24000 : 44100};
        size_t seg = start_file(&in, &t, 1000);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        static uint8_t y[64 * 48], u[32 * 24], v[32 * 24], blk[2048];
        for (int k = 0; k < 20; k++) {
            memset(y, k * 9, sizeof y), memset(u, 100, sizeof u), memset(v, 150, sizeof v);
            H264Picture p = {y, u, v, 64, 32, 32, k};
            H264Packet pk;
            CHECK(h264enc_encode(e, &p, &pk));
            w_simple(&in, 1, k * 40, pk.idr, pk.data, pk.size);
            size_t n = mp3_frame(blk, half, (uint8_t)k);
            if (k == 5) n += mp3_frame(blk + n, half, 0xA5); /* (one block, two frames: two samples) */
            w_simple(&in, 2, k * 40, true, blk, n);
        }
        h264enc_free(e);
        w_close(&in, cl);
        w_close(&in, seg);

        W out;
        AvConvJob j = {0};
        Seen s;
        CHECK(run(&in, &out, &j, &s));
        CHECK_STR(j.what, "H.264 copied · MP3 copied");
        int n[2];
        counts(&out, n);
        CHECK_INT(n[0], 20);
        CHECK_INT(n[1], 21); /* every frame a sample */
        size_t moov = box(&out, "moov");
        const uint8_t *m = out.p + moov, *end = m + be32(m), *esds = NULL, *stts = NULL, *mdhd = NULL;
        for (const uint8_t *q = m; q + 4 <= end; q++) {
            if (!memcmp(q, "esds", 4)) esds = q - 4;
            if (!memcmp(q, "stts", 4)) stts = q - 4; /* (the last: the sound's) */
            if (!memcmp(q, "mdhd", 4)) mdhd = q - 4;
        }
        CHECK(esds && stts && mdhd);
        if (esds) { /* ES_Descriptor 03, DecoderConfigDescriptor 04, its object type: no DecoderSpecificInfo (05) */
            const uint8_t *d = esds + 12;
            CHECK_INT(d[0], 0x03);
            const uint8_t *dc = memchr(d, 0x04, 12);
            CHECK(dc && dc[2] == (half ? 0x69 : 0x6B));
            CHECK(dc && dc[1] == 13 && dc[2 + 13] == 0x06); /* (right after it the SLConfigDescriptor) */
        }
        if (stts) { /* one run: 21 samples of 1152 (576) */
            CHECK_INT((int)be32(stts + 12), 1);
            CHECK_INT((int)be32(stts + 16), 21);
            CHECK_INT((int)be32(stts + 20), half ? 576 : 1152);
        }
        if (mdhd) CHECK_INT((int)be32(mdhd + 20), half ? 24000 : 44100); /* (its timescale: the rate) */
        free(in.p), free(out.p);
    }
}

/* 2.5 (the owner: "Its sound is Opus, which the converter can't change yet. So we should support that"): Opus is
   copied into the MP4 - 'Opus' with a dOps box made of the MKV's OpusHead (big-endian, version 0), 48 kHz, each packet a
   sample as long as its TOC byte says (20 ms, 60 ms, two frames ...), the pre-skip as the edit list's start. An Opus
   whose OpusHead is missing or of a mapping browsers don't play is left out, as before. */
static const uint8_t OPUS_HEAD[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0};

static size_t opus_packet(uint8_t *p, int k) { /* (a TOC byte and a body: copied, never decoded) */
    size_t n = 40;
    memset(p, k, n);
    p[0] = k == 3 ? 0x18 : k == 5 ? 0xF9 : k == 7 ? 0xFB : 0xF8; /* SILK 60 ms; CELT 20 ms x2 (code 1); code 3, cut short */
    if (k == 7) n = 1;                                          /* (no frame count: as long as the one before) */
    return n;
}

TEST(conv_copies_opus_sound) {
    H264Params hp = {.width = 64, .height = 48, .fps_num = 25, .fps_den = 1, .keyint = 10, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    uint8_t avcc[256], pkt[64];
    size_t avcc_len = h264enc_avcc(e, avcc, sizeof avcc);
    for (int bad = 0; bad < 2; bad++) {
        W in;
        static const uint8_t SURROUND[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 6, 0x38, 1, 0x80, 0xBB, 0, 0, 0, 0, 2};
        Tracks t = {"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_OPUS", bad ? SURROUND : OPUS_HEAD, sizeof OPUS_HEAD};
        size_t seg = start_file(&in, &t, 1000);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        static uint8_t y[64 * 48], u[32 * 24], v[32 * 24];
        for (int k = 0; k < 20; k++) {
            memset(y, k * 9, sizeof y), memset(u, 100, sizeof u), memset(v, 150, sizeof v);
            H264Picture p = {y, u, v, 64, 32, 32, k};
            H264Packet pk;
            CHECK(h264enc_encode(e, &p, &pk));
            w_simple(&in, 1, k * 40, pk.idr, pk.data, pk.size);
            w_simple(&in, 2, k * 40, true, pkt, opus_packet(pkt, k));
        }
        w_close(&in, cl);
        w_close(&in, seg);
        W out;
        AvConvJob j = {0};
        Seen s;
        bool ok = run(&in, &out, &j, &s);
        if (bad) { /* (mapping family 2: no sound we can copy - the file is refused, saying it) */
            CHECK(!ok && j.unsupported);
            CHECK_HAS(j.err, "Its sound is Opus");
            free(in.p), free(out.p);
            continue;
        }
        CHECK(ok);
        CHECK_STR(j.what, "H.264 copied · Opus copied");
        int n[2];
        counts(&out, n);
        CHECK_INT(n[0], 20);
        CHECK_INT(n[1], 20); /* every packet a sample */
        size_t moov = box(&out, "moov");
        const uint8_t *m = out.p + moov, *end = m + be32(m), *dops = NULL, *stts = NULL, *mdhd = NULL, *elst = NULL, *entry = NULL;
        for (const uint8_t *q = m; q + 4 <= end; q++) {
            if (!memcmp(q, "dOps", 4)) dops = q - 4;
            if (!memcmp(q, "Opus", 4) && !entry) entry = q - 4;
            if (!memcmp(q, "stts", 4)) stts = q - 4; /* (the last ones: the sound's) */
            if (!memcmp(q, "mdhd", 4)) mdhd = q - 4;
            if (!memcmp(q, "elst", 4)) elst = q - 4;
        }
        CHECK(dops && stts && mdhd && elst && entry);
        if (entry) CHECK_INT(entry[8 + 6 + 2 + 8 + 1], 2); /* (the sample entry's channel count: 2) */
        if (dops) { /* version 0, 2 channels, pre-skip 312, 48000, gain 0, mapping 0 - big-endian now */
            static const uint8_t want[11] = {0, 2, 0x01, 0x38, 0, 0, 0xBB, 0x80, 0, 0, 0};
            CHECK_INT((int)be32(dops), 8 + 11);
            CHECK(!memcmp(dops + 8, want, 11));
        }
        if (stts) { /* 960 x3, 2880, 960, 1920, 960 x14 (the cut one as long as the one before) */
            static const uint32_t runs[][2] = {{3, 960}, {1, 2880}, {1, 960}, {1, 1920}, {14, 960}};
            CHECK_INT((int)be32(stts + 12), 5);
            for (int r = 0; r < 5 && be32(stts + 12) == 5; r++) {
                CHECK_INT((int)be32(stts + 16 + r * 8), (int)runs[r][0]);
                CHECK_INT((int)be32(stts + 20 + r * 8), (int)runs[r][1]);
            }
        }
        if (mdhd) CHECK_INT((int)be32(mdhd + 20), 48000);
        if (elst) CHECK_INT((int)be32(elst + 12 + 4 + 8 + 4), 312); /* (its one edit - version 1 - starts at the pre-skip) */
        free(in.p), free(out.p);
    }
    h264enc_free(e);
}

/* HEVC (x265 films) can't be decoded here, but Safari plays it - only not inside MKV. So the
   frames move into an MP4 as they are: 'hvc1' with the MKV's hvcC record (parameter-set arrays
   marked complete, as Apple wants), decode order kept, B-frames as composition offsets. */
TEST(conv_copies_hevc_for_safari) {
    static const uint8_t hvcc[] = {
        1, 0x22, 0x20, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 0x5D, 0xF0, 0, 0xFC, 0xFD, 0xFA, 0xFA, 0, 0, 0x0F, /* Main 10 */
        3,                                                                                            /* arrays */
        0x20, 0, 1, 0, 2, 0x40, 0x01, /* VPS */
        0x21, 0, 1, 0, 2, 0x42, 0x01, /* SPS */
        0x22, 0, 1, 0, 2, 0x44, 0x01, /* PPS */
    };
    W in;
    Tracks t = {"V_MPEGH/ISO/HEVC", hvcc, sizeof hvcc, 64, 48, NULL, NULL, 0};
    size_t seg = start_file(&in, &t, 1000);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static const int order[] = {0, 3, 1, 2, 6, 4, 5, 9, 7, 8}; /* I P B B ...: file order is decode order */
    uint8_t frame[64];
    for (int k = 0; k < 10; k++) {
        for (int i = 0; i < (int)sizeof frame; i++) frame[i] = (uint8_t)(k * 16 + i);
        frame[0] = frame[1] = frame[2] = 0, frame[3] = (uint8_t)(sizeof frame - 4); /* one length-prefixed NAL */
        w_simple(&in, 1, frame_ms(order[k]), k == 0, frame, sizeof frame);
    }
    w_close(&in, cl);
    w_close(&in, seg);

    W out;
    AvConvJob j = {0};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_STR(j.what, "HEVC copied");
    int n[2];
    counts(&out, n);
    CHECK_INT(n[0], 10);
    size_t moov = box(&out, "moov");
    CHECK(moov != 0);
    const uint8_t *m = out.p + moov, *end = m + be32(m);
    const uint8_t *entry = NULL, *cfg = NULL, *ctts = NULL;
    for (const uint8_t *q = m; q + 4 <= end; q++) {
        if (!entry && !memcmp(q, "hvc1", 4)) entry = q;
        if (!cfg && !memcmp(q, "hvcC", 4)) cfg = q + 4;
        if (!ctts && !memcmp(q, "ctts", 4)) ctts = q;
    }
    CHECK(entry != NULL);
    bool avc = false;
    for (const uint8_t *q = m; q + 4 <= end; q++) avc |= !memcmp(q, "avc1", 4) || !memcmp(q, "avcC", 4);
    CHECK(!avc);
    CHECK(cfg != NULL);
    if (cfg) {
        CHECK(!memcmp(cfg, hvcc, 22)); /* the record as MKV had it... */
        CHECK_INT(cfg[23], 0xA0);       /* ...with its VPS, SPS and PPS arrays marked complete */
        CHECK_INT(cfg[30], 0xA1);
        CHECK_INT(cfg[37], 0xA2);
        CHECK(!memcmp(cfg + 28, hvcc + 28, 2)); /* NAL bytes untouched */
    }
    CHECK(ctts != NULL); /* B-frames: shown in another order than decoded */
    /* the pictures are the original bytes, in file order */
    size_t mdat = box(&out, "mdat");
    CHECK(mdat && (out.p[mdat + 8 + 4] == 4 || out.p[mdat + 16 + 4] == 4));
    free(in.p), free(out.p);
}

/* 2.1 (owner, 2026-10-29: an AV1 upload "went up as it was" - "make this viable also as a convert"): AV1 (web
   releases, "AV1-WoKE") isn't decoded either, but every Chrome, Edge and Firefox plays it in an MP4, and Safari
   does on devices with AV1 - never in an MKV. 'av01' with MKV's own record as its av1C; a temporal delimiter
   leading a block is dropped (MP4 samples have none); keyframes only where the file says so. */
TEST(conv_copies_av1) {
    static const uint8_t av1c[] = {0x81, 0x08, 0x0C, 0x00, 0x0A, 0x0B, 0x00, 0x00, 0x00, 0x2D, 0x4C, /* + a sequence */
                                   0xFF, 0xB3, 0xDF, 0xFF, 0x96, 0x04};                            /* header OBU */
    W in;
    Tracks t = {"V_AV1", av1c, sizeof av1c, 64, 48, NULL, NULL, 0};
    size_t seg = start_file(&in, &t, 1000);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    uint8_t frame[40];
    for (int k = 0; k < 10; k++) {
        bool td = k % 2 == 0; /* every other block still carries its temporal delimiter */
        size_t n = 0;
        if (td) frame[n++] = 0x12, frame[n++] = 0;
        frame[n++] = 0x32, frame[n++] = 30; /* an OBU_FRAME, 30 bytes */
        for (int i = 0; i < 30; i++) frame[n++] = (uint8_t)(k * 16 + i);
        w_simple(&in, 1, frame_ms(k), k == 0 || k == 6, frame, n);
    }
    w_close(&in, cl);
    w_close(&in, seg);

    W out;
    AvConvJob j = {0};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_STR(j.what, "AV1 copied");
    int n[2];
    counts(&out, n);
    CHECK_INT(n[0], 10);
    CHECK(!memcmp(out.p + 4, "ftyp", 4) && !memcmp(out.p + 24, "av01", 4)); /* its compatible brands say AV1 */
    size_t moov = box(&out, "moov");
    CHECK(moov != 0);
    const uint8_t *m = out.p + moov, *end = m + be32(m);
    const uint8_t *entry = NULL, *cfg = NULL, *stss = NULL, *stsz = NULL;
    bool other = false;
    for (const uint8_t *q = m; q + 4 <= end; q++) {
        if (!entry && !memcmp(q, "av01", 4)) entry = q;
        if (!cfg && !memcmp(q, "av1C", 4)) cfg = q - 4;
        if (!stss && !memcmp(q, "stss", 4)) stss = q - 4;
        if (!stsz && !memcmp(q, "stsz", 4)) stsz = q - 4;
        other |= !memcmp(q, "avc1", 4) || !memcmp(q, "hvc1", 4) || !memcmp(q, "ctts", 4); /* (no reordering in AV1) */
    }
    CHECK(entry && cfg && stss && stsz && !other);
    if (cfg) {
        CHECK_INT((int)be32(cfg), 8 + (int)sizeof av1c);
        CHECK(!memcmp(cfg + 8, av1c, sizeof av1c)); /* the record as MKV had it */
    }
    if (stss) { /* the two keyframes: samples 1 and 7 */
        CHECK_INT((int)be32(stss + 12), 2);
        CHECK_INT((int)be32(stss + 16), 1);
        CHECK_INT((int)be32(stss + 20), 7);
    }
    if (stsz) { /* every sample 32 bytes: the delimiters gone */
        CHECK_INT((int)be32(stsz + 16), 10);
        for (int i = 0; i < 10; i++) CHECK_INT((int)be32(stsz + 20 + 4 * i), 32);
    }
    size_t mdat = box(&out, "mdat"); /* the first sample: the frame OBU, as it was */
    CHECK(mdat && (out.p[mdat + 8] == 0x32 || out.p[mdat + 16] == 0x32));
    free(in.p), free(out.p);

    /* no record (Matroska requires one, an MP4 needs it): said, nothing written */
    t.vpriv = NULL, t.vpriv_len = 0;
    seg = start_file(&in, &t, 1000);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Its AV1 picture has no settings record");
    CHECK_INT((int)out.len, 0);
    free(in.p), free(out.p);
}

TEST(conv_says_what_it_cannot_convert) {
    W in, out;
    AvConvJob j = {0};
    Seen s;
    Tracks vp9 = {"V_VP9", NULL, 0, 64, 48, NULL, NULL, 0};
    size_t seg = start_file(&in, &vp9, 1000);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Its picture is VP9");
    CHECK_INT((int)out.len, 0); /* nothing written */
    free(in.p), free(out.p);

    Tracks truehd = {"V_MPEG2", NULL, 0, 48, 32, "A_TRUEHD", NULL, 0};
    seg = start_file(&in, &truehd, 1000);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Its sound is Dolby TrueHD");
    free(in.p), free(out.p);

    memset(&in, 0, sizeof in); /* not Matroska at all */
    w_put(&in, "RIFF....AVI LIST", 16);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Not a readable MKV or MP4 file");
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
        CHECK_HAS(j.what, "Dolby Digital → AAC");
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

/* a track's timescale (its mdhd box, k-th in the file), or -1 */
static int64_t track_timescale(const W *w, int k) {
    size_t moov = box(w, "moov");
    if (!moov) return -1;
    size_t end = moov + be32(w->p + moov);
    int seen = 0;
    for (size_t at = moov; at + 24 <= end; at++)
        if (!memcmp(w->p + at, "mdhd", 4) && seen++ == k) return be32(w->p + at + (w->p[at + 4] == 1 ? 24 : 16));
    return -1;
}

/* Dolby at 44.1 kHz (TV rips) or 32 kHz becomes AAC at that same rate: 1536 samples a frame take
   longer, and the copy has to play at the right speed (it was refused until 1.1.2). */
TEST(conv_turns_dolby_at_44_1_and_32_khz_into_aac) {
    static const int RATES[] = {44100, 32000};
    for (int r = 0; r < 2; r++) {
        int rate = RATES[r];
        static uint8_t ac3[24 * 1152];
        size_t bytes = ac3_test_tone_at(ac3, sizeof ac3, 24, rate);
        int fsize = ac3_frame_size(ac3, bytes), nf = fsize > 0 ? (int)(bytes / (size_t)fsize) : 0;
        CHECK_INT(fsize, rate == 44100 ? 834 : 1152);
        CHECK_INT(nf, 24);
        W in;
        Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_AC3", NULL, 0, .arate = rate};
        size_t seg = start_file(&in, &t, 1200);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        static uint8_t pic[4096];
        for (int k = 0, a = 0; k < 23 || a < nf;) {
            int a_ms = (int)((int64_t)a * 1536 * 1000 / rate);
            if (k < 23 && (a >= nf || frame_ms(k) <= a_ms)) {
                w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, k));
                k++;
            } else {
                w_simple(&in, 2, a_ms, true, ac3 + (size_t)a * fsize, (size_t)fsize);
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
        CHECK_HAS(j.what, "Dolby Digital → AAC");
        CHECK_INT(track_timescale(&out, 1), rate);
        int n[2];
        counts(&out, n);
        CHECK(n[1] >= nf * 3 / 2 && n[1] <= nf * 3 / 2 + 3); /* the same samples, at their own rate */
        CHECK(track_bytes(&out, 1) / n[1] > 40);              /* a tone, not silence */
        free(in.p), free(out.p);
    }
    /* a file that says 44.1 kHz around 48 kHz frames would play too fast: refused, in words */
    static uint8_t ac3[8 * 768];
    size_t bytes = ac3_test_tone(ac3, sizeof ac3, 8);
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_AC3", NULL, 0, .arate = 44100};
    size_t seg = start_file(&in, &t, 300);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t pic[4096];
    for (int k = 0; k < 8; k++) {
        w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, k));
        w_simple(&in, 2, k * 32, true, ac3 + (size_t)k * 768, 768);
    }
    w_close(&in, cl);
    w_close(&in, seg);
    CHECK_INT((int)bytes, 8 * 768);
    W out;
    AvConvJob j = {0};
    Seen s;
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_STR(j.err, "Its Dolby sound isn't at the rate the file says");
    free(in.p), free(out.p);
    /* E-AC-3's half rates stay refused, with the rates that work named */
    Tracks half = {"V_MPEG2", NULL, 0, 48, 32, "A_EAC3", NULL, 0, .arate = 24000};
    seg = start_file(&in, &half, 300);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_STR(j.err, "Can't convert Dolby sound that isn't at 48, 44.1 or 32 kHz yet");
    free(in.p), free(out.p);
}

/* Dolby Digital Plus (E-AC-3, here in 3-block frames of 16 ms) becomes AAC too, and the file's text
   subtitles - SRT and ASS - come back as SubRip texts on the copy's clock. */
TEST(conv_turns_dolby_digital_plus_into_aac_and_keeps_text_subtitles) {
    static uint8_t eac3[48 * 384];
    size_t bytes = eac3_test_tone(eac3, sizeof eac3, 48);
    int fsize = ac3_frame_size(eac3, bytes), nf = fsize > 0 ? (int)(bytes / (size_t)fsize) : 0;
    CHECK_INT(fsize, 384);
    CHECK_INT(nf, 48);
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_EAC3", NULL, 0,
                {{"S_TEXT/UTF8", "eng", "English", false}, {"S_TEXT/ASS", "swe", NULL, true}}};
    size_t seg = start_file(&in, &t, 768);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    w_group(&in, 3, 100, 1500, "Hello\r\nthere", 12);
    const char *ass = "0,0,Default,,0,0,0,,{\\i1}Hej{\\i0}\\Nv\xC3\xA4rlden";
    w_group(&in, 4, 200, 1000, ass, strlen(ass));
    w_simple(&in, 3, 700, true, "Second", 6); /* no duration: 3 s */
    w_group(&in, 3, 750, 500, "\n\n", 2);     /* nothing to show: left out */
    static uint8_t pic[4096];
    for (int k = 0, a = 0; k < 23 || a < nf;) {
        if (k < 23 && (a >= nf || frame_ms(k) <= a * 16)) {
            w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, k));
            k++;
        } else {
            w_simple(&in, 2, a * 16, true, eac3 + (size_t)a * fsize, (size_t)fsize);
            a++;
        }
    }
    w_close(&in, cl);
    w_close(&in, seg);
    for (int want = 0; want < 2; want++) {
        W out;
        AvConvJob j = {.want_subs = want};
        Seen s;
        CHECK(run(&in, &out, &j, &s));
        CHECK_STR(j.err, "");
        CHECK_HAS(j.what, "Dolby Digital Plus → AAC");
        int n[2];
        counts(&out, n);
        CHECK(n[1] >= nf * 3 / 4 && n[1] <= nf * 3 / 4 + 3); /* 768 samples a frame, 1024 an AAC one */
        CHECK(track_bytes(&out, 1) / n[1] > 40);             /* a tone, not silence */
        CHECK_INT(j.nsubs, want ? 2 : 0);
        if (want) {
            CHECK_STR(j.subs[0].lang, "eng");
            CHECK_STR(j.subs[0].name, "English");
            CHECK(!j.subs[0].forced);
            CHECK_INT(j.subs[0].cues, 2);
            CHECK_STR(j.subs[0].srt, "1\n00:00:00,100 --> 00:00:01,600\nHello\nthere\n\n"
                                     "2\n00:00:00,700 --> 00:00:03,700\nSecond\n\n");
            CHECK_INT((int)j.subs[0].len, (int)strlen(j.subs[0].srt));
            CHECK_STR(j.subs[1].lang, "swe");
            CHECK(j.subs[1].forced);
            CHECK_STR(j.subs[1].srt, "1\n00:00:00,200 --> 00:00:01,200\nHej\nv\xC3\xA4rlden\n\n");
        }
        av_convert_free(&j);
        CHECK_INT(j.nsubs, 0);
        free(out.p);
    }
    free(in.p);
}

/* Sound the converter can't change: said in words people know, and no copy is made - but its subtitles
   still come out (1.2: they used to go with the refusal). */
TEST(conv_names_the_sound_it_cant_convert) {
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_TRUEHD", NULL, 0, {{"S_TEXT/UTF8", "eng", NULL, false}}};
    size_t seg = start_file(&in, &t, 100);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t pic[4096];
    w_simple(&in, 1, 0, true, pic, mpeg2_picture(pic, 0));
    w_group(&in, 3, 0, 500, "Hi", 2);
    w_close(&in, cl);
    w_close(&in, seg);
    W out;
    AvConvJob j = {.want_subs = true};
    Seen s;
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_STR(j.err, "Its sound is Dolby TrueHD, which the converter can't change yet (browsers play it silently)");
    CHECK_INT((int)out.len, 0);
    CHECK_INT(j.nsubs, 1);
    CHECK_STR(j.subs[0].srt, "1\n00:00:00,000 --> 00:00:00,500\nHi\n\n");
    av_convert_free(&j);
    free(in.p), free(out.p);
}

/* ------------------------------------------------------------ subtitles */

/* a PGS segment: type, u16 size, payload */
static size_t pgs_seg(uint8_t *o, int type, const uint8_t *payload, size_t n) {
    o[0] = (uint8_t)type, o[1] = (uint8_t)(n >> 8), o[2] = (uint8_t)n;
    if (n) memcpy(o + 3, payload, n);
    return 3 + n;
}

/* One PGS display set as a Blu-ray holds it: a presentation composition (number k) showing `objects` (0 = a
   clear: what was shown goes), its window, palette and object when it shows one, then the end segment. */
static size_t pgs_set(uint8_t *o, int k, int objects) {
    const uint8_t pcs[19] = {0x07, 0x80, 0x04, 0x38, 0x10, (uint8_t)(k >> 8), (uint8_t)k, 0x80, 0, 0, (uint8_t)objects,
                             0, 0, 0, 0, 0x02, 0x00, 0x03, 0xC0}; /* 1920x1080; object 0, window 0, at 512,960 */
    size_t n = pgs_seg(o, 0x16, pcs, objects ? sizeof pcs : 11);
    if (objects) {
        static const uint8_t wds[10] = {1, 0, 0x02, 0x00, 0x03, 0xC0, 0x00, 0x40, 0x00, 0x10};
        static const uint8_t pds[7] = {0, 0, 1, 235, 128, 128, 255};
        static const uint8_t ods[15] = {0, 0, 0, 0xC0, 0, 0, 8, 0x00, 0x40, 0x00, 0x10, 1, 1, 0, 0};
        n += pgs_seg(o + n, 0x17, wds, sizeof wds);
        n += pgs_seg(o + n, 0x14, pds, sizeof pds);
        n += pgs_seg(o + n, 0x15, ods, sizeof ods);
    }
    return n + pgs_seg(o + n, 0x80, NULL, 0);
}

/* A DVD subpicture unit of n bytes: its size first, then made-up bytes (the converter only carries them) */
static void spu_of(uint8_t *o, size_t n, int k) {
    o[0] = (uint8_t)(n >> 8), o[1] = (uint8_t)n;
    for (size_t i = 2; i < n; i++) o[i] = (uint8_t)(i * 7 + (size_t)k * 13);
}

/* The .sup back: records ("PG", PTS, DTS 0, a segment) one after another to the very end, their PTS and
   segment types into pts[] / types[] (up to cap). The count, or -1 when it isn't a whole .sup. */
static int read_sup(const uint8_t *p, size_t n, uint32_t *pts, uint8_t *types, int cap) {
    int k = 0;
    for (size_t at = 0; at < n; k++) {
        if (n - at < 13 || p[at] != 'P' || p[at + 1] != 'G' || be32(p + at + 6) != 0) return -1;
        size_t len = 13 + ((size_t)p[at + 11] << 8 | p[at + 12]);
        if (len > n - at) return -1;
        if (k < cap) pts[k] = be32(p + at + 2), types[k] = p[at + 10];
        at += len;
    }
    return k;
}

typedef struct {
    uint64_t pts;
    size_t at, len; /* its first pack's place in the .sub; its bytes */
    uint8_t data[8192];
} Spu;

static uint64_t pts_of(const uint8_t *q) {
    return (uint64_t)(q[0] >> 1 & 7) << 30 | (uint64_t)q[1] << 22 | (uint64_t)(q[2] >> 1) << 15 | (uint64_t)q[3] << 7 | q[4] >> 1;
}

/* The .sub back, pack by pack: each 2048 bytes - a pack header, a private stream 1 PES (substream 0x20),
   then nothing or a padding stream to its end; a PES with a PTS starts the next subpicture, the others
   carry on with its bytes. The subpictures (into s, up to cap), or -1 and why. */
static int read_sub(const uint8_t *p, size_t n, Spu *s, int cap, const char **why) {
    int k = -1;
    for (size_t at = 0; at < n; at += 2048) {
        const uint8_t *q = p + at;
        if (n - at < 2048) return *why = "a pack cut short", -1;
        if (memcmp(q, "\0\0\1\xBA", 4) || (q[4] & 0xC4) != 0x44 || (q[6] & 0x04) != 0x04 || q[13] != 0xF8)
            return *why = "no pack header", -1;
        q += 14;
        if (memcmp(q, "\0\0\1\xBD", 4) || q[6] != 0x81) return *why = "no private stream 1", -1;
        size_t pes = (size_t)q[4] << 8 | q[5], hlen = q[8];
        bool has_pts = q[7] == 0x80;
        if ((!has_pts && q[7]) || (has_pts && hlen < 5) || 3 + hlen + 1 > pes) return *why = "a bad PES header", -1;
        if (has_pts) {
            if ((q[9] & 0xF1) != 0x21 || !(q[11] & 1) || !(q[13] & 1)) return *why = "a bad PTS", -1;
            if (++k < cap) s[k].pts = pts_of(q + 9), s[k].at = at, s[k].len = 0;
        }
        if (k < 0) return *why = "a subpicture without a PTS", -1;
        for (size_t i = has_pts ? 14 : 9; i < 9 + hlen; i++)
            if (q[i] != 0xFF) return *why = "stuffing that isn't 0xFF", -1;
        if (q[9 + hlen] != 0x20) return *why = "not substream 0x20", -1;
        size_t take = pes - 3 - hlen - 1, used = 14 + 6 + pes;
        if (used > 2048) return *why = "a PES past its pack", -1;
        if (k < cap) {
            if (s[k].len + take > sizeof s[k].data) return *why = "a subpicture too big for the test", -1;
            memcpy(s[k].data + s[k].len, q + 10 + hlen, take);
            s[k].len += take;
        }
        if (used < 2048) { /* the rest: a padding stream */
            const uint8_t *pad = p + at + used;
            size_t left = 2048 - used;
            if (left < 6 || memcmp(pad, "\0\0\1\xBE", 4) || ((size_t)pad[4] << 8 | pad[5]) != left - 6)
                return *why = "a pack not filled by a padding stream", -1;
            for (size_t i = 6; i < left; i++)
                if (pad[i] != 0xFF) return *why = "padding that isn't 0xFF", -1;
        }
    }
    return k + 1;
}

/* WebVTT (S_TEXT/WEBVTT) cues become SubRip: italics, bold and underline stay, the rest of WebVTT's own
   markup goes (voices, classes, languages, ruby readings, karaoke timestamps), entities are unescaped, and
   what WebVTT keeps in BlockAdditions (cue settings, ids) changes nothing. A zlib-compressed track (none of
   the household's files have one) is left out quietly. */
TEST(conv_cleans_webvtt_into_srt) {
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0,
                {{"S_TEXT/WEBVTT", "eng", "English", false}, {"S_TEXT/UTF8", "eng", "Packed", false, .zlib = true}}};
    size_t seg = start_file(&in, &t, 300);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t pic[4096];
    w_simple(&in, 1, 0, true, pic, mpeg2_picture(pic, 0));
    const char *cues[] = {
        "<v Roger>Hello <i.loud>there</i> &amp; <c.yellow>you</c></v>",
        "<ruby>\xE6\xBC\xA2<rt>kan</rt></ruby>\xE5\xAD\x97 &lt;3&gt; &nbsp;x&lrm;y&rlm;",
        "a<00:00:01.500>b<c>c</c>\n<lang sv>hej</lang>",
        "&unknown; &amp <b>bold</b> <u>u</u> <I>shout</I>",
        "<v.loud Mary>\n\n",
    };
    for (int k = 0; k < 5; k++) {
        size_t g = w_open(&in, 0xA0); /* a BlockGroup: the Block, its BlockAdditions (settings), its length */
        w_id(&in, 0xA1);
        w_size8(&in, 4 + strlen(cues[k]));
        w_byte(&in, 0x83), w_byte(&in, 0), w_byte(&in, (k + 1) * 10), w_byte(&in, 0);
        w_put(&in, cues[k], strlen(cues[k]));
        size_t adds = w_open(&in, 0x75A1), more = w_open(&in, 0xA6);
        w_uint(&in, 0xEE, 1);
        w_bin(&in, 0xA5, "align:start line:0", 18);
        w_close(&in, more);
        w_close(&in, adds);
        w_uint(&in, 0x9B, 100);
        w_close(&in, g);
    }
    w_group(&in, 4, 20, 100, "\x78\x9C\xF3\x48\xCD\xC9\xC9\x07\x00", 9); /* (packed bytes, as zlib would leave them) */
    for (int k = 1; k < 9; k++) w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, k));
    w_close(&in, cl);
    w_close(&in, seg);
    W out;
    AvConvJob j = {.want_subs = true};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_INT(j.nsubs, 1);
    CHECK_INT(j.subs[0].kind, AV_SUB_TEXT);
    CHECK_INT(j.subs[0].cues, 4); /* the last held nothing to show */
    CHECK_STR(j.subs[0].srt, "1\n00:00:00,010 --> 00:00:00,110\nHello <i>there</i> & you\n\n"
                             "2\n00:00:00,020 --> 00:00:00,120\n\xE6\xBC\xA2\xE5\xAD\x97 <3> \xC2\xA0x\xE2\x80\x8Ey\xE2\x80\x8F\n\n"
                             "3\n00:00:00,030 --> 00:00:00,130\nabc\nhej\n\n"
                             "4\n00:00:00,040 --> 00:00:00,140\n&unknown; &amp <b>bold</b> <u>u</u> shout\n\n");
    av_convert_free(&j);
    free(in.p), free(out.p);
}

/* Blu-ray PGS (S_HDMV/PGS) becomes the bytes of a .sup file: each segment of a block (a block may hold a
   whole display set) as its own record, the block's time on the copy's clock (90 kHz; before the first
   picture: 0) - what VLC, mpv, Plex and Hemma's own player read. A segment cut short is left out. */
TEST(conv_turns_pgs_into_a_sup_file) {
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0, {{"S_HDMV/PGS", "swe", "Svenska", true}}};
    size_t seg = start_file(&in, &t, 3000);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t set[1024], pic[4096];
    w_simple(&in, 3, 100, true, set, pgs_set(set, 0, 1)); /* before the first picture (200 ms): at 0 */
    for (int k = 0; k < 60; k++) w_simple(&in, 1, 200 + frame_ms(k), true, pic, mpeg2_picture(pic, k));
    w_simple(&in, 3, 1200, true, set, pgs_set(set, 1, 0)); /* a clear: no subpicture */
    size_t n = pgs_set(set, 2, 1);                       /* one display set over two blocks */
    w_simple(&in, 3, 1500, true, set, 22 + 13);          /* (its composition and window) */
    w_simple(&in, 3, 1500, true, set + 35, n - 35);
    n = pgs_set(set, 3, 1);
    set[n] = 0x15, set[n + 1] = 0, set[n + 2] = 100; /* + an object said to hold 100 bytes, with 10 */
    w_simple(&in, 3, 2100, true, set, n + 13);
    w_close(&in, cl);
    w_close(&in, seg);
    W out;
    AvConvJob j = {.want_subs = true};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_INT(j.nsubs, 1);
    AvConvSub *o = &j.subs[0];
    CHECK_INT(o->kind, AV_SUB_PGS);
    CHECK_STR(o->lang, "swe");
    CHECK_STR(o->name, "Svenska");
    CHECK(o->forced);
    CHECK(o->srt == NULL && o->idx == NULL);
    CHECK_INT(o->cues, 3); /* compositions that show something */
    uint32_t pts[32];
    uint8_t types[32];
    CHECK_INT(read_sup(o->data, o->size, pts, types, 32), 5 + 2 + 5 + 5);
    static const uint8_t want_types[] = {0x16, 0x17, 0x14, 0x15, 0x80, 0x16, 0x80, 0x16, 0x17, 0x14, 0x15, 0x80, 0x16, 0x17, 0x14, 0x15, 0x80};
    static const uint32_t want_pts[] = {0, 0, 0, 0, 0, 90000, 90000, 117000, 117000, 117000, 117000, 117000, 171000, 171000, 171000, 171000, 171000};
    for (int k = 0; k < 17; k++) {
        CHECK_INT(types[k], want_types[k]);
        CHECK_INT(pts[k], want_pts[k]);
    }
    /* the segments are the block's bytes as they were */
    n = pgs_set(set, 0, 1);
    CHECK(!memcmp(o->data + 10, set, 22)); /* (the first: the composition, 3 + 19 bytes) */
    av_convert_free(&j);
    CHECK(o->data == NULL);
    free(in.p), free(out.p);
}

/* DVD VobSub (S_VOBSUB) becomes an .idx + .sub pair as mkvextract writes it: the .idx is the track's own
   header (size, palette) behind the line players look for, "id: en, index: 0", and one timestamp + file
   position a subpicture; the .sub an MPEG program stream of 2048-byte packs holding each subpicture whole,
   its PTS on the first. Header stripping (a ContentEncoding some muxers use) is undone. */
TEST(conv_turns_vobsub_into_idx_and_sub) {
    static const char head[] = "size: 720x480\npalette: 000000, f0f0f0, cccccc, 999999, 3333fa, 1111bb, fa3333, 7b1a1a, "
                               "3333fa, 1111bb, fa3333, 7b1a1a, 33fa33, 0b7b0b, ffffff, 000000\ncustom colors: OFF\n";
    static const char head_v7[] = "# VobSub index file, v7 (do not modify this line!)\nsize: 720x576\n";
    static const uint8_t strip[1] = {0x01}; /* every subpicture of the second track starts so (300..400 bytes) */
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0,
                {{"S_VOBSUB", "eng", NULL, false, head, sizeof head - 1},
                 {"S_VOBSUB", "fre", "Commentaire", false, head_v7, sizeof head_v7 - 1, strip, 1}}};
    size_t seg = start_file(&in, &t, 30000);
    static const size_t sizes[] = {100, 2015, 2019, 3000, 6100}; /* 1 pack (padding stream), 1 (stuffing), 1 exactly,
                                                                    2, 4 */
    static const int at_ms[] = {100, 1000, 2500, 3725, 29999};
    static uint8_t spu[5][6100], small[2][400], pic[4096];
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    for (int k = 0, f = 0; k < 5; k++) {
        for (; f < 30 && 200 + frame_ms(f) <= at_ms[k]; f++) w_simple(&in, 1, 200 + frame_ms(f), true, pic, mpeg2_picture(pic, f));
        spu_of(spu[k], sizes[k], k);
        w_group(&in, 3, at_ms[k], 2000, spu[k], sizes[k]);
        if (k < 2) {
            spu_of(small[k], 300 + (size_t)k * 100, 7 + k);
            w_group(&in, 4, at_ms[k] + 1, 0, small[k] + 1, 300 + (size_t)k * 100 - 1); /* (stripped: the first byte) */
        }
    }
    w_close(&in, cl);
    w_close(&in, seg);
    W out;
    AvConvJob j = {.want_subs = true};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_INT(j.nsubs, 2);
    AvConvSub *o = &j.subs[0];
    CHECK_INT(o->kind, AV_SUB_VOBSUB);
    CHECK_INT(o->cues, 5);
    CHECK(o->srt == NULL);
    CHECK_INT((int)(o->size % 2048), 0);
    CHECK_INT((int)o->size, (1 + 1 + 1 + 2 + 4) * 2048);
    static Spu got[8];
    const char *why = "";
    int n = read_sub(o->data, o->size, got, 8, &why);
    if (n < 0) test_fail(__FILE__, __LINE__, "the .sub: %s", why);
    CHECK_INT(n, 5);
    static const size_t at[] = {0, 2048, 4096, 6144, 10240}; /* where each starts */
    for (int k = 0; k < 5; k++) {
        CHECK_INT((int)got[k].len, (int)sizes[k]);
        CHECK(!memcmp(got[k].data, spu[k], sizes[k])); /* whole, byte for byte */
        CHECK_INT((long long)got[k].pts, k ? (at_ms[k] - 200) * 90LL : 0);
        CHECK_INT((int)got[k].at, (int)at[k]);
    }
    char want[1024];
    snprintf(want, sizeof want,
             "# VobSub index file, v7 (do not modify this line!)\n%s\nid: en, index: 0\n"
             "timestamp: 00:00:00:000, filepos: 000000000\ntimestamp: 00:00:00:800, filepos: 000000800\n"
             "timestamp: 00:00:02:300, filepos: 000001000\ntimestamp: 00:00:03:525, filepos: 000001800\n"
             "timestamp: 00:00:29:799, filepos: 000002800\n",
             head);
    CHECK_STR(o->idx, want);
    CHECK_INT((int)o->idx_len, (int)strlen(want));
    /* the second: its own v7 line kept (not twice), French by its two letters, the stripped byte back */
    o = &j.subs[1];
    CHECK_STR(o->name, "Commentaire");
    CHECK_STR(o->idx, "# VobSub index file, v7 (do not modify this line!)\nsize: 720x576\n\nid: fr, index: 0\n"
                      "timestamp: 00:00:00:000, filepos: 000000000\ntimestamp: 00:00:00:801, filepos: 000000800\n");
    n = read_sub(o->data, o->size, got, 8, &why);
    CHECK_INT(n, 2);
    CHECK(got[0].len == 300 && !memcmp(got[0].data, small[0], 300));
    CHECK(got[1].len == 400 && !memcmp(got[1].data, small[1], 400));
    av_convert_free(&j);
    free(in.p), free(out.p);
}

/* 1.2: 32 subtitle tracks (the Simpsons carry 13), the first 32 in file order; past them, left out. */
TEST(conv_takes_32_subtitle_tracks) {
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0};
    static char names[MANY_SUBS][16], cue[MANY_SUBS][16];
    for (int k = 0; k < MANY_SUBS; k++) {
        snprintf(names[k], sizeof names[k], "Track %d", k + 1);
        snprintf(cue[k], sizeof cue[k], "Line %d", k + 1);
        t.sub[k].codec = k % 2 ? "S_TEXT/ASS" : "S_TEXT/UTF8";
        t.sub[k].lang = "eng";
        t.sub[k].name = names[k];
    }
    size_t seg = start_file(&in, &t, 300);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t pic[4096];
    w_simple(&in, 1, 0, true, pic, mpeg2_picture(pic, 0));
    for (int k = 0; k < MANY_SUBS; k++) {
        char ass[64];
        snprintf(ass, sizeof ass, "%d,0,Default,,0,0,0,,%.15s", k, cue[k]);
        const char *text = k % 2 ? ass : cue[k];
        w_group(&in, 3 + k, 10 + k, 100, text, strlen(text));
    }
    for (int k = 1; k < 9; k++) w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, k));
    w_close(&in, cl);
    w_close(&in, seg);
    W out;
    AvConvJob j = {.want_subs = true};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_INT(j.nsubs, AV_CONV_MAX_SUBS);
    CHECK_INT(AV_CONV_MAX_SUBS, 32);
    for (int k = 0; k < 32; k++) {
        CHECK_STR(j.subs[k].name, names[k]);
        CHECK_HAS(j.subs[k].srt, cue[k]);
        CHECK(strstr(j.subs[k].srt, "\n\n") == j.subs[k].srt + j.subs[k].len - 2); /* its own cue only */
    }
    av_convert_free(&j);
    free(in.p), free(out.p);
}

/* HEMMA_TEST_SUBS=<an .mkv> (opt-in, never in the suite): av_subtitles over a real file, each track written
   to HEMMA_TEST_SUBS_OUT (a folder; default: this test's scratch folder, removed after) as
   sub-<k>.<language>.srt / .sup / .idx + .sub - to open in VLC or ffprobe. Reads the file, never writes
   next to it. */
static bool real_read(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

TEST(conv_subtitles_of_a_real_file_when_given) {
    const char *path = plat_getenv("HEMMA_TEST_SUBS");
    if (!path || !*path) return;
    const char *dir = plat_getenv("HEMMA_TEST_SUBS_OUT");
    if (!dir || !*dir) dir = test_tmpdir();
    CHECK(dir_make(dir));
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    AvConvJob j = {.in = {fp, size, real_read}};
    int64_t t0 = now_ms();
    bool ok = av_subtitles(&j);
    fclose(fp);
    if (!ok) test_fail(__FILE__, __LINE__, "av_subtitles: %s", j.err);
    CHECK(ok);
    fprintf(stderr, "    %d subtitle track(s) in %lld ms (%lld MB read) -> %s\n", j.nsubs, (long long)(now_ms() - t0),
            (long long)(size >> 20), dir);
    for (int k = 0; k < j.nsubs; k++) {
        const AvConvSub *s = &j.subs[k];
        static const char *const ext[] = {"srt", "sup", "sub"};
        char out[1200];
        snprintf(out, sizeof out, "%s/sub-%d.%s.%s", dir, k, s->lang, ext[s->kind]);
        CHECK(file_write_atomic(out, s->kind == AV_SUB_TEXT ? (const void *)s->srt : s->data, s->kind == AV_SUB_TEXT ? s->len : s->size));
        if (s->kind == AV_SUB_VOBSUB) {
            snprintf(out, sizeof out, "%s/sub-%d.%s.idx", dir, k, s->lang);
            CHECK(file_write_atomic(out, s->idx, s->idx_len));
        }
        fprintf(stderr, "    %d: %s %s \"%s\"%s%s: %d cue(s), %zu bytes\n", k, ext[s->kind], s->lang, s->name, s->forced ? " forced" : "",
                s->is_default ? " default" : "", s->cues, s->kind == AV_SUB_TEXT ? s->len : s->size);
    }
    av_convert_free(&j);
}

/* ------------------------------------------------- files built from streams of blocks */

#define MAX_BLOCKS 640

typedef struct { /* one track's blocks: their bytes one after another, times and key flags */
    W data;
    size_t at[MAX_BLOCKS + 1];
    int ms[MAX_BLOCKS], n;
    bool key[MAX_BLOCKS];
} Stream;

static void s_add(Stream *s, const void *p, size_t n, int ms, bool key) {
    if (s->n == MAX_BLOCKS) return;
    w_put(&s->data, p, n);
    s->ms[s->n] = ms, s->key[s->n] = key;
    s->at[++s->n] = s->data.len;
}

static void s_free(Stream *s) {
    free(s->data.p);
    memset(s, 0, sizeof *s);
}

static const uint8_t *s_block(const Stream *s, int b) { return s->data.p ? s->data.p + s->at[b] : NULL; }
static size_t s_size(const Stream *s, int b) { return s->at[b + 1] - s->at[b]; }

/* the video blocks (track 1) and the sound's (track 2; NULL: none), interleaved by time like a
   real file, in one cluster (times up to 32 s) */
static void mux(W *w, const Tracks *t, double ms, const Stream *v, const Stream *a) {
    size_t seg = start_file(w, t, ms);
    size_t cl = w_open(w, 0x1F43B675);
    w_uint(w, 0xE7, 0);
    for (int i = 0, k = 0; i < v->n || (a && k < a->n);) {
        bool vid = i < v->n && (!a || k >= a->n || v->ms[i] <= a->ms[k]);
        const Stream *s = vid ? v : a;
        int b = vid ? i++ : k++;
        w_simple(w, vid ? 1 : 2, s->ms[b], s->key[b], s_block(s, b), s_size(s, b));
    }
    w_close(w, cl);
    w_close(w, seg);
}

/* MPEG-2 pictures k0 .. k0+n-1 (see mpeg2_picture_of). cadence: only every 5th's third and
   fourth combed - the 3:2 pattern of film telecined to 29.97 */
static void add_mpeg2(Stream *s, int k0, int n, int width, int height, bool il, int comb, bool cadence) {
    static uint8_t pic[4096];
    for (int k = k0; k < k0 + n; k++) {
        int c = !cadence || k % 5 == 2 || k % 5 == 3 ? comb : 0;
        s_add(s, pic, mpeg2_picture_of(pic, k, width, height, il, c), frame_ms(k), true);
    }
}

/* n frames of H.264 (64x48, a keyframe every 5); its avcC record goes to avcc */
static void add_h264(Stream *s, int n, uint8_t avcc[256], size_t *avcc_len) {
    H264Params hp = {.width = 64, .height = 48, .fps_num = 30000, .fps_den = 1001, .keyint = 5, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    *avcc_len = h264enc_avcc(e, avcc, 256);
    static uint8_t y[64 * 48], u[32 * 24], v[32 * 24];
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < 64 * 48; i++) y[i] = (uint8_t)((i % 64 * 3 + i / 64 + k * 5) & 0xFF);
        memset(u, 90 + k, sizeof u), memset(v, 160 - k, sizeof v);
        H264Picture p = {y, u, v, 64, 32, 32, k};
        H264Packet pk;
        if (h264enc_encode(e, &p, &pk)) s_add(s, pk.data, pk.size, frame_ms(k), pk.idr);
    }
    h264enc_free(e);
}

/* (2.5) blocks 0 .. n-1 of mpeg4_testenc.h's MPEG-4 Part 2 stream (I P B B P B B ... in decoding order), each at
   ms_of(its place in display order) - packed: DivX's blocks, each at its slot's. Returns its VOL (CodecPrivate) in vol. */
static size_t add_mpeg4(Stream *s, int n, int width, int height, unsigned flags, bool packed, int (*ms_of)(int),
                        uint8_t vol[64]) {
    static uint8_t blk[32768];
    for (int k = 0; k < n; k++) {
        int disp;
        size_t len = mpeg4_test_block(blk, sizeof blk, k, width, height, flags, packed, &disp);
        s_add(s, blk, len, ms_of(disp), k == 0);
    }
    static M4w w;
    memset(&w, 0, sizeof w);
    m4_vol(&w, width, height, flags);
    memcpy(vol, w.b, m4_bytes(&w));
    return m4_bytes(&w);
}

/* (2.6) hevc_testenc.h's stream as an MKV track: each picture a block (4-byte lengths) at its display slot's ms.
   Returns its hvcC (CodecPrivate) in hvcc. */
static size_t add_hevc(Stream *s, const HwStream *hs, uint8_t *hvcc, size_t cap) {
    static uint8_t au[1 << 17];
    for (int k = 0; k < hw_count(hs); k++) {
        int poc;
        size_t len = hw_picture(hs, k, false, au, sizeof au, &poc);
        s_add(s, au, len, frame_ms(poc), k == 0);
    }
    return hw_hvcc(hs, hvcc, cap);
}

/* (2.6.1) a Stream as an MP4's only track (mp4.c's writer, 1 ms ticks): the blocks in their order, each block's ms its
   presentation time (the decode times their sorted ms, the edit list's start what keeps composition offsets >= 0);
   shown_ms > 0: the edit list stops showing it there */
static void mp4_of(W *w, int codec, const uint8_t *cfg, size_t cfg_len, int width, int height, const Stream *v, int shown_ms) {
    memset(w, 0, sizeof *w);
    static int dts[MAX_BLOCKS];
    for (int i = 0; i < v->n; i++) { /* (insertion sort: a few blocks) */
        int k = i;
        for (; k > 0 && dts[k - 1] > v->ms[i]; k--) dts[k] = dts[k - 1];
        dts[k] = v->ms[i];
    }
    int shift = 0;
    for (int i = 0; i < v->n; i++) shift = dts[i] - v->ms[i] > shift ? dts[i] - v->ms[i] : shift;
    Mp4Out o = {w, mem_write, mem_write_at};
    Mp4 *m = mp4_new(&o, 0, 1 << 16);
    Mp4Track t = {.codec = codec, .timescale = 1000, .config = cfg, .config_len = cfg_len, .width = width,
                  .height = height, .start = shift, .end = shown_ms > 0 ? shift + shown_ms : 0};
    bool ok = mp4_add_track(m, &t) == 0 && mp4_begin(m);
    for (int i = 0; ok && i < v->n; i++) {
        int next = i + 1 < v->n ? dts[i + 1] : dts[i] + (i ? dts[i] - dts[i - 1] : 40);
        ok = mp4_sample(m, 0, s_block(v, i), s_size(v, i), (uint32_t)(next - dts[i]), v->ms[i] - dts[i] + shift, v->key[i]);
    }
    CHECK(ok && mp4_finish(m));
    mp4_free(m);
}

static HwStream hevc_stream(int w, int h, int depth, unsigned flags, int frames) {
    HwStream hs;
    memset(&hs, 0, sizeof hs);
    hs.width = w, hs.height = h, hs.depth = depth, hs.chroma = 1, hs.flags = flags, hs.frames = frames, hs.seed = 11;
    hs.mv[0][0] = 2, hs.mv[1][1] = -2;
    return hs;
}

/* An AVI's track as mkvmerge keeps it (V_MS/VFW/FOURCC): a BITMAPINFOHEADER as CodecPrivate */
static void bitmapinfo(uint8_t bih[40], int width, int height, const char *fourcc) {
    memset(bih, 0, 40);
    bih[0] = 40, bih[4] = (uint8_t)width, bih[5] = (uint8_t)(width >> 8), bih[8] = (uint8_t)height, bih[9] = (uint8_t)(height >> 8);
    bih[12] = 1, bih[14] = 24;
    memcpy(bih + 16, fourcc, 4);
}

/* `seconds` of AAC; its AudioSpecificConfig goes to asc */
static void add_aac(Stream *s, double seconds, uint8_t asc[2]) {
    static uint8_t aus[200][AAC_MAX_AU];
    int sizes[200], n = make_aac(aus, sizes, 200, seconds, asc);
    for (int a = 0; a < n; a++) s_add(s, aus[a], (size_t)sizes[a], au_ms(a), true);
}

/* `frames` DTS frames of 512 samples (dts_testenc.h's tone: 5.1, or stereo), every one a block */
static void add_dts(Stream *s, int frames, bool surround) {
    static uint8_t dts[96 * 2048];
    size_t len = dts_test_tone(dts, sizeof dts, frames, surround), at = 0;
    for (int f = 0; at < len; f++) {
        int n = dts_frame_size(dts + at, len - at);
        if (n <= 0) break;
        s_add(s, dts + at, (size_t)n, f * 32 / 3, true); /* (10.67 ms a frame) */
        at += (size_t)n;
    }
}

/* 1.3: DTS sound (Blu-ray rips: 5.1 at 48 kHz) becomes stereo AAC at its rate; the picture is copied */
TEST(conv_turns_dts_into_aac) {
    enum { NF = 60 }; /* 0.64 s */
    for (int surround = 0; surround < 2; surround++) {
        static Stream v, a;
        uint8_t avcc[256];
        size_t avcc_len = 0;
        add_h264(&v, 20, avcc, &avcc_len);
        add_dts(&a, NF, surround);
        CHECK_INT(a.n, NF);
        Tracks t = {"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_DTS", NULL, 0};
        W in, out;
        mux(&in, &t, v.n * 33.4, &v, &a);
        AvConvJob j = {0};
        Seen s;
        CHECK(run(&in, &out, &j, &s));
        CHECK_STR(j.err, "");
        CHECK_STR(j.what, "H.264 copied · DTS → AAC");
        int n[2];
        counts(&out, n);
        CHECK_INT(n[0], 20);
        CHECK(n[1] >= NF / 2 && n[1] <= NF / 2 + 3); /* 512 samples a DTS frame, 1024 an AAC one */
        CHECK(track_bytes(&out, 1) / n[1] > 40);      /* a tone, not silence */
        free(in.p), free(out.p);
        s_free(&v), s_free(&a);
    }
}

/* ------------------------------------------------- several sound tracks (2.2) */

typedef struct { /* one sound of a file with several (tracks 2, 3 ...) */
    const char *codec, *lang, *ietf, *name; /* lang / ietf / name NULL: not written */
    const uint8_t *priv;
    size_t priv_len;
    int is_default; /* its FlagDefault (written always) */
    Stream s;
} Snd;

/* an H.264 picture (track 1) and n sounds, their blocks interleaved by time in one cluster */
static void mux_sounds(W *w, const uint8_t *avcc, size_t avcc_len, const Stream *v, const Snd *snd, int n) {
    memset(w, 0, sizeof *w);
    size_t e = w_open(w, 0x1A45DFA3);
    w_uint(w, 0x4286, 1);
    w_bin(w, 0x4282, "matroska", 8);
    w_uint(w, 0x4287, 4);
    w_close(w, e);
    size_t seg = w_open(w, 0x18538067);
    size_t info = w_open(w, 0x1549A966);
    w_uint(w, 0x2AD7B1, 1000000);
    w_float(w, 0x4489, v->n * 33.4);
    w_close(w, info);
    size_t tracks = w_open(w, 0x1654AE6B);
    size_t tr = w_open(w, 0xAE);
    w_uint(w, 0xD7, 1);
    w_uint(w, 0x83, 1);
    w_bin(w, 0x86, "V_MPEG4/ISO/AVC", 15);
    w_bin(w, 0x63A2, avcc, avcc_len);
    size_t vv = w_open(w, 0xE0);
    w_uint(w, 0xB0, 64);
    w_uint(w, 0xBA, 48);
    w_close(w, vv);
    w_close(w, tr);
    for (int k = 0; k < n; k++) {
        const Snd *s = &snd[k];
        tr = w_open(w, 0xAE);
        w_uint(w, 0xD7, (uint64_t)(2 + k));
        w_uint(w, 0x83, 2);
        w_bin(w, 0x86, s->codec, strlen(s->codec));
        if (s->priv) w_bin(w, 0x63A2, s->priv, s->priv_len);
        if (s->lang) w_bin(w, 0x22B59C, s->lang, strlen(s->lang));
        if (s->ietf) w_bin(w, 0x22B59D, s->ietf, strlen(s->ietf));
        if (s->name) w_bin(w, 0x536E, s->name, strlen(s->name));
        w_uint(w, 0x88, (uint64_t)s->is_default);
        size_t a = w_open(w, 0xE1);
        w_float(w, 0xB5, 48000.0);
        w_uint(w, 0x9F, 2);
        w_close(w, a);
        w_close(w, tr);
    }
    w_close(w, tracks);
    size_t cl = w_open(w, 0x1F43B675);
    w_uint(w, 0xE7, 0);
    int at[16] = {0}, i = 0;
    for (;;) { /* the earliest next block (the picture first on a tie) */
        int best = -1, best_ms = 0;
        if (i < v->n) best = 0, best_ms = v->ms[i];
        for (int k = 0; k < n; k++)
            if (at[k] < snd[k].s.n && (best < 0 || snd[k].s.ms[at[k]] < best_ms)) best = 1 + k, best_ms = snd[k].s.ms[at[k]];
        if (best < 0) break;
        if (!best) w_simple(w, 1, v->ms[i], v->key[i], s_block(v, i), s_size(v, i)), i++;
        else {
            const Stream *s = &snd[best - 1].s;
            int b = at[best - 1]++;
            w_simple(w, 1 + best, s->ms[b], s->key[b], s_block(s, b), s_size(s, b));
        }
    }
    w_close(w, cl);
    w_close(w, seg);
}

/* `frames` Dolby frames of the test tone (32 ms each) from block `from` on; rate: 48000, or 44100 for frames
   that say another rate than the file */
static void add_ac3(Stream *s, int from, int frames, int rate) {
    static uint8_t ac3[64 * 1600];
    size_t len = rate == 48000 ? ac3_test_tone(ac3, sizeof ac3, frames) : ac3_test_tone_at(ac3, sizeof ac3, frames, rate);
    for (size_t at = 0, f = 0; at < len; f++) {
        int n = ac3_frame_size(ac3 + at, len - at);
        if (n <= 0) break;
        s_add(s, ac3 + at, (size_t)n, (int)(from + f) * 32, true);
        at += (size_t)n;
    }
}

/* the MP4's boxes: a box's payload, its k-th child of a type, a path under it ("mdia/minf/stbl/stsz") */
typedef struct {
    const uint8_t *p;
    size_t n;
} Bx;

static bool bx_child(Bx in, const char *type, int k, Bx *out) {
    for (size_t i = 0; i + 8 <= in.n;) {
        size_t sz = be32(in.p + i);
        if (sz < 8 || sz > in.n - i) return false;
        if (!memcmp(in.p + i + 4, type, 4) && k-- == 0) return *out = (Bx){in.p + i + 8, sz - 8}, true;
        i += sz;
    }
    return false;
}

static bool bx_path(Bx in, const char *path, Bx *out) {
    char seg[5] = "";
    for (const char *s = path; *s; s += s[4] == '/' ? 5 : 4) {
        memcpy(seg, s, 4);
        if (!bx_child(in, seg, 0, &in)) return false;
        if (!s[4]) break;
    }
    return *out = in, true;
}

/* the k-th trak of the moov */
static bool trak_k(const W *w, int k, Bx *t) {
    size_t moov = box(w, "moov");
    return moov && bx_child((Bx){w->p + moov + 8, be32(w->p + moov) - 8}, "trak", k, t);
}

typedef struct {
    uint32_t flags; /* tkhd: 3 = enabled + in movie, 2 = in movie */
    int group;      /* its alternate group */
    char lang[4], name[96];
    uint32_t samples, first_run; /* stsz's count; samples in its first chunks (stsc's first entry) */
} TrakFacts;

static bool trak_facts(const W *w, int k, TrakFacts *f) {
    memset(f, 0, sizeof *f);
    Bx t, b;
    if (!trak_k(w, k, &t)) return false;
    if (bx_path(t, "tkhd", &b)) {
        f->flags = be32(b.p) & 0xFFFFFF;
        const uint8_t *q = b.p + (b.p[0] == 1 ? 36 : 24) + 8 + 2;
        f->group = q[0] << 8 | q[1];
    }
    if (bx_path(t, "mdia/mdhd", &b)) {
        const uint8_t *q = b.p + (b.p[0] == 1 ? 32 : 20);
        int v = q[0] << 8 | q[1];
        f->lang[0] = (char)(0x60 + (v >> 10 & 31)), f->lang[1] = (char)(0x60 + (v >> 5 & 31)), f->lang[2] = (char)(0x60 + (v & 31));
    }
    if (bx_path(t, "mdia/hdlr", &b) && b.n > 24) snprintf(f->name, sizeof f->name, "%.*s", (int)(b.n - 24), (const char *)b.p + 24);
    if (bx_path(t, "mdia/minf/stbl/stsz", &b)) f->samples = be32(b.p + 8);
    if (bx_path(t, "mdia/minf/stbl/stsc", &b) && be32(b.p + 4)) f->first_run = be32(b.p + 12);
    return true;
}

/* where sample s of the k-th trak lies in the file (its size too); -1 = none */
static int64_t trak_sample(const W *w, int k, uint32_t s, uint32_t *size) {
    Bx t, stsz, stsc, co;
    bool c64 = false;
    if (!trak_k(w, k, &t) || !bx_path(t, "mdia/minf/stbl/stsz", &stsz) || !bx_path(t, "mdia/minf/stbl/stsc", &stsc)) return -1;
    if (!bx_path(t, "mdia/minf/stbl/stco", &co)) {
        if (!bx_path(t, "mdia/minf/stbl/co64", &co)) return -1;
        c64 = true;
    }
    uint32_t runs = be32(stsc.p + 4), chunks = be32(co.p + 4), at = 0;
    for (uint32_t c = 1; c <= chunks; c++) {
        uint32_t per = 0;
        for (uint32_t r = 0; r < runs; r++)
            if (be32(stsc.p + 8 + r * 12) <= c) per = be32(stsc.p + 8 + r * 12 + 4);
        int64_t off = c64 ? (int64_t)((uint64_t)be32(co.p + 8 + (c - 1) * 8) << 32 | be32(co.p + 12 + (c - 1) * 8))
                          : (int64_t)be32(co.p + 8 + (c - 1) * 4);
        for (uint32_t i = 0; i < per; i++, at++) {
            uint32_t sz = be32(stsz.p + 12 + at * 4);
            if (at == s) return *size = sz, off;
            off += sz;
        }
    }
    return -1;
}

/* 2.2 (owner: "not language locked"): every sound the converter takes becomes its own track, in the file's
   order - AAC copied byte for byte, Dolby made AAC -, each with its language (an IETF tag read as one) and name,
   only the default one enabled; what it can't take (Opus) is left out; the runs keep the chunk tables small. */
TEST(conv_keeps_every_sound_track) {
    static Stream v;
    static Snd snd[5];
    static uint8_t aus[200][AAC_MAX_AU];
    int sizes[200];
    uint8_t avcc[256], asc[2];
    size_t avcc_len = 0;
    memset(snd, 0, sizeof snd);
    add_h264(&v, 60, avcc, &avcc_len); /* 2 s */
    int naus = make_aac(aus, sizes, 200, 2.0, asc);
    for (int a = 0; a < naus; a++) s_add(&snd[0].s, aus[a], (size_t)sizes[a], au_ms(a), true), s_add(&snd[3].s, aus[a], (size_t)sizes[a], au_ms(a), true);
    add_ac3(&snd[1].s, 0, 40, 48000);
    for (int b = 0; b < 20; b++) s_add(&snd[2].s, "opus bytes", 10, b * 20, true);
    add_ac3(&snd[4].s, 0, 40, 48000);
    snd[0].codec = "A_AAC", snd[0].lang = "eng", snd[0].name = "English", snd[0].priv = asc, snd[0].priv_len = 2;
    snd[1].codec = "A_AC3", snd[1].lang = "rus", snd[1].name = "Русский дубляж", snd[1].is_default = 1;
    snd[2].codec = "A_OPUS", snd[2].lang = "fin";
    snd[3].codec = "A_AAC", snd[3].ietf = "sv-SE", snd[3].name = "Svenska", snd[3].priv = asc, snd[3].priv_len = 2;
    snd[4].codec = "A_AC3", snd[4].lang = "ukr";
    W in, out;
    mux_sounds(&in, avcc, avcc_len, &v, snd, 5);
    AvConvJob j = {0};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_STR(j.err, "");
    CHECK_STR(j.what, "H.264 copied · Dolby Digital → AAC (+3 more sound tracks)");
    CHECK(box(&out, "moov") < box(&out, "mdat")); /* the index fitted its room: it comes first */
    static const struct {
        uint32_t flags;
        int group;
        const char *lang, *name;
    } WANT[] = {{3, 0, "und", "VideoHandler"}, {2, 1, "eng", "English"}, {3, 1, "rus", "Русский дубляж"},
                {2, 1, "swe", "Svenska"},      {2, 1, "ukr", "SoundHandler"}};
    TrakFacts f;
    for (int k = 0; k < 5; k++) {
        CHECK(trak_facts(&out, k, &f));
        CHECK_INT((int)f.flags, (int)WANT[k].flags);
        CHECK_INT(f.group, WANT[k].group);
        CHECK_STR(f.lang, WANT[k].lang);
        CHECK_STR(f.name, WANT[k].name);
        if (k == 1 || k == 3) {
            CHECK_INT((int)f.samples, naus);
            CHECK_INT((int)f.first_run, 48); /* a run of ~1 s a chunk */
        }
        if (k == 2 || k == 4) {
            CHECK((int)f.samples >= 60 && (int)f.samples <= 63); /* 40 x 1536 = 60 x 1024, and the tail */
            CHECK_INT((int)f.first_run, 48);
        }
    }
    CHECK(!trak_facts(&out, 5, &f)); /* (no Opus) */
    for (int k = 1; k <= 3; k += 2) /* the copied sounds: the very same frames */
        for (int a = 0; a < naus; a++) {
            uint32_t size = 0;
            int64_t off = trak_sample(&out, k, (uint32_t)a, &size);
            CHECK(off > 0 && (int)size == sizes[a] && !memcmp(out.p + off, aus[a], size));
        }
    CHECK(track_bytes(&out, 2) / 60 > 40 && track_bytes(&out, 4) / 60 > 40); /* the Dolby ones: a tone, not silence */
    free(in.p), free(out.p);
    s_free(&v);
    for (int k = 0; k < 5; k++) s_free(&snd[k].s);
}

/* A sound that goes undecodable part-way (here: Dolby frames at another rate than the file says) is dropped
   while another lives - the default one too, and another takes its place; alone it refuses the file, in the
   words it always did. */
TEST(conv_drops_a_sound_that_goes_undecodable) {
    static Stream v;
    static Snd snd[2];
    static uint8_t aus[200][AAC_MAX_AU];
    int sizes[200];
    uint8_t avcc[256], asc[2];
    size_t avcc_len = 0;
    add_h264(&v, 45, avcc, &avcc_len);
    int naus = make_aac(aus, sizes, 200, 1.5, asc);
    for (int def = 0; def < 3; def++) { /* the AAC the default / the Dolby the default / the Dolby alone */
        memset(snd, 0, sizeof snd);
        for (int a = 0; a < naus; a++) s_add(&snd[0].s, aus[a], (size_t)sizes[a], au_ms(a), true);
        add_ac3(&snd[1].s, 0, 20, 48000);
        add_ac3(&snd[1].s, 20, 20, 44100);
        snd[0].codec = "A_AAC", snd[0].lang = "eng", snd[0].priv = asc, snd[0].priv_len = 2, snd[0].is_default = def == 0;
        snd[1].codec = "A_AC3", snd[1].lang = "rus", snd[1].is_default = def >= 1;
        W in, out;
        if (def < 2) mux_sounds(&in, avcc, avcc_len, &v, snd, 2);
        else mux_sounds(&in, avcc, avcc_len, &v, snd + 1, 1);
        AvConvJob j = {0};
        Seen s;
        if (def == 2) {
            CHECK(!run(&in, &out, &j, &s));
            CHECK(j.unsupported);
            CHECK_STR(j.err, "Its Dolby sound isn't at the rate the file says");
        } else {
            CHECK(run(&in, &out, &j, &s));
            CHECK_STR(j.err, "");
            TrakFacts f;
            CHECK(trak_facts(&out, 1, &f));
            CHECK_STR(f.lang, "eng");
            CHECK_INT((int)f.flags, 3); /* the default - or, its default gone, the one left */
            CHECK_INT((int)f.samples, naus);
            CHECK(!trak_facts(&out, 2, &f)); /* the Dolby one left out of the index */
        }
        free(in.p), free(out.p);
        for (int k = 0; k < 2; k++) s_free(&snd[k].s);
    }
    s_free(&v);
}

/* At most 8 sounds: the first eight in the file's order - or, when the default comes later, it takes the eighth's
   place. */
TEST(conv_keeps_eight_sounds) {
    static const char *const LANGS[10] = {"eng", "swe", "nor", "dan", "fin", "ger", "fre", "spa", "ita", "dut"};
    static Stream v;
    static Snd snd[10];
    static uint8_t aus[200][AAC_MAX_AU];
    int sizes[200];
    uint8_t avcc[256], asc[2];
    size_t avcc_len = 0;
    add_h264(&v, 15, avcc, &avcc_len);
    int naus = make_aac(aus, sizes, 200, 0.5, asc);
    for (int late = 0; late < 2; late++) {
        memset(snd, 0, sizeof snd);
        for (int k = 0; k < 10; k++) {
            for (int a = 0; a < naus; a++) s_add(&snd[k].s, aus[a], (size_t)sizes[a], au_ms(a), true);
            snd[k].codec = "A_AAC", snd[k].lang = LANGS[k], snd[k].priv = asc, snd[k].priv_len = 2;
            snd[k].is_default = late && k == 9;
        }
        W in, out;
        mux_sounds(&in, avcc, avcc_len, &v, snd, 10);
        AvConvJob j = {0};
        Seen s;
        CHECK(run(&in, &out, &j, &s));
        CHECK_HAS(j.what, "(+7 more sound tracks)");
        TrakFacts f;
        for (int k = 1; k <= 8; k++) {
            CHECK(trak_facts(&out, k, &f));
            const char *want = late && k == 8 ? "dut" : LANGS[k - 1];
            CHECK_STR(f.lang, want);
            CHECK_INT((int)f.flags, (late ? k == 8 : k == 1) ? 3 : 2);
            CHECK_INT((int)f.samples, naus);
        }
        CHECK(!trak_facts(&out, 9, &f));
        free(in.p), free(out.p);
        for (int k = 0; k < 10; k++) s_free(&snd[k].s);
    }
    s_free(&v);
}

/* MPEG-2 coded interlaced, as most DVDs are: fields from different instants throughout are
   deinterlaced (one picture each), film telecined to 29.97 goes back to 23.976 (one picture in
   five dropped) - picked by the analysis, as for a real disc. */
TEST(conv_deinterlaces_video_and_inverse_telecines_film) {
    for (int film = 0; film < 2; film++) {
        static Stream v;
        add_mpeg2(&v, 0, 60, 64, 32, true, 70, film);
        W in, out;
        Tracks t = {"V_MPEG2", NULL, 0, 64, 32, NULL, NULL, 0};
        mux(&in, &t, 2000, &v, NULL);
        AvConvJob j = {0};
        Seen s;
        CHECK(run(&in, &out, &j, &s));
        CHECK_STR(j.err, "");
        CHECK_HAS(j.what, film ? "(film, 23.976 fps)" : "(video, 29.970 fps)");
        int n[2];
        counts(&out, n);
        if (film) CHECK(n[0] >= 60 * 4 / 5 - 2 && n[0] <= 60 * 4 / 5 + 1);
        else CHECK_INT(n[0], 60);
        free(in.p), free(out.p);
        s_free(&v);
    }
}

/* Issue #4: a later sequence header that changes the picture size. Everything after the decoder
   (deinterlacer, encoder) is set up for the first size; the decoder makes its pictures the new
   size. Same width with fewer rows used to be read past the end of the decoder's picture (a heap
   overflow); now any change stops the job cleanly, saying why. */
TEST(conv_refuses_a_picture_that_changes_size) {
    static const struct {
        int w, h;
        bool il;
        const char *says;
    } later[] = {
        {48, 16, false, "48×32, later 48×16"}, /* same width, fewer rows: the overflow */
        {48, 64, false, "48×32, later 48×64"}, /* more rows */
        {32, 32, false, "48×32, later 32×32"}, /* narrower */
        {64, 16, false, "48×32, later 64×16"}, /* wider and fewer rows */
        {48, 32, true, NULL},                  /* the same size, now interlaced: fine */
    };
    for (int i = 0; i < (int)(sizeof later / sizeof *later); i++) {
        static Stream v;
        add_mpeg2(&v, 0, 20, 48, 32, false, 0, false);
        add_mpeg2(&v, 20, 20, later[i].w, later[i].h, later[i].il, 0, false);
        W in, out;
        Tracks t = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0};
        mux(&in, &t, 1400, &v, NULL);
        AvConvJob j = {0};
        Seen s;
        bool ok = run(&in, &out, &j, &s);
        if (later[i].says) {
            CHECK(!ok);
            CHECK(j.unsupported); /* the browser sends the original instead */
            CHECK_HAS(j.err, "changes size");
            CHECK_HAS(j.err, later[i].says);
        } else {
            CHECK(ok);
            int n[2];
            counts(&out, n);
            CHECK_INT(n[0], 40);
        }
        free(in.p), free(out.p);
        s_free(&v);
    }
}

/* one block of 256 empty frames (fixed lacing): 14 bytes of file, 256 samples */
static void w_laced_empty(W *w, int track) {
    w_id(w, 0xA3);
    w_size8(w, 5);
    w_byte(w, 0x80 | track);
    w_byte(w, 0), w_byte(w, 0); /* at the cluster's time */
    w_byte(w, 0x80 | 0x04);     /* key, fixed-size lacing */
    w_byte(w, 255);             /* 256 frames */
}

static const uint8_t AVCC[] = {1, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0, 0}; /* copied as it is: any bytes do */

/* Issue #4, part two: every sample keeps ~20-40 bytes of index in memory until the end, so a file
   of tiny blocks (or of laced empty frames, 256 in 14 bytes) could need many times its own size
   in RAM and get a 1 GB Pi's server killed. A track gets at most 300 pictures / 120 sound blocks
   a second of the stated length, and 2 million whatever the length says. */
TEST(conv_bounds_the_samples_a_file_can_make) {
    static const uint8_t one[1] = {0x55};
    for (int honest = 0; honest < 2; honest++) { /* 2000 pictures: in 1 s too many, in 100 s fine */
        W in, out;
        Tracks t = {"V_MPEG4/ISO/AVC", AVCC, sizeof AVCC, 64, 48, NULL, NULL, 0};
        size_t seg = start_file(&in, &t, honest ? 100000 : 1000);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        for (int k = 0; k < 2000; k++) w_simple(&in, 1, k * 10, k % 50 == 0, one, 1);
        w_close(&in, cl);
        w_close(&in, seg);
        AvConvJob j = {0};
        Seen s;
        bool ok = run(&in, &out, &j, &s);
        if (honest) {
            CHECK(ok);
            int n[2];
            counts(&out, n);
            CHECK_INT(n[0], 2000);
        } else {
            CHECK(!ok);
            CHECK(j.unsupported);
            CHECK_HAS(j.err, "far too many pictures");
            CHECK_INT((int)out.len, 0); /* found while reading: nothing written */
        }
        free(in.p), free(out.p);
    }

    { /* sound: 2000 AAC blocks in a file that says it lasts a second */
        W in, out;
        uint8_t asc[2] = {0x11, 0x90};
        Tracks t = {"V_MPEG4/ISO/AVC", AVCC, sizeof AVCC, 64, 48, "A_AAC", asc, 2};
        size_t seg = start_file(&in, &t, 1000);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        for (int k = 0; k < 2000; k++) {
            if (k % 60 == 0) w_simple(&in, 1, k / 2, k == 0, one, 1);
            w_simple(&in, 2, k / 2, true, one, 1);
        }
        w_close(&in, cl);
        w_close(&in, seg);
        AvConvJob j = {0};
        Seen s;
        CHECK(!run(&in, &out, &j, &s));
        CHECK(j.unsupported);
        CHECK_HAS(j.err, "far too many sound blocks");
        free(in.p), free(out.p);
    }

    { /* no length at all: 2,000,128 laced empty pictures in 110 KB stop at 2 million */
        W in, out;
        Tracks t = {"V_MPEG4/ISO/AVC", AVCC, sizeof AVCC, 64, 48, NULL, NULL, 0};
        size_t seg = start_file(&in, &t, 0);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        for (int b = 0; b < 7813; b++) w_laced_empty(&in, 1);
        w_close(&in, cl);
        w_close(&in, seg);
        CHECK(in.len < 120000);
        AvConvJob j = {0};
        Seen s;
        CHECK(!run(&in, &out, &j, &s));
        CHECK(j.unsupported);
        CHECK_HAS(j.err, "far too many pictures");
        /* a job can ask for a smaller bound (tests, fuzzing): 1024 pictures are too many for 1000 */
        free(in.p), free(out.p);
        seg = start_file(&in, &t, 0);
        cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        for (int b = 0; b < 4; b++) w_laced_empty(&in, 1);
        w_close(&in, cl);
        w_close(&in, seg);
        AvConvJob small = {.max_samples = 1000};
        CHECK(!run(&in, &out, &small, &s));
        CHECK_HAS(small.err, "far too many pictures");
        free(out.p);
        AvConvJob roomy = {.max_samples = 1024};
        CHECK(run(&in, &out, &roomy, &s));
        free(in.p), free(out.p);
    }

    { /* Dolby with a 10 s jump at every block: each is filled with silence (a damaged disc's
         holes), but not past what a file of 1 s can hold */
        static uint8_t ac3[768]; /* one Dolby frame, sent again every time */
        CHECK_INT((int)ac3_test_tone(ac3, sizeof ac3, 1), 768);
        W in, out;
        Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_AC3", NULL, 0};
        size_t seg = start_file(&in, &t, 1000);
        static uint8_t pic[4096];
        for (int b = 0; b < 8; b++) {
            size_t cl = w_open(&in, 0x1F43B675);
            w_uint(&in, 0xE7, (uint64_t)b * 10000);
            for (int k = 0; k < 4; k++) w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, b * 4 + k));
            w_simple(&in, 2, 0, true, ac3, sizeof ac3);
            w_close(&in, cl);
        }
        w_close(&in, seg);
        AvConvJob j = {0};
        Seen s;
        CHECK(!run(&in, &out, &j, &s));
        CHECK_HAS(j.err, "far too many sound blocks");
        free(in.p), free(out.p);
    }
}

/* Timestamps and lengths no real file has - damage, or made up: no arithmetic on them may
   overflow (the debug build traps on that), and a stated length of days mustn't reserve a
   giant index (it used to: up to 2 GB of padding at the front of the copy). */
TEST(conv_keeps_wild_timestamps_and_lengths_in_check) {
    for (int mpeg2 = 0; mpeg2 < 2; mpeg2++) { /* a second cluster ~146 years in */
        static Stream v, a;
        uint8_t avcc[256], asc[2];
        size_t avcc_len = 0;
        if (mpeg2) add_mpeg2(&v, 0, 10, 48, 32, false, 0, false);
        else add_h264(&v, 10, avcc, &avcc_len);
        add_aac(&a, 0.2, asc);
        CHECK(v.n == 10 && a.n >= 8);
        W in, out;
        Tracks t = {mpeg2 ? "V_MPEG2" : "V_MPEG4/ISO/AVC", mpeg2 ? NULL : avcc, avcc_len, mpeg2 ? 48 : 64,
                    mpeg2 ? 32 : 48, "A_AAC", asc, 2};
        size_t seg = start_file(&in, &t, 400);
        for (int c = 0; c < 2; c++) {
            size_t cl = w_open(&in, 0x1F43B675);
            w_uint(&in, 0xE7, c ? 4600000000000ull : 0); /* (ms) in ns: 4.6e18, close to INT64_MAX */
            for (int i = c * 5; i < c * 5 + 5; i++) w_simple(&in, 1, v.ms[i] % 200, v.key[i], s_block(&v, i), s_size(&v, i));
            for (int i = c * 4; i < c * 4 + 4; i++) w_simple(&in, 2, a.ms[i] % 100, true, s_block(&a, i), s_size(&a, i));
            w_close(&in, cl);
        }
        w_close(&in, seg);
        AvConvJob j = {0};
        Seen s;
        CHECK(run(&in, &out, &j, &s)); /* the far-out times count as missing */
        int n[2];
        counts(&out, n);
        CHECK_INT(n[0], 10);
        free(in.p), free(out.p);
        s_free(&v), s_free(&a);
    }
    static const double lengths[] = {10 * 86400e3, 365 * 86400e3, 1e300}; /* 10 days, a year, nonsense (ms) */
    for (int i = 0; i < 3; i++)
        for (int mpeg2 = 0; mpeg2 < 2; mpeg2++) {
            static Stream v;
            uint8_t avcc[256];
            size_t avcc_len = 0;
            if (mpeg2) add_mpeg2(&v, 0, 10, 48, 32, false, 0, false);
            else add_h264(&v, 10, avcc, &avcc_len);
            W in, out;
            Tracks t = {mpeg2 ? "V_MPEG2" : "V_MPEG4/ISO/AVC", mpeg2 ? NULL : avcc, avcc_len, mpeg2 ? 48 : 64,
                        mpeg2 ? 32 : 48, NULL, NULL, 0};
            mux(&in, &t, lengths[i], &v, NULL);
            AvConvJob j = {0};
            Seen s;
            CHECK(run(&in, &out, &j, &s));
            CHECK(out.len < 100000); /* the copy of a 10-picture file stays small */
            free(in.p), free(out.p);
            s_free(&v);
        }
}

/* 1.2: a file the converter refuses for what it holds (a VP9 picture, TrueHD sound) still gives its subtitles,
   read once more on their own and timed on the copy's clock (from the first picture, whatever its codec).
   Not when asked to skip that (the browser: the original goes to the server, which takes them itself); a
   real error (stopped) gives none. */
TEST(conv_takes_subtitles_from_a_file_it_refuses) {
    static uint8_t set[1024], spu[600], pic[4096];
    static const char head[] = "size: 720x480\n";
    for (int sound = 0; sound < 2; sound++) {
        W in;
        Tracks t = {sound ? "V_MPEG2" : "V_VP9", NULL, 0, 48, 32, sound ? "A_TRUEHD" : NULL, NULL, 0,
                    {{"S_TEXT/UTF8", "swe", NULL, false},
                     {"S_HDMV/PGS", "eng", NULL, false},
                     {"S_VOBSUB", "eng", NULL, false, head, sizeof head - 1},
                     {"S_TEXT/ASS", "eng", NULL, false}}};
        size_t seg = start_file(&in, &t, 3000);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        for (int k = 0; k < 20; k++) {
            if (sound) {
                w_simple(&in, 1, 300 + frame_ms(k), true, pic, mpeg2_picture(pic, k));
                w_simple(&in, 2, 300 + k * 32, true, "\x7F\xFE\x80\x01", 4);
            } else {
                const uint8_t vp9[8] = {0x82, 0x49, 0x83, (uint8_t)k};
                w_simple(&in, 1, 300 + frame_ms(k), k == 0, vp9, sizeof vp9);
            }
        }
        w_group(&in, 3, 1300, 1000, "Hej", 3);
        w_simple(&in, 4, 800, true, set, pgs_set(set, 0, 1));
        spu_of(spu, sizeof spu, 1);
        w_group(&in, 5, 2300, 0, spu, sizeof spu);
        const char *ass = "0,0,Default,,0,0,0,,Hi";
        w_group(&in, 6, 2400, 500, ass, strlen(ass));
        w_close(&in, cl);
        w_close(&in, seg);
        for (int how = 0; how < 3; how++) { /* asked, not asked, asked to skip them after a refusal */
            W out;
            AvConvJob j = {.want_subs = how != 1, .skip_refused_subs = how == 2};
            Seen s;
            CHECK(!run(&in, &out, &j, &s));
            CHECK(j.unsupported);
            CHECK_HAS(j.err, sound ? "Its sound is Dolby TrueHD" : "Its picture is VP9"); /* the refusal stays the reason */
            CHECK_INT((int)out.len, 0);
            CHECK_INT(j.nsubs, how ? 0 : 4);
            if (!how) {
                CHECK_STR(j.subs[0].lang, "swe");
                CHECK_STR(j.subs[0].srt, "1\n00:00:01,000 --> 00:00:02,000\nHej\n\n");
                CHECK_INT(j.subs[1].kind, AV_SUB_PGS);
                uint32_t pts[8];
                uint8_t types[8];
                CHECK_INT(read_sup(j.subs[1].data, j.subs[1].size, pts, types, 8), 5);
                CHECK_INT(pts[0], 500 * 90);
                CHECK_INT(j.subs[2].kind, AV_SUB_VOBSUB);
                CHECK_HAS(j.subs[2].idx, "\nid: en, index: 0\ntimestamp: 00:00:02:000, filepos: 000000000\n");
                CHECK_STR(j.subs[3].srt, "1\n00:00:02,100 --> 00:00:02,600\nHi\n\n");
                CHECK(s.rising && s.last > 900 && s.last < 1000); /* (read to the end; a refusal never says 1000) */
            }
            av_convert_free(&j);
            free(out.p);
        }
        volatile int stop = 1; /* stopped while reading them: none, and still the refusal */
        W out;
        AvConvJob j = {.want_subs = true, .stop = &stop};
        Seen s;
        CHECK(!run(&in, &out, &j, &s));
        CHECK(j.unsupported);
        CHECK_HAS(j.err, sound ? "Its sound is Dolby TrueHD" : "Its picture is VP9");
        CHECK_INT(j.nsubs, 0);
        free(in.p), free(out.p);
    }

    /* refused part-way (the picture changes size): the subtitles read again from the start - each once */
    static Stream v;
    add_mpeg2(&v, 0, 20, 48, 32, false, 0, false);
    add_mpeg2(&v, 20, 20, 48, 16, false, 0, false);
    W in, out;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0, {{"S_TEXT/UTF8", "eng", NULL, false}}};
    size_t seg = start_file(&in, &t, 1400);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    w_group(&in, 3, 100, 200, "One", 3); /* gathered before the refusal */
    for (int k = 0; k < v.n; k++) w_simple(&in, 1, v.ms[k], v.key[k], s_block(&v, k), s_size(&v, k));
    w_group(&in, 3, 1200, 200, "Two", 3); /* after it */
    w_close(&in, cl);
    w_close(&in, seg);
    AvConvJob j = {.want_subs = true};
    Seen s;
    CHECK(!run(&in, &out, &j, &s));
    CHECK_HAS(j.err, "changes size");
    CHECK_INT(j.nsubs, 1);
    CHECK_STR(j.subs[0].srt, "1\n00:00:00,100 --> 00:00:00,300\nOne\n\n2\n00:00:01,200 --> 00:00:01,400\nTwo\n\n");
    av_convert_free(&j);
    free(in.p), free(out.p);
    s_free(&v);
}

static bool run_subs(W *in, W *out, AvConvJob *j, Seen *s) {
    memset(out, 0, sizeof *out);
    j->in = (MkvReader){in, (int64_t)in->len, mem_read};
    j->out = (Mp4Out){out, mem_write, mem_write_at};
    memset(s, 0, sizeof *s);
    s->rising = true;
    j->progress = seen;
    j->progress_ctx = s;
    return av_subtitles(j);
}

/* av_subtitles: the subtitle tracks alone, for a video that is never converted - timed exactly as a
   conversion times them (the clock starts at the first picture, found without decoding anything), and
   nothing written. */
TEST(conv_subtitles_alone_time_like_a_conversion) {
    static uint8_t set[1024], spu[3000];
    static const char head[] = "size: 720x480\n";
    for (int copy = 0; copy < 2; copy++) { /* MPEG-2 (re-encoded by a conversion), H.264 (copied) */
        static Stream v;
        uint8_t avcc[256];
        size_t avcc_len = 0;
        if (copy) add_h264(&v, 30, avcc, &avcc_len);
        else add_mpeg2(&v, 0, 30, 48, 32, false, 0, false);
        W in;
        Tracks t = {copy ? "V_MPEG4/ISO/AVC" : "V_MPEG2", copy ? avcc : NULL, avcc_len, copy ? 64 : 48, copy ? 48 : 32, NULL, NULL, 0,
                    {{"S_TEXT/UTF8", "eng", NULL, false},
                     {"S_TEXT/WEBVTT", "eng", "SDH", false},
                     {"S_HDMV/PGS", "eng", NULL, false},
                     {"S_VOBSUB", "eng", NULL, false, head, sizeof head - 1}}};
        size_t seg = start_file(&in, &t, 1500);
        size_t cl = w_open(&in, 0x1F43B675);
        w_uint(&in, 0xE7, 0);
        w_group(&in, 3, 50, 900, "Early", 5); /* before the first picture (400 ms): from 0, shortened */
        w_group(&in, 4, 60, 100, "Gone", 4);  /* over before it: left out */
        for (int k = 0; k < v.n; k++) {
            w_simple(&in, 1, 400 + v.ms[k], v.key[k], s_block(&v, k), s_size(&v, k));
            if (k == 12) {
                w_group(&in, 3, 820, 300, "Later", 5);
                w_group(&in, 4, 830, 300, "<v Al>Also</v>", 14);
                w_simple(&in, 5, 840, true, set, pgs_set(set, 1, 1));
                spu_of(spu, sizeof spu, 2);
                w_group(&in, 6, 850, 0, spu, sizeof spu);
            }
        }
        w_close(&in, cl);
        w_close(&in, seg);
        W out, none;
        AvConvJob conv = {.want_subs = true}, alone = {0};
        Seen s;
        CHECK(run(&in, &out, &conv, &s));
        CHECK(run_subs(&in, &none, &alone, &s));
        CHECK_INT((int)none.len, 0); /* nothing written */
        CHECK(s.rising && s.last == 1000);
        CHECK_STR(alone.err, "");
        CHECK_INT(alone.nsubs, 4);
        CHECK_INT(conv.nsubs, 4);
        CHECK_STR(alone.subs[0].srt, "1\n00:00:00,000 --> 00:00:00,550\nEarly\n\n2\n00:00:00,420 --> 00:00:00,720\nLater\n\n");
        CHECK_STR(alone.subs[1].srt, "1\n00:00:00,430 --> 00:00:00,730\nAlso\n\n");
        for (int k = 0; k < 4; k++) { /* the very same as the conversion's */
            AvConvSub *a = &alone.subs[k], *b = &conv.subs[k];
            CHECK_INT(a->kind, b->kind);
            CHECK_INT(a->cues, b->cues);
            CHECK(a->len == b->len && (!a->len || !memcmp(a->srt, b->srt, a->len)));
            CHECK(a->size == b->size && (!a->size || !memcmp(a->data, b->data, a->size)));
            CHECK(a->idx_len == b->idx_len && (!a->idx_len || !strcmp(a->idx, b->idx)));
        }
        uint32_t pts[8];
        uint8_t types[8];
        CHECK_INT(read_sup(alone.subs[2].data, alone.subs[2].size, pts, types, 8), 5);
        CHECK_INT(pts[0], 440 * 90);
        CHECK_HAS(alone.subs[3].idx, "timestamp: 00:00:00:450, filepos: 000000000\n");
        av_convert_free(&conv);
        av_convert_free(&alone);
        CHECK_INT(alone.nsubs, 0);

        volatile int stop = 1; /* stopped: none */
        AvConvJob stopped = {.stop = &stop};
        CHECK(!run_subs(&in, &none, &stopped, &s));
        CHECK_STR(stopped.err, "Stopped");
        CHECK(!stopped.unsupported);
        CHECK_INT(stopped.nsubs, 0);
        free(in.p), free(out.p), free(none.p);
        s_free(&v);
    }
    W in, none; /* a file without subtitles: read at once, none; not Matroska: says so */
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_DTS", NULL, 0};
    w_close(&in, start_file(&in, &t, 100));
    AvConvJob j = {0};
    Seen s;
    CHECK(run_subs(&in, &none, &j, &s));
    CHECK_INT(j.nsubs, 0);
    CHECK_INT(s.last, 1000);
    free(in.p), free(none.p);
    memset(&in, 0, sizeof in);
    w_put(&in, "RIFF....AVI LIST", 16);
    CHECK(!run_subs(&in, &none, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Not a readable MKV or MP4 file");
    free(in.p), free(none.p);
}

/* The bounds (a job may ask for less, as here: 64 KB a track, 256 KB in all): a picture track past its
   most is left out whole - one subpicture too big, or growing past it part-way - never cut; past the most
   all picture tracks may hold together, the track that would cross it goes, and room given back by a
   dropped one is used again. Text tracks are none of it. */
TEST(conv_bounds_picture_subtitles) {
    static const char head[] = "size: 720x480\n";
    static const char *const names[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0};
    for (int k = 0; k < 8; k++)
        t.sub[k] = (SubTrack){k == 7 ? "S_TEXT/UTF8" : k == 1 ? "S_HDMV/PGS" : "S_VOBSUB", "eng", names[k], false,
                              k == 7 || k == 1 ? NULL : head, sizeof head - 1};
    size_t seg = start_file(&in, &t, 3000);
    size_t cl = w_open(&in, 0x1F43B675);
    w_uint(&in, 0xE7, 0);
    static uint8_t pic[4096], big[70000], spu[1000], set[4096];
    w_simple(&in, 1, 0, true, pic, mpeg2_picture(pic, 0));
    for (int i = 0; i < 40; i++) {
        int ms = 100 + i * 50;
        if (i == 0) { /* track 3: one subpicture bigger than a track may be */
            spu_of(big, sizeof big, 0);
            w_group(&in, 3, ms, 0, big, sizeof big);
        }
        size_t n = pgs_set(set, i, 1); /* track 4: ~2 KB a block, 80 KB in all: past 64 KB part-way */
        static uint8_t fill[2000];
        n += pgs_seg(set + n, 0x15, fill, sizeof fill);
        w_simple(&in, 4, ms, true, set, n);
        spu_of(spu, sizeof spu, i); /* 1000 bytes: one 2 KB pack each */
        for (int k = 2; k < 6 && i < 30; k++) w_group(&in, 3 + k, ms, 0, spu, sizeof spu); /* tracks 5-8: 60 KB each */
        if (i >= 35) w_group(&in, 9, ms, 0, spu, sizeof spu); /* track 9: after track 4 gave its room back */
        if (i == 0) w_group(&in, 10, ms, 500, "Text", 4);
    }
    w_close(&in, cl);
    w_close(&in, seg);
    W none;
    AvConvJob j = {.pic_bytes = 65536};
    Seen s;
    CHECK(run_subs(&in, &none, &j, &s));
    CHECK_INT(j.nsubs, 5);
    static const char *const kept[] = {"2", "3", "4", "6", "7"}; /* 0: too big, 1: grew past, 5: no room left */
    for (int k = 0; k < 5; k++) CHECK_STR(j.subs[k].name, kept[k]);
    CHECK_INT(j.subs[0].cues, 30);
    CHECK_INT((int)j.subs[0].size, 30 * 2048);
    CHECK_INT(j.subs[3].cues, 5);
    CHECK_INT(j.subs[4].kind, AV_SUB_TEXT);
    av_convert_free(&j);
    /* the same file with the built-in bounds: everything (the PGS track's 80 KB, the 70 KB subpicture) */
    AvConvJob roomy = {0};
    CHECK(run_subs(&in, &none, &roomy, &s));
    CHECK_INT(roomy.nsubs, 8);
    CHECK_INT(roomy.subs[1].kind, AV_SUB_PGS);
    CHECK_INT(roomy.subs[1].cues, 40);
    av_convert_free(&roomy);
    free(in.p), free(none.p);
}

/* ------------------------------------------------------------ fuzzing seeds */

/* Xiph lacing: the sound's blocks 4 at a time, as some muxers write them */
static void mux_laced(W *in, const Tracks *t, double ms, const Stream *v, const Stream *a) {
    size_t seg = start_file(in, t, ms);
    size_t cl = w_open(in, 0x1F43B675);
    w_uint(in, 0xE7, 0);
    for (int i = 0, k = 0; i < v->n || k < a->n;) {
        if (i < v->n && (k >= a->n || v->ms[i] <= a->ms[k])) {
            w_simple(in, 1, v->ms[i], v->key[i], s_block(v, i), s_size(v, i));
            i++;
            continue;
        }
        int nf = a->n - k < 4 ? a->n - k : 4;
        size_t lens = 0, body = a->at[k + nf] - a->at[k];
        uint8_t head[64];
        for (int f = 0; f < nf - 1; f++) /* every size but the last: runs of 255 and the rest */
            for (size_t len = s_size(a, k + f);; len -= 255) {
                head[lens++] = (uint8_t)(len < 255 ? len : 255);
                if (len < 255) break;
            }
        w_id(in, 0xA3);
        w_size8(in, 5 + lens + body);
        w_byte(in, 0x82), w_byte(in, (a->ms[k] >> 8) & 0xFF), w_byte(in, a->ms[k] & 0xFF);
        w_byte(in, 0x80 | 0x02); /* key, Xiph lacing */
        w_byte(in, nf - 1);
        w_put(in, head, lens);
        w_put(in, s_block(a, k), body);
        k += nf;
    }
    w_close(in, cl);
    w_close(in, seg);
}

/* ------------------------------------------------- MPEG-4 Part 2: DivX, Xvid (2.5) */

/* the k-th track's sample durations, from its stts (at most cap); -1 without one */
static int durations(const W *w, int k, uint32_t *d, int cap) {
    Bx t, b;
    if (!trak_k(w, k, &t) || !bx_path(t, "mdia/minf/stbl/stts", &b)) return -1;
    int n = 0;
    for (uint32_t e = 0, entries = be32(b.p + 4); e < entries; e++)
        for (uint32_t c = 0, cnt = be32(b.p + 8 + e * 8); c < cnt && n < cap; c++) d[n++] = be32(b.p + 12 + e * 8);
    return n;
}

/* pictures at an uneven pace (Solsidan S06E04: 11.7 a second, unevenly): 40 ms apart, then a gap of 140 ms */
static int uneven_ms(int d) { return d * 40 + (d >= 7 ? 100 : 0); }

/* An Xvid stream (B-VOPs, coefficients, four vectors, intra macroblocks in P-VOPs) decoded and made H.264, each
   picture as long as its own time says; its MP3 copied. The same as an AVI's track (mkvmerge's V_MS/VFW/FOURCC,
   DivX's packed blocks), and from an MP4's 'mp4v' track. */
TEST(conv_reencodes_mpeg4_part_2) {
    static Stream v, a;
    uint8_t vol[64];
    size_t nvol = add_mpeg4(&v, 13, 72, 40, 0, false, uneven_ms, vol);
    static uint8_t mp3[417];
    for (int f = 0; f < 22; f++) s_add(&a, mp3, mp3_frame(mp3, false, (uint8_t)f), f * 26, true);
    Tracks t = {"V_MPEG4/ISO/ASP", vol, nvol, 72, 40, "A_MPEG/L3", NULL, 0, .arate = 44100};
    W in, out;
    mux(&in, &t, 600, &v, &a);
    AvConvJob j = {0};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_STR(j.err, "");
    CHECK_HAS(j.what, "MPEG-4 video (DivX/Xvid, 25.000 fps) → H.264"); /* (the usual gap: 40 ms) */
    CHECK_HAS(j.what, "MP3 copied");
    int n[2];
    counts(&out, n);
    CHECK_INT(n[0], 13); /* every picture, B-VOPs in their place */
    CHECK(track_timescale(&out, 0) == 90000);
    uint32_t d[16];
    CHECK_INT(durations(&out, 0, d, 16), 13);
    for (int i = 0; i < 13; i++) CHECK_INT((int)d[i], i == 6 ? 12600 : 3600); /* 40 ms, 140 ms at the gap, 40 ms the last */
    free(in.p), free(out.p);
    s_free(&v), s_free(&a);

    /* an AVI's: a BITMAPINFOHEADER (XVID) in front, the VOL in the first block, packed blocks at their slots' times */
    uint8_t bih[40];
    bitmapinfo(bih, 48, 32, "XVID");
    add_mpeg4(&v, 13, 48, 32, M4_RESYNC, true, frame_ms, vol);
    for (int f = 0; f < 12; f++) s_add(&a, mp3, mp3_frame(mp3, false, (uint8_t)f), f * 26, true);
    t = (Tracks){"V_MS/VFW/FOURCC", bih, sizeof bih, 48, 32, "A_MPEG/L3", NULL, 0, .arate = 44100};
    mux(&in, &t, 450, &v, &a);
    AvConvJob j2 = {0};
    CHECK(run(&in, &out, &j2, &s));
    CHECK_STR(j2.err, "");
    counts(&out, n);
    CHECK_INT(n[0], 13);
    CHECK_INT(durations(&out, 0, d, 16), 13);
    bool even = true; /* (the slots: 1001/30 ms, in whole ms - 33 or 34) */
    int sum = 0;
    for (int i = 0; i < 12; i++) even &= d[i] == 2970 || d[i] == 3060, sum += (int)d[i];
    CHECK(even);
    CHECK_INT(sum, frame_ms(12) * 90);
    free(in.p), free(out.p);
    s_free(&v), s_free(&a);

    /* refused, in words: interlaced Xvid, global motion, DivX 3 (another codec) */
    static const struct {
        const char *codec, *fourcc, *err;
        unsigned flags;
    } R[] = {{"V_MPEG4/ISO/ASP", NULL, "Its MPEG-4 picture uses interlaced coding, which the converter can't decode yet", M4_INTERLACED},
             {"V_MPEG4/ISO/ASP", NULL, "Its MPEG-4 picture uses global motion compensation (GMC), which the converter can't decode yet", M4_GMC},
             {"V_MS/VFW/FOURCC", "DIV3", "Its picture is Video for Windows (DIV3), which the converter can't change yet", 0}};
    for (int r = 0; r < 3; r++) {
        nvol = add_mpeg4(&v, 4, 32, 32, R[r].flags, false, frame_ms, vol);
        bitmapinfo(bih, 32, 32, R[r].fourcc ? R[r].fourcc : "XVID");
        t = (Tracks){R[r].codec, R[r].fourcc ? bih : vol, R[r].fourcc ? sizeof bih : nvol, 32, 32, NULL, NULL, 0};
        mux(&in, &t, 150, &v, NULL);
        AvConvJob jr = {0};
        CHECK(!run(&in, &out, &jr, &s));
        CHECK(jr.unsupported);
        CHECK_STR(jr.err, R[r].err);
        free(in.p), free(out.p);
        s_free(&v);
    }
}

/* (2.6) HEVC is copied as 'hvc1' unless the job asks for H.264 (AvConvJob.h264): then it's decoded and re-encoded,
   timed by each picture's own time on 90 kHz like MPEG-4; AV1 and HEVC the decoder can't read are refused in words */
TEST(conv_makes_hevc_h264_when_asked) {
    static Stream v;
    uint8_t hvcc[2048];
    HwStream hs = hevc_stream(64, 48, 10, HW_RANDOM | HW_SAO | HW_TMVP, 9);
    hs.rate_num = 30000, hs.rate_den = 1001;
    size_t nh = add_hevc(&v, &hs, hvcc, sizeof hvcc);
    Tracks t = {"V_MPEGH/ISO/HEVC", hvcc, nh, 64, 48, NULL, NULL, 0};
    W in, out;
    mux(&in, &t, 330, &v, NULL);
    AvConvJob j = {0};
    Seen s;
    CHECK(run(&in, &out, &j, &s));
    CHECK_STR(j.what, "HEVC copied");
    free(out.p);
    AvConvJob j2 = {0};
    j2.h264 = true;
    CHECK(run(&in, &out, &j2, &s));
    CHECK_STR(j2.err, "");
    CHECK_HAS(j2.what, "HEVC video (10-bit, 29.970 fps) → H.264");
    int n[2];
    counts(&out, n);
    CHECK_INT(n[0], 9); /* every picture, B-pictures in their place */
    CHECK(track_timescale(&out, 0) == 90000);
    uint32_t d[16];
    CHECK_INT(durations(&out, 0, d, 16), 9);
    bool even = true; /* (frame_ms: 1001/30 ms in whole ms - 33 or 34) */
    int sum = 0;
    for (int i = 0; i < 8; i++) even &= d[i] == 2970 || d[i] == 3060, sum += (int)d[i];
    CHECK(even);
    CHECK_INT(sum, frame_ms(8) * 90);
    free(in.p), free(out.p);
    s_free(&v);

    /* refused when H.264 is asked for: AV1 (copied otherwise), and HEVC in 4:2:2 */
    static const uint8_t av1c[] = {0x81, 0x00, 0x0C, 0x00, 0x0A, 0x05, 0x00, 0x00, 0x00, 0x24, 0x9F};
    for (int f = 0; f < 4; f++) s_add(&v, "\x12\x00\x0a\x0b", 4, frame_ms(f), f == 0);
    t = (Tracks){"V_AV1", av1c, sizeof av1c, 64, 48, NULL, NULL, 0};
    mux(&in, &t, 150, &v, NULL);
    AvConvJob j3 = {0};
    j3.h264 = true;
    CHECK(!run(&in, &out, &j3, &s));
    CHECK(j3.unsupported);
    CHECK_STR(j3.err, "Its picture is AV1, which the converter can only copy so far (H.264 from it: not yet)");
    free(in.p), free(out.p);
    s_free(&v);
    hs = hevc_stream(32, 32, 8, 0, 3);
    hs.chroma = 2;
    nh = add_hevc(&v, &hs, hvcc, sizeof hvcc);
    t = (Tracks){"V_MPEGH/ISO/HEVC", hvcc, nh, 32, 32, NULL, NULL, 0};
    mux(&in, &t, 150, &v, NULL);
    AvConvJob j4 = {0};
    j4.h264 = true;
    CHECK(!run(&in, &out, &j4, &s));
    CHECK(j4.unsupported);
    CHECK_STR(j4.err, "Its HEVC picture uses 4:2:2 chroma, which the converter can't decode yet");
    free(in.p), free(out.p);
    s_free(&v);
}

/* the k-th track's sample sizes, from its stsz (at most cap); -1 without one */
static int sample_sizes(const W *w, int k, uint32_t *d, int cap) {
    Bx t, b;
    if (!trak_k(w, k, &t) || !bx_path(t, "mdia/minf/stbl/stsz", &b) || b.n < 12) return -1;
    int n = 0;
    for (uint32_t i = 0, count = be32(b.p + 8); i < count && n < cap && 12 + (i + 1) * 4 <= b.n; i++) d[n++] = be32(b.p + 12 + i * 4);
    return n;
}

/* (2.6.1) HEVC in an MP4 whose edit list stops before its last picture (Friends': the encoder's last two, 83 ms the
   copy showed and the original didn't): the hidden picture is still decoded - the shown B-picture before it is
   predicted from it - but left out of the H.264 copy; what's shown comes out as without the cut */
TEST(conv_leaves_out_what_an_mp4_edit_list_hides) {
    static Stream v;
    uint8_t hvcc[2048];
    HwStream hs = hevc_stream(64, 48, 8, HW_RANDOM | HW_TMVP, 9); /* I0 P2 B1 P4 B3 P6 B5 P8 B7 */
    hs.rate_num = 30000, hs.rate_den = 1001;
    size_t nh = add_hevc(&v, &hs, hvcc, sizeof hvcc);
    W whole, cut, out1, out2;
    mp4_of(&whole, MP4_HEVC, hvcc, nh, 64, 48, &v, 0);
    mp4_of(&cut, MP4_HEVC, hvcc, nh, 64, 48, &v, frame_ms(8)); /* pictures 0-7 shown, 8 (B7's reference) hidden */
    AvConvJob j1 = {0}, j2 = {0};
    j1.h264 = j2.h264 = true;
    Seen s;
    CHECK(run(&whole, &out1, &j1, &s));
    CHECK(run(&cut, &out2, &j2, &s));
    CHECK_STR(j2.err, "");
    int n1[2], n2[2];
    counts(&out1, n1);
    counts(&out2, n2);
    CHECK_INT(n1[0], 9);
    CHECK_INT(n2[0], 8);
    uint32_t a[16], b[16]; /* the same eight pictures, B7 too: its hidden reference was decoded */
    CHECK_INT(sample_sizes(&out1, 0, a, 16), 9);
    CHECK_INT(sample_sizes(&out2, 0, b, 16), 8);
    for (int i = 0; i < 8; i++) CHECK_INT((int)b[i], (int)a[i]);
    size_t m1 = box(&out1, "mdat"), m2 = box(&out2, "mdat");
    uint32_t first8 = 0;
    for (int i = 0; i < 8; i++) first8 += a[i];
    CHECK(m1 && m2);
    if (m1 && m2) { /* (an mdat's header: 8 bytes, or 16 with a 64-bit size) */
        m1 += be32(out1.p + m1) == 1 ? 16 : 8, m2 += be32(out2.p + m2) == 1 ? 16 : 8;
        CHECK(out1.len >= m1 + first8 && out2.len >= m2 + first8 && !memcmp(out1.p + m1, out2.p + m2, first8));
    }
    free(whole.p), free(cut.p), free(out1.p), free(out2.p);
    s_free(&v);
}

/* the k-th track's first sync sample (stss; 1 = the first): -1 without one, 1 when every sample is one (no stss) */
static int first_sync(const W *w, int k) {
    Bx t, b;
    if (!trak_k(w, k, &t)) return -1;
    if (!bx_path(t, "mdia/minf/stbl/stss", &b)) return 1;
    return b.n >= 12 && be32(b.p + 4) ? (int)be32(b.p + 8) : -1;
}

/* (2.7; owner: "Full throttle ... processes the videos in the most effective way") One video over several cores: HEVC
   made H.264 in parts side by side - each from the keyframe before it, its own pictures only, the first a keyframe -
   and joined with the sound: the same pictures at the same times as one conversion of the whole, the sound the same.
   Three streams one after another (an IDR each), cut where no keyframe is. */
/* HEVC + AAC: three streams one after another - 27 pictures, a keyframe at 0, 9 and 18 - 0.9 s */
static void hevc_three(W *in) {
    static Stream v, a;
    uint8_t hvcc[2048], asc[2];
    HwStream hs = hevc_stream(64, 48, 8, HW_RANDOM | HW_TMVP, 9);
    hs.rate_num = 30000, hs.rate_den = 1001;
    static uint8_t au[1 << 17];
    for (int g = 0; g < 3; g++)
        for (int k = 0; k < hw_count(&hs); k++) {
            int poc;
            size_t len = hw_picture(&hs, k, false, au, sizeof au, &poc);
            s_add(&v, au, len, frame_ms(poc + 9 * g), k == 0);
        }
    size_t nh = hw_hvcc(&hs, hvcc, sizeof hvcc);
    add_aac(&a, 0.9, asc);
    Tracks t = {"V_MPEGH/ISO/HEVC", hvcc, nh, 64, 48, "A_AAC", asc, 2};
    mux(in, &t, 900, &v, &a);
    s_free(&v), s_free(&a);
}

/* (2.7) for media/convert_test.c (a part, a join, by files): that file at `path` */
bool conv_test_hevc_three(const char *path) {
    W in;
    hevc_three(&in);
    bool ok = file_write_atomic(path, in.p, in.len);
    free(in.p);
    return ok;
}

TEST(conv_makes_one_video_in_parts_and_joins_them) {
    W in, whole, part[3], joined;
    hevc_three(&in);
    AvConvJob j = {0};
    Seen s;
    j.h264 = true;
    CHECK(run(&in, &whole, &j, &s));
    CHECK_STR(j.err, "");
    int n[2];
    counts(&whole, n);
    CHECK_INT(n[0], 27);

    static const int cut[] = {0, 10, 19}; /* (pictures 10 and 19: no keyframe there - decoded from 9 and 18) */
    static const int want[] = {10, 9, 8};
    MkvReader pieces[3];
    for (int p = 0; p < 3; p++) {
        AvConvJob jp = {0};
        jp.h264 = true;
        jp.piece_from = (int64_t)frame_ms(cut[p]) * 1000000;
        jp.piece_to = p < 2 ? (int64_t)frame_ms(cut[p + 1]) * 1000000 : AV_CONV_TO_END;
        CHECK(run(&in, &part[p], &jp, &s));
        CHECK_STR(jp.err, "");
        int pn[2];
        counts(&part[p], pn);
        CHECK_INT(pn[0], want[p]);
        CHECK_INT(pn[1], -1);                   /* (the picture alone) */
        CHECK_INT(first_sync(&part[p], 0), 1); /* (it starts with a keyframe) */
        pieces[p] = (MkvReader){&part[p], (int64_t)part[p].len, mem_read};
    }
    AvConvJob jj = {0};
    jj.h264 = true;
    jj.pieces = pieces;
    jj.npieces = 3;
    CHECK(run(&in, &joined, &jj, &s));
    CHECK_STR(jj.err, "");
    CHECK_HAS(jj.what, "(made in 3 parts)");
    int nj[2];
    counts(&joined, nj);
    CHECK_INT(nj[0], 27);
    CHECK_INT(nj[1], n[1]); /* the sound as the whole's */
    CHECK(track_bytes(&joined, 1) == track_bytes(&whole, 1));
    uint32_t dw[32], dj[32]; /* every picture at its time */
    CHECK_INT(durations(&joined, 0, dj, 32), 27);
    CHECK_INT(durations(&whole, 0, dw, 32), 27);
    for (int i = 0; i < 27; i++) CHECK_INT((int)dj[i], (int)dw[i]);
    free(joined.p);

    /* a part of another video (another picture size: another settings record) is refused, and only HEVC made H.264 is
       made in parts */
    static Stream v2;
    uint8_t hvcc2[2048];
    HwStream hs2 = hevc_stream(32, 32, 8, 0, 5);
    size_t nh2 = add_hevc(&v2, &hs2, hvcc2, sizeof hvcc2);
    Tracks t2 = {"V_MPEGH/ISO/HEVC", hvcc2, nh2, 32, 32, NULL, NULL, 0};
    W in2, other;
    mux(&in2, &t2, 200, &v2, NULL);
    AvConvJob jq = {0};
    jq.h264 = true;
    jq.piece_to = AV_CONV_TO_END;
    CHECK(run(&in2, &other, &jq, &s));
    pieces[1] = (MkvReader){&other, (int64_t)other.len, mem_read};
    AvConvJob jb = {0};
    jb.h264 = true;
    jb.pieces = pieces;
    jb.npieces = 3;
    CHECK(!run(&in, &joined, &jb, &s));
    CHECK_STR(jb.err, "A part of the picture was made with other settings");
    free(joined.p), free(other.p), free(in2.p);
    s_free(&v2);
    AvConvJob jc = {0}; /* (copied, not made H.264: no parts) */
    jc.piece_to = 1000000;
    CHECK(!run(&in, &joined, &jc, &s));
    CHECK(jc.unsupported);
    free(joined.p);
    for (int p = 0; p < 3; p++) free(part[p].p);
    free(in.p), free(whole.p);
}

/* Small files of every kind the converter takes - the starting corpus for tools/convfuzz.c.
   Each must convert (the size change: fail cleanly); with HEMMA_FUZZ_SEEDS=<dir> they are
   also written there (CI's fuzz job makes them so, no binaries live in the repo). */
TEST(conv_fuzz_seeds) {
    const char *dir = getenv("HEMMA_FUZZ_SEEDS");
    if (dir && *dir) CHECK(dir_make(dir));
    static Stream v, a;
    static uint8_t ac3[4 * 768]; /* the test encoder is slow: 4 frames, sent in turn */
    CHECK_INT((int)ac3_test_tone(ac3, sizeof ac3, 4), 4 * 768);
    static uint8_t eac3[8 * 384]; /* Dolby Digital Plus, 3-block frames */
    CHECK_INT((int)eac3_test_tone(eac3, sizeof eac3, 8), 8 * 384);
    const char *raw = getenv("HEMMA_FUZZ_RAW");
    if (raw && *raw) { /* tools/avfuzz.c's seeds: raw Dolby frames, MPEG-2 and MPEG-4 streams, each after its options byte */
        char path[1024];
        Buf b = {0};
        static uint8_t pic[4096];
        for (int i = 0; i < 6; i++) {
            static const char *const names[] = {"ac3/ac3-tone", "ac3/eac3-tone", "ac3/eac3-tone-wav-reset",
                                                "mpeg2/progressive", "mpeg2/interlaced-in-pieces", "mpeg2/sizes-reset"};
            static const uint8_t opts[] = {0, 2, 1 | 16, 128, 5, 128 | 1};
            buf_reset(&b);
            buf_putc(&b, (char)opts[i]);
            if (i == 0) buf_put(&b, ac3, sizeof ac3);
            else if (i < 3) buf_put(&b, eac3, sizeof eac3);
            else
                for (int k = 0; k < 8; k++)
                    buf_put(&b, pic, i == 3   ? mpeg2_picture(pic, k)
                                     : i == 4 ? mpeg2_picture_of(pic, k, 64, 32, true, 70)
                                              : mpeg2_picture_of(pic, k, 48, k < 4 ? 32 : 16, false, 0));
            snprintf(path, sizeof path, "%s/%s", raw, names[i]);
            char folder[1024];
            snprintf(folder, sizeof folder, "%s/%s", raw, names[i]);
            *strrchr(folder, '/') = '\0'; /* (the name's own folder: ac3/, mpeg2/) */
            CHECK(dir_make(folder));
            CHECK(file_write_atomic(path, b.p, b.len));
        }
        /* DTS core frames (1.3): a 5.1 tone, and frames using every tool - stereo and 5.1, 512 and 1024 samples */
        snprintf(path, sizeof path, "%s/dts", raw);
        CHECK(dir_make(path));
        for (int i = 0; i < 3; i++) {
            static const char *const names[] = {"dts-tone", "dts-every-tool", "dts-every-tool-stereo-reset"};
            static const uint8_t opts[] = {0, 2, 1 | 16};
            static uint8_t frames[16 * 2048], frame[16384];
            buf_reset(&b);
            buf_putc(&b, (char)opts[i]);
            if (i == 0) buf_put(&b, frames, dts_test_tone(frames, sizeof frames, 12, true));
            else
                for (uint32_t k = 1; k <= 6; k++) {
                    TencOpts o = {.amode = i == 1 ? 9 : 2, .lfe = i == 1, .nblks = k & 1 ? 15 : 31, .subframes = k & 1 ? 2 : 4,
                                  .random = true, .aspf = k == 2, .cpf = k == 3, .dynf = k == 4, .sumf = k == 5,
                                  .sums = k == 6, .seed = k * 7919u + (uint32_t)i};
                    buf_put(&b, frame, dts_test_frame(&o, frame, sizeof frame, NULL));
                }
            snprintf(path, sizeof path, "%s/dts/%s", raw, names[i]);
            CHECK(file_write_atomic(path, b.p, b.len));
        }
        /* MPEG-4 Part 2 streams (2.5): B-VOPs, quarter samples, video packets, the MPEG quantiser, packed blocks */
        snprintf(path, sizeof path, "%s/mpeg4", raw);
        CHECK(dir_make(path));
        for (int i = 0; i < 4; i++) {
            static const char *const names[] = {"mpeg4-b-vops", "mpeg4-qpel-reversed", "mpeg4-packets-mpegquant-pieces",
                                                "mpeg4-packed-reset"};
            static const uint8_t opts[] = {0, 128, 3 | 8, 64};
            static const unsigned flags[] = {0, M4_QPEL, M4_RESYNC | M4_MPEGQ, 0};
            static uint8_t blk[32768];
            buf_reset(&b);
            buf_putc(&b, (char)opts[i]);
            for (int k = 0, disp; k < 10; k++)
                buf_put(&b, blk, mpeg4_test_block(blk, sizeof blk, k, i == 2 ? 72 : 48, i == 2 ? 40 : 32, flags[i], i == 3, &disp));
            snprintf(path, sizeof path, "%s/mpeg4/%s", raw, names[i]);
            CHECK(file_write_atomic(path, b.p, b.len));
        }
        /* HEVC streams (2.6): every syntax element the writer knows, 8, 9 and 10 bits, SAO, temporal vectors, PCM, QP deltas */
        snprintf(path, sizeof path, "%s/hevc", raw);
        CHECK(dir_make(path));
        for (int i = 0; i < 5; i++) {
            static const char *const names[] = {"hevc-pieces", "hevc-blocks-sao", "hevc-10bit-tmvp-reset", "hevc-exact-whole",
                                                "hevc-9bit-qp-blocks"};
            static const uint8_t opts[] = {0x0C, 3, 64 | 1, 2, 3};
            static const unsigned flags[] = {HW_RANDOM, HW_RANDOM | HW_SAO | HW_PCM_LF, HW_RANDOM | HW_TMVP | HW_SAO, 0,
                                             HW_RANDOM | HW_QP | HW_SAO};
            static const int depths[] = {8, 8, 10, 8, 9};
            static uint8_t stream[1 << 17];
            HwStream hs = hevc_stream(i == 1 ? 80 : 48, i == 1 ? 48 : 32, depths[i], flags[i], 7);
            hs.seed = (uint32_t)i + 1;
            buf_reset(&b);
            buf_putc(&b, (char)opts[i]);
            buf_put(&b, stream, hevc_test_stream(stream, sizeof stream, &hs));
            snprintf(path, sizeof path, "%s/hevc/%s", raw, names[i]);
            CHECK(file_write_atomic(path, b.p, b.len));
        }
        buf_free(&b);
    }
    static const uint8_t hvcc[] = {1,    0x01, 0x60, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 0x5D, 0xF0, 0, 0xFC,
                                   0xFD, 0xF8, 0xF8, 0, 0, 0x0F, 1,    0x20, 0, 1, 0, 2, 0x40, 0x01};
    char took[64][40]; /* every CodecID a seed carries: each format the converter takes needs one (below) */
    int ntook = 0;
    for (int seed = 0; seed < 24; seed++) {
        uint8_t avcc[256], asc[2], vol[64], bih[40];
        size_t avcc_len = 0;
        Tracks mp2 = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0}, t = mp2;
        const char *name = "";
        bool fails = false, laced = false, built = false;
        int subs = -1; /* subtitle tracks it gives (-1: not checked) */
        W in, out;
        switch (seed) {
        case 11:   /* H.264 with every subtitle kind a conversion carries: WebVTT, PGS (.sup), VobSub (.idx/.sub) */
        case 12: { /* refused (a VP9 picture) - its subtitles taken alone: SRT, PGS, VobSub with header stripping */
            static const char head[] = "size: 720x480\npalette: 000000, f0f0f0, cccccc, 999999, 3333fa, 1111bb, fa3333, 7b1a1a, "
                                       "3333fa, 1111bb, fa3333, 7b1a1a, 33fa33, 0b7b0b, ffffff, 000000\n";
            static const uint8_t strip[1] = {0x01};
            static uint8_t set[1024], spu[2][2600];
            name = seed == 11 ? "h264-webvtt-pgs-vobsub.mkv" : "vp9-refused-srt-pgs-vobsub.mkv";
            if (seed == 11) add_h264(&v, 12, avcc, &avcc_len);
            else
                for (int k = 0; k < 12; k++) {
                    const uint8_t vp9[8] = {0x82, 0x49, 0x83, (uint8_t)k};
                    s_add(&v, vp9, sizeof vp9, frame_ms(k), k == 0);
                }
            t = (Tracks){seed == 11 ? "V_MPEG4/ISO/AVC" : "V_VP9", seed == 11 ? avcc : NULL, avcc_len, 64, 48, NULL, NULL, 0,
                         {{seed == 11 ? "S_TEXT/WEBVTT" : "S_TEXT/UTF8", "eng", NULL, false},
                          {"S_HDMV/PGS", "eng", "English", false},
                          {"S_VOBSUB", "fre", NULL, true, head, sizeof head - 1, seed == 12 ? strip : NULL, 1}}};
            size_t seg = start_file(&in, &t, 400), cl = w_open(&in, 0x1F43B675);
            w_uint(&in, 0xE7, 0);
            for (int k = 0; k < v.n; k++) {
                w_simple(&in, 1, v.ms[k], v.key[k], s_block(&v, k), s_size(&v, k));
                if (k == 2) {
                    const char *cue = seed == 11 ? "<v Ann>Hi <i>you</i> &amp; me</v>" : "Hi <i>you</i>";
                    w_group(&in, 3, 70, 200, cue, strlen(cue));
                    w_simple(&in, 4, 80, true, set, pgs_set(set, 0, 1));
                    spu_of(spu[0], 2600, 0); /* two packs */
                    spu[0][0] = 0x01;        /* (what the stripping takes off) */
                    w_group(&in, 5, 90, 0, spu[0] + (seed == 12), 2600 - (seed == 12));
                }
                if (k == 8) {
                    w_simple(&in, 4, 270, true, set, pgs_set(set, 1, 0));
                    spu_of(spu[1], 300, 1);
                    w_group(&in, 5, 280, 0, spu[1] + (seed == 12), 300 - (seed == 12));
                }
            }
            w_close(&in, cl);
            w_close(&in, seg);
            built = true, fails = seed == 12, subs = 3;
            break;
        }
        case 9: { /* Dolby Digital Plus and two text subtitle tracks (SRT, ASS) */
            name = "mpeg2-eac3-subs.mkv";
            t = (Tracks){"V_MPEG2", NULL, 0, 48, 32, "A_EAC3", NULL, 0,
                         {{"S_TEXT/UTF8", "eng", "English", false}, {"S_TEXT/ASS", "swe", NULL, true}}};
            size_t seg = start_file(&in, &t, 400), cl = w_open(&in, 0x1F43B675);
            w_uint(&in, 0xE7, 0);
            w_group(&in, 3, 10, 300, "Hello <i>there</i>", 18);
            const char *ass = "0,0,Default,,0,0,0,,{\\an8}Hej\\Nd\xC3\xA5";
            w_group(&in, 4, 20, 300, ass, strlen(ass));
            static uint8_t pic[4096];
            for (int k = 0, f = 0; k < 12 || f < 8;) {
                if (k < 12 && (f >= 8 || frame_ms(k) <= f * 16)) w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, k)), k++;
                else w_simple(&in, 2, f * 16, true, eac3 + (size_t)f * 384, 384), f++;
            }
            w_close(&in, cl);
            w_close(&in, seg);
            built = true, subs = 2;
            break;
        }
        case 10: { /* MPEG-1 by its CodecID (the stream inside is MPEG-2: the fuzzer turns it into MPEG-1), AAC
                      sound and SSA subtitles */
            name = "mpeg1-aac-ssa.mkv";
            add_aac(&a, 0.5, asc);
            t = (Tracks){"V_MPEG1", NULL, 0, 48, 32, "A_AAC", asc, 2, {{"S_TEXT/SSA", "eng", NULL, false}}};
            size_t seg = start_file(&in, &t, 400), cl = w_open(&in, 0x1F43B675);
            w_uint(&in, 0xE7, 0);
            const char *ssa = "0,0,Default,,0,0,0,,Hello\\Nthere";
            w_group(&in, 3, 10, 300, ssa, strlen(ssa));
            static uint8_t pic[4096];
            for (int k = 0, f = 0; k < 12 || f < a.n;) {
                if (k < 12 && (f >= a.n || frame_ms(k) <= au_ms(f))) w_simple(&in, 1, frame_ms(k), true, pic, mpeg2_picture(pic, k)), k++;
                else w_simple(&in, 2, au_ms(f), true, s_block(&a, f), s_size(&a, f)), f++;
            }
            w_close(&in, cl);
            w_close(&in, seg);
            built = true, subs = 1;
            break;
        }
        case 13: /* DTS sound (1.3): 5.1 + LFE, ~0.4 s */
            name = "h264-dts.mkv";
            add_h264(&v, 12, avcc, &avcc_len);
            add_dts(&a, 38, true);
            t = (Tracks){"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_DTS", NULL, 0};
            break;
        case 14: { /* AV1 (2.1): copied, never decoded - its record, temporal delimiters on some blocks; Dolby Digital
                      Plus beside it made AAC (an AV1 file used to be refused whole, its sound with it) */
            static const uint8_t av1c[] = {0x81, 0x00, 0x0C, 0x00, 0x0A, 0x05, 0x00, 0x00, 0x00, 0x24, 0x9F};
            name = "av1-eac3.mkv";
            uint8_t frame[24] = {0x32, 22};
            for (int k = 0; k < 12; k++) {
                frame[2] = (uint8_t)k;
                if (k % 3) s_add(&v, frame, sizeof frame, frame_ms(k), k % 6 == 0);
                else {
                    uint8_t td[26] = {0x12, 0};
                    memcpy(td + 2, frame, sizeof frame);
                    s_add(&v, td, sizeof td, frame_ms(k), k % 6 == 0);
                }
            }
            for (int f = 0; f < 8; f++) s_add(&a, eac3 + (size_t)f * 384, 384, f * 16, true);
            t = (Tracks){"V_AV1", av1c, sizeof av1c, 64, 48, "A_EAC3", NULL, 0};
            break;
        }
        case 17: { /* an MP4 in (2.5: the owner's DuckTales) - H.264 + Dolby, read by mkv.h's MP4 side, the sound made AAC */
            name = "h264-ac3.mp4";
            memset(&in, 0, sizeof in); /* (mux() starts it anew for the others: here mp4.c writes into it) */
            add_h264(&v, 12, avcc, &avcc_len);
            uint8_t dac3[3];
            CHECK(mp4_dac3(ac3, 768, dac3));
            Mp4Out mo = {&in, mem_write, mem_write_at};
            Mp4 *m4 = mp4_new(&mo, 0, 1 << 16);
            Mp4Track mv = {.codec = MP4_H264, .timescale = 1000, .config = avcc, .config_len = avcc_len, .width = 64, .height = 48};
            Mp4Track ma = {.codec = MP4_AC3, .timescale = 48000, .config = dac3, .config_len = 3, .channels = 2, .sample_rate = 48000};
            int tv = mp4_add_track(m4, &mv), ta = mp4_add_track(m4, &ma);
            bool w4 = tv >= 0 && ta >= 0 && mp4_begin(m4);
            for (int k = 0; w4 && k < v.n; k++) {
                int next = k + 1 < v.n ? v.ms[k + 1] : v.ms[k] + 33;
                w4 = mp4_sample(m4, tv, v.data.p + v.at[k], v.at[k + 1] - v.at[k], (uint32_t)(next - v.ms[k]), 0, v.key[k]);
                if (w4 && k % 3 == 0) w4 = mp4_sample(m4, ta, ac3 + (size_t)(k / 3 % 4) * 768, 768, 1536, 0, true);
            }
            CHECK(w4 && mp4_finish(m4));
            mp4_free(m4);
            t = (Tracks){"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_AC3", NULL, 0};
            built = true;
            break;
        }
        case 19: { /* MPEG-4 Part 2 (2.5, Xvid as Solsidan S07's): B-VOPs, quarter samples, its MP3 copied */
            static uint8_t mp3[417];
            name = "mpeg4-asp-qpel-mp3.mkv";
            size_t nvol = add_mpeg4(&v, 13, 48, 32, M4_QPEL, false, frame_ms, vol);
            for (int f = 0; f < 17; f++) s_add(&a, mp3, mp3_frame(mp3, false, (uint8_t)f), f * 26, true);
            t = (Tracks){"V_MPEG4/ISO/ASP", vol, nvol, 48, 32, "A_MPEG/L3", NULL, 0, .arate = 44100};
            break;
        }
        case 20: { /* ... video packets and the MPEG quantiser (an odd size), AAC copied */
            name = "mpeg4-sp-resync-mpegquant-aac.mkv";
            size_t nvol = add_mpeg4(&v, 10, 72, 40, M4_RESYNC | M4_MPEGQ, false, frame_ms, vol);
            add_aac(&a, 0.35, asc);
            t = (Tracks){"V_MPEG4/ISO/SP", vol, nvol, 72, 40, "A_AAC", asc, 2};
            break;
        }
        case 21: /* ... an AVI's track (V_MS/VFW/FOURCC, XVID), DivX's packed blocks, Dolby sound made AAC */
            name = "mpeg4-vfw-packed-ac3.mkv";
            add_mpeg4(&v, 13, 48, 32, 0, true, frame_ms, vol);
            for (int f = 0; f < 14; f++) s_add(&a, ac3 + f % 4 * 768, 768, f * 32, true);
            bitmapinfo(bih, 48, 32, "XVID");
            t = (Tracks){"V_MS/VFW/FOURCC", bih, sizeof bih, 48, 32, "A_AC3", NULL, 0};
            break;
        case 23: /* (2.7) three HEVC streams one after another - keyframes at 0, 9 and 18, 0.9 s - AAC: what Full
                    throttle makes in parts (the browser tests: data parts of 300 ms) */
            name = "hevc-three-keyframes-aac.mkv";
            hevc_three(&in);
            t = (Tracks){"V_MPEGH/ISO/HEVC", NULL, 0, 64, 48, "A_AAC", NULL, 0};
            built = true;
            break;
        case 22: { /* HEVC that decodes (2.6): every syntax element hevc_testenc.h writes, 10-bit, AAC copied - copied
                      as 'hvc1' here; convfuzz converts a file it copied HEVC from once more, made H.264 */
            static uint8_t hvcc[2048];
            name = "hevc-random-10bit-aac.mkv";
            HwStream hs = hevc_stream(64, 48, 10, HW_RANDOM | HW_SAO | HW_TMVP | HW_PCM_LF | HW_QP, 9);
            size_t nh = add_hevc(&v, &hs, hvcc, sizeof hvcc);
            add_aac(&a, 0.35, asc);
            t = (Tracks){"V_MPEGH/ISO/HEVC", hvcc, nh, 64, 48, "A_AAC", asc, 2};
            break;
        }
        case 18: { /* Opus copied (2.5): packets of 20 and 60 ms and two frames, one cut short */
            static uint8_t pkt[64];
            name = "h264-opus.mkv";
            add_h264(&v, 12, avcc, &avcc_len);
            for (int f = 0; f < 20; f++) s_add(&a, pkt, opus_packet(pkt, f), f * 20, true);
            t = (Tracks){"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_OPUS", OPUS_HEAD, sizeof OPUS_HEAD};
            break;
        }
        case 16: { /* MP3 copied (2.5): MPEG-1 frames, one block holding two */
            static uint8_t mp3[2 * 417];
            name = "h264-mp3.mkv";
            add_h264(&v, 12, avcc, &avcc_len);
            for (int f = 0; f < 15; f++) {
                size_t n = mp3_frame(mp3, false, (uint8_t)f);
                if (f == 7) n += mp3_frame(mp3 + n, false, 0x5A);
                s_add(&a, mp3, n, f * 26, true);
            }
            t = (Tracks){"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_MPEG/L3", NULL, 0, .arate = 44100};
            break;
        }
        case 15: { /* several sounds (2.2): each kept as its own track - AAC copied, Dolby, Dolby Digital Plus and DTS
                      made AAC, an Opus left out; the default flag on the second, a language as an IETF tag */
            static Snd snd[5];
            memset(snd, 0, sizeof snd);
            name = "h264-five-sounds.mkv";
            add_h264(&v, 12, avcc, &avcc_len);
            add_aac(&snd[0].s, 0.4, asc);
            for (int f = 0; f < 12; f++) s_add(&snd[1].s, ac3 + f % 4 * 768, 768, f * 32, true);
            for (int f = 0; f < 8; f++) s_add(&snd[2].s, eac3 + (size_t)f * 384, 384, f * 16, true);
            add_dts(&snd[3].s, 38, true);
            for (int f = 0; f < 10; f++) s_add(&snd[4].s, "opus", 4, f * 20, true);
            snd[0].codec = "A_AAC", snd[0].lang = "eng", snd[0].name = "English", snd[0].priv = asc, snd[0].priv_len = 2;
            snd[1].codec = "A_AC3", snd[1].lang = "rus", snd[1].name = "Русский", snd[1].is_default = 1;
            snd[2].codec = "A_EAC3", snd[2].ietf = "uk-UA";
            snd[3].codec = "A_DTS", snd[3].lang = "fin";
            snd[4].codec = "A_OPUS", snd[4].lang = "swe";
            mux_sounds(&in, avcc, avcc_len, &v, snd, 5);
            for (int k = 0; k < 5; k++) s_free(&snd[k].s);
            t = (Tracks){"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_AC3", NULL, 0};
            built = true;
            break;
        }
        case 0:
            name = "mpeg2-aac.mkv";
            add_mpeg2(&v, 0, 30, 48, 32, false, 0, false);
            add_aac(&a, 1.0, asc);
            t.acodec = "A_AAC", t.apriv = asc, t.apriv_len = 2;
            break;
        case 1: /* Dolby sound: 12 frames, ~0.4 s */
            name = "mpeg2-ac3.mkv";
            add_mpeg2(&v, 0, 12, 48, 32, false, 0, false);
            for (int f = 0; f < 12; f++) s_add(&a, ac3 + f % 4 * 768, 768, f * 32, true);
            t.acodec = "A_AC3";
            break;
        case 2:
            name = "mpeg2-interlaced.mkv";
            add_mpeg2(&v, 0, 30, 64, 32, true, 70, false);
            t.width = 64;
            break;
        case 3:
            name = "mpeg2-telecine.mkv";
            add_mpeg2(&v, 0, 30, 64, 32, true, 70, true);
            t.width = 64;
            break;
        case 4: /* several sequence headers: the size changes part-way */
            name = "mpeg2-sizes.mkv";
            add_mpeg2(&v, 0, 8, 48, 32, false, 0, false);
            add_mpeg2(&v, 8, 8, 48, 16, false, 0, false);
            fails = true;
            break;
        case 5:
        case 6:
            name = seed == 5 ? "h264-aac.mkv" : "h264-aac-laced.mkv";
            add_h264(&v, 20, avcc, &avcc_len);
            add_aac(&a, 0.7, asc);
            t = (Tracks){"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, "A_AAC", asc, 2};
            laced = seed == 6;
            break;
        case 7:
            name = "h264.mkv";
            add_h264(&v, 10, avcc, &avcc_len);
            t = (Tracks){"V_MPEG4/ISO/AVC", avcc, avcc_len, 64, 48, NULL, NULL, 0};
            break;
        default: { /* HEVC: copied, never decoded - the record and length-prefixed units are what matter */
            name = "hevc.mkv";
            uint8_t frame[24] = {0, 0, 0, 20};
            for (int k = 0; k < 8; k++) frame[4] = (uint8_t)k, s_add(&v, frame, sizeof frame, frame_ms(k), k % 4 == 0);
            t = (Tracks){"V_MPEGH/ISO/HEVC", hvcc, sizeof hvcc, 64, 48, NULL, NULL, 0};
        }
        }
        if (built) {
        } else if (laced) mux_laced(&in, &t, v.n * 33.4, &v, &a);
        else mux(&in, &t, v.n * 33.4, &v, t.acodec ? &a : NULL);
        AvConvJob j = {.want_subs = true};
        Seen s;
        bool ok = run(&in, &out, &j, &s);
        if (fails) CHECK(!ok && j.unsupported);
        else if (!ok) test_fail(__FILE__, __LINE__, "seed %s: %s", name, j.err);
        if (subs >= 0) CHECK_INT(j.nsubs, subs);
        av_convert_free(&j);
        const char *ids[2 + MANY_SUBS] = {t.vcodec, t.acodec};
        for (int i = 0; i < MANY_SUBS; i++) ids[2 + i] = t.sub[i].codec;
        for (int i = 0; i < 2 + MANY_SUBS && ntook < 64; i++)
            if (ids[i]) snprintf(took[ntook++], sizeof took[0], "%s", ids[i]);
        if (dir && *dir) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dir, name);
            CHECK(file_write_atomic(path, in.p, in.len));
        }
        free(in.p), free(out.p);
        s_free(&v), s_free(&a);
    }
    /* Every format the converter takes has a seed carrying it - so the fuzzing (every push, every night)
       starts from it. A format added to AV_CONV_CODECS without one fails here (docs/decisions/0006). */
    for (const AvCodec *k = AV_CONV_CODECS; k->id; k++) {
        bool seeded = false;
        for (int i = 0; i < ntook && !seeded; i++) seeded = av_conv_codec(took[i]) == k;
        if (!seeded) test_fail(__FILE__, __LINE__, "the converter takes %s, but no fuzz seed carries it", k->id);
    }
}
