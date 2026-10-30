/* modules/av/mp4.h — MP4 (ISO BMFF, ISO/IEC 14496-12) writer: the container
 * every browser plays.
 *
 * Two shapes:
 *
 *  - Progressive "faststart" (default): ftyp, moov, mdat — the index comes
 *    first, so a browser starts at once and seeks with Range requests. Media
 *    is never buffered: samples go straight to the output as they arrive and
 *    only the index (about 4 bytes per sample) is kept in memory. The moov is
 *    known only at the end, so mp4_new() reserves room for it right after
 *    ftyp (a 'free' box; mp4_moov_bound() gives a safe size) and mp4_finish()
 *    writes it there with write_at — the mdat never moves. If the reservation
 *    turns out too small the moov is appended at the end instead (still a
 *    valid file; mp4_faststart() then reports false). The other route — mdat
 *    to a temporary file, then ftyp + moov + a copy of it — costs a second
 *    write of every byte; reserving avoids it. Needs a seekable output.
 *
 *  - Fragmented (MP4_FRAGMENTED): an init segment (ftyp + moov with mvex)
 *    from mp4_begin(), then moof + mdat fragments, one per mp4_fragment()
 *    (start each with a video keyframe). Only the current fragment is held in
 *    memory and the output may be append-only — for remuxing on the fly.
 *
 * Tracks: H.264 ('avc1' + avcC, pasp, colr), HEVC ('hvc1' + hvcC - what Safari plays), AV1 ('av01' + av1C, AV1 in
 * ISOBMFF v1.2.0: samples are temporal units without their temporal delimiter), AAC ('mp4a' + esds), MP3 ('mp4a' + esds of
 * object type 0x6B or 0x69, one frame a sample - 2.5), FLAC
 * ('fLaC' + dfLa) and AC-3 ('ac-3' + dac3, for Safari). Samples are given in
 * decode order with a duration and a composition offset (pts - dts) in the
 * track's own timescale; interleave tracks by time (chunks form as tracks
 * alternate). Edit lists appear only when a track starts later or doesn't
 * start at media time 0. Files over 4 GB switch to co64 by themselves.
 *
 * Several sound tracks (2.2: the converter keeps every language): each its own trak in alternate group 1,
 * its language in mdhd, its name in hdlr, only the one that plays by default tkhd-enabled (Mp4Track.off on
 * the others). One found useless part-way is dropped (mp4_drop_track): left out of the moov, its bytes left in
 * the mdat unreferenced, which ISO/IEC 14496-12 allows.
 *
 * Pure library: libc only, no globals, no OS calls (wasm32 / Pi friendly). */
#ifndef HEMMA_AV_MP4_H
#define HEMMA_AV_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP4_MAX_TRACKS 10 /* a picture, the converter's AV_CONV_MAX_AUDIO (8) sounds and a spare */

typedef struct {
    void *ctx;
    bool (*write)(void *ctx, const void *p, size_t n);                 /* append n bytes */
    bool (*write_at)(void *ctx, int64_t off, const void *p, size_t n); /* overwrite earlier bytes (NULL: can't) */
} Mp4Out;

enum { MP4_H264 = 1, MP4_AAC, MP4_FLAC, MP4_AC3, MP4_HEVC, MP4_AV1, MP4_MP3 };

typedef struct {
    int codec;          /* MP4_H264, MP4_HEVC, MP4_AV1, MP4_AAC, MP4_MP3 (2.5: no config), MP4_FLAC or MP4_AC3 */
    uint32_t timescale; /* ticks per second: 30000 for 29.97 fps video, the sample rate for audio */
    /* avcC / hvcC record (HEVCDecoderConfigurationRecord, as MKV keeps it) / av1C record (AV1CodecConfigurationRecord,
       MKV's CodecPrivate too) / AudioSpecificConfig / FLAC STREAMINFO (34 bytes) / dac3 payload (3 bytes) */
    const uint8_t *config;
    size_t config_len;
    char lang[4]; /* ISO 639-2, lower case: "eng", "swe"; "" = "und" */
    const char *name; /* the hdlr's name - a sound's own ("English", "Kommentar"; UTF-8, copied, cut at 64 bytes on a
                         character's edge); NULL or "" = the usual "SoundHandler" / "VideoHandler" */
    bool off;         /* tkhd not enabled: a sound that doesn't play by default (another of its group does) */
    /* video */
    int width, height;              /* coded size */
    int sar_num, sar_den;           /* pixel aspect (DVD 4:3 = 8:9, 16:9 = 32:27); 0 = square */
    int primaries, transfer, matrix; /* 'colr' nclx (DVD NTSC: 6 6 6, HD: 1 1 1); 0 = left out */
    bool full_range;
    /* audio */
    int channels, sample_rate, sample_bits;
    uint32_t avg_bitrate, max_bitrate; /* AAC esds, 0 = unknown */
    /* timing, in this track's ticks */
    int64_t start; /* media time shown at movie time 0: the first pts when it isn't 0, or an AAC
                      encoder's priming (1024 samples) so players skip it and stay in sync */
    int64_t delay; /* nothing for this long before the track starts (audio that begins late) */
} Mp4Track;

#define MP4_FRAGMENTED 1

typedef struct Mp4 Mp4;

/* `reserve`: bytes kept after ftyp for the moov (progressive only; 0 = moov at the end). */
Mp4 *mp4_new(const Mp4Out *out, int flags, int64_t reserve);
void mp4_free(Mp4 *m);

int mp4_add_track(Mp4 *m, const Mp4Track *t); /* before mp4_begin; the track's index, or -1 */
/* Replace a track's codec config (copied): e.g. the final FLAC STREAMINFO.
   Progressive: any time before mp4_finish. Fragmented: before mp4_begin. */
bool mp4_set_config(Mp4 *m, int track, const uint8_t *config, size_t n);
bool mp4_begin(Mp4 *m); /* progressive: ftyp + reservation + mdat header; fragmented: the init segment */
/* Leave a track out of the moov (progressive: before mp4_finish; fragmented: before mp4_begin) - a sound that
   turned out undecodable part-way. Samples it already has stay in the mdat, referenced by nothing; it takes no
   more (mp4_sample: false). An enabled sound hands "enabled" to the first sound left, so the file keeps a
   default. The other tracks keep their ids. False when it can't: a bad index, too late, already dropped. */
bool mp4_drop_track(Mp4 *m, int track);

/* One sample in decode order. `cts` = pts - dts (0 without B-frames). */
bool mp4_sample(Mp4 *m, int track, const void *data, size_t n, uint32_t duration, int32_t cts, bool key);
bool mp4_fragment(Mp4 *m); /* fragmented: write out what is pending as moof + mdat */
bool mp4_finish(Mp4 *m);   /* progressive: the moov; fragmented: the last fragment */

int64_t mp4_bytes(const Mp4 *m);    /* bytes in the output so far */
bool mp4_faststart(const Mp4 *m);   /* after mp4_finish: the moov sits before the mdat */
/* A safe reservation for `samples` samples in `chunks` chunks, all tracks together. */
int64_t mp4_moov_bound(int64_t samples, int64_t chunks);

/* ---- helpers ---- */

/* avcC record from one SPS and one PPS (NAL units without start codes; 4:2:0 8-bit assumed for High profiles). */
size_t mp4_avcc(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len, uint8_t *out, size_t cap);
/* Annex B (00 00 01 start codes) -> 4-byte big-endian lengths, as MP4 samples need. 0 if cap is too small. */
size_t mp4_annexb_to_lengths(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/* The dac3 payload for an AC-3 track, from any of its syncframes. */
bool mp4_dac3(const uint8_t *frame, size_t n, uint8_t out[3]);

void mp4_set_co64_threshold(Mp4 *m, uint64_t offset); /* tests: chunk offsets above this use co64 */

#endif
