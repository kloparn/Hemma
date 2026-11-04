/* modules/av/intro_testmk.h — made-up episodes for finding intros (tests only: av/intro_test.c, media/intros_test.c).
 * "Music" - every quarter second a few new partials between 200 and 2,500 Hz - of an episode's own, with a theme
 * (the same seed = the same theme in every episode) put in at a given place; as PCM, or as a whole MP4 the
 * library takes (a 32x32 H.264 picture at 2 fps + AAC-LC mono, our own encoders). */
#ifndef HEMMA_AV_INTRO_TESTMK_H
#define HEMMA_AV_INTRO_TESTMK_H

#include "aac.h"
#include "h264enc.h"
#include "mp4.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double itm_rnd(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return (*s >> 8) / 16777216.0;
}

/* n samples of a seed's music at `gain` */
static void intro_test_music(float *out, long n, int rate, uint32_t seed, double gain) {
    uint32_t r = seed;
    double c[5] = {0}, s[5] = {0}, wc[5] = {0}, ws[5] = {0}; /* oscillators by rotation: no sin() a sample */
    int np = 0;
    long step = rate / 4;
    for (long i = 0; i < n; i++) {
        if (i % step == 0) {
            np = 3 + (int)(itm_rnd(&r) * 3);
            for (int k = 0; k < np; k++) {
                double f = 200 + itm_rnd(&r) * 2300, a = (0.05 + itm_rnd(&r) * 0.2) * gain;
                wc[k] = cos(2 * 3.14159265358979 * f / rate), ws[k] = sin(2 * 3.14159265358979 * f / rate);
                c[k] = a, s[k] = 0;
            }
        }
        double v = 0;
        for (int k = 0; k < np; k++) {
            double nc = c[k] * wc[k] - s[k] * ws[k];
            s[k] = c[k] * ws[k] + s[k] * wc[k], c[k] = nc;
            v += s[k];
        }
        out[i] = (float)v;
    }
}

/* An episode's sound: its own music, the theme (theme seed 0 = none; at `gain`) in [at, at + len) seconds, noise on top */
static float *intro_test_sound(long n, int rate, double at, double len, uint32_t own, uint32_t theme, double gain, double noise) {
    float *x = malloc(sizeof(float) * (size_t)n);
    if (!x) return NULL;
    intro_test_music(x, n, rate, own, 1.0);
    long t0 = (long)(at * rate), tn = (long)(len * rate);
    if (theme && tn > 0) {
        float *th = malloc(sizeof(float) * (size_t)tn);
        if (!th) return free(x), NULL;
        intro_test_music(th, tn, rate, theme, gain);
        for (long i = 0; i < tn && t0 + i < n; i++) x[t0 + i] = th[i];
        free(th);
    }
    uint32_t r = own ^ 0x5555u;
    for (long i = 0; i < n; i++) x[i] += (float)((itm_rnd(&r) - 0.5) * noise);
    return x;
}

typedef struct {
    FILE *f;
    bool ok;
} ItmSink;

static bool itm_write(void *ctx, const void *p, size_t n) {
    ItmSink *s = ctx;
    s->ok = s->ok && fwrite(p, 1, n, s->f) == n;
    return s->ok;
}

static bool itm_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    ItmSink *s = ctx;
    s->ok = s->ok && fseek(s->f, (long)off, SEEK_SET) == 0 && fwrite(p, 1, n, s->f) == n && fseek(s->f, 0, SEEK_END) == 0;
    return s->ok;
}

/* An MP4 episode `seconds` long at `path` (its folder there already): the theme at [at, at + len) */
static bool intro_test_episode(const char *path, int seconds, double at, double len, uint32_t own, uint32_t theme) {
    enum { W = 32, HH = 32, FPS = 2, RATE = 22050 };
    long n = (long)seconds * RATE;
    float *pcm = intro_test_sound(n, RATE, at, len, own, theme, 1.0, 0.01);
    FILE *f = pcm ? fopen(path, "wb") : NULL;
    if (!f) return free(pcm), false;
    ItmSink sink = {f, true};
    H264Params hp = {.width = W, .height = HH, .fps_num = FPS, .fps_den = 1, .keyint = FPS * 4, .annexb = false};
    H264Enc *e = h264enc_new(&hp);
    AacConfig ac = {RATE, 1, 48000, 0, 0};
    AacEnc *a = aac_open(&ac, NULL, 0);
    bool ok = e && a;
    uint8_t avcc[256], asc[2];
    size_t avcc_len = ok ? h264enc_avcc(e, avcc, sizeof avcc) : 0;
    if (a) aac_specific_config(a, asc);
    int frames = seconds * FPS, blocks = (int)(n / AAC_FRAME) + 1;
    Mp4Out out = {&sink, itm_write, itm_write_at};
    Mp4 *m = ok ? mp4_new(&out, 0, mp4_moov_bound(frames + blocks + 8, frames + blocks + 8)) : NULL;
    Mp4Track vt = {.codec = MP4_H264, .timescale = FPS, .config = avcc, .config_len = avcc_len, .width = W, .height = HH};
    Mp4Track tt = {.codec = MP4_AAC, .timescale = RATE, .config = asc, .config_len = 2, .channels = 1, .sample_rate = RATE,
                   .avg_bitrate = 48000, .max_bitrate = 48000, .start = AAC_PRIMING};
    int vtrack = m ? mp4_add_track(m, &vt) : -1, atrack = m ? mp4_add_track(m, &tt) : -1;
    ok = ok && m && vtrack >= 0 && atrack >= 0 && mp4_begin(m);
    static uint8_t y[W * HH], u[W / 2 * HH / 2], v[W / 2 * HH / 2], au[AAC_MAX_AU];
    float blk[AAC_FRAME];
    int fi = 0, b = 0;
    bool flushing = false;
    while (ok && (fi < frames || !flushing)) {
        if (fi < frames && (flushing || (int64_t)fi * RATE <= (int64_t)b * AAC_FRAME * FPS)) { /* by time */
            for (int i = 0; i < W * HH; i++) y[i] = (uint8_t)((i * 7 + fi * 13) & 0xFF);
            memset(u, 128, sizeof u);
            memset(v, 128, sizeof v);
            H264Picture p = {y, u, v, W, W / 2, W / 2, fi};
            H264Packet pk;
            ok = h264enc_encode(e, &p, &pk) && mp4_sample(m, vtrack, pk.data, pk.size, 1, 0, pk.idr);
            fi++;
            continue;
        }
        int got;
        if ((long)b * AAC_FRAME < n) {
            for (int i = 0; i < AAC_FRAME; i++) {
                long at2 = (long)b * AAC_FRAME + i;
                blk[i] = at2 < n ? pcm[at2] : 0;
            }
            got = aac_encode(a, blk, au, AAC_MAX_AU);
            b++;
        } else {
            flushing = true;
            got = aac_flush(a, au, AAC_MAX_AU);
            if (got <= 0) continue;
        }
        if (got > 0) ok = mp4_sample(m, atrack, au, (size_t)got, AAC_FRAME, 0, true);
        ok = ok && got >= 0;
    }
    ok = ok && mp4_finish(m) && sink.ok;
    if (m) mp4_free(m);
    if (e) h264enc_free(e);
    if (a) aac_close(a);
    free(pcm);
    return fclose(f) == 0 && ok;
}

#endif
