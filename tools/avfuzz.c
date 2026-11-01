/* tools/avfuzz.c — fuzz targets that each reach one part of modules/av directly, many times
 * faster than tools/convfuzz.c's whole conversion (which encodes every picture it decodes), so the
 * nightly run (.github/workflows/fuzz.yml) goes deeper into each (issue #50). Pick one when building:
 *
 *   -DAVFUZZ_MKV     the Matroska reader: headers, tracks, every frame, a seek, a rewind
 *   -DAVFUZZ_MP4     (2.5) the same calls on an MP4/MOV (mkv.c's mkv_mp4.h): boxes, sample tables, every frame - each
 *                    one inside the input, an MP4 saying it is one
 *   -DAVFUZZ_AC3     the Dolby decoder: raw AC-3 / E-AC-3 frames, with the CRC check off
 *   -DAVFUZZ_DTS     the DTS decoder (modules/av/dts.c): raw core frames
 *   -DAVFUZZ_MPEG2   the MPEG-2 decoder: a raw video stream, fed in chunks of any size
 *   -DAVFUZZ_MPEG4   (2.5) the MPEG-4 Part 2 decoder (DivX, Xvid): a raw stream, fed a VOP a block, whole or in
 *                    pieces, its block times in or out of order
 *   -DAVFUZZ_HEVC    (2.6) the HEVC decoder: an Annex B stream in pieces of any size, or its NAL units as a
 *                    container's blocks after an hvcC made of its parameter sets
 *   -DAVFUZZ_PGS    Blu-ray picture subtitles (modules/av/subpic.c): a .sup opened, every cue drawn and
 *                    made a PNG (png.c)
 *   -DAVFUZZ_VOBSUB  DVD picture subtitles: the input's bytes up to its first NUL are the .idx text, the rest
 *                    the .sub; opened, every cue drawn and made a PNG
 *   -DAVFUZZ_TAGS    an audio file's tags (modules/av/audiotags.c): an MP4's boxes or an MP3's ID3 tags and
 *                    frames read, every string and chapter looked at
 *
 * For mkv, ac3, dts, mpeg2, mpeg4 and hevc the first byte of an input chooses the target's options (a layout, a chunk
 * size, a reset part-way), for tags the file's name (none, x.mp3, x.m4b, book.MP3), so the fuzzer explores those too.
 * Seeds: `HEMMA_FUZZ_SEEDS=build/fuzz-seeds HEMMA_FUZZ_RAW=build/fuzz-raw build/test conv_fuzz_seeds` - the MKVs
 * for mkv (as for convfuzz), build/fuzz-raw/ac3, .../dts, .../mpeg2, .../mpeg4 and .../hevc; `HEMMA_FUZZ_RAW=build/fuzz-raw build/test
 * subpic_fuzz_seeds` - build/fuzz-raw/pgs and .../vobsub; `HEMMA_FUZZ_RAW=build/fuzz-raw build/test
 * audiotags_fuzz_seeds` - build/fuzz-raw/tags; `HEMMA_FUZZ_RAW=build/fuzz-raw build/test mp4read_fuzz_seeds` -
 * build/fuzz-raw/mp4.
 *
 *   clang -g -O1 -std=gnu11 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
 *         -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -DAVFUZZ_AC3 tools/avfuzz.c modules/av/ac3.c -lm -o build/fuzz-ac3
 *   (mkv, mp4: modules/av/mkv.c; dts: modules/av/dts.c; mpeg2: modules/av/mpeg2.c; mpeg4: modules/av/mpeg4.c;
 *    hevc: modules/av/hevc.c hevc_ctu.c hevc_dsp.c; pgs, vobsub: modules/av/subpic.c modules/av/png.c;
 *    tags: modules/av/audiotags.c)
 *   build/fuzz-ac3 -max_total_time=60 build/fuzz-corpus-ac3 build/fuzz-raw/ac3
 *
 * A finding is a sanitizer report (out of bounds, use after free, undefined behaviour, a leak) or a
 * hang. Every byte a decoder hands back is read here, so a frame or picture pointing past its buffer
 * shows up as one; the subtitle targets also abort when a promise of subpic.h is broken (a cue off its
 * frame, out of order, a pixel past its colour count), and tags when one of audiotags.h is (text that isn't
 * valid UTF-8 or holds control characters, chapters out of order, a cover past the file's end or not a
 * JPEG/PNG there). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(AVFUZZ_MKV) + defined(AVFUZZ_MP4) + defined(AVFUZZ_AC3) + defined(AVFUZZ_DTS) + defined(AVFUZZ_MPEG2) + defined(AVFUZZ_MPEG4) + defined(AVFUZZ_HEVC) + defined(AVFUZZ_PGS) + defined(AVFUZZ_VOBSUB) + defined(AVFUZZ_TAGS) != 1
#error "build with exactly one of -DAVFUZZ_MKV, -DAVFUZZ_MP4, -DAVFUZZ_AC3, -DAVFUZZ_DTS, -DAVFUZZ_MPEG2, -DAVFUZZ_MPEG4, -DAVFUZZ_HEVC, -DAVFUZZ_PGS, -DAVFUZZ_VOBSUB, -DAVFUZZ_TAGS"
#endif

static volatile uint32_t g_sink; /* what was read: kept, so no read is optimised away */

static void touch(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s = s * 31 + p[i];
    g_sink += s;
}

#if defined(AVFUZZ_MKV) || defined(AVFUZZ_MP4)
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
#ifdef AVFUZZ_MP4
    bool mp4 = !strcmp(mkv_doctype(m), "mp4") || !strcmp(mkv_doctype(m), "mov");
#endif
    for (int i = 0; i < mkv_track_count(m); i++) {
        const MkvTrack *t = mkv_track(m, i);
        touch(t->priv, t->priv_len);
        touch(t->strip, t->strip_len);
        touch((const uint8_t *)t->codec, strlen(t->codec));
        touch((const uint8_t *)t->lang, strlen(t->lang));
        touch((const uint8_t *)t->name, strlen(t->name));
        touch((const uint8_t *)t->fourcc, strlen(t->fourcc));
#ifdef AVFUZZ_MP4
        if (mp4 && (!t->type || !t->codec[0] || strlen(t->lang) != 3)) __builtin_trap(); /* (a track it hands out has these) */
#endif
    }
    MkvFrame f;
    for (int k = 0; k < 4000 && mkv_next(m, &f) == 1; k++) {
        touch(f.data, f.size);
#ifdef AVFUZZ_MP4
        if (mp4 && (f.size > n || (f.size && !f.data) || f.pos < 0 || (uint64_t)f.pos + f.size > n)) __builtin_trap();
#endif
    }
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

#ifdef AVFUZZ_MPEG4
#include "../modules/av/mpeg4.h"

#include <stdlib.h>

static void drain4(Mpeg4 *m) {
    Mpeg4Frame f;
    while (mpeg4_frame(m, &f)) {
        if (f.width <= 0 || f.height <= 0) abort(); /* (a frame comes only from a VOL's size) */
        int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
        touch(f.y + (size_t)(f.height - 1) * (size_t)f.stride_y, (size_t)f.width);
        touch(f.u + (size_t)(ch - 1) * (size_t)f.stride_c, (size_t)cw);
        touch(f.v + (size_t)(ch - 1) * (size_t)f.stride_c, (size_t)cw);
    }
}

/* The next VOP's start code (00 00 01 B6) at or after `from` - a block's end when blocks are VOPs - or n */
static size_t next_vop(const uint8_t *p, size_t n, size_t from) {
    for (size_t i = from; i + 4 <= n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1 && p[i + 3] == 0xB6) return i;
    return n;
}

/* Options byte: & 3 = how blocks are cut (0, 1: each VOP its own, as Matroska and MP4 keep them - the headers in
   front go with the first; 2: the whole; 3: pieces of 16 + (& 0x3C) bytes cut anywhere); & 64 = a reset part-way
   (a seek); & 128 = the block times in reverse (B-VOPs, packed blocks: the times sort themselves). */
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 2) return 0;
    Mpeg4 *m = mpeg4_new();
    if (!m) return 0;
    uint8_t opt = d[0];
    bool reset = opt & 64, done_reset = false;
    int64_t t = opt & 128 ? (int64_t)n : 0;
    for (size_t at = 1; at < n;) {
        size_t end = (opt & 3) < 2 ? next_vop(d, n, next_vop(d, n, at) + 4) : (opt & 3) == 2 ? n : at + 16 + (opt & 0x3C);
        if (end > n) end = n;
        if (mpeg4_decode(m, d + at, end - at, opt & 128 ? t-- : t++) < 0) break;
        drain4(m);
        at = end;
        if (reset && !done_reset && at > n / 2) mpeg4_reset(m), done_reset = true;
    }
    mpeg4_flush(m);
    drain4(m);
    mpeg4_free(m);
    return 0;
}
#endif

#ifdef AVFUZZ_HEVC
#include "../modules/av/hevc.h"

#include <stdlib.h>

static void drain5(Hevc *h) {
    HevcFrame f;
    while (hevc_frame(h, &f)) {
        if (f.width <= 0 || f.height <= 0 || f.bit_depth < 8 || f.bit_depth > 10) abort(); /* (Main 10: 8, 9 or 10 bits) */
        int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
        touch(f.y + (size_t)(f.height - 1) * (size_t)f.stride_y, (size_t)f.width);
        touch(f.u + (size_t)(ch - 1) * (size_t)f.stride_c, (size_t)cw);
        touch(f.v + (size_t)(ch - 1) * (size_t)f.stride_c, (size_t)cw);
        touch((const uint8_t *)(f.y16 + (size_t)(f.height - 1) * (size_t)f.stride16_y), (size_t)f.width * 2);
        touch((const uint8_t *)(f.v16 + (size_t)(ch - 1) * (size_t)f.stride16_c), (size_t)cw * 2);
    }
}

static size_t next_sc(const uint8_t *p, size_t n, size_t from) { /* the next 00 00 01 at or after `from`, else n */
    for (size_t i = from; i + 3 <= n; i++)
        if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i;
    return n;
}

/* Options byte: & 3 = how it's fed (0, 1: Annex B in pieces of 1 + (& 0x3C) bytes; 2: the whole stream at once; 3: as
   a container keeps it - an hvcC holding the VPS/SPS/PPS units found, then each other unit a block of its own with a
   4-byte length); & 64 = a reset part-way (a seek); & 128 = the block times in reverse. */
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 2) return 0;
    Hevc *h = hevc_new();
    if (!h) return 0;
    uint8_t opt = d[0];
    bool reset = opt & 64, done_reset = false;
    int64_t t = opt & 128 ? (int64_t)n : 0;
    if ((opt & 3) == 3) {
        static uint8_t cfg[1 << 16], blk[1 << 16];
        memset(cfg, 0, 23);
        cfg[0] = 1, cfg[21] = 0x0f; /* 4-byte lengths */
        size_t c = 23;
        for (size_t s = next_sc(d, n, 1); s < n;) { /* the parameter sets, each an array of one */
            size_t e = next_sc(d, n, s + 3), len = e - s - 3;
            int type = s + 3 < n ? (d[s + 3] >> 1) & 63 : 0;
            if (type >= 32 && type <= 34 && len <= 0xffff && c + 5 + len <= sizeof cfg && cfg[22] < 255) {
                cfg[c++] = (uint8_t)(0x80 | type), cfg[c++] = 0, cfg[c++] = 1;
                cfg[c++] = (uint8_t)(len >> 8), cfg[c++] = (uint8_t)len;
                memcpy(cfg + c, d + s + 3, len), c += len;
                cfg[22]++;
            }
            s = e;
        }
        hevc_config(h, cfg, c);
        for (size_t s = next_sc(d, n, 1); s < n;) {
            size_t e = next_sc(d, n, s + 3), len = e - s - 3;
            int type = s + 3 < n ? (d[s + 3] >> 1) & 63 : 0;
            if ((type < 32 || type > 34) && len + 4 <= sizeof blk) {
                blk[0] = (uint8_t)(len >> 24), blk[1] = (uint8_t)(len >> 16), blk[2] = (uint8_t)(len >> 8), blk[3] = (uint8_t)len;
                memcpy(blk + 4, d + s + 3, len);
                if (hevc_decode(h, blk, len + 4, opt & 128 ? t-- : t++) < 0) break;
                drain5(h);
            }
            s = e;
            if (reset && !done_reset && s > n / 2) hevc_reset(h), done_reset = true;
        }
    } else {
        for (size_t at = 1; at < n;) {
            size_t end = (opt & 3) == 2 ? n : at + 1 + (opt & 0x3C);
            if (end > n) end = n;
            if (hevc_decode(h, d + at, end - at, opt & 128 ? t-- : t++) < 0) break;
            drain5(h);
            at = end;
            if (reset && !done_reset && at > n / 2) hevc_reset(h), done_reset = true;
        }
    }
    hevc_flush(h);
    drain5(h);
    hevc_free(h);
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

#ifdef AVFUZZ_TAGS
#include "../modules/av/audiotags.h"

#include <stdlib.h>

typedef struct {
    const uint8_t *p;
    size_t n;
} TagIn;

static bool tag_read(void *ctx, int64_t off, void *dst, size_t n) {
    const TagIn *in = ctx;
    if (off < 0 || (uint64_t)off > in->n || n > in->n - (size_t)off) return false;
    if (n) memcpy(dst, in->p + off, n);
    return true;
}

/* A string as audiotags.h promises: ends inside its array, valid UTF-8, no control characters (but a
   description's line breaks), no blank at either end - aborts otherwise */
static void text_ok(const char *s, size_t cap, bool lines) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    if (n == cap || (n && (s[0] == ' ' || s[0] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\n'))) abort();
    for (size_t i = 0; i < n;) {
        uint8_t b = (uint8_t)s[i];
        size_t k = b < 0x80 ? 0 : b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 : b >= 0xF0 && b <= 0xF4 ? 3 : 9;
        if ((b < 0x20 && !(lines && b == '\n')) || b == 0x7F || k == 9 || n - i <= k) abort();
        uint32_t cp = k ? b & (0x3Fu >> k) : b;
        for (size_t j = 1; j <= k; j++) {
            if (((uint8_t)s[i + j] & 0xC0) != 0x80) abort();
            cp = cp << 6 | ((uint8_t)s[i + j] & 0x3Fu);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000) ||
            (cp >= 0x80 && cp < 0xA0))
            abort();
        i += k + 1;
    }
    touch((const uint8_t *)s, n);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static const char *const HINTS[4] = {NULL, "x.mp3", "x.m4b", "book.MP3"};
    if (n < 1) return 0;
    TagIn in = {d + 1, n - 1};
    AtReader r = {&in, (int64_t)in.n, tag_read};
    AudioTags t;
    bool ok = audio_tags_read(&r, HINTS[d[0] & 3], &t);
    if (ok != (t.format[0] != 0)) abort();
    text_ok(t.format, sizeof t.format, false), text_ok(t.codec, sizeof t.codec, false);
    text_ok(t.title, sizeof t.title, false), text_ok(t.artist, sizeof t.artist, false);
    text_ok(t.album_artist, sizeof t.album_artist, false), text_ok(t.album, sizeof t.album, false);
    text_ok(t.narrator, sizeof t.narrator, false), text_ok(t.genre, sizeof t.genre, false);
    text_ok(t.cover_type, sizeof t.cover_type, false);
    if (t.description) {
        if (!t.description[0]) abort();
        text_ok(t.description, AT_DESC_MAX, true);
    }
    if (t.duration_ms < 0 || t.channels < 0 || t.sample_rate < 0 || t.year < 0 || t.year > 9999) abort();
    if (t.nchapters < 0 || t.nchapters > AT_MAX_CHAPTERS || !t.nchapters != !t.chapters) abort();
    for (int i = 0; i < t.nchapters; i++) {
        if (t.chapters[i].start_ms < 0 || (i && t.chapters[i].start_ms < t.chapters[i - 1].start_ms)) abort();
        text_ok(t.chapters[i].title, sizeof t.chapters[i].title, false);
    }
    if (t.cover_len) { /* its place: in the file, and a JPEG's or PNG's first bytes there */
        const uint8_t *c = in.p + t.cover_off;
        bool jpeg = !strcmp(t.cover_type, "image/jpeg"), png = !strcmp(t.cover_type, "image/png");
        if (t.cover_len < 0 || t.cover_off < 0 || t.cover_off > r.size - t.cover_len || (!jpeg && !png) ||
            (jpeg && (t.cover_len < 2 || c[0] != 0xFF || c[1] != 0xD8)) ||
            (png && (t.cover_len < 4 || c[0] != 0x89 || c[1] != 'P')))
            abort();
    } else if (t.cover_off || t.cover_type[0]) abort();
    audio_tags_free(&t);
    return 0;
}
#endif
