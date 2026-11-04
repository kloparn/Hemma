/* modules/av/aacdec.h — AAC decoder (MPEG-4 Audio, ISO/IEC 14496-3: AAC-LC, Audio Object Type 2).
 *
 * The sound of most videos that already play everywhere, and of every copy Hemma made from them (the
 * converter copies AAC as it is): 2.9 reads it to find a series' intros (modules/av/intro). Any encoder's
 * AAC-LC, not only ours: every sample rate (7.35-96 kHz), mono to 7.1 (SCE / CPE / LFE elements, a
 * channel configuration or a program config element), long and short windows with sine and KBD shapes,
 * all eleven spectral codebooks + escapes, pulse data, TNS, M/S and intensity stereo, perceptual noise
 * substitution. Data stream and fill elements are read past.
 *
 * HE-AAC (v1 SBR, v2 PS) decodes as its AAC-LC core: the lower half of the sound at the core's own rate
 * (aacdec_info: sample_rate = the core's, ext_rate = what the whole plays at) - what a sound fingerprint
 * looks at. Refused (AACDEC_ERR_UNSUPPORTED): other object types (Main's prediction, LTP, SSR, ER), 960-
 * sample frames, coupling channel elements.
 *
 * Pure leaf library: libc only (math.h at setup), no core/ dependency, no mutable globals - one decoder
 * per stream and thread. Damaged input never crashes or reads out of bounds (it is fuzzed: tools/avfuzz.c
 * -DAVFUZZ_AAC). */
#ifndef HEMMA_AV_AACDEC_H
#define HEMMA_AV_AACDEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AACDEC_FRAME 1024       /* PCM samples per channel an access unit decodes to */
#define AACDEC_MAX_CHANNELS 8   /* 7.1 */
#define AACDEC_MAX_AU (768 * AACDEC_MAX_CHANNELS) /* bytes: 6144 bits per channel at most */

typedef struct {
    int object_type; /* 2 = AAC-LC; 5 = HE-AAC (SBR), 29 = HE-AAC v2 (PS): only the LC core is decoded */
    int sample_rate; /* the core's: what the decoder's output is at */
    int ext_rate;    /* the rate the whole sound plays at (twice the core's with SBR), else = sample_rate */
    int config;      /* channelConfiguration 0..7 (0 = a program config element says) */
    int channels;    /* how many the configuration names (the stream's elements decide in the end) */
    bool sbr;        /* SBR signalled in the config or found in the stream (implicit signalling) */
} AacDecInfo;

typedef enum {
    AACDEC_LAYOUT_NATIVE, /* the stream's element order: C L R Ls Rs ... then LFE, as present */
    AACDEC_LAYOUT_WAV,    /* WAVE/FLAC order for configurations 1-6: L R C LFE Ls Rs (those present) */
    AACDEC_LAYOUT_STEREO, /* a Lo/Ro downmix to 2 channels (centre and surrounds at -3 dB, LFE dropped) */
    AACDEC_LAYOUT_MONO,   /* every channel but the LFE averaged */
} AacDecLayout;

enum {
    AACDEC_OK = 0,
    AACDEC_ERR_UNSUPPORTED = -3, /* a form this decoder lacks (see above): output is silence */
    AACDEC_ERR_BITSTREAM = -5,   /* invalid or truncated data: output is silence, decoding goes on */
};

/* Read an AudioSpecificConfig (MP4 esds DecoderSpecificInfo, Matroska CodecPrivate). False when it
   isn't one this decoder can decode (info, when given, is filled as far as it was read). */
bool aacdec_config(const uint8_t *asc, size_t n, AacDecInfo *info, char *err, size_t errcap);
/* The 2-byte AudioSpecificConfig for an AAC-LC stream (old Matroska ids carry none): 2, or 0 for a rate
   no table holds. */
int aacdec_make_config(int sample_rate, int channels, uint8_t out[2]);

typedef struct AacDec AacDec;
/* NULL (why in err) when the config is refused or memory runs out. */
AacDec *aacdec_open(const uint8_t *asc, size_t n, char *err, size_t errcap);
void aacdec_close(AacDec *d);
void aacdec_reset(AacDec *d); /* forget the overlap, e.g. after a seek */
void aacdec_set_layout(AacDec *d, AacDecLayout layout);
const AacDecInfo *aacdec_info(const AacDec *d);

/* Decode one raw access unit (an ADTS frame's header is skipped too) into `out` (room for AACDEC_FRAME x
   AACDEC_MAX_CHANNELS): AACDEC_FRAME x *channels interleaved floats, nominal range +-1.0. Returns
   AACDEC_OK or a negative AACDEC_ERR_*; on an error `out` holds silence of the last good channel count
   (*channels 0 before any) so the timing stays. */
int aacdec_decode(AacDec *d, const uint8_t *au, size_t n, float *out, int *channels);

typedef struct {
    int frames, errors;
    int short_frames; /* channel-frames with eight short windows */
    int tns, pns, is, ms, pulse; /* channel-frames (CPE frames for is/ms) using each tool */
    int fill_sbr;     /* fill elements holding SBR data (implicit HE-AAC) */
} AacDecStats;
const AacDecStats *aacdec_stats(const AacDec *d);

/* ---- exposed for tests ---- */
/* Scalefactor band offsets the decoder uses for a rate: n+1 offsets for n bands, NULL for a rate out of range. */
const uint16_t *aacdec_swb(int sample_rate, bool short_window, int *nbands);
/* The last channel element's spectrum (k = 0, or 1 = a pair's right), dequantised, after stereo and TNS. */
const float *aacdec_test_spectrum(const AacDec *d, int k);
/* The inverse MDCT as the standard writes it (2/N, no output scaling): n = 2048 or 256 outputs from n/2. */
void aacdec_test_imdct(const AacDec *d, const float *in, float *out, int n);
/* A window's rising half: 1024 values (long) or 128 (short); shape 0 = sine, 1 = KBD. */
const float *aacdec_test_window(const AacDec *d, bool short_window, int shape);

#endif
