/* modules/av/conv.c — MKV -> MP4 that plays everywhere (HEVC and AV1 copied: on the devices that decode them; HEVC
   made H.264 when asked). See conv.h. */
#include "conv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aac.h"
#include "ac3.h"
#include "deint.h"
#include "dts.h"
#include "h264enc.h"
#include "hevc.h"
#include "mpeg2.h"
#include "mpeg4.h"

#define NS 1000000000LL
#define COPY_SURVEY_PM 300 /* copying: the first read (every timestamp) is about a third of the work */
#define ANALYZE_FRAMES 240 /* per window (~8 s), four windows decide between inverse telecine and deinterlacing */
/* Timestamps and lengths beyond ~13 days (in ns) are damage and count as missing: every sum
   and product of them below then stays far from overflowing. */
#define MAX_TS ((int64_t)1 << 50)
/* Samples one track may get. Each keeps ~20-40 bytes of index in memory until the end, and a
   file of tiny blocks (or laced empty frames, hundreds per block) could otherwise need many
   times its own size in RAM. At most MAX_SAMPLES (9 hours of 60 fps video, 11 hours of AAC),
   and with a known length at most this many a second of it - far above any real rate. */
#define MAX_SAMPLES 2000000
#define VIDEO_PER_S 300 /* frames: real ones top out at 240 fps (60i bobbed: 60) */
#define AUDIO_PER_S 120 /* AAC blocks: 47 a second at 48 kHz, 100 at 96 kHz */
/* With 2+ sound tracks each one's AAC frames go out in runs of this many (~1 s at 48 kHz), one chunk each: one
   sample a chunk, six sounds interleaved, made ~20 MB of chunk tables in a film's moov (2.2). One sound: as ever. */
#define AUDIO_RUN 48
#define CMIN(a, b) ((a) < (b) ? (a) : (b))
#define CMAX(a, b) ((a) > (b) ? (a) : (b))

static int64_t gcd64(int64_t a, int64_t b) {
    while (b) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

static void sar_of(int w, int h, int dar_n, int dar_d, int *sw, int *sh) {
    *sw = *sh = 0; /* square */
    if (w <= 0 || h <= 0 || dar_n <= 0 || dar_d <= 0) return;
    int64_t n = (int64_t)dar_n * h, d = (int64_t)dar_d * w, g = gcd64(n, d);
    n /= g, d /= g;
    if (n != d && n < 65536 && d < 65536) *sw = (int)n, *sh = (int)d;
}

/* One sound track kept (2.2: every one the converter takes, AV_CONV_MAX_AUDIO). */
struct ConvAudio {
    const MkvTrack *t;
    bool code;     /* Dolby or DTS decoded and made AAC (false: AAC, MP3 or Opus copied) */
    bool mp3;      /* MP3 copied (2.5): each block's frames one sample each */
    bool opus;     /* Opus copied (2.5): each packet a sample, as long as its TOC says - fdur = the last one's */
    uint32_t fdur; /* a frame's samples in the MP4: AAC 1024, MP3 1152 (MPEG-1) or 576 (MPEG-2's half rates) */
    uint8_t dops[32]; /* Opus: its dOps box's body, made of the OpusHead */
    size_t dops_len;
    int channels, preskip; /* Opus: from the OpusHead */
    bool dead;     /* dropped part-way (lose): fed no more, left out of the moov */
    int track;     /* its MP4 track */
    int rate;      /* coded: the track's rate - 48000, 44100 or 32000 - and so the AAC's */
    int64_t a0, n; /* its first pts (ns); samples written (against amax) */
    /* coded: Dolby (ac3) or DTS (dtsd) decoded, made AAC */
    Ac3Dec *ac3;
    DtsDec *dtsd;
    AacEnc *aac;
    float *fifo;
    int nfifo;
    int good, bad;        /* frames decoded / not */
    int frame;            /* samples in the last frame (Dolby 1536 - E-AC-3 may send 256, 512 or 768 -; DTS 512 mostly) */
    int64_t next, skip;   /* expected pts of the next frame (ns); samples to drop at the start */
    /* 2+ sounds: frames held until AUDIO_RUN of them go out as one chunk */
    uint8_t *run;
    size_t nrun, crun;
    uint32_t rsize[AUDIO_RUN], rdur[AUDIO_RUN];
    int nrunf;
};

typedef struct {
    AvConvJob *j;
    Mkv *mkv;
    const MkvTrack *vt;
    bool vcode;        /* re-encode (true) or copy (false) */
    int vcopy;         /* a copied picture's MP4 codec: MP4_H264, MP4_HEVC or MP4_AV1 */
    int64_t v0;        /* first video pts (ns) */
    int64_t duration;  /* ns, 0 = unknown */
    Mp4 *mp4;
    int vtrack;
    int64_t nv, vmax, amax; /* pictures written, the most allowed - and the most for each sound track (cap_for) */
    /* sound: every track we can convert, in the file's order */
    struct ConvAudio aud[AV_CONV_MAX_AUDIO];
    int naud, alive, def; /* kept, still fed, the default one (aud[def]: tkhd-enabled) */
    float *pcm;           /* a decoded frame, every channel (one for all: off the stack) */
    uint8_t au[AAC_MAX_AU];
    /* video re-encode */
    Mpeg2 *dec;
    bool m4;                     /* (2.5) the picture is MPEG-4 Part 2 - DivX, Xvid: dec4 instead of dec */
    Mpeg4 *dec4;
    bool hv;                     /* (2.6) HEVC made H.264 (AvConvJob.h264): dec5 instead of dec */
    Hevc *dec5;
    int hv_depth;                /* its samples' bit depth (8 or 10) */
    const uint8_t *vol;          /* its setup (the VOL): CodecPrivate, past an AVI's BITMAPINFOHEADER */
    size_t nvol;
    int64_t seen[ANALYZE_FRAMES]; /* its first pictures' times (the survey), for their usual length */
    int nseen;
    int64_t vgap, vprev; /* MPEG-4 and HEVC pictures are timed by their own times on 90 kHz: a usual length (ns), the last place */
    Deint *di;
    int w, h; /* the picture size everything was set up for */
    int dmode, out_num, out_den; /* (MPEG-4: the track's clock is 90 kHz, out_* only the encoder's rate) */
    H264Enc *enc;
    uint8_t *pend; /* the last encoded frame waits for the next to learn its duration */
    size_t npend, cpend;
    int64_t pend_index;
    bool pend_key, have_pend;
    /* video copy */
    int64_t *vpts; /* every video pts in file order (decode order) */
    int nvpts, vi;
    int64_t *dts, vshift; /* sorted pts = decode times; shift keeps composition offsets >= 0 */
    /* subtitles (j->want_subs, av_subtitles): text gathered as SRT, pictures as the files players read */
    struct ConvSub {
        const MkvTrack *t;
        int kind;      /* SK_* */
        char *p;       /* the SRT text / the .sup bytes / the .sub bytes */
        size_t n, cap;
        char *lines;   /* VobSub: the .idx's timestamp lines (the header goes in front at the end) */
        size_t ln, lcap;
        int cues;
        bool full;     /* text: no more cues fit (what came stays); pictures: past a bound - the track is left out */
    } sub[AV_CONV_MAX_SUBS];
    int nsub;
    size_t pic_held;           /* what the picture tracks hold (their buffers), against pic_total */
    size_t pic_max, pic_total; /* AV_CONV_PIC_BYTES and _TOTAL, or the job's smaller pic_bytes (pic_bounds) */
    bool failed;
} Conv;

static bool fail(Conv *c, const char *fmt, const char *arg) {
    if (!c->failed) snprintf(c->j->err, sizeof c->j->err, fmt, arg ? arg : "");
    c->failed = true;
    return false;
}

static bool unsupported(Conv *c, const char *fmt, const char *arg) {
    if (!c->failed) c->j->unsupported = true;
    return fail(c, fmt, arg);
}

static bool stopped(const Conv *c) { return c->j->stop && *c->j->stop; }

/* What it takes (conv.h: a new format goes here - and gets its fuzz seed, conv_test.c checks). */
const AvCodec AV_CONV_CODECS[] = {
    {"V_MPEG2", MKV_VIDEO, AV_DECODE, false},
    {"V_MPEG1", MKV_VIDEO, AV_DECODE, false},
    {"V_MPEG4/ISO/ASP", MKV_VIDEO, AV_DECODE, false}, /* (2.5: MPEG-4 Part 2 - Xvid, DivX; an MP4's 'mp4v' too) */
    {"V_MPEG4/ISO/SP", MKV_VIDEO, AV_DECODE, false},
    {"V_MS/VFW/FOURCC", MKV_VIDEO, AV_DECODE, false}, /* (an AVI's track as mkvmerge keeps it: by its fourcc, vfw_mpeg4) */
    {"V_MPEG4/ISO/AVC", MKV_VIDEO, AV_COPY, false},
    {"V_MPEGH/ISO/HEVC", MKV_VIDEO, AV_COPY, false}, /* (as 'hvc1': Safari plays it in MP4, never in MKV) */
    {"V_AV1", MKV_VIDEO, AV_COPY, false},            /* (as 'av01': the same for Safari on devices with AV1) */
    {"A_AAC", MKV_AUDIO, AV_COPY, true},
    {"A_AC3", MKV_AUDIO, AV_DECODE, false},
    {"A_EAC3", MKV_AUDIO, AV_DECODE, false},
    {"A_DTS", MKV_AUDIO, AV_DECODE, false}, /* (the core: DTS-HD's extra substream is left alone) */
    {"A_MPEG/L3", MKV_AUDIO, AV_COPY, false}, /* (2.5: MP3 copied - every browser plays it in an MP4, an MKV only Chrome's) */
    {"A_OPUS", MKV_AUDIO, AV_COPY, false},    /* (2.5: Opus copied - Chrome, Firefox, Edge, Safari 17+ play it in an MP4) */
    {"S_TEXT/UTF8", MKV_SUBTITLE, AV_TEXT, false},
    {"S_TEXT/ASS", MKV_SUBTITLE, AV_TEXT, false},
    {"S_TEXT/SSA", MKV_SUBTITLE, AV_TEXT, false},
    {"S_TEXT/WEBVTT", MKV_SUBTITLE, AV_TEXT, false},
    {"S_HDMV/PGS", MKV_SUBTITLE, AV_PICTURE, false}, /* Blu-ray: a .sup file */
    {"S_VOBSUB", MKV_SUBTITLE, AV_PICTURE, false},   /* DVD: an .idx + .sub pair */
    {NULL, 0, 0, false},
};

const AvCodec *av_conv_codec(const char *id) {
    for (const AvCodec *k = AV_CONV_CODECS; id && k->id; k++)
        if (k->prefix ? !strncmp(id, k->id, strlen(k->id)) : !strcmp(id, k->id)) return k;
    return NULL;
}

static bool dolby(const char *codec) { return !strcmp(codec, "A_AC3") || !strcmp(codec, "A_EAC3"); }
static bool is_dts(const char *codec) { return !strcmp(codec, "A_DTS"); }
static bool is_opus(const char *codec) { return !strcmp(codec, "A_OPUS"); }

/* An Opus track's setup - its OpusHead (RFC 7845 5.1, little-endian; Matroska's CodecPrivate) - as the MP4's dOps box
   ("Encapsulation of Opus in ISO Base Media File Format" 4.3.2: version 0, big-endian), with its channels and pre-skip.
   0 = not one we copy: a channel mapping family other than 0 (mono, stereo) or 1 (up to 8, as Vorbis orders them) -
   what browsers play. */
static size_t opus_dops(const uint8_t *h, size_t n, uint8_t *out, size_t cap, int *channels, int *preskip) {
    if (!h || n < 19 || memcmp(h, "OpusHead", 8) || h[8] >> 4) return 0; /* (version 1; 0-15 read alike) */
    int ch = h[9], fam = h[18];
    if (!ch || fam > 1 || (fam == 0 && ch > 2) || ch > 8 || (fam && n < 21 + (size_t)ch)) return 0;
    size_t len = fam ? 13 + (size_t)ch : 11;
    if (cap < len) return 0;
    out[0] = 0, out[1] = (uint8_t)ch;
    out[2] = h[11], out[3] = h[10];                         /* PreSkip */
    out[4] = h[15], out[5] = h[14], out[6] = h[13], out[7] = h[12]; /* InputSampleRate */
    out[8] = h[17], out[9] = h[16];                         /* OutputGain */
    out[10] = (uint8_t)fam;
    if (fam) out[11] = h[19], out[12] = h[20], memcpy(out + 13, h + 21, (size_t)ch); /* streams, coupled, the mapping */
    *channels = ch;
    *preskip = h[10] | h[11] << 8;
    return len;
}

/* An Opus packet's length in 48 kHz samples (RFC 6716 3.1): its TOC byte's frame size times its frame count. 0 = an
   empty or broken one. */
static uint32_t opus_samples(const uint8_t *p, size_t n) {
    static const uint16_t SIZE[32] = {480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920, 2880, /* SILK 10-60 ms */
                                      480, 960, 480,  960,                                              /* hybrid 10, 20 */
                                      120, 240, 480,  960,  120, 240, 480,  960,  120, 240, 480,  960,  /* CELT 2.5-20 */
                                      120, 240, 480,  960};
    if (!n) return 0;
    int code = p[0] & 3, frames = code == 0 ? 1 : code < 3 ? 2 : n >= 2 ? p[1] & 0x3F : 0;
    uint32_t total = (uint32_t)frames * SIZE[p[0] >> 3];
    return total <= 5760 ? total : 0; /* (120 ms at most) */
}
#define MAX_AFRAME DTS_SAMPLES /* the most samples one decoded frame brings (DTS; Dolby's is 1536) */

/* The rate of a Dolby or DTS track we can make AAC of at that same rate: the three both use (48 kHz on
   DVDs, Blu-rays and films; 44.1 kHz in TV rips and DTS CDs - an AAC of that rate plays everywhere),
   48000 when the file doesn't say. 0 for E-AC-3's half rates, DTS's low ones and the rest. */
static int dolby_rate(const MkvTrack *t) {
    if (t->rate <= 0) return 48000;
    static const int RATES[] = {48000, 44100, 32000};
    for (int i = 0; i < 3; i++)
        if (t->rate > RATES[i] - 1 && t->rate < RATES[i] + 1) return RATES[i];
    return 0;
}

/* Sound we can make AAC of: AAC itself, or Dolby AC-3 / E-AC-3 or DTS at one of those rates (dolby_rate). */
static bool audio_ok(const MkvTrack *t) {
    const AvCodec *k = av_conv_codec(t->codec);
    if (t->compressed || !k || k->type != MKV_AUDIO) return false;
    if (k->how == AV_DECODE) return dolby_rate(t) > 0;
    uint8_t d[32];
    int ch, ps;
    return !is_opus(t->codec) || opus_dops(t->priv, t->priv_len, d, sizeof d, &ch, &ps) > 0;
}

/* A codec in the words people know it by (the reasons in err are read on the upload page). */
static const char *codec_words(const char *id) {
    static const struct {
        const char *id, *words;
    } names[] = {
        {"A_DTS", "DTS"},           {"A_TRUEHD", "Dolby TrueHD"}, {"A_MLP", "Dolby TrueHD"},    {"A_OPUS", "Opus"},
        {"A_VORBIS", "Vorbis"},     {"A_FLAC", "FLAC"},           {"A_MPEG/L3", "MP3"},         {"A_MPEG/L2", "MP2"},
        {"A_PCM/", "uncompressed (PCM)"}, {"A_ALAC", "ALAC"},     {"A_EAC3", "Dolby Digital Plus"}, {"A_AC3", "Dolby Digital"},
        {"V_AV1", "AV1"},           {"V_VP9", "VP9"},             {"V_VP8", "VP8"},             {"V_THEORA", "Theora"},
        {"V_MPEG4/ISO/ASP", "MPEG-4 (DivX/Xvid)"}, {"V_MPEG4/ISO/SP", "MPEG-4 (DivX/Xvid)"}, {"V_MS/VFW/FOURCC", "Video for Windows"},
        {"V_PRORES", "ProRes"},     {"V_REAL/", "RealVideo"},     {"A_REAL/", "RealAudio"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strncmp(id, names[i].id, strlen(names[i].id))) return names[i].words;
    return id;
}

static void set_progress(Conv *c, int pm) {
    if (pm == c->j->permille) return;
    c->j->permille = pm;
    if (c->j->progress) c->j->progress(c->j->progress_ctx, pm);
}

/* ns -> ticks, rounded towards zero like ns * timescale / NS but without its overflow: fine for
   any |ns| below 2^60 and any timescale up to 2^32 (sums of a few MAX_TS-bounded times). */
static int64_t ticks(int64_t ns, int64_t timescale) { return ns / NS * timescale + ns % NS * timescale / NS; }

/* The same, to the nearest tick, for a place (ns >= 0) - a copied picture's place on the 90 kHz clock (2.5): lengths
   taken as differences of rounded places never add up an error. An MKV's whole milliseconds are whole ticks either
   way; an MP4's 1/24000 s weren't, and rounding each length down lost 0.75 tick a picture - a quarter of a second in
   a 23-minute DuckTales episode, the picture running ahead of its sound. */
static int64_t at_tick(int64_t ns) { return ns / NS * 90000 + (ns % NS * 90000 + NS / 2) / NS; }

static void append(char *s, size_t cap, const char *more) {
    size_t n = strlen(s);
    if (n < cap) snprintf(s + n, cap - n, "%s", more);
}

/* mkv_next, with timestamps too far out to be real (damage) made missing (see MAX_TS) */
static int next_frame(Conv *c, MkvFrame *fr) {
    int r = mkv_next(c->mkv, fr);
    if (r > 0 && (fr->pts <= -MAX_TS || fr->pts >= MAX_TS)) fr->pts = MKV_NOPTS;
    return r;
}

/* The most samples a track gets: MAX_SAMPLES (or the job's own, smaller bound), and with a known
   length per_s for every second of it (+ some for short clips). */
static int64_t cap_for(const Conv *c, int per_s) {
    int64_t cap = c->j->max_samples > 0 ? CMIN(c->j->max_samples, MAX_SAMPLES) : MAX_SAMPLES;
    if (c->duration > 0) cap = CMIN(cap, (c->duration / NS + 2) * per_s + 1000);
    return cap;
}

static bool too_many(Conv *c, bool video) {
    return unsupported(c, "The file has far too many %s for its length (damaged?)", video ? "pictures" : "sound blocks");
}

/* Every picture goes out through here, counted against the bound. */
static bool put_sample(Conv *c, int track, const void *p, size_t n, uint32_t dur, int32_t cts, bool key) {
    if (++c->nv > c->vmax) return too_many(c, true);
    if (!mp4_sample(c->mp4, track, p, n, dur, cts, key)) return fail(c, "Couldn't write the new file (disk full?)", NULL);
    return true;
}

/* A sound's held frames, written one after another: one chunk */
static bool write_run(Conv *c, struct ConvAudio *a) {
    size_t at = 0;
    for (int i = 0; i < a->nrunf && !c->failed; i++) {
        if (!mp4_sample(c->mp4, a->track, a->run + at, a->rsize[i], a->rdur[i], 0, true))
            fail(c, "Couldn't write the new file (disk full?)", NULL);
        at += a->rsize[i];
    }
    a->nrunf = 0, a->nrun = 0;
    return !c->failed;
}

/* Every sound frame goes out through here, `dur` samples long, counted against its bound: at once with one sound,
   else held for a run (AUDIO_RUN). */
static bool put_audio_d(Conv *c, struct ConvAudio *a, const void *p, size_t n, uint32_t dur) {
    if (a->dead) return true;
    if (++a->n > c->amax) return too_many(c, false);
    if (c->naud < 2) {
        if (!mp4_sample(c->mp4, a->track, p, n, dur, 0, true)) return fail(c, "Couldn't write the new file (disk full?)", NULL);
        return true;
    }
    if (n > 0xFFFFFFFFu) return fail(c, "Couldn't write the new file (disk full?)", NULL);
    if (a->nrun + n > a->crun) {
        size_t cap = CMAX(a->crun * 2, CMAX(a->nrun + n, (size_t)65536));
        uint8_t *q = realloc(a->run, cap);
        if (!q) return fail(c, "Out of memory", NULL);
        a->run = q, a->crun = cap;
    }
    memcpy(a->run + a->nrun, p, n);
    a->nrun += n;
    a->rdur[a->nrunf] = dur;
    a->rsize[a->nrunf++] = (uint32_t)n;
    return a->nrunf < AUDIO_RUN || write_run(c, a);
}

/* ... a frame of the sound's one length (AAC, MP3) */
static bool put_audio(Conv *c, struct ConvAudio *a, const void *p, size_t n) { return put_audio_d(c, a, p, n, a->fdur); }

/* An Opus packet copied (2.5): a sample of its own, as long as its TOC says - a broken one as long as the one before. */
static bool copy_opus(Conv *c, struct ConvAudio *a, const MkvFrame *fr) {
    uint32_t d = opus_samples(fr->data, fr->size);
    if (d) a->fdur = d;
    return put_audio_d(c, a, fr->data, fr->size, a->fdur);
}

/* An MPEG audio Layer III frame's size from its header (ISO 11172-3, 13818-3 and MPEG-2.5's 8-12 kHz): 0 = not one
   (Layer I/II, free format, reserved values). */
static size_t mp3_frame_size(const uint8_t *p, size_t n) {
    if (n < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    int ver = p[1] >> 3 & 3, layer = p[1] >> 1 & 3, bi = p[2] >> 4, si = p[2] >> 2 & 3, pad = p[2] >> 1 & 1;
    if (ver == 1 || layer != 1 || bi == 0 || bi == 15 || si == 3) return 0;
    static const int KBPS1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
                     KBPS2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}, RATES[] = {44100, 48000, 32000};
    int rate = RATES[si] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2), kbps = ver == 3 ? KBPS1[bi] : KBPS2[bi];
    return (size_t)((ver == 3 ? 144 : 72) * kbps * 1000 / rate + pad);
}

/* An MP3 block copied: each frame in it its own sample (a muxer may put several in one block); what doesn't start
   with a frame header goes as it is - a decoder skips it, and the time stays. */
static bool copy_mp3(Conv *c, struct ConvAudio *a, const MkvFrame *fr) {
    const uint8_t *p = fr->data;
    size_t left = fr->size;
    while (left > 0 && !c->failed && !a->dead) {
        size_t n = mp3_frame_size(p, left);
        if (!n || n > left) n = left;
        put_audio(c, a, p, n);
        p += n, left -= n;
    }
    return !c->failed;
}

/* A sound that can't go on (coded in a way the decoder can't follow, at another rate than the file says, mostly
   undecodable): dropped while another lives - left out of the moov, its frames so far left in the mdat - else the
   whole file is refused in these words, as with one sound. True = the conversion goes on. */
static bool lose(Conv *c, struct ConvAudio *a, const char *fmt, const char *arg) {
    if (c->alive <= 1) return unsupported(c, fmt, arg);
    a->dead = true;
    a->nrunf = 0, a->nrun = 0;
    c->alive--;
    if (!mp4_drop_track(c->mp4, a->track)) return fail(c, "The MP4 writer refused a track", NULL);
    return true;
}

/* ---- video: re-encode ---- */

/* index: the picture's number at the output rate (MPEG-2, through the deinterlacer), or - MPEG-4 - its place on the
   90 kHz clock, so a file whose pictures come at an uneven pace (Solsidan S06E04: 11.7 a second, unevenly) keeps it */
static bool put_video(Conv *c, const uint8_t *data, size_t n, int64_t index, bool key) {
    if (c->have_pend) {
        int64_t frames = index - c->pend_index;
        if (frames < 1) frames = 1;
        uint32_t dur = (uint32_t)(c->m4 || c->hv ? CMIN(frames, 90000 * 60) : frames * c->out_den);
        if (!put_sample(c, c->vtrack, c->pend, c->npend, dur, 0, c->pend_key)) return false;
    }
    if (n > c->cpend) {
        uint8_t *p = realloc(c->pend, n);
        if (!p) return fail(c, "Out of memory", NULL);
        c->pend = p;
        c->cpend = n;
    }
    memcpy(c->pend, data, n);
    c->npend = n;
    c->pend_index = index;
    c->pend_key = key;
    c->have_pend = true;
    return true;
}

/* An MPEG-4 picture's place on the 90 kHz clock from the first picture: its own time, else (none, or one out of
   order) the last place + its usual length. Places only go forward. */
static int64_t place_of(Conv *c, int64_t pts) {
    int64_t t = c->vprev + CMAX(1, at_tick(c->vgap));
    if (pts != MKV_NOPTS && c->v0 != MKV_NOPTS && pts >= c->v0 && pts - c->v0 < MAX_TS) t = at_tick(pts - c->v0);
    if (c->have_pend && t <= c->vprev) t = c->vprev + 1;
    return c->vprev = t;
}

static bool encode_out(Conv *c, const DeintOut *o) {
    H264Picture pic = {o->y, o->u, o->v, o->stride_y, o->stride_c, o->stride_c, o->index};
    H264Packet pk;
    if (!h264enc_encode(c->enc, &pic, &pk)) return fail(c, "The H.264 encoder stopped", NULL);
    return put_video(c, pk.data, pk.size, c->m4 || c->hv ? place_of(c, o->pts) : o->index, pk.idr);
}

/* ---- the picture decoder: MPEG-2/-1, or (2.5) MPEG-4 Part 2, or (2.6) HEVC, its frames as MPEG-2's ---- */

static void vdec_decode(Conv *c, const uint8_t *p, size_t n, int64_t pts) {
    if (c->dec5) hevc_decode(c->dec5, p, n, pts);
    else if (c->dec4) mpeg4_decode(c->dec4, p, n, pts);
    else mpeg2_decode(c->dec, p, n, pts);
}

static bool vdec_frame(Conv *c, Mpeg2Frame *pf) {
    if (c->dec5) { /* progressive pictures (field pictures are refused: hevc_unsupported), 8-bit */
        HevcFrame f;
        if (!hevc_frame(c->dec5, &f)) return false;
        memset(pf, 0, sizeof *pf);
        pf->y = f.y, pf->u = f.u, pf->v = f.v, pf->stride_y = f.stride_y, pf->stride_c = f.stride_c;
        pf->width = pf->disp_width = f.width, pf->height = pf->disp_height = f.height;
        pf->dar_num = f.width * f.par_num, pf->dar_den = f.height * f.par_den;
        pf->rate_num = f.rate_num, pf->rate_den = f.rate_den;
        pf->type = f.type;
        pf->progressive_sequence = pf->progressive_frame = true;
        pf->pts = f.pts;
        pf->concealed = f.damaged;
        c->hv_depth = f.bit_depth;
        return true;
    }
    if (!c->dec4) return mpeg2_frame(c->dec, pf);
    Mpeg4Frame f;
    if (!mpeg4_frame(c->dec4, &f)) return false;
    memset(pf, 0, sizeof *pf);
    pf->y = f.y, pf->u = f.u, pf->v = f.v, pf->stride_y = f.stride_y, pf->stride_c = f.stride_c;
    pf->width = pf->disp_width = f.width, pf->height = pf->disp_height = f.height;
    pf->dar_num = f.width * f.par_num, pf->dar_den = f.height * f.par_den;
    pf->rate_num = f.rate_num, pf->rate_den = f.rate_den;
    pf->type = f.type == 'B' ? 'B' : f.type == 'I' ? 'I' : 'P';
    pf->progressive_sequence = pf->progressive_frame = true; /* (interlaced coding is refused: mpeg4_unsupported) */
    pf->pts = f.pts;
    pf->concealed = f.concealed;
    return true;
}

static void vdec_flush(Conv *c) {
    if (c->dec5) hevc_flush(c->dec5);
    else if (c->dec4) mpeg4_flush(c->dec4);
    else mpeg2_flush(c->dec);
}

static bool decoded(Conv *c) {
    Mpeg2Frame pf;
    while (!c->failed && vdec_frame(c, &pf)) {
        if (pf.width != c->w || pf.height != c->h) { /* a later sequence header changed the size: the
                                                        deinterlacer and the encoder are set up for one */
            char sizes[64];
            snprintf(sizes, sizeof sizes, "%d×%d, later %d×%d", c->w, c->h, pf.width, pf.height);
            return unsupported(c, "The picture changes size part-way (%s): can't convert that yet", sizes);
        }
        DeintIn in = {pf.y, pf.u, pf.v, pf.stride_y, pf.stride_c, pf.width, pf.height, pf.progressive_frame,
                      pf.top_field_first, pf.repeat_first_field, pf.pts};
        if (deint_push(c->di, &in) < 0) return fail(c, "The deinterlacer refused a frame", NULL);
        DeintOut o;
        while (!c->failed && deint_pull(c->di, &o)) encode_out(c, &o);
    }
    return !c->failed;
}

/* ---- video: copy (H.264, HEVC or AV1 in MKV) ---- */

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* An AV1CodecConfigurationRecord (MKV's CodecPrivate for V_AV1 is one, as an MP4's av1C): marker 1 + version 1,
   then profile/level, tier/depth/chroma, the delay byte, the configOBUs. */
static bool av1c_ok(const uint8_t *p, size_t n) { return n >= 4 && p[0] == 0x81; }

static bool copy_video(Conv *c, const MkvFrame *fr) {
    if (c->vi >= c->nvpts) return true; /* more frames than the first pass saw: ignore the tail */
    int i = c->vi++;
    int64_t pts = c->vpts[i], dts = c->dts[i];
    int64_t next = i + 1 < c->nvpts ? c->dts[i + 1] : dts + (c->vt->default_duration ? c->vt->default_duration : NS / 25);
    int64_t base = c->dts[0], d0 = at_tick(dts - base); /* (places from the first: never below 0) */
    uint32_t dur = (uint32_t)CMAX(1, at_tick(next - base) - d0);
    int32_t cts = (int32_t)(at_tick(pts - base + c->vshift) - d0);
    const uint8_t *p = fr->data;
    size_t n = fr->size;
    /* AV1: a temporal delimiter OBU (type 2, a size field of 0) leading a block goes - an MP4 sample mustn't have it,
       and Matroska says the same, but not every muxer listened */
    if (c->vcopy == MP4_AV1 && n > 2 && p[0] == 0x12 && p[1] == 0) p += 2, n -= 2;
    return put_sample(c, c->vtrack, p, n, dur, cts, fr->key);
}

/* ---- audio ---- */

static bool push_pcm(Conv *c, struct ConvAudio *a, const float *pcm, int frames) {
    while (frames > 0 && a->skip > 0) { /* audio that starts before the picture */
        pcm += 2, frames--, a->skip--;
    }
    if (frames <= 0) return true;
    memcpy(a->fifo + a->nfifo * 2, pcm, (size_t)frames * 2 * sizeof *pcm);
    a->nfifo += frames;
    while (a->nfifo >= AAC_FRAME) {
        int n = aac_encode(a->aac, a->fifo, c->au, (int)sizeof c->au);
        if (n < 0) return fail(c, "The AAC encoder stopped", NULL);
        if (n > 0 && !put_audio(c, a, c->au, (size_t)n)) return false;
        a->nfifo -= AAC_FRAME;
        memmove(a->fifo, a->fifo + AAC_FRAME * 2, (size_t)a->nfifo * 2 * sizeof *a->fifo);
    }
    return true;
}

/* One DTS core frame of a block (a DTS-HD stream's extension substream follows it: left alone). The
   frame's samples, 0 = no frame here (the rest of the block isn't one), -1 = refused (the track lost, or
   c->failed). */
static int code_dts(Conv *c, struct ConvAudio *a, const uint8_t *p, size_t left, int *size) {
    static const float silence[MAX_AFRAME * 2];
    *size = left >= DTS_HEADER_BYTES ? dts_frame_size(p, left) : 0;
    if (*size <= 0 || (size_t)*size > left) return 0;
    int ch = 0;
    DtsInfo in = {.samples = a->frame};
    int rc = dts_decode(a->dtsd, p, (size_t)*size, c->pcm, &ch, &in);
    if (rc == DTS_ERR_UNSUPPORTED) return lose(c, a, "Its sound is %s coded in a way the converter can't decode yet", "DTS"), -1;
    if (rc == DTS_OK && in.sample_rate != a->rate) return lose(c, a, "Its %s sound isn't at the rate the file says", "DTS"), -1;
    int n = in.samples > 0 && in.samples <= MAX_AFRAME ? in.samples : a->frame;
    a->frame = n;
    if (rc == DTS_OK && ch == 2) push_pcm(c, a, c->pcm, n), a->good++;
    else push_pcm(c, a, silence, n), a->bad++; /* damaged: silence keeps the timing */
    return n;
}

static bool code_audio(Conv *c, struct ConvAudio *a, const MkvFrame *fr) {
    static const float silence[MAX_AFRAME * 2];
    const int64_t frame_ns = (int64_t)a->frame * NS / a->rate;
    if (fr->pts != MKV_NOPTS && a->next != MKV_NOPTS && fr->pts > a->next + frame_ns / 2) {
        /* a hole in the sound (damaged disc): fill it with silence so the picture stays in sync */
        int64_t gap = CMIN((fr->pts - a->next + frame_ns / 2) / frame_ns, 10 * NS / frame_ns); /* <= ~10 s: a bigger jump is a broken timestamp, not a hole */
        for (int64_t g = 0; g < gap && !c->failed; g++) push_pcm(c, a, silence, a->frame);
    }
    const uint8_t *p = fr->data;
    size_t left = fr->size;
    int64_t samples = 0; /* the sound this block held (an E-AC-3 block may hold extra-channel frames: no time) */
    while (a->dtsd && !c->failed && !a->dead) {
        int size = 0, n = code_dts(c, a, p, left, &size);
        if (n <= 0) break;
        p += size, left -= (size_t)size, samples += n;
    }
    while (a->ac3 && left >= AC3_HEADER_BYTES && !c->failed && !a->dead) {
        int size = ac3_frame_size(p, left);
        if (size <= 0 || (size_t)size > left) break;
        int ch = 0;
        Ac3Info in = {.samples = a->frame};
        int rc = ac3_decode(a->ac3, p, (size_t)size, c->pcm, &ch, &in);
        p += size, left -= (size_t)size;
        if (rc == AC3_SKIP) continue;
        if (rc == AC3_ERR_UNSUPPORTED)
            return lose(c, a, "Its sound is %s coded in a way the converter can't decode yet", in.eac3 ? "Dolby Digital Plus" : "Dolby");
        if (rc == AC3_OK && in.sample_rate != a->rate) /* the file said another rate: the copy would play too fast or slow */
            return lose(c, a, "Its %s sound isn't at the rate the file says", in.eac3 ? "Dolby Digital Plus" : "Dolby");
        int n = in.samples > 0 && in.samples <= AC3_SAMPLES ? in.samples : AC3_SAMPLES;
        a->frame = n;
        if (rc == AC3_OK && ch == 2) push_pcm(c, a, c->pcm, n), a->good++;
        else push_pcm(c, a, silence, n), a->bad++; /* damaged: silence keeps the timing */
        samples += n;
    }
    int64_t took = samples * NS / a->rate;
    if (fr->pts != MKV_NOPTS) a->next = fr->pts + took;
    else if (a->next != MKV_NOPTS) a->next += took;
    return !c->failed;
}

/* ---- subtitles: text tracks become SubRip (.srt) texts; picture tracks .sup (PGS) and .idx + .sub (VobSub) ---- */

enum { SK_NONE, SK_SRT, SK_ASS, SK_VTT, SK_PGS, SK_VOBSUB };

/* What a track is to us. A compressed one (zlib, bzlib, lzo - mkvmerge can do that to subtitles; header
   stripping is undone by mkv.c) is left out quietly: none of the household's files were (2026-10-23). */
static int sub_kind(const MkvTrack *t) {
    const AvCodec *k = av_conv_codec(t->codec);
    if (t->type != MKV_SUBTITLE || t->compressed || !k || k->type != MKV_SUBTITLE) return SK_NONE;
    if (!strcmp(k->id, "S_TEXT/UTF8")) return SK_SRT;
    if (!strcmp(k->id, "S_TEXT/WEBVTT")) return SK_VTT;
    if (!strcmp(k->id, "S_HDMV/PGS")) return SK_PGS;
    if (!strcmp(k->id, "S_VOBSUB")) return SK_VOBSUB;
    return SK_ASS; /* S_TEXT/ASS, S_TEXT/SSA */
}

static bool is_pic(const struct ConvSub *s) { return s->kind == SK_PGS || s->kind == SK_VOBSUB; }

/* The first AV_CONV_MAX_SUBS subtitle tracks we take, in file order; the bounds for picture tracks. */
static void choose_subs(Conv *c) {
    size_t b = c->j->pic_bytes;
    c->pic_max = b && b < AV_CONV_PIC_BYTES ? b : AV_CONV_PIC_BYTES;
    c->pic_total = c->pic_max * (AV_CONV_PIC_TOTAL / AV_CONV_PIC_BYTES);
    for (int i = 0; i < mkv_track_count(c->mkv) && c->nsub < AV_CONV_MAX_SUBS; i++) {
        const MkvTrack *t = mkv_track(c->mkv, i);
        int kind = sub_kind(t);
        if (kind) c->sub[c->nsub++] = (struct ConvSub){.t = t, .kind = kind};
    }
}

static void sub_put(struct ConvSub *s, const char *p, size_t n) {
    if (s->full) return;
    if (s->n + n + 1 > AV_CONV_SUB_BYTES) { /* far past any real track: keep what came */
        s->full = true;
        return;
    }
    if (s->n + n + 1 > s->cap) {
        size_t cap = CMAX(s->cap * 2, CMAX(s->n + n + 1, (size_t)16384));
        char *q = realloc(s->p, cap);
        if (!q) {
            s->full = true;
            return;
        }
        s->p = q;
        s->cap = cap;
    }
    memcpy(s->p + s->n, p, n);
    s->n += n;
    s->p[s->n] = '\0';
}

static void srt_time(char *out, size_t cap, int64_t ns) { /* (ns >= 0) */
    uint64_t ms = (uint64_t)ns / 1000000;
    snprintf(out, cap, "%02u:%02u:%02u,%03u", (unsigned)(ms / 3600000 % 100), (unsigned)(ms / 60000 % 60),
             (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000));
}

/* WebVTT cue text as SubRip has it: <i> <b> <u> kept (their classes dropped: <i.loud> -> <i>), every other
   tag left out - <c.x>, <v Name>, <lang en>, <ruby>, inline timestamps <00:00:01.000> - and <rt>'s text with
   it (the reading above the letters); &amp; &lt; &gt; &nbsp; &lrm; &rlm; unescaped. A cue's settings and id
   ride in BlockAdditions, which mkv.c doesn't hand out: they're not needed for SRT. */
static size_t vtt_text(const char *t, size_t size, char *out, size_t cap) {
    static const struct {
        const char *name, *text;
    } ENT[] = {{"amp;", "&"}, {"lt;", "<"}, {"gt;", ">"}, {"nbsp;", "\xC2\xA0"}, {"lrm;", "\xE2\x80\x8E"}, {"rlm;", "\xE2\x80\x8F"}};
    size_t n = 0;
    bool rt = false;
    for (size_t i = 0; i < size && n + 4 < cap;) {
        char ch = t[i];
        if (ch == '<') {
            size_t e = i + 1;
            while (e < size && t[e] != '>') e++;
            bool close = i + 1 < size && t[i + 1] == '/';
            size_t a = i + 1 + close, b = a;
            while (b < e && ((t[b] >= 'a' && t[b] <= 'z') || (t[b] >= 'A' && t[b] <= 'Z'))) b++;
            if (b - a == 1 && (t[a] == 'i' || t[a] == 'b' || t[a] == 'u') && !rt) {
                out[n++] = '<';
                if (close) out[n++] = '/';
                out[n++] = t[a];
                out[n++] = '>';
            } else if (b - a == 2 && !memcmp(t + a, "rt", 2)) {
                rt = !close;
            } else if (b - a == 4 && !memcmp(t + a, "ruby", 4) && close) {
                rt = false; /* (</ruby> ends an <rt> left open) */
            }
            i = e < size ? e + 1 : size;
            continue;
        }
        if (rt) {
            i++;
            continue;
        }
        size_t k = 0;
        while (ch == '&' && k < sizeof ENT / sizeof ENT[0] &&
               (size - i - 1 < strlen(ENT[k].name) || memcmp(t + i + 1, ENT[k].name, strlen(ENT[k].name))))
            k++;
        if (ch == '&' && k < sizeof ENT / sizeof ENT[0]) {
            memcpy(out + n, ENT[k].text, strlen(ENT[k].text));
            n += strlen(ENT[k].text);
            i += 1 + strlen(ENT[k].name);
            continue;
        }
        out[n++] = ch;
        i++;
    }
    return n;
}

/* A cue's text as SubRip has it: SRT as it is; ASS/SSA its ninth field (Text) without the override tags
   ({...}), \N \n as line breaks and \h a space; WebVTT see vtt_text. Then tidied: no \r or NULs, no blank
   line (one would end the cue), no breaks or spaces at the end. */
static size_t cue_text(int kind, const char *t, size_t size, char *out, size_t cap) {
    size_t n = 0;
    if (kind == SK_VTT) {
        n = vtt_text(t, size, out, cap);
    } else {
        const char *e = t + size;
        if (kind == SK_ASS) /* ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text */
            for (int commas = 0; t < e && commas < 8; t++) commas += *t == ',';
        bool tag = false;
        for (; t < e && n + 2 < cap; t++) {
            char ch = *t;
            if (kind == SK_ASS && ch == '{') tag = true;
            else if (kind == SK_ASS && ch == '}' && tag) tag = false;
            else if (tag) continue;
            else if (kind == SK_ASS && ch == '\\' && t + 1 < e && (t[1] == 'N' || t[1] == 'n' || t[1] == 'h')) {
                out[n++] = t[1] == 'h' ? ' ' : '\n';
                t++;
            } else out[n++] = ch;
        }
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = out[i];
        if (ch == '\r' || ch == '\0' || (ch == '\n' && (!m || out[m - 1] == '\n'))) continue;
        out[m++] = ch;
    }
    while (m && (out[m - 1] == '\n' || out[m - 1] == ' ')) m--;
    return m;
}

/* One cue: its text cleaned up (cue_text), then numbered and timed on the copy's clock - which starts
   at the first picture. */
static void collect_text(Conv *c, struct ConvSub *s, const MkvFrame *fr) {
    if (fr->pts == MKV_NOPTS || s->full || fr->size > 16384) return;
    int64_t start = fr->pts - (c->v0 != MKV_NOPTS ? c->v0 : 0);
    int64_t end = start + (fr->duration > 0 && fr->duration < MAX_TS ? fr->duration : 3 * NS);
    if (end <= 0) return;
    if (start < 0) start = 0;
    char text[16384];
    size_t n = cue_text(s->kind, (const char *)fr->data, fr->size, text, sizeof text);
    if (!n) return;
    char head[96], a[32], b[32];
    srt_time(a, sizeof a, start);
    srt_time(b, sizeof b, end);
    int hn = snprintf(head, sizeof head, "%d\n%s --> %s\n", s->cues + 1, a, b);
    if (s->n + (size_t)hn + n + 2 + 1 > AV_CONV_SUB_BYTES) { /* (whole cues only) */
        s->full = true;
        return;
    }
    sub_put(s, head, (size_t)hn);
    sub_put(s, text, n);
    sub_put(s, "\n\n", 2);
    if (!s->full) s->cues++;
}

/* A picture track past a bound: left out whole (a cut one would end part-way through the film). */
static void drop_pic(Conv *c, struct ConvSub *s) {
    c->pic_held -= s->cap;
    free(s->p);
    free(s->lines);
    s->p = s->lines = NULL;
    s->n = s->cap = s->ln = s->lcap = 0;
    s->cues = 0;
    s->full = true;
}

/* Room for `more` bytes in a picture track: at most AV_CONV_PIC_BYTES, and every picture track together
   at most AV_CONV_PIC_TOTAL (counted as allocated). False - and the track dropped - past either. */
static bool pic_room(Conv *c, struct ConvSub *s, size_t more) {
    if (s->full) return false;
    if (more <= s->cap - s->n) return true;
    size_t others = c->pic_held - s->cap, limit = CMIN(c->pic_max, c->pic_total - others);
    if (more > limit - s->n) { /* (s->n <= s->cap <= limit) */
        drop_pic(c, s);
        return false;
    }
    size_t need = s->n + more, cap = CMIN(limit, CMAX(need, CMAX(s->cap + s->cap / 2, (size_t)65536)));
    char *q = realloc(s->p, cap);
    if (!q) {
        drop_pic(c, s);
        return false;
    }
    c->pic_held += cap - s->cap;
    s->p = q;
    s->cap = cap;
    return true;
}

/* A picture track's time on the copy's clock in 90 kHz ticks: clamped at 0 (and at 2^33 - 1, ~26 hours,
   the most an MPEG timestamp holds; .sup keeps 32 bits of it). */
static uint64_t pic_time(const Conv *c, int64_t pts) {
    int64_t t = pts - (c->v0 != MKV_NOPTS ? c->v0 : 0);
    if (t <= 0) return 0;
    int64_t k = ticks(t, 90000);
    return k >= ((int64_t)1 << 33) ? ((uint64_t)1 << 33) - 1 : (uint64_t)k;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* Blu-ray PGS: the block's segments (type, u16 size, payload - a block may hold several), each as one .sup
   record: "PG", PTS (u32, 90 kHz, the block's time), DTS 0, the segment as it is. A segment cut short by the
   block's end is left out. A subpicture = a presentation composition (0x16) that shows objects. */
static void collect_pgs(Conv *c, struct ConvSub *s, const MkvFrame *fr) {
    if (fr->pts == MKV_NOPTS || s->full) return;
    uint32_t pts = (uint32_t)CMIN(pic_time(c, fr->pts), (uint64_t)UINT32_MAX);
    for (const uint8_t *p = fr->data, *e = p + fr->size; e - p >= 3;) {
        size_t len = 3 + ((size_t)p[1] << 8 | p[2]);
        if (len > (size_t)(e - p) || !pic_room(c, s, 10 + len)) return;
        uint8_t *o = (uint8_t *)s->p + s->n;
        o[0] = 'P', o[1] = 'G';
        put_be32(o + 2, pts);
        put_be32(o + 6, 0);
        memcpy(o + 10, p, len);
        s->n += 10 + len;
        if (p[0] == 0x16 && len >= 3 + 11 && p[3 + 10] > 0) s->cues++; /* (its number of composition objects) */
        p += len;
    }
}

/* An MPEG-2 program stream pack header (14 bytes): SCR = t (90 kHz), mux rate as mkvextract writes it */
static uint8_t *ps_pack(uint8_t *o, uint64_t t) {
    static const uint8_t rest[] = {0x01, 0x89, 0xC3, 0xF8}; /* mux rate + markers, no stuffing */
    o[0] = 0, o[1] = 0, o[2] = 1, o[3] = 0xBA;
    o[4] = (uint8_t)(0x44 | (t >> 27 & 0x38) | (t >> 28 & 0x03));
    o[5] = (uint8_t)(t >> 20);
    o[6] = (uint8_t)((t >> 12 & 0xF8) | 0x04 | (t >> 13 & 0x03));
    o[7] = (uint8_t)(t >> 5);
    o[8] = (uint8_t)((t << 3 & 0xF8) | 0x04);
    o[9] = 0x01;
    memcpy(o + 10, rest, 4);
    return o + 14;
}

#define VOB_PACK 2048
#define VOB_FIRST (VOB_PACK - 14 - 15) /* a subpicture's first pack: pack header, PES header with PTS, substream */
#define VOB_NEXT (VOB_PACK - 14 - 10)  /* the following ones: no PTS */

/* DVD VobSub: the block is one subpicture unit (SPU). It goes into the .sub as mkvextract writes it -
   2048-byte packs: a pack header, then a private stream 1 PES (0xBD) with the PTS on the first, substream
   0x20, the SPU's bytes; the last pack filled by stuffing bytes in the PES header (under 6) or a padding
   stream (0xBE) - and the .idx gets "timestamp: HH:MM:SS:mmm, filepos: <its first pack, 9 hex digits>". */
static void collect_vobsub(Conv *c, struct ConvSub *s, const MkvFrame *fr) {
    if (fr->pts == MKV_NOPTS || s->full || !fr->size) return;
    size_t size = fr->size, packs = size <= VOB_FIRST ? 1 : 1 + (size - VOB_FIRST + VOB_NEXT - 1) / VOB_NEXT;
    if (packs > c->pic_max / VOB_PACK) {
        drop_pic(c, s);
        return;
    }
    char line[64];
    uint64_t t = pic_time(c, fr->pts), ms = t / 90;
    int ln = snprintf(line, sizeof line, "timestamp: %02u:%02u:%02u:%03u, filepos: %09llx\n", (unsigned)(ms / 3600000 % 100),
                      (unsigned)(ms / 60000 % 60), (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000), (unsigned long long)s->n);
    if (s->ln + (size_t)ln + 1 > s->lcap) { /* (bounded by the .sub: under 50 bytes for each 2 KB there) */
        size_t cap = CMAX(s->lcap * 2, (size_t)4096);
        char *q = realloc(s->lines, cap);
        if (!q) {
            drop_pic(c, s);
            return;
        }
        s->lines = q;
        s->lcap = cap;
    }
    if (!pic_room(c, s, packs * VOB_PACK)) return;
    memcpy(s->lines + s->ln, line, (size_t)ln + 1);
    s->ln += (size_t)ln;
    uint8_t *o = (uint8_t *)s->p + s->n;
    for (size_t off = 0, k = 0; k < packs; k++) {
        size_t room = k ? VOB_NEXT : VOB_FIRST, take = CMIN(size - off, room), pad = room - take;
        size_t stuff = pad < 6 ? pad : 0; /* (only the last pack has any pad) */
        uint8_t *q = ps_pack(o, t);
        size_t pes = (k ? 4 : 9) + stuff + take; /* the bytes after its length field */
        q[0] = 0, q[1] = 0, q[2] = 1, q[3] = 0xBD, q[4] = (uint8_t)(pes >> 8), q[5] = (uint8_t)pes;
        q[6] = 0x81, q[7] = k ? 0x00 : 0x80, q[8] = (uint8_t)((k ? 0 : 5) + stuff);
        q += 9;
        if (!k) { /* the PTS: '0010', then its 33 bits in three parts, each with a marker bit */
            q[0] = (uint8_t)(0x21 | (t >> 29 & 0x0E)), q[1] = (uint8_t)(t >> 22), q[2] = (uint8_t)(0x01 | (t >> 14 & 0xFE));
            q[3] = (uint8_t)(t >> 7), q[4] = (uint8_t)(0x01 | (t << 1 & 0xFE));
            q += 5;
        }
        memset(q, 0xFF, stuff);
        q += stuff;
        *q++ = 0x20; /* the substream: subpicture 0 */
        memcpy(q, fr->data + off, take);
        q += take;
        if (pad > stuff) { /* a padding stream fills the pack */
            size_t left = pad - 6;
            q[0] = 0, q[1] = 0, q[2] = 1, q[3] = 0xBE, q[4] = (uint8_t)(left >> 8), q[5] = (uint8_t)left;
            memset(q + 6, 0xFF, left);
        } /* (every pack is whole: 2048 bytes) */
        o += VOB_PACK;
        off += take;
    }
    s->n += packs * VOB_PACK;
    s->cues++;
}

static void collect(Conv *c, const MkvFrame *fr) {
    for (int k = 0; k < c->nsub; k++) {
        struct ConvSub *s = &c->sub[k];
        if (fr->track != s->t->number) continue;
        if (s->kind == SK_PGS) collect_pgs(c, s, fr);
        else if (s->kind == SK_VOBSUB) collect_vobsub(c, s, fr);
        else collect_text(c, s, fr);
    }
}

/* ISO 639-2 (bibliographic and terminology) -> 639-1, for the .idx's "id:" (media/subs.c's list) */
static const char *const LANG3[][2] = {
    {"eng", "en"}, {"swe", "sv"}, {"nor", "no"}, {"nob", "no"}, {"nno", "no"}, {"dan", "da"}, {"fin", "fi"},
    {"ger", "de"}, {"deu", "de"}, {"fre", "fr"}, {"fra", "fr"}, {"spa", "es"}, {"ita", "it"}, {"dut", "nl"},
    {"nld", "nl"}, {"pol", "pl"}, {"por", "pt"}, {"jpn", "ja"}, {"kor", "ko"}, {"chi", "zh"}, {"zho", "zh"},
    {"rus", "ru"}, {"ara", "ar"}, {"tur", "tr"}, {"ice", "is"}, {"isl", "is"}, {"cze", "cs"}, {"ces", "cs"},
    {"slo", "sk"}, {"slk", "sk"}, {"slv", "sl"}, {"hun", "hu"}, {"gre", "el"}, {"ell", "el"}, {"heb", "he"},
    {"tha", "th"}, {"vie", "vi"}, {"ind", "id"}, {"may", "ms"}, {"msa", "ms"}, {"ukr", "uk"}, {"rum", "ro"},
    {"ron", "ro"}, {"bul", "bg"}, {"hrv", "hr"}, {"srp", "sr"}, {"bos", "bs"}, {"est", "et"}, {"lav", "lv"},
    {"lit", "lt"}, {"per", "fa"}, {"fas", "fa"}, {"cat", "ca"}, {"glg", "gl"}, {"baq", "eu"}, {"eus", "eu"},
    {"wel", "cy"}, {"cym", "cy"}, {"gle", "ga"}, {"tam", "ta"}, {"tel", "te"}, {"ben", "bn"}, {"urd", "ur"},
    {"tgl", "tl"}, {"afr", "af"}, {"alb", "sq"}, {"sqi", "sq"}, {"mac", "mk"}, {"mkd", "mk"}, {"geo", "ka"},
    {"kat", "ka"}, {"arm", "hy"}, {"hye", "hy"}, {"aze", "az"}, {"kaz", "kk"}, {"uzb", "uz"}, {"mon", "mn"},
    {"nep", "ne"}, {"sin", "si"}, {"khm", "km"}, {"lao", "lo"}, {"bur", "my"}, {"mya", "my"}, {"swa", "sw"},
    {"amh", "am"}, {"yor", "yo"}, {"zul", "zu"}, {"som", "so"}, {"mlt", "mt"}, {"ltz", "lb"}, {"fao", "fo"},
    {"epo", "eo"}, {"lat", "la"}, {"kur", "ku"}, {"pus", "ps"}, {"tir", "ti"}, {"hin", "hi"},
};

/* The track's language as the two letters an .idx names it by ("eng", "en-US" -> "en"); "--" when unknown. */
static void lang2(const char *lang, char out[3]) {
    char l[4] = "";
    size_t n = strcspn(lang, "-_");
    for (size_t i = 0; i < n && n <= 3; i++) l[i] = (char)(lang[i] >= 'A' && lang[i] <= 'Z' ? lang[i] + 32 : lang[i]);
    memcpy(out, "--", 3);
    if (n == 2 && l[0] >= 'a' && l[0] <= 'z' && l[1] >= 'a' && l[1] <= 'z') out[0] = l[0], out[1] = l[1];
    for (size_t i = 0; n == 3 && i < sizeof LANG3 / sizeof LANG3[0]; i++)
        if (!strcmp(l, LANG3[i][0])) memcpy(out, LANG3[i][1], 3);
}

/* A sound's language as an MP4's mdhd names it (ISO 639-2, three lower-case letters): a 3-letter Language as it is
   ("eng", "rus"), a LanguageIETF's first part in three ("sv-SE" -> "swe", "en" -> "eng"); else "und". */
static void lang3(const char *lang, char out[4]) {
    char l[4] = "";
    size_t n = strcspn(lang, "-_");
    bool letters = n == 2 || n == 3;
    for (size_t i = 0; i < n && letters; i++) {
        char ch = (char)(lang[i] >= 'A' && lang[i] <= 'Z' ? lang[i] + 32 : lang[i]);
        letters = ch >= 'a' && ch <= 'z';
        l[i] = ch;
    }
    memcpy(out, letters && n == 3 ? l : "und", 4);
    for (size_t i = 0; letters && n == 2 && i < sizeof LANG3 / sizeof LANG3[0]; i++)
        if (!strcmp(l, LANG3[i][1])) {
            memcpy(out, LANG3[i][0], 4);
            break;
        }
}

/* The .idx: the track's CodecPrivate (VobSub's own header: size, palette ...; the "# VobSub index file, v7"
   line put in front when it lacks it - players look for it), then "id: xx, index: 0" and the timestamp lines. */
static char *vobsub_idx(const struct ConvSub *s, size_t *len) {
    static const char HEAD[] = "# VobSub index file, v7 (do not modify this line!)\n";
    const char *priv = (const char *)s->t->priv;
    size_t pn = priv ? s->t->priv_len : 0;
    const char *nul = pn ? memchr(priv, 0, pn) : NULL;
    if (nul) pn = (size_t)(nul - priv);
    if (pn >= 3 && !memcmp(priv, "\xEF\xBB\xBF", 3)) priv += 3, pn -= 3; /* (a byte order mark) */
    bool head = pn >= 22 && !memcmp(priv, HEAD, 22);
    char id[32], l[3];
    lang2(s->t->lang, l);
    int idn = snprintf(id, sizeof id, "\nid: %s, index: 0\n", l);
    size_t n = (head ? 0 : sizeof HEAD - 1) + pn + 1 + (size_t)idn + s->ln;
    char *p = malloc(n + 1), *q = p;
    if (!p) return NULL;
    if (!head) memcpy(q, HEAD, sizeof HEAD - 1), q += sizeof HEAD - 1;
    if (pn) memcpy(q, priv, pn), q += pn;
    if (pn && priv[pn - 1] != '\n') *q++ = '\n';
    memcpy(q, id, (size_t)idn), q += idn;
    if (s->ln) memcpy(q, s->lines, s->ln), q += s->ln;
    *q = '\0';
    *len = (size_t)(q - p);
    return p;
}

static void free_sub(Conv *c, struct ConvSub *s) {
    if (is_pic(s)) c->pic_held -= s->cap;
    free(s->p);
    free(s->lines);
    s->p = s->lines = NULL;
    s->n = s->cap = s->ln = s->lcap = 0;
    s->cues = 0;
    s->full = false;
}

/* The gathered tracks go to the job (empty or dropped ones left out) when ok; else freed. */
static void hand_subs(Conv *c, bool ok) {
    AvConvJob *j = c->j;
    for (int k = 0; k < c->nsub; k++) {
        struct ConvSub *s = &c->sub[k];
        if (ok && s->cues && s->p && j->nsubs < AV_CONV_MAX_SUBS) {
            AvConvSub *o = &j->subs[j->nsubs];
            memset(o, 0, sizeof *o);
            if (s->kind == SK_VOBSUB && !(o->idx = vobsub_idx(s, &o->idx_len))) {
                free_sub(c, s);
                continue;
            }
            snprintf(o->lang, sizeof o->lang, "%s", s->t->lang);
            snprintf(o->name, sizeof o->name, "%s", s->t->name);
            o->forced = s->t->forced;
            o->is_default = s->t->is_default;
            o->cues = s->cues;
            if (is_pic(s)) {
                o->kind = s->kind == SK_PGS ? AV_SUB_PGS : AV_SUB_VOBSUB;
                uint8_t *fit = realloc(s->p, s->n); /* (the buffer grew in steps: give back what's spare) */
                o->data = fit ? fit : (uint8_t *)s->p;
                o->size = s->n;
                c->pic_held -= s->cap;
            } else {
                o->kind = AV_SUB_TEXT;
                o->srt = s->p;
                o->len = s->n;
            }
            j->nsubs++;
            s->p = NULL;
            s->cap = 0;
        }
        free_sub(c, s);
    }
}

void av_convert_free(AvConvJob *j) {
    for (int k = 0; k < j->nsubs && k < AV_CONV_MAX_SUBS; k++) {
        AvConvSub *s = &j->subs[k];
        free(s->srt);
        free(s->data);
        free(s->idx);
        s->srt = s->idx = NULL;
        s->data = NULL;
        s->len = s->size = s->idx_len = 0;
    }
    j->nsubs = 0;
}

/* The copy's clock starts at the first picture: the first video block with a time, in file order - what
   the conversion's first look finds (survey). Found here without decoding anything. */
static bool find_v0(Conv *c) {
    if (c->v0 != MKV_NOPTS || !c->vt) return true;
    mkv_rewind(c->mkv);
    MkvFrame fr;
    int r;
    while ((r = next_frame(c, &fr)) > 0) {
        if (stopped(c)) return fail(c, "Stopped", NULL);
        if (fr.track == c->vt->number && fr.pts != MKV_NOPTS) {
            c->v0 = fr.pts;
            return true;
        }
    }
    return r < 0 ? fail(c, "Couldn't read the original (disk error?)", NULL) : true;
}

/* The chosen subtitle tracks alone, from the start (pictures and sound skipped); progress from pm0 to 999. */
static bool subs_pass(Conv *c, int pm0) {
    if (!find_v0(c)) return false;
    mkv_rewind(c->mkv);
    MkvFrame fr;
    int r;
    while ((r = next_frame(c, &fr)) > 0) {
        if (stopped(c)) return fail(c, "Stopped", NULL);
        collect(c, &fr);
        if (c->j->in.size > 0 && fr.pos >= 0)
            set_progress(c, pm0 + (int)((double)(999 - pm0) * (double)CMIN(fr.pos, c->j->in.size) / (double)c->j->in.size));
    }
    return r < 0 ? fail(c, "Couldn't read the original (disk error?)", NULL) : true;
}

/* ---- the passes ---- */
static bool restart_decoder(Conv *c) {
    if (c->hv) {
        if (c->dec5) hevc_free(c->dec5);
        c->dec5 = hevc_new();
        if (!c->dec5) return fail(c, "Out of memory", NULL);
        if (!hevc_config(c->dec5, c->vt->priv, c->vt->priv_len))
            return unsupported(c, "Its HEVC picture has no readable settings record (CodecPrivate)", NULL);
        return true;
    }
    if (c->m4) {
        if (c->dec4) mpeg4_free(c->dec4);
        c->dec4 = mpeg4_new();
        if (!c->dec4) return fail(c, "Out of memory", NULL);
        if (c->nvol) mpeg4_decode(c->dec4, c->vol, c->nvol, MPEG4_NOPTS);
        return true;
    }
    if (c->dec) mpeg2_free(c->dec);
    c->dec = mpeg2_new();
    if (!c->dec) return fail(c, "Out of memory", NULL);
    if (c->vt->priv_len) mpeg2_decode(c->dec, c->vt->priv, c->vt->priv_len, MPEG2_NOPTS);
    return true;
}

/* An AVI's picture kept as it was (V_MS/VFW/FOURCC: CodecPrivate = a BITMAPINFOHEADER, then the codec's own
   setup): MPEG-4 Part 2 by its fourcc - not DivX 3's MS-MPEG4 (DIV3, MP43 ...), another codec. */
static bool vfw_mpeg4(const MkvTrack *t, char cc[5]) {
    memcpy(cc, "????", 5);
    if (t->priv_len < 40) return false;
    for (int i = 0; i < 4; i++) {
        uint8_t ch = t->priv[16 + i];
        cc[i] = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch >= ' ' && ch < 127 ? ch : '?');
    }
    static const char *const MPEG4[] = {"XVID", "DIVX", "DX50", "FMP4", "MP4V", "3IV2", "XVIX", "RMP4", "SIPP", "DM4V", "M4S2"};
    for (size_t i = 0; i < sizeof MPEG4 / sizeof *MPEG4; i++)
        if (!memcmp(cc, MPEG4[i], 4)) return true;
    return false;
}

/* An MPEG-4 picture's usual length (ns): the gap most common between the first pictures' times (median), else the
   file's stated one, else 1/25 s - for the last picture and any without a time; and so the encoder's rate. */
static int cmp_gap(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static int64_t usual_gap(Conv *c) {
    int64_t gaps[ANALYZE_FRAMES];
    int n = 0;
    for (int i = 1; i < c->nseen; i++)
        if (c->seen[i] != MKV_NOPTS && c->seen[i - 1] != MKV_NOPTS && c->seen[i] > c->seen[i - 1])
            gaps[n++] = c->seen[i] - c->seen[i - 1];
    if (n >= 4) {
        qsort(gaps, (size_t)n, sizeof *gaps, cmp_gap);
        if (gaps[n / 2] >= NS / 240 && gaps[n / 2] <= 10 * NS) return gaps[n / 2];
    }
    if (c->vt->default_duration >= NS / 240 && c->vt->default_duration <= 10 * NS) return c->vt->default_duration;
    return NS / 25;
}

/* A block of a kept sound with a time: its first one is where that sound starts */
static void note_a0(Conv *c, const MkvFrame *fr) {
    for (int k = 0; k < c->naud && fr->pts != MKV_NOPTS; k++)
        if (fr->track == c->aud[k].t->number && c->aud[k].a0 == MKV_NOPTS) c->aud[k].a0 = fr->pts;
}

/* The opening window read on for a sound's start: the default's, however far (as ever), the others' within a
   minute of the first picture (none by then: it starts with the picture). */
static bool sound_unseen(const Conv *c, const MkvFrame *fr) {
    if (!c->naud) return false;
    if (c->aud[c->def].a0 == MKV_NOPTS) return true;
    bool soon = fr->pts != MKV_NOPTS && c->v0 != MKV_NOPTS && fr->pts - c->v0 < 60 * NS;
    for (int k = 0; k < c->naud && soon; k++)
        if (c->aud[k].a0 == MKV_NOPTS) return true;
    return false;
}

static int analyze_window(Conv *c, DeintStats *ds, Mpeg2Frame *first, bool *have_first, bool opening) {
    MkvFrame fr;
    int got = 0;
    ds->hist = ds->mhist = 0; /* a jump in time: forget the previous window's history */
    ds->nhist = 0;
    ds->have_thumb = false;
    for (int r; (r = next_frame(c, &fr)) > 0;) {
        if (stopped(c)) return -1;
        if (opening) note_a0(c, &fr);
        if (fr.track != c->vt->number) continue;
        if (opening && c->v0 == MKV_NOPTS && fr.pts != MKV_NOPTS) c->v0 = fr.pts;
        if (got >= ANALYZE_FRAMES) {
            if (opening && sound_unseen(c, &fr)) continue; /* still looking for the first sound */
            break;
        }
        vdec_decode(c, fr.data, fr.size, fr.pts);
        Mpeg2Frame pf;
        while (vdec_frame(c, &pf)) {
            if (!*have_first) *first = pf, *have_first = true;
            if (c->m4 || c->hv) { /* (progressive: nothing to analyse - only the pictures' times) */
                if (opening && c->nseen < ANALYZE_FRAMES) c->seen[c->nseen++] = pf.pts;
            } else {
                DeintIn in = {pf.y, pf.u, pf.v, pf.stride_y, pf.stride_c, pf.width, pf.height, pf.progressive_frame,
                              pf.top_field_first, pf.repeat_first_field, pf.pts};
                deint_analyze(ds, &in, pf.width, pf.height);
            }
            got++;
        }
    }
    return got;
}

/* First look: the stream's shape (size, rate, aspect) and whether it is film or video
   (inverse telecine vs deinterlacing), from windows spread over the whole file - an
   opening title says little. For copies: every video pts. Also the first timestamps. */
static bool survey(Conv *c, Mpeg2Frame *first, DeintStats *ds) {
    MkvFrame fr;
    if (!c->vcode) { /* copy: remember every timestamp, in decode (= file) order */
        int cap = 0;
        for (int r; (r = next_frame(c, &fr)) > 0;) {
            if (stopped(c)) return fail(c, "Stopped", NULL);
            note_a0(c, &fr);
            if (fr.track != c->vt->number) continue;
            if (c->v0 == MKV_NOPTS && fr.pts != MKV_NOPTS) c->v0 = fr.pts;
            if (fr.pts != MKV_NOPTS && c->duration > 0) /* a big film takes minutes to read on a Pi: show it */
                set_progress(c, (int)CMIN(COPY_SURVEY_PM - 1, CMAX(0, (fr.pts - c->v0) * COPY_SURVEY_PM / c->duration)));
            if (c->nvpts >= c->vmax) return too_many(c, true); /* before the timestamps outgrow memory */
            if (c->nvpts == cap) {
                cap = cap ? cap * 2 : 4096;
                int64_t *p = realloc(c->vpts, (size_t)cap * sizeof *c->vpts);
                if (!p) return fail(c, "Out of memory", NULL);
                c->vpts = p;
            }
            c->vpts[c->nvpts] = fr.pts != MKV_NOPTS ? fr.pts : c->nvpts ? c->vpts[c->nvpts - 1] + 1 : 0;
            c->nvpts++;
        }
        return c->nvpts ? true : fail(c, "The video track is empty", NULL);
    }
    bool have_first = false;
    if (!restart_decoder(c) || analyze_window(c, ds, first, &have_first, true) < 0) return fail(c, "Stopped", NULL);
    if (c->dec4 && mpeg4_unsupported(c->dec4)) /* (interlaced Xvid, GMC ...) */
        return unsupported(c, "Its MPEG-4 picture uses %s, which the converter can't decode yet", mpeg4_unsupported(c->dec4));
    if (c->dec5 && hevc_unsupported(c->dec5)) /* (4:2:2, 12 bits, fields ...) */
        return unsupported(c, "Its HEVC picture uses %s, which the converter can't decode yet", hevc_unsupported(c->dec5));
    if (!have_first) return fail(c, "No picture could be decoded", NULL);
    if (c->m4 || c->hv) {
        c->vgap = usual_gap(c);
        return true;
    }
    static const int at_percent[] = {25, 50, 75};
    for (int w = 0; w < 3 && c->duration > 120 * NS; w++) { /* (clips under 2 min: the opening is all) */
        if (!mkv_seek(c->mkv, c->duration * at_percent[w] / 100, c->vt->number) || !restart_decoder(c)) break;
        if (analyze_window(c, ds, first, &have_first, false) < 0) return fail(c, "Stopped", NULL);
    }
    return true;
}

/* One kept sound's MP4 track: AAC copied, or a Dolby/DTS decoder + AAC encoder at its own rate. Its words go to
   j->what when it's the default. */
static bool setup_audio(Conv *c, struct ConvAudio *a, bool is_def) {
    Mp4Track at = {0};
    int64_t lead = a->a0 != MKV_NOPTS && c->v0 != MKV_NOPTS ? a->a0 - c->v0 : 0; /* > 0: sound starts later */
    at.codec = MP4_AAC;
    at.timescale = (uint32_t)a->rate;
    at.channels = 2;
    at.sample_rate = a->rate;
    lang3(a->t->lang, at.lang);
    at.name = a->t->name;
    at.off = !is_def;
    uint8_t asc[2];
    const char *words;
    if (a->code) {
        AacConfig ac = {a->rate, 2, (c->j->audio_kbps ? c->j->audio_kbps : 160) * 1000, 0, 0};
        char err[120] = "";
        bool dts = is_dts(a->t->codec);
        if (dts) a->dtsd = dts_open();
        else a->ac3 = ac3_open();
        a->aac = aac_open(&ac, err, sizeof err);
        if ((!a->ac3 && !a->dtsd) || !a->aac) return fail(c, "Can't set up the sound encoder: %s", err);
        if (a->ac3) ac3_set_layout(a->ac3, AC3_LAYOUT_STEREO);
        if (a->dtsd) dts_set_layout(a->dtsd, DTS_LAYOUT_STEREO);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        if (a->ac3) ac3_set_crc_check(a->ac3, false); /* fuzzers: mutated frames reach the decoder (crafted files can carry good CRCs anyway) */
#endif
        aac_specific_config(a->aac, asc);
        at.config = asc;
        at.config_len = 2;
        at.avg_bitrate = at.max_bitrate = (uint32_t)ac.bitrate;
        at.start = AAC_PRIMING; /* the encoder's first 1024 samples are priming */
        a->fifo = malloc((AAC_FRAME + MAX_AFRAME) * 2 * sizeof *a->fifo);
        if (!c->pcm) c->pcm = malloc((size_t)MAX_AFRAME * DTS_MAX_CHANNELS * sizeof *c->pcm); /* (a frame, every channel: off the stack) */
        if (!a->fifo || !c->pcm) return fail(c, "Out of memory", NULL);
        a->next = MKV_NOPTS;
        a->frame = dts ? 512 : AC3_SAMPLES;
        if (lead < 0) a->skip = ticks(-lead, a->rate);
        words = dts ? " · DTS → AAC" : strcmp(a->t->codec, "A_EAC3") ? " · Dolby Digital → AAC" : " · Dolby Digital Plus → AAC";
    } else if (a->opus) { /* (2.5: 'Opus' + dOps at 48 kHz; the pre-skip the edit list's start, as the spec asks) */
        at.codec = MP4_OPUS;
        at.config = a->dops;
        at.config_len = a->dops_len;
        at.channels = a->channels;
        at.sample_rate = 48000;
        at.timescale = 48000;
        at.start = a->preskip + (lead < 0 ? ticks(-lead, 48000) : 0);
        words = " · Opus copied";
    } else {
        at.codec = a->mp3 ? MP4_MP3 : MP4_AAC;
        at.config = a->mp3 ? NULL : a->t->priv; /* (MP3: no setup data - its frames carry it) */
        at.config_len = a->mp3 ? 0 : a->t->priv_len;
        at.channels = a->t->channels ? a->t->channels : 2;
        at.sample_rate = a->t->rate > 0 ? (int)a->t->rate : a->mp3 ? 44100 : 48000;
        at.timescale = (uint32_t)at.sample_rate;
        if (lead < 0) at.start = ticks(-lead, at.timescale);
        words = a->mp3 ? " · MP3 copied" : " · AAC copied";
    }
    if (lead > 0) at.delay = ticks(lead, at.timescale);
    if (is_def) append(c->j->what, sizeof c->j->what, words);
    a->track = mp4_add_track(c->mp4, &at);
    return a->track >= 0 || fail(c, "The MP4 writer refused a track", NULL);
}

static bool setup_tracks(Conv *c, const Mpeg2Frame *first, const DeintStats *ds) {
    Mp4Track vt = {0};
    uint8_t avcc[512];
    if (c->vcode) {
        int rn = first->rate_num, rd = first->rate_den, dn = first->dar_num, dd = first->dar_den;
        if (c->m4 || c->hv) { /* progressive pictures as they are; the rate the VOL / VUI states, else their usual length's */
            c->dmode = DEINT_PASS;
            if (rn <= 0 || rd <= 0) {
                int64_t mfps = (1000 * NS + c->vgap / 2) / c->vgap, g = gcd64(mfps, 1000);
                rn = (int)(mfps / g), rd = (int)(1000 / g);
            }
            if (c->vt->disp_width > 0 && c->vt->disp_height > 0 && c->vt->disp_unit == 0) /* (the file's shape wins) */
                dn = c->vt->disp_width, dd = c->vt->disp_height;
        } else c->dmode = deint_mode_for(deint_kind(ds));
        deint_rate(c->dmode, rn, rd, &c->out_num, &c->out_den);
        c->w = first->width, c->h = first->height;
        c->di = deint_new(first->width, first->height, c->dmode);
        H264Params hp = {0};
        hp.width = first->width;
        hp.height = first->height;
        hp.fps_num = c->out_num;
        hp.fps_den = c->out_den;
        sar_of(first->width, first->height, dn, dd, &hp.sar_w, &hp.sar_h);
        hp.quality = c->j->quality ? c->j->quality : 23;
        hp.preset = H264_FAST;
        hp.annexb = false;
        hp.colour = first->height >= 720 ? 709 : 601;
        c->enc = h264enc_new(&hp);
        if (!c->di || !c->enc) return fail(c, "Can't set up the converter for this picture size", NULL);
        size_t n = h264enc_avcc(c->enc, avcc, sizeof avcc);
        vt.codec = MP4_H264;
        vt.timescale = c->m4 || c->hv ? 90000 : (uint32_t)c->out_num;
        vt.config = avcc;
        vt.config_len = n;
        vt.width = first->width;
        vt.height = first->height;
        vt.sar_num = hp.sar_w;
        vt.sar_den = hp.sar_h;
        bool pal = first->height == 576;
        vt.primaries = first->height >= 720 ? 1 : pal ? 5 : 6;
        vt.transfer = first->height >= 720 ? 1 : 6;
        vt.matrix = first->height >= 720 ? 1 : pal ? 5 : 6;
        const char *kind = c->dmode == DEINT_IVTC ? "film" : c->dmode == DEINT_PASS ? "progressive" : "video";
        if (c->hv)
            snprintf(c->j->what, sizeof c->j->what, "HEVC video (%d-bit, %.3f fps) → H.264", c->hv_depth, (double)c->out_num / c->out_den);
        else if (c->m4)
            snprintf(c->j->what, sizeof c->j->what, "MPEG-4 video (DivX/Xvid, %.3f fps) → H.264", (double)c->out_num / c->out_den);
        else snprintf(c->j->what, sizeof c->j->what, "DVD video (%s, %.3f fps) → H.264", kind, (double)c->out_num / c->out_den);
    } else {
        if (!c->vt->priv_len) return fail(c, "The video track has no setup data", NULL);
        /* decode order is file order; presentation times are the pts. dts = the sorted
           pts, shifted so that pts - dts never goes negative (B-frames). */
        c->dts = malloc((size_t)c->nvpts * sizeof *c->dts);
        if (!c->dts) return fail(c, "Out of memory", NULL);
        memcpy(c->dts, c->vpts, (size_t)c->nvpts * sizeof *c->dts);
        qsort(c->dts, (size_t)c->nvpts, sizeof *c->dts, cmp_i64);
        for (int i = 0; i < c->nvpts; i++) c->vshift = CMAX(c->vshift, c->dts[i] - c->vpts[i]);
        vt.codec = c->vcopy;
        vt.timescale = 90000;
        vt.config = c->vt->priv;
        vt.config_len = c->vt->priv_len;
        vt.width = c->vt->width;
        vt.height = c->vt->height;
        if (c->vt->disp_width && c->vt->disp_height && c->vt->disp_unit == 0)
            sar_of(c->vt->width, c->vt->height, c->vt->disp_width, c->vt->disp_height, &vt.sar_num, &vt.sar_den);
        vt.start = at_tick(c->vshift); /* the first picture at movie time 0 (as copy_video places it) */
        snprintf(c->j->what, sizeof c->j->what, c->vcopy == MP4_HEVC ? "HEVC copied" : c->vcopy == MP4_AV1 ? "AV1 copied" : "H.264 copied");
    }
    snprintf(vt.lang, sizeof vt.lang, "und");
    c->vtrack = mp4_add_track(c->mp4, &vt);
    if (c->vtrack < 0) return fail(c, "The MP4 writer refused a track", NULL);
    for (int k = 0; k < c->naud; k++) /* the sounds in the file's order, after the picture */
        if (!setup_audio(c, &c->aud[k], k == c->def)) return false;
    if (c->naud > 1) {
        char more[48];
        snprintf(more, sizeof more, c->naud == 2 ? " (+%d more sound track)" : " (+%d more sound tracks)", c->naud - 1);
        append(c->j->what, sizeof c->j->what, more);
    }
    return true;
}

static bool main_pass(Conv *c) {
    MkvFrame fr;
    mkv_rewind(c->mkv);
    if (c->vcode && !restart_decoder(c)) return false; /* start over with a fresh decoder: the survey consumed frames */
    int r = 0;
    while (!c->failed && (r = next_frame(c, &fr)) > 0) {
        if (stopped(c)) return fail(c, "Stopped", NULL);
        if (fr.track == c->vt->number) {
            if (c->vcode) {
                vdec_decode(c, fr.data, fr.size, fr.pts);
                decoded(c);
            } else {
                copy_video(c, &fr);
            }
            if (fr.pts != MKV_NOPTS && c->v0 != MKV_NOPTS && c->duration > 0) {
                int64_t base = c->vcode ? 0 : COPY_SURVEY_PM;
                set_progress(c, (int)CMIN(999, base + CMAX(0, (fr.pts - c->v0) * (1000 - base) / c->duration)));
            }
            if (c->j->limit_ns && fr.pts != MKV_NOPTS && c->v0 != MKV_NOPTS && fr.pts - c->v0 >= c->j->limit_ns) break;
            continue;
        }
        struct ConvAudio *a = NULL;
        for (int k = 0; k < c->naud && !a; k++)
            if (fr.track == c->aud[k].t->number) a = &c->aud[k];
        if (!a) collect(c, &fr);
        else if (a->dead) continue;
        else if (a->code) code_audio(c, a, &fr);
        else if (a->mp3) copy_mp3(c, a, &fr);
        else if (a->opus) copy_opus(c, a, &fr);
        else put_audio(c, a, fr.data, fr.size);
    }
    if (c->failed) return false;
    if (r < 0) return fail(c, "Couldn't read the original (disk error?)", NULL);
    if (c->vcode) { /* the last pictures still inside the decoder and the deinterlacer */
        vdec_flush(c);
        decoded(c);
        deint_flush(c->di);
        DeintOut o;
        while (!c->failed && deint_pull(c->di, &o)) encode_out(c, &o);
        uint32_t last = (uint32_t)(c->m4 || c->hv ? CMAX(1, at_tick(c->vgap)) : c->out_den); /* (its usual length) */
        if (c->failed || (c->have_pend && !put_sample(c, c->vtrack, c->pend, c->npend, last, 0, c->pend_key)))
            return false;
    }
    for (int k = 0; k < c->naud && !c->failed; k++) { /* not a silent copy that claims success */
        struct ConvAudio *a = &c->aud[k];
        if (a->code && !a->dead && a->bad > 50 && a->bad > a->good)
            lose(c, a, "Most of its %s sound couldn't be decoded (damaged?)", a->dtsd ? "DTS" : "Dolby");
    }
    for (int k = 0; k < c->naud && !c->failed; k++) { /* the rest of the sound, padded to a whole block, then the encoder's tail */
        struct ConvAudio *a = &c->aud[k];
        if (!a->code || a->dead) continue;
        static const float zero[AAC_FRAME * 2];
        if (a->nfifo) push_pcm(c, a, zero, AAC_FRAME - a->nfifo);
        for (int n; !c->failed && (n = aac_flush(a->aac, c->au, (int)sizeof c->au)) > 0;) put_audio(c, a, c->au, (size_t)n);
    }
    for (int k = 0; k < c->naud && !c->failed; k++) /* what the runs still hold */
        if (!c->aud[k].dead) write_run(c, &c->aud[k]);
    return !c->failed;
}

/* Room for the MP4 index, kept right after the header (written last, so players can start at
   once). A bound for how THIS file gets written, not for any MP4 (mp4_moov_bound's 44 bytes a
   sample left ~7 MB of padding in every copy): per video sample stsz 4 + stts 8 + stss 4, and
   ctts 8 more when H.264/HEVC with B-frames is copied; per sound sample stsz 4 (one duration: one
   stts entry); per chunk stsc 12 + co64 8, where chunks change with every switch between the
   tracks - at most twice the smaller track's count. Pictures: the copied count is exact (the
   survey saw every frame); re-encoded ones at most twice the source rate (field-rate
   deinterlacing). 5% over the stated length for rounding. Too small after all, the writer
   puts the index at the end instead: still a valid file. Never more than the tracks' bounds
   allow (cap_for), and no more pictures than a quarter of the file's bytes (each needs a start
   code and a header, and makes at most two): a file claiming days of length gets no huge padding.
   2+ sounds (2.2): each one's samples, and chunks from their runs - one per AUDIO_RUN frames, each splitting
   the pictures at most once - plus a sound trak's own boxes. */
static int64_t index_room(const Conv *c, const Mpeg2Frame *first) {
    double secs = (c->duration > 0 ? (double)c->duration / NS : 4 * 3600.0) * 1.05 + 2; /* unknown: 4 hours */
    double fps = first->rate_den > 0 ? 2.0 * first->rate_num / first->rate_den : 60;
    int64_t v = c->vcode ? CMIN((int64_t)(secs * CMIN(fps, 120)), CMIN(c->vmax, c->j->in.size / 4)) + 16
                         : (int64_t)c->nvpts + 16;
    int64_t all = 0, runs = 0;
    for (int k = 0; k < c->naud; k++) {
        const struct ConvAudio *s = &c->aud[k];
        double rate = s->code ? s->rate : s->t->rate <= 0 ? 48000 : s->t->rate;
        int64_t a = CMIN((int64_t)(secs * rate / s->fdur), c->amax) + 16; /* (MP3 at 24 kHz: 576 a frame, 42 a second) */
        all += a, runs += a / AUDIO_RUN + 1;
    }
    if (c->naud < 2) { /* one sound (or none): every switch between the two tracks a chunk */
        int64_t chunks = all ? 2 * CMIN(v, all) + 2 : 2;
        return 16384 + v * (c->vcode ? 16 : 24) + all * 4 + chunks * 20;
    }
    return 16384 + v * (c->vcode ? 16 : 24) + all * 4 + (2 * runs + 2) * 20 + (int64_t)c->naud * 1024;
}

/* 2.5: an MP4/MOV that plays everywhere as it is needs no copy - its picture H.264, HEVC as 'hvc1' (what Safari
   plays; 'hev1' is copied as hvc1) or AV1, and every sound AAC, MP3 or Opus - a copy would hold the same (several
   sounds too: the server sends a viewer one at a time). Decided from the moov alone: nothing past it is read. */
static bool plays_as_it_is(const Conv *c) {
    const char *d = mkv_doctype(c->mkv);
    if (strcmp(d, "mp4") && strcmp(d, "mov")) return false;
    const MkvTrack *v = c->vt;
    if (v && strcmp(v->codec, "V_MPEG4/ISO/AVC") && strcmp(v->codec, "V_AV1") &&
        !(!strcmp(v->codec, "V_MPEGH/ISO/HEVC") && !strcmp(v->fourcc, "hvc1")))
        return false;
    if (v && c->j->h264 && strcmp(v->codec, "V_MPEG4/ISO/AVC")) return false; /* (2.6: asked to become H.264) */
    for (int i = 0; i < mkv_track_count(c->mkv); i++) {
        const MkvTrack *t = mkv_track(c->mkv, i);
        if (t->type == MKV_AUDIO && strcmp(t->codec, "A_AAC") && strcmp(t->codec, "A_MPEG/L3") && !is_opus(t->codec)) return false;
    }
    return true;
}

bool av_convert(AvConvJob *j) {
    Conv cv = {0}, *c = &cv;
    c->j = j;
    c->v0 = MKV_NOPTS;
    j->err[0] = j->what[0] = '\0';
    j->permille = 0;
    j->out_bytes = 0;
    j->unsupported = j->as_is = false;
    j->nsubs = 0;
    char err[160] = "";
    c->mkv = mkv_open(&j->in, err, sizeof err);
    if (!c->mkv) {
        if (j->in.size <= 0) return fail(c, "The original is empty", NULL);
        return unsupported(c, "Not a readable MKV or MP4 file: %s", err);
    }
    c->duration = mkv_duration(c->mkv);
    if (c->duration >= MAX_TS) c->duration = 0; /* damage: unknown */
    c->vmax = cap_for(c, VIDEO_PER_S);
    c->amax = cap_for(c, AUDIO_PER_S);
    c->vt = mkv_find_track(c->mkv, MKV_VIDEO);
    if (plays_as_it_is(c)) { /* (2.5: nothing to make - the caller sends it as it is, without a word) */
        j->as_is = true;
        snprintf(j->err, sizeof j->err, "It plays everywhere as it is");
        mkv_close(c->mkv);
        return false;
    }
    /* The sound: every track we can convert, in the file's order (2.2: each language its own track) - a TrueHD
       track ahead of the Dolby one doesn't sink the file, it's left out. The default = the first flagged so
       (else the first); one past the 8th takes the 8th's place. */
    const MkvTrack *first_audio = NULL;
    int def = -1;
    for (int i = 0; i < mkv_track_count(c->mkv); i++) {
        const MkvTrack *t = mkv_track(c->mkv, i);
        if (t->type != MKV_AUDIO) continue;
        if (!first_audio) first_audio = t;
        if (!audio_ok(t)) continue;
        if (c->naud < AV_CONV_MAX_AUDIO) {
            if (def < 0 && t->is_default) def = c->naud;
            c->aud[c->naud++].t = t;
        } else if (def < 0 && t->is_default) {
            def = AV_CONV_MAX_AUDIO - 1;
            c->aud[def].t = t;
        }
    }
    c->def = def < 0 ? 0 : def;
    c->alive = c->naud;
    for (int k = 0; k < c->naud; k++) {
        struct ConvAudio *a = &c->aud[k];
        a->a0 = a->next = MKV_NOPTS;
        a->code = dolby(a->t->codec) || is_dts(a->t->codec);
        a->rate = a->code ? dolby_rate(a->t) : 48000;
        a->mp3 = !strcmp(a->t->codec, "A_MPEG/L3");
        a->fdur = !a->mp3 ? AAC_FRAME : a->t->rate <= 0 || a->t->rate >= 32000 ? 1152 : 576;
        if ((a->opus = is_opus(a->t->codec))) { /* (audio_ok checked its OpusHead) */
            a->dops_len = opus_dops(a->t->priv, a->t->priv_len, a->dops, sizeof a->dops, &a->channels, &a->preskip);
            a->fdur = 960; /* (20 ms, until a packet says) */
        }
    }
    const AvCodec *vk = c->vt ? av_conv_codec(c->vt->codec) : NULL;
    if (!c->vt) unsupported(c, "There's no picture to convert", NULL);
    else if (c->vt->compressed) unsupported(c, "The picture is compressed or encrypted inside the file", NULL);
    else if (!vk || vk->type != MKV_VIDEO)
        unsupported(c, "Its picture is %s, which the converter can't change yet", codec_words(c->vt->codec));
    else if (vk->how == AV_DECODE) { /* MPEG-2 / MPEG-1, or (2.5) MPEG-4 Part 2 */
        c->vcode = true;
        char cc[5];
        if (!strcmp(vk->id, "V_MS/VFW/FOURCC")) {
            if (!vfw_mpeg4(c->vt, cc)) {
                char words[48];
                snprintf(words, sizeof words, "Video for Windows (%s)", cc);
                unsupported(c, "Its picture is %s, which the converter can't change yet", words);
            } else {
                uint32_t hdr = (uint32_t)c->vt->priv[0] | (uint32_t)c->vt->priv[1] << 8 | (uint32_t)c->vt->priv[2] << 16 |
                               (uint32_t)c->vt->priv[3] << 24; /* biSize: its own setup follows */
                if (hdr < 40 || hdr > c->vt->priv_len) hdr = 40;
                c->m4 = true, c->vol = c->vt->priv + hdr, c->nvol = c->vt->priv_len - hdr;
            }
        } else if (!strncmp(vk->id, "V_MPEG4/ISO/", 12)) c->m4 = true, c->vol = c->vt->priv, c->nvol = c->vt->priv_len;
    } else if (j->h264 && !strcmp(vk->id, "V_MPEGH/ISO/HEVC")) { /* (2.6) decoded and made H.264, as MPEG-4 is */
        c->vcode = c->hv = true;
    } else if (j->h264 && !strcmp(vk->id, "V_AV1")) {
        unsupported(c, "Its picture is AV1, which the converter can only copy so far (H.264 from it: not yet)", NULL);
    } else if (!strcmp(vk->id, "V_AV1") && !av1c_ok(c->vt->priv, c->vt->priv_len)) /* (Matroska requires it; an MP4 needs it) */
        unsupported(c, "Its AV1 picture has no settings record (CodecPrivate) in the file", NULL);
    else /* copied: Safari plays HEVC (and AV1, where the device decodes it) in an MP4, never in an MKV */
        c->vcopy = !strcmp(vk->id, "V_MPEGH/ISO/HEVC") ? MP4_HEVC : !strcmp(vk->id, "V_AV1") ? MP4_AV1 : MP4_H264;
    if (!c->failed && first_audio && !c->naud) { /* sound, but none we can convert: say why for the first */
        if (first_audio->compressed) unsupported(c, "The sound is compressed or encrypted inside the file", NULL);
        else if (dolby(first_audio->codec) || is_dts(first_audio->codec))
            unsupported(c, "Can't convert %s sound that isn't at 48, 44.1 or 32 kHz yet", dolby(first_audio->codec) ? "Dolby" : "DTS");
        else if (!strcmp(first_audio->codec, "A_TRUEHD") || !strcmp(first_audio->codec, "A_MLP"))
            unsupported(c, "Its sound is %s, which the converter can't change yet (browsers play it silently)",
                        codec_words(first_audio->codec));
        else unsupported(c, "Its sound is %s, which the converter can't change yet", codec_words(first_audio->codec));
        /* (Vorbis, FLAC ... in an MKV: Chrome and Edge play them as they are - only TrueHD is silent) */
    }
    if (j->want_subs) choose_subs(c);

    Mpeg2Frame first = {0};
    DeintStats ds;
    memset(&ds, 0, sizeof ds);
    if (!c->failed) survey(c, &first, &ds);
    if (!c->failed) {
        c->mp4 = mp4_new(&j->out, 0, index_room(c, &first));
        if (!c->mp4) fail(c, "Out of memory", NULL);
    }
    if (!c->failed) setup_tracks(c, &first, &ds);
    if (!c->failed && !mp4_begin(c->mp4)) fail(c, "Couldn't write the new file", NULL);
    if (!c->failed) main_pass(c);
    if (!c->failed && !mp4_finish(c->mp4)) fail(c, "Couldn't finish the new file (disk full?)", NULL);
    if (!c->failed) j->out_bytes = mp4_bytes(c->mp4);
    bool subs_ok = !c->failed;
    if (c->failed && j->unsupported && c->nsub && !j->skip_refused_subs) {
        /* refused for what it holds (AV1, DTS ...): its subtitles all the same, read alone from the start
           (what the main pass gathered stops part-way). err stays the refusal; c->failed keeps it so. */
        for (int k = 0; k < c->nsub; k++) free_sub(c, &c->sub[k]);
        subs_ok = subs_pass(c, j->permille);
    }
    hand_subs(c, subs_ok);

    if (c->mp4) mp4_free(c->mp4);
    mkv_close(c->mkv);
    if (c->dec) mpeg2_free(c->dec);
    if (c->dec4) mpeg4_free(c->dec4);
    if (c->dec5) hevc_free(c->dec5);
    if (c->di) deint_free(c->di);
    if (c->enc) h264enc_free(c->enc);
    for (int k = 0; k < c->naud; k++) {
        struct ConvAudio *a = &c->aud[k];
        if (a->ac3) ac3_close(a->ac3);
        if (a->dtsd) dts_close(a->dtsd);
        if (a->aac) aac_close(a->aac);
        free(a->fifo);
        free(a->run);
    }
    free(c->pend);
    free(c->vpts);
    free(c->dts);
    free(c->pcm);
    if (!c->failed) set_progress(c, 1000);
    return !c->failed;
}

bool av_subtitles(AvConvJob *j) {
    Conv cv = {0}, *c = &cv;
    c->j = j;
    c->v0 = MKV_NOPTS;
    j->err[0] = j->what[0] = '\0';
    j->permille = 0;
    j->out_bytes = 0;
    j->unsupported = false;
    j->nsubs = 0;
    char err[160] = "";
    c->mkv = mkv_open(&j->in, err, sizeof err);
    if (!c->mkv) {
        if (j->in.size <= 0) return fail(c, "The original is empty", NULL);
        return unsupported(c, "Not a readable MKV or MP4 file: %s", err);
    }
    c->vt = mkv_find_track(c->mkv, MKV_VIDEO); /* (whatever its codec: the clock starts at its first picture) */
    choose_subs(c);
    bool ok = !c->nsub || subs_pass(c, 0); /* none: nothing to read */
    hand_subs(c, ok);
    mkv_close(c->mkv);
    if (ok) set_progress(c, 1000);
    return ok;
}
