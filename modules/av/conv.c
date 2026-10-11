/* modules/av/conv.c — MKV -> MP4 that plays everywhere. See conv.h. */
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
#define ANALYZE_FRAMES 240 /* per window (~8 s), four windows decide between inverse telecine and deinterlacing */
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
    int64_t v0, a0;    /* first video / audio pts (ns) */
    int64_t duration;  /* ns */
    Mp4 *mp4;
    int vtrack, atrack;
    /* video re-encode */
    Mpeg2 *dec;
    Deint *di;
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
    int64_t a_next, skip; /* expected pts of the next AC-3 frame (ns); samples to drop at the start */
    uint8_t au[AAC_MAX_AU];
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

/* Sound we can make AAC of: AAC itself, or Dolby AC-3 at 48 kHz (the decoder's output rate -
   DVDs always are; anything else would play at the wrong speed). */
static bool audio_ok(const MkvTrack *t) {
    if (t->compressed) return false;
    if (!strcmp(t->codec, "A_AC3")) return t->rate <= 0 || (t->rate > 47999 && t->rate < 48001);
    return !strncmp(t->codec, "A_AAC", 5);
}

static void set_progress(Conv *c, int pm) {
    if (pm == c->j->permille) return;
    c->j->permille = pm;
    if (c->j->progress) c->j->progress(c->j->progress_ctx, pm);
}

static int64_t ticks(int64_t ns, int64_t timescale) { return ns * timescale / NS; }

static void append(char *s, size_t cap, const char *more) {
    size_t n = strlen(s);
    if (n < cap) snprintf(s + n, cap - n, "%s", more);
}

/* ---- video: re-encode ---- */

static bool put_video(Conv *c, const uint8_t *data, size_t n, int64_t index, bool key) {
    if (c->have_pend) {
        int64_t frames = index - c->pend_index;
        if (frames < 1) frames = 1;
        if (!mp4_sample(c->mp4, c->vtrack, c->pend, c->npend, (uint32_t)(frames * c->out_den), 0, c->pend_key))
            return fail(c, "Couldn't write the new file (disk full?)", NULL);
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
        DeintIn in = {pf.y, pf.u, pf.v, pf.stride_y, pf.stride_c, pf.progressive_frame, pf.top_field_first,
                      pf.repeat_first_field, pf.pts};
        if (deint_push(c->di, &in) < 0) return fail(c, "The deinterlacer refused a frame", NULL);
        DeintOut o;
        while (!c->failed && deint_pull(c->di, &o)) encode_out(c, &o);
    }
    return !c->failed;
}

/* ---- video: copy (H.264 in MKV) ---- */

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
    if (!mp4_sample(c->mp4, c->vtrack, fr->data, fr->size, dur, cts, fr->key))
        return fail(c, "Couldn't write the new file (disk full?)", NULL);
    return true;
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
        if (n > 0 && !mp4_sample(c->mp4, c->atrack, c->au, (size_t)n, AAC_FRAME, 0, true))
            return fail(c, "Couldn't write the new file (disk full?)", NULL);
        c->nfifo -= AAC_FRAME;
        memmove(c->fifo, c->fifo + AAC_FRAME * 2, (size_t)c->nfifo * 2 * sizeof *c->fifo);
    }
    return true;
}

static bool code_audio(Conv *c, const MkvFrame *fr) {
    static const float silence[AC3_SAMPLES * 2];
    float pcm[AC3_SAMPLES * AC3_MAX_CHANNELS];
    const int64_t frame_ns = (int64_t)AC3_SAMPLES * NS / 48000;
    if (fr->pts != MKV_NOPTS && c->a_next != MKV_NOPTS && fr->pts > c->a_next + frame_ns / 2) {
        /* a hole in the sound (damaged disc): fill it with silence so the picture stays in sync */
        int64_t gap = CMIN((fr->pts - c->a_next + frame_ns / 2) / frame_ns, (int64_t)300); /* <= ~10 s: a bigger jump is a broken timestamp, not a hole */
        for (int64_t g = 0; g < gap && !c->failed; g++) push_pcm(c, silence, AC3_SAMPLES);
    }
    const uint8_t *p = fr->data;
    size_t left = fr->size;
    int frames = 0;
    while (left >= AC3_HEADER_BYTES && !c->failed) {
        int size = ac3_frame_size(p, left);
        if (size <= 0 || (size_t)size > left) break;
        int ch = 0;
        int rc = ac3_decode(c->ac3, p, (size_t)size, pcm, &ch, NULL);
        if (ch == 2) push_pcm(c, pcm, AC3_SAMPLES), c->a_good++;
        else if (rc != AC3_OK) push_pcm(c, silence, AC3_SAMPLES), c->a_bad++; /* keep the timing */
        p += size, left -= (size_t)size, frames++;
    }
    if (fr->pts != MKV_NOPTS) c->a_next = fr->pts + frames * frame_ns;
    else if (c->a_next != MKV_NOPTS) c->a_next += frames * frame_ns;
    return !c->failed;
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
    for (int r; (r = mkv_next(c->mkv, &fr)) > 0;) {
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
            DeintIn in = {pf.y, pf.u, pf.v, pf.stride_y, pf.stride_c, pf.progressive_frame, pf.top_field_first,
                          pf.repeat_first_field, pf.pts};
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
        for (int r; (r = mkv_next(c->mkv, &fr)) > 0;) {
            if (stopped(c)) return fail(c, "Stopped", NULL);
            if (c->at && fr.track == c->at->number && c->a0 == MKV_NOPTS && fr.pts != MKV_NOPTS) c->a0 = fr.pts;
            if (fr.track != c->vt->number) continue;
            if (c->v0 == MKV_NOPTS && fr.pts != MKV_NOPTS) c->v0 = fr.pts;
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
        if (!c->vt->priv_len) return fail(c, "The H.264 track has no setup data", NULL);
        /* decode order is file order; presentation times are the pts. dts = the sorted
           pts, shifted so that pts - dts never goes negative (B-frames). */
        c->dts = malloc((size_t)c->nvpts * sizeof *c->dts);
        if (!c->dts) return fail(c, "Out of memory", NULL);
        memcpy(c->dts, c->vpts, (size_t)c->nvpts * sizeof *c->dts);
        qsort(c->dts, (size_t)c->nvpts, sizeof *c->dts, cmp_i64);
        for (int i = 0; i < c->nvpts; i++) c->vshift = CMAX(c->vshift, c->dts[i] - c->vpts[i]);
        vt.codec = MP4_H264;
        vt.timescale = 90000;
        vt.config = c->vt->priv;
        vt.config_len = c->vt->priv_len;
        vt.width = c->vt->width;
        vt.height = c->vt->height;
        if (c->vt->disp_width && c->vt->disp_height && c->vt->disp_unit == 0)
            sar_of(c->vt->width, c->vt->height, c->vt->disp_width, c->vt->disp_height, &vt.sar_num, &vt.sar_den);
        vt.start = ticks(c->vshift, 90000); /* the first picture at movie time 0 */
        snprintf(c->j->what, sizeof c->j->what, "H.264 copied");
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
            aac_specific_config(c->aac, asc);
            at.config = asc;
            at.config_len = 2;
            at.avg_bitrate = at.max_bitrate = (uint32_t)ac.bitrate;
            at.start = AAC_PRIMING; /* the encoder's first 1024 samples are priming */
            c->fifo = malloc((AAC_FRAME + AC3_SAMPLES) * 2 * sizeof *c->fifo);
            if (!c->fifo) return fail(c, "Out of memory", NULL);
            c->a_next = MKV_NOPTS;
            if (lead < 0) c->skip = -lead * 48000 / NS;
            append(c->j->what, sizeof c->j->what, " · Dolby → AAC");
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
    while (!c->failed && (r = mkv_next(c->mkv, &fr)) > 0) {
        if (stopped(c)) return fail(c, "Stopped", NULL);
        if (fr.track == c->vt->number) {
            if (c->vcode) {
                mpeg2_decode(c->dec, fr.data, fr.size, fr.pts);
                decoded(c);
            } else {
                copy_video(c, &fr);
            }
            if (fr.pts != MKV_NOPTS && c->duration > 0)
                set_progress(c, (int)CMIN(999, CMAX(0, (fr.pts - c->v0) * 1000 / c->duration)));
            if (c->j->limit_ns && fr.pts != MKV_NOPTS && c->v0 != MKV_NOPTS && fr.pts - c->v0 >= c->j->limit_ns) break;
        } else if (c->at && fr.track == c->at->number) {
            if (c->acode) code_audio(c, &fr);
            else if (!mp4_sample(c->mp4, c->atrack, fr.data, fr.size, AAC_FRAME, 0, true))
                fail(c, "Couldn't write the new file (disk full?)", NULL);
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
        if (c->have_pend && !mp4_sample(c->mp4, c->vtrack, c->pend, c->npend, (uint32_t)c->out_den, 0, c->pend_key))
            return fail(c, "Couldn't write the new file (disk full?)", NULL);
    }
    if (c->acode && c->a_bad > 50 && c->a_bad > c->a_good) /* not a silent copy that claims success */
        return unsupported(c, "Most of the Dolby sound couldn't be decoded", NULL);
    if (c->acode) { /* the rest of the sound, padded to a whole block, then the encoder's tail */
        static const float zero[AAC_FRAME * 2];
        if (c->nfifo) push_pcm(c, zero, AAC_FRAME - c->nfifo);
        for (int n; !c->failed && (n = aac_flush(c->aac, c->au, (int)sizeof c->au)) > 0;)
            if (!mp4_sample(c->mp4, c->atrack, c->au, (size_t)n, AAC_FRAME, 0, true))
                fail(c, "Couldn't write the new file (disk full?)", NULL);
    }
    return !c->failed;
}

/* Room for the MP4 index, kept right after the header (written last, so players can start at
   once). A bound for how THIS file gets written, not for any MP4 (mp4_moov_bound's 44 bytes a
   sample left ~7 MB of padding in every copy): per video sample stsz 4 + stts 8 + stss 4, and
   ctts 8 more when H.264 with B-frames is copied; per sound sample stsz 4 (one duration: one
   stts entry); per chunk stsc 12 + co64 8, where chunks change with every switch between the
   tracks - at most twice the smaller track's count. Pictures: the copied count is exact (the
   survey saw every frame); re-encoded ones at most twice the source rate (field-rate
   deinterlacing). 5% over the stated length for rounding. Too small after all, the writer
   puts the index at the end instead: still a valid file. */
static int64_t index_room(const Conv *c, const Mpeg2Frame *first) {
    double secs = (c->duration > 0 ? (double)c->duration / NS : 4 * 3600.0) * 1.05 + 2; /* unknown: 4 hours */
    double fps = first->rate_den > 0 ? 2.0 * first->rate_num / first->rate_den : 60;
    int64_t v = c->vcode ? (int64_t)(secs * CMIN(fps, 120)) + 16 : (int64_t)c->nvpts + 16;
    double rate = !c->at ? 0 : c->acode || c->at->rate <= 0 ? 48000 : c->at->rate;
    int64_t a = (int64_t)(secs * rate / AAC_FRAME) + (c->at ? 16 : 0);
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
    char err[160] = "";
    c->mkv = mkv_open(&j->in, err, sizeof err);
    if (!c->mkv) {
        if (j->in.size <= 0) return fail(c, "The original is empty", NULL);
        return unsupported(c, "Not a readable MKV file: %s", err);
    }
    c->duration = mkv_duration(c->mkv);
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
    else if (strcmp(c->vt->codec, "V_MPEG4/ISO/AVC") != 0) unsupported(c, "Can't convert %s video yet", c->vt->codec);
    if (!c->failed && first_audio && !c->at) { /* sound, but none we can convert: say why for the first */
        if (first_audio->compressed) unsupported(c, "The sound is compressed or encrypted inside the file", NULL);
        else if (!strcmp(first_audio->codec, "A_AC3")) unsupported(c, "Can't convert Dolby sound that isn't 48 kHz yet", NULL);
        else unsupported(c, "Can't convert %s sound yet", first_audio->codec);
    }
    if (c->at) c->acode = !strcmp(c->at->codec, "A_AC3");

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
