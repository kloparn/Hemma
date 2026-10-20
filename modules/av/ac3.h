/* modules/av/ac3.h — Dolby Digital (ATSC A/52 "AC-3") and Dolby Digital Plus
 * (E-AC-3, A/52 Annex E / ETSI TS 102 366) audio decoder.
 *
 * DVDs and most TV recordings carry their sound as AC-3, and many films and
 * series files as E-AC-3 ("DDP5.1"), which Chrome and Firefox cannot play.
 * This decoder turns their syncframes into PCM so it can be re-encoded into
 * something every browser plays (or played from a WebAssembly build in the
 * browser itself).
 *
 * Complete A/52: block switching, coupling with phase flags, rematrixing,
 * D15/D25/D45 exponents with reuse, parametric bit allocation with delta bit
 * allocation, grouped mantissas, dither, LFE, dynamic range control and a
 * Lo/Ro stereo downmix. E-AC-3 on top: its frame header and frame-wide
 * strategies (exponent strategy table, default bit allocation, SNR offset
 * strategies), 1/2/3/6 blocks a frame, spectral extension, the adaptive hybrid
 * transform (vector and gain-adaptive quantisation; ac3_aht.h holds the
 * spec's vector tables) and the Lo/Ro mix levels from the mixing metadata.
 * Not done (AC3_ERR_UNSUPPORTED): enhanced coupling and half sample rates,
 * never seen in real files. Of a stream with several substreams only
 * independent substream 0 is decoded (a dependent one adds channels past
 * 5.1: AC3_SKIP).
 *
 * Pure leaf library: libc only (math.h: table setup in ac3_open and
 * ac3_set_drc, and sqrtf for spectral extension), no mutable globals and no
 * OS calls, so it builds for wasm32-wasi and the Pi (aarch64, armv6) as-is.
 * Every decoder instance (~80 KB) is independent; use one per stream and
 * thread.
 *
 * Checked against real DVD rips (Dolby encoders, bsid 6 and 8): all 11.4
 * million frames of 183 tracks (101 h) parse bit-exactly, without clicks at
 * block edges (tools/ac3dump.c; HEMMA_TEST_AC3 in ac3_test.c for any file).
 * E-AC-3: real DDP5.1 episodes parse bit-exactly too and match FFmpeg's
 * decoder but for the random dither, as do Dolby-made test clips with
 * coupling, spectral extension and AHT (FFmpeg's hebap 4 vector table lacks
 * the spec's first row; ours is the spec's). */
#ifndef HEMMA_AV_AC3_H
#define HEMMA_AV_AC3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AC3_SAMPLES 1536   /* PCM samples per channel in one syncframe (6 blocks of 256): the most any frame has */
#define AC3_MAX_CHANNELS 6 /* 5 full-bandwidth + LFE */
#define AC3_MAX_FRAME 4096 /* bytes: E-AC-3's largest (2048 words); AC-3's is 3840 (640 kbit/s at 32 kHz) */
#define AC3_HEADER_BYTES 8 /* enough to know the size of a frame */

typedef struct {
    int sample_rate;  /* 48000, 44100 or 32000 (E-AC-3 at half rate: 24000, 22050, 16000) */
    int bitrate;      /* kbit/s */
    int frame_bytes;  /* size of this syncframe */
    int bsid, bsmod;  /* bitstream id (<= 8 AC-3, 11..16 E-AC-3) and service type (0 = main audio) */
    bool eac3;        /* Dolby Digital Plus (E-AC-3) */
    int strmtyp;      /* E-AC-3: 0 independent, 1 dependent (extra channels), 2 converted from AC-3 */
    int substreamid;  /* E-AC-3: which substream (0..7) */
    int blocks;       /* audio blocks of 256 samples: 6 for AC-3, 1, 2, 3 or 6 for E-AC-3 */
    int samples;      /* PCM samples per channel this frame decodes to (blocks * 256) */
    int acmod;        /* 0 = 1+1 (dual mono), 1 = 1/0 (C), 2 = 2/0 (L R), 3 = 3/0, 4 = 2/1, 5 = 3/1, 6 = 2/2, 7 = 3/2 */
    bool lfe;         /* low-frequency effects channel present */
    int channels;     /* full-bandwidth channels + LFE, e.g. 6 for 5.1 */
    int dialnorm;     /* dialogue level: dialogue sits at -dialnorm dBFS (1..31) */
    int dsurmod;      /* 2/0 only: 2 = Dolby Surround encoded, 1 = not, 0 = not indicated */
    float clev, slev; /* centre / surround downmix levels from the stream */
    int fscod, frmsizecod; /* the raw codes (an MP4 'dac3' box wants fscod and frmsizecod >> 1; AC-3 only) */
} Ac3Info;

/* ---- framing (no decoder needed) ---- */

/* CRC-16 of AC-3: polynomial x^16+x^15+x^2+1, MSB first, starting value crc. */
uint16_t ac3_crc16(uint16_t crc, const uint8_t *p, size_t n);

/* Size in bytes of the AC-3 or E-AC-3 syncframe starting at p (needs
   AC3_HEADER_BYTES), or 0 when p is not the start of one. */
int ac3_frame_size(const uint8_t *p, size_t n);

/* Parse syncinfo + bitstream information of an AC-3 or E-AC-3 frame. false
   for bad sync, reserved codes or a bitstream id we don't know (9, 10, 17+).
   n >= AC3_HEADER_BYTES is enough for the size; the full BSI may need up to
   ~80 bytes (E-AC-3 mixing data). */
bool ac3_parse_header(const uint8_t *p, size_t n, Ac3Info *info);

/* The frame's CRCs: AC-3 has crc1 over the first 5/8 and crc2 over the rest,
   E-AC-3 one CRC over the whole frame. False when n is shorter than the frame. */
bool ac3_crc_ok(const uint8_t *frame, size_t n);

/* Offset of the first frame in p[0..n) whose header is valid and whose CRCs
   check, or -1. A candidate that runs past the end of the buffer is returned
   unverified (read more and look again) unless `complete` is set, in which
   case it is skipped. Finds frames in raw streams and inside containers. */
long ac3_find_frame(const uint8_t *p, size_t n, bool complete);

/* ---- decoding ---- */

typedef enum {
    AC3_LAYOUT_NATIVE, /* stream order: L C R Ls Rs as present (S for one surround), then LFE */
    AC3_LAYOUT_WAV,    /* WAVE/FLAC/MP4 order: L R C LFE Ls Rs (those present; 1+1 = Ch1 Ch2) */
    AC3_LAYOUT_STEREO, /* Lo/Ro downmix to 2 channels with the stream's levels (LFE dropped) */
} Ac3Layout;

enum {
    AC3_OK = 0,
    AC3_SKIP = 1,             /* E-AC-3: another substream or a dependent one (channels past 5.1): no output, go on */
    AC3_ERR_SYNC = -1,        /* not an AC-3 or E-AC-3 frame */
    AC3_ERR_TRUNCATED = -2,   /* fewer bytes than the header says */
    AC3_ERR_UNSUPPORTED = -3, /* a tool this decoder lacks (E-AC-3: AHT, enhanced coupling, half rates; bsid 9/10) */
    AC3_ERR_CRC = -4,         /* damaged frame: output is silence */
    AC3_ERR_BITSTREAM = -5,   /* invalid data inside the frame: output is silence */
};

typedef struct Ac3Dec Ac3Dec;
Ac3Dec *ac3_open(void); /* NULL when out of memory */
void ac3_close(Ac3Dec *d);
void ac3_reset(Ac3Dec *d); /* forget the overlap tail, e.g. after a seek */
void ac3_set_layout(Ac3Dec *d, Ac3Layout layout);
void ac3_set_drc(Ac3Dec *d, float scale);   /* 1 = the stream's dynamic range control (default), 0 = off */
void ac3_set_crc_check(Ac3Dec *d, bool on); /* default on */

/* Decode one syncframe into `out` (room for AC3_SAMPLES x AC3_MAX_CHANNELS):
   info->samples (1536 for AC-3, 256 per block for E-AC-3) x *channels
   interleaved floats, nominal range +-1.0 (may exceed it a little: clip when
   converting). `info` (optional) receives the header, filled whenever the
   header parsed. Returns AC3_OK, AC3_SKIP or a negative AC3_ERR_*. On
   AC3_ERR_CRC / AC3_ERR_BITSTREAM `out` holds silence with the frame's
   channel count and length, and decoding simply continues with the next
   frame; for AC3_SKIP and the other errors *channels is 0 and `out` holds
   nothing useful. */
int ac3_decode(Ac3Dec *d, const uint8_t *frame, size_t n, float *out, int *channels, Ac3Info *info);

/* float -> int16 with rounding and clipping (n values). */
void ac3_to_s16(const float *in, int16_t *out, size_t n);

int ac3_layout_channels(int acmod, bool lfe, Ac3Layout layout);
/* "L" "R" "C" "LFE" "Ls" "Rs" "S" "Ch1" "Ch2" for output channel i of a layout ("" if out of range). */
const char *ac3_channel_name(int acmod, bool lfe, Ac3Layout layout, int i);
/* "1+1" "1.0" "2.0" "3.0" "2/1" "3/1" "2/2" "5.0"; with LFE "1+1.1" "1.1" "2.1" "3.1" "2/1.1" "3/1.1" "2/2.1" "5.1" */
const char *ac3_acmod_name(int acmod, bool lfe);

/* What the last decoded frame used (tools, tests, curiosity). */
typedef struct {
    uint8_t short_mask[6]; /* per block: bit ch set when channel ch used the short transform */
    int blocks_short;  /* channel-blocks with the short (2 x 256) transform */
    int blocks_cpl;    /* blocks with coupling in use */
    int blocks_remat;  /* 2/0 blocks with any rematrixing band on */
    int blocks_dba;    /* channel-blocks applying delta bit allocation */
    int blocks_phase;  /* 2/0 coupling blocks with any phase flag set */
    int blocks_dither; /* channel-blocks with dither on */
    int blocks_spx;    /* E-AC-3 blocks with spectral extension in use */
    int blocks_aht;    /* E-AC-3 channel-blocks coded with the adaptive hybrid transform */
    int exp_strategy[4]; /* channel-blocks per exponent strategy: reuse, D15, D25, D45 */
    int bap[16];       /* mantissas per bit allocation pointer */
    int bits_used;     /* bits read up to the end of the last audio block */
    int bits_free;     /* bits between the audio data and the auxiliary data/CRC */
    bool tail_zero;    /* those free bits are all zero (a sign the frame was parsed exactly) */
    int aux_bits;      /* auxiliary data bits declared by the encoder */
    int error_block, error_line; /* where an AC3_ERR_BITSTREAM was raised (debugging) */
} Ac3Stats;
const Ac3Stats *ac3_stats(const Ac3Dec *d);

/* ---- building blocks, exposed for tests ---- */

/* Expand ngrps 7-bit exponent groups (A/52 7.1.3) for strategy 1 (D15),
   2 (D25) or 3 (D45), starting from absexp. Writes ngrps*3*{1,2,4} values to
   out; false on an invalid group or an exponent outside 0..24. */
bool ac3_expand_exponents(int strategy, int absexp, const uint8_t *groups, int ngrps, int8_t *out);

typedef struct {
    int fscod;                                 /* 0 = 48 kHz, 1 = 44.1, 2 = 32 */
    int sdecay, fdecay, sgain, dbknee, floor;  /* decoded parameters (Tables 7.6-7.10) */
    int fgain;                                 /* fast gain of this channel (Table 7.11) */
    int snroffset;                             /* ((csnroffst - 15) * 16 + fsnroffst) * 4 */
    int cplfleak, cplsleak;                    /* coupling channel: leak codes (0..7) */
    int dba_mode;                              /* 0 = reuse, 1 = new, 2 = none */
    int dba_nseg;
    uint8_t dba_offst[8], dba_len[8], dba_ba[8];
    bool hebap;                                /* E-AC-3 AHT: high-efficiency pointers 0..19 (Table E.2.1) */
} Ac3BitAlloc;

/* Parametric bit allocation (A/52 7.2.2) for bins [start, end) of one channel:
   writes bap[start..end) (hebap[] with p->hebap). A coupling channel is recognised
   by start > 0, the LFE channel by end == 7. false on out-of-range delta bit
   allocation. */
bool ac3_bit_allocation(const Ac3BitAlloc *p, const int8_t *exps, int start, int end, uint8_t *bap);

/* Unwindowed inverse transform of 256 coefficients (A/52 7.9.4): the long
   512-point IMDCT, or the two 256-point ones of a short block, into y[512]. */
void ac3_imdct(Ac3Dec *d, const float coef[256], bool short_block, float y[512]);
const float *ac3_window(const Ac3Dec *d); /* first half of the KBD window (alpha = 5), 256 values */

#endif
