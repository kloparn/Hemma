/* modules/av/hevc.h — HEVC (H.265) video decoder.
 *
 * ITU-T H.265 Main and Main 10 profiles: 4:2:0, 8 or 10 bits - what x265
 * releases, phones and streaming services carry (an MKV's V_MPEGH/ISO/HEVC, an
 * MP4's 'hvc1'/'hev1'). I-, P- and B-slices; CABAC; coding, prediction and
 * transform quadtrees with every partition (AMP too); the 35 intra modes with
 * reference smoothing (strong too); merge and AMVP motion with temporal vectors,
 * 8-tap luma and 4-tap chroma interpolation, explicit weighted prediction; the
 * DCT 4-32 and DST, transform skip, scaling lists, sign hiding, transquant
 * bypass, PCM; deblocking and SAO; tiles, wavefronts (WPP), dependent slice
 * segments; short- and long-term reference sets, reordering, CRA/BLA/RASL. The
 * output is exact: every sample as the standard says (checked against FFmpeg -
 * see hevc_test.c).
 * Not decoded - hevc_unsupported() says which: 4:2:2, 4:4:4 and monochrome,
 * more than 10 bits, the range/screen-content/multilayer extensions, and
 * interlaced material sent as fields. Layers above the base are ignored.
 *
 * Feed it one access unit at a time - in Matroska and MP4 a block - after
 * hevc_config() with the track's CodecPrivate / hvcC (the NAL length size and
 * the parameter sets). Without hevc_config() the input is an Annex B byte
 * stream (start codes) in pieces of any size; a picture is then complete when the
 * next one starts, or at hevc_flush(). Frames come out in display order, each
 * with its own block's time (Matroska and MP4 times are presentation times).
 * After a seek call hevc_reset(): decoding resumes at the next random access
 * picture. Damage is never fatal: a slice that can't be read leaves its area as
 * it was predicted (or grey), and the picture is counted as damaged.
 *
 * Pure library: libc only (no core/), no mutable globals, no OS calls — every
 * decoder instance is independent. Builds for x86-64, aarch64/armv6 and wasm32. */
#ifndef HEMMA_AV_HEVC_H
#define HEMMA_AV_HEVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEVC_NOPTS INT64_MIN

typedef struct {
    const uint8_t *y, *u, *v;       /* 8-bit planes, 4:2:0 (9- and 10-bit pictures rounded to 8) */
    int stride_y, stride_c;
    const uint16_t *y16, *u16, *v16; /* the decoded samples at their own depth (strides in samples) */
    int stride16_y, stride16_c;
    int width, height;              /* the conformance window: what's shown */
    int bit_depth;                  /* 8, 9 or 10 (Main 10 allows 9 too) */
    int par_num, par_den;           /* pixel aspect ratio (1:1 when the stream gives none) */
    int rate_num, rate_den;         /* the VUI's timing (time_scale / num_units_in_tick), 0/0 when none */
    char type;                      /* 'I', 'P' or 'B': the picture's first slice */
    int64_t pts;                    /* its block's time, HEVC_NOPTS when it had none */
    int damaged;                    /* slices that couldn't be read whole (0 = a clean picture) */
} HevcFrame;

typedef struct Hevc Hevc;

Hevc *hevc_new(void);
void hevc_free(Hevc *d);

/* The track's hvcC (HEVCDecoderConfigurationRecord): the NAL length size, then its VPS/SPS/PPS/SEI.
   false = not a record this decoder understands (decoding then expects Annex B). */
bool hevc_config(Hevc *d, const uint8_t *data, size_t n);
/* Decode an access unit (or, without hevc_config, any piece of an Annex B stream). Returns the number of
   frames now waiting in display order, -1 on out-of-memory. */
int hevc_decode(Hevc *d, const uint8_t *data, size_t n, int64_t pts);
/* The next frame in display order. Its planes stay valid until the next call to hevc_decode / hevc_frame /
   hevc_flush / hevc_reset / hevc_free. */
bool hevc_frame(Hevc *d, HevcFrame *out);
void hevc_flush(Hevc *d); /* end of stream: every picture still held comes out */
void hevc_reset(Hevc *d); /* after a seek: forget pictures and wait for a random access point */

/* Stream facts once a sequence parameter set was seen (false before): the shown size. */
bool hevc_info(const Hevc *d, int *width, int *height);
/* A tool the stream uses that this decoder doesn't ("4:2:2 chroma", "12-bit samples" ...), NULL while there's
   none: its pictures aren't decoded. */
const char *hevc_unsupported(const Hevc *d);

#endif
