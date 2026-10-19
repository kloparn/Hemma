/* modules/media/library.h — the media library index.
 *
 * Module-internal, and the seam for neighbours such as a future "tv" module
 * (90's channels on a shared clock): it can include this header, walk
 * media_lib()->series / ->episodes for shows and their durations, and play a
 * file through /media/file/<id>.
 *
 * A scanner thread walks the library roots and builds a fresh MediaIndex; the
 * module's tick swaps it in on the event loop. Everything here is main-thread
 * only. Pointers into the index stay valid until the next swap, i.e. within
 * one render, action or tick — keep ids (uint64_t), not pointers. */
#ifndef HEMMA_MEDIA_LIBRARY_H
#define HEMMA_MEDIA_LIBRARY_H

#include "../../core/app.h"
#include "mediainfo.h"

#define MEDIA_MAX_ROOTS 8

enum { MF_VIDEO = 'v', MF_SUB = 's', MF_IMAGE = 'i' };
/* MediaFile.role of the titles on a ripped disc that are not episodes (see library.c), and
   of "<name>.web.mp4": the play-everywhere copy made by convert.c, hidden behind its original */
enum { MF_MAIN = 0, MF_PLAYALL = 'a', MF_BONUS = 'b', MF_WEBCOPY = 'w' };

typedef struct {
    uint64_t id; /* hash64(root path + "\n" + rel): stable across scans and restarts */
    int root;    /* index into MediaIndex.roots */
    char type;   /* MF_VIDEO / MF_SUB / MF_IMAGE */
    char role;   /* MF_MAIN, or a disc's "play all" title / bonus clip: listed only as series extras */
    char rel[400]; /* path below the root, '/' separators */
    int64_t size, mtime;
    /* videos */
    MediaName name;   /* kind, title, year, season/episode, ep_title */
    MediaProbe probe; /* container, duration, resolution, codecs, playable + why */
    uint64_t series;  /* episodes and series extras: their MediaSeries id */
    uint64_t poster;  /* image file for this item (0 = draw a CSS poster) */
    uint64_t web;     /* its "<name>.web.mp4" copy that plays everywhere (0 = none): what gets streamed */
    uint64_t fp;      /* its content: size + the first and last 64 KB, hashed - the same file under another
                         name or folder (issue #93); read once per file, kept in the cache; 0 = not yet */
    /* subtitles */
    uint64_t video; /* the video this subtitle belongs to (0 = none) */
    char lang[16];  /* "sv", "en", ... or "" */
} MediaFile;

typedef struct {
    uint64_t id; /* hash of the folded series title */
    char title[128];
    int year;
    uint64_t poster;
    int first, count; /* range in MediaIndex.episodes (season/episode order) */
    int nseasons;
    int64_t newest; /* mtime of the newest episode */
} MediaSeries;

/* A video the scan left out (issue #28: they used to vanish without a word), or an upload's
   unfinished MEDIA_PART_SUFFIX file not touched for MEDIA_PART_DAYS (issue #40: they stayed for
   good, unseen) - both listed on Library settings, at most MEDIA_LEFT_MAX of each (the counts say
   how many). Uploads write "<name>.hemma-part", a suffix of Hemma's own (issue #89): a torrent's or
   a browser's ".part" in a library folder is never listed, nor removed. */
enum { MEDIA_LEFT_EXTRA = 'e', MEDIA_LEFT_SAMPLE = 's', MEDIA_LEFT_NAMED = 'n' };
#define MEDIA_LEFT_MAX 100
#define MEDIA_PART_DAYS 7
#define MEDIA_PART_SUFFIX ".hemma-part"
#define MEDIA_SAMPLE_LONG_S 600 /* a "sample..." this long is a real video (a scene's preview clip runs a minute or two) */
#define MEDIA_EXTRA_LONG_S 2400 /* ... and a "Film-featurette" this long a film (issue #94) */
typedef struct {
    int root;
    char rel[400];
    int64_t size, mtime;
    int64_t duration_ms; /* what the probe said (named like an extra): the next scan needn't ask again */
    char why;            /* MEDIA_LEFT_*; parts: 0 */
} MediaLeft;

typedef struct {
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots;
    MediaLeft *left, *parts; /* see MediaLeft */
    int nleft, nleft_all, nparts, nparts_all;
    int64_t parts_bytes; /* of every unfinished upload found, listed or not */
    MediaFile *files; /* sorted by id */
    int nfiles;
    int *movies, nmovies;     /* indices into files: A-Z */
    int *others, nothers;     /* by folder, then name */
    int *recent, nrecent;     /* every video except series extras, newest first */
    int *episodes, nepisodes; /* grouped per series (same order as `series`), season/episode order */
    int *extras, nextras;     /* play-all titles and bonus clips of ripped series: series, season, disc, title */
    MediaSeries *series;      /* A-Z */
    int nseries;
    int64_t scanned_at; /* when the scan that produced this index finished (0 = loaded from cache) */
    int scan_ms, probed;
} MediaIndex;

/* ---- reading the index (never NULL; an empty index before the first scan) ---- */
const MediaIndex *media_lib(void);
unsigned media_lib_gen(void); /* changes whenever another index is swapped in (kids.c remakes its copy) */
const MediaFile *media_file(uint64_t id);
const MediaSeries *media_series(uint64_t id);
const MediaFile *media_episode(const MediaSeries *s, int i); /* i-th episode in order */
const MediaFile *media_neighbour(const MediaFile *ep, int dir); /* next (+1) / previous (-1) episode */
const MediaFile *media_playable(const MediaFile *f); /* the file to stream: its .web.mp4 copy when there is one */
bool media_is_web_copy(const char *rel);              /* "...name.web.mp4" */
/* "...name.playall.mkv": just the first MB of a disc's play-all title (upload.c sends that instead
   of the whole thing). Its header says how long the disc is - all the library needs to tell the
   disc's episodes from its bonus clips. Counted as the disc's play-all, never listed or played. */
bool media_is_playall_head(const char *rel);
/* Would a video at root/rel be something the library already has, under any name? Named the way
   the library will name it there: an episode counts when every episode it holds is in (S01E01-E02
   needs both), a film when one with the same title, year (when both say) and part ("CD2") is.
   Extras, disc-rip titles numbered only by order, and the file's own original or copy (x.mkv /
   x.web.mp4 in the same folder) never count. `have` gets the match's path below its root. */
bool media_have(const char *root, const char *rel, char *have, size_t cap);
size_t media_stem_len(const char *rel); /* "Show/x" of "Show/x.mkv", "Show/x.web.mp4", "Show/x.playall.mkv" */
bool media_full_path(const MediaFile *f, char *out, size_t cap);
int media_hue(const char *title); /* stable colour for CSS posters */
bool media_parse_id(const char *hex, uint64_t *id); /* 16 hex digits */
void media_id_hex(uint64_t id, char out[17]);
uint64_t media_make_id(const char *root, const char *rel);

/* ---- pictures set from the browser (admins): files where the scan looks first ---- */
/* Series -> (its show folder, "poster"); film or other video -> (its folder, its name without
   extension). Episodes and disc extras have none (they show the series' picture). */
bool media_art_target(uint64_t id, int *root, char *dir, size_t dcap, char *stem, size_t scap);
bool media_art_folder(uint64_t id, char *abs, size_t acap, char *stem, size_t scap); /* same, as a full path */
uint64_t media_art_current(uint64_t id); /* the picture file at that place now (0 = none) */
/* Delete the pictures named <stem>.<jpg|jpeg|png|webp> (any case) in dir, except `keep`
   (a file name, or NULL). Touches only the disk, so any thread may call it. Returns how many. */
int media_art_prune(const char *dir, const char *stem, const char *keep);

/* ---- scanning ---- */
int media_roots(char roots[][300], int max); /* from setting "media.roots" (one per line) */
bool media_scanning(void);
int media_scan_seen(void); /* files looked at by the running scan */
void media_scan_start(void); /* background thread; no-op under app_testing() or while one runs */
void media_scan_sync(void);  /* scan on this thread and swap in (tests) */
/* tests: drive the scanner's hand-over without threads */
void media_scan_publish_sync(void);   /* scan on this thread, leave the result pending like the thread does */
void media_scan_pretend(bool running); /* act as if a background scan were (not) running */
bool media_rescan_queued(void);       /* a rescan was asked for while one ran */

/* ---- lifecycle, called by media.c ---- */
void media_lib_start(void); /* create tables, load the cached index from SQLite */
void media_lib_tick(int64_t now_ms);
void media_lib_stop(void);

/* ---- HTTP endpoints (stream.c): /media/file/<id>, /media/img/<id>, /media/sub/<id>.vtt ---- */
typedef struct {
    int status; /* 200 whole file, 206 partial, 416 unsatisfiable */
    int64_t from, to, len;
} MediaRange;
MediaRange media_range(const char *range_header, int64_t size);
void media_range_head(Buf *out, const MediaRange *r, int64_t size, const char *ctype, const char *cache);
bool media_sub_vtt(const MediaFile *sub, Buf *out); /* subtitle file as WebVTT */
bool media_http(Ctx *c, Conn *conn, HttpReq *r);
/* Worker-thread body: send `head`, then bytes [from, from+len) of path, then close s. */
void media_stream_run(sock_t s, const char *path, int64_t from, int64_t len, const char *head, size_t head_len);
int media_streams_active(void);
void media_stream_stop(void);
void media_stream_resume(void);

/* ---- per-person progress ---- */
typedef struct {
    int id;
    int person;
    char item[17]; /* hex media id */
    int pos, dur;  /* seconds */
    bool done;
    int64_t updated;
    uint64_t place; /* hash64 of the video's path below its library folder: how the history finds the
                       video again when the folder moves (issue #36; 0 = not known yet) */
    uint64_t fp;    /* ... its content (MediaFile.fp), when it was renamed or moved within (issue #93) */
    uint64_t ident; /* ... what it is by name (media_ident): film + year, show + season + episode */
    char label[100]; /* ... and what to call it when none of that finds it ("Harbour Lights (2021)") */
    int64_t saved;  /* memory only: last write to SQLite */
} MediaWatch;
extern Table T_media_watch;
MediaWatch *media_watch_get(int person, uint64_t id);
/* Record playback progress; writes to SQLite at most every 15 s per item unless
 * it just became done. Returns true when the done state changed. */
bool media_watch_report(int person, uint64_t id, int pos, int dur, bool ended, int64_t now);
void media_watch_set_done(int person, uint64_t id, bool done, int64_t now);
/* An original gave way to its copy (remove.c): everyone's progress and watched ticks move to it. */
void media_watch_move(uint64_t from, uint64_t to);
/* What a video is by name, for finding its history again (0 = nothing to go by), and what to call it. */
uint64_t media_ident(const MediaName *n);
void media_label(const MediaName *n, char *out, size_t cap);
/* History whose video isn't in the library, after the last scan did what it could (issue #93): how
   many videos, and up to max of their labels ("" = from before labels). *away: a library folder is
   missing or empty (an unplugged disk) - its videos may well come back. */
int media_history_lost(char labels[][100], int max, bool *away);
int media_history_forget(void); /* drops it (nothing away): how many videos' history went */

#endif
