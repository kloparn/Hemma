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

typedef struct {
    const char *vcodec;
    const uint8_t *vpriv;
    size_t vpriv_len;
    int width, height;
    const char *acodec; /* NULL: no sound */
    const uint8_t *apriv;
    size_t apriv_len;
    struct { /* subtitle tracks 3 and 4 (codec NULL: none) */
        const char *codec, *lang, *name;
        bool forced;
    } sub[2];
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
        w_float(w, 0xB5, 48000.0);
        w_uint(w, 0x9F, 2);
        w_close(w, a);
        w_close(w, tr);
    }
    for (int k = 0; k < 2; k++) {
        if (!t->sub[k].codec) continue;
        tr = w_open(w, 0xAE);
        w_uint(w, 0xD7, (uint64_t)(3 + k));
        w_uint(w, 0x83, 0x11);
        w_bin(w, 0x86, t->sub[k].codec, strlen(t->sub[k].codec));
        if (t->sub[k].lang) w_bin(w, 0x22B59C, t->sub[k].lang, strlen(t->sub[k].lang));
        if (t->sub[k].name) w_bin(w, 0x536E, t->sub[k].name, strlen(t->sub[k].name));
        if (t->sub[k].forced) w_uint(w, 0x55AA, 1);
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

TEST(conv_says_what_it_cannot_convert) {
    W in, out;
    AvConvJob j = {0};
    Seen s;
    Tracks av1 = {"V_AV1", NULL, 0, 64, 48, NULL, NULL, 0};
    size_t seg = start_file(&in, &av1, 1000);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Its picture is AV1");
    CHECK_INT((int)out.len, 0); /* nothing written */
    free(in.p), free(out.p);

    Tracks dts = {"V_MPEG2", NULL, 0, 48, 32, "A_DTS", NULL, 0};
    seg = start_file(&in, &dts, 1000);
    w_close(&in, seg);
    CHECK(!run(&in, &out, &j, &s));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "Its sound is DTS");
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

/* Sound the converter can't change: said in words people know, and nothing is made. */
TEST(conv_names_the_sound_it_cant_convert) {
    W in;
    Tracks t = {"V_MPEG2", NULL, 0, 48, 32, "A_DTS", NULL, 0, {{"S_TEXT/UTF8", "eng", NULL, false}}};
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
    CHECK_STR(j.err, "Its sound is DTS, which the converter can't change yet (browsers play it silently)");
    CHECK_INT(j.nsubs, 0);
    free(in.p), free(out.p);
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

/* `seconds` of AAC; its AudioSpecificConfig goes to asc */
static void add_aac(Stream *s, double seconds, uint8_t asc[2]) {
    static uint8_t aus[200][AAC_MAX_AU];
    int sizes[200], n = make_aac(aus, sizes, 200, seconds, asc);
    for (int a = 0; a < n; a++) s_add(s, aus[a], (size_t)sizes[a], au_ms(a), true);
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
    if (raw && *raw) { /* tools/avfuzz.c's seeds: raw Dolby frames and MPEG-2 streams, each after its options byte */
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
        buf_free(&b);
    }
    static const uint8_t hvcc[] = {1,    0x01, 0x60, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 0x5D, 0xF0, 0, 0xFC,
                                   0xFD, 0xF8, 0xF8, 0, 0, 0x0F, 1,    0x20, 0, 1, 0, 2, 0x40, 0x01};
    char took[64][40]; /* every CodecID a seed carries: each format the converter takes needs one (below) */
    int ntook = 0;
    for (int seed = 0; seed < 11; seed++) {
        uint8_t avcc[256], asc[2];
        size_t avcc_len = 0;
        Tracks mp2 = {"V_MPEG2", NULL, 0, 48, 32, NULL, NULL, 0}, t = mp2;
        const char *name = "";
        bool fails = false, laced = false, built = false;
        W in, out;
        switch (seed) {
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
            built = true;
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
        if (built) CHECK_INT(j.nsubs, seed == 10 ? 1 : 2);
        av_convert_free(&j);
        const char *ids[] = {t.vcodec, t.acodec, t.sub[0].codec, t.sub[1].codec};
        for (int i = 0; i < 4 && ntook < 64; i++)
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
