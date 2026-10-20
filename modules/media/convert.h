/* modules/media/convert.h — make a video play everywhere: one MP4 (H.264 video,
 * AAC sound) that every browser, iPhone, iPad, TV and Chromecast plays.
 * Module-internal (convert.c); media.c wires the card, actions and tick. */
#ifndef HEMMA_MEDIA_CONVERT_H
#define HEMMA_MEDIA_CONVERT_H

#include <stdatomic.h>

#include "library.h"

typedef enum {
    CONV_NONE,      /* plays everywhere already, or nothing we can do yet */
    CONV_REMUX,     /* H.264 + AAC in MKV: a new container is enough (fast, lossless) */
    CONV_TRANSCODE, /* MPEG-2 video (DVD rips) and/or Dolby AC-3 / E-AC-3 sound: re-encoded */
} ConvertNeed;

ConvertNeed convert_need(const MediaFile *f);
void convert_web_path(const char *src, char *out, size_t cap); /* "dir/x.mkv" -> "dir/x.web.mp4" */
/* "dir/x.web.mp4" -> "dir/x.web.mp4.conv.part": the converter's own name for a copy being made,
   so an upload's "x.web.mp4.part" (resumable) is never mistaken for it, nor the other way round */
void convert_part_path(const char *dst, char *out, size_t cap);

typedef struct {
    char src[1024], dst[1024]; /* dst is written as convert_part_path(dst) and renamed when complete */
    int quality;               /* H.264 QP (lower = better, bigger); 0 = 23 */
    int audio_kbps;            /* AAC bitrate; 0 = 160 */
    int64_t limit_ns;          /* stop after this much video (previews, tests); 0 = the whole file */
    atomic_int *stop;          /* non-zero: give up soon (the .part is removed); NULL = never */
    atomic_int permille;       /* progress 0..1000, written by the worker */
    atomic_int out_mb;         /* written so far, in MB: what the copy already takes on the disk */
    char err[200];             /* why it failed */
    char what[120];            /* what was done: "DVD video (film) → H.264 · Dolby Digital → AAC" */
    int64_t out_bytes;
    int subs;                  /* text subtitle tracks saved next to the original as .srt */
} ConvertJob;

/* Blocking - run it on a worker thread. True when dst is complete. Uses only
   the av library and plain file I/O, so any number may run at once. Never
   replaces a file already at dst. The original's text subtitle tracks become
   .srt files next to it (convert_save_sub). */
bool convert_file(ConvertJob *j);

/* Save one subtitle track taken out of `video` (a path) next to it, named by media_sub_sidecar -
   never over a file that is there (one saved earlier, or the household's own). True when written. */
bool convert_save_sub(const char *video, const char *lang, const char *name, bool forced, const char *srt, size_t len);

/* The queue (event-loop side): one file per worker thread, progress on screen. Before a file
   starts, its original's size must fit on the disk next to it (room.h): what doesn't wait while
   other conversions run, and is set aside as "no room" when nothing else could make room. */
void convert_tick(int64_t now_ms);
void convert_stop(void);
void convert_card(Ctx *c, Buf *b);          /* library settings: what needs converting, progress */
void convert_act_start(Ctx *c, Fields *f);  /* media.convert: id=<hex> for one video, else all */
void convert_act_stop(Ctx *c, Fields *f);   /* media.convert_stop */
bool convert_queued(uint64_t id);           /* waiting or being converted */
/* Still on the card: conversions set aside for lack of room, and failed ones (the System page). */
void convert_trouble(int *noroom, int *failed);
/* The copy at dst lasts as long as the original (library id, file src): delete the original, its
   progress moves to the copy. What "delete each original once its copy is made" does per file. */
bool convert_replace_original(uint64_t id, const char *src, const char *dst);

/* tests (no worker threads there: media.convert only queues): start the next queued conversion
   that fits, on this thread, without converting - it stays running until convert_test_finish.
   Returns its name ("Show S01E02") or NULL when none could start. */
const char *convert_test_start(void);
void convert_test_finish(bool ok); /* every running one: done (ok) or failed */

#endif
