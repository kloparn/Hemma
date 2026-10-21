/* Real videos for the browser tests (tests/browser, issue #49): small MP4s - H.264 + AAC, what every
 * browser plays - made by modules/av's own encoders, so nothing binary sits in the repository.
 *
 *   HEMMA_TEST_CLIPS=<folder> build/test av_test_clips
 *
 * writes a library under <folder>/library (a series of two 100-second episodes - long enough for the
 * "next episode" card, which wants three times its 30 seconds - and a 40-second film) and one
 * clip to upload under <folder>/upload. Without the variable it only makes one clip in memory and
 * checks it - so the suite keeps the generator working. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "aac.h"
#include "h264enc.h"
#include "mp4.h"

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

/* `seconds` of a moving picture (its colours turn once a second) and a tone (its pitch says which
   clip it is), as H.264 + AAC in a faststart MP4. */
static bool clip(Sink *s, int seconds, int pitch) {
    H264Params hp = {.width = W, .height = H, .fps_num = FPS, .fps_den = 1, .keyint = FPS, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    AacConfig ac = {48000, 2, 96000, 0, 0};
    char err[100];
    AacEnc *a = aac_open(&ac, err, sizeof err);
    if (!e || !a) return false;
    uint8_t avcc[256], asc[2];
    size_t avcc_len = h264enc_avcc(e, avcc, sizeof avcc);
    aac_specific_config(a, asc);
    int frames = seconds * FPS, blocks = seconds * 48000 / AAC_FRAME + 2;
    Mp4Out out = {s, sink_write, sink_write_at};
    Mp4 *m = mp4_new(&out, 0, mp4_moov_bound(frames + blocks + 8, frames + blocks + 8));
    Mp4Track vt = {.codec = MP4_H264, .timescale = FPS, .config = avcc, .config_len = avcc_len, .width = W, .height = H};
    Mp4Track at = {.codec = MP4_AAC, .timescale = 48000, .config = asc, .config_len = 2, .channels = 2,
                   .sample_rate = 48000, .avg_bitrate = 96000, .max_bitrate = 96000, .start = AAC_PRIMING};
    int vtrack = mp4_add_track(m, &vt), atrack = mp4_add_track(m, &at);
    bool ok = vtrack >= 0 && atrack >= 0 && mp4_begin(m);
    static uint8_t y[W * H], u[W / 2 * H / 2], v[W / 2 * H / 2], au[AAC_MAX_AU];
    static float pcm[AAC_FRAME * 2];
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
        int got;
        if (b < blocks) {
            for (int i = 0; i < AAC_FRAME; i++)
                pcm[2 * i] = pcm[2 * i + 1] = 0.2f * (float)sin(2 * 3.14159265 * pitch * (b * AAC_FRAME + i) / 48000.0);
            got = aac_encode(a, pcm, au, AAC_MAX_AU);
            b++;
        } else {
            flushing = true;
            got = aac_flush(a, au, AAC_MAX_AU);
            if (got <= 0) continue;
        }
        if (got > 0) ok = mp4_sample(m, atrack, au, (size_t)got, AAC_FRAME, 0, true);
        ok = ok && got >= 0;
    }
    ok = ok && mp4_finish(m) && s->ok;
    mp4_free(m);
    h264enc_free(e);
    aac_close(a);
    return ok;
}

static bool clip_file(const char *dir, const char *rel, int seconds, int pitch) {
    char path[600], folder[600];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash) *slash = '\0';
    if (!dir_make(folder)) return false;
    Sink s = {.f = fopen(path, "wb"), .ok = true};
    if (!s.f) return false;
    bool ok = clip(&s, seconds, pitch);
    return fclose(s.f) == 0 && ok;
}

TEST(av_test_clips) {
    const char *dir = getenv("HEMMA_TEST_CLIPS");
    if (!dir || !*dir) { /* the suite: one clip in memory, a whole MP4 */
        Sink s = {.ok = true};
        CHECK(clip(&s, 2, 440));
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
        return;
    }
    char lib[600], up[600];
    snprintf(lib, sizeof lib, "%s/library", dir);
    snprintf(up, sizeof up, "%s/upload", dir);
    CHECK(clip_file(lib, "Series/Testshow/Season 1/Testshow S01E01.mp4", 100, 440));
    CHECK(clip_file(lib, "Series/Testshow/Season 1/Testshow S01E02.mp4", 100, 550));
    CHECK(clip_file(lib, "Movies/Clip (2020)/Clip (2020).mp4", 40, 660));
    CHECK(clip_file(up, "Upload test (2021).mp4", 4, 330));
    printf("test clips in %s\n", dir);
}
