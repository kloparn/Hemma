/* tools/avfuzz.c — three fuzz targets that each reach one part of modules/av directly, many times
 * faster than tools/convfuzz.c's whole conversion (which encodes every picture it decodes), so the
 * nightly run (.github/workflows/fuzz.yml) goes deeper into each (issue #50). Pick one when building:
 *
 *   -DAVFUZZ_MKV     the Matroska reader: headers, tracks, every frame, a seek, a rewind
 *   -DAVFUZZ_AC3     the Dolby decoder: raw AC-3 / E-AC-3 frames, with the CRC check off
 *   -DAVFUZZ_MPEG2   the MPEG-2 decoder: a raw video stream, fed in chunks of any size
 *
 * The first byte of an input chooses the target's options (a layout, a chunk size, a reset part-way),
 * so the fuzzer explores those too. Seeds: `HEMMA_FUZZ_SEEDS=build/fuzz-seeds HEMMA_FUZZ_RAW=build/fuzz-raw
 * build/test conv_fuzz_seeds` - the MKVs for mkv (as for convfuzz), build/fuzz-raw/ac3 and .../mpeg2.
 *
 *   clang -g -O1 -std=gnu11 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
 *         -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -DAVFUZZ_AC3 tools/avfuzz.c modules/av/ac3.c -lm -o build/fuzz-ac3
 *   (mkv: modules/av/mkv.c; mpeg2: modules/av/mpeg2.c)
 *   build/fuzz-ac3 -max_total_time=60 build/fuzz-corpus-ac3 build/fuzz-raw/ac3
 *
 * A finding is a sanitizer report (out of bounds, use after free, undefined behaviour, a leak) or a
 * hang. Every byte a decoder hands back is read here, so a frame or picture pointing past its buffer
 * shows up as one. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(AVFUZZ_MKV) + defined(AVFUZZ_AC3) + defined(AVFUZZ_MPEG2) != 1
#error "build with exactly one of -DAVFUZZ_MKV, -DAVFUZZ_AC3, -DAVFUZZ_MPEG2"
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
