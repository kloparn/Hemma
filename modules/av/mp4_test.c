/* modules/av/mp4_test.c — MP4 writer: box layouts read back byte by byte,
 * sample offsets that really point at the samples, the faststart fix-up (and
 * its fallback), co64, ctts/edit lists, fragments (sequence numbers, tfdt,
 * data offsets), and the avcC / Annex B / dac3 helpers. Chrome is the second
 * opinion (tools/muxcheck.c plays real files). */
#include "../../tests/test.h"
#include "mp4.h"

/* ------------------------------------------------------- memory output */

typedef struct {
    uint8_t *p;
    size_t len, cap;
} Mem;

static bool mem_write(void *ctx, const void *p, size_t n) {
    Mem *m = ctx;
    if (m->len + n > m->cap) {
        m->cap = (m->len + n) * 2 + 4096;
        m->p = realloc(m->p, m->cap);
    }
    memcpy(m->p + m->len, p, n);
    m->len += n;
    return true;
}

static bool mem_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    Mem *m = ctx;
    if (off < 0 || (size_t)off + n > m->len) return false;
    memcpy(m->p + off, p, n);
    return true;
}

/* ------------------------------------------------------------- box reader */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

typedef struct {
    const uint8_t *p; /* payload (after the 8/16-byte header) */
    size_t n;
    size_t at; /* offset of the box itself in the file */
} Box;

/* The k-th child of type `type` in p[0..n) (n = payload of the parent). */
static bool child(const uint8_t *base, const uint8_t *p, size_t n, const char *type, int k, Box *out) {
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t sz = be32(p + i);
        size_t hdr = 8;
        if (sz == 1) sz = be64(p + i + 8), hdr = 16;
        if (sz < hdr || i + sz > n) return false;
        if (!memcmp(p + i + 4, type, 4) && k-- == 0) {
            *out = (Box){p + i + hdr, (size_t)sz - hdr, (size_t)(p + i - base)};
            return true;
        }
        i += (size_t)sz;
    }
    return false;
}

/* "moov/trak.1/mdia/minf/stbl/stsz": '.k' picks the k-th box of a type. Sample
   entries are entered with "stsd/avc1" (the reader skips the entry fields). */
static bool path(const Mem *f, const char *path_str, Box *out) {
    Box cur = {f->p, f->len, 0};
    char seg[16];
    const char *s = path_str;
    while (*s) {
        size_t l = strcspn(s, "/");
        memcpy(seg, s, l);
        seg[l] = '\0';
        s += l + (s[l] == '/');
        int k = 0;
        char *dot = strchr(seg, '.');
        if (dot) k = atoi(dot + 1), *dot = '\0';
        const uint8_t *p = cur.p;
        size_t n = cur.n;
        if (cur.at && !memcmp(f->p + cur.at + 4, "stsd", 4)) p += 8, n -= 8; /* version/flags + entry count */
        else if (cur.at && (!memcmp(f->p + cur.at + 4, "avc1", 4))) p += 78, n -= 78;
        else if (cur.at && (!memcmp(f->p + cur.at + 4, "fLaC", 4) || !memcmp(f->p + cur.at + 4, "mp4a", 4) ||
                            !memcmp(f->p + cur.at + 4, "ac-3", 4)))
            p += 28, n -= 28;
        else if (cur.at && (!memcmp(f->p + cur.at + 4, "dref", 4))) p += 8, n -= 8;
        if (!child(f->p, p, n, seg, k, &cur)) return false;
    }
    *out = cur;
    return true;
}

static int top_level(const Mem *f, char types[][5], int max) {
    int k = 0;
    size_t i = 0;
    while (i + 8 <= f->len && k < max) {
        uint64_t sz = be32(f->p + i);
        if (sz == 1) sz = be64(f->p + i + 8);
        memcpy(types[k], f->p + i + 4, 4);
        types[k++][4] = '\0';
        if (sz < 8) break;
        i += (size_t)sz;
    }
    return k;
}

/* Byte offset of sample s (0-based) of a track, from stsz/stsc/stco|co64. */
static int64_t sample_offset(const Mem *f, int track, uint32_t s, uint32_t *size) {
    char p[96];
    Box stsz, stsc, stco;
    snprintf(p, sizeof p, "moov/trak.%d/mdia/minf/stbl/stsz", track);
    if (!path(f, p, &stsz)) return -1;
    snprintf(p, sizeof p, "moov/trak.%d/mdia/minf/stbl/stsc", track);
    if (!path(f, p, &stsc)) return -1;
    bool co64 = false;
    snprintf(p, sizeof p, "moov/trak.%d/mdia/minf/stbl/stco", track);
    if (!path(f, p, &stco)) {
        snprintf(p, sizeof p, "moov/trak.%d/mdia/minf/stbl/co64", track);
        if (!path(f, p, &stco)) return -1;
        co64 = true;
    }
    uint32_t nruns = be32(stsc.p + 4), nchunks = be32(stco.p + 4);
    uint32_t sample = 0;
    for (uint32_t c = 1; c <= nchunks; c++) {
        uint32_t per = 0;
        for (uint32_t r = 0; r < nruns; r++)
            if (be32(stsc.p + 8 + r * 12) <= c) per = be32(stsc.p + 8 + r * 12 + 4);
        int64_t off = co64 ? (int64_t)be64(stco.p + 8 + (c - 1) * 8) : (int64_t)be32(stco.p + 8 + (c - 1) * 4);
        for (uint32_t k = 0; k < per; k++, sample++) {
            uint32_t sz = be32(stsz.p + 12 + sample * 4);
            if (sample == s) {
                *size = sz;
                return off;
            }
            off += sz;
        }
    }
    return -1;
}

/* ----------------------------------------------------------------- data */

static const uint8_t SPS[] = {0x67, 0x64, 0x00, 0x1F, 0xAC, 0xD9, 0x40, 0x50, 0x05, 0xBB, 0x01, 0x10};
static const uint8_t PPS[] = {0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0};

static void fill(uint8_t *p, size_t n, int seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed * 31 + i * 7);
}

static Mp4 *two_tracks(Mem *f, int flags, int64_t reserve, int *v, int *a) {
    Mp4Out o = {f, mem_write, mem_write_at};
    Mp4 *m = mp4_new(&o, flags, reserve);
    uint8_t avcc[64];
    size_t an = mp4_avcc(SPS, sizeof SPS, PPS, sizeof PPS, avcc, sizeof avcc);
    Mp4Track vt = {.codec = MP4_H264, .timescale = 30000, .config = avcc, .config_len = an, .width = 720, .height = 480,
                   .sar_num = 8, .sar_den = 9, .primaries = 6, .transfer = 6, .matrix = 6, .lang = "eng"};
    uint8_t si[34];
    fill(si, sizeof si, 9);
    Mp4Track at = {.codec = MP4_FLAC, .timescale = 48000, .config = si, .config_len = 34, .channels = 2,
                   .sample_rate = 48000, .sample_bits = 16, .lang = "swe"};
    *v = mp4_add_track(m, &vt);
    *a = mp4_add_track(m, &at);
    return m;
}

/* ----------------------------------------------------------------- tests */

TEST(mp4_faststart_file_reads_back) {
    Mem f = {0};
    int v, a;
    Mp4 *m = two_tracks(&f, 0, mp4_moov_bound(200, 100), &v, &a);
    CHECK(m && v == 0 && a == 1);
    CHECK(mp4_begin(m));
    static uint8_t buf[50][3000];
    /* 30 video frames (key every 10th) interleaved with 20 audio frames */
    int vs = 0;
    for (int i = 0; i < 50; i++) {
        bool video = (i % 5) < 3;
        size_t n = 100 + (size_t)(i * 37 % 2500);
        fill(buf[i], n, i);
        if (video) CHECK(mp4_sample(m, v, buf[i], n, 1001, 0, vs++ % 10 == 0));
        else CHECK(mp4_sample(m, a, buf[i], n, 4096, 0, true));
    }
    uint8_t final_si[34];
    fill(final_si, sizeof final_si, 77);
    CHECK(mp4_set_config(m, a, final_si, sizeof final_si)); /* the real STREAMINFO arrives at the end */
    CHECK(mp4_finish(m));
    CHECK(mp4_faststart(m));
    CHECK_INT(mp4_bytes(m), f.len);
    mp4_free(m);

    char t[8][5];
    int nt = top_level(&f, t, 8);
    CHECK_INT(nt, 4);
    CHECK_STR(t[0], "ftyp");
    CHECK_STR(t[1], "moov");
    CHECK_STR(t[2], "free");
    CHECK_STR(t[3], "mdat");
    Box b;
    CHECK(path(&f, "mdat", &b));
    CHECK_INT(b.at + 16 + b.n, f.len); /* 64-bit mdat size patched to the real length */
    /* every sample is where the index says */
    for (int s = 0, vi = 0, ai = 0; s < 50; s++) {
        bool video = (s % 5) < 3;
        uint32_t sz = 0;
        int64_t off = sample_offset(&f, video ? 0 : 1, (uint32_t)(video ? vi++ : ai++), &sz);
        size_t n = 100 + (size_t)(s * 37 % 2500);
        CHECK(off > 0 && sz == n && (size_t)off + n <= f.len);
        CHECK(memcmp(f.p + off, buf[s], n) == 0);
    }
    /* timing and sync tables */
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stts", &b));
    CHECK(be32(b.p + 4) == 1 && be32(b.p + 8) == 30 && be32(b.p + 12) == 1001);
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stss", &b));
    CHECK(be32(b.p + 4) == 3 && be32(b.p + 8) == 1 && be32(b.p + 12) == 11 && be32(b.p + 16) == 21);
    CHECK(!path(&f, "moov/trak.1/mdia/minf/stbl/stss", &b)); /* audio: every sample is a sync sample */
    CHECK(!path(&f, "moov/trak.0/mdia/minf/stbl/ctts", &b));
    CHECK(!path(&f, "moov/trak.0/edts", &b));
    CHECK(path(&f, "moov/trak.0/mdia/mdhd", &b));
    CHECK(be32(b.p + 12) == 30000 && be32(b.p + 16) == 30 * 1001);
    CHECK(be32(b.p + 20) >> 16 == ((('e' - 0x60) << 10) | (('n' - 0x60) << 5) | ('g' - 0x60)));
    CHECK(path(&f, "moov/mvhd", &b));
    CHECK_INT(be32(b.p + 16), 1707); /* the longest track: audio 20 x 4096/48000 s (video is 30 x 1001/30000 = 1001 ms) */
    CHECK(path(&f, "moov/trak.1/tkhd", &b));
    CHECK_INT(be32(b.p + 20), 1707);
    CHECK_INT(be32(b.p + 32), 1);           /* audio: layer 0, alternate group 1 */
    CHECK_INT(be32(b.p + 36) >> 16, 0x100); /* volume 1.0 */
    /* sample entries */
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stsd/avc1", &b));
    CHECK(be32(b.p + 24) >> 16 == 720 && (be32(b.p + 24) & 0xFFFF) == 480);
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stsd/avc1/avcC", &b));
    CHECK(b.p[0] == 1 && b.p[1] == 0x64 && b.p[4] == 0xFF && b.p[5] == 0xE1);
    CHECK(b.n == 6 + 2 + sizeof SPS + 3 + sizeof PPS + 4); /* High profile: chroma/bit-depth extension */
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stsd/avc1/pasp", &b));
    CHECK(be32(b.p) == 8 && be32(b.p + 4) == 9);
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stsd/avc1/colr", &b));
    CHECK(!memcmp(b.p, "nclx", 4) && b.p[5] == 6 && b.p[7] == 6 && b.p[9] == 6 && b.p[10] == 0);
    CHECK(path(&f, "moov/trak.0/tkhd", &b));
    CHECK_INT(be32(b.p + 76) >> 16, 640); /* display width: 720 x 8/9 */
    CHECK(path(&f, "moov/trak.1/mdia/minf/stbl/stsd/fLaC/dfLa", &b));
    CHECK(b.p[4] == 0x80 && b.p[7] == 34 && !memcmp(b.p + 8, final_si, 34));
    free(f.p);
}

TEST(mp4_moov_goes_last_when_the_reservation_is_too_small) {
    Mem f = {0};
    int v, a;
    Mp4 *m = two_tracks(&f, 0, 64, &v, &a);
    CHECK(mp4_begin(m));
    uint8_t s[500];
    for (int i = 0; i < 20; i++) {
        fill(s, sizeof s, i);
        CHECK(mp4_sample(m, i & 1 ? a : v, s, sizeof s, i & 1 ? 1024 : 1001, 0, true));
    }
    CHECK(mp4_finish(m));
    CHECK(!mp4_faststart(m));
    mp4_free(m);
    char t[8][5];
    CHECK_INT(top_level(&f, t, 8), 4);
    CHECK_STR(t[1], "free");
    CHECK_STR(t[2], "mdat");
    CHECK_STR(t[3], "moov");
    uint32_t sz;
    int64_t off = sample_offset(&f, 1, 9, &sz);
    fill(s, sizeof s, 19);
    CHECK(off > 0 && sz == sizeof s && !memcmp(f.p + off, s, sizeof s));
    free(f.p);
}

TEST(mp4_switches_to_co64_for_big_offsets) {
    Mem f = {0};
    int v, a;
    Mp4 *m = two_tracks(&f, 0, 4096, &v, &a);
    mp4_set_co64_threshold(m, 5000); /* stands in for 4 GB */
    CHECK(mp4_begin(m));
    uint8_t s[1000];
    for (int i = 0; i < 12; i++) { /* alternating tracks: a new chunk (and offset) each time */
        fill(s, sizeof s, i);
        CHECK(mp4_sample(m, v, s, sizeof s, 1001, 0, i == 0));
        fill(s, 200, 100 + i);
        CHECK(mp4_sample(m, a, s, 200, 4096, 0, true));
    }
    CHECK(mp4_finish(m));
    mp4_free(m);
    Box b;
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/co64", &b));
    CHECK(!path(&f, "moov/trak.0/mdia/minf/stbl/stco", &b));
    uint32_t sz;
    int64_t off = sample_offset(&f, 0, 11, &sz);
    fill(s, sizeof s, 11);
    CHECK(off > 5000 && !memcmp(f.p + off, s, sizeof s));
    free(f.p);

    /* below the threshold it stays 32-bit */
    Mem g = {0};
    m = two_tracks(&g, 0, 4096, &v, &a);
    CHECK(mp4_begin(m));
    CHECK(mp4_sample(m, v, s, sizeof s, 1001, 0, true));
    CHECK(mp4_finish(m));
    mp4_free(m);
    CHECK(path(&g, "moov/trak.0/mdia/minf/stbl/stco", &b));
    free(g.p);
}

TEST(mp4_writes_ctts_and_edit_lists_only_when_needed) {
    Mem f = {0};
    Mp4Out o = {&f, mem_write, mem_write_at};
    Mp4 *m = mp4_new(&o, 0, 8192);
    uint8_t avcc[64];
    size_t an = mp4_avcc(SPS, sizeof SPS, PPS, sizeof PPS, avcc, sizeof avcc);
    Mp4Track vt = {.codec = MP4_H264, .timescale = 24000, .config = avcc, .config_len = an, .width = 1280, .height = 720,
                   .start = 2002};
    Mp4Track at = {.codec = MP4_AAC, .timescale = 48000, .config = (const uint8_t *)"\x11\x90", .config_len = 2,
                   .channels = 2, .sample_rate = 48000, .delay = 4800};
    int v = mp4_add_track(m, &vt), a = mp4_add_track(m, &at);
    CHECK(mp4_begin(m));
    /* I P B B P B B: decode order with composition offsets */
    static const int32_t cts[] = {2002, 5005, 1001, 1001, 4004, 0, 0};
    uint8_t s[64] = {0};
    for (int i = 0; i < 7; i++) CHECK(mp4_sample(m, v, s, sizeof s, 1001, cts[i], i == 0));
    for (int i = 0; i < 4; i++) CHECK(mp4_sample(m, a, s, 16, 1024, 0, true));
    CHECK(mp4_finish(m));
    mp4_free(m);
    Box b;
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/ctts", &b));
    CHECK(b.p[0] == 0);           /* version 0: all offsets >= 0 */
    CHECK_INT(be32(b.p + 4), 5);  /* runs: 2002, 5005, 1001x2, 4004, 0x2 */
    CHECK(be32(b.p + 16) == 1 && be32(b.p + 20) == 5005);
    CHECK(be32(b.p + 24) == 2 && be32(b.p + 28) == 1001);
    CHECK(path(&f, "moov/trak.0/edts/elst", &b));
    CHECK(b.p[0] == 1 && be32(b.p + 4) == 1);
    CHECK_INT(be64(b.p + 16), 2002); /* media time 2002 shows at 0 */
    CHECK_INT(be64(b.p + 8), 292);   /* from 2002 to where the last shown ends (8008 + 1001): 7007 / 24000 s = 292 ms
                                        (2.6.1: it was 7 x 1001 - 2002 - the sum of lengths less the start - which hid
                                        a B-frame stream's last pictures) */
    CHECK(path(&f, "moov/trak.1/edts/elst", &b));
    CHECK(be32(b.p + 4) == 2);
    CHECK_INT(be64(b.p + 8), 100);          /* 4800 / 48000 s of nothing first ... */
    CHECK(be64(b.p + 16) == UINT64_MAX);    /* ... as an empty edit */
    CHECK(path(&f, "moov/trak.1/mdia/minf/stbl/stsd/mp4a/esds", &b));
    CHECK(b.n == 31 && b.p[4] == 0x03 && b.p[5] == 25); /* ES_Descriptor covers the rest */
    CHECK(b.p[9] == 0x04 && b.p[10] == 17);              /* DecoderConfigDescriptor: 13 + DSI (2 + 2) */
    CHECK(b.p[11] == 0x40 && b.p[24] == 0x05 && b.p[25] == 2 && b.p[26] == 0x11 && b.p[27] == 0x90);
    CHECK(b.p[28] == 0x06 && b.p[29] == 1 && b.p[30] == 2);
    free(f.p);

    /* a negative offset needs ctts version 1 */
    Mem g = {0};
    o.ctx = &g;
    m = mp4_new(&o, 0, 4096);
    vt.start = 0;
    v = mp4_add_track(m, &vt);
    CHECK(mp4_begin(m));
    CHECK(mp4_sample(m, v, s, 8, 1001, 0, true));
    CHECK(mp4_sample(m, v, s, 8, 1001, -1001, false));
    CHECK(mp4_finish(m));
    mp4_free(m);
    CHECK(path(&g, "moov/trak.0/mdia/minf/stbl/ctts", &b));
    CHECK(b.p[0] == 1);
    free(g.p);
}

TEST(mp4_skips_aac_priming_with_an_edit_list) {
    /* an AAC encoder's first 1024 samples are priming: media time 1024 is shown at 0 */
    Mem f = {0};
    Mp4Out o = {&f, mem_write, mem_write_at};
    Mp4 *m = mp4_new(&o, 0, 4096);
    static const uint8_t asc[2] = {0x11, 0x90}; /* AAC-LC, 48 kHz, stereo */
    Mp4Track at = {.codec = MP4_AAC, .timescale = 48000, .config = asc, .config_len = 2, .channels = 2,
                   .sample_rate = 48000, .start = 1024, .avg_bitrate = 160000, .max_bitrate = 192000};
    int a = mp4_add_track(m, &at);
    CHECK(mp4_begin(m));
    uint8_t s[300] = {0};
    for (int i = 0; i < 48; i++) CHECK(mp4_sample(m, a, s, 200 + (size_t)i, 1024, 0, true));
    CHECK(mp4_finish(m));
    mp4_free(m);
    Box b;
    CHECK(path(&f, "moov/trak.0/edts/elst", &b));
    CHECK(be32(b.p + 4) == 1);
    CHECK_INT(be64(b.p + 16), 1024);                      /* media_time: skip the priming */
    CHECK_INT(be64(b.p + 8), 1003); /* (48 - 1) x 1024 samples = 1002.67 ms */
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stsd/mp4a/esds", &b));
    CHECK(b.p[26] == 0x11 && b.p[27] == 0x90);
    CHECK(be32(b.p + 16) == 192000 && be32(b.p + 20) == 160000); /* max, avg bitrate */
    free(f.p);
}

/* tkhd's flags (enabled = 1) and alternate group, mdhd's language, hdlr's name of the k-th trak */
static void trak_facts(const Mem *f, int k, uint32_t *flags, int *group, char lang[4], char *name, size_t cap) {
    char p[64];
    Box b;
    *flags = 0, *group = -1, lang[0] = name[0] = '\0';
    snprintf(p, sizeof p, "moov/trak.%d/tkhd", k);
    if (path(f, p, &b)) {
        bool v1 = b.p[0] == 1;
        *flags = be32(b.p) & 0xFFFFFF;
        const uint8_t *q = b.p + (v1 ? 4 + 8 + 8 + 4 + 4 + 8 : 4 + 4 + 4 + 4 + 4 + 4) + 8 + 2;
        *group = q[0] << 8 | q[1];
    }
    snprintf(p, sizeof p, "moov/trak.%d/mdia/mdhd", k);
    if (path(f, p, &b)) {
        const uint8_t *q = b.p + (b.p[0] == 1 ? 4 + 8 + 8 + 4 + 8 : 4 + 4 + 4 + 4 + 4);
        int v = q[0] << 8 | q[1];
        lang[0] = (char)(0x60 + (v >> 10 & 31)), lang[1] = (char)(0x60 + (v >> 5 & 31)), lang[2] = (char)(0x60 + (v & 31)), lang[3] = 0;
    }
    snprintf(p, sizeof p, "moov/trak.%d/mdia/hdlr", k);
    if (path(f, p, &b) && b.n > 24) snprintf(name, cap, "%.*s", (int)(b.n - 24), (const char *)b.p + 24);
}

/* 2.2: several sounds of one group - each its language and name, only the default one enabled; a dropped
   track (one that went undecodable) leaves the moov and hands "enabled" on; the others keep their ids. */
TEST(mp4_names_sounds_and_drops_a_track) {
    static const uint8_t asc[2] = {0x11, 0x90};
    /* 63 bytes of "a", then "ö" (2 bytes) across the 64-byte cut: the cut steps back before it */
    char longname[80];
    memset(longname, 'a', 63);
    memcpy(longname + 63, "\xC3\xB6xyz", 6);
    for (int drop = 0; drop < 2; drop++) {
        Mem f = {0};
        int v, a;
        Mp4 *m = two_tracks(&f, 0, 8192, &v, &a); /* (a FLAC track: a sound too, on by default) */
        Mp4Track eng = {.codec = MP4_AAC, .timescale = 48000, .config = asc, .config_len = 2, .channels = 2, .sample_rate = 48000,
                        .lang = "eng", .name = "English", .off = true};
        Mp4Track swe = eng;
        memcpy(swe.lang, "swe", 4);
        swe.name = longname;
        int e = mp4_add_track(m, &eng), s = mp4_add_track(m, &swe);
        CHECK(e == 2 && s == 3);
        CHECK(mp4_begin(m));
        uint8_t data[64];
        for (int i = 0; i < 6; i++) {
            fill(data, sizeof data, i);
            CHECK(mp4_sample(m, v, data, 40, 1001, 0, i == 0));
            CHECK(mp4_sample(m, a, data, 30, 1024, 0, true));
            CHECK(mp4_sample(m, e, data, 20 + (size_t)i, 1024, 0, true));
            CHECK(mp4_sample(m, s, data, 10, 1024, 0, true));
        }
        if (drop) {
            CHECK(mp4_drop_track(m, a)); /* the enabled sound goes: the first one left (English) takes over */
            CHECK(!mp4_drop_track(m, a));
            CHECK(!mp4_sample(m, a, data, 30, 1024, 0, true));
            CHECK(mp4_sample(m, e, data, 26, 1024, 0, true));
        }
        CHECK(mp4_finish(m));
        CHECK(!mp4_drop_track(m, e)); /* too late */
        mp4_free(m);
        uint32_t flags;
        int group;
        char lang[4], name[96];
        int k = 0;
        trak_facts(&f, k++, &flags, &group, lang, name, sizeof name);
        CHECK_INT((int)flags, 3);
        CHECK_INT(group, 0);
        CHECK_STR(name, "VideoHandler");
        if (!drop) {
            trak_facts(&f, k++, &flags, &group, lang, name, sizeof name);
            CHECK_INT((int)flags, 3);
            CHECK_INT(group, 1);
            CHECK_STR(lang, "swe");
            CHECK_STR(name, "SoundHandler");
        }
        trak_facts(&f, k++, &flags, &group, lang, name, sizeof name);
        CHECK_INT((int)flags, drop ? 3 : 2); /* in movie; enabled once the default went */
        CHECK_INT(group, 1);
        CHECK_STR(lang, "eng");
        CHECK_STR(name, "English");
        trak_facts(&f, k++, &flags, &group, lang, name, sizeof name);
        CHECK_INT((int)flags, 2);
        CHECK_STR(lang, "swe");
        CHECK_INT((int)strlen(name), 63); /* cut before the "ö", never inside it */
        Box b;
        char p[64];
        snprintf(p, sizeof p, "moov/trak.%d", k);
        CHECK(!path(&f, p, &b)); /* (a dropped one left out) */
        snprintf(p, sizeof p, "moov/trak.%d/tkhd", k - 1);
        CHECK(path(&f, p, &b));
        CHECK_INT((int)be32(b.p + 12), 4); /* the Swedish one keeps id 4 */
        CHECK(path(&f, "moov/mvhd", &b));
        CHECK_INT((int)be32(b.p + b.n - 4), 5); /* next track id */
        CHECK(path(&f, drop ? "moov/trak.1/mdia/minf/stbl/stsz" : "moov/trak.2/mdia/minf/stbl/stsz", &b));
        CHECK_INT((int)be32(b.p + 8), drop ? 7 : 6);
        uint32_t size = 0;
        int64_t off = sample_offset(&f, drop ? 1 : 2, drop ? 6 : 5, &size); /* the English track's last sample */
        CHECK(off > 0 && size == (drop ? 26u : 25u));
        fill(data, sizeof data, 5); /* (what the loop's last turn wrote) */
        CHECK(off > 0 && !memcmp(f.p + off, data, size));
        free(f.p);
    }

    /* fragments: a track dropped before the start isn't in the init segment; after it, it can't go */
    Mem f = {0};
    int v, a;
    Mp4 *m = two_tracks(&f, MP4_FRAGMENTED, 0, &v, &a);
    CHECK(mp4_drop_track(m, a));
    CHECK(mp4_begin(m));
    CHECK(!mp4_drop_track(m, v));
    uint8_t data[16] = {0};
    CHECK(mp4_sample(m, v, data, 16, 1001, 0, true));
    CHECK(!mp4_sample(m, a, data, 16, 1024, 0, true));
    CHECK(mp4_finish(m));
    mp4_free(m);
    Box b;
    CHECK(path(&f, "moov/trak.0", &b));
    CHECK(!path(&f, "moov/trak.1", &b));
    CHECK(path(&f, "moov/mvex/trex.0", &b));
    CHECK(!path(&f, "moov/mvex/trex.1", &b));
    free(f.p);
}

TEST(mp4_fragments_carry_sequence_tfdt_and_data_offsets) {
    Mem f = {0};
    Mp4Out o = {&f, mem_write, NULL}; /* append-only output is fine for fragments */
    int v, a;
    Mp4 *m;
    {
        Mp4Out oo = o;
        m = mp4_new(&oo, MP4_FRAGMENTED, 0);
        uint8_t avcc[64];
        size_t an = mp4_avcc(SPS, sizeof SPS, PPS, sizeof PPS, avcc, sizeof avcc);
        Mp4Track vt = {.codec = MP4_H264, .timescale = 30000, .config = avcc, .config_len = an, .width = 640, .height = 360};
        uint8_t si[34] = {0};
        Mp4Track at = {.codec = MP4_FLAC, .timescale = 48000, .config = si, .config_len = 34, .channels = 2,
                       .sample_rate = 48000, .sample_bits = 16};
        v = mp4_add_track(m, &vt);
        a = mp4_add_track(m, &at);
    }
    CHECK(mp4_begin(m));
    size_t init = f.len;
    uint8_t s[300];
    for (int frag = 0; frag < 3; frag++) {
        for (int i = 0; i < 5; i++) {
            fill(s, 100 + (size_t)i, frag * 10 + i);
            CHECK(mp4_sample(m, v, s, 100 + (size_t)i, 1001, i == 1 ? 2002 : 0, i == 0));
            fill(s, 50 + (size_t)i, 100 + frag * 10 + i);
            CHECK(mp4_sample(m, a, s, 50 + (size_t)i, 1600, 0, true));
        }
        CHECK(frag == 2 ? mp4_finish(m) : mp4_fragment(m));
    }
    mp4_free(m);
    char t[10][5];
    int nt = top_level(&f, t, 10);
    CHECK_INT(nt, 8);
    CHECK_STR(t[0], "ftyp");
    CHECK_STR(t[1], "moov");
    CHECK_STR(t[2], "moof");
    CHECK_STR(t[3], "mdat");
    CHECK_STR(t[7], "mdat");
    Box b;
    CHECK(path(&f, "moov/mvex/trex.1", &b));
    CHECK(be32(b.p + 4) == 2 && be32(b.p + 8) == 1);
    CHECK(path(&f, "moov/trak.0/mdia/minf/stbl/stsz", &b));
    CHECK_INT(be32(b.p + 8), 0); /* empty tables in the init segment */
    /* walk the fragments */
    size_t pos = init;
    for (int frag = 0; frag < 3; frag++) {
        Mem one = {f.p + pos, f.len - pos, 0};
        Box moof, mdat;
        CHECK(path(&one, "moof", &moof));
        CHECK(path(&one, "moof/mfhd", &b));
        CHECK_INT(be32(b.p + 4), frag + 1);
        CHECK(path(&one, "moof/traf.0/tfdt", &b));
        CHECK(b.p[0] == 1 && be64(b.p + 4) == (uint64_t)frag * 5 * 1001);
        CHECK(path(&one, "moof/traf.1/tfdt", &b));
        CHECK(be64(b.p + 4) == (uint64_t)frag * 5 * 1600);
        CHECK(path(&one, "moof/traf.0/trun", &b));
        CHECK(be32(b.p) == 0x000F01 && be32(b.p + 4) == 5); /* data offset, duration, size, flags, cts */
        uint32_t data_off = be32(b.p + 8);
        CHECK(be32(b.p + 12) == 1001 && be32(b.p + 16) == 100 && be32(b.p + 20) == 0x02000000 && be32(b.p + 24) == 0);
        CHECK(be32(b.p + 12 + 16 + 12) == 2002); /* second sample's cts */
        fill(s, 100, frag * 10);
        CHECK(!memcmp(one.p + moof.at + data_off, s, 100));
        CHECK(path(&one, "moof/traf.1/trun", &b));
        CHECK(be32(b.p) == 0x000701); /* no cts column for audio */
        uint32_t aoff = be32(b.p + 8);
        fill(s, 50, 100 + frag * 10);
        CHECK(!memcmp(one.p + moof.at + aoff, s, 50));
        CHECK(path(&one, "mdat", &mdat));
        pos += mdat.at + 8 + mdat.n;
    }
    CHECK_INT(pos, f.len);
    free(f.p);
}

TEST(mp4_helpers_avcc_annexb_dac3) {
    uint8_t out[128];
    static const uint8_t base_sps[] = {0x67, 0x42, 0xC0, 0x1E, 0x9A};
    size_t n = mp4_avcc(base_sps, sizeof base_sps, PPS, sizeof PPS, out, sizeof out);
    CHECK_INT(n, 6 + 2 + sizeof base_sps + 3 + sizeof PPS); /* Baseline: no extension */
    CHECK(out[1] == 0x42 && out[2] == 0xC0 && out[3] == 0x1E && out[7] == sizeof base_sps);
    CHECK_INT(mp4_avcc(base_sps, sizeof base_sps, PPS, sizeof PPS, out, 10), 0);
    /* Annex B with 4- and 3-byte start codes and trailing zero padding */
    static const uint8_t annexb[] = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 1, 0x65, 0x88, 0x84, 0, 0, 0, 0, 1, 0x41, 0x9A, 0};
    n = mp4_annexb_to_lengths(annexb, sizeof annexb, out, sizeof out);
    static const uint8_t want[] = {0, 0, 0, 2, 0x09, 0xF0, 0, 0, 0, 3, 0x65, 0x88, 0x84, 0, 0, 0, 2, 0x41, 0x9A};
    CHECK_INT(n, sizeof want);
    CHECK(!memcmp(out, want, sizeof want));
    CHECK_INT(mp4_annexb_to_lengths(annexb, sizeof annexb, out, 5), 0);
    /* dac3: 48 kHz, 384 kbit/s, bsid 8, 3/2 + LFE */
    uint8_t d[3];
    static const uint8_t ac3[8] = {0x0B, 0x77, 0, 0, 28, 8 << 3, 0xE1, 0};
    CHECK(mp4_dac3(ac3, sizeof ac3, d));
    CHECK(d[0] == 0x10 && d[1] == 0x3D && d[2] == 0xC0);
    /* 2/0 Dolby Surround: dsurmod sits between acmod and lfeon */
    static const uint8_t st[8] = {0x0B, 0x77, 0, 0, 16, 8 << 3, 0x54, 0};
    CHECK(mp4_dac3(st, sizeof st, d));
    uint32_t v = (uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2];
    CHECK((v >> 11 & 7) == 2 && (v >> 10 & 1) == 1 && (v >> 5 & 31) == 8);
    static const uint8_t bad[8] = {0x0B, 0x78};
    CHECK(!mp4_dac3(bad, sizeof bad, d));
}
