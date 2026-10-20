/* modules/av/conv.c — MKV -> MP4 that plays everywhere (HEVC: on Apple devices and newer browsers). See conv.h. */
#include "conv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aac.h"
#include "ac3.h"
#include "deint.h"
#include "h264enc.h"
#include "mpeg2.h"

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

typedef struct {
    AvConvJob *j;
    Mkv *mkv;
    const MkvTrack *vt, *at;
    bool vcode, acode; /* re-encode (true) or copy (false) */
    bool hevc;         /* copying HEVC rather than H.264 */
    int64_t v0, a0;    /* first video / audio pts (ns) */
    int64_t duration;  /* ns, 0 = unknown */
    Mp4 *mp4;
    int vtrack, atrack;
    int64_t nv, na, vmax, amax; /* samples written per track, and the most allowed (cap_for) */
    /* video re-encode */
    Mpeg2 *dec;
    Deint *di;
    int w, h; /* the picture size everything was set up for */
    int dmode, out_num, out_den;
    H264Enc *enc;
    uint8_t *pend; /* the last encoded frame waits for the next to learn its duration */
    size_t npend, cpend;
    int64_t pend_index;
    bool pend_key, have_pend;
    /* video copy */
    int64_t *vpts; /* every video pts in file order (decode order) */
    int nvpts, vi;
    int64_t *dts, vshift; /* sorted pts = decode times; shift keeps composition offsets >= 0 */
    /* audio */
    Ac3Dec *ac3;
    AacEnc *aac;
    float *fifo;
    int nfifo;
    int a_good, a_bad; /* Dolby frames decoded / not */
    int a_frame;       /* samples in the last Dolby frame (1536; E-AC-3 may send 256, 512 or 768) */
    int64_t a_next, skip; /* expected pts of the next Dolby frame (ns); samples to drop at the start */
    uint8_t au[AAC_MAX_AU];
    /* text subtitles, gathered as SRT (j->want_subs) */
    struct ConvSub {
        const MkvTrack *t;
        bool ass; /* S_TEXT/ASS or /SSA: the text is the ninth field, with override tags */
        char *p;
        size_t n, cap;
        int cues;
        bool full;
    } sub[AV_CONV_MAX_SUBS];
    int nsub;
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

static bool dolby(const char *codec) { return !strcmp(codec, "A_AC3") || !strcmp(codec, "A_EAC3"); }

/* Sound we can make AAC of: AAC itself, or Dolby AC-3 / E-AC-3 at 48 kHz (the decoder's output
   rate - DVDs and films always are; anything else would play at the wrong speed). */
static bool audio_ok(const MkvTrack *t) {
    if (t->compressed) return false;
    if (dolby(t->codec)) return t->rate <= 0 || (t->rate > 47999 && t->rate < 48001);
    return !strncmp(t->codec, "A_AAC", 5);
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

/* Every sample goes out through here, counted against its track's bound. */
static bool put_sample(Conv *c, int track, const void *p, size_t n, uint32_t dur, int32_t cts, bool key) {
    bool video = track == c->vtrack;
    int64_t *count = video ? &c->nv : &c->na;
    if (++*count > (video ? c->vmax : c->amax)) return too_many(c, video);
    if (!mp4_sample(c->mp4, track, p, n, dur, cts, key)) return fail(c, "Couldn't write the new file (disk full?)", NULL);
    return true;
}

/* ---- video: re-encode ---- */

static bool put_video(Conv *c, const uint8_t *data, size_t n, int64_t index, bool key) {
    if (c->have_pend) {
        int64_t frames = index - c->pend_index;
        if (frames < 1) frames = 1;
        if (!put_sample(c, c->vtrack, c->pend, c->npend, (uint32_t)(frames * c->out_den), 0, c->pend_key)) return false;
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

static bool encode_out(Conv *c, const DeintOut *o) {
    H264Picture pic = {o->y, o->u, o->v, o->stride_y, o->stride_c, o->stride_c, o->index};
    H264Packet pk;
    if (!h264enc_encode(c->enc, &pic, &pk)) return fail(c, "The H.264 encoder stopped", NULL);
    return put_video(c, pk.data, pk.size, o->index, pk.idr);
}

static bool decoded(Conv *c) {
    Mpeg2Frame pf;
    while (!c->failed && mpeg2_frame(c->dec, &pf)) {
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

/* ---- video: copy (H.264 or HEVC in MKV) ---- */

static int cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static bool copy_video(Conv *c, const MkvFrame *fr) {
    if (c->vi >= c->nvpts) return true; /* more frames than the first pass saw: ignore the tail */
    int i = c->vi++;
    int64_t pts = c->vpts[i], dts = c->dts[i];
    int64_t next = i + 1 < c->nvpts ? c->dts[i + 1] : dts + (c->vt->default_duration ? c->vt->default_duration : NS / 25);
    uint32_t dur = (uint32_t)CMAX(1, ticks(next - dts, 90000));
    int32_t cts = (int32_t)ticks(pts - dts + c->vshift, 90000);
    return put_sample(c, c->vtrack, fr->data, fr->size, dur, cts, fr->key);
}

/* ---- audio ---- */

static bool push_pcm(Conv *c, const float *pcm, int frames) {
    while (frames > 0 && c->skip > 0) { /* audio that starts before the picture */
        pcm += 2, frames--, c->skip--;
    }
    if (frames <= 0) return true;
    memcpy(c->fifo + c->nfifo * 2, pcm, (size_t)frames * 2 * sizeof *pcm);
    c->nfifo += frames;
    while (c->nfifo >= AAC_FRAME) {
        int n = aac_encode(c->aac, c->fifo, c->au, (int)sizeof c->au);
        if (n < 0) return fail(c, "The AAC encoder stopped", NULL);
        if (n > 0 && !put_sample(c, c->atrack, c->au, (size_t)n, AAC_FRAME, 0, true)) return false;
        c->nfifo -= AAC_FRAME;
        memmove(c->fifo, c->fifo + AAC_FRAME * 2, (size_t)c->nfifo * 2 * sizeof *c->fifo);
    }
    return true;
}

static bool code_audio(Conv *c, const MkvFrame *fr) {
    static const float silence[AC3_SAMPLES * 2];
    float pcm[AC3_SAMPLES * AC3_MAX_CHANNELS];
    const int64_t frame_ns = (int64_t)c->a_frame * NS / 48000;
    if (fr->pts != MKV_NOPTS && c->a_next != MKV_NOPTS && fr->pts > c->a_next + frame_ns / 2) {
        /* a hole in the sound (damaged disc): fill it with silence so the picture stays in sync */
        int64_t gap = CMIN((fr->pts - c->a_next + frame_ns / 2) / frame_ns, 10 * NS / frame_ns); /* <= ~10 s: a bigger jump is a broken timestamp, not a hole */
        for (int64_t g = 0; g < gap && !c->failed; g++) push_pcm(c, silence, c->a_frame);
    }
    const uint8_t *p = fr->data;
    size_t left = fr->size;
    int64_t samples = 0; /* the sound this block held (an E-AC-3 block may hold extra-channel frames: no time) */
    while (left >= AC3_HEADER_BYTES && !c->failed) {
        int size = ac3_frame_size(p, left);
        if (size <= 0 || (size_t)size > left) break;
        int ch = 0;
        Ac3Info in = {.samples = c->a_frame};
        int rc = ac3_decode(c->ac3, p, (size_t)size, pcm, &ch, &in);
        p += size, left -= (size_t)size;
        if (rc == AC3_SKIP) continue;
        if (rc == AC3_ERR_UNSUPPORTED)
            return unsupported(c, "Its sound is %s coded in a way the converter can't decode yet", in.eac3 ? "Dolby Digital Plus" : "Dolby");
        int n = in.samples > 0 && in.samples <= AC3_SAMPLES ? in.samples : AC3_SAMPLES;
        c->a_frame = n;
        if (rc == AC3_OK && ch == 2) push_pcm(c, pcm, n), c->a_good++;
        else push_pcm(c, silence, n), c->a_bad++; /* damaged: silence keeps the timing */
        samples += n;
    }
    int64_t took = samples * NS / 48000;
    if (fr->pts != MKV_NOPTS) c->a_next = fr->pts + took;
    else if (c->a_next != MKV_NOPTS) c->a_next += took;
    return !c->failed;
}

/* ---- subtitles: text tracks become SubRip (.srt) texts ---- */

static int sub_kind(const MkvTrack *t) { /* 0 = not one we take, 1 = SRT, 2 = ASS/SSA */
    if (t->type != MKV_SUBTITLE || t->compressed) return 0;
    if (!strcmp(t->codec, "S_TEXT/UTF8")) return 1;
    return !strcmp(t->codec, "S_TEXT/ASS") || !strcmp(t->codec, "S_TEXT/SSA") ? 2 : 0;
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

/* One cue: its text cleaned up (ASS: the text field, override tags and \N), then numbered and
   timed on the copy's clock - which starts at the first picture. */
static void collect_sub(Conv *c, struct ConvSub *s, const MkvFrame *fr) {
    if (fr->pts == MKV_NOPTS || s->full || fr->size > 16384) return;
    int64_t start = fr->pts - (c->v0 != MKV_NOPTS ? c->v0 : 0);
    int64_t end = start + (fr->duration > 0 && fr->duration < MAX_TS ? fr->duration : 3 * NS);
    if (end <= 0) return;
    if (start < 0) start = 0;
    const char *t = (const char *)fr->data, *e = t + fr->size;
    if (s->ass) /* ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text */
        for (int commas = 0; t < e && commas < 8; t++) commas += *t == ',';
    char text[16384];
    size_t n = 0;
    bool tag = false;
    for (; t < e && n + 2 < sizeof text; t++) {
        char ch = *t;
        if (s->ass && ch == '{') tag = true;
        else if (s->ass && ch == '}' && tag) tag = false;
        else if (tag || ch == '\r' || ch == '\0') continue;
        else if (s->ass && ch == '\\' && t + 1 < e && (t[1] == 'N' || t[1] == 'n' || t[1] == 'h')) {
            text[n++] = t[1] == 'h' ? ' ' : '\n';
            t++;
        } else if (ch == '\n' && (!n || text[n - 1] == '\n')) continue; /* a blank line would end the cue */
        else text[n++] = ch;
    }
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' ')) n--;
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

/* After a success: the gathered tracks go to the job (empty ones are dropped); else freed. */
static void hand_subs(Conv *c, bool ok) {
    AvConvJob *j = c->j;
    for (int k = 0; k < c->nsub; k++) {
        struct ConvSub *s = &c->sub[k];
        if (ok && s->cues && s->p && j->nsubs < AV_CONV_MAX_SUBS) {
            AvConvSub *o = &j->subs[j->nsubs++];
            memset(o, 0, sizeof *o);
            snprintf(o->lang, sizeof o->lang, "%s", s->t->lang);
            snprintf(o->name, sizeof o->name, "%s", s->t->name);
            o->forced = s->t->forced;
            o->is_default = s->t->is_default;
            o->cues = s->cues;
            o->srt = s->p;
            o->len = s->n;
        } else {
            free(s->p);
        }
        s->p = NULL;
    }
}

void av_convert_free(AvConvJob *j) {
    for (int k = 0; k < j->nsubs && k < AV_CONV_MAX_SUBS; k++) {
        free(j->subs[k].srt);
        j->subs[k].srt = NULL;
    }
    j->nsubs = 0;
}

/* ---- the passes ---- */
static bool restart_decoder(Conv *c) {
    if (c->dec) mpeg2_free(c->dec);
    c->dec = mpeg2_new();
    if (!c->dec) return fail(c, "Out of memory", NULL);
    if (c->vt->priv_len) mpeg2_decode(c->dec, c->vt->priv, c->vt->priv_len, MPEG2_NOPTS);
    return true;
}

static int analyze_window(Conv *c, DeintStats *ds, Mpeg2Frame *first, bool *have_first, bool opening) {
    MkvFrame fr;
    int got = 0;
    ds->hist = ds->mhist = 0; /* a jump in time: forget the previous window's history */
    ds->nhist = 0;
    ds->have_thumb = false;
    for (int r; (r = next_frame(c, &fr)) > 0;) {
        if (stopped(c)) return -1;
        if (opening && c->at && fr.track == c->at->number && c->a0 == MKV_NOPTS && fr.pts != MKV_NOPTS) c->a0 = fr.pts;
        if (fr.track != c->vt->number) continue;
        if (opening && c->v0 == MKV_NOPTS && fr.pts != MKV_NOPTS) c->v0 = fr.pts;
        if (got >= ANALYZE_FRAMES) {
            if (opening && c->at && c->a0 == MKV_NOPTS) continue; /* still looking for the first sound */
            break;
        }
        mpeg2_decode(c->dec, fr.data, fr.size, fr.pts);
        Mpeg2Frame pf;
        while (mpeg2_frame(c->dec, &pf)) {
            if (!*have_first) *first = pf, *have_first = true;
            DeintIn in = {pf.y, pf.u, pf.v, pf.stride_y, pf.stride_c, pf.width, pf.height, pf.progressive_frame,
                          pf.top_field_first, pf.repeat_first_field, pf.pts};
            deint_analyze(ds, &in, pf.width, pf.height);
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
            if (c->at && fr.track == c->at->number && c->a0 == MKV_NOPTS && fr.pts != MKV_NOPTS) c->a0 = fr.pts;
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
    if (!have_first) return fail(c, "No picture could be decoded", NULL);
    static const int at_percent[] = {25, 50, 75};
    for (int w = 0; w < 3 && c->duration > 120 * NS; w++) { /* (clips under 2 min: the opening is all) */
        if (!mkv_seek(c->mkv, c->duration * at_percent[w] / 100, c->vt->number) || !restart_decoder(c)) break;
        if (analyze_window(c, ds, first, &have_first, false) < 0) return fail(c, "Stopped", NULL);
    }
    return true;
}

static bool setup_tracks(Conv *c, const Mpeg2Frame *first, const DeintStats *ds) {
    Mp4Track vt = {0}, at = {0};
    uint8_t avcc[512];
    if (c->vcode) {
        int k = deint_kind(ds);
        c->dmode = deint_mode_for(k);
        deint_rate(c->dmode, first->rate_num, first->rate_den, &c->out_num, &c->out_den);
        c->w = first->width, c->h = first->height;
        c->di = deint_new(first->width, first->height, c->dmode);
        H264Params hp = {0};
        hp.width = first->width;
        hp.height = first->height;
        hp.fps_num = c->out_num;
        hp.fps_den = c->out_den;
        sar_of(first->width, first->height, first->dar_num, first->dar_den, &hp.sar_w, &hp.sar_h);
        hp.quality = c->j->quality ? c->j->quality : 23;
        hp.preset = H264_FAST;
        hp.annexb = false;
        hp.colour = first->height >= 720 ? 709 : 601;
        c->enc = h264enc_new(&hp);
        if (!c->di || !c->enc) return fail(c, "Can't set up the converter for this picture size", NULL);
        size_t n = h264enc_avcc(c->enc, avcc, sizeof avcc);
        vt.codec = MP4_H264;
        vt.timescale = (uint32_t)c->out_num;
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
        snprintf(c->j->what, sizeof c->j->what, "DVD video (%s, %.3f fps) → H.264", kind, (double)c->out_num / c->out_den);
    } else {
        if (!c->vt->priv_len) return fail(c, "The video track has no setup data", NULL);
        /* decode order is file order; presentation times are the pts. dts = the sorted
           pts, shifted so that pts - dts never goes negative (B-frames). */
        c->dts = malloc((size_t)c->nvpts * sizeof *c->dts);
        if (!c->dts) return fail(c, "Out of memory", NULL);
        memcpy(c->dts, c->vpts, (size_t)c->nvpts * sizeof *c->dts);
        qsort(c->dts, (size_t)c->nvpts, sizeof *c->dts, cmp_i64);
        for (int i = 0; i < c->nvpts; i++) c->vshift = CMAX(c->vshift, c->dts[i] - c->vpts[i]);
        vt.codec = c->hevc ? MP4_HEVC : MP4_H264;
        vt.timescale = 90000;
        vt.config = c->vt->priv;
        vt.config_len = c->vt->priv_len;
        vt.width = c->vt->width;
        vt.height = c->vt->height;
        if (c->vt->disp_width && c->vt->disp_height && c->vt->disp_unit == 0)
            sar_of(c->vt->width, c->vt->height, c->vt->disp_width, c->vt->disp_height, &vt.sar_num, &vt.sar_den);
        vt.start = ticks(c->vshift, 90000); /* the first picture at movie time 0 */
        snprintf(c->j->what, sizeof c->j->what, c->hevc ? "HEVC copied" : "H.264 copied");
    }
    snprintf(vt.lang, sizeof vt.lang, "und");
    c->vtrack = mp4_add_track(c->mp4, &vt);

    if (c->at) {
        int64_t lead = c->a0 != MKV_NOPTS && c->v0 != MKV_NOPTS ? c->a0 - c->v0 : 0; /* > 0: sound starts later */
        at.codec = MP4_AAC;
        at.timescale = 48000;
        at.channels = 2;
        at.sample_rate = 48000;
        snprintf(at.lang, sizeof at.lang, "%s", strlen(c->at->lang) == 3 ? c->at->lang : "und");
        uint8_t asc[2];
        if (c->acode) {
            AacConfig ac = {48000, 2, (c->j->audio_kbps ? c->j->audio_kbps : 160) * 1000, 0, 0};
            char err[120] = "";
            c->ac3 = ac3_open();
            c->aac = aac_open(&ac, err, sizeof err);
            if (!c->ac3 || !c->aac) return fail(c, "Can't set up the sound encoder: %s", err);
            ac3_set_layout(c->ac3, AC3_LAYOUT_STEREO);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
            ac3_set_crc_check(c->ac3, false); /* fuzzers: mutated frames reach the decoder (crafted files can carry good CRCs anyway) */
#endif
            aac_specific_config(c->aac, asc);
            at.config = asc;
            at.config_len = 2;
            at.avg_bitrate = at.max_bitrate = (uint32_t)ac.bitrate;
            at.start = AAC_PRIMING; /* the encoder's first 1024 samples are priming */
            c->fifo = malloc((AAC_FRAME + AC3_SAMPLES) * 2 * sizeof *c->fifo);
            if (!c->fifo) return fail(c, "Out of memory", NULL);
            c->a_next = MKV_NOPTS;
            c->a_frame = AC3_SAMPLES;
            if (lead < 0) c->skip = ticks(-lead, 48000);
            append(c->j->what, sizeof c->j->what,
                   strcmp(c->at->codec, "A_EAC3") ? " · Dolby Digital → AAC" : " · Dolby Digital Plus → AAC");
        } else {
            at.config = c->at->priv;
            at.config_len = c->at->priv_len;
            at.channels = c->at->channels ? c->at->channels : 2;
            at.sample_rate = c->at->rate > 0 ? (int)c->at->rate : 48000;
            at.timescale = (uint32_t)at.sample_rate;
            if (lead < 0) at.start = ticks(-lead, at.timescale);
            append(c->j->what, sizeof c->j->what, " · AAC copied");
        }
        if (lead > 0) at.delay = ticks(lead, at.timescale);
        c->atrack = mp4_add_track(c->mp4, &at);
    }
    if (c->vtrack < 0 || (c->at && c->atrack < 0)) return fail(c, "The MP4 writer refused a track", NULL);
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
                mpeg2_decode(c->dec, fr.data, fr.size, fr.pts);
                decoded(c);
            } else {
                copy_video(c, &fr);
            }
            if (fr.pts != MKV_NOPTS && c->v0 != MKV_NOPTS && c->duration > 0) {
                int64_t base = c->vcode ? 0 : COPY_SURVEY_PM;
                set_progress(c, (int)CMIN(999, base + CMAX(0, (fr.pts - c->v0) * (1000 - base) / c->duration)));
            }
            if (c->j->limit_ns && fr.pts != MKV_NOPTS && c->v0 != MKV_NOPTS && fr.pts - c->v0 >= c->j->limit_ns) break;
        } else if (c->at && fr.track == c->at->number) {
            if (c->acode) code_audio(c, &fr);
            else put_sample(c, c->atrack, fr.data, fr.size, AAC_FRAME, 0, true);
        } else {
            for (int k = 0; k < c->nsub; k++)
                if (fr.track == c->sub[k].t->number) collect_sub(c, &c->sub[k], &fr);
        }
    }
    if (c->failed) return false;
    if (r < 0) return fail(c, "Couldn't read the original (disk error?)", NULL);
    if (c->vcode) { /* the last pictures still inside the decoder and the deinterlacer */
        mpeg2_flush(c->dec);
        decoded(c);
        deint_flush(c->di);
        DeintOut o;
        while (!c->failed && deint_pull(c->di, &o)) encode_out(c, &o);
        if (c->failed || (c->have_pend && !put_sample(c, c->vtrack, c->pend, c->npend, (uint32_t)c->out_den, 0, c->pend_key)))
            return false;
    }
    if (c->acode && c->a_bad > 50 && c->a_bad > c->a_good) /* not a silent copy that claims success */
        return unsupported(c, "Most of its Dolby sound couldn't be decoded (damaged?)", NULL);
    if (c->acode) { /* the rest of the sound, padded to a whole block, then the encoder's tail */
        static const float zero[AAC_FRAME * 2];
        if (c->nfifo) push_pcm(c, zero, AAC_FRAME - c->nfifo);
        for (int n; !c->failed && (n = aac_flush(c->aac, c->au, (int)sizeof c->au)) > 0;)
            put_sample(c, c->atrack, c->au, (size_t)n, AAC_FRAME, 0, true);
    }
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
   code and a header, and makes at most two): a file claiming days of length gets no huge padding. */
static int64_t index_room(const Conv *c, const Mpeg2Frame *first) {
    double secs = (c->duration > 0 ? (double)c->duration / NS : 4 * 3600.0) * 1.05 + 2; /* unknown: 4 hours */
    double fps = first->rate_den > 0 ? 2.0 * first->rate_num / first->rate_den : 60;
    int64_t v = c->vcode ? CMIN((int64_t)(secs * CMIN(fps, 120)), CMIN(c->vmax, c->j->in.size / 4)) + 16
                         : (int64_t)c->nvpts + 16;
    double rate = !c->at ? 0 : c->acode || c->at->rate <= 0 ? 48000 : c->at->rate;
    int64_t a = c->at ? CMIN((int64_t)(secs * rate / AAC_FRAME), c->amax) + 16 : 0;
    int64_t chunks = a ? 2 * CMIN(v, a) + 2 : 2;
    return 16384 + v * (c->vcode ? 16 : 24) + a * 4 + chunks * 20;
}

bool av_convert(AvConvJob *j) {
    Conv cv = {0}, *c = &cv;
    c->j = j;
    c->v0 = c->a0 = MKV_NOPTS;
    j->err[0] = j->what[0] = '\0';
    j->permille = 0;
    j->out_bytes = 0;
    j->unsupported = false;
    j->nsubs = 0;
    char err[160] = "";
    c->mkv = mkv_open(&j->in, err, sizeof err);
    if (!c->mkv) {
        if (j->in.size <= 0) return fail(c, "The original is empty", NULL);
        return unsupported(c, "Not a readable MKV file: %s", err);
    }
    c->duration = mkv_duration(c->mkv);
    if (c->duration >= MAX_TS) c->duration = 0; /* damage: unknown */
    c->vmax = cap_for(c, VIDEO_PER_S);
    c->amax = cap_for(c, AUDIO_PER_S);
    c->vt = mkv_find_track(c->mkv, MKV_VIDEO);
    /* The sound: the first track we can convert, a default one first - a DTS track ahead of
       the Dolby one shouldn't sink the whole file. */
    const MkvTrack *first_audio = NULL;
    for (int pass = 0; pass < 2 && !c->at; pass++)
        for (int i = 0; i < mkv_track_count(c->mkv) && !c->at; i++) {
            const MkvTrack *t = mkv_track(c->mkv, i);
            if (t->type != MKV_AUDIO) continue;
            if (!first_audio) first_audio = t;
            if ((pass == 0 && !t->is_default) || !audio_ok(t)) continue;
            c->at = t;
        }
    if (!c->vt) unsupported(c, "There's no picture to convert", NULL);
    else if (c->vt->compressed) unsupported(c, "The picture is compressed or encrypted inside the file", NULL);
    else if (!strcmp(c->vt->codec, "V_MPEG2") || !strcmp(c->vt->codec, "V_MPEG1")) c->vcode = true;
    else if (!strcmp(c->vt->codec, "V_MPEGH/ISO/HEVC")) c->hevc = true; /* copied: Safari plays it in MP4, never in MKV */
    else if (strcmp(c->vt->codec, "V_MPEG4/ISO/AVC") != 0)
        unsupported(c, "Its picture is %s, which the converter can't change yet", codec_words(c->vt->codec));
    if (!c->failed && first_audio && !c->at) { /* sound, but none we can convert: say why for the first */
        if (first_audio->compressed) unsupported(c, "The sound is compressed or encrypted inside the file", NULL);
        else if (dolby(first_audio->codec)) unsupported(c, "Can't convert Dolby sound that isn't 48 kHz yet", NULL);
        else unsupported(c, "Its sound is %s, which the converter can't change yet (browsers play it silently)",
                         codec_words(first_audio->codec));
    }
    if (c->at) c->acode = dolby(c->at->codec);
    for (int i = 0; j->want_subs && i < mkv_track_count(c->mkv) && c->nsub < AV_CONV_MAX_SUBS; i++) {
        const MkvTrack *t = mkv_track(c->mkv, i);
        int kind = sub_kind(t);
        if (kind) c->sub[c->nsub++] = (struct ConvSub){.t = t, .ass = kind == 2};
    }

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
    hand_subs(c, !c->failed);

    if (c->mp4) mp4_free(c->mp4);
    mkv_close(c->mkv);
    if (c->dec) mpeg2_free(c->dec);
    if (c->di) deint_free(c->di);
    if (c->enc) h264enc_free(c->enc);
    if (c->ac3) ac3_close(c->ac3);
    if (c->aac) aac_close(c->aac);
    free(c->pend);
    free(c->vpts);
    free(c->dts);
    free(c->fifo);
    if (!c->failed) set_progress(c, 1000);
    return !c->failed;
}
