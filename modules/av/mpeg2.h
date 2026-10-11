/* modules/av/mpeg2.h — MPEG-2 (and MPEG-1) video decoder.
 *
 * ISO/IEC 13818-2 Main Profile @ Main/High Level, 4:2:0: what DVDs, DVB/ATSC
 * broadcasts and MakeMKV rips carry. Frame and field pictures, frame/field
 * DCT, frame/field/16x8/dual-prime motion compensation, B-frame reordering,
 * quantiser matrices, both VLC tables, mismatch control. MPEG-1 streams decode
 * through the same path (D-pictures are skipped).
 *
 * Feed it whole pictures — a Matroska block, or a reassembled PES payload —
 * optionally preceded by sequence/GOP headers; in Matroska the sequence header
 * also sits in CodecPrivate (pass that first). Frames come out in display order
 * with their own pts. After a seek call mpeg2_reset(): B frames whose forward
 * reference is missing are dropped (unless the GOP is closed). Damaged slices
 * are concealed from the previous picture and decoding resumes at the next
 * slice.
 *
 * Pure library: libc only (no core/), no mutable globals, no OS calls — every
 * decoder instance is independent, so one per thread is fine. Builds for
 * x86-64, aarch64/armv6 and wasm32. */
#ifndef HEMMA_AV_MPEG2_H
#define HEMMA_AV_MPEG2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MPEG2_NOPTS INT64_MIN

typedef struct {
    const uint8_t *y, *u, *v; /* planes, 4:2:0 (chroma is half width, half height) */
    int stride_y, stride_c;
    int width, height;             /* picture size (horizontal/vertical_size) */
    int coded_width, coded_height; /* whole macroblocks */
    int disp_width, disp_height;   /* sequence display extension, else = width/height */
    int dar_num, dar_den;          /* display aspect ratio: 4:3, 16:9, ... */
    int rate_num, rate_den;        /* frame rate, e.g. 30000/1001 */
    char type;                     /* 'I', 'P' or 'B' */
    bool mpeg1;
    bool progressive_sequence, progressive_frame, top_field_first, repeat_first_field;
    int temporal_reference;
    int matrix;      /* matrix_coefficients (1 = BT.709, 5/6 = BT.601), 0 = not given */
    int64_t pts;     /* given with the data that carried this picture, else MPEG2_NOPTS */
    int concealed;   /* macroblocks that had to be patched up (damaged data) */
} Mpeg2Frame;

typedef struct Mpeg2 Mpeg2;

Mpeg2 *mpeg2_new(void);
void mpeg2_free(Mpeg2 *d);

/* Decode whole pictures (plus any headers in front of them). `pts` belongs to
   the first picture that starts in this data. Returns the number of frames now
   waiting in display order, -1 on out-of-memory. Damaged input is never fatal. */
int mpeg2_decode(Mpeg2 *d, const uint8_t *data, size_t n, int64_t pts);
/* The next frame in display order. Its planes stay valid until the next call
   to mpeg2_decode / mpeg2_flush / mpeg2_reset / mpeg2_free. */
bool mpeg2_frame(Mpeg2 *d, Mpeg2Frame *out);
void mpeg2_flush(Mpeg2 *d); /* end of stream: also queue the last reference frame */
void mpeg2_reset(Mpeg2 *d); /* after a seek: forget references and queued frames */

/* Stream facts once a sequence header was seen (false before). */
bool mpeg2_info(const Mpeg2 *d, int *width, int *height, int *rate_num, int *rate_den);

#endif
