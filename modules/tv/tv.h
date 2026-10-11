/* modules/tv/tv.h — module-local: "90's TV" channels on a shared clock.
 *
 * A channel airs a playlist of episodes (and optionally movies) from the media
 * library in a loop that started at `epoch`. Where we are in the loop is pure
 * arithmetic on the wall clock — (now - epoch) mod cycle — so every screen, and
 * every restart, agrees on what's on right now, like broadcast TV.
 *
 * tv_plan.c holds the pure schedule math (no media dependency, tested
 * directly); tv.c builds plans from the library and renders the module. */
#ifndef HEMMA_TV_H
#define HEMMA_TV_H

#include "../../core/app.h"

enum { TV_ROTATE = 0, TV_SHUFFLE = 1 };         /* playlist order */
enum { TV_ALIGN_NONE = 0, TV_ALIGN_HALF_HOUR = 1 }; /* programmes start back-to-back or on :00/:30 */
#define TV_SLOT_S 1800

typedef struct {
    int id;
    int number; /* 1..99, unique */
    char name[40];
    int hue;
    bool all_series; /* air every series in the library (new ones join automatically) */
    bool movies;     /* movies join the rotation too */
    char shows[1024]; /* chosen series: comma-separated 16-hex media series ids */
    int order;        /* TV_ROTATE / TV_SHUFFLE */
    int align;        /* TV_ALIGN_* */
    int64_t epoch;    /* when the loop started */
    int seed;         /* shuffle seed */
    int created_by;
    int64_t created;
} Channel;

extern Table T_tv_channel;

/* ------------------------------------------------- pure schedule math */

typedef struct {
    uint64_t file; /* media file id */
    int dur_s;     /* programme length, whole seconds (rounded up) */
} TvItem;

typedef struct {
    const TvItem *items;
    int n;
} TvStream; /* one show's episodes in order (or the movies) */

/* Round-robin across shows: A1 B1 C1 A2 C2 A3 ... Returns the count (<= max). */
int tv_rotate(const TvStream *streams, int nstreams, TvItem *out, int max);
/* Deterministic Fisher-Yates shuffle: same seed, same order, on every machine. */
void tv_shuffle(TvItem *items, int n, uint32_t seed);

typedef struct {
    TvItem *items;
    int n;
    int64_t *starts; /* n+1 prefix sums of slot lengths: starts[k] = offset of airing k, starts[n] = cycle */
    int64_t cycle;   /* seconds for one full loop */
    bool half_hour;
    int skipped; /* items left out: unknown length or won't play in browsers (set by the builder) */
} TvPlan;

void tv_plan_init(TvPlan *p, const TvItem *items, int n, bool half_hour); /* copies items */
void tv_plan_free(TvPlan *p);
int64_t tv_epoch(const TvPlan *p, int64_t epoch); /* effective start (rounded down to :00/:30 for half-hour) */

typedef struct {
    int index;      /* position in the playlist */
    uint64_t file;  /* what's airing */
    int64_t start;  /* when this airing started (Unix seconds) */
    int64_t end;    /* when the programme itself ends */
    int64_t next;   /* when the next programme starts (end, or the next :00/:30) */
    int64_t offset; /* seconds into the programme (0 during an intermission) */
    bool gap;       /* intermission: programme over, the next one starts at `next` */
} TvSlot;

bool tv_now(const TvPlan *p, int64_t epoch, int64_t now, TvSlot *out); /* false = nothing to air */
/* Airings overlapping [from, to), in order (the guide). Returns the count (<= max). */
int tv_window(const TvPlan *p, int64_t epoch, int64_t from, int64_t to, TvSlot *out, int max);

/* --------------------------------------- channels and the media library */

Channel *tv_channel_by_number(int number);
const TvPlan *tv_channel_plan(const Channel *ch); /* cached per channel; never NULL (maybe empty) */
void tv_plans_reset(void);

#endif
