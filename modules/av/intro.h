/* modules/av/intro.h — a series' intro found by its sound (2.9).
 *
 * An intro is the one stretch of sound every episode of a season shares; the rest of each episode differs.
 * So: each episode's first minutes become a FINGERPRINT (intro_read: the file's default sound track decoded
 * - AAC, AC-3, E-AC-3 or DTS - mixed to mono, filtered down to ~5 kHz; INTRO_FPS frames a second, each 32
 * bits saying which of 33 bands between 300 and 2000 Hz grew louder than its neighbour since the frame
 * before - the robust hash of Haitsma & Kalker, 2002), and the fingerprints of a season are compared in
 * pairs (intro_match: every offset, the longest stretches whose bits mostly agree). intro_find then gives each
 * episode the stretch it shares with the most of its neighbours, when that's long enough to be an intro.
 * It survives other encodings, volumes and a cold open of any length before the intro; it finds nothing in
 * a pilot, a special, or a show whose intro changes every time.
 *
 * Pure library: libc only (no core/), no mutable globals - any thread. Bounded by its inputs: a print is
 * INTRO_FPS x the seconds asked for. */
#ifndef HEMMA_AV_INTRO_H
#define HEMMA_AV_INTRO_H

#include "mkv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INTRO_FPS 16 /* fingerprint frames a second */

typedef struct {
    uint32_t *v;      /* one 32-bit print a frame */
    uint8_t *loud;    /* 0 = (near) silence there: matches nothing */
    int n;            /* frames */
    int64_t start_ms; /* the video time of frame 0's centre */
} IntroPrint;

void intro_print_free(IntroPrint *p);

/* ---- making one from PCM ---- */
typedef struct IntroPrinter IntroPrinter;
IntroPrinter *intro_printer_open(int sample_rate); /* NULL for a rate under 4 kHz or over 192 kHz */
/* n mono samples in +-1.0 (any number per call) */
bool intro_printer_feed(IntroPrinter *p, const float *mono, int n);
/* The frames so far, moved into out (start_ms left 0); the printer is closed. */
bool intro_printer_finish(IntroPrinter *p, IntroPrint *out);
void intro_printer_close(IntroPrinter *p);

/* ---- making one from a file ---- */
enum {
    INTRO_READ_OK = 0,
    INTRO_READ_NO_SOUND = -1, /* no sound track */
    INTRO_READ_CODEC = -2,    /* its sound is in a format this can't decode (MP3, Opus, Vorbis, FLAC ...) */
    INTRO_READ_BROKEN = -3,   /* not a readable Matroska/MP4, or its sound wouldn't decode */
    INTRO_READ_STOPPED = -4,  /* the reader failed or `stop` said so: try again later */
};

typedef struct {
    int64_t seconds;               /* how much of its start to read */
    bool (*stop)(void *ctx);       /* asked now and then: true = give up (INTRO_READ_STOPPED) */
    void *ctx;
} IntroReadOpts;

/* The default sound track's first o->seconds as a print (out->start_ms = where its first frame sits). `why`
   says what failed, in words ("its sound is MP3"), for a log. */
int intro_read(const MkvReader *rd, const IntroReadOpts *o, IntroPrint *out, char *why, size_t whycap);

/* ---- comparing ---- */
typedef struct {
    int a_from, a_to, b_from, b_to; /* frames, `to` exclusive */
    int cost;                       /* mean differing bits x 100 over the stretch (random sound: ~1600) */
} IntroRun;

/* The longest stretches (at least min_frames) the two share, at most max_runs, longest first. */
int intro_match(const IntroPrint *a, const IntroPrint *b, int min_frames, IntroRun *runs, int max_runs);

typedef struct {
    int64_t from_ms, to_ms; /* -1 = none found */
    int votes;              /* how many other episodes share it */
} IntroFound;

typedef struct {
    int64_t min_ms, max_ms; /* an intro's length: shorter or longer isn't one (default 15 s .. 150 s) */
    int partners;           /* how many of the following episodes each is compared with (default 6) */
} IntroFindOpts;

/* Episodes in order (NULL or an empty print = couldn't be read): each one's intro, or none. */
void intro_find(const IntroPrint *const *prints, int n, const IntroFindOpts *o, IntroFound *out);

#endif
