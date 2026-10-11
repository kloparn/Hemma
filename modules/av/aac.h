/* modules/av/aac.h — AAC-LC audio encoder (MPEG-4 Audio Object Type 2).
 *
 * The one audio format every browser and device plays: Safari and iOS,
 * Chrome, Firefox, Samsung TVs, Chromecast, AirPlay. The converter feeds it
 * PCM decoded from the DVDs' AC-3 (modules/av/ac3) and puts the access units
 * into MP4 (mp4a + esds, modules/av/mp4).
 *
 * What it does: MDCT 2048/256 with sine or KBD windows and block switching
 * (LONG / START / EIGHT_SHORT / STOP) driven by a transient detector one
 * frame ahead, grouped short windows, a psychoacoustic model (band energies,
 * Bark spreading, tonality, absolute threshold of hearing, pre-echo limit),
 * per-band M/S stereo that keeps Lt/Rt matrixed surround intact (both M and
 * S held to the stricter of the L/R thresholds), non-uniform quantisation
 * with a rate loop to a target bitrate and a bit reservoir, all eleven
 * spectral codebooks + escapes with trellis-optimal sectioning, and
 * differential scalefactors. No TNS, PNS or intensity stereo.
 *
 * Pure leaf library: libc only (math.h at setup), no core/ dependency, no
 * mutable globals and no OS calls, so it builds for wasm32 and the Pi
 * (aarch64, armv6) as-is. Each encoder (~140 KB, mostly lookup tables) is
 * independent: use one per stream and thread. */
#ifndef HEMMA_AV_AAC_H
#define HEMMA_AV_AAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AAC_FRAME 1024          /* PCM samples per channel in one access unit */
#define AAC_MAX_CHANNELS 2      /* mono (SCE) or stereo (CPE) */
#define AAC_MAX_AU (768 * AAC_MAX_CHANNELS) /* bytes: 6144 bits per channel */
#define AAC_PRIMING 1024        /* decoded output starts with this many samples of encoder delay */

typedef struct {
    int sample_rate;  /* 48000, 44100, 32000, 24000, 22050, 16000, 12000 or 11025 */
    int channels;     /* 1 or 2 */
    int bitrate;      /* bit/s for all channels together; 0 = 80 kbit/s per channel */
    int bandwidth;    /* Hz of audio kept; 0 = chosen from the bitrate */
    int window_shape; /* 0 = sine (default), 1 = Kaiser-Bessel derived */
} AacConfig;

typedef struct AacEnc AacEnc;

/* NULL (with a reason in err) for an unsupported rate or channel count. */
AacEnc *aac_open(const AacConfig *cfg, char *err, size_t errcap);
void aac_close(AacEnc *e);

/* AudioSpecificConfig for the MP4 esds box and WebCodecs ("mp4a.40.2"
   description): writes 2 bytes, returns 2. */
int aac_specific_config(const AacEnc *e, uint8_t out[2]);

/* Encode one block: AAC_FRAME samples per channel, interleaved floats in
   +-1.0 (louder is clipped at +-1.5, NaN/Inf become silence). Writes at most
   one raw access unit into out (cap >= AAC_MAX_AU) and returns its size in
   bytes: 0 = nothing yet (the encoder looks one block ahead), -1 = error.
   Access unit k decodes to samples [(k-1)*1024, k*1024) of the input: the
   first one is priming, so an MP4 edit list starts at AAC_PRIMING. */
int aac_encode(AacEnc *e, const float *pcm, uint8_t *out, int cap);

/* After the last block: call until it returns 0. Emits the remaining access
   units (two more than were returned so far), feeding silence. Every input
   sample is then covered: units = blocks + 1. */
int aac_flush(AacEnc *e, uint8_t *out, int cap);

/* ADTS header (no CRC) for a raw access unit of payload_bytes, for .aac
   files and tools. Returns 7, or 0 for an unsupported rate. */
int aac_adts_header(int sample_rate, int channels, int payload_bytes, uint8_t hdr[7]);

typedef struct {
    int frames;           /* access units produced */
    int short_frames;     /* of which used eight short windows */
    int ms_bands;         /* bands coded mid/side */
    int bandwidth;        /* Hz actually coded at most */
    int bits_min, bits_max;
    long long bits_total; /* sum of access unit sizes, in bits */
    int reservoir;        /* bits in the reservoir now */
} AacStats;
const AacStats *aac_stats(const AacEnc *e);

/* ---- building blocks, exposed for tests (and a test decoder) ---- */

typedef struct {
    int size;            /* entries: 81, 64, 169 or 289 */
    int dim;             /* 4 (quads) or 2 (pairs) */
    int lav;             /* largest absolute value (16 = escape for book 11) */
    bool is_unsigned;    /* values coded as magnitudes, signs follow as bits */
    const uint16_t *code;
    const uint8_t *len;
} AacCodebook;
const AacCodebook *aac_codebook(int cb);                         /* 1..11 */
void aac_sf_codebook(const uint32_t **code, const uint8_t **len); /* 121 entries, index = diff + 60 */

/* Scalefactor band offsets for a sample rate: n+1 offsets for n bands, NULL if unsupported. */
const uint16_t *aac_swb_offsets(int sample_rate, bool short_window, int *nbands);
int aac_rate_index(int sample_rate); /* 0..12, or -1 */

/* Rising half of a window: 1024 values (long) or 128 (short), shape 0 = sine, 1 = KBD. */
const float *aac_window(const AacEnc *e, bool short_window, int shape);

/* Forward MDCT as in the standard (X[k] = 2 sum z[n] cos(...), n0 = N/4 + 1/2):
   n = 2048 or 256 windowed inputs -> n/2 coefficients. */
void aac_mdct(const AacEnc *e, const float *in, float *out, int n);
/* Its inverse (x[n] = 2/N sum X[k] cos(...)): n/2 coefficients -> n time-aliased
   samples, to be windowed and overlap-added. */
void aac_imdct(const AacEnc *e, const float *in, float *out, int n);

#endif
