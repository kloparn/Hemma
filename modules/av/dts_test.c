#include "../../tests/test.h"
#include "../../core/plat.h"
#include "dts.h"
#include "dts_tables.h"
#include "dts_testenc.h"
#include "mkv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------ the code books */

/* Every Huffman book is a complete code: a tree whose every node has two children, with one leaf per
   level of its quantiser, each level once. */
static int leaves(const DtsHuff *h, int node, int seen[512]) {
    int n = 0;
    for (int bit = 0; bit < 2; bit++) {
        int v = h->node[node][bit];
        if (v < 0) {
            int sym = -v - 1 - DTS_LEAF;
            if (sym < -256 || sym >= 256) return -1000;
            seen[sym + 256]++;
            n++;
        } else {
            if (v <= node || v >= h->nodes) return -1000; /* (children come after their parent) */
            n += leaves(h, v, seen);
        }
    }
    return n;
}

static void check_book(const DtsHuff *h, int lo, int hi) {
    int seen[512] = {0};
    CHECK_INT(leaves(h, 0, seen), hi - lo + 1);
    CHECK_INT(h->nodes, hi - lo);
    for (int s = lo; s <= hi; s++) CHECK_INT(seen[s + 256], 1);
}

TEST(dts_code_books_are_complete) {
    for (int i = 0; i < 4; i++) check_book(&DTS_TMODE[i], 0, 3);
    for (int i = 0; i < 5; i++) check_book(&DTS_BITALLOC[i], 1, 12);
    for (int i = 0; i < 5; i++) check_book(&DTS_SCALES[i], -64, 64);
    for (int a = 1; a <= 10; a++)
        for (int s = 0; s < DTS_QUANT_BOOKS[a - 1]; s++) {
            int half = (DTS_QUANT_LEVELS[a - 1] - 1) / 2;
            check_book(&DTS_QUANT[a - 1][s], -half, half);
        }
    /* the scalar tables: rising scale factors, step sizes falling from ABITS 1, the joint scales at 0 dB
       for index 64, filters that pass a constant through the LFE interpolation unchanged */
    for (int i = 1; i < 63; i++) CHECK(DTS_SCALE6[i] >= DTS_SCALE6[i - 1]);
    for (int i = 1; i < 125; i++) CHECK(DTS_SCALE7[i] >= DTS_SCALE7[i - 1]);
    CHECK_INT(DTS_SCALE6[62], 8317638);
    CHECK_INT(DTS_SCALE7[124], 8317638);
    for (int i = 2; i < 27; i++) CHECK(DTS_STEP_LOSSY[i] < DTS_STEP_LOSSY[i - 1] && DTS_STEP_LOSSLESS[i] < DTS_STEP_LOSSLESS[i - 1]);
    CHECK(DTS_JOINT[64] == 1.0f);
    for (int k = 0; k < 64; k++) {
        double s = 0;
        for (int j = 0; j < 8; j++) s += DTS_LFE64[k + 64 * j];
        CHECK(fabs(s - 1.0) < 0.02);
    }
    for (int k = 0; k < 128; k++) {
        double s = 0;
        for (int j = 0; j < 4; j++) s += DTS_LFE128[k + 128 * j];
        CHECK(fabs(s - 1.0) < 0.02);
    }
}

/* ------------------------------------------- frames from the test writer */

/* Every tool, random contents: each frame parses exactly (each subframe ends on its check word) and the
   decoder's counts are the writer's - Huffman, block and plain codes, transients, prediction, vectors,
   joint intensity, the bit allocations - in stereo and 5.1, 512 and 1024 samples, 1, 2 or 4 subframes. */
TEST(dts_every_tool_round_trip) {
    static uint8_t frame[16384];
    static float out[DTS_SAMPLES * DTS_MAX_CHANNELS];
    TencRecord all = {0};
    for (uint32_t seed = 1; seed <= 300; seed++) {
        uint32_t r = seed * 2654435761u;
        int nblks = r & 1 ? 31 : 15, ss = (nblks + 1) / 8;
        int subframes = ss == 2 ? 1 + (int)(r >> 3 & 1) : 1 << (int)((r >> 3 & 3) % 3);
        TencOpts o = {.amode = r & 2 ? 9 : 2, .lfe = (r & 4) != 0, .nblks = nblks, .subframes = subframes, .random = true,
                      .aspf = (r >> 6 & 1) != 0, .cpf = (r >> 7 & 1) != 0, .dynf = (r >> 8 & 1) != 0,
                      .sumf = (r >> 9 & 1) != 0, .sums = (r >> 10 & 1) != 0, .seed = seed};
        TencRecord rec;
        size_t n = dts_test_frame(&o, frame, sizeof frame, &rec);
        CHECK_INT(dts_frame_size(frame, n), (int)n);
        DtsDec *d = dts_open();
        dts_set_drc(d, 1);
        int ch = 0;
        DtsInfo in;
        int rc = dts_decode(d, frame, n, out, &ch, &in);
        if (rc != DTS_OK) test_fail(__FILE__, __LINE__, "seed %u: %d at dts.c:%d", seed, rc, dts_stats(d)->error_line);
        CHECK_INT(rc, DTS_OK);
        CHECK_INT(ch, (o.amode == 9 ? 5 : 2) + o.lfe);
        CHECK_INT(in.samples, (nblks + 1) * 32);
        const DtsStats *st = dts_stats(d);
        CHECK_INT(st->huffman_codes, rec.huffman);
        CHECK_INT(st->block_codes, rec.block);
        CHECK_INT(st->linear_codes, rec.linear);
        CHECK_INT(st->transients, rec.transients);
        CHECK_INT(st->subbands_predicted, rec.predicted);
        CHECK_INT(st->subbands_vq, rec.vq);
        CHECK_INT(st->subbands_joint, rec.joint);
        for (int k = 0; k < 32; k++) CHECK_INT(st->abits[k], (int)rec.abits[k]);
        CHECK(st->bits_used <= (int)n * 8);
        for (int i = 0; i < in.samples * ch; i++) CHECK(isfinite(out[i]));
        all.huffman += rec.huffman, all.block += rec.block, all.linear += rec.linear, all.joint += rec.joint;
        all.predicted += rec.predicted, all.vq += rec.vq, all.transients += rec.transients;
        dts_close(d);
    }
    /* (and every tool came up) */
    CHECK(all.huffman > 100 && all.block > 100 && all.linear > 100 && all.joint > 10 && all.predicted > 100 &&
          all.vq > 100 && all.transients > 100);
}

/* power of x[0..n) at frequency f (Goertzel) */
static double tone_power(const float *x, int stride, int n, double f, double rate) {
    double w = 2 * 3.14159265358979 * f / rate, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double s0 = x[i * stride] + c * s1 - s2;
        s2 = s1, s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

/* 5.1: band 2 ch + 1 of channel ch carries a steady level - a tone at that band's top edge, (ch + 1) x 1500 Hz.
   Each channel's tone is its own; the WAVE order is the native one rearranged; Lo/Ro is the -3 dB mix of
   it, normalised; the LFE carries its constant. */
TEST(dts_tone_channels_layouts_and_downmix) {
    enum { FRAMES = 12 };
    static uint8_t stream[FRAMES * 2048];
    size_t len = dts_test_tone(stream, sizeof stream, FRAMES, true);
    CHECK(len > 0);
    DtsDec *nat = dts_open(), *wav = dts_open(), *st = dts_open();
    dts_set_layout(wav, DTS_LAYOUT_WAV);
    dts_set_layout(st, DTS_LAYOUT_STEREO);
    static float on[FRAMES * 512 * 6], ow[512 * 6], os[512 * 2];
    size_t at = 0;
    for (int f = 0; f < FRAMES; f++) {
        int n = dts_frame_size(stream + at, len - at), ch = 0;
        CHECK(n > 0);
        float *o = on + (size_t)f * 512 * 6;
        CHECK_INT(dts_decode(nat, stream + at, (size_t)n, o, &ch, NULL), DTS_OK);
        CHECK_INT(ch, 6);
        CHECK_INT(dts_decode(wav, stream + at, (size_t)n, ow, &ch, NULL), DTS_OK);
        CHECK_INT(dts_decode(st, stream + at, (size_t)n, os, &ch, NULL), DTS_OK);
        CHECK_INT(ch, 2);
        static const int wav_from_native[6] = {1, 2, 0, 5, 3, 4}; /* L R C LFE SL SR <- C L R SL SR LFE */
        const float c3 = 0.70710678f, norm = 1 + c3 + c3;
        for (int i = 0; i < 512; i++) {
            const float *s = o + i * 6;
            for (int k = 0; k < 6; k++) CHECK(ow[i * 6 + k] == s[wav_from_native[k]]);
            CHECK(fabsf(os[2 * i] - (s[1] + c3 * s[0] + c3 * s[3]) / norm) < 1e-6f);
            CHECK(fabsf(os[2 * i + 1] - (s[2] + c3 * s[0] + c3 * s[4]) / norm) < 1e-6f);
        }
        at += (size_t)n;
    }
    /* each channel's tone (after the filter bank's start): its own band's centre far above the others' */
    const float *tail = on + 4 * 512 * 6;
    int n = (FRAMES - 4) * 512;
    for (int c = 0; c < 5; c++) {
        double mine = tone_power(tail + c, 6, n, (c + 1) * 1500.0, 48000);
        CHECK(mine > 1);
        for (int k = 0; k < 5; k++)
            if (k != c) CHECK(tone_power(tail + c, 6, n, (k + 1) * 1500.0, 48000) < mine / 1000);
    }
    double lfe = 0;
    for (int i = 0; i < n; i++) lfe += tail[i * 6 + 5];
    CHECK(lfe / n > 1e-4); /* (a constant: the interpolated decimated samples) */
    CHECK_STR(dts_channel_name(9, true, DTS_LAYOUT_WAV, 3), "LFE");
    CHECK_STR(dts_channel_name(9, true, DTS_LAYOUT_NATIVE, 0), "C");
    CHECK_STR(dts_channel_name(9, true, DTS_LAYOUT_STEREO, 1), "R");
    CHECK_STR(dts_amode_name(9, true), "5.1");
    dts_close(nat), dts_close(wav), dts_close(st);
}

/* Cut short, damaged, or not DTS at all: an answer, never a crash; damage is silence of the right length */
TEST(dts_cut_and_damaged_frames) {
    static uint8_t frame[16384], buf[3 * 16384 + 100];
    static float out[DTS_SAMPLES * DTS_MAX_CHANNELS];
    TencOpts o = {.amode = 9, .lfe = true, .nblks = 15, .subframes = 1, .random = true, .seed = 7};
    size_t n = dts_test_frame(&o, frame, sizeof frame, NULL);
    DtsDec *d = dts_open();
    int ch = 0;
    CHECK_INT(dts_decode(d, frame, n - 1, out, &ch, NULL), DTS_ERR_TRUNCATED);
    CHECK_INT(dts_decode(d, frame, 8, out, &ch, NULL), DTS_ERR_SYNC);
    uint8_t junk[64] = {1, 2, 3};
    CHECK_INT(dts_frame_size(junk, sizeof junk), 0);
    CHECK_INT(dts_decode(d, junk, sizeof junk, out, &ch, NULL), DTS_ERR_SYNC);
    int damaged = 0;
    for (size_t at = 20; at < n; at += 7) { /* a flipped byte anywhere in the frame */
        frame[at] ^= 0x5A;
        int rc = dts_decode(d, frame, n, out, &ch, NULL);
        CHECK(rc == DTS_OK || rc == DTS_ERR_BITSTREAM || rc == DTS_ERR_SYNC || rc == DTS_ERR_TRUNCATED);
        if (rc == DTS_ERR_BITSTREAM) {
            damaged++;
            CHECK_INT(ch, 6);
            for (int i = 0; i < 512 * 6; i++) CHECK(out[i] == 0.0f);
        }
        frame[at] ^= 0x5A;
    }
    CHECK(damaged > 0);
    /* found in a stream with junk around: the frames, then nothing */
    memset(buf, 0xAB, sizeof buf);
    memcpy(buf + 50, frame, n);
    memcpy(buf + 50 + n, frame, n);
    CHECK_INT(dts_find_frame(buf, 50 + 2 * n, true), 50);
    CHECK_INT(dts_find_frame(buf + 51, 2 * n, true), (long)n - 1);
    dts_close(d);
}

/* ------------------------------------------------- real media (opt-in) */

static bool file_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

static void wav_header(FILE *f, int channels, int rate, uint32_t frames) {
    uint32_t data = frames * (uint32_t)channels * 4;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 3, 0};
    uint32_t v[] = {36 + data, (uint32_t)rate, (uint32_t)(rate * channels * 4)};
    memcpy(h + 4, &v[0], 4);
    h[22] = (uint8_t)channels;
    memcpy(h + 24, &v[1], 4);
    memcpy(h + 28, &v[2], 4);
    h[32] = (uint8_t)(channels * 4);
    h[34] = 32;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

/* HEMMA_TEST_DTS=<file.mkv with a DTS track> decodes its first HEMMA_TEST_DTS_S seconds (30 by default; 0 = all)
   and says what the frames used; HEMMA_TEST_DTS_WAV=<file.wav> keeps the PCM (native order, float) to compare
   with another decoder. */
TEST(dts_real_media_from_env) {
    const char *path = plat_getenv("HEMMA_TEST_DTS");
    if (!path || !*path) return;
    const char *secs_s = plat_getenv("HEMMA_TEST_DTS_S"), *wav = plat_getenv("HEMMA_TEST_DTS_WAV");
    double secs = secs_s && *secs_s ? atof(secs_s) : 30;
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    MkvReader r = {fp, size, file_cb};
    char err[160] = "";
    Mkv *m = mkv_open(&r, err, sizeof err);
    if (!m) test_fail(__FILE__, __LINE__, "mkv_open: %s", err);
    CHECK(m);
    int track = -1;
    for (int i = 0; i < mkv_track_count(m); i++)
        if (!strcmp(mkv_track(m, i)->codec, "A_DTS")) track = mkv_track(m, i)->number;
    CHECK(track > 0);
    FILE *out = wav && *wav ? fopen(wav, "wb") : NULL;
    if (out) wav_header(out, 6, 48000, 0);
    DtsDec *d = dts_open();
    static float pcm[DTS_SAMPLES * DTS_MAX_CHANNELS];
    int frames = 0, good = 0, bad = 0, unsupported = 0, other = 0, chans = 0;
    long predicted = 0, vq = 0, joint = 0, transients = 0, huff = 0, block = 0, linear = 0, abits[32] = {0};
    uint32_t written = 0;
    DtsInfo first = {0};
    MkvFrame f;
    float peak = 0;
    double sum2 = 0;
    long nsum = 0;
    while (mkv_next(m, &f) == 1) {
        if ((int)f.track != track) continue;
        DtsInfo in = {0};
        int ch = 0, rc = dts_decode(d, f.data, f.size, pcm, &ch, &in);
        if (!frames) first = in;
        frames++;
        const DtsStats *st = dts_stats(d);
        predicted += st->subbands_predicted, vq += st->subbands_vq, joint += st->subbands_joint;
        transients += st->transients, huff += st->huffman_codes, block += st->block_codes, linear += st->linear_codes;
        for (int k = 0; k < 32; k++) abits[k] += st->abits[k];
        if (rc == DTS_OK) good++;
        else if (rc == DTS_ERR_BITSTREAM) bad++, fprintf(stderr, "    frame %d: bitstream error at dts.c:%d\n", frames, st->error_line);
        else if (rc == DTS_ERR_UNSUPPORTED) unsupported++;
        else other++, fprintf(stderr, "    frame %d: %d\n", frames, rc);
        if (rc == DTS_OK) {
            chans = ch;
            for (int i = 0; i < in.samples * ch; i++) {
                float v = fabsf(pcm[i]);
                peak = v > peak ? v : peak;
                sum2 += (double)pcm[i] * pcm[i];
            }
            nsum += in.samples * ch;
            if (out && ch == 6) written += (uint32_t)fwrite(pcm, sizeof(float) * 6, (size_t)in.samples, out);
        }
        if (secs > 0 && frames * (double)(in.samples ? in.samples : 512) / 48000.0 >= secs) break;
    }
    if (out) {
        wav_header(out, 6, 48000, written);
        fclose(out);
    }
    fprintf(stderr,
            "    %s %d Hz %d kbit/s, %d-sample frames of %d bytes, %d channel(s)%s, %s filter bank, version %d\n"
            "    %d frames: %d decoded, %d damaged, %d unsupported, %d other; %d channel(s) out\n"
            "    subbands: %ld predicted, %ld vector-coded, %ld joint, %ld transients; codes: %ld Huffman, %ld block, %ld plain\n"
            "    peak %.3f, rms %.4f\n",
            dts_amode_name(first.amode, first.lfe), first.sample_rate, first.bitrate, first.samples, first.frame_bytes,
            first.channels, first.ext_audio ? " + extension" : "", first.perfect_qmf ? "perfect" : "lossy", first.version,
            frames, good, bad, unsupported, other, chans, predicted, vq, joint, transients, huff, block, linear, peak,
            nsum ? sqrt(sum2 / (double)nsum) : 0.0);
    fprintf(stderr, "    abits:");
    for (int k = 0; k < 27; k++) fprintf(stderr, " %d:%ld", k, abits[k]);
    fprintf(stderr, "\n");
    dts_close(d);
    mkv_close(m);
    fclose(fp);
    CHECK(frames > 0);
}
