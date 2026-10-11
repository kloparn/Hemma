/* The AAC encoder against the standard's definitions, and back through the small AAC-LC
 * decoder in aac_testdec.h (written from ISO/IEC 14496-3: raw_data_block, SCE/CPE, sections,
 * scalefactors, spectral data, M/S, IMDCT with every window sequence): what goes in must
 * come back out. Chrome's own decoder (WebCodecs) is the independent check on top - it
 * matches that decoder to ~136 dB on every stream tried, see tools/aaccheck.c. */
#include "../../core/plat.h"
#include "../../tests/test.h"
#include "aac.h"
#include "aac_testdec.h"
#include "ac3.h"
#include "mkv.h"

#include <math.h>

#define PI 3.14159265358979323846

/* -------------------------------------------------- encode / decode runs */

typedef struct {
    uint8_t *data;
    int *size;
    int n, bytes;
} Aus;

static void aus_free(Aus *a) {
    free(a->data);
    free(a->size);
}

/* Encode n samples per channel (interleaved) into a list of access units. */
static bool encode_all(const AacConfig *cfg, const float *pcm, int n, Aus *out, AacStats *st) {
    memset(out, 0, sizeof *out);
    AacEnc *e = aac_open(cfg, NULL, 0);
    if (!e) return false;
    int blocks = (n + AAC_FRAME - 1) / AAC_FRAME, ch = cfg->channels;
    out->data = malloc((size_t)(blocks + 2) * AAC_MAX_AU);
    out->size = malloc(sizeof(int) * (size_t)(blocks + 2));
    float *blk = malloc(sizeof(float) * AAC_FRAME * (size_t)ch);
    bool ok = out->data && out->size && blk;
    for (int b = 0; ok && b <= blocks + 1; b++) {
        int got;
        if (b < blocks) {
            for (int i = 0; i < AAC_FRAME * ch; i++) {
                long src = (long)b * AAC_FRAME * ch + i;
                blk[i] = src < (long)n * ch ? pcm[src] : 0.0f;
            }
            got = aac_encode(e, blk, out->data + out->bytes, AAC_MAX_AU);
        } else {
            got = aac_flush(e, out->data + out->bytes, AAC_MAX_AU);
        }
        if (got < 0) ok = false;
        if (got > 0) {
            out->size[out->n++] = got;
            out->bytes += got;
        }
    }
    if (ok) ok = aac_flush(e, out->data + out->bytes, AAC_MAX_AU) == 0 && out->n == blocks + 1;
    if (st) *st = *aac_stats(e);
    free(blk);
    aac_close(e);
    return ok;
}

/* Decode every unit; returns the output (n + priming samples per channel) or NULL. */
static float *decode_all(int sr, int ch, const Aus *a, int *frames_short) {
    Dec d;
    if (!dec_open(&d, sr, ch)) return NULL;
    float *out = malloc(sizeof(float) * AAC_FRAME * (size_t)ch * (size_t)(a->n + 1));
    int off = 0;
    for (int i = 0; out && i < a->n; i++) {
        if (!dec_au(&d, a->data + off, a->size[i], out + (size_t)i * AAC_FRAME * ch)) {
            free(out);
            out = NULL;
            break;
        }
        off += a->size[i];
    }
    if (frames_short) *frames_short = d.short_units;
    dec_close(&d);
    return out;
}

/* SNR in dB of y against x (channel c of interleaved data) over [from, to). */
static double snr(const float *x, const float *y, int ch, int c, int from, int to) {
    double s = 0, e = 0;
    for (int i = from; i < to; i++) {
        double a = x[(size_t)i * ch + c], b = y[(size_t)i * ch + c];
        s += a * a;
        e += (a - b) * (a - b);
    }
    return 10.0 * log10((s + 1e-20) / (e + 1e-20));
}

static double seg_snr(const float *x, const float *y, int ch, int from, int to) {
    double sum = 0;
    int n = 0;
    for (int s = from; s + 1024 <= to; s += 1024) {
        double en = 0;
        for (int i = s; i < s + 1024; i++)
            for (int c = 0; c < ch; c++) en += (double)x[(size_t)i * ch + c] * x[(size_t)i * ch + c];
        if (en < 1e-6 * 1024 * ch) continue; /* skip near-silence */
        double v = 0;
        for (int c = 0; c < ch; c++) v += snr(x, y, ch, c, s, s + 1024);
        v /= ch;
        sum += v < -10 ? -10 : v > 60 ? 60 : v;
        n++;
    }
    return n ? sum / n : 99;
}

static uint32_t rng = 12345u;
static float frand(void) { /* uniform [-1, 1) */
    rng = rng * 1664525u + 1013904223u;
    return (float)((rng >> 8) & 0xFFFFFF) / 8388608.0f - 1.0f;
}

/* ------------------------------------------------------------ the tests */

TEST(aac_mdct_matches_the_definition) {
    AacConfig c = {48000, 2, 0, 0, 0};
    AacEnc *e = aac_open(&c, NULL, 0);
    CHECK(e);
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? 256 : 2048, N = n / 2;
        double n0 = n / 4.0 + 0.5;
        static float z[2048], x[1024], y[2048];
        for (int i = 0; i < n; i++) z[i] = frand() * 1000.0f;
        aac_mdct(e, z, x, n);
        double worst = 0, peak = 0;
        for (int k = 0; k < N; k++) {
            double ref = 0;
            for (int i = 0; i < n; i++) ref += 2.0 * z[i] * cos(2 * PI / n * (i + n0) * (k + 0.5));
            worst = fmax(worst, fabs(ref - x[k]));
            peak = fmax(peak, fabs(ref));
        }
        CHECK(worst < 2e-5 * peak);
        aac_imdct(e, x, y, n);
        worst = peak = 0;
        for (int i = 0; i < n; i++) {
            double ref = 0;
            for (int k = 0; k < N; k++) ref += 2.0 / n * x[k] * cos(2 * PI / n * (i + n0) * (k + 0.5));
            worst = fmax(worst, fabs(ref - y[i]));
            peak = fmax(peak, fabs(ref));
        }
        CHECK(worst < 2e-5 * peak);
    }
    aac_close(e);
}

TEST(aac_windows_are_power_complementary) {
    AacConfig c = {44100, 1, 0, 0, 0};
    AacEnc *e = aac_open(&c, NULL, 0);
    CHECK(e);
    for (int sh = 0; sh < 2; sh++)
        for (int shape = 0; shape < 2; shape++) {
            const float *w = aac_window(e, sh, shape);
            int half = sh ? 128 : 1024;
            for (int i = 0; i < half; i++) {
                CHECK(fabs((double)w[i] * w[i] + (double)w[half - 1 - i] * w[half - 1 - i] - 1.0) < 1e-5);
                if (i) CHECK(w[i] >= w[i - 1]);
            }
        }
    /* KBD against its definition (14496-3 4.6.11.3.2) at a few points, alpha 4 long / 6 short */
    for (int sh = 0; sh < 2; sh++) {
        int n = sh ? 256 : 2048, half = n / 2;
        double alpha = sh ? 6 : 4, total = 0, acc = 0, wp[1025];
        for (int p = 0; p <= half; p++) {
            double r = (p - n / 4.0) / (n / 4.0), x = PI * alpha * sqrt(fmax(0, 1 - r * r)), i0 = 1, term = 1;
            for (int k = 1; k < 80; k++) term *= x * x / 4 / ((double)k * k), i0 += term;
            total += wp[p] = i0;
        }
        const float *w = aac_window(e, sh, 1);
        for (int i = 0; i < half; i++) {
            acc += wp[i];
            if (i == 0 || i == half / 4 || i == half / 2 || i == half - 1) CHECK(fabs(w[i] - sqrt(acc / total)) < 1e-6);
        }
    }
    aac_close(e);
}

TEST(aac_codebooks_are_complete_prefix_codes) {
    static const int sizes[12] = {121, 81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289};
    for (int cb = 0; cb < 12; cb++) {
        const uint32_t *sfc = NULL;
        const uint8_t *len;
        const uint16_t *code16 = NULL;
        int n = sizes[cb];
        if (cb == 0) aac_sf_codebook(&sfc, &len);
        else {
            CHECK_INT(aac_codebook(cb)->size, n);
            code16 = aac_codebook(cb)->code;
            len = aac_codebook(cb)->len;
        }
        unsigned long long kraft = 0;
        for (int i = 0; i < n; i++) {
            uint32_t ci = cb ? code16[i] : sfc[i];
            CHECK(len[i] >= 1 && len[i] <= 19);
            CHECK(ci < (1u << len[i]));
            kraft += 1ull << (19 - len[i]);
            for (int j = 0; j < n; j++) { /* nobody is a prefix of somebody else */
                if (j == i || len[j] < len[i]) continue;
                uint32_t cj = cb ? code16[j] : sfc[j];
                CHECK((cj >> (len[j] - len[i])) != ci || (len[j] == len[i] && cj != ci));
            }
        }
        CHECK(kraft == 1ull << 19);
    }
    /* well-known entries: the all-zero quad of book 1 is the 1-bit "0", so is "same scalefactor" */
    CHECK_INT(aac_codebook(1)->len[40], 1);
    CHECK_INT(aac_codebook(1)->code[40], 0);
    const uint32_t *sfc;
    const uint8_t *sfl;
    aac_sf_codebook(&sfc, &sfl);
    CHECK_INT(sfl[60], 1);
    CHECK_INT(sfc[60], 0);
    CHECK(aac_codebook(0) == NULL && aac_codebook(12) == NULL);
}

TEST(aac_band_tables_and_rates) {
    static const int ok_rates[] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025};
    for (int r = 0; r < 8; r++)
        for (int sh = 0; sh < 2; sh++) {
            int nb = 0;
            const uint16_t *o = aac_swb_offsets(ok_rates[r], sh, &nb);
            CHECK(o && nb > 0);
            CHECK_INT(o[0], 0);
            CHECK_INT(o[nb], sh ? 128 : 1024);
            for (int b = 0; b < nb; b++) CHECK(o[b + 1] > o[b] && o[b] % 4 == 0);
        }
    int nb;
    aac_swb_offsets(48000, false, &nb);
    CHECK_INT(nb, 49);
    aac_swb_offsets(32000, false, &nb);
    CHECK_INT(nb, 51);
    aac_swb_offsets(44100, true, &nb);
    CHECK_INT(nb, 14);
    char err[100];
    AacConfig bad = {8000, 2, 0, 0, 0};
    CHECK(aac_open(&bad, err, sizeof err) == NULL);
    CHECK_HAS(err, "8000");
    bad = (AacConfig){48000, 3, 0, 0, 0};
    CHECK(aac_open(&bad, err, sizeof err) == NULL);
    CHECK_INT(aac_rate_index(48000), 3);
    CHECK_INT(aac_rate_index(44100), 4);
    CHECK_INT(aac_rate_index(1234), -1);
}

TEST(aac_specific_config_and_adts) {
    AacConfig c = {48000, 2, 160000, 0, 0};
    AacEnc *e = aac_open(&c, NULL, 0);
    uint8_t asc[2];
    CHECK_INT(aac_specific_config(e, asc), 2);
    CHECK_INT(asc[0], 0x11); /* AOT 2, rate index 3, 2 channels, 1024 samples */
    CHECK_INT(asc[1], 0x90);
    aac_close(e);
    c = (AacConfig){44100, 1, 0, 0, 0};
    e = aac_open(&c, NULL, 0);
    aac_specific_config(e, asc);
    CHECK_INT(asc[0], 0x12);
    CHECK_INT(asc[1], 0x08);
    aac_close(e);
    uint8_t h[7];
    CHECK_INT(aac_adts_header(48000, 2, 500, h), 7);
    CHECK_INT(h[0], 0xFF);
    CHECK_INT(h[1] & 0xF6, 0xF0);         /* sync, layer 0 */
    CHECK_INT(h[1] & 1, 1);               /* no CRC */
    CHECK_INT(h[2] >> 6, 1);              /* AAC LC */
    CHECK_INT((h[2] >> 2) & 15, 3);       /* 48 kHz */
    CHECK_INT(((h[2] & 1) << 2) | (h[3] >> 6), 2);
    CHECK_INT(((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5), 507);
    CHECK_INT(aac_adts_header(7000, 2, 10, h), 0);
}

TEST(aac_sine_round_trip) {
    int sr = 48000, n = sr * 2;
    float *x = malloc(sizeof(float) * (size_t)n);
    for (int i = 0; i < n; i++) x[i] = 0.5f * (float)sin(2 * PI * 1000.0 * i / sr);
    AacConfig c = {sr, 1, 64000, 0, 0};
    Aus a;
    AacStats st;
    CHECK(encode_all(&c, x, n, &a, &st));
    CHECK_INT(a.n, n / AAC_FRAME + 1 + (n % AAC_FRAME != 0));
    float *y = decode_all(sr, 1, &a, NULL);
    CHECK(y);
    double s = snr(x, y + AAC_PRIMING, 1, 0, 4096, n - 4096);
    if (getenv("AAC_VERBOSE")) printf("    sine: %.1f dB, %lld bit/s\n", s, st.bits_total * sr / AAC_FRAME / a.n);
    CHECK(s > 30.0);
    CHECK(st.short_frames <= 2); /* only the abrupt start from silence */
    free(x);
    free(y);
    aus_free(&a);
}

/* A stand-in for programme audio: tones with vibrato, a melody, noise with a moving
   envelope, wide stereo. */
static float *music(int sr, int n, int ch) {
    float *x = malloc(sizeof(float) * (size_t)n * (size_t)ch);
    double ph[6] = {0};
    for (int i = 0; i < n; i++) {
        double t = (double)i / sr, env = 0.5 + 0.5 * sin(2 * PI * 0.7 * t);
        double f0 = 220.0 * pow(2.0, ((int)(t * 4) % 8) / 12.0);
        ph[0] += 2 * PI * f0 * (1 + 0.003 * sin(2 * PI * 5 * t)) / sr;
        ph[1] += 2 * PI * f0 * 2.01 / sr;
        ph[2] += 2 * PI * 3150.0 / sr;
        double tone = 0.25 * sin(ph[0]) + 0.12 * sin(ph[1]) + 0.05 * sin(ph[2]);
        for (int c = 0; c < ch; c++) {
            double noise = 0.06 * env * frand();
            x[(size_t)i * ch + c] = (float)(tone * (c ? 0.8 : 1.0) + noise);
        }
    }
    return x;
}

TEST(aac_stereo_programme_round_trip_and_rate) {
    int sr = 48000, n = sr * 3;
    float *x = music(sr, n, 2);
    for (int br = 128000; br <= 192000; br += 32000) {
        AacConfig c = {sr, 2, br, 0, 0};
        Aus a;
        AacStats st;
        CHECK(encode_all(&c, x, n, &a, &st));
        float *y = decode_all(sr, 2, &a, NULL);
        CHECK(y);
        double s0 = snr(x, y + 2 * AAC_PRIMING, 2, 0, 4096, n - 4096), s1 = snr(x, y + 2 * AAC_PRIMING, 2, 1, 4096, n - 4096);
        double rate = (double)st.bits_total * sr / AAC_FRAME / a.n;
        if (getenv("AAC_VERBOSE"))
            printf("    %d kbit/s: SNR %.1f / %.1f dB, seg %.1f dB, actual %.0f kbit/s, %d M/S bands, bw %d Hz\n",
                   br / 1000, s0, s1, seg_snr(x, y + 2 * AAC_PRIMING, 2, 4096, n - 4096), rate / 1000, st.ms_bands,
                   st.bandwidth);
        CHECK(s0 > 12.0 && s1 > 12.0);
        CHECK(rate < br * 1.08);
        CHECK(st.bits_max <= 6144 * 2);
        free(y);
        aus_free(&a);
    }
    free(x);
}

TEST(aac_transients_switch_to_short_windows) {
    int sr = 48000, n = sr * 2;
    float *x = calloc((size_t)n, sizeof(float));
    for (int i = 0; i < n; i++) x[i] = 0.01f * (float)sin(2 * PI * 440.0 * i / sr); /* quiet tone underneath */
    for (int click = 8000; click < n - 4000; click += 12000)                       /* castanet-like bursts */
        for (int i = 0; i < 1500; i++) x[click + i] += 0.8f * frand() * (float)exp(-i / 300.0);
    AacConfig c = {sr, 1, 96000, 0, 0};
    Aus a;
    AacStats st;
    CHECK(encode_all(&c, x, n, &a, &st));
    CHECK(st.short_frames > 0);
    int shorts = 0;
    float *y = decode_all(sr, 1, &a, &shorts);
    CHECK(y);
    CHECK_INT(shorts, st.short_frames);
    /* pre-echo: from 20 ms to 3.3 ms before each burst (outside the attack's own short window,
       which backward masking covers) the error stays below the quiet tone itself */
    double worst = -99;
    for (int click = 8000; click < n - 4000; click += 12000) {
        double ein = 0, eerr = 0;
        for (int i = click - 960; i < click - 160; i++) {
            double a0 = x[i], b0 = y[i + AAC_PRIMING];
            ein += a0 * a0;
            eerr += (a0 - b0) * (a0 - b0);
        }
        worst = fmax(worst, 10 * log10((eerr + 1e-12) / (ein + 1e-12)));
    }
    if (getenv("AAC_VERBOSE")) printf("    %d short frames, worst pre-echo %.1f dB re the tone\n", st.short_frames, worst);
    CHECK(worst < 0.0); /* the error before a burst stays below the quiet tone itself */
    free(x);
    free(y);
    aus_free(&a);
}

TEST(aac_silence_costs_almost_nothing) {
    int n = 48000;
    float *x = calloc((size_t)n * 2, sizeof(float));
    AacConfig c = {48000, 2, 160000, 0, 0};
    Aus a;
    CHECK(encode_all(&c, x, n, &a, NULL));
    for (int i = 0; i < a.n; i++) CHECK(a.size[i] <= 8);
    float *y = decode_all(48000, 2, &a, NULL);
    CHECK(y);
    for (int i = 0; i < 2 * n; i++) CHECK(y[i] == 0.0f);
    free(x);
    free(y);
    aus_free(&a);
}

TEST(aac_survives_extreme_input) {
    int n = 48000;
    float *x = malloc(sizeof(float) * (size_t)n * 2);
    for (int i = 0; i < 2 * n; i++) {
        int k = i / 2;
        x[i] = (k / 50) % 2 ? 1.0f : -1.0f; /* full-scale square wave */
        if (k % 4001 == 7) x[i] = NAN;
        if (k % 5003 == 9) x[i] = INFINITY;
        if (k % 7001 == 3) x[i] = 1e30f;
        if (k > 40000) x[i] = (k % 2) ? 3.0f : -3.0f; /* way past full scale */
    }
    AacConfig c = {48000, 2, 64000, 0, 1};
    Aus a;
    AacStats st;
    CHECK(encode_all(&c, x, n, &a, &st));
    for (int i = 0; i < a.n; i++) CHECK(a.size[i] > 0 && a.size[i] <= AAC_MAX_AU);
    float *y = decode_all(48000, 2, &a, NULL);
    CHECK(y);
    for (int i = 0; i < 2 * (n + AAC_PRIMING) && i < 2 * AAC_FRAME * a.n; i++) CHECK(isfinite(y[i]) && fabsf(y[i]) < 4.0f);
    free(x);
    free(y);
    aus_free(&a);
}

TEST(aac_mid_side_for_centred_sound) {
    int sr = 48000, n = sr * 2;
    float *m = music(sr, n, 1), *x = malloc(sizeof(float) * (size_t)n * 2);
    for (int i = 0; i < n; i++) x[2 * i] = x[2 * i + 1] = m[i];
    AacConfig c = {sr, 2, 128000, 0, 0};
    Aus a;
    AacStats st;
    CHECK(encode_all(&c, x, n, &a, &st));
    CHECK(st.ms_bands > 0);
    float *y = decode_all(sr, 2, &a, NULL);
    CHECK(y);
    double diff = 0, sig = 0;
    for (int i = AAC_PRIMING; i < n; i++) {
        diff += (y[2 * i] - y[2 * i + 1]) * (double)(y[2 * i] - y[2 * i + 1]);
        sig += (double)y[2 * i] * y[2 * i];
    }
    CHECK(diff < sig * 1e-6); /* a centred source stays centred */
    free(m);
    free(x);
    free(y);
    aus_free(&a);
}

/* Lt/Rt matrixed surround: L = C + S, R = C - S. A Pro Logic decoder steers (L - R)/2 to
   the surround speakers, where nothing masks it - so S must come through on its own. */
TEST(aac_matrixed_surround_survives) {
    int sr = 48000, n = sr * 3;
    float *x = malloc(sizeof(float) * (size_t)n * 2), *s = malloc(sizeof(float) * (size_t)n);
    for (int i = 0; i < n; i++) {
        double t = (double)i / sr;
        double cc = 0.3 * sin(2 * PI * 330 * t) + 0.15 * sin(2 * PI * 1250 * t) + 0.05 * frand();
        double ss = 0.03 * sin(2 * PI * 523 * t) * (0.5 + 0.5 * sin(2 * PI * 0.5 * t)) + 0.01 * frand(); /* -20 dB */
        x[2 * i] = (float)(cc + ss);
        x[2 * i + 1] = (float)(cc - ss);
        s[i] = (float)ss;
    }
    AacConfig c = {sr, 2, 160000, 0, 0};
    Aus a;
    CHECK(encode_all(&c, x, n, &a, NULL));
    float *y = decode_all(sr, 2, &a, NULL);
    CHECK(y);
    double es = 0, ee = 0;
    for (int i = 4096; i < n - 4096; i++) {
        double back = 0.5 * (y[2 * (i + AAC_PRIMING)] - y[2 * (i + AAC_PRIMING) + 1]);
        es += (double)s[i] * s[i];
        ee += (back - s[i]) * (back - s[i]);
    }
    double snr_s = 10 * log10(es / (ee + 1e-20));
    if (getenv("AAC_VERBOSE")) printf("    surround (L-R)/2 comes back at %.1f dB SNR\n", snr_s);
    CHECK(snr_s > 9.0);
    free(x);
    free(s);
    free(y);
    aus_free(&a);
}

TEST(aac_every_rate_and_window_shape) {
    static const int rates_ok[] = {48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025};
    for (int r = 0; r < 8; r++)
        for (int shape = 0; shape < 2; shape++) {
            int sr = rates_ok[r], n = sr;
            float *x = malloc(sizeof(float) * (size_t)n * 2);
            for (int i = 0; i < n; i++) {
                x[2 * i] = 0.4f * (float)sin(2 * PI * 700.0 * i / sr);
                x[2 * i + 1] = 0.3f * (float)sin(2 * PI * 1100.0 * i / sr);
            }
            for (int i = n / 2; i < n / 2 + 200; i++) x[2 * i] += 0.5f * frand(); /* one burst: short windows too */
            AacConfig c = {sr, 2, 96000, 0, shape};
            Aus a;
            AacStats st;
            CHECK(encode_all(&c, x, n, &a, &st));
            float *y = decode_all(sr, 2, &a, NULL);
            CHECK(y);
            double s = snr(x, y + 2 * AAC_PRIMING, 2, 1, 2048, n / 2 - 2048);
            if (getenv("AAC_VERBOSE")) printf("    %5d Hz shape %d: %.1f dB, %d short\n", sr, shape, s, st.short_frames);
            CHECK(s > 25.0);
            CHECK(st.short_frames > 0);
            free(x);
            free(y);
            aus_free(&a);
        }
}

TEST(aac_units_cover_every_sample) {
    AacConfig c = {48000, 1, 64000, 0, 0};
    AacEnc *e = aac_open(&c, NULL, 0);
    static float blk[AAC_FRAME];
    static uint8_t au[AAC_MAX_AU];
    int units = 0;
    for (int b = 0; b < 5; b++) {
        for (int i = 0; i < AAC_FRAME; i++) blk[i] = 0.1f * (float)sin(i * 0.05 + b);
        int n = aac_encode(e, blk, au, sizeof au);
        CHECK(n >= 0);
        units += n > 0;
    }
    CHECK_INT(units, 4); /* one block of look-ahead */
    int n;
    while ((n = aac_flush(e, au, sizeof au)) > 0) units++;
    CHECK_INT(n, 0);
    CHECK_INT(units, 6); /* blocks + 1: every input sample is decodable */
    CHECK_INT(aac_flush(e, au, sizeof au), 0);
    aac_close(e);
}

/* ------------------------------------------------- real media (opt-in) */

static bool read_file_at(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

/* HEMMA_TEST_AC3=<an MKV with AC-3>: up to 60 s of its first AC-3 track, decoded to
   stereo, through the encoder at 160 kbit/s and back. */
TEST(aac_real_media_from_env) {
    const char *path = getenv("HEMMA_TEST_AC3");
    if (!path || !*path) return;
    FILE *fp = file_open(path, "rb");
    CHECK(fp);
    int64_t size = 0;
    path_stat(path, &size, NULL, NULL);
    MkvReader rd = {fp, size, read_file_at};
    char err[200];
    Mkv *m = mkv_open(&rd, err, sizeof err);
    CHECK(m);
    int track = -1;
    for (int i = 0; i < mkv_track_count(m); i++)
        if (strcmp(mkv_track(m, i)->codec, "A_AC3") == 0) {
            track = mkv_track(m, i)->number;
            break;
        }
    CHECK(track > 0);
    Ac3Dec *d = ac3_open();
    ac3_set_layout(d, AC3_LAYOUT_STEREO);
    int cap = 48000 * 60, n = 0, sr = 48000;
    float *pcm = malloc(sizeof(float) * (size_t)cap * 2);
    static float buf[AC3_SAMPLES * AC3_MAX_CHANNELS];
    MkvFrame f;
    while (n + AC3_SAMPLES <= cap && mkv_next(m, &f) == 1) {
        if (f.track != track) continue;
        int ch = 0;
        Ac3Info info;
        if (ac3_decode(d, f.data, f.size, buf, &ch, &info) != AC3_OK || ch != 2) continue;
        sr = info.sample_rate;
        memcpy(pcm + 2 * (size_t)n, buf, sizeof(float) * AC3_SAMPLES * 2);
        n += AC3_SAMPLES;
    }
    CHECK(n > sr);
    AacConfig c = {sr, 2, 160000, 0, 0};
    Aus a;
    AacStats st;
    CHECK(encode_all(&c, pcm, n, &a, &st));
    float *y = decode_all(sr, 2, &a, NULL);
    CHECK(y);
    double s0 = snr(pcm, y + 2 * AAC_PRIMING, 2, 0, 0, n), seg = seg_snr(pcm, y + 2 * AAC_PRIMING, 2, 0, n);
    printf("    %s: %.1f s, %.1f dB SNR, %.1f dB segmental, %.0f kbit/s, %d of %d frames short, %d M/S bands\n", path,
           (double)n / sr, s0, seg, (double)st.bits_total * sr / AAC_FRAME / a.n / 1000, st.short_frames, st.frames,
           st.ms_bands);
    CHECK(s0 > 10.0);
    free(pcm);
    free(y);
    aus_free(&a);
    ac3_close(d);
    mkv_close(m);
    fclose(fp);
}
