/* tools/avfuzz.c — six fuzz targets that each reach one part of modules/av directly, many times
 * faster than tools/convfuzz.c's whole conversion (which encodes every picture it decodes), so the
 * nightly run (.github/workflows/fuzz.yml) goes deeper into each (issue #50). Pick one when building:
 *
 *   -DAVFUZZ_MKV     the Matroska reader: headers, tracks, every frame, a seek, a rewind
 *   -DAVFUZZ_AC3     the Dolby decoder: raw AC-3 / E-AC-3 frames, with the CRC check off
 *   -DAVFUZZ_DTS     the DTS decoder (modules/av/dts.c): raw core frames
 *   -DAVFUZZ_MPEG2   the MPEG-2 decoder: a raw video stream, fed in chunks of any size
 *   -DAVFUZZ_PGS     Blu-ray picture subtitles (modules/av/subpic.c): a .sup opened, every cue drawn and
 *                    made a PNG (png.c)
 *   -DAVFUZZ_VOBSUB  DVD picture subtitles: the input's bytes up to its first NUL are the .idx text, the rest
 *                    the .sub; opened, every cue drawn and made a PNG
 *
 * For mkv, ac3, dts and mpeg2 the first byte of an input chooses the target's options (a layout, a chunk size,
 * a reset part-way), so the fuzzer explores those too. Seeds: `HEMMA_FUZZ_SEEDS=build/fuzz-seeds
 * HEMMA_FUZZ_RAW=build/fuzz-raw build/test conv_fuzz_seeds` - the MKVs for mkv (as for convfuzz),
 * build/fuzz-raw/ac3, .../dts and .../mpeg2; `HEMMA_FUZZ_RAW=build/fuzz-raw build/test subpic_fuzz_seeds` -
 * build/fuzz-raw/pgs and .../vobsub.
 *
 *   clang -g -O1 -std=gnu11 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
 *         -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -DAVFUZZ_AC3 tools/avfuzz.c modules/av/ac3.c -lm -o build/fuzz-ac3
 *   (mkv: modules/av/mkv.c; dts: modules/av/dts.c; mpeg2: modules/av/mpeg2.c; pgs, vobsub: modules/av/subpic.c modules/av/png.c)
 *   build/fuzz-ac3 -max_total_time=60 build/fuzz-corpus-ac3 build/fuzz-raw/ac3
 *
 * A finding is a sanitizer report (out of bounds, use after free, undefined behaviour, a leak) or a
 * hang. Every byte a decoder hands back is read here, so a frame or picture pointing past its buffer
 * shows up as one; the subtitle targets also abort when a promise of subpic.h is broken (a cue off its
 * frame, out of order, a pixel past its colour count). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(AVFUZZ_MKV) + defined(AVFUZZ_AC3) + defined(AVFUZZ_DTS) + defined(AVFUZZ_MPEG2) + defined(AVFUZZ_PGS) +         defined(AVFUZZ_VOBSUB) != 1
#error "build with exactly one of -DAVFUZZ_MKV, -DAVFUZZ_AC3, -DAVFUZZ_DTS, -DAVFUZZ_MPEG2, -DAVFUZZ_PGS, -DAVFUZZ_VOBSUB"
#endif

static volatile uint32_t g_sink; /* what was read: kept, so no read is optimised away */

static void touch(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s = s * 31 + p[i];
    g_sink += s;
}

#ifdef AVFUZZ_MKV
#include "../modules/av/mkv.h"

typedef struct {
    const uint8_t *p;
    size_t n;
} In;

static bool in_read(void *ctx, int64_t off, void *dst, size_t n) {
    const In *in = ctx;
    if (off < 0 || (uint64_t)off > in->n || n > in->n - (size_t)off) return false;
    if (n) memcpy(dst, in->p + off, n);
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    In in = {d, n};
    MkvReader r = {&in, (int64_t)n, in_read};
    char err[160];
    Mkv *m = mkv_open(&r, err, sizeof err);
    if (!m) return 0;
    touch((const uint8_t *)mkv_doctype(m), strlen(mkv_doctype(m)));
    touch((const uint8_t *)mkv_title(m), strlen(mkv_title(m)));
    touch((const uint8_t *)mkv_writing_app(m), strlen(mkv_writing_app(m)));
    for (int i = 0; i < mkv_track_count(m); i++) {
        const MkvTrack *t = mkv_track(m, i);
        touch(t->priv, t->priv_len);
        touch(t->strip, t->strip_len);
        touch((const uint8_t *)t->codec, strlen(t->codec));
        touch((const uint8_t *)t->lang, strlen(t->lang));
        touch((const uint8_t *)t->name, strlen(t->name));
    }
    MkvFrame f;
    for (int k = 0; k < 4000 && mkv_next(m, &f) == 1; k++) touch(f.data, f.size);
    if (mkv_seek(m, mkv_duration(m) / 2, 0))
        for (int k = 0; k < 200 && mkv_next(m, &f) == 1; k++) touch(f.data, f.size);
    mkv_rewind(m);
    if (mkv_next(m, &f) == 1) touch(f.data, f.size);
    mkv_close(m);
    return 0;
}
#endif

#ifdef AVFUZZ_AC3
#include "../modules/av/ac3.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 2) return 0;
    Ac3Dec *dec = ac3_open();
    if (!dec) return 0;
    uint8_t opt = d[0];
    ac3_set_crc_check(dec, false); /* (mutated frames reach the decoder; a crafted file can carry good CRCs anyway) */
    ac3_set_layout(dec, (Ac3Layout)(opt % 3));
    ac3_set_drc(dec, opt & 4 ? 0.0f : 1.0f);
    static float out[AC3_SAMPLES * AC3_MAX_CHANNELS];
    const uint8_t *p = d + 1;
    size_t left = n - 1;
    for (int k = 0; k < 256 && left >= AC3_HEADER_BYTES; k++) {
        long at = ac3_find_frame(p, left, true);
        if (at < 0) break;
        p += at, left -= (size_t)at;
        int size = ac3_frame_size(p, left);
        if (size <= 0 || (size_t)size > left) { /* (a header find_frame took: skip a byte) */
            p++, left--;
            continue;
        }
        /* sometimes a frame cut short: the decoder must notice, not read on */
        size_t give = opt & 8 && k == 1 ? (size_t)size / 2 : (size_t)size;
        int ch = 0;
        Ac3Info info;
        int rc = ac3_decode(dec, p, give, out, &ch, &info);
        if ((rc == AC3_OK || rc == AC3_ERR_CRC || rc == AC3_ERR_BITSTREAM) && ch > 0 && ch <= AC3_MAX_CHANNELS &&
            info.samples > 0 && info.samples <= AC3_SAMPLES)
            touch((const uint8_t *)out, (size_t)info.samples * (size_t)ch * sizeof out[0]);
        if (opt & 16 && k == 3) ac3_reset(dec); /* (a seek) */
        p += size, left -= (size_t)size;
    }
    ac3_close(dec);
    return 0;
}
#endif

#ifdef AVFUZZ_DTS
#include "../modules/av/dts.h"

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 2) return 0;
    DtsDec *dec = dts_open();
    if (!dec) return 0;
    uint8_t opt = d[0];
    dts_set_layout(dec, (DtsLayout)(opt % 3));
    dts_set_drc(dec, opt & 4 ? 1.0f : 0.0f);
    static float out[DTS_SAMPLES * DTS_MAX_CHANNELS];
    const uint8_t *p = d + 1;
    size_t left = n - 1;
    /* (32 frames: a 100-byte frame may say 4096 samples on 6 channels - more frames only slow the fuzzer) */
    for (int k = 0; k < 32 && left >= DTS_HEADER_BYTES; k++) {
        long at = dts_find_frame(p, left, true);
        if (at < 0) break;
        p += at, left -= (size_t)at;
        int size = dts_frame_size(p, left);
        if (size <= 0 || (size_t)size > left) { /* (a header find_frame took: skip a byte) */
            p++, left--;
            continue;
        }
        /* sometimes a frame cut short: the decoder must notice, not read on */
        size_t give = opt & 8 && k == 1 ? (size_t)size / 2 : (size_t)size;
        int ch = 0;
        DtsInfo info;
        int rc = dts_decode(dec, p, give, out, &ch, &info);
        if ((rc == DTS_OK || rc == DTS_ERR_BITSTREAM) && ch > 0 && ch <= DTS_MAX_CHANNELS &&
            info.samples > 0 && info.samples <= DTS_SAMPLES)
            touch((const uint8_t *)out, (size_t)info.samples * (size_t)ch * sizeof out[0]);
        if (opt & 16 && k == 3) dts_reset(dec); /* (a seek) */
        p += size, left -= (size_t)size;
    }
    dts_close(dec);
    return 0;
}
#endif

#ifdef AVFUZZ_MPEG2
#include "../modules/av/mpeg2.h"

#define MAX_PIXELS (720 * 576) /* bigger is right, just slow under sanitizers */

/* A sequence header asking for more than MAX_PIXELS anywhere in the bytes */
static bool too_big(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 7 <= n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1 || p[i + 3] != 0xB3) continue;
        unsigned w = (unsigned)p[i + 4] << 4 | p[i + 5] >> 4, h = (unsigned)(p[i + 5] & 15) << 8 | p[i + 6];
        if ((unsigned long)w * h > MAX_PIXELS) return true;
    }
    return false;
}

static void drain(Mpeg2 *m) {
    Mpeg2Frame f;
    while (mpeg2_frame(m, &f)) {
        if (f.width <= 0 || f.height <= 0) continue;
        /* the last row of each plane: a frame pointing past its buffer shows here */
        int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
        touch(f.y + (size_t)(f.height - 1) * (size_t)f.stride_y, (size_t)f.width);
        touch(f.u + (size_t)(ch - 1) * (size_t)f.stride_c, (size_t)cw);
        touch(f.v + (size_t)(ch - 1) * (size_t)f.stride_c, (size_t)cw);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 2 || too_big(d, n)) return 0;
    Mpeg2 *m = mpeg2_new();
    if (!m) return 0;
    uint8_t opt = d[0];
    size_t chunk = opt & 128 ? n : 16 + (size_t)(opt & 127) * 8; /* whole, or pieces cut anywhere */
    bool reset = opt & 1, done_reset = false;
    for (size_t at = 1; at < n;) {
        size_t k = n - at < chunk ? n - at : chunk;
        if (mpeg2_decode(m, d + at, k, (int64_t)at) < 0) break;
        drain(m);
        at += k;
        if (reset && !done_reset && at > n / 2) mpeg2_reset(m), done_reset = true; /* (a seek) */
    }
    mpeg2_flush(m);
    drain(m);
    mpeg2_free(m);
    return 0;
}
#endif

#if defined(AVFUZZ_PGS) || defined(AVFUZZ_VOBSUB)
#include "../modules/av/subpic.h"

#include <stdlib.h>

#define PIC_BUDGET (1u << 21) /* pixels drawn per input: bigger is right, just slow under sanitizers */

typedef struct {
    const uint8_t *p;
    size_t n;
} SubIn;

static bool sub_read(void *ctx, int64_t off, void *dst, size_t n) {
    const SubIn *in = ctx;
    if (off < 0 || (uint64_t)off > in->n || n > in->n - (size_t)off) return false;
    if (n) memcpy(dst, in->p + off, n);
    return true;
}

/* Every cue checked against subpic.h's promises, drawn (within the budget) and made a PNG */
static void every_cue(SubPic *s) {
    int fw = 0, fh = 0, n = subpic_count(s);
    subpic_frame(s, &fw, &fh);
    if (fw < 1 || fh < 1 || fw > SUBPIC_MAX_SIDE || fh > SUBPIC_MAX_SIDE || n < 0 || n > SUBPIC_MAX_CUES ||
        subpic_cue(s, -1) || subpic_cue(s, n))
        abort();
    size_t budget = PIC_BUDGET;
    int64_t prev = 0;
    for (int i = 0; i < n; i++) {
        const SubPicCue *c = subpic_cue(s, i);
        if (!c || c->start_ms < prev || c->end_ms <= c->start_ms || c->x < 0 || c->y < 0 || c->w < 1 || c->h < 1 ||
            c->x + c->w > fw || c->y + c->h > fh)
            abort();
        prev = c->start_ms;
        size_t px_n = (size_t)c->w * (size_t)c->h;
        if (px_n > budget) continue;
        budget -= px_n;
        uint8_t *px = malloc(px_n);
        uint32_t pal[256];
        int nc = 0;
        if (px && subpic_render(s, i, px, px_n, pal, &nc)) {
            if (nc < 1 || nc > 256) abort();
            for (size_t k = 0; k < px_n; k++)
                if (px[k] >= nc) abort();
            touch((const uint8_t *)pal, (size_t)nc * sizeof pal[0]);
            size_t len = 0;
            uint8_t *png = png_indexed(px, c->w, c->h, pal, nc, &len);
            if (png) touch(png, len);
            free(png);
        }
        free(px);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    char err[160];
#ifdef AVFUZZ_PGS
    SubIn in = {d, n};
    MkvReader r = {&in, (int64_t)n, sub_read};
    SubPic *s = subpic_open_sup(&r, err, sizeof err);
#else
    const uint8_t *z = memchr(d, 0, n); /* idx text, NUL, the .sub */
    size_t ni = z ? (size_t)(z - d) : n;
    SubIn in = {z ? z + 1 : d + n, z ? n - ni - 1 : 0};
    MkvReader r = {&in, (int64_t)in.n, sub_read};
    SubPic *s = subpic_open_vobsub((const char *)d, ni, &r, err, sizeof err);
#endif
    if (!s) {
        touch((const uint8_t *)err, strlen(err));
        return 0;
    }
    every_cue(s);
    subpic_close(s);
    return 0;
}
#endif
