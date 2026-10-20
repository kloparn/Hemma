#include "../../tests/test.h"
#include "../../core/plat.h"
#include "mediainfo.h"

/* ------------------------------------------------------- byte writers */

static void p8(Buf *b, unsigned v) { buf_putc(b, (char)(v & 0xFF)); }
static void p16(Buf *b, unsigned v) {
    p8(b, v >> 8);
    p8(b, v);
}
static void p32(Buf *b, uint32_t v) {
    p16(b, v >> 16);
    p16(b, v & 0xFFFF);
}
static void p64(Buf *b, uint64_t v) {
    p32(b, (uint32_t)(v >> 32));
    p32(b, (uint32_t)v);
}
static void l16(Buf *b, unsigned v) {
    p8(b, v);
    p8(b, v >> 8);
}
static void l32(Buf *b, uint32_t v) {
    l16(b, v & 0xFFFF);
    l16(b, v >> 16);
}
static void pz(Buf *b, size_t n) {
    while (n--) p8(b, 0);
}
static void put_be32_at(Buf *b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b->p[at + i] = (char)(v >> (24 - 8 * i));
}

/* ------------------------------------------------------------ MP4 */

static size_t box(Buf *b, const char *type) {
    size_t at = b->len;
    p32(b, 0);
    buf_put(b, type, 4);
    return at;
}
static void end_box(Buf *b, size_t at) { put_be32_at(b, at, (uint32_t)(b->len - at)); }

static void tkhd0(Buf *b, uint32_t id, int w, int h) {
    size_t a = box(b, "tkhd");
    p32(b, 0x00000003); /* v0, enabled | in movie */
    p32(b, 0), p32(b, 0), p32(b, id), p32(b, 0), p32(b, 0);
    pz(b, 8);
    p16(b, 0), p16(b, 0), p16(b, 0), p16(b, 0);
    pz(b, 36);
    p32(b, (uint32_t)w << 16), p32(b, (uint32_t)h << 16);
    end_box(b, a);
}

static void visual_entry(Buf *b, const char *fourcc, int w, int h) {
    size_t a = box(b, fourcc);
    pz(b, 6), p16(b, 1), p16(b, 0), p16(b, 0), pz(b, 12);
    p16(b, (unsigned)w), p16(b, (unsigned)h);
    p32(b, 0x00480000), p32(b, 0x00480000), p32(b, 0), p16(b, 1);
    pz(b, 32), p16(b, 0x18), p16(b, 0xFFFF);
    size_t c = box(b, "avcC");
    pz(b, 8);
    end_box(b, c);
    end_box(b, a);
}

static void audio_entry(Buf *b, const char *fourcc, int channels, int oti) {
    size_t a = box(b, fourcc);
    pz(b, 6), p16(b, 1), pz(b, 8);
    p16(b, (unsigned)channels), p16(b, 16), p16(b, 0), p16(b, 0), p32(b, 48000u << 16);
    if (oti >= 0) {
        size_t e = box(b, "esds");
        p32(b, 0);
        p8(b, 0x03), p8(b, 0x80), p8(b, 0x80), p8(b, 0x80), p8(b, 25); /* ES_Descriptor, 4-byte length */
        p16(b, 1), p8(b, 0);
        p8(b, 0x04), p8(b, 17); /* DecoderConfigDescriptor */
        p8(b, (unsigned)oti), p8(b, 0x15), pz(b, 3), p32(b, 128000), p32(b, 128000);
        p8(b, 0x05), p8(b, 2), p8(b, 0x11), p8(b, 0x90);
        p8(b, 0x06), p8(b, 1), p8(b, 2);
        end_box(b, e);
    }
    end_box(b, a);
}

static void trak(Buf *b, uint32_t id, bool video, const char *fourcc, int w, int h, int ch, int oti) {
    size_t t = box(b, "trak");
    tkhd0(b, id, w, h);
    size_t md = box(b, "mdia");
    size_t mh = box(b, "mdhd");
    p32(b, 0), p32(b, 0), p32(b, 0), p32(b, video ? 90000 : 48000), p32(b, (video ? 90000u : 48000u) * 90), p16(b, 0x55C4),
        p16(b, 0);
    end_box(b, mh);
    size_t hd = box(b, "hdlr");
    p32(b, 0), p32(b, 0), buf_put(b, video ? "vide" : "soun", 4), pz(b, 12), buf_put(b, "h", 2);
    end_box(b, hd);
    size_t mi = box(b, "minf"), st = box(b, "stbl"), sd = box(b, "stsd");
    p32(b, 0), p32(b, 1);
    if (video) visual_entry(b, fourcc, w, h);
    else audio_entry(b, fourcc, ch, oti);
    end_box(b, sd), end_box(b, st), end_box(b, mi), end_box(b, md), end_box(b, t);
}

typedef struct {
    bool moov_last, v1, largesize, qt, mehd;
    const char *vfourcc, *afourcc;
    int oti;
} Mp4Opts;

static void moov(Buf *b, const Mp4Opts *o) {
    size_t m = box(b, "moov");
    size_t mv = box(b, "mvhd");
    if (o->v1) {
        p32(b, 0x01000000), p64(b, 0), p64(b, 0), p32(b, 600), p64(b, 600ull * 5400);
    } else {
        p32(b, 0), p32(b, 0), p32(b, 0), p32(b, 1000), p32(b, o->mehd ? 0 : 90500);
    }
    p32(b, 0x00010000), p16(b, 0x0100), pz(b, 10), pz(b, 36), pz(b, 24), p32(b, 3);
    end_box(b, mv);
    if (o->mehd) {
        size_t x = box(b, "mvex"), h = box(b, "mehd");
        p32(b, 0), p32(b, 42000);
        end_box(b, h), end_box(b, x);
    }
    trak(b, 1, true, o->vfourcc ? o->vfourcc : "avc1", 1920, 1080, 0, -1);
    trak(b, 2, false, o->afourcc ? o->afourcc : "mp4a", 0, 0, 2, o->oti);
    end_box(b, m);
}

static void make_mp4(Buf *b, const Mp4Opts *o) {
    size_t f = box(b, "ftyp");
    buf_put(b, o->qt ? "qt  " : "isom", 4), p32(b, 0x200), buf_put(b, "isomavc1", 8);
    end_box(b, f);
    if (!o->moov_last) moov(b, o);
    if (o->largesize) {
        p32(b, 1), buf_put(b, "mdat", 4), p64(b, 16 + 3000), pz(b, 3000);
    } else {
        size_t a = box(b, "mdat");
        pz(b, 3000);
        end_box(b, a);
    }
    if (o->moov_last) moov(b, o);
}

static MediaProbe probe_buf(const Buf *b, const char *name) {
    MediaProbe p;
    media_probe_mem(b->p, b->len, name, &p);
    return p;
}

TEST(probe_mp4_faststart_h264_aac) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.oti = 0x40});
    MediaProbe p;
    CHECK(media_probe_mem(b.p, b.len, "movie.mp4", &p));
    CHECK_STR(p.container, "mp4");
    CHECK_INT(p.duration_ms, 90500);
    CHECK_STR(p.vcodec, "h264");
    CHECK_STR(p.acodec, "aac");
    CHECK_INT(p.width, 1920);
    CHECK_INT(p.height, 1080);
    CHECK_INT(p.audio_channels, 2);
    CHECK_INT(p.playable, PLAY_YES);
    CHECK_STR(p.why, "");
    buf_free(&b);
}

TEST(probe_mp4_moov_at_end_and_largesize_mdat) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.moov_last = true, .oti = 0x40});
    MediaProbe p = probe_buf(&b, "a.m4v");
    CHECK_INT(p.duration_ms, 90500);
    CHECK_STR(p.vcodec, "h264");
    buf_free(&b);
    Buf c = {0};
    make_mp4(&c, &(Mp4Opts){.moov_last = true, .largesize = true, .oti = 0x40});
    p = probe_buf(&c, "a.mp4");
    CHECK_INT(p.duration_ms, 90500);
    CHECK_STR(p.acodec, "aac");
    buf_free(&c);
}

TEST(probe_mp4_v1_header_and_mov_brand) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.v1 = true, .qt = true, .oti = 0x40});
    MediaProbe p = probe_buf(&b, "clip.mov");
    CHECK_STR(p.container, "mov");
    CHECK_INT(p.duration_ms, 5400000);
    CHECK_INT(p.playable, PLAY_YES);
    buf_free(&b);
}

TEST(probe_mp4_fragmented_uses_mehd) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.mehd = true, .oti = 0x40});
    MediaProbe p = probe_buf(&b, "frag.mp4");
    CHECK_INT(p.duration_ms, 42000);
    buf_free(&b);
}

TEST(probe_mp4_hevc_and_dolby_is_maybe) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.vfourcc = "hev1", .afourcc = "ac-3", .oti = -1});
    MediaProbe p = probe_buf(&b, "x.mp4");
    CHECK_STR(p.vcodec, "hevc");
    CHECK_STR(p.acodec, "ac3");
    CHECK_INT(p.playable, PLAY_MAYBE);
    CHECK_HAS(p.why, "silent in Chrome"); /* sound that plays silently: the bigger surprise of the two */
    buf_free(&b);
    /* a probe from the library's cache gets today's verdict and words from its codecs */
    MediaProbe old = {.playable = PLAY_MAYBE};
    str_copy(old.container, sizeof old.container, "mkv");
    str_copy(old.vcodec, sizeof old.vcodec, "hevc");
    str_copy(old.acodec, sizeof old.acodec, "eac3");
    str_copy(old.why, sizeof old.why, "Dolby audio: Safari/Edge only");
    media_probe_judge(&old);
    CHECK_INT(old.playable, PLAY_MAYBE);
    CHECK_STR(old.why, "Dolby sound: Safari/Edge only - silent in Chrome and Firefox");
    str_copy(old.acodec, sizeof old.acodec, "aac");
    media_probe_judge(&old);
    CHECK_HAS(old.why, "HEVC");
}

TEST(probe_mp4_esds_object_types) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.oti = 0x6B});
    MediaProbe p = probe_buf(&b, "x.mp4");
    CHECK_STR(p.acodec, "mp3");
    CHECK_INT(p.playable, PLAY_YES);
    buf_free(&b);
    Buf c = {0};
    make_mp4(&c, &(Mp4Opts){.oti = 0xA9});
    p = probe_buf(&c, "x.mp4");
    CHECK_STR(p.acodec, "dts");
    CHECK_INT(p.playable, PLAY_NO);
    CHECK_HAS(p.why, "DTS");
    buf_free(&c);
    Buf d = {0};
    make_mp4(&d, &(Mp4Opts){.vfourcc = "mp4v", .oti = 0x40});
    p = probe_buf(&d, "x.mp4");
    CHECK_STR(p.vcodec, "mpeg4");
    CHECK_INT(p.playable, PLAY_NO);
    CHECK_HAS(p.why, "conversion");
    buf_free(&d);
}

TEST(probe_mp4_truncated_download_is_unknown) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.moov_last = true, .oti = 0x40});
    MediaProbe p;
    CHECK(!media_probe_mem(b.p, 1200, "partial.mp4", &p)); /* cut inside mdat: no moov yet */
    CHECK_STR(p.container, "mp4");
    CHECK_INT(p.playable, PLAY_UNKNOWN);
    CHECK_HAS(p.why, "header");
    buf_free(&b);
}

/* ------------------------------------------------------------ Matroska */

static void eid(Buf *b, uint32_t v) {
    if (v > 0xFFFFFF) p8(b, v >> 24);
    if (v > 0xFFFF) p8(b, v >> 16);
    if (v > 0xFF) p8(b, v >> 8);
    p8(b, v);
}
static size_t el(Buf *b, uint32_t id) { /* 8-byte size placeholder */
    eid(b, id);
    size_t at = b->len;
    pz(b, 8);
    return at;
}
static void el_end(Buf *b, size_t at) {
    uint64_t n = b->len - at - 8;
    b->p[at] = 0x01;
    for (int i = 1; i < 8; i++) b->p[at + i] = (char)(n >> (8 * (7 - i)));
}
static void el_uint(Buf *b, uint32_t id, uint64_t x) {
    eid(b, id);
    int n = 1;
    while (n < 8 && (x >> (8 * n))) n++;
    p8(b, 0x80u | (unsigned)n);
    for (int i = n - 1; i >= 0; i--) p8(b, (unsigned)(x >> (8 * i)));
}
static void el_str(Buf *b, uint32_t id, const char *s) {
    eid(b, id);
    size_t n = strlen(s);
    p8(b, 0x80u | (unsigned)n);
    buf_put(b, s, n);
}
static void el_f64(Buf *b, uint32_t id, double d) {
    eid(b, id);
    p8(b, 0x88);
    uint64_t u;
    memcpy(&u, &d, 8);
    p64(b, u);
}
static void el_f32(Buf *b, uint32_t id, float f) {
    eid(b, id);
    p8(b, 0x84);
    uint32_t u;
    memcpy(&u, &f, 4);
    p32(b, u);
}

typedef struct {
    const char *doctype, *vcodec, *acodec, *acodec2;
    uint64_t timescale;
    double dur;
    bool f32, unknown_seg, after_cluster, a1_not_default;
} MkvOpts;

static void mkv_info(Buf *b, const MkvOpts *o) {
    size_t i = el(b, 0x1549A966);
    if (o->timescale) el_uint(b, 0x2AD7B1, o->timescale);
    if (o->f32) el_f32(b, 0x4489, (float)o->dur);
    else el_f64(b, 0x4489, o->dur);
    el_str(b, 0x4D80, "hemma-test"); /* MuxingApp: an element we ignore */
    el_end(b, i);
}

static void mkv_audio(Buf *b, int num, const char *codec, bool not_default, int channels) {
    size_t t = el(b, 0xAE);
    el_uint(b, 0xD7, (uint64_t)num);
    el_uint(b, 0x83, 2);
    el_str(b, 0x86, codec);
    if (not_default) el_uint(b, 0x88, 0);
    size_t a = el(b, 0xE1);
    el_uint(b, 0x9F, (uint64_t)channels);
    el_end(b, a);
    el_end(b, t);
}

static void mkv_tracks(Buf *b, const MkvOpts *o) {
    size_t ts = el(b, 0x1654AE6B);
    size_t t = el(b, 0xAE);
    el_uint(b, 0xD7, 1);
    el_uint(b, 0x83, 1);
    el_str(b, 0x86, o->vcodec);
    size_t v = el(b, 0xE0);
    el_uint(b, 0xB0, 1280);
    el_uint(b, 0xBA, 720);
    el_end(b, v);
    el_end(b, t);
    mkv_audio(b, 2, o->acodec, o->a1_not_default, 6);
    if (o->acodec2) mkv_audio(b, 3, o->acodec2, false, 2);
    el_end(b, ts);
}

static void mkv_cluster(Buf *b) {
    size_t c = el(b, 0x1F43B675);
    el_uint(b, 0xE7, 0);
    eid(b, 0xA3);
    p8(b, 0x80 | 40);
    pz(b, 40);
    el_end(b, c);
}

static void make_mkv(Buf *b, const MkvOpts *o) {
    size_t h = el(b, 0x1A45DFA3);
    el_uint(b, 0x4286, 1);
    el_str(b, 0x4282, o->doctype ? o->doctype : "matroska");
    el_uint(b, 0x4287, 4);
    el_end(b, h);
    size_t seg = el(b, 0x18538067);
    size_t data = b->len;
    if (o->after_cluster) {
        size_t sh = el(b, 0x114D9B74), pos[2];
        uint32_t ids[2] = {0x1549A966, 0x1654AE6B};
        for (int i = 0; i < 2; i++) {
            size_t s = el(b, 0x4DBB);
            eid(b, 0x53AB), p8(b, 0x84), p32(b, ids[i]);
            eid(b, 0x53AC), p8(b, 0x88);
            pos[i] = b->len;
            pz(b, 8);
            el_end(b, s);
        }
        el_end(b, sh);
        size_t v = el(b, 0xEC); /* Void */
        pz(b, 16);
        el_end(b, v);
        mkv_cluster(b);
        uint64_t at = b->len - data;
        for (int i = 0; i < 8; i++) b->p[pos[0] + i] = (char)(at >> (56 - 8 * i));
        mkv_info(b, o);
        at = b->len - data;
        for (int i = 0; i < 8; i++) b->p[pos[1] + i] = (char)(at >> (56 - 8 * i));
        mkv_tracks(b, o);
    } else {
        mkv_info(b, o);
        mkv_tracks(b, o);
        mkv_cluster(b);
        mkv_cluster(b);
    }
    el_end(b, seg);
    if (o->unknown_seg) {
        b->p[seg] = 0x01;
        memset(b->p + seg + 1, 0xFF, 7);
    }
}

TEST(probe_mkv_h264_dts_needs_conversion) {
    Buf b = {0};
    make_mkv(&b, &(MkvOpts){.vcodec = "V_MPEG4/ISO/AVC", .acodec = "A_DTS", .dur = 1234567.0});
    MediaProbe p;
    CHECK(media_probe_mem(b.p, b.len, "film.mkv", &p));
    CHECK_STR(p.container, "mkv");
    CHECK_INT(p.duration_ms, 1234567);
    CHECK_STR(p.vcodec, "h264");
    CHECK_STR(p.acodec, "dts");
    CHECK_INT(p.audio_channels, 6);
    CHECK_INT(p.width, 1280);
    CHECK_INT(p.height, 720);
    CHECK_INT(p.playable, PLAY_NO);
    CHECK_HAS(p.why, "DTS");
    buf_free(&b);
}

TEST(probe_mkv_custom_timescale_float32) {
    Buf b = {0};
    make_mkv(&b, &(MkvOpts){.vcodec = "V_MPEG4/ISO/AVC", .acodec = "A_AAC", .timescale = 100000, .dur = 50000.0,
                             .f32 = true});
    MediaProbe p = probe_buf(&b, "x.mkv");
    CHECK_INT(p.duration_ms, 5000);
    CHECK_INT(p.playable, PLAY_MAYBE);
    CHECK_HAS(p.why, "MKV");
    buf_free(&b);
}

TEST(probe_mkv_unknown_size_segment) {
    Buf b = {0};
    make_mkv(&b, &(MkvOpts){.vcodec = "V_MPEGH/ISO/HEVC", .acodec = "A_EAC3", .dur = 60000.0, .unknown_seg = true});
    MediaProbe p = probe_buf(&b, "x.mkv");
    CHECK_INT(p.duration_ms, 60000);
    CHECK_STR(p.vcodec, "hevc");
    CHECK_STR(p.acodec, "eac3");
    CHECK_INT(p.playable, PLAY_MAYBE);
    buf_free(&b);
}

TEST(probe_mkv_seekhead_past_cluster) {
    Buf b = {0};
    make_mkv(&b, &(MkvOpts){.vcodec = "V_MPEG4/ISO/AVC", .acodec = "A_OPUS", .dur = 2500.0, .after_cluster = true});
    MediaProbe p = probe_buf(&b, "x.mkv");
    CHECK_INT(p.duration_ms, 2500);
    CHECK_STR(p.vcodec, "h264");
    CHECK_STR(p.acodec, "opus");
    buf_free(&b);
}

TEST(probe_webm_vp9_opus_plays) {
    Buf b = {0};
    make_mkv(&b, &(MkvOpts){.doctype = "webm", .vcodec = "V_VP9", .acodec = "A_OPUS", .dur = 10000.0});
    MediaProbe p = probe_buf(&b, "clip.webm");
    CHECK_STR(p.container, "webm");
    CHECK_STR(p.vcodec, "vp9");
    CHECK_INT(p.playable, PLAY_YES);
    buf_free(&b);
}

TEST(probe_mkv_prefers_default_audio_track) {
    Buf b = {0};
    make_mkv(&b, &(MkvOpts){.vcodec = "V_MPEG4/ISO/AVC", .acodec = "A_DTS", .a1_not_default = true, .acodec2 = "A_AAC",
                             .dur = 1000.0});
    MediaProbe p = probe_buf(&b, "x.mkv");
    CHECK_STR(p.acodec, "aac");
    CHECK_INT(p.audio_channels, 2);
    buf_free(&b);
}

/* ----------------------------------------------------------------- AVI */

static size_t chunk(Buf *b, const char *id) {
    buf_put(b, id, 4);
    size_t at = b->len;
    pz(b, 4);
    return at;
}
static void chunk_end(Buf *b, size_t at) {
    uint32_t n = (uint32_t)(b->len - at - 4);
    for (int i = 0; i < 4; i++) b->p[at + i] = (char)(n >> (8 * i));
    if (n & 1) p8(b, 0);
}

static void make_avi(Buf *b) {
    size_t riff = chunk(b, "RIFF");
    buf_put(b, "AVI ", 4);
    size_t hdrl = chunk(b, "LIST");
    buf_put(b, "hdrl", 4);
    size_t avih = chunk(b, "avih");
    l32(b, 40000), l32(b, 0), l32(b, 0), l32(b, 0x10), l32(b, 2500), l32(b, 0), l32(b, 2), l32(b, 0), l32(b, 640),
        l32(b, 480), pz(b, 16);
    chunk_end(b, avih);
    size_t s1 = chunk(b, "LIST");
    buf_put(b, "strl", 4);
    size_t strh = chunk(b, "strh");
    buf_put(b, "vids", 4), buf_put(b, "XVID", 4), l32(b, 0), l16(b, 0), l16(b, 0), l32(b, 0), l32(b, 1), l32(b, 25),
        l32(b, 0), l32(b, 2500), pz(b, 20);
    chunk_end(b, strh);
    size_t strf = chunk(b, "strf");
    l32(b, 40), l32(b, 640), l32(b, 480), l16(b, 1), l16(b, 24), buf_put(b, "XVID", 4), pz(b, 20);
    chunk_end(b, strf);
    chunk_end(b, s1);
    size_t s2 = chunk(b, "LIST");
    buf_put(b, "strl", 4);
    strh = chunk(b, "strh");
    buf_put(b, "auds", 4), pz(b, 4), pz(b, 48);
    chunk_end(b, strh);
    strf = chunk(b, "strf");
    l16(b, 0x55), l16(b, 2), l32(b, 44100), l32(b, 16000), l16(b, 1), l16(b, 0);
    chunk_end(b, strf);
    chunk_end(b, s2);
    chunk_end(b, hdrl);
    size_t movi = chunk(b, "LIST");
    buf_put(b, "movi", 4);
    pz(b, 500);
    chunk_end(b, movi);
    chunk_end(b, riff);
}

TEST(probe_avi_xvid_mp3) {
    Buf b = {0};
    make_avi(&b);
    MediaProbe p;
    CHECK(media_probe_mem(b.p, b.len, "old.avi", &p));
    CHECK_STR(p.container, "avi");
    CHECK_INT(p.duration_ms, 100000);
    CHECK_STR(p.vcodec, "mpeg4");
    CHECK_STR(p.acodec, "mp3");
    CHECK_INT(p.audio_channels, 2);
    CHECK_INT(p.width, 640);
    CHECK_INT(p.height, 480);
    CHECK_INT(p.playable, PLAY_NO);
    CHECK_HAS(p.why, "conversion");
    buf_free(&b);
}

/* ---------------------------------------------------- recognition only */

TEST(probe_recognises_ts_wmv_and_garbage) {
    uint8_t ts[600] = {0};
    ts[0] = ts[188] = ts[376] = 0x47;
    MediaProbe p;
    CHECK(media_probe_mem(ts, sizeof ts, "rec.ts", &p));
    CHECK_STR(p.container, "ts");
    CHECK_INT(p.playable, PLAY_NO);
    uint8_t asf[64] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
    CHECK(media_probe_mem(asf, sizeof asf, "x.wmv", &p));
    CHECK_STR(p.container, "wmv");
    CHECK(!media_probe_mem("hello world, not a video", 24, "notes.txt", &p));
    CHECK_STR(p.container, "");
    CHECK_INT(p.playable, PLAY_UNKNOWN);
    CHECK(!media_probe_mem(NULL, 0, "empty.mkv", &p));
    CHECK_STR(p.container, "mkv");
    CHECK_INT(p.playable, PLAY_UNKNOWN);
}

/* --------------------------------------------------------- robustness */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

TEST(probe_survives_random_bytes) {
    static uint8_t buf[4096];
    static const char *const names[] = {"a.mp4", "a.mkv", "a.avi", "a.ts", "a", NULL};
    for (int iter = 0; iter < 4000; iter++) {
        size_t n = (size_t)(rnd() % sizeof buf);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd();
        if (n >= 12) switch (iter % 4) { /* plant a magic number to get past the first gate */
            case 0: memcpy(buf + 4, "ftyp", 4); break;
            case 1: memcpy(buf, "\x1A\x45\xDF\xA3", 4); break;
            case 2: memcpy(buf, "RIFF", 4), memcpy(buf + 8, "AVI ", 4); break;
            default: break;
            }
        MediaProbe p;
        media_probe_mem(buf, n, names[iter % 5], &p);
        CHECK(p.playable >= PLAY_UNKNOWN && p.playable <= PLAY_NO);
        CHECK(memchr(p.vcodec, 0, sizeof p.vcodec) && memchr(p.why, 0, sizeof p.why));
    }
}

static void torture(const Buf *b, const char *name) {
    MediaProbe p;
    for (size_t n = 0; n <= b->len; n += (b->len > 3000 ? 3 : 1)) media_probe_mem(b->p, n, name, &p); /* truncations */
    uint8_t *m = xmalloc(b->len);
    for (int iter = 0; iter < 1500; iter++) { /* mutations */
        memcpy(m, b->p, b->len);
        int flips = 1 + (int)(rnd() % 8);
        for (int k = 0; k < flips; k++) m[rnd() % b->len] = (uint8_t)rnd();
        media_probe_mem(m, b->len, name, &p);
    }
    free(m);
}

TEST(probe_survives_truncation_and_mutation) {
    Buf a = {0}, b = {0}, c = {0}, d = {0};
    make_mp4(&a, &(Mp4Opts){.moov_last = true, .largesize = true, .oti = 0x40});
    make_mkv(&b, &(MkvOpts){.vcodec = "V_MPEG4/ISO/AVC", .acodec = "A_AAC", .dur = 1.0, .after_cluster = true});
    make_avi(&c);
    make_mkv(&d, &(MkvOpts){.vcodec = "V_MS/VFW/FOURCC", .acodec = "A_MS/ACM", .dur = 1.0, .unknown_seg = true});
    torture(&a, "a.mp4");
    torture(&b, "b.mkv");
    torture(&c, "c.avi");
    torture(&d, "d.mkv");
    MediaProbe p = probe_buf(&b, "b.mkv"); /* the originals still parse afterwards */
    CHECK_STR(p.acodec, "aac");
    buf_free(&a), buf_free(&b), buf_free(&c), buf_free(&d);
}

/* ---------------------------------------------------------- real files */

TEST(probe_file_roundtrip) {
    Buf b = {0};
    make_mp4(&b, &(Mp4Opts){.moov_last = true, .oti = 0x40});
    char path[600];
    snprintf(path, sizeof path, "%s/Filmer/Räksmörgås (2020).mp4", test_tmpdir());
    char dir[600];
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = 0;
    CHECK(dir_make(dir));
    CHECK(file_write_atomic(path, b.p, b.len));
    MediaProbe p;
    CHECK(media_probe_file(path, &p));
    CHECK_INT(p.duration_ms, 90500);
    CHECK_STR(p.vcodec, "h264");
    CHECK(!media_probe_file("no/such/file.mkv", &p));
    CHECK_INT(p.playable, PLAY_UNKNOWN);
    buf_free(&b);
}

TEST(probe_mdn_samples_if_present) {
    /* Optional fixtures (CC0, not committed): build/fixtures/flower.mp4 and flower.webm */
    MediaProbe p;
    if (path_is_dir("build/fixtures")) {
        if (media_probe_file("build/fixtures/flower.mp4", &p)) {
            CHECK_STR(p.vcodec, "h264");
            CHECK(p.duration_ms > 1000);
            CHECK(p.width > 0);
        }
        if (media_probe_file("build/fixtures/flower.webm", &p)) {
            CHECK_STR(p.container, "webm");
            CHECK(p.duration_ms > 1000);
        }
    }
}

/* ------------------------------------------------------------- helpers */

TEST(media_ext_mime_and_labels) {
    CHECK(media_is_video_ext("Movie.MKV"));
    CHECK(media_is_video_ext("dir.with.dots/clip.m2ts"));
    CHECK(!media_is_video_ext("movie.srt"));
    CHECK(!media_is_video_ext("mkv"));
    CHECK(media_is_sub_ext("a.SRT") && media_is_sub_ext("a.vtt") && !media_is_sub_ext("a.mkv"));
    CHECK(media_is_image_ext("poster.JPG") && media_is_image_ext("folder.webp") && !media_is_image_ext("a.gif"));
    CHECK_STR(media_mime("mp4"), "video/mp4");
    CHECK_STR(media_mime("webm"), "video/webm");
    CHECK_STR(media_mime("mkv"), "video/x-matroska");
    CHECK_STR(media_mime("nope"), "application/octet-stream");
    CHECK_STR(media_playable_label(PLAY_NO), "Needs conversion");
    char d[32];
    media_fmt_duration(5400000, d, sizeof d);
    CHECK_STR(d, "1 h 30 min");
    media_fmt_duration(42 * 60000 + 10000, d, sizeof d);
    CHECK_STR(d, "42 min");
    media_fmt_duration(45000, d, sizeof d);
    CHECK_STR(d, "45 s");
    media_fmt_duration(0, d, sizeof d);
    CHECK_STR(d, "");
}
