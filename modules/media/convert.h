/* modules/media/convert.h — make a video play everywhere: one MP4 (H.264 video,
 * AAC sound) that every browser, iPhone, iPad, TV and Chromecast plays.
 * Module-internal (convert.c); media.c wires the card, actions and tick. */
#ifndef HEMMA_MEDIA_CONVERT_H
#define HEMMA_MEDIA_CONVERT_H

#include <stdatomic.h>

#include "../av/conv.h"
#include "library.h"

typedef enum {
    CONV_NONE,      /* plays everywhere already, or nothing we can do yet */
    CONV_REMUX,     /* H.264 + AAC in MKV: a new container is enough (fast, lossless) */
    CONV_TRANSCODE, /* MPEG-2 video (DVD rips) and/or Dolby AC-3 / E-AC-3 sound: re-encoded */
} ConvertNeed;

ConvertNeed convert_need(const MediaFile *f);
void convert_web_path(const char *src, char *out, size_t cap); /* "dir/x.mkv" -> "dir/x.web.mp4" */
/* (2.6) PLAY EVERYWHERE FROM HEVC: a video (as the library lists it) whose playable file is HEVC, made an H.264 copy
   (AvConvJob.h264) - read from its original where that's a file the converter reads (MKV, MP4, MOV: every sound,
   the best picture), else from its copy; written as its .web.mp4: a new one beside an original, or Hemma's own
   HEVC copy replaced (*replace: only ever a ".web.mp4") - an original never is. False: nothing to do (not HEVC, an AV1, a
   play-all, two versions of one name (2.5) whose HEVC one is an original ...). */
bool convert_h264_paths(const MediaFile *f, char *src, size_t scap, char *dst, size_t dcap, bool *replace);
bool convert_h264_need(const MediaFile *f); /* the same, by the scan alone - no disk (pages count every video with it) */
const MediaFile *convert_h264_from(const MediaFile *f); /* the file it's made from (NULL: nothing to make) */
/* the videos of a set that need it - "id=<hex>", "series=<hex>[&season=<n>]", "films=1", "" = every one - into out;
   how many (an unknown set: 0) */
int convert_h264_set(const char *what, const MediaFile **out, int cap);
/* "dir/x.web.mp4" -> "dir/x.web.mp4.conv.part": the converter's own name for a copy being made,
   so an upload's "x.web.mp4.part" (resumable) is never mistaken for it, nor the other way round */
void convert_part_path(const char *dst, char *out, size_t cap);
/* (2.7) part k of a copy made in parts: "x.web.mp4.p3.conv.part" (a leftover one goes with a scan as any .conv.part) */
void convert_piece_path(const char *dst, int k, char *out, size_t cap);

typedef struct {
    char src[1024], dst[1024]; /* dst is written as convert_part_path(dst) and renamed when complete */
    int quality;               /* H.264 QP (lower = better, bigger); 0 = 23 */
    int audio_kbps;            /* AAC bitrate; 0 = 160 */
    int64_t limit_ns;          /* stop after this much video (previews, tests); 0 = the whole file */
    bool h264;                 /* (2.6) the copy's picture must be H.264: HEVC decoded and re-encoded (AvConvJob.h264) */
    bool replace;              /* (2.6) dst may be there: Hemma's own .web.mp4 (HEVC), replaced once the new one is whole */
    /* (2.7) Full throttle: part `piece` of the picture alone (piece_to > 0, as AvConvJob's), written as
       convert_piece_path(dst, piece) and kept; or (npieces > 0) the join of parts 0..npieces-1 into dst, whose files then go */
    int64_t piece_from, piece_to;
    int piece, npieces;
    atomic_int *stop;          /* non-zero: give up soon (the .part is removed); NULL = never */
    atomic_int permille;       /* progress 0..1000, written by the worker */
    atomic_int out_mb;         /* written so far, in MB: what the copy already takes on the disk */
    char err[200];             /* why it failed */
    char what[120];            /* what was done: "DVD video (film) → H.264 · Dolby Digital → AAC" */
    int64_t out_bytes;
    int subs;                  /* subtitle tracks saved next to the original (convert_save_subs) */
} ConvertJob;

/* Blocking - run it on a worker thread. True when dst is complete. Uses only
   the av library and plain file I/O, so any number may run at once. Never
   replaces a file already at dst. The original's subtitle tracks become files
   next to it (convert_save_subs) - also when the converter refuses the file for
   what it holds (an AV1 picture, DTS sound): its subtitles still come out. */
bool convert_file(ConvertJob *j);

/* The tracks of one file met so far, in its order: a second track in the same language and flags
   is named ".2", a third ".3" (contract C1) - whether the first was saved or not. */
typedef struct {
    char keys[AV_CONV_MAX_SUBS][40]; /* media_sub_key of each (the first AV_CONV_MAX_SUBS) */
    int n;
} ConvertSubs;

/* Save one subtitle track taken out of `video` (a path) next to it, named by media_sub_sidecar:
   text -> "x.en.srt", Blu-ray PGS -> "x.en.sup", DVD VobSub -> "x.en.idx" + "x.en.sub" (both or
   neither). `seen` (NULL: the file's first) numbers it and remembers it. Never over a file that is
   there (one saved earlier, or the household's own): that track is skipped. True when written. */
bool convert_save_sub(const char *video, const AvConvSub *s, ConvertSubs *seen);
/* Every track of one file, in its order: how many were written. */
int convert_save_subs(const char *video, const AvConvSub *subs, int n);

/* The queue (event-loop side): one file per worker thread, progress on screen. Before a file
   starts, its original's size must fit on the disk next to it (room.h): what doesn't wait while
   other conversions run, and is set aside as "no room" when nothing else could make room. */
void convert_tick(int64_t now_ms);
void convert_stop(void);
void convert_card(Ctx *c, Buf *b);          /* library settings: what needs converting, progress */
/* media.convert: id=<hex> for one video, else all; (2.6) with h264=1 the HEVC ones made H.264 instead - id=<hex>,
   series=<hex>[&season=<n>], films=1, else every one */
void convert_act_start(Ctx *c, Fields *f);
void convert_h264_card(Ctx *c, Buf *b); /* (2.6) library settings: the HEVC videos, by series, and both ways to convert them */
/* (2.6) the two buttons for a set (one video id=, a series [and season], films=1, or all): on Hemma's computer, and -
   where the page can run the converter - on this device (an upload batch: upload.c's "here"). `what` = the hidden
   fields ("id=<hex>", "series=<hex>&season=2", "films=1", ""). */
void convert_h264_buttons(Ctx *c, Buf *b, const char *what, int count);
void convert_act_stop(Ctx *c, Fields *f);   /* media.convert_stop */
bool convert_queued(uint64_t id);           /* waiting or being converted */
bool convert_busy(void);                    /* any video waiting or being converted (subtake.c gives way) */
/* Still on the card: conversions set aside for lack of room, and failed ones (the System page). */
void convert_trouble(int *noroom, int *failed);
/* (2.7) What the Converting tab holds, for Library settings' Overview: videos Convert would make a copy of (need), and
   the queue - waiting or running (busy), failed, set aside for lack of room. */
typedef struct {
    int need, busy, failed, noroom;
} ConvertSummary;
void convert_summary(ConvertSummary *s);

/* (2.7) How hard conversions work: Low (~30% of the cores), Medium (~60%: the default), Full (all of them; a video being
   re-encoded is split into parts when fewer are left than cores). Hemma's computer: setting media.power; a person's
   devices ("On this device", the upload form): pref media.power_here - action media.power (where=server|here, tier). */
enum { CONVERT_LOW, CONVERT_MEDIUM, CONVERT_FULL };
int convert_power_of(const char *s);                          /* "low" "medium" "full" (else medium) */
int convert_power_workers(int tier, int cores, bool phone);   /* videos at once on a machine of `cores` */
int convert_device_workers(Ctx *c); /* this session's device at its person's tier; 0 = its cores not known yet */
int convert_device_tier(Ctx *c);    /* its person's tier for their devices (pref media.power_here) */
#define CONVERT_PART_MIN_S 300      /* a video is made in parts of 5 minutes at least (the keyframes they start at cost) */
void convert_power_card(Ctx *c, Buf *b); /* Library settings' Converting tab (admins) */
void convert_act_power(Ctx *c, Fields *f);
/* The copy at dst lasts as long as the original (library id, file src): delete the original, its
   progress moves to the copy. What "delete each original once its copy is made" does per file. */
bool convert_replace_original(uint64_t id, const char *src, const char *dst);

/* tests (no worker threads there: media.convert only queues): start the next queued conversion
   that fits, on this thread, without converting - it stays running until convert_test_finish.
   Returns its name ("Show S01E02") or NULL when none could start. */
const char *convert_test_start(void);
void convert_test_finish(bool ok); /* every running one: done (ok) or failed */
void convert_test_device_cores(int cores); /* (2.7) every device has this many cores (0: as it said) */

#endif
