/* tools/aaccheck.c — dev check for modules/av/aac (not part of the app).
 *
 *   aaccheck <file.mkv | synth:RATE> <outdir> [--from S] [--seconds S] [--kbps K] [--shape 0|1] [--mono]
 *
 * Takes the first AC-3 track of a Matroska file (only read) and decodes it to stereo
 * (modules/av/ac3) - or makes a synthetic test signal at RATE Hz (tones with vibrato,
 * noise, castanet clicks, silence, a full-scale stretch) - then encodes AAC-LC
 * (modules/av/aac; --mono downmixes first) and writes to outdir:
 *   source.wav    the decoded AC-3, 16-bit stereo: what went in
 *   out.aac       the AAC as an ADTS stream (plays in browsers, VLC, ...)
 *   frames.bin    raw access units: u32 count, u32 size[count], then the data
 *   asc.bin       the AudioSpecificConfig (WebCodecs description, MP4 esds)
 *   source.f32    the input, float32 interleaved
 *   decoded.f32   the access units through the decoder in aac_testdec.h, priming
 *                 removed, float32 interleaved (to compare with Chrome's decoder)
 * and prints the encoder's speed (x real time, encoding only), bitrate and the
 * round trip's SNR / segmental SNR.
 *
 * Build: zig cc -O2 -std=gnu11 -Icore tools/aaccheck.c modules/av/aac.c modules/av/ac3.c
 *          modules/av/mkv.c core/base.c core/plat.c -lws2_32 -liphlpapi -lbcrypt -o aaccheck.exe */
#include "../core/plat.h"
#include "../modules/av/aac.h"
#include "../modules/av/aac_testdec.h"
#include "../modules/av/ac3.h"
#include "../modules/av/mkv.h"

#include <math.h>

static bool read_at(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

static bool write_file(const char *dir, const char *name, const void *p, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = file_open(path, "wb");
    if (!f) return false;
    bool ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static bool write_wav(const char *dir, const float *x, int n, int sr, int ch) {
    size_t bytes = (size_t)n * 2 * (size_t)ch;
    uint8_t *w = malloc(44 + bytes);
    if (!w) return false;
    memcpy(w, "RIFF", 4);
    le32(w + 4, (uint32_t)(36 + bytes));
    memcpy(w + 8, "WAVEfmt ", 8);
    le32(w + 16, 16);
    w[20] = 1, w[21] = 0, w[22] = (uint8_t)ch, w[23] = 0; /* PCM */
    le32(w + 24, (uint32_t)sr);
    le32(w + 28, (uint32_t)(sr * 2 * ch));
    w[32] = (uint8_t)(2 * ch), w[33] = 0, w[34] = 16, w[35] = 0;
    memcpy(w + 36, "data", 4);
    le32(w + 40, (uint32_t)bytes);
    for (int i = 0; i < ch * n; i++) {
        float v = x[i] * 32768.0f;
        int s = v > 32767 ? 32767 : v < -32768 ? -32768 : (int)lrintf(v);
        w[44 + 2 * i] = (uint8_t)s;
        w[45 + 2 * i] = (uint8_t)(s >> 8);
    }
    bool ok = write_file(dir, "source.wav", w, 44 + bytes);
    free(w);
    return ok;
}

/* Tones with vibrato, a melody, noise bursts, clicks, a silent second and a full-scale stretch. */
static float *make_signal(int sr, int n) {
    float *x = malloc(sizeof(float) * 2 * (size_t)n);
    uint32_t r = 7;
    double p0 = 0, p1 = 0;
    for (int i = 0; i < n; i++) {
        double t = (double)i / sr, f0 = 220.0 * pow(2.0, ((int)(t * 3) % 8) / 12.0);
        p0 += 2 * 3.14159265358979 * f0 * (1 + 0.004 * sin(2 * 3.14159265358979 * 5 * t)) / sr;
        p1 += 2 * 3.14159265358979 * 2637.0 / sr;
        r = r * 1664525u + 1013904223u;
        double nz = ((double)(r >> 8) / 8388608.0 - 1.0) * 0.08 * (0.5 + 0.5 * sin(t));
        double v = 0.3 * sin(p0) + 0.05 * sin(p1) + nz;
        if ((i % (sr / 2)) < sr / 200) v += 0.6 * ((double)(r >> 9) / 4194304.0 - 1.0) * exp(-(i % (sr / 2)) / (sr / 2000.0));
        if (t >= 5 && t < 6) v = 0;                                  /* silence */
        if (t >= 8 && t < 9) v = (i / (sr / 400)) % 2 ? 0.999 : -0.999; /* full-scale square */
        x[2 * i] = (float)v;
        x[2 * i + 1] = (float)(0.8 * v + 0.15 * sin(p1 * 1.5));
    }
    return x;
}

static double snr_db(const float *x, const float *y, size_t n) {
    double s = 0, e = 0;
    for (size_t i = 0; i < n; i++) {
        s += (double)x[i] * x[i];
        e += ((double)x[i] - y[i]) * ((double)x[i] - y[i]);
    }
    return 10 * log10((s + 1e-20) / (e + 1e-20));
}

/* The first AC-3 track of an MKV, decoded to stereo from `from` for `seconds`. */
static float *from_mkv(const char *path, double from, double seconds, int *n_out, int *sr_out, double *speed) {
    FILE *fp = file_open(path, "rb");
    int64_t size = 0;
    if (!fp || !path_stat(path, &size, NULL, NULL)) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    MkvReader rd = {fp, size, read_at};
    char err[200];
    Mkv *m = mkv_open(&rd, err, sizeof err);
    if (!m) {
        fprintf(stderr, "%s: %s\n", path, err);
        fclose(fp);
        return NULL;
    }
    int track = -1;
    for (int i = 0; i < mkv_track_count(m) && track < 0; i++)
        if (!strcmp(mkv_track(m, i)->codec, "A_AC3")) track = mkv_track(m, i)->number;
    if (track < 0) {
        fprintf(stderr, "no AC-3 track\n");
        return NULL;
    }
    if (from > 0) mkv_seek(m, (int64_t)(from * 1e9), track);
    Ac3Dec *ad = ac3_open();
    ac3_set_layout(ad, AC3_LAYOUT_STEREO);
    int sr = 48000, cap = (int)(seconds * 48000) + AC3_SAMPLES, n = 0;
    float *pcm = malloc(sizeof(float) * 2 * (size_t)cap);
    static float frame[AC3_SAMPLES * AC3_MAX_CHANNELS];
    MkvFrame f;
    int64_t t0 = now_ms();
    while (pcm && n + AC3_SAMPLES <= cap && mkv_next(m, &f) == 1) {
        if (f.track != track || (f.pts != MKV_NOPTS && f.pts < (int64_t)(from * 1e9))) continue;
        int ch = 0;
        Ac3Info info;
        if (ac3_decode(ad, f.data, f.size, frame, &ch, &info) != AC3_OK || ch != 2) continue;
        sr = info.sample_rate;
        memcpy(pcm + 2 * (size_t)n, frame, sizeof(float) * 2 * AC3_SAMPLES);
        n += AC3_SAMPLES;
    }
    int64_t ms = now_ms() - t0;
    *speed = ms ? (double)n / sr * 1000.0 / (double)ms : 0;
    ac3_close(ad);
    mkv_close(m);
    fclose(fp);
    *n_out = n;
    *sr_out = sr;
    return pcm;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: aaccheck <file.mkv | synth:RATE> <outdir> [--from S] [--seconds S] [--kbps K] "
                        "[--shape 0|1] [--mono]\n");
        return 2;
    }
    const char *path = argv[1], *dir = argv[2];
    double from = 0, seconds = 60, ac3_speed = 0;
    int kbps = 160, shape = 0, ch = 2;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--mono")) ch = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--from")) from = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--seconds")) seconds = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--kbps")) kbps = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--shape")) shape = atoi(argv[++i]);
    }
    dir_make(dir);
    int n = 0, sr = 0;
    float *pcm;
    if (!strncmp(path, "synth:", 6)) {
        sr = atoi(path + 6);
        n = (int)(seconds * sr);
        pcm = make_signal(sr, n);
    } else {
        pcm = from_mkv(path, from, seconds, &n, &sr, &ac3_speed);
    }
    if (!pcm || n < AAC_FRAME) {
        fprintf(stderr, "no audio\n");
        return 1;
    }
    if (ch == 1)
        for (int i = 0; i < n; i++) pcm[i] = 0.5f * (pcm[2 * i] + pcm[2 * i + 1]);

    /* PCM -> AAC (timed) */
    char err[200];
    AacConfig cfg = {sr, ch, kbps * 1000, 0, shape};
    AacEnc *e = aac_open(&cfg, err, sizeof err);
    if (!e) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    int blocks = (n + AAC_FRAME - 1) / AAC_FRAME, units = 0;
    uint8_t *aus = malloc((size_t)(blocks + 2) * AAC_MAX_AU);
    uint32_t *sizes = malloc(sizeof(uint32_t) * (size_t)(blocks + 2));
    float blk[2 * AAC_FRAME];
    size_t bytes = 0;
    int64_t t1 = now_ms();
    for (int b = 0; b <= blocks + 1; b++) {
        int got;
        if (b < blocks) {
            for (int i = 0; i < ch * AAC_FRAME; i++) {
                size_t s = (size_t)b * (size_t)ch * AAC_FRAME + (size_t)i;
                blk[i] = s < (size_t)ch * (size_t)n ? pcm[s] : 0.0f;
            }
            got = aac_encode(e, blk, aus + bytes, AAC_MAX_AU);
        } else {
            got = aac_flush(e, aus + bytes, AAC_MAX_AU);
        }
        if (got < 0) {
            fprintf(stderr, "encoder error at block %d\n", b);
            return 1;
        }
        if (got > 0) {
            sizes[units++] = (uint32_t)got;
            bytes += (size_t)got;
        }
    }
    int64_t t_aac = now_ms() - t1;
    double secs = (double)n / sr;

    /* files */
    uint8_t asc[2];
    aac_specific_config(e, asc);
    write_file(dir, "asc.bin", asc, 2);
    size_t fb_len = 4 + 4 * (size_t)units + bytes;
    uint8_t *fb = malloc(fb_len), *adts = malloc(bytes + 7 * (size_t)units);
    le32(fb, (uint32_t)units);
    size_t off = 0, ao = 0;
    for (int i = 0; i < units; i++) {
        le32(fb + 4 + 4 * i, sizes[i]);
        aac_adts_header(sr, ch, (int)sizes[i], adts + ao);
        memcpy(adts + ao + 7, aus + off, sizes[i]);
        ao += 7 + sizes[i];
        off += sizes[i];
    }
    memcpy(fb + 4 + 4 * (size_t)units, aus, bytes);
    write_file(dir, "frames.bin", fb, fb_len);
    write_file(dir, "out.aac", adts, ao);
    write_file(dir, "source.f32", pcm, sizeof(float) * (size_t)ch * (size_t)n);
    write_wav(dir, pcm, n, sr, ch);

    /* back through the test decoder */
    Dec d;
    if (!dec_open(&d, sr, ch)) return 1;
    float *y = malloc(sizeof(float) * (size_t)ch * AAC_FRAME * (size_t)units);
    off = 0;
    for (int i = 0; i < units; i++) {
        if (!dec_au(&d, aus + off, (int)sizes[i], y + (size_t)i * (size_t)ch * AAC_FRAME)) {
            fprintf(stderr, "test decoder rejected unit %d\n", i);
            return 1;
        }
        off += sizes[i];
    }
    const float *yy = y + (size_t)ch * AAC_PRIMING;
    write_file(dir, "decoded.f32", yy, sizeof(float) * (size_t)ch * (size_t)n);
    double snr = snr_db(pcm, yy, (size_t)ch * (size_t)n), seg = 0;
    int segs = 0;
    for (int s = 0; s + 1024 <= n; s += 1024) {
        double en = 0;
        for (size_t i = (size_t)ch * (size_t)s; i < (size_t)ch * (size_t)(s + 1024); i++) en += (double)pcm[i] * pcm[i];
        if (en < 1024.0 * ch * 1e-6) continue;
        double v = snr_db(pcm + (size_t)ch * (size_t)s, yy + (size_t)ch * (size_t)s, (size_t)ch * 1024);
        seg += v < -10 ? -10 : v > 60 ? 60 : v;
        segs++;
    }
    const AacStats *st = aac_stats(e);
    printf("%s\n  %.1f s, %d Hz, %d channel%s", path, secs, sr, ch, ch == 1 ? "" : "s");
    if (ac3_speed > 0) printf(", from %.1f s (AC-3 decode %.0fx real time)", from, ac3_speed);
    printf("\n  AAC %d kbit/s target: %.1f kbit/s actual, %d units (%d short, %d M/S bands), bandwidth %d Hz, %s window\n",
           kbps, (double)bytes * 8 / secs / 1000, units, st->short_frames, st->ms_bands, st->bandwidth,
           shape ? "KBD" : "sine");
    printf("  encode: %lld ms = %.0fx real time\n", (long long)t_aac, t_aac ? secs * 1000.0 / (double)t_aac : 0.0);
    printf("  round trip: SNR %.1f dB, segmental %.1f dB\n", snr, segs ? seg / segs : 0.0);
    dec_close(&d);
    aac_close(e);
    return 0;
}
