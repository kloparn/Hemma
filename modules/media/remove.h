/* modules/media/remove.h — admins delete a film (or another video), an episode, a season or a series from the disk. */
#ifndef HEMMA_MEDIA_REMOVE_H
#define HEMMA_MEDIA_REMOVE_H

#include "library.h"

typedef struct {
    char path[1024]; /* full path */
    char rel[400];   /* below the library folder, for the page */
    bool dir;
    int64_t size;
} RemoveItem;

typedef struct {
    char root[300];
    char folder[400];  /* the film's own folder (below the root), going as a whole; "" = only its files */
    RemoveItem *items; /* in deleting order: what's inside a folder before the folder */
    int n, cap, files;
    int64_t bytes;
} RemovePlan;

/* What removing f takes. Films and other videos only (episodes belong to their series);
   false = it can't be removed this way. Free the plan either way. */
bool media_remove_plan(const MediaFile *f, RemovePlan *p);
/* Delete what the plan lists, then folders left empty above it (never the library folder).
   Returns the files deleted; *failed counts what wouldn't go (in use, permissions). */
int media_remove_run(const RemovePlan *p, int *failed);
void media_remove_free(RemovePlan *p);

/* ---- an episode, a season or a whole series (the Manage view, manage.c) ----
   What goes is planned from the library's index (no disk work: the dialog shows it): the videos, their
   play-everywhere copies, subtitles and own pictures - for a season its extras too (a ripped disc's play-all
   and bonus clips), for a series also its picture. The item's OWN folders - a season's "Season 1", a show's
   folder whose name is the show's - go whole when nothing else of the library is in them (as a film's own
   folder does): the run walks them, and a video that isn't the item's (nor an extra by its name), an upload
   or conversion at work, a link anywhere inside, or more than PURGE_MAX entries keeps that folder. Folders
   left empty go too - never the library folder, nor a folder right in it ("Series"). */
#define PURGE_DIRS 8
typedef struct {
    int root;      /* index into the library's roots */
    char rel[400]; /* the folder below it */
} PurgeDir;

typedef struct {
    uint64_t *ids; /* every library file that goes, each once, sorted */
    int n, cap;
    int videos;    /* of which videos people watch (episodes and extras, not their copies) */
    int64_t bytes;
    PurgeDir dirs[PURGE_DIRS]; /* the item's own folders */
    int ndirs;
} MediaPurge;

bool media_purge_episode(const MediaFile *ep, MediaPurge *p); /* an episode, or one of a series' extras */
bool media_purge_season(const MediaSeries *s, int season, MediaPurge *p);
bool media_purge_series(const MediaSeries *s, MediaPurge *p);
void media_purge_free(MediaPurge *p);

/* The run: paths taken from the index on the event loop (media_purge_job), the deleting on any thread
   (media_purge_run touches only the disk) - a series of disc rips is a thousand files. */
typedef struct {
    int root;
    char rel[400];
    int64_t size;
    uint64_t id;
    bool video; /* (its watch history goes when it's gone) */
    bool gone;  /* the run: deleted, or not there any more */
} PurgeFile;

typedef struct {
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots;
    PurgeFile *files;
    int nfiles;
    PurgeDir dirs[PURGE_DIRS];
    int ndirs;
    uint64_t *belong; /* sorted: the ids the own folders may hold */
    int nbelong;
    int deleted, failed; /* the run's results: files deleted (also those left in an own folder), and those that wouldn't go */
    int64_t freed;
} PurgeJob;

PurgeJob *media_purge_job(const MediaPurge *p);
void media_purge_run(PurgeJob *j);
void media_purge_job_free(PurgeJob *j);

/* The folder a path's show lives in: its folders without the season and disc folders at the end
   ("Show/Season 1/Disc - 2/x.mkv" -> length of "Show"; 0 = right in the library folder). */
size_t media_show_dir_len(const char *rel);
/* Does that folder (dir[0..dl) below library folder `root`; dl 0 = the library folder itself) name the
   show `title` - the way the library names a show from its folder? A category folder ("Series"), a
   download folder or another show's doesn't. */
bool media_names_show(const char *root, const char *dir, size_t dl, const char *title);
/* A library folder's own name ("70's show" for "/mnt/media/70's show"), as the library names a show from
   it; "" for a drive. */
const char *media_root_name(const char *root);

/* Two versions of one video: x.mkv and its play-everywhere copy x.web.mp4, of the same length
   (within 2 s or 1% - a cut-off copy never counts). The one that plays in fewer places is the
   lesser (on a tie, the original): what gets deleted, while the better one stays. A copy keeps the
   picture and one sound track; other sound tracks go with the original. */
int media_compat_cost(const MediaProbe *p); /* how far from playing everywhere: 0 = MP4, H.264, AAC */
bool media_copy_lasts(int64_t original_ms, int64_t copy_ms);
const MediaFile *media_lesser_version(const MediaFile *video, const MediaFile **better); /* NULL = no pair */
/* Delete the lesser version (event loop only). When that's the original, everyone's progress and
   watched ticks move to the copy. */
bool media_drop_version(const MediaFile *lesser, const MediaFile *better);
bool media_drop_original(const char *path, uint64_t original, uint64_t copy);

#endif
