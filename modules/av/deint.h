/* modules/av/deint.h — progressive frames from interlaced and telecined video.
 *
 * Browsers and phones want progressive pictures; DVDs mostly carry fields.
 * Four ways to get there, picked per source (deint_analyze tells which):
 *
 *  - DEINT_IVTC   inverse telecine for film transferred to 29.97i with 3:2
 *                 pulldown baked in ("hard telecine" — e.g. That '70s Show on
 *                 DVD). Each frame's top field is matched with the bottom field
 *                 (previous, current or next frame) that belongs to the same
 *                 film frame, then one duplicate in every five is dropped:
 *                 23.976p with every original film frame intact and nothing
 *                 interpolated. Frames no match can fix (orphan fields at edits,
 *                 video-shot titles) are deinterlaced as below.
 *  - DEINT_YADIF  motion-adaptive spatial-temporal deinterlacing (yadif's
 *                 algorithm: edge-directed interpolation, clamped by the fields
 *                 before and after) for video-shot material: 29.97p.
 *  - DEINT_BOB    the same, one frame per field: 59.94p, smoothest motion.
 *  - DEINT_PASS   frames untouched: progressive sources, soft telecine (film
 *                 frames with repeat_first_field flags — time them by `fields`),
 *                 and progressive pictures stored as interlaced frames (e.g. a
 *                 29.97p show on DVD). Only a frame that really shows combing
 *                 gets YADIF.
 *
 * Planes are 4:2:0; chroma rows alternate fields like luma rows do, so every
 * plane is handled field-wise. Frames go in in display order (copied: the
 * decoder may reuse its buffers) and come out with up to ~6 frames of delay.
 *
 * Pure library: libc only, no globals, no OS calls (wasm32 / Pi friendly). */
#ifndef HEMMA_AV_DEINT_H
#define HEMMA_AV_DEINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *y, *u, *v;
    int stride_y, stride_c;
    int width, height; /* the picture's own size: deint_push refuses one that isn't deint_new's (a
                          stream that changes size mid-way would otherwise be read past its end) */
    bool progressive; /* progressive_frame */
    bool tff;         /* top_field_first */
    bool rff;         /* repeat_first_field */
    int64_t pts;
} DeintIn;

typedef struct {
    const uint8_t *y, *u, *v; /* valid until the next deint_push / deint_flush / deint_free */
    int stride_y, stride_c, width, height;
    int64_t pts;      /* pts of the source frame this one mostly came from */
    int64_t index;    /* output frame number: shown at index / output rate (deint_rate) */
    int fields;       /* how long the source showed it, in field periods (DEINT_PASS: 2, or 3 with RFF) */
    bool interpolated; /* part of it was made up (yadif) */
} DeintOut;

enum { DEINT_YADIF, DEINT_BOB, DEINT_IVTC, DEINT_PASS };

typedef struct Deint Deint;

Deint *deint_new(int width, int height, int mode); /* NULL on bad arguments or out of memory */
void deint_free(Deint *d);
int deint_push(Deint *d, const DeintIn *in); /* frames ready to pull, -1 on bad input (another size, short strides) */
bool deint_pull(Deint *d, DeintOut *out);
int deint_flush(Deint *d); /* end of stream: the frames still held back become ready */
/* Output frame rate for an input rate: IVTC 30000/1001 -> 24000/1001, BOB -> 60000/1001. */
void deint_rate(int mode, int in_num, int in_den, int *out_num, int *out_den);

typedef struct {
    int64_t in, out;
    int64_t match_c, match_p, match_n; /* IVTC: which bottom field fitted each frame */
    int64_t interpolated;               /* frames that needed yadif */
    int64_t dropped;                    /* IVTC: duplicates removed */
} DeintCounters;
const DeintCounters *deint_counters(const Deint *d);

/* ---- what is this video? ---- */

#define DEINT_THUMB_W 24
#define DEINT_THUMB_H 16

typedef struct {
    int64_t frames, progressive, rff;
    int64_t combed;  /* frames whose fields don't belong together (motion between them) */
    int64_t moving;  /* frames that differ from the one before */
    /* 10-frame windows: */
    int64_t cadence; /* a steady 3:2 pattern: 2 adjacent combed frames in every 5 (hard telecine) */
    int64_t video;   /* combed nearly throughout: fields from different instants (interlaced video) */
    int64_t clean;   /* moving, yet never combed: progressive pictures, whatever the flags say */
    int64_t still;   /* nothing moving */
    int64_t mixed;   /* anything else (cuts, fades, partial motion) */
    /* private */
    uint32_t hist, mhist;
    int nhist;
    bool have_thumb;
    uint8_t thumb[DEINT_THUMB_W * DEINT_THUMB_H];
} DeintStats; /* zero it, then feed frames in display order */

void deint_analyze(DeintStats *s, const DeintIn *in, int width, int height);

enum { DEINT_KIND_UNKNOWN, DEINT_KIND_PROGRESSIVE, DEINT_KIND_SOFT_TELECINE, DEINT_KIND_HARD_TELECINE, DEINT_KIND_INTERLACED };
int deint_kind(const DeintStats *s);
const char *deint_kind_name(int kind);
int deint_mode_for(int kind); /* PROGRESSIVE/SOFT_TELECINE -> PASS, HARD_TELECINE -> IVTC, else YADIF */

/* Combed pixels in the worst 16x16 block (0..256; the woven frame top + bottom). Exposed for tests/tools. */
int deint_comb(const uint8_t *top, const uint8_t *bottom, int stride, int width, int height);

#endif
