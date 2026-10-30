/* modules/av/conv.h — the "play everywhere" converter: one MKV in, one MP4 out
 * that every browser, iPhone, iPad, TV and Chromecast plays.
 *
 *   MPEG-2 / MPEG-1 (DVD rips) -> mpeg2 -> deint -> h264enc  \
 *   H.264                      -> copied as it is             >  mp4 (faststart)
 *   HEVC (x265 films)          -> copied as 'hvc1'           /   (plays in Safari; other browsers need HEVC hardware)
 *   AV1 (web releases)         -> copied as 'av01'          /    (Chrome, Edge, Firefox; Safari where the device has AV1)
 *   Dolby AC-3 / E-AC-3 (DD+)  -> ac3 (stereo) -> aac         /
 *   DTS (the core)             -> dts (stereo) -> aac        /
 *   AAC                        -> copied                    /   every sound track, each its own (AV_CONV_MAX_AUDIO)
 *   text subtitles (SRT, ASS,  -> SubRip texts handed back (want_subs): the caller saves them as
 *   WebVTT)                       "<name>.<language>.srt" next to the video, where the library finds them
 *   picture subtitles: Blu-ray -> the bytes of a .sup file (PGS) / of an .idx + .sub pair (VobSub),
 *   PGS, DVD VobSub               as mkvextract writes them: VLC, mpv and Plex read them, Hemma's
 *                                 player draws them (browsers can't)
 *   Subtitles are timed on the copy's clock (0 = the file's first picture), so they fit the copy and the
 *   original alike. They also come out of a file the converter refuses (`unsupported`: VP9, TrueHD ...),
 *   and av_subtitles() takes them alone, for a video that is never converted.
 *
 * Frames stream through: memory stays a few MB whatever the file size (plus
 * ~4 bytes per sample for the MP4 index, and 16 per video frame when H.264 is
 * copied). Samples are bounded, so a file of tiny blocks can't blow that up: at
 * most 2 million a track (9 hours of 60 fps), and with a stated length at most
 * 300 pictures / 120 sound blocks a second of it; past that the job fails as
 * `unsupported`. So does an MPEG-2 stream whose picture size changes part-way
 * (later pictures smaller or bigger than the first). Subtitles are kept in memory
 * until the end, bounded: AV_CONV_SUB_BYTES a text track, AV_CONV_PIC_BYTES a
 * picture track and AV_CONV_PIC_TOTAL all of those (a DVD episode's VobSub is
 * ~1 MB, a Blu-ray film's PGS ~30 MB). Input is random access
 * (MkvReader), output appends and patches the reserved header at the end
 * (Mp4Out with write_at).
 *
 * Pure library: libc only, no globals, no OS calls. The same code runs in the
 * server (media/convert.c, files on disk) and in the uploading browser
 * (wasm/avconv.c compiled to WebAssembly: the copy is made on the device that
 * holds the original, and only the copy travels). One job per thread. */
#ifndef HEMMA_AV_CONV_H
#define HEMMA_AV_CONV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mkv.h"
#include "mp4.h"

#define AV_CONV_MAX_SUBS 32              /* subtitle tracks taken from one file (the first 32 it takes, in file order) */
/* Sound tracks kept (2.2; owner, 2026-10-30: "not language locked"): every one the converter takes, in the
   file's order, each its own MP4 trak (its language in mdhd, its name in hdlr) - the first 8, the default one
   among them. The default (the first flagged so, else the first) is the one tkhd-enabled; the server hands a
   viewer one of them (media_view, mediainfo.h). A track that goes undecodable part-way is dropped while another
   lives. */
#define AV_CONV_MAX_AUDIO 8

/* Every format the converter takes, by Matroska CodecID - the one place its checks look (av_conv_codec).
   Formats grow when real files need them (owner, 2026-10-21; docs/decisions/0006): a new one goes in this
   table, and conv_test.c's conv_fuzz_seeds then fails until a seed file carries it - so the fuzzing on
   every push and every night (tools/ci/fuzz.sh) covers it from its first day. */
typedef enum { AV_DECODE = 1, AV_COPY, AV_TEXT, AV_PICTURE } AvHow; /* decoded and re-encoded / copied as it is /
                                                                       a subtitle text / picture subtitles, as files */
typedef struct {
    const char *id; /* CodecID; with prefix, every CodecID starting so ("A_AAC/MPEG4/LC") */
    int type;       /* MKV_VIDEO, MKV_AUDIO, MKV_SUBTITLE */
    AvHow how;
    bool prefix;
} AvCodec;
extern const AvCodec AV_CONV_CODECS[]; /* ends with {NULL} */
const AvCodec *av_conv_codec(const char *codec_id); /* NULL: not one the converter takes */
#define AV_CONV_SUB_BYTES (768u << 10)   /* the most SRT one track may come to (a film's is ~100 KB) */
#define AV_CONV_PIC_BYTES (64u << 20)    /* the most one picture track may come to (.sup, or .sub; a film's PGS ~30 MB):
                                            a track past it is left out whole, never cut */
#define AV_CONV_PIC_TOTAL (256u << 20)   /* ... and every picture track of one file together */

typedef enum { AV_SUB_TEXT = 0, AV_SUB_PGS, AV_SUB_VOBSUB } AvSubKind;

typedef struct {
    char lang[24];        /* the track's language as the file names it: "eng", "en-US", "und" */
    char name[128];       /* its name, if any ("English (SDH)") */
    bool forced, is_default;
    AvSubKind kind;
    int cues;             /* cues (text) / subpictures (pictures) */
    char *srt;            /* AV_SUB_TEXT: SubRip, UTF-8, NUL-terminated, timed like the copy */
    size_t len;
    uint8_t *data;        /* AV_SUB_PGS: the whole .sup file; AV_SUB_VOBSUB: the .sub file (MPEG-PS, 2048-byte packs) */
    size_t size;
    char *idx;            /* AV_SUB_VOBSUB: the .idx text (NUL-terminated): the track's own header (size, palette ...),
                             then "id: <2-letter language>, index: 0" and a timestamp/filepos line per subpicture */
    size_t idx_len;
} AvConvSub; /* av_convert_free frees srt, data and idx */

typedef struct {
    MkvReader in;
    Mp4Out out;           /* needs write_at: the index is written into room reserved up front */
    int quality;          /* H.264 QP (lower = better, bigger); 0 = 23 */
    int audio_kbps;       /* AAC bitrate; 0 = 160 */
    int64_t limit_ns;     /* stop after this much video (previews, tests); 0 = the whole file */
    int64_t max_samples;  /* most samples a track may get, if fewer than the built-in bound (tests, fuzzing); 0 = that */
    size_t pic_bytes;     /* the most one picture track may come to, if less than AV_CONV_PIC_BYTES (tests, fuzzing);
                             0 = that. Every picture track together: AV_CONV_PIC_TOTAL / AV_CONV_PIC_BYTES times it */
    bool want_subs;       /* hand back the subtitle tracks (subs) */
    bool skip_refused_subs; /* with want_subs: no subtitles-only pass after an `unsupported` refusal (the browser:
                               the original goes to the server then, which takes them out itself) */
    volatile int *stop;   /* non-zero: give up soon */
    /* called whenever permille changes (may be NULL) */
    void (*progress)(void *ctx, int permille);
    void *progress_ctx;
    /* results */
    volatile int permille; /* 0..1000 */
    char err[200];         /* why it failed: words for people ("Its sound is DTS, which ...") */
    char what[120];        /* what was done: "DVD video (film, 23.976 fps) → H.264 · Dolby Digital → AAC" */
    int64_t out_bytes;
    bool unsupported;      /* failed because of what the file holds (a codec we don't convert yet), not an error */
    AvConvSub subs[AV_CONV_MAX_SUBS]; /* with want_subs, after a success or an `unsupported` refusal: the
                                         subtitle tracks in file order (empty ones left out) */
    int nsubs;
} AvConvJob;

/* Blocking. True when the output is complete. Starts with nsubs = 0: free a previous run's first. With
   want_subs, a file it refuses as `unsupported` is read once more for its subtitles alone (as av_subtitles):
   j->err stays the refusal. A real error (stopped, unreadable, disk full) gives none. */
bool av_convert(AvConvJob *j);
/* Only the subtitle tracks: reads j->in, honours stop/progress, writes nothing to j->out - timed exactly as a
   conversion would time them. True when the file could be read (it may hold none: nsubs 0); false + j->err
   when not. Free with av_convert_free. */
bool av_subtitles(AvConvJob *j);
void av_convert_free(AvConvJob *j); /* the subtitles' memory */

#endif
