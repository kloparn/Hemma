/* modules/media/remove.h — admins delete a film (or another video) from the disk. */
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
