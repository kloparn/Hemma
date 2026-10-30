/* modules/av/mp4read_test.c — 2.5: an MP4/MOV read behind mkv.h (mkv_mp4.h) and converted (the owner's DuckTales: MP4s
 * of H.264 + AC-3, silent in Chrome and Firefox). The test files are written here by mp4.c's own writer - with
 * B-frames and an edit list, the moov last, co64, a 'text' track, tx3g subtitles, a fragmented one - and read back the
 * same way, so what goes in and what comes out are compared sample by sample. */
#include "../../tests/test.h"
#include "../../core/plat.h"

#include "aac.h"
#include "ac3_testenc.h"
#include "conv.h"
#include "h264enc.h"
#include "mkv.h"
#include "mp4.h"

typedef struct {
    uint8_t *p;
    size_t len, cap;
} Mem;

static bool m_write(void *ctx, const void *p, size_t n) {
    Mem *m = ctx;
    if (m->len + n > m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 65536;
        while (cap < m->len + n) cap *= 2;
        uint8_t *q = realloc(m->p, cap);
        if (!q) return false;
        m->p = q, m->cap = cap;
    }
    memcpy(m->p + m->len, p, n);
    m->len += n;
    return true;
}

static bool m_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    Mem *m = ctx;
    if (off < 0 || (size_t)off + n > m->len) return false;
    memcpy(m->p + off, p, n);
    return true;
}

static bool m_read(void *ctx, int64_t off, void *dst, size_t n) {
    const Mem *m = ctx;
    if (off < 0 || (size_t)off > m->len || n > m->len - (size_t)off) return false;
    memcpy(dst, m->p + off, n);
    return true;
}

enum { SND_NONE, SND_AC3, SND_AAC, SND_MP3, SND_EAC3 };

typedef struct {
    int sound;
    bool moov_last, co64, text_track, subs, fragmented, hev1;
    int frames; /* pictures (25 a second), with B-frames: decode I P B B P B B ... */
    int vts, vdelta; /* the pictures' timescale and length in it (0: 90000 and 3600 - 25 a second) */
    int snd_frames;  /* Dolby frames (0: as long as the pictures) */
} Make;

/* What the file holds: every picture's and the sound's presentation times (ns), as mkv.h reads them */
typedef struct {
    int64_t vpts[800];
    int nv, na;
    int64_t a_first, a_end;
    char vcodec[64], acodec[64]; /* (MkvTrack.codec's size) */
} Seen;

/* "soun" -> what, the k-th one (0 = the first): a trak made with mp4.c becomes another kind for the reader */
static void patch(Mem *m, const char *from, const char *to, int k) {
    for (size_t at = 0; at + 4 <= m->len; at++)
        if (!memcmp(m->p + at, from, 4) && k-- == 0) {
            memcpy(m->p + at, to, 4);
            return;
        }
}

static bool make(Mem *out, const Make *o) {
    memset(out, 0, sizeof *out);
    H264Params hp = {.width = 64, .height = 48, .fps_num = 25, .fps_den = 1, .keyint = 10, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    uint8_t avcc[256];
    size_t avcc_len = h264enc_avcc(e, avcc, sizeof avcc);
    h264enc_free(e);
    Mp4Out w = {out, m_write, m_write_at};
    Mp4 *m = mp4_new(&w, o->fragmented ? MP4_FRAGMENTED : 0, o->moov_last || o->fragmented ? 0 : 1 << 20);
    if (o->co64) mp4_set_co64_threshold(m, 0);
    uint32_t vts = o->vts ? (uint32_t)o->vts : 90000, vd = o->vdelta ? (uint32_t)o->vdelta : 3600;
    Mp4Track vt = {.codec = o->hev1 ? MP4_HEVC : MP4_H264, .timescale = vts, .config = avcc, .config_len = avcc_len,
                   .width = 64, .height = 48, .start = 2 * vd}; /* (B-frames: two pictures' worth of reordering) */
    int v = mp4_add_track(m, &vt), a = -1, t = -1, s = -1;
    static uint8_t snd[64 * 768];
    size_t nsnd = 0, fsize = 0;
    int nframes = 0;
    if (o->sound == SND_AC3 || o->sound == SND_EAC3) {
        bool plus = o->sound == SND_EAC3;
        int want = o->snd_frames ? o->snd_frames : o->frames * 5 / (plus ? 2 : 4); /* (as long as the pictures: 32 ms
                                                                                     Dolby frames, 16 ms DD+) */
        nsnd = plus ? eac3_test_tone(snd, sizeof snd, want > 128 ? 128 : want) : ac3_test_tone(snd, sizeof snd, want > 64 ? 64 : want);
        fsize = plus ? 384 : 768;
        nframes = (int)(nsnd / fsize);
        uint8_t dac3[3];
        mp4_dac3(snd, 768, dac3); /* (E-AC-3: the 'ec-3'/'dec3' patched in after - the reader goes by the frames) */
        Mp4Track at = {.codec = MP4_AC3, .timescale = 48000, .config = dac3, .config_len = 3, .channels = 2, .sample_rate = 48000};
        memcpy(at.lang, "eng", 4);
        a = mp4_add_track(m, &at);
    } else if (o->sound == SND_AAC || o->sound == SND_MP3) {
        static const uint8_t asc[2] = {0x11, 0x90};
        Mp4Track at = {.codec = o->sound == SND_AAC ? MP4_AAC : MP4_MP3, .timescale = 48000, .config = asc,
                       .config_len = o->sound == SND_AAC ? 2 : 0, .channels = 2, .sample_rate = 48000};
        a = mp4_add_track(m, &at);
        nframes = 50, fsize = 96;
        memset(snd, 0x21, sizeof snd);
    }
    if (o->text_track) { /* QuickTime chapters: a 'text' handler - becomes one after the writer (patched) */
        static const uint8_t asc[2] = {0x11, 0x90};
        Mp4Track tt = {.codec = MP4_AAC, .timescale = 1000, .config = asc, .config_len = 2, .channels = 2, .sample_rate = 48000};
        t = mp4_add_track(m, &tt);
    }
    if (o->subs) { /* tx3g subtitles: 'sbtl' + 'tx3g' (patched) */
        static const uint8_t asc[2] = {0x11, 0x90};
        Mp4Track st = {.codec = MP4_AAC, .timescale = 1000, .config = asc, .config_len = 2, .channels = 2, .sample_rate = 48000};
        memcpy(st.lang, "swe", 4);
        s = mp4_add_track(m, &st);
    }
    bool ok = v >= 0 && mp4_begin(m);
    static const int ORDER[] = {0, 3, 1, 2, 6, 4, 5, 9, 7, 8}; /* display index of the k-th decoded picture in a group */
    uint8_t pic[40];
    int ai = 0;
    for (int k = 0; ok && k < o->frames; k++) {
        int g = k / 10 * 10, shown = g + ORDER[k % 10];
        int32_t cts = (int32_t)((shown - k) * (int32_t)vd + 2 * (int32_t)vd); /* pts - dts, shifted to stay >= 0 */
        memset(pic, k & 0xFF, sizeof pic);
        pic[0] = 0, pic[1] = 0, pic[2] = 0, pic[3] = (uint8_t)(sizeof pic - 4);
        ok = mp4_sample(m, v, pic, sizeof pic, vd, cts, k % 10 == 0);
        /* the sound in step: a frame of it (32 ms Dolby, 21 ms AAC) for every picture's 40 ms, roughly */
        while (ok && a >= 0 && ai < nframes && (int64_t)ai * (o->sound == SND_AC3 ? 1536 : o->sound == SND_EAC3 ? 768 : 1024) * 25 <
                                                  (int64_t)(k + 1) * 48000) {
            uint32_t dur = o->sound == SND_AC3 ? 1536 : o->sound == SND_EAC3 ? 768 : o->sound == SND_MP3 ? 1152 : 1024;
            ok = mp4_sample(m, a, snd + (o->sound == SND_AC3 || o->sound == SND_EAC3 ? (size_t)ai * fsize : 0), fsize, dur, 0, true);
            ai++;
        }
        if (ok && t >= 0 && k % 25 == 0) ok = mp4_sample(m, t, "\0\4Ch 1", 6, 1000, 0, true);
        if (ok && s >= 0 && k == 0) ok = mp4_sample(m, s, "\0\5Hello" "\0\0\0\x0c" "styl\0\0\0\0", 19, 2000, 0, true);
        if (ok && s >= 0 && k == 0) ok = mp4_sample(m, s, "\0\0", 2, 1000, 0, true); /* (a gap: empty) */
        if (ok && s >= 0 && k == 0) ok = mp4_sample(m, s, "\0\4Hej!", 6, 1500, 0, true);
        if (o->fragmented && ok && k % 10 == 9) ok = mp4_fragment(m);
    }
    ok = ok && mp4_finish(m);
    mp4_free(m);
    if (o->sound == SND_EAC3) patch(out, "ac-3", "ec-3", 0), patch(out, "dac3", "dec3", 0);
    /* the traks after the sound's: each patched one is no longer a "soun"/"mp4a", so the next is at the same index */
    int souns = o->sound != SND_NONE, mp4as = o->sound == SND_AAC || o->sound == SND_MP3;
    if (t >= 0) patch(out, "soun", "text", souns), patch(out, "mp4a", "text", mp4as);
    if (s >= 0) patch(out, "soun", "sbtl", souns), patch(out, "mp4a", "tx3g", mp4as);
    return ok;
}

static bool look(const Mem *f, Seen *s, char *err, size_t cap) {
    memset(s, 0, sizeof *s);
    MkvReader r = {(void *)f, (int64_t)f->len, m_read};
    Mkv *m = mkv_open(&r, err, cap);
    if (!m) return false;
    const MkvTrack *v = mkv_find_track(m, MKV_VIDEO), *a = mkv_find_track(m, MKV_AUDIO);
    if (v) snprintf(s->vcodec, sizeof s->vcodec, "%s", v->codec);
    if (a) snprintf(s->acodec, sizeof s->acodec, "%s", a->codec);
    s->a_first = INT64_MAX;
    MkvFrame fr;
    while (mkv_next(m, &fr) > 0) {
        if (v && fr.track == v->number && s->nv < (int)countof(s->vpts)) s->vpts[s->nv++] = fr.pts;
        if (a && fr.track == a->number) {
            s->na++;
            if (fr.pts < s->a_first) s->a_first = fr.pts;
            if (fr.pts + fr.duration > s->a_end) s->a_end = fr.pts + fr.duration;
        }
    }
    mkv_close(m);
    return true;
}

static int cmp64(const void *x, const void *y) {
    int64_t a = *(const int64_t *)x, b = *(const int64_t *)y;
    return a < b ? -1 : a > b;
}

static bool convert(const Mem *in, Mem *out, AvConvJob *j) {
    memset(out, 0, sizeof *out);
    memset(j, 0, sizeof *j);
    j->in = (MkvReader){(void *)in, (int64_t)in->len, m_read};
    j->out = (Mp4Out){out, m_write, m_write_at};
    j->want_subs = true;
    return av_convert(j);
}

/* The owner's DuckTales: H.264 + AC-3 in an MP4 -> H.264 copied (every picture at its time: B-frames' composition
   offsets and the edit list read and written again) + AAC; the moov first or last, co64, a chapter track left out */
TEST(mp4read_converts_h264_and_dolby_from_an_mp4) {
    for (int shape = 0; shape < 3; shape++) {
        int n = shape ? 10 : 30; /* (the Dolby decoded and AAC made: slow in a debug build - one long, two short) */
        Make o = {.sound = SND_AC3, .frames = n, .moov_last = shape == 1, .co64 = shape == 2, .text_track = shape == 2};
        Mem in, out;
        CHECK(make(&in, &o));
        Seen a, b;
        char err[160];
        CHECK(look(&in, &a, err, sizeof err));
        CHECK_STR(a.vcodec, "V_MPEG4/ISO/AVC");
        CHECK_STR(a.acodec, "A_AC3");
        CHECK_INT(a.nv, n);
        qsort(a.vpts, (size_t)a.nv, sizeof a.vpts[0], cmp64);
        CHECK(a.vpts[0] == 0); /* (the edit list's media_time taken off: the first picture shows at 0) */
        CHECK(a.vpts[n - 1] == (n - 1) * 40000000LL);
        AvConvJob j;
        CHECK(convert(&in, &out, &j));
        CHECK_HAS(j.what, "H.264 copied");
        CHECK_HAS(j.what, "Dolby Digital → AAC");
        CHECK(look(&out, &b, err, sizeof err));
        CHECK_STR(b.vcodec, "V_MPEG4/ISO/AVC");
        CHECK_STR(b.acodec, "A_AAC");
        CHECK_INT(b.nv, n);
        qsort(b.vpts, (size_t)b.nv, sizeof b.vpts[0], cmp64);
        for (int i = 0; i < n; i++) CHECK(b.vpts[i] == a.vpts[i]); /* every picture where it was */
        /* the sound: from 0 (its first block the encoder's priming, 21 ms before) to the Dolby's end (its frames of
           32 ms), within an AAC block */
        CHECK(b.a_first >= -22000000 && b.a_first <= 22000000);
        CHECK(b.a_end > a.a_end - 30000000 && b.a_end < a.a_end + 30000000);
        free(in.p), free(out.p);
    }
}

/* What plays everywhere as it is makes nothing (and reads nothing past the moov): H.264 + AAC, H.264 + MP3, a picture
   alone; HEVC as 'hev1' (Safari wants hvc1) is copied */
TEST(mp4read_leaves_what_plays_as_it_is) {
    static const int SOUNDS[] = {SND_AAC, SND_MP3, SND_NONE};
    for (int k = 0; k < 3; k++) {
        Make o = {.sound = SOUNDS[k], .frames = 30};
        Mem in, out;
        CHECK(make(&in, &o));
        AvConvJob j;
        CHECK(!convert(&in, &out, &j));
        CHECK(j.as_is);
        CHECK(!j.unsupported);
        CHECK_STR(j.err, "It plays everywhere as it is");
        CHECK_INT((int)out.len, 0);
        free(in.p), free(out.p);
    }
    Make h = {.sound = SND_AAC, .frames = 30, .hev1 = true};
    Mem in, out;
    CHECK(make(&in, &h));
    for (int k = 0; k < 4; k++) patch(&in, "hvc1", "hev1", 0); /* (the brands in ftyp and the sample entry) */
    AvConvJob j;
    CHECK(convert(&in, &out, &j)); /* (made again: as 'hvc1') */
    CHECK(!j.as_is);
    CHECK_HAS(j.what, "HEVC copied");
    bool hvc1 = false;
    for (size_t at = 0; at + 4 <= out.len && !hvc1; at++) hvc1 = !memcmp(out.p + at, "hvc1", 4);
    CHECK(hvc1);
    free(in.p), free(out.p);
}

/* E-AC-3 (Dolby Digital Plus) in an MP4 -> AAC; tx3g subtitles -> an .srt with their text alone */
TEST(mp4read_takes_dolby_plus_and_tx3g_subtitles) {
    Make o = {.sound = SND_EAC3, .frames = 30, .subs = true};
    Mem in, out;
    CHECK(make(&in, &o));
    Seen a;
    char err[160];
    CHECK(look(&in, &a, err, sizeof err));
    CHECK_STR(a.acodec, "A_EAC3");
    AvConvJob j;
    CHECK(convert(&in, &out, &j));
    CHECK_HAS(j.what, "Dolby Digital Plus → AAC");
    CHECK_INT(j.nsubs, 1);
    if (j.nsubs) {
        CHECK_STR(j.subs[0].lang, "swe");
        CHECK_STR(j.subs[0].srt, "1\n00:00:00,000 --> 00:00:02,000\nHello\n\n2\n00:00:03,000 --> 00:00:04,500\nHej!\n\n");
    }
    av_convert_free(&j);
    free(in.p), free(out.p);
}

/* Fragmented MP4s: refused in words. Damaged ones: refused or read as far as they go - never a crash */
TEST(mp4read_refuses_fragments_and_survives_damage) {
    Make f = {.sound = SND_AAC, .frames = 30, .fragmented = true};
    Mem in, out;
    CHECK(make(&in, &f));
    AvConvJob j;
    CHECK(!convert(&in, &out, &j));
    CHECK(j.unsupported);
    CHECK_HAS(j.err, "fragmented");
    free(in.p), free(out.p);

    Make o = {.sound = SND_AC3, .frames = 12, .text_track = true};
    Mem good;
    CHECK(make(&good, &o));
    uint32_t seed = 12345;
    for (int round = 0; round < 400; round++) {
        Mem bad = {malloc(good.len), good.len, good.len};
        memcpy(bad.p, good.p, good.len);
        if (round % 3 == 0) bad.len = (size_t)(seed % good.len); /* cut short */
        for (int k = 0; k < 1 + round % 8; k++) { /* bytes changed: often in the moov, where it hurts */
            seed = seed * 1103515245 + 12345;
            size_t at = round % 2 ? seed % (good.len < 4096 ? good.len : 4096) : seed % good.len;
            if (at < bad.len) bad.p[at] = (uint8_t)(seed >> 16);
        }
        char err[160];
        MkvReader r = {&bad, (int64_t)bad.len, m_read};
        Mkv *m = mkv_open(&r, err, sizeof err);
        if (m) {
            MkvFrame fr;
            int n = 0;
            while (n < 100000 && mkv_next(m, &fr) > 0) {
                CHECK(fr.size == 0 || fr.data != NULL);
                n++;
            }
            mkv_seek(m, 500000000, 0);
            while (n < 200000 && mkv_next(m, &fr) > 0) n++;
            mkv_close(m);
        }
        if (round % 8) {
            free(bad.p);
            continue;
        }
        AvConvJob jj; /* (and every eighth through the converter too) */
        Mem o2;
        convert(&bad, &o2, &jj);
        av_convert_free(&jj);
        free(o2.p);
        free(bad.p);
    }
    free(good.p);
}

/* The MP4 fuzz target's seeds (tools/ci/fuzz.sh mp4): HEMMA_FUZZ_RAW=<dir> build/test mp4read_fuzz_seeds writes
   <dir>/mp4/ - every shape above, small. Without the variable: nothing (the suite has the tests above). */
TEST(mp4read_fuzz_seeds) {
    const char *raw = getenv("HEMMA_FUZZ_RAW");
    if (!raw || !*raw) return;
    char dir[1024], path[1100];
    snprintf(dir, sizeof dir, "%s/mp4", raw);
    CHECK(dir_make(dir));
    static const struct {
        const char *name;
        Make o;
    } SEEDS[] = {
        {"h264-ac3.mp4", {.sound = SND_AC3, .frames = 10}},
        {"h264-ac3-moov-last.mp4", {.sound = SND_AC3, .frames = 10, .moov_last = true}},
        {"h264-ac3-co64-chapters.mp4", {.sound = SND_AC3, .frames = 10, .co64 = true, .text_track = true}},
        {"h264-eac3-tx3g.mp4", {.sound = SND_EAC3, .frames = 10, .subs = true}},
        {"h264-aac.mp4", {.sound = SND_AAC, .frames = 10}},
        {"h264-mp3.mp4", {.sound = SND_MP3, .frames = 10}},
        {"hevc-aac.mp4", {.sound = SND_AAC, .frames = 10, .hev1 = true}},
        {"fragmented.mp4", {.sound = SND_AAC, .frames = 20, .fragmented = true}},
    };
    for (size_t i = 0; i < countof(SEEDS); i++) {
        Mem m;
        CHECK(make(&m, &SEEDS[i].o));
        snprintf(path, sizeof path, "%s/%s", dir, SEEDS[i].name);
        CHECK(file_write_atomic(path, m.p, m.len));
        free(m.p);
    }
}

/* 23.976 pictures a second on a 1/24000 s clock (the DuckTales episodes'): every picture where it was, to the tick.
   Before, the copy rounded each picture's length down to 90 kHz ticks: 0.75 tick lost a picture - a quarter second of
   a 23-minute episode, the picture running ahead of its sound (here 600 pictures: 5 ms). */
TEST(mp4read_keeps_film_rate_pictures_in_place) {
    Make o = {.sound = SND_AC3, .frames = 600, .vts = 24000, .vdelta = 1001, .snd_frames = 4};
    Mem in, out;
    CHECK(make(&in, &o));
    Seen a, b;
    char err[160];
    CHECK(look(&in, &a, err, sizeof err));
    AvConvJob j;
    CHECK(convert(&in, &out, &j));
    CHECK(look(&out, &b, err, sizeof err));
    CHECK_INT(a.nv, 600);
    CHECK_INT(b.nv, 600);
    qsort(a.vpts, (size_t)a.nv, sizeof a.vpts[0], cmp64);
    qsort(b.vpts, (size_t)b.nv, sizeof b.vpts[0], cmp64);
    int64_t worst = 0;
    for (int i = 0; i < 600 && i < b.nv; i++) {
        int64_t d = b.vpts[i] - a.vpts[i];
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    CHECK(worst <= 11112); /* (one tick of 90 kHz: rounding, never adding up) */
    CHECK(a.vpts[599] > 24900000000LL); /* (the last at 599 x 1001/24000 s = 24.98 s) */
    free(in.p), free(out.p);
}
