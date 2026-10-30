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
    /* a collection's order number in front of a film's (or other video's) name, in tenths: "1  Aladdin" 10,
       "1.5. The Lion King (Hakuna Matata)" 15, "2 - Title" 20 - left out of the title (2.4); 0 = none */
    int order;
} MediaName;

/* rel = path relative to the library root with '/' separators, e.g.
   "Series/The Office (US)/Season 02/The.Office.US.S02E05.Halloween.720p.WEB-DL.x264.mkv" */
void media_parse_name(const char *rel, MediaName *out);
/* Same, knowing the root folder's own name ("70's show" for a root at ".../70's show"): it names
   the show when the root itself is a show (or season) folder. NULL/"" = unknown. */
void media_parse_name_in(const char *root_name, const char *rel, MediaName *out);
/* The same with the name rules for extras off: for a video named like one that runs too long to be one. */
void media_parse_name_long(const char *root_name, const char *rel, MediaName *out);
/* The same read as a film (2.4): as if a Movies folder held it - for a video an hour long or more that names no
   episode (library.c), or one an upload's "These are films" took (upload_sort.c). A home-videos folder still wins. */
void media_parse_name_film(const char *root_name, const char *rel, MediaName *out);
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
    int nsounds;       /* sound tracks in the file (2.2: an MP4 with 2+ is served one at a time - media_view) */
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
/* p->why (kept in English) in the language in use (core/i18n.h), for a page */
void media_probe_why(const MediaProbe *p, char *out, size_t cap);

/* ------------------------------------------------- sound tracks (2.2) */

/* SOUND LANGUAGES (2.2; owner, 2026-10-30: "I want hemma to be not 'language locked'"): the converter keeps every
   sound track of an MKV in its MP4 copy (modules/av/conv.c: video first, then the sounds in the MKV's order, each with
   its language in mdhd and its name in hdlr, only the file's default one tkhd-enabled), and the server hands each
   viewer an MP4 holding just the one they listen to (media_view) - browsers play one sound track, and only Safari
   could switch between several. */
#define MEDIA_MAX_SOUNDS 8
typedef struct {
    char lang[8];   /* two letters, lower case ("en", "sv", "ru"; "eng"/"en-US" in the file -> "en"); "" = not said / "und" */
    char name[48];  /* the track's own name ("MVO, Fox, iTunes"); "" = none (handler names such as "SoundHandler" left out) */
    char codec[12]; /* as MediaProbe.acodec: "aac", "ac3" ... */
    int channels;
    bool on;        /* the file's default: tkhd enabled (the first sound track when none or several are) */
} MediaSound;

/* An MP4/MOV's sound tracks ('soun' handlers) in file order, at most max of them in out. Returns how many there
   are in all; -1 = not an MP4 or its moov unreadable. Reads only the box headers and the moov. */
int media_mp4_sounds(const MediaReader *r, MediaSound *out, int max);

/* A multi-sound MP4 seen with ONE sound track: a new ftyp + moov in memory (the other sound traks left out, the
   chosen one enabled, chunk offsets moved - stco becomes co64 when it must), then the file's own other top-level
   boxes (mdat ...) as they are, the other sounds' bytes still in there, unreferenced (allowed: 14496-12). Works on
   any MP4 whose moov parses, moov first or last; not on fragmented ones (mvex). */
typedef struct MediaView MediaView;
/* sound = index among the sound tracks (media_mp4_sounds' order); -1 or out of range = the default one. NULL + err. */
MediaView *media_view_new(const MediaReader *r, int sound, char *err, size_t cap);
void media_view_free(MediaView *v);
int64_t media_view_size(const MediaView *v);
/* What the view holds at `off`: bytes in memory (mem != NULL) or bytes of the file from file_off, len of them
   (len > 0 until the end; then false). A reader of the view loops over these parts. */
typedef struct {
    const uint8_t *mem;
    int64_t file_off, len;
} MediaViewPart;
bool media_view_part(const MediaView *v, int64_t off, MediaViewPart *out);

const char *media_mime(const char *container);    /* "video/mp4", "video/webm", "video/x-matroska", ... */
bool media_is_video_ext(const char *name);        /* .mp4 .m4v .mkv .webm .mov .avi .ts .m2ts .mts .wmv .mpg .mpeg */
bool media_is_sub_ext(const char *name);          /* .srt .vtt .sup .idx (a VobSub's .sub: media_is_vobsub_data) */
bool media_is_image_ext(const char *name);        /* .jpg .jpeg .png .webp (posters, folder art) */
/* AUDIOBOOKS (1.6): .m4b .m4a .mp3 - what they come as (every browser plays AAC in MP4, and MP3) */
bool media_is_audio_ext(const char *name);
/* A folder whose name says it holds audiobooks: "Audiobooks", "Audio books", "Ljudböcker", "Audiolibros",
   "Hörbücher" ... (any case; '_', '.', '-' as spaces) */
bool media_is_books_folder(const char *name);
/* An audio file the library takes as (part of) an audiobook: an .m4b anywhere; an .m4a or .mp3 only below a
   books folder (media_is_books_folder - any folder above it, or the library folder itself: root_name, its own
   name, NULL/"" = unknown) - so a show's theme.mp3 or a film's soundtrack stays out. rel: below the library folder. */
bool media_is_book_file(const char *root_name, const char *rel);
const char *media_playable_label(Playable p);     /* "Plays in browsers", "May not play everywhere", ... (English: T() it) */
void media_fmt_duration(int64_t ms, char *out, size_t cap); /* "1 h 42 min", "42 min", "45 s", "" */

/* -------------------------------------------------------------- subtitles */

/* SRT -> WebVTT. Handles BOMs, UTF-16, Windows-1252 (Swedish subs), CRLF,
   comma/dot millis, missing blank lines and garbage blocks. Keeps <i>/<b>/<u>,
   drops {\an8}-style ASS tags and <font>. Always writes the "WEBVTT\n\n"
   header; returns false when no cue could be read. */
bool srt_to_vtt(const char *src, size_t n, Buf *out);

/* Subtitle files: text (.srt .vtt), Blu-ray PGS pictures (.sup) and DVD VobSub pictures (.idx, whose
   pictures are in the .sub of the same name beside it - the library indexes such a .sub with its .idx,
   never on its own: a .sub alone is MicroDVD text, which Hemma doesn't read). */
bool media_is_sub_pic(const char *name);     /* .sup .idx .sub: pictures, which a browser can't show as a <track> */
bool media_is_vobsub_data(const char *name); /* .sub: an .idx's pictures */

/* What a subtitle's name says beyond its video's: "Movie.en.sdh.2.srt" -> en, sdh, the second. */
typedef struct {
    char lang[16]; /* "en", "sv" ... or "" when it doesn't say */
    bool sdh;      /* for the deaf and hard of hearing: "sdh", "cc", "hi" */
    bool forced;   /* only what's said in another language (and signs): "forced", "foreign" */
    int n;         /* 2, 3 ...: the second, third track of one video in the same language and flags; 0 = the first */
} MediaSubTags;

/* Does subtitle file `sub` (e.g. "Movie.sv.forced.srt", "Movie.en.2.sup", "Movie.en.idx", a VobSub's
   "Movie.en.sub") belong to video file `video` ("Movie.mkv"; "Movie.web.mp4" counts as "Movie")? Plain
   file names, case-insensitive. A number counts only after a language ("Movie.en.2.srt"; "Movie 2.srt"
   is another video's), "und" is a language that names none. */
bool media_sub_tags(const char *video, const char *sub, MediaSubTags *t);
/* The same, giving only the language code ("sv", "en", ... or "" when untagged). */
bool media_sub_for(const char *video, const char *sub, char *lang, size_t cap);
/* The variant as a key, unique among one video's subtitles: "en", "en.sdh", "en.forced", "en.sdh.forced",
   "en.2", "und" (no language). Letters, digits and dots only. */
void media_sub_key(const MediaSubTags *t, char *out, size_t cap);

/* A subtitle track's tags from what a file says of it: its language as the file names it ("eng",
   "en-US" -> "en"; none for "und" or one we don't know), sdh when its name says so ("SDH", "hearing
   impaired", "CC"), forced when it is (or its name says so). n = 0. */
void media_sub_track_tags(const char *lang, const char *name, bool forced, MediaSubTags *t);
/* "<video without its extension>[.<lang>][.sdh][.forced][.<n>].<ext>" ("x.web.mp4" counts as "x"; a
   numbered one with no language is "x.und.2.srt", so media_sub_tags takes it back). false = too long. */
bool media_sub_name(const char *video, const MediaSubTags *t, const char *ext, char *out, size_t cap);
/* What a subtitle track taken out of `video` (a path or name) is saved as: media_sub_name of the
   track's tags (media_sub_track_tags), ext "srt" / "sup" / "idx" (NULL or "" = "srt"), n = 2, 3 ... for
   the second, third track of one file in one language and flags (0 or 1 = the first). media_sub_for
   gives that file to the video. false when the name would be too long. */
bool media_sub_sidecar(const char *video, const char *lang, const char *name, bool forced, const char *ext, int n,
                       char *out, size_t cap);

#endif
