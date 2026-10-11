/* modules/media/convert.h — make a video play everywhere: one MP4 (H.264 video,
 * AAC sound) that every browser, iPhone, iPad, TV and Chromecast plays.
 * Module-internal (convert.c); media.c wires the card, actions and tick. */
#ifndef HEMMA_MEDIA_CONVERT_H
#define HEMMA_MEDIA_CONVERT_H

#include "library.h"

typedef enum {
    CONV_NONE,      /* plays everywhere already, or nothing we can do yet */
    CONV_REMUX,     /* H.264 + AAC in MKV: a new container is enough (fast, lossless) */
    CONV_TRANSCODE, /* MPEG-2 video (DVD rips) and/or Dolby AC-3 sound: re-encoded */
} ConvertNeed;

ConvertNeed convert_need(const MediaFile *f);
void convert_web_path(const char *src, char *out, size_t cap); /* "dir/x.mkv" -> "dir/x.web.mp4" */

typedef struct {
    char src[1024], dst[1024]; /* dst is written as "<dst>.part" and renamed when complete */
    int quality;               /* H.264 QP (lower = better, bigger); 0 = 23 */
    int audio_kbps;            /* AAC bitrate; 0 = 160 */
    int64_t limit_ns;          /* stop after this much video (previews, tests); 0 = the whole file */
    volatile int *stop;        /* non-zero: give up soon (the .part is removed) */
    volatile int permille;     /* progress 0..1000, written by the worker */
    char err[200];             /* why it failed */
    char what[120];            /* what was done: "DVD video (film) → H.264 · Dolby → AAC" */
    int64_t out_bytes;
} ConvertJob;

/* Blocking - run it on a worker thread. True when dst is complete. Uses only
   the av library and plain file I/O, so any number may run at once. */
bool convert_file(ConvertJob *j);

/* The queue (event-loop side): one file per worker thread, progress on screen. */
void convert_tick(int64_t now_ms);
void convert_stop(void);
void convert_card(Ctx *c, Buf *b);          /* library settings: what needs converting, progress */
void convert_act_start(Ctx *c, Fields *f);  /* media.convert: id=<hex> for one video, else all */
void convert_act_stop(Ctx *c, Fields *f);   /* media.convert_stop */
bool convert_queued(uint64_t id);           /* waiting or being converted */
/* The copy at dst lasts as long as the original (library id, file src): delete the original, its
   progress moves to the copy. What "delete each original once its copy is made" does per file. */
bool convert_replace_original(uint64_t id, const char *src, const char *dst);

#endif
