/* modules/av/dts.h — DTS Coherent Acoustics ("DTS") core audio decoder
 * (ETSI TS 102 114).
 *
 * Blu-ray rips often carry their sound as DTS - 5.1 at 48 kHz, ~1.5 Mbit/s -
 * which no browser plays: the picture runs silent. This decoder turns the core
 * frames into PCM so the converter can make AAC of them (on the server, or in
 * the uploading browser's WebAssembly build).
 *
 * The core is what every DTS stream carries, DTS-HD High Resolution and Master
 * Audio included (their extra substream follows the core frame and is left
 * alone: the core is the full 5.1 at a lossy rate, all a stereo AAC needs).
 * Done: the frame and coding headers, subframes and subsubframes, ADPCM
 * prediction (the spec's vector table), the high-frequency vector table, joint
 * intensity, transients, every Huffman / block / linear code for the
 * quantisation indices, scale factors with their adjustment, dynamic range, the
 * LFE channel (64x and 128x interpolation) and both 32-band synthesis filter
 * banks. Channel layouts up to 5.1 (AMODE 0-9 + LFE); a Lo/Ro stereo downmix.
 * Not done (DTS_ERR_UNSUPPORTED): AMODE 10-15 (6-8 primary channels), the 14-bit
 * packed and little-endian forms of the stream (DTS on audio CDs, never in MKV),
 * and the extensions inside a core frame (XCh 6.1, X96, XBR): skipped.
 *
 * Pure leaf library: libc only (math.h for table setup), no mutable globals and
 * no OS calls, so it builds for wasm32-wasi and the Pi as-is. Every decoder
 * instance is independent; use one per stream and thread. */
#ifndef HEMMA_AV_DTS_H
#define HEMMA_AV_DTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DTS_SAMPLES 4096   /* PCM samples per channel one frame may decode to (128 blocks of 32) */
#define DTS_MAX_CHANNELS 6 /* 5 primary channels + LFE */
#define DTS_MAX_FRAME 16384 /* bytes: the largest core frame (FSIZE is 14 bits) */
#define DTS_HEADER_BYTES 16 /* enough to know a frame's size, rate and channels */

typedef struct {
    int sample_rate;   /* 8000 .. 48000 (the core's own rate: 48 kHz on Blu-rays, 44.1 kHz on CDs) */
    int bitrate;       /* kbit/s as the header says (0 = open / variable / lossless) */
    int frame_bytes;   /* size of this core frame */
    int blocks;        /* blocks of 32 samples (NBLKS + 1): 16 for a 512-sample frame */
    int samples;       /* PCM samples per channel this frame decodes to (blocks * 32) */
    int amode;         /* 0 mono, 1 dual mono, 2 L R, 3 L+R L-R, 4 Lt Rt, 5 C L R, 6 L R S, 7 C L R S, 8 L R SL SR,
                          9 C L R SL SR (10-15: more than 5 - DTS_ERR_UNSUPPORTED) */
    bool lfe;          /* low-frequency effects channel present */
    int lfe_factor;    /* its interpolation: 128 or 64 (0 = none) */
    int channels;      /* primary channels + LFE, e.g. 6 for 5.1 */
    int subframes;     /* subframes in this frame */
    bool crc;          /* the frame carries CRC words (the spec says never to test them: they're skipped) */
    bool dynrange;     /* dynamic range coefficients present */
    bool ext_audio;    /* an extension (XCh, X96, XBR...) follows the core audio in the frame: skipped */
    int ext_audio_id;  /* which: 0 XCh, 2 X96, 6 XXCH, ... */
    bool perfect_qmf;  /* the perfect reconstruction filter bank (lossless modes); else the usual one */
    int version;       /* encoder software revision (VERNUM) */
    int pcm_bits;      /* source PCM resolution: 16, 20 or 24 */
} DtsInfo;

/* ---- framing (no decoder needed) ---- */

/* Size in bytes of the core frame starting at p (needs DTS_HEADER_BYTES): the 16-bit big-endian
   form with sync 7F FE 80 01 - the one Matroska and Blu-ray carry. 0 when p is not one. */
int dts_frame_size(const uint8_t *p, size_t n);

/* Parse the frame header (and the start of the coding header). false for bad sync, reserved codes or
   values the spec rules out. n >= DTS_HEADER_BYTES. */
bool dts_parse_header(const uint8_t *p, size_t n, DtsInfo *info);

/* Offset of the first frame in p[0..n) whose header is valid, or -1. A candidate that runs past the
   end of the buffer is returned unverified unless `complete` is set (then it is skipped). A good
   candidate is followed by another sync word, or by a DTS-HD substream (64 58 20 25), or the buffer
   ends with it. */
long dts_find_frame(const uint8_t *p, size_t n, bool complete);

/* ---- decoding ---- */

typedef enum {
    DTS_LAYOUT_NATIVE, /* stream order (AMODE's: C L R SL SR as present), then LFE */
    DTS_LAYOUT_WAV,    /* WAVE/FLAC/MP4 order: L R C LFE SL SR (those present) */
    DTS_LAYOUT_STEREO, /* Lo/Ro downmix to 2 channels (centre and surrounds at -3 dB, LFE dropped) */
} DtsLayout;

enum {
    DTS_OK = 0,
    DTS_ERR_SYNC = -1,        /* not a DTS core frame */
    DTS_ERR_TRUNCATED = -2,   /* fewer bytes than the header says */
    DTS_ERR_UNSUPPORTED = -3, /* a form this decoder lacks (more than 5.1, 14-bit packing) */
    DTS_ERR_BITSTREAM = -5,   /* invalid data inside the frame: output is silence */
};

typedef struct DtsDec DtsDec;
DtsDec *dts_open(void); /* NULL when out of memory */
void dts_close(DtsDec *d);
void dts_reset(DtsDec *d); /* forget the filter banks' history and the predictors, e.g. after a seek */
void dts_set_layout(DtsDec *d, DtsLayout layout);
void dts_set_drc(DtsDec *d, float scale);   /* 1 = the stream's dynamic range coefficients, 0 = off (default) */

/* Decode one core frame into `out` (room for DTS_SAMPLES x DTS_MAX_CHANNELS): info->samples x
   *channels interleaved floats, nominal range +-1.0 (may exceed it a little: clip when converting).
   `info` (optional) receives the header, filled whenever it parsed. Returns DTS_OK or a negative
   DTS_ERR_*. On DTS_ERR_BITSTREAM `out` holds silence with the frame's channel count
   and length and decoding simply continues with the next frame; for the other errors *channels is 0. */
int dts_decode(DtsDec *d, const uint8_t *frame, size_t n, float *out, int *channels, DtsInfo *info);

int dts_layout_channels(int amode, bool lfe, DtsLayout layout);
/* "L" "R" "C" "LFE" "SL" "SR" "S" "Ch1" "Ch2" "L+R" "L-R" "Lt" "Rt" for output channel i ("" if out of range) */
const char *dts_channel_name(int amode, bool lfe, DtsLayout layout, int i);
/* "1.0" "1+1" "2.0" ... "5.0"; with LFE "5.1" and so on */
const char *dts_amode_name(int amode, bool lfe);

/* What the last decoded frame used (tools, tests, curiosity). */
typedef struct {
    int subbands_predicted; /* channel-subbands with ADPCM prediction on (summed over subframes) */
    int subbands_vq;        /* channel-subbands coded with the high-frequency vector table */
    int subbands_joint;     /* channel-subbands taken from another channel (joint intensity) */
    int transients;         /* channel-subbands with a transient (two scale factors) */
    int huffman_codes;      /* quantisation index groups read as Huffman codes */
    int block_codes;        /* ... as block codes (four samples in one) */
    int linear_codes;       /* ... as plain linear codes */
    int abits[32];          /* subband samples per allocation index (0 = none ... 26/27 = 24 bits) */
    int bits_used;          /* bits read up to the end of the core audio */
    int error_line;         /* where a DTS_ERR_BITSTREAM was raised (debugging) */
} DtsStats;
const DtsStats *dts_stats(const DtsDec *d);

#endif
