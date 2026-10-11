/* modules/av/flac.h — FLAC (Free Lossless Audio Codec) encoder.
 *
 * Lossless sound that every browser plays: in MP4 ('fLaC' + 'dfLa', see
 * mp4.h), as a plain .flac file, or through WebCodecs (codec "flac"). Hemma
 * uses it to replace sound browsers can't decode (Dolby AC-3 on DVDs) without
 * losing a single sample.
 *
 * Fixed-blocksize stream (RFC 9639): CONSTANT, VERBATIM, FIXED (orders 0-4)
 * and LPC subframes (Levinson-Durbin on a Tukey(0.5)-windowed autocorrelation,
 * order search up to 12, error-feedback coefficient quantisation), Rice and
 * Rice2 partitioned residuals with a parameter per partition and escape codes,
 * wasted-bits detection, and the four stereo modes (independent, left/side,
 * right/side, mid/side) chosen per frame by size. STREAMINFO carries the MD5
 * of the audio, so `flac -t` and players can verify it.
 *
 * Block size: 4096 samples (the FLAC default for 44.1/48 kHz) unless set. A
 * DVD's AC-3 frames hold 1536 samples; feeding them through flac_write() lets
 * the encoder cut 4096-sample frames regardless (fewer, better-compressed
 * frames and a smaller MP4 index than matching 1536).
 *
 * Pure library: libc only (math.h for the window), no globals, no OS calls:
 * builds for wasm32 and the Pi as-is. One Flac per stream and thread. */
#ifndef HEMMA_AV_FLAC_H
#define HEMMA_AV_FLAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLAC_STREAMINFO_BYTES 34
#define FLAC_HEADER_BYTES 42 /* "fLaC" + a metadata block header + STREAMINFO */
#define FLAC_MAX_CHANNELS 8

typedef struct {
    int rate;          /* Hz, 1..655350 */
    int channels;      /* 1..8, in WAVE order (L R C LFE Ls Rs ...); 2 gets stereo decorrelation */
    int bits;          /* bits per sample, 4..24 (16 for CD/DVD sound) */
    int block_size;    /* samples per channel per frame, 16..65535; 0 = 4096 */
    int max_lpc_order; /* 1..32 for LPC, 0 = default (12), -1 = FIXED predictors only (faster) */
} FlacConfig;

typedef struct Flac Flac;

Flac *flac_new(const FlacConfig *cfg); /* NULL on a bad config or out of memory */
void flac_free(Flac *f);

/* Most bytes one frame can take: size flac_encode_frame's output with it. */
size_t flac_frame_bound(const Flac *f);

/* Encode one frame of n samples per channel (1 <= n <= block size; only the
   last frame of a stream may be shorter). `pcm` is interleaved, right-justified
   signed samples of cfg.bits bits. Returns the frame's size in bytes, or -1
   (n out of range, or a sample that doesn't fit cfg.bits). */
long flac_encode_frame(Flac *f, const int32_t *pcm, int n, uint8_t *out, size_t cap);

/* The same, buffered: any number of samples in, whole frames out through
   `sink` (each frame is one MP4 sample of `samples` samples per channel). */
typedef void (*FlacSink)(void *ctx, const uint8_t *frame, size_t bytes, int samples);
bool flac_write(Flac *f, const int32_t *pcm, int n, FlacSink sink, void *ctx);
bool flac_finish(Flac *f, FlacSink sink, void *ctx); /* the last (shorter) frame, if any */

/* STREAMINFO as it stands. Frame sizes, the sample count and the MD5 are final
   once every frame is encoded; before that they read 0 ("unknown"), which is
   valid for an init segment. */
void flac_streaminfo(const Flac *f, uint8_t out[FLAC_STREAMINFO_BYTES]);
/* "fLaC" + the STREAMINFO block (flagged last): the start of a .flac file,
   and the WebCodecs AudioDecoder `description` for codec "flac". */
void flac_header(const Flac *f, uint8_t out[FLAC_HEADER_BYTES]);

uint64_t flac_samples(const Flac *f); /* samples per channel encoded so far */

/* Building blocks, exposed for tests: FLAC's CRC-8 (x^8+x^2+x+1) and CRC-16
   (x^16+x^15+x^2+1), both MSB first from 0, and MD5 (RFC 1321). */
uint8_t flac_crc8(const uint8_t *p, size_t n);
uint16_t flac_crc16(const uint8_t *p, size_t n);
void flac_md5(const void *p, size_t n, uint8_t out[16]);

#endif
