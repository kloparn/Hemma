/* modules/av/mpeg4.h — MPEG-4 Part 2 video decoder (DivX, Xvid).
 *
 * ISO/IEC 14496-2 Simple and Advanced Simple Profile, rectangular 4:2:0 8-bit
 * video: what DivX/Xvid encodes, old phones and cameras carry (an MKV's
 * V_MPEG4/ISO/ASP, an MP4's 'mp4v'). I-, P- and B-VOPs, intra AC/DC
 * prediction, both quantisers (H.263 and MPEG with its matrices), one or four
 * vectors a macroblock, half- and quarter-sample motion, B-VOP direct mode,
 * video packets (resync markers) with concealment, "not coded" VOPs, and
 * DivX's packed B-frames (a P- and a B-VOP in one block, an empty one after).
 * Not decoded - mpeg4_unsupported() says which: interlaced coding, global
 * motion compensation (S-VOPs with warping points), data partitioning,
 * shapes, sprites, complexity estimation, NEWPRED, scalability; nor DivX 3
 * (MS-MPEG4), a different codec.
 *
 * Feed it whole blocks - each a VOP, or a packed pair - with any headers in
 * front; in Matroska and MP4 the VOL sits in CodecPrivate / the esds (pass that
 * first). Frames come out in display order. Their pts: the block times, handed
 * out smallest first as frames leave in display order - so a file's own times
 * hold through B-frames, packed blocks (whose empty follower brings the second
 * frame's time) and frame rates that change. A "not coded" VOP brings no frame:
 * the one before shows longer. After a seek call mpeg4_reset(): a B-VOP whose
 * references are missing is dropped. Damaged packets are concealed from the
 * reference and decoding resumes at the next resync marker or VOP.
 *
 * The IDCT is modules/av/idct.h's (IEEE 1180), not the one an encoder used, so
 * a picture differs from the encoder's by rounding that P-VOPs carry on until
 * the next I-VOP (measured against FFmpeg on real Xvid files: see mpeg4_test.c).
 *
 * Pure library: libc only (no core/), no mutable globals, no OS calls — every
 * decoder instance is independent. Builds for x86-64, aarch64/armv6 and wasm32. */
#ifndef HEMMA_AV_MPEG4_H
#define HEMMA_AV_MPEG4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MPEG4_NOPTS INT64_MIN

typedef struct {
    const uint8_t *y, *u, *v; /* planes, 4:2:0 (chroma is half width, half height, rounded up) */
    int stride_y, stride_c;
    int width, height;     /* the VOL's size */
    int par_num, par_den;  /* pixel aspect ratio (1:1 when the VOL gives none) */
    int rate_num, rate_den; /* the VOL's fixed VOP rate, 0/0 when it states none */
    char type;             /* 'I', 'P', 'B' or 'S' */
    int64_t pts;           /* see above; MPEG4_NOPTS when no block time is left for it */
    int concealed;         /* macroblocks patched up from the reference (damaged data) */
} Mpeg4Frame;

typedef struct Mpeg4 Mpeg4;

Mpeg4 *mpeg4_new(void);
void mpeg4_free(Mpeg4 *d);

/* Decode a block (VOPs, plus any headers in front of them). `pts` is its time. Returns the number
   of frames now waiting in display order, -1 on out-of-memory. Damaged input is never fatal. */
int mpeg4_decode(Mpeg4 *d, const uint8_t *data, size_t n, int64_t pts);
/* The next frame in display order. Its planes stay valid until the next call to
   mpeg4_decode / mpeg4_flush / mpeg4_reset / mpeg4_free. */
bool mpeg4_frame(Mpeg4 *d, Mpeg4Frame *out);
void mpeg4_flush(Mpeg4 *d); /* end of stream: also queue the reference held back for display */
void mpeg4_reset(Mpeg4 *d); /* after a seek: forget references, queued frames and block times */

/* Stream facts once a VOL was seen (false before). */
bool mpeg4_info(const Mpeg4 *d, int *width, int *height);
/* A tool the stream uses that this decoder doesn't ("interlaced coding", "global motion compensation"
   ...), NULL while there's none: its VOPs aren't decoded. */
const char *mpeg4_unsupported(const Mpeg4 *d);

#endif
