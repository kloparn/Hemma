/* modules/media/mediainfo.h — understand media files without decoding them.
 *
 *  - names.c: movie / series / episode from a library-relative path
 *  - probe.c: container headers (MP4/MOV, Matroska/WebM, AVI) -> duration,
 *             resolution, codecs and whether a browser can play it
 *  - subs.c:  SRT -> WebVTT for <track>, plus subtitle/video file matching
 *
 * Pure leaf library: depends only on core/base.h and core/plat.h. */
#ifndef HEMMA_MEDIAINFO_H
#define HEMMA_MEDIAINFO_H

#include "../../core/base.h"

/* ------------------------------------------------------------------ names */

typedef enum { MEDIA_OTHER, MEDIA_MOVIE, MEDIA_EPISODE } MediaKind;

typedef struct {
    MediaKind kind;
    char title[128];                  /* cleaned movie title or series name: "The Office", "Spirited Away" */
    int year;                         /* 0 = unknown */
    int season, episode, episode_end; /* episode_end for S01E01E02 / S01E01-E02, else = episode */
    char ep_title[128];               /* text after SxxEyy when it looks like a title, cleaned */
    bool extra;                       /* sample, trailer or bonus material (Extras/, -trailer, sample) */
    bool sample; /* an extra only because the name starts with "sample": a scene's preview clip - or a
                    real video called that, when it runs long (issue #28: library.c asks the probe) */
    bool named;  /* an extra only by its name's ending ("Film-trailer", "film.sample"): a real video when it
                    runs long, or its length can't be read (issue #94) */
    /* disc rips ("Show/Season 1/Disc - 2/B3_t03.mkv"): an episode known only by its folders */
    bool unnumbered; /* episode 0 until the library numbers it (disc, then title order) */
    int disc;        /* from a "Disc - 2" / "DVD 2" folder; 0 = none */
    int track;       /* MakeMKV title number ("B3_t03" -> 3, "title00" -> 0); -1 = none */
} MediaName;

/* rel = path relative to the library root with '/' separators, e.g.
   "Series/The Office (US)/Season 02/The.Office.US.S02E05.Halloween.720p.WEB-DL.x264.mkv" */
void media_parse_name(const char *rel, MediaName *out);
/* Same, knowing the root folder's own name ("70's show" for a root at ".../70's show"): it names
   the show when the root itself is a show (or season) folder. NULL/"" = unknown. */
void media_parse_name_in(const char *root_name, const char *rel, MediaName *out);
/* The same with the name rules for extras off: for a video named like one that runs too long to be one. */
void media_parse_name_long(const char *root_name, const char *rel, MediaName *out);
/* Folder roles: a season folder ("Season 2", "season 1 70's show", "S01", "Specials" = 0) or a
   disc folder ("Disc - 1", "DVD 2", "D1") returns its number, anything else -1. */
int media_season_folder(const char *name);
int media_disc_folder(const char *name);
/* Which part of a film a file is, from its own name: "Film (2003) CD2.mkv", "Part 2", "pt1", "Disc 1",
   "Del 2", "Part Two" -> 2, 2, 1, 1, 2, 2; 0 = none. (The title leaves these out: "Film".) */
int media_name_part(const char *rel);

/* ---------------------------------------------------------------- probing */

typedef enum { PLAY_UNKNOWN, PLAY_YES, PLAY_MAYBE, PLAY_NO } Playable;

typedef struct {
    char container[8]; /* "mp4" "mov" "mkv" "webm" "avi" "ts" "wmv" "mpg" ("" = unknown) */
    int64_t duration_ms;
    int width, height;
    char vcodec[12], acodec[12]; /* h264 hevc av1 vp9 vp8 mpeg4 mpeg2 vc1 mjpeg prores ...
                                    aac mp3 ac3 eac3 dts truehd opus vorbis flac pcm alac ... */
    int audio_channels;
    Playable playable; /* in a typical browser */
    char why[96];      /* short human reason when not PLAY_YES */
} MediaProbe;

/* Random-access byte source. read_at must fill exactly n bytes or return false. */
typedef struct {
    void *ctx;
    int64_t size;
    bool (*read_at)(void *ctx, int64_t off, void *dst, size_t n);
} MediaReader;

/* Returns true when the container was recognised and its header parsed.
   `out` is always filled (best effort) — including playable/why. Only
   headers are read (moov box, Matroska Info/Tracks, AVI hdrl), never the whole file. */
bool media_probe(const MediaReader *r, const char *filename_hint, MediaProbe *out);
bool media_probe_file(const char *path, MediaProbe *out); /* UTF-8 path */
bool media_probe_mem(const void *data, size_t n, const char *filename_hint, MediaProbe *out);
/* playable + why again from the container and codecs: a cached probe gets today's verdict and words */
void media_probe_judge(MediaProbe *p);

const char *media_mime(const char *container);    /* "video/mp4", "video/webm", "video/x-matroska", ... */
bool media_is_video_ext(const char *name);        /* .mp4 .m4v .mkv .webm .mov .avi .ts .m2ts .mts .wmv .mpg .mpeg */
bool media_is_sub_ext(const char *name);          /* .srt .vtt */
bool media_is_image_ext(const char *name);        /* .jpg .jpeg .png .webp (posters, folder art) */
const char *media_playable_label(Playable p);     /* "Plays in browsers", "May not play everywhere", ... */
void media_fmt_duration(int64_t ms, char *out, size_t cap); /* "1 h 42 min", "42 min", "45 s", "" */

/* -------------------------------------------------------------- subtitles */

/* SRT -> WebVTT. Handles BOMs, UTF-16, Windows-1252 (Swedish subs), CRLF,
   comma/dot millis, missing blank lines and garbage blocks. Keeps <i>/<b>/<u>,
   drops {\an8}-style ASS tags and <font>. Always writes the "WEBVTT\n\n"
   header; returns false when no cue could be read. */
bool srt_to_vtt(const char *src, size_t n, Buf *out);

/* Does subtitle file `sub` (e.g. "Movie.sv.forced.srt") belong to video file
   `video` ("Movie.mkv")? Plain file names, case-insensitive. On success
   copies a language code ("sv", "en", ... or "" when untagged) into lang. */
bool media_sub_for(const char *video, const char *sub, char *lang, size_t cap);

/* The .srt a subtitle track taken out of `video` (a path or name; a copy "x.web.mp4" counts as "x")
   is saved as: "<video without its extension>.<code>[.sdh][.forced].srt" - code = the track's
   language in two letters ("eng", "en-US" -> "en"), none for "und" or one we don't know; sdh when
   the track's name says so ("SDH", "hearing impaired", "CC"), forced when the track is. media_sub_for
   gives that file to the video. false when the name would be too long. */
bool media_sub_sidecar(const char *video, const char *lang, const char *name, bool forced, char *out, size_t cap);

#endif
