/* modules/media/room.h — room on the library's disks for what uploads and conversions write.
 *
 * Neither fills a disk to its last gigabyte (MEDIA_RESERVE): the database often lives on the
 * same disk (a Pi serving its library from its SD card). Nor does one count on room another
 * still needs: a running upload holds what it has left to send, a running conversion the size
 * of its original (the most a copy can take) minus what it has written so far.
 * Module-internal (upload.c, convert.c). */
#ifndef HEMMA_MEDIA_ROOM_H
#define HEMMA_MEDIA_ROOM_H

#include "library.h"

#define MEDIA_RESERVE (1LL << 30)

/* What a running upload or conversion may still write below a library folder. */
typedef struct {
    char root[300];
    int64_t bytes;
} MediaHold;
#define MEDIA_MAX_HOLDS 24 /* 12 conversions, 4 uploads, and then some */

/* Spare room for something new below library folder `root`: free space minus the reserve minus
   the holds on the same disk (*held gets their sum; may be NULL). Huge when the disk can't say. */
int64_t media_room(const char *root, const MediaHold *holds, int n, int64_t *held);
/* "the drive has 2.0 GB to spare (Hemma keeps 1 GB free)" - and what the running ones hold */
void media_room_text(char *out, size_t cap, int64_t spare, int64_t held);
void media_size_text(int64_t n, char *out, size_t cap); /* "720 MB", "1.2 GB" */
void media_test_free_space(int64_t bytes); /* tests: every disk has this much free (-1 = ask the disk) */
/* Free and total bytes of the disk holding path - or, under tests, what media_test_free_space set. */
bool media_disk_space(const char *path, int64_t *free_bytes, int64_t *total);

/* What the other side holds and claims (each takes its own lock; upload.c may call convert.c
   while holding its lock, never the other way round). */
int convert_holds(MediaHold *out, int max); /* running conversions */
int upload_holds(MediaHold *out, int max);  /* running uploads */
bool convert_making(const char *path); /* a running conversion reads or writes this file */
/* An upload brings this file (event loop): waiting conversions that would make it leave the
   queue - the upload's copy does instead. True when a running one makes it already: then the
   upload leaves it to the server. */
bool convert_give_way(const char *path);
bool upload_claims(const char *path); /* an upload writes this file now, or will soon (a live one) */
bool upload_claims_under(const char *dir); /* ... any file in that folder (a series being renamed: manage.c) */
/* The library scan found a "<copy>.conv.part": deleted unless a queued conversion is making it
   (a crash or power cut left it). Any thread. */
void convert_drop_leftover(const char *path);

#endif
