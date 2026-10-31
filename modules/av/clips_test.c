/* Real videos for the browser tests (tests/browser, issue #49): small MP4s - H.264 + AAC, what every
 * browser plays - made by modules/av's own encoders, so nothing binary sits in the repository.
 *
 *   HEMMA_TEST_CLIPS=<folder> build/test av_test_clips
 *
 * writes a library under <folder>/library (a series of two 100-second episodes - long enough for the
 * "next episode" card, which wants three times its 30 seconds - and a 40-second film; 2.2: a 40-second film
 * in two languages, "Two Voices (2024)", its sounds as a converted copy has them; 1.6: audiobooks - a
 * 40-second .m4b with three chapters, its index after its sound as some are, and a book in two .m4a parts),
 * one clip to upload under <folder>/upload (2.5: and "Dolby test (2019).mp4", H.264 + AC-3 in an MP4 - an upload makes
 * its sound AAC), and picture subtitles (Blu-ray .sup files) in <folder>/sidecars
 * - outside the library: the test that wants them puts them beside the episodes. Without the variable
 * it only makes one clip and the .sup files in memory and checks them - so the suite keeps the generator
 * working. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "aac.h"
#include "ac3_testenc.h"
#include "audiotags_testmk.h"
#include "h264enc.h"
#include "mkv.h"
#include "mp4.h"
#include "subpic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 48
#define FPS 25

typedef struct {
    FILE *f;
    Buf mem; /* no file: in memory */
    bool ok;
} Sink;

static bool sink_write(void *ctx, const void *p, size_t n) {
    Sink *s = ctx;
    if (s->f) s->ok = s->ok && fwrite(p, 1, n, s->f) == n;
    else buf_put(&s->mem, p, n);
    return s->ok;
}

static bool sink_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    Sink *s = ctx;
    if (!s->f) {
        if (off < 0 || (size_t)off + n > s->mem.len) return false;
        memcpy(s->mem.p + off, p, n);
        return true;
    }
    s->ok = s->ok && fseek(s->f, (long)off, SEEK_SET) == 0 && fwrite(p, 1, n, s->f) == n && fseek(s->f, 0, SEEK_END) == 0;
    return s->ok;
}

/* 2.2: a sound's AAC frames held and written 48 at a time - one chunk each, as the converter writes a file with
   several sounds */
#define RUN 48
typedef struct {
    Buf data;
    uint32_t size[RUN];
    int n;
} Run;

static bool run_out(Mp4 *m, int track, Run *r) {
    bool ok = true;
    size_t at = 0;
    for (int i = 0; i < r->n && ok; at += r->size[i++]) ok = mp4_sample(m, track, r->data.p + at, r->size[i], AAC_FRAME, 0, true);
    r->n = 0;
    buf_reset(&r->data);
    return ok;
}

static bool hold(Mp4 *m, int track, Run *r, const uint8_t *au, int n) {
    buf_put(&r->data, au, (size_t)n);
    r->size[r->n++] = (uint32_t)n;
    return r->n < RUN || run_out(m, track, r);
}

/* `seconds` of a moving picture (its colours turn once a second) and a tone (its pitch says which
   clip it is), as H.264 + AAC in a faststart MP4. pitch2 > 0 (2.2): a film in two languages as a converted
   copy has them - a second sound at that pitch, "English" (eng, the default) then "Svenska" (swe, off), each
   its own track, their frames in runs of 48. */
static bool clip(Sink *s, int seconds, int pitch, int pitch2) {
    H264Params hp = {.width = W, .height = H, .fps_num = FPS, .fps_den = 1, .keyint = FPS, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    AacConfig ac = {48000, 2, 96000, 0, 0};
    char err[100];
    AacEnc *a = aac_open(&ac, err, sizeof err), *a2 = pitch2 > 0 ? aac_open(&ac, err, sizeof err) : NULL;
    if (!e || !a || (pitch2 > 0 && !a2)) return false;
    uint8_t avcc[256], asc[2];
    size_t avcc_len = h264enc_avcc(e, avcc, sizeof avcc);
    aac_specific_config(a, asc);
    int frames = seconds * FPS, blocks = seconds * 48000 / AAC_FRAME + 2, sounds = a2 ? 2 : 1;
    Mp4Out out = {s, sink_write, sink_write_at};
    Mp4 *m = mp4_new(&out, 0, mp4_moov_bound(frames + sounds * blocks + 8, frames + sounds * blocks + 8));
    Mp4Track vt = {.codec = MP4_H264, .timescale = FPS, .config = avcc, .config_len = avcc_len, .width = W, .height = H};
    Mp4Track at = {.codec = MP4_AAC, .timescale = 48000, .config = asc, .config_len = 2, .channels = 2,
                   .sample_rate = 48000, .avg_bitrate = 96000, .max_bitrate = 96000, .start = AAC_PRIMING};
    Mp4Track at2 = at;
    if (a2) {
        memcpy(at.lang, "eng", 4), at.name = "English";
        memcpy(at2.lang, "swe", 4), at2.name = "Svenska", at2.off = true;
    }
    int vtrack = mp4_add_track(m, &vt), atrack = mp4_add_track(m, &at), atrack2 = a2 ? mp4_add_track(m, &at2) : -1;
    bool ok = vtrack >= 0 && atrack >= 0 && (!a2 || atrack2 >= 0) && mp4_begin(m);
    static uint8_t y[W * H], u[W / 2 * H / 2], v[W / 2 * H / 2], au[AAC_MAX_AU], au2[AAC_MAX_AU];
    static float pcm[AAC_FRAME * 2];
    Run r1 = {{0}}, r2 = {{0}};
    int f = 0, b = 0;
    bool flushing = false;
    while (ok && (f < frames || !flushing)) {
        /* by time: a picture when it's due before the next sound block */
        if (f < frames && (flushing || (int64_t)f * 48000 <= (int64_t)b * AAC_FRAME * FPS)) {
            for (int i = 0; i < W * H; i++) y[i] = (uint8_t)((i % W * 2 + i / W * 3 + f * 4) & 0xFF);
            memset(u, 64 + f / FPS * 23 % 128, sizeof u);
            memset(v, 192 - f / FPS * 31 % 128, sizeof v);
            H264Picture p = {y, u, v, W, W / 2, W / 2, f};
            H264Packet pk;
            ok = h264enc_encode(e, &p, &pk) && mp4_sample(m, vtrack, pk.data, pk.size, 1, 0, pk.idr);
            f++;
            continue;
        }
        int got, got2 = 0;
        if (b < blocks) {
            for (int i = 0; i < AAC_FRAME; i++)
                pcm[2 * i] = pcm[2 * i + 1] = 0.2f * (float)sin(2 * 3.14159265 * pitch * (b * AAC_FRAME + i) / 48000.0);
            got = aac_encode(a, pcm, au, AAC_MAX_AU);
            for (int i = 0; a2 && i < AAC_FRAME; i++)
                pcm[2 * i] = pcm[2 * i + 1] = 0.2f * (float)sin(2 * 3.14159265 * pitch2 * (b * AAC_FRAME + i) / 48000.0);
            if (a2) got2 = aac_encode(a2, pcm, au2, AAC_MAX_AU);
            b++;
        } else {
            flushing = true;
            got = aac_flush(a, au, AAC_MAX_AU);
            if (a2) got2 = aac_flush(a2, au2, AAC_MAX_AU);
            if (got <= 0 && got2 <= 0) continue;
        }
        if (!a2) {
            if (got > 0) ok = mp4_sample(m, atrack, au, (size_t)got, AAC_FRAME, 0, true);
        } else {
            if (got > 0) ok = hold(m, atrack, &r1, au, got);
            if (got2 > 0) ok = ok && hold(m, atrack2, &r2, au2, got2);
        }
        ok = ok && got >= 0 && got2 >= 0;
    }
    if (a2) ok = ok && run_out(m, atrack, &r1) && run_out(m, atrack2, &r2);
    ok = ok && mp4_finish(m) && s->ok;
    mp4_free(m);
    h264enc_free(e);
    aac_close(a);
    if (a2) aac_close(a2);
    buf_free(&r1.data), buf_free(&r2.data);
    return ok;
}

/* 2.5: H.264 + Dolby (AC-3 2.0, 48 kHz, 192 kbit/s) in an MP4 - what the owner's DuckTales are, silent in Chrome and
   Firefox: an upload makes its sound AAC. The tone's 64 frames (ac3_test_tone) over and over. */
static bool dolby_clip(Sink *s, int seconds) {
    H264Params hp = {.width = W, .height = H, .fps_num = FPS, .fps_den = 1, .keyint = FPS, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    static uint8_t ac3[64 * 768];
    size_t got = ac3_test_tone(ac3, sizeof ac3, 64);
    uint8_t avcc[256], dac3[3];
    if (!e || got != sizeof ac3 || !mp4_dac3(ac3, 768, dac3)) return false;
    size_t avcc_len = h264enc_avcc(e, avcc, sizeof avcc);
    int frames = seconds * FPS, blocks = seconds * 48000 / 1536;
    Mp4Out out = {s, sink_write, sink_write_at};
    Mp4 *m = mp4_new(&out, 0, mp4_moov_bound(frames + blocks + 8, frames + blocks + 8));
    Mp4Track vt = {.codec = MP4_H264, .timescale = FPS, .config = avcc, .config_len = avcc_len, .width = W, .height = H};
    Mp4Track at = {.codec = MP4_AC3, .timescale = 48000, .config = dac3, .config_len = 3, .channels = 2, .sample_rate = 48000};
    memcpy(at.lang, "eng", 4);
    int vtrack = mp4_add_track(m, &vt), atrack = mp4_add_track(m, &at);
    bool ok = vtrack >= 0 && atrack >= 0 && mp4_begin(m);
    static uint8_t y[W * H], u[W / 2 * H / 2], v[W / 2 * H / 2];
    for (int f = 0, b = 0; ok && (f < frames || b < blocks);) {
        if (f < frames && (b >= blocks || (int64_t)f * 48000 <= (int64_t)b * 1536 * FPS)) { /* (by time, as clip()) */
            for (int i = 0; i < W * H; i++) y[i] = (uint8_t)((i % W * 3 + i / W * 2 + f * 5) & 0xFF);
            memset(u, 90, sizeof u), memset(v, 170, sizeof v);
            H264Picture p = {y, u, v, W, W / 2, W / 2, f};
            H264Packet pk;
            ok = h264enc_encode(e, &p, &pk) && mp4_sample(m, vtrack, pk.data, pk.size, 1, 0, pk.idr);
            f++;
        } else {
            ok = mp4_sample(m, atrack, ac3 + (size_t)(b % 64) * 768, 768, 1536, 0, true);
            b++;
        }
    }
    ok = ok && mp4_finish(m) && s->ok;
    mp4_free(m);
    h264enc_free(e);
    return ok;
}

static bool dolby_file(const char *dir, const char *rel, int seconds) {
    char path[600], folder[600];
    if (snprintf(path, sizeof path, "%s/%s", dir, rel) >= (int)sizeof path) return false;
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash) *slash = '\0';
    if (!dir_make(folder)) return false;
    Sink s = {.f = fopen(path, "wb"), .ok = true};
    if (!s.f) return false;
    bool ok = dolby_clip(&s, seconds);
    return fclose(s.f) == 0 && ok;
}

static bool sink_read(void *ctx, int64_t off, void *dst, size_t n) {
    const Buf *b = ctx;
    if (off < 0 || (size_t)off > b->len || n > b->len - (size_t)off) return false;
    memcpy(dst, b->p + off, n);
    return true;
}

/* ------------------------------------------------- picture subtitles (a .sup)
   Blu-ray PGS, written the way a disc has it (subpic.h reads it back): per segment "PG", PTS (90 kHz), DTS,
   type, u16 size, payload. Two cues each: a box low in the middle at 2-9 s, a smaller one at the top left
   at 20-24 s - white with a black edge, so a test can find it on the picture. pictures.sup is on a 640 x 480
   frame (the clips' 4:3); pictures-576.sup on 720 x 576, the frame MakeMKV's .idx gives even an NTSC DVD
   (not the video's shape: the player maps the frame onto the shown picture, whatever their sizes). The
   browser tests put them beside the episodes (tests/browser/subpictures.spec.mjs). */
typedef struct {
    int start_ms, end_ms, x, y, w, h;
} SupCue;
static const struct {
    const char *name;
    int fw, fh;
    SupCue cues[2];
} SUPS[] = {
    {"pictures.sup", 640, 480, {{2000, 9000, 160, 380, 320, 60}, {20000, 24000, 40, 40, 200, 50}}},
    {"pictures-576.sup", 720, 576, {{2000, 9000, 180, 448, 360, 74}, {20000, 24000, 45, 48, 225, 60}}},
};

static void put8(Buf *b, unsigned v) {
    uint8_t c = (uint8_t)v;
    buf_put(b, &c, 1);
}
static void put16(Buf *b, unsigned v) { put8(b, v >> 8), put8(b, v); }

static void sup_seg(Buf *b, int ms, int type, const Buf *pay) {
    uint32_t pts = (uint32_t)ms * 90;
    buf_put(b, "PG", 2);
    put16(b, pts >> 16), put16(b, pts & 0xFFFF), put16(b, 0), put16(b, 0); /* PTS, DTS */
    put8(b, (unsigned)type), put16(b, (unsigned)pay->len);
    buf_put(b, pay->p, pay->len);
}

/* PGS RLE: a run of colour c (0 = clear): 00 LL (6 bits) / 00 4L LL (14 bits), coloured 00 8L C / 00 CL LL C */
static void sup_run(Buf *o, int c, int n) {
    put8(o, 0);
    if (n < 64) put8(o, (c ? 0x80u : 0) | (unsigned)n);
    else put8(o, (c ? 0xC0u : 0x40u) | (unsigned)n >> 8), put8(o, (unsigned)n & 0xFF);
    if (c) put8(o, (unsigned)c);
}

static void sup_cue(Buf *b, int fw, int fh, const SupCue *c, int k) {
    const int w = c->w, h = c->h, at = c->start_ms;
    Buf p = {0}, rle = {0};
    put16(&p, (unsigned)fw), put16(&p, (unsigned)fh), put8(&p, 0x10), put16(&p, (unsigned)k), put8(&p, 0x80); /* epoch start */
    put8(&p, 0), put8(&p, 0), put8(&p, 1); /* palette 0, one object: */
    put16(&p, 0), put8(&p, 0), put8(&p, 0), put16(&p, (unsigned)c->x), put16(&p, (unsigned)c->y);
    sup_seg(b, at, 0x16, &p); /* PCS */
    buf_reset(&p);
    put8(&p, 1), put8(&p, 0), put16(&p, (unsigned)c->x), put16(&p, (unsigned)c->y);
    put16(&p, (unsigned)w), put16(&p, (unsigned)h);
    sup_seg(b, at, 0x17, &p); /* WDS */
    buf_reset(&p);
    put8(&p, 0), put8(&p, 0);
    put8(&p, 1), put8(&p, 235), put8(&p, 128), put8(&p, 128), put8(&p, 255); /* 1: white (Y Cr Cb A) */
    put8(&p, 2), put8(&p, 16), put8(&p, 128), put8(&p, 128), put8(&p, 255);  /* 2: black */
    sup_seg(b, at, 0x14, &p); /* PDS */
    for (int y = 0; y < h; y++) {
        if (y < 4 || y >= h - 4) sup_run(&rle, 2, w);
        else sup_run(&rle, 2, 4), sup_run(&rle, 1, w - 8), sup_run(&rle, 2, 4);
        put8(&rle, 0), put8(&rle, 0); /* the line's end */
    }
    buf_reset(&p);
    put16(&p, 0), put8(&p, 0), put8(&p, 0xC0); /* object 0, the first and last of its segments */
    put8(&p, (unsigned)(rle.len + 4) >> 16), put16(&p, (unsigned)(rle.len + 4) & 0xFFFF), put16(&p, (unsigned)w),
        put16(&p, (unsigned)h);
    buf_put(&p, rle.p, rle.len);
    sup_seg(b, at, 0x15, &p); /* ODS */
    buf_reset(&p);
    sup_seg(b, at, 0x80, &p); /* END */
    /* the end: a display set showing nothing */
    put16(&p, (unsigned)fw), put16(&p, (unsigned)fh), put8(&p, 0x10), put16(&p, (unsigned)k + 100), put8(&p, 0),
        put8(&p, 0), put8(&p, 0), put8(&p, 0);
    sup_seg(b, c->end_ms, 0x16, &p);
    buf_reset(&p);
    sup_seg(b, c->end_ms, 0x80, &p);
    buf_free(&p);
    buf_free(&rle);
}

static void sup_file(Buf *b, int s) {
    for (int k = 0; k < (int)countof(SUPS[s].cues); k++) sup_cue(b, SUPS[s].fw, SUPS[s].fh, &SUPS[s].cues[k], k);
}

static bool sup_mem_read(void *ctx, int64_t off, void *dst, size_t n) {
    const Buf *b = ctx;
    if (off < 0 || (uint64_t)off > b->len || n > b->len - (size_t)off) return false;
    memcpy(dst, b->p + off, n);
    return true;
}

/* An audiobook's file: `seconds` of a tone as AAC (44.1 kHz stereo, aac.c), with what m says of it
   (audiotags_testmk.h at_test_m4b: tags, chapters, the moov's place) */
static bool book_file(const char *dir, const char *rel, int seconds, int pitch, AtTestM4b m) {
    AacConfig ac = {44100, 2, 64000, 0, 0};
    AacEnc *e = aac_open(&ac, NULL, 0);
    if (!e) return false;
    uint8_t asc[2], au[AAC_MAX_AU];
    aac_specific_config(e, asc);
    static float pcm[AAC_FRAME * 2];
    int blocks = seconds * 44100 / AAC_FRAME + 1, n = 0;
    int *lens = malloc((size_t)(blocks + 8) * sizeof *lens);
    Buf aus = {0};
    for (int k = 0; lens && k < blocks + 8; k++) {
        for (int i = 0; k < blocks && i < AAC_FRAME; i++)
            pcm[2 * i] = pcm[2 * i + 1] = 0.2f * (float)sin(2 * 3.14159265 * pitch * (k * AAC_FRAME + i) / 44100.0);
        int got = k < blocks ? aac_encode(e, pcm, au, AAC_MAX_AU) : aac_flush(e, au, AAC_MAX_AU);
        if (got > 0) buf_put(&aus, au, (size_t)got), lens[n++] = got;
        if (k >= blocks && got <= 0) break;
    }
    aac_close(e);
    m.aac = (const uint8_t *)aus.p, m.aac_len = lens, m.aac_frames = n, m.asc = asc, m.asc_len = 2, m.priming = AAC_PRIMING;
    Buf b = {0};
    if (lens) at_test_m4b(&b, &m);
    char path[600], folder[600];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash) *slash = '\0';
    bool ok = lens && b.len > 1000 && dir_make(folder) && file_write_atomic(path, b.p, b.len);
    buf_free(&b);
    buf_free(&aus);
    free(lens);
    return ok;
}

static bool clip_file(const char *dir, const char *rel, int seconds, int pitch, int pitch2) {
    char path[600], folder[600];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash) *slash = '\0';
    if (!dir_make(folder)) return false;
    Sink s = {.f = fopen(path, "wb"), .ok = true};
    if (!s.f) return false;
    bool ok = clip(&s, seconds, pitch, pitch2);
    return fclose(s.f) == 0 && ok;
}

TEST(av_test_clips) {
    const char *dir = getenv("HEMMA_TEST_CLIPS");
    if (!dir || !*dir) { /* the suite: one clip in memory, a whole MP4 */
        Sink two = {.ok = true}; /* 2.2: two sounds - three traks, the names in their hdlr boxes */
        CHECK(clip(&two, 2, 495, 990));
        int traks = 0;
        bool eng = false, swe = false;
        for (size_t at = 0; at + 8 <= two.mem.len; at++) {
            traks += !memcmp(two.mem.p + at, "trak", 4);
            eng |= !memcmp(two.mem.p + at, "English", 8);
            swe |= !memcmp(two.mem.p + at, "Svenska", 8);
        }
        CHECK_INT(traks, 3);
        CHECK(eng && swe);
        buf_free(&two.mem);

        Sink dolby = {.ok = true}; /* 2.5: H.264 + AC-3 in an MP4, read back as mkv.h reads one */
        CHECK(dolby_clip(&dolby, 2));
        MkvReader r = {&dolby.mem, (int64_t)dolby.mem.len, sink_read};
        char why[100];
        Mkv *mk = mkv_open(&r, why, sizeof why);
        CHECK(mk != NULL);
        if (mk) {
            const MkvTrack *vt = mkv_find_track(mk, MKV_VIDEO), *at = mkv_find_track(mk, MKV_AUDIO);
            CHECK(vt && !strcmp(vt->codec, "V_MPEG4/ISO/AVC"));
            CHECK(at && !strcmp(at->codec, "A_AC3") && at->rate == 48000 && at->channels == 2);
            mkv_close(mk);
        }
        buf_free(&dolby.mem);

        Sink s = {.ok = true};
        CHECK(clip(&s, 2, 440, 0));
        CHECK(s.mem.len > 1000);
        /* top-level boxes back to back: ftyp, then the index (moov) before the media (mdat) */
        const uint8_t *p = (const uint8_t *)s.mem.p;
        int moov = -1, mdat = -1, k = 0;
        for (size_t at = 0; at + 8 <= s.mem.len; k++) {
            uint64_t n = (uint32_t)p[at] << 24 | (uint32_t)p[at + 1] << 16 | (uint32_t)p[at + 2] << 8 | p[at + 3];
            if (n == 1 && at + 16 <= s.mem.len) { /* a 64-bit size follows: the mdat, begun before its size was known */
                n = 0;
                for (size_t i = 8; i < 16; i++) n = n << 8 | p[at + i];
            }
            if (k == 0) CHECK(memcmp(p + at + 4, "ftyp", 4) == 0);
            if (!memcmp(p + at + 4, "moov", 4)) moov = k;
            if (!memcmp(p + at + 4, "mdat", 4)) mdat = k;
            CHECK(n >= 8);
            at += n;
            if (at == s.mem.len) break;
        }
        CHECK(moov > 0 && mdat > moov);
        buf_free(&s.mem);

        /* each .sup reads back as written: its frame, two cues, their times and places, a white box with a
           black edge */
        for (int s = 0; s < (int)countof(SUPS); s++) {
            Buf sup = {0};
            sup_file(&sup, s);
            MkvReader r = {&sup, (int64_t)sup.len, sup_mem_read};
            char err[100];
            SubPic *sp = subpic_open_sup(&r, err, sizeof err);
            CHECK(sp != NULL);
            int fw = 0, fh = 0;
            subpic_frame(sp, &fw, &fh);
            CHECK_INT(fw, SUPS[s].fw);
            CHECK_INT(fh, SUPS[s].fh);
            CHECK_INT(subpic_count(sp), (int)countof(SUPS[s].cues));
            for (int k = 0; k < subpic_count(sp) && k < (int)countof(SUPS[s].cues); k++) {
                const SubPicCue *c = subpic_cue(sp, k);
                const SupCue *w = &SUPS[s].cues[k];
                CHECK(c->start_ms == w->start_ms && c->end_ms == w->end_ms);
                CHECK(c->x == w->x && c->y == w->y && c->w == w->w && c->h == w->h);
                uint8_t *px = malloc((size_t)c->w * (size_t)c->h);
                uint32_t pal[256];
                int n = 0;
                CHECK(px && subpic_render(sp, k, px, (size_t)c->w * (size_t)c->h, pal, &n));
                if (px && n > px[0] && n > px[5 * c->w + 5]) {
                    CHECK(pal[px[0]] == 0x000000FFu);            /* the edge: black, solid */
                    CHECK(pal[px[5 * c->w + 5]] == 0xFFFFFFFFu); /* inside: white, solid */
                }
                free(px);
            }
            subpic_close(sp);
            buf_free(&sup);
        }
        return;
    }
    char lib[600], up[600], sides[600], path[700];
    snprintf(lib, sizeof lib, "%s/library", dir);
    snprintf(up, sizeof up, "%s/upload", dir);
    snprintf(sides, sizeof sides, "%s/sidecars", dir);
    CHECK(clip_file(lib, "Series/Testshow/Season 1/Testshow S01E01.mp4", 100, 440, 0));
    CHECK(clip_file(lib, "Series/Testshow/Season 1/Testshow S01E02.mp4", 100, 550, 0));
    CHECK(clip_file(lib, "Movies/Clip (2020)/Clip (2020).mp4", 40, 660, 0));
    CHECK(clip_file(lib, "Movies/Two Voices (2024)/Two Voices (2024).mp4", 40, 495, 990)); /* 2.2: English, Svenska */
    static const AtTestChapter story[] = {{0, "Opening"}, {16000, "Chapter 1 - The Middle"}, {30000, "Chapter 2 - The End"}};
    CHECK(book_file(lib, "Audiobooks/Test Story.m4b", 40, 330,
                    (AtTestM4b){.title = "Test Story", .artist = "Ann Author", .narrator = "Nora Narrator", .year = 2026,
                                .chapters = story, .nchapters = 3, .moov_last = true}));
    CHECK(book_file(lib, "Audiobooks/Parted Book/01 - Part One.m4a", 6, 440,
                    (AtTestM4b){.title = "Part One", .album = "Parted Book", .artist = "Ann Author", .track = 1}));
    CHECK(book_file(lib, "Audiobooks/Parted Book/02 - Part Two.m4a", 6, 550,
                    (AtTestM4b){.title = "Part Two", .album = "Parted Book", .artist = "Ann Author", .track = 2}));
    CHECK(clip_file(up, "Upload test (2021).mp4", 4, 330, 0));
    CHECK(dolby_file(up, "Dolby test (2019).mp4", 20)); /* 2.5: an MP4 whose sound the upload makes AAC */
    CHECK(dir_make(sides));
    for (int s = 0; s < (int)countof(SUPS); s++) {
        Buf sup = {0};
        sup_file(&sup, s);
        snprintf(path, sizeof path, "%s/%s", sides, SUPS[s].name);
        CHECK(file_write_atomic(path, sup.p, sup.len));
        buf_free(&sup);
    }
    printf("test clips in %s\n", dir);
}
