/* modules/av/mkv.h — Matroska / WebM demuxer.
 *
 * mkv_open() reads the headers (EBML header, Info, Tracks and Cues, following
 * the SeekHead when they live at the end of the file); mkv_next() then hands
 * out frames in file order: SimpleBlock and BlockGroup, every lacing kind
 * (Xiph, EBML, fixed), unknown-size Segments and Clusters (live/streamed
 * files) and header stripping. mkv_seek() uses Cues when there are any and
 * otherwise bisects the file for Clusters.
 *
 * Pure library: libc only (no core/), no mutable globals — one Mkv per thread,
 * any number of them. Input is random access through MkvReader, so it works on
 * files, memory or anything else. Corrupt input never crashes or loops: every
 * size is bounded and damaged data is skipped by resynchronising on the next
 * Cluster.
 *
 * 2.5: an MP4, M4V or MOV file opens too, behind the same calls (mkv_mp4.h): its tracks named as Matroska names
 * their codecs, its samples handed out in file order with their presentation times. */
#ifndef HEMMA_AV_MKV_H
#define HEMMA_AV_MKV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    int64_t size; /* total bytes */
    /* Copy exactly n bytes at offset off into dst; false on error or short read. */
    bool (*read_at)(void *ctx, int64_t off, void *dst, size_t n);
} MkvReader;

enum { /* TrackType */
    MKV_VIDEO = 1,
    MKV_AUDIO = 2,
    MKV_COMPLEX = 3,
    MKV_LOGO = 16,
    MKV_SUBTITLE = 17,
    MKV_BUTTONS = 18,
    MKV_CONTROL = 32,
    MKV_METADATA = 33,
};

#define MKV_MAX_TRACKS 64
#define MKV_NOPTS INT64_MIN

typedef struct {
    int number; /* TrackNumber, as used by blocks */
    uint64_t uid;
    int type;       /* MKV_VIDEO, MKV_AUDIO, MKV_SUBTITLE, ... (0 = unknown) */
    char codec[64]; /* CodecID: "V_MPEG2", "V_MPEG4/ISO/AVC", "A_AC3", "S_VOBSUB", ... */
    char lang[24];  /* LanguageIETF when present, else Language ("eng" when absent) */
    char name[128];
    bool enabled, is_default, forced;
    const uint8_t *priv; /* CodecPrivate (NULL when absent) */
    size_t priv_len;
    int64_t default_duration; /* ns per frame, 0 = unknown */
    int64_t codec_delay;      /* ns */
    /* video */
    int width, height;           /* PixelWidth / PixelHeight */
    int disp_width, disp_height; /* DisplayWidth / DisplayHeight (0 = not given) */
    int disp_unit;               /* 0 pixels, 1 cm, 2 inches, 3 aspect ratio */
    int interlaced;              /* FlagInterlaced: 0 unknown, 1 interlaced, 2 progressive */
    /* audio */
    double rate; /* SamplingFrequency (Hz) */
    int channels, bits;
    /* content encoding */
    const uint8_t *strip; /* header stripping: these bytes are put back in front of every frame */
    size_t strip_len;
    bool compressed; /* zlib/bzlib/lzo compressed or encrypted: frames are not usable as they are */
    char fourcc[5];  /* 2.5, an MP4/MOV's: its sample entry ("avc1", "hvc1", "mp4a", "ac-3" ...); "" in Matroska */
    int64_t end;     /* 2.6.1, an MP4/MOV's: where its edit list stops showing it (ns, on the frames' pts clock) - a frame
                        at or past it is there to be decoded, not shown (an encoder's last pictures, a cut); 0 = none */
} MkvTrack;

typedef struct {
    int track;        /* TrackNumber */
    int64_t pts;      /* ns; MKV_NOPTS for the later frames of a lace when the track has no DefaultDuration */
    int64_t duration; /* ns, 0 = unknown */
    bool key, invisible, discardable;
    const uint8_t *data; /* valid until the next mkv_next / mkv_seek / mkv_rewind / mkv_close */
    size_t size;
    int64_t pos; /* file offset of the (Simple)Block element */
} MkvFrame;

typedef struct Mkv Mkv;

/* Reads the headers. NULL when this is not Matroska/WebM or has no tracks (err says why). */
Mkv *mkv_open(const MkvReader *r, char *err, size_t errcap);
void mkv_close(Mkv *m);

const char *mkv_doctype(const Mkv *m); /* "matroska" or "webm" - or (2.5) "mp4" / "mov" */
const char *mkv_title(const Mkv *m);
const char *mkv_writing_app(const Mkv *m);
int64_t mkv_duration(const Mkv *m); /* ns, 0 = unknown */
int mkv_track_count(const Mkv *m);
const MkvTrack *mkv_track(const Mkv *m, int i); /* 0 <= i < mkv_track_count() */
const MkvTrack *mkv_track_by_number(const Mkv *m, int number);
const MkvTrack *mkv_find_track(const Mkv *m, int type); /* the default one of that type, else the first */
int mkv_cue_count(const Mkv *m);

/* 1 = a frame, 0 = end of file, -1 = the reader failed. Damaged data is skipped. */
int mkv_next(Mkv *m, MkvFrame *f);
/* Position at the last Cue (or Cluster) at or before t_ns for `track` (0 = the
   first video track). Frames then continue from there: skip until a keyframe
   of your track. False when the file has no clusters. */
bool mkv_seek(Mkv *m, int64_t t_ns, int track);
void mkv_rewind(Mkv *m); /* back to the first cluster */

#endif
