/* modules/av/conv.h — the "play everywhere" converter: one MKV in, one MP4 out
 * that every browser, iPhone, iPad, TV and Chromecast plays.
 *
 *   MPEG-2 / MPEG-1 (DVD rips) -> mpeg2 -> deint -> h264enc  \
 *   H.264                      -> copied as it is             >  mp4 (faststart)
 *   Dolby AC-3                 -> ac3 (stereo) -> aac         /
 *   AAC                        -> copied
 *
 * Frames stream through: memory stays a few MB whatever the file size (plus
 * ~4 bytes per sample for the MP4 index, and 16 per video frame when H.264 is
 * copied). Input is random access (MkvReader), output appends and patches the
 * reserved header at the end (Mp4Out with write_at).
 *
 * Pure library: libc only, no globals, no OS calls. The same code runs in the
 * server (media/convert.c, files on disk) and in the uploading browser
 * (wasm/avconv.c compiled to WebAssembly: the copy is made on the device that
 * holds the original, and only the copy travels). One job per thread. */
#ifndef HEMMA_AV_CONV_H
#define HEMMA_AV_CONV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mkv.h"
#include "mp4.h"

typedef struct {
    MkvReader in;
    Mp4Out out;           /* needs write_at: the index is written into room reserved up front */
    int quality;          /* H.264 QP (lower = better, bigger); 0 = 23 */
    int audio_kbps;       /* AAC bitrate; 0 = 160 */
    int64_t limit_ns;     /* stop after this much video (previews, tests); 0 = the whole file */
    volatile int *stop;   /* non-zero: give up soon */
    /* called whenever permille changes (may be NULL) */
    void (*progress)(void *ctx, int permille);
    void *progress_ctx;
    /* results */
    volatile int permille; /* 0..1000 */
    char err[200];         /* why it failed */
    char what[120];        /* what was done: "DVD video (film, 23.976 fps) → H.264 · Dolby → AAC" */
    int64_t out_bytes;
    bool unsupported;      /* failed because of what the file holds (a codec we don't convert yet), not an error */
} AvConvJob;

/* Blocking. True when the output is complete. */
bool av_convert(AvConvJob *j);

#endif
