/* modules/av/conv.h — the "play everywhere" converter: one MKV in, one MP4 out
 * that every browser, iPhone, iPad, TV and Chromecast plays.
 *
 *   MPEG-2 / MPEG-1 (DVD rips) -> mpeg2 -> deint -> h264enc  \
 *   H.264                      -> copied as it is             >  mp4 (faststart)
 *   HEVC (x265 films)          -> copied as 'hvc1'           /   (plays in Safari; other browsers need HEVC hardware)
 *   Dolby AC-3 / E-AC-3 (DD+)  -> ac3 (stereo) -> aac         /
 *   AAC                        -> copied
 *   text subtitles (SRT, ASS)  -> SubRip texts handed back (want_subs): the caller saves them as
 *                                 "<name>.<language>.srt" next to the video, where the library finds them
 *
 * Frames stream through: memory stays a few MB whatever the file size (plus
 * ~4 bytes per sample for the MP4 index, and 16 per video frame when H.264 is
 * copied). Samples are bounded, so a file of tiny blocks can't blow that up: at
 * most 2 million a track (9 hours of 60 fps), and with a stated length at most
 * 300 pictures / 120 sound blocks a second of it; past that the job fails as
 * `unsupported`. So does an MPEG-2 stream whose picture size changes part-way
 * (later pictures smaller or bigger than the first). Input is random access
 * (MkvReader), output appends and patches the reserved header at the end
 * (Mp4Out with write_at).
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

#define AV_CONV_MAX_SUBS 8               /* text subtitle tracks taken from one file */

/* Every format the converter takes, by Matroska CodecID - the one place its checks look (av_conv_codec).
   Formats grow when real files need them (owner, 2026-10-21; docs/decisions/0006): a new one goes in this
   table, and conv_test.c's conv_fuzz_seeds then fails until a seed file carries it - so the fuzzing on
   every push and every night (tools/ci/fuzz.sh) covers it from its first day. */
typedef enum { AV_DECODE = 1, AV_COPY, AV_TEXT } AvHow; /* decoded and re-encoded / copied as it is / a subtitle text */
typedef struct {
    const char *id; /* CodecID; with prefix, every CodecID starting so ("A_AAC/MPEG4/LC") */
    int type;       /* MKV_VIDEO, MKV_AUDIO, MKV_SUBTITLE */
    AvHow how;
    bool prefix;
} AvCodec;
extern const AvCodec AV_CONV_CODECS[]; /* ends with {NULL} */
const AvCodec *av_conv_codec(const char *codec_id); /* NULL: not one the converter takes */
#define AV_CONV_SUB_BYTES (768u << 10)   /* the most SRT one track may come to (a film's is ~100 KB) */

typedef struct {
    char lang[24];        /* the track's language as the file names it: "eng", "en-US", "und" */
    char name[128];       /* its name, if any ("English (SDH)") */
    bool forced, is_default;
    int cues;
    char *srt;            /* SubRip, UTF-8, NUL-terminated, timed like the copy; av_convert_free frees it */
    size_t len;
} AvConvSub;

typedef struct {
    MkvReader in;
    Mp4Out out;           /* needs write_at: the index is written into room reserved up front */
    int quality;          /* H.264 QP (lower = better, bigger); 0 = 23 */
    int audio_kbps;       /* AAC bitrate; 0 = 160 */
    int64_t limit_ns;     /* stop after this much video (previews, tests); 0 = the whole file */
    int64_t max_samples;  /* most samples a track may get, if fewer than the built-in bound (tests, fuzzing); 0 = that */
    bool want_subs;       /* hand back the text subtitle tracks (subs) */
    volatile int *stop;   /* non-zero: give up soon */
    /* called whenever permille changes (may be NULL) */
    void (*progress)(void *ctx, int permille);
    void *progress_ctx;
    /* results */
    volatile int permille; /* 0..1000 */
    char err[200];         /* why it failed: words for people ("Its sound is DTS, which ...") */
    char what[120];        /* what was done: "DVD video (film, 23.976 fps) → H.264 · Dolby Digital → AAC" */
    int64_t out_bytes;
    bool unsupported;      /* failed because of what the file holds (a codec we don't convert yet), not an error */
    AvConvSub subs[AV_CONV_MAX_SUBS]; /* with want_subs, after a success: the text subtitles (empty ones left out) */
    int nsubs;
} AvConvJob;

/* Blocking. True when the output is complete. Starts with nsubs = 0: free a previous run's first. */
bool av_convert(AvConvJob *j);
void av_convert_free(AvConvJob *j); /* the subtitles' memory */

#endif
