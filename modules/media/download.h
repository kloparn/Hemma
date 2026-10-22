/* modules/media/download.h — taking videos home: one video, or a season or a whole series as one .zip.
 *
 * For grown-ups only (owner, 2026-10-21): members and admins - not children, guests or display accounts
 * (a TV, a wall). A download is the version that plays everywhere (media_playable: the converted copy
 * when there is one), named for people: "Film (2020).mp4", "Show S01E03 - Title.mp4". A zip holds
 * "<Show>/Season <N>/<episode>" for each main episode (no play-alls, bonus clips or extras) and its
 * subtitles, stored as they are (zip.c), streamed from disk with its exact length up front.
 *
 * Downloads give way to people watching (owner's review, 2026-10-21: on a Pi 3B+ the USB disk and the
 * network share one USB 2 bus): while any ordinary video stream runs, each download sends at most
 * MEDIA_DOWNLOAD_SLOW bytes a second; at full speed when nobody watches (stream.c, checked every second).
 *
 * download.c: who, names, what a zip holds, sizes, the buttons. stream.c: the endpoints
 * GET|HEAD /media/file/<id>?download=1 and /media/zip/<series id>[?s=<season>]. Module-internal. */
#ifndef HEMMA_MEDIA_DOWNLOAD_H
#define HEMMA_MEDIA_DOWNLOAD_H

#include "library.h"

#define MEDIA_MAX_ZIPS 2                     /* zips at once (more: 503 + Retry-After) */
#define MEDIA_DOWNLOAD_SLOW (4LL << 20)      /* bytes a second for each download while someone watches */

/* A member or an admin (signed in): who may download. */
bool media_grown_up(const Ctx *c);

/* A name any computer can save: \ / : * ? " < > | and control characters go (": " -> " - ", "/" -> "-",
   '"' -> "'"), no leading or trailing spaces or dots, Windows' device names get a "_" ("Con_"); "video" when
   nothing is left. UTF-8 stays. */
void media_safe_name(const char *in, char *out, size_t cap);
/* What a download of f is called: a film "Title (Year).ext" (" - Part 2" for a second part), an episode
   "Show S01E03[-E04][ - Title].ext", anything else its own file name; ext = what goes out (".mp4" for a
   copy). Safe (media_safe_name). */
void media_download_name(const MediaFile *f, char *out, size_t cap);
/* "Content-Disposition: attachment; filename="<ASCII>"; filename*=UTF-8''<the name>" + CRLF */
void media_content_disposition(Buf *out, const char *name);

/* What a zip of series s holds (season < 0 = every season): each main episode's playable file, then its
   subtitles, in episode order, every name unique. A new array (free() it) or NULL; *n = how many. */
#define MEDIA_ZIP_NAME 800
typedef struct {
    uint64_t file; /* the MediaFile that goes in */
    int64_t size;  /* as the library has it (the download looks at the disk again) */
    char name[MEDIA_ZIP_NAME]; /* "Show/Season 1/Show S01E01 - Pilot.mp4", "Show/Season 1/Show S01E01.sv.srt" */
} MediaZipItem;
MediaZipItem *media_zip_items(const MediaSeries *s, int season, int *n);
void media_zip_name(const MediaSeries *s, int season, char *out, size_t cap); /* "Show - Season 1.zip", "Show.zip" */
uint64_t media_zip_bytes(const MediaSeries *s, int season); /* the zip's exact size by the library; 0 = nothing in it */

/* The buttons (grown-ups, never in TV mode): the watch page's "Download (1.2 GB)", the series page's
   "Download season 2 (8.2 GB)" and "Download all (34 GB)". */
void media_download_link(Ctx *c, Buf *b, const MediaFile *f);
void media_download_series(Ctx *c, Buf *b, const MediaSeries *s, int season);

/* stream.c: downloads running now (files and zips), zips among them, ordinary streams (people watching) */
int media_downloads_active(void);
int media_zips_active(void);
int media_watching_active(void);
/* tests: `zips` more zips than really run (0 = none); while someone watches, downloads send `cap` bytes a
   second (0 = MEDIA_DOWNLOAD_SLOW), and `watching` more streams than really run (0 = none) */
void media_zip_test_busy(int zips);
void media_download_test_pace(int64_t cap, int watching);

#endif
