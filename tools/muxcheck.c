/* tools/muxcheck.c — try modules/av's MP4 writer, FLAC encoder and
 * deinterlacer on real files (dev aid, not part of the app).
 *
 *   muxcheck remux   <h264.mkv> <out.mp4> [--seconds S]
 *                    H.264 + AAC Matroska -> faststart MP4, AAC kept as it is
 *   muxcheck flac    <h264.mkv> <ac3.mkv> <out.mp4> <out.flac> <out.pcm> [--seconds S]
 *                    the same video with the AC-3 sound of <ac3.mkv> as FLAC, plus
 *                    a raw .flac and the s16le PCM it came from (bit-exact checks)
 *   muxcheck analyze <dvd.mkv> [--seconds S]           film / video / progressive?
 *   muxcheck deint   <dvd.mkv> <dir> [--at SEC] [--n K] input, IVTC and yadif PNGs
 *   muxcheck bench   <dvd.mkv> [--seconds S] [--repeat K]  deinterlace fps, FLAC x real time
 *
 * Build (from the project root):
 *   zig cc -O2 -std=gnu11 -Icore tools/muxcheck.c modules/av/mkv.c modules/av/mpeg2.c \
 *          modules/av/mp4.c modules/av/flac.c modules/av/deint.c modules/av/ac3.c \
 *          core/base.c core/plat.c -lws2_32 -liphlpapi -lbcrypt -o muxcheck.exe */
#include "../core/plat.h"
#include "../modules/av/ac3.h"
#include "../modules/av/deint.h"
#include "../modules/av/flac.h"
#include "../modules/av/mkv.h"
#include "../modules/av/mp4.h"
#include "../modules/av/mpeg2.h"

/* ------------------------------------------------------------------ files */

static bool read_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

static Mkv *open_mkv(const char *path, FILE **fp) {
    int64_t size = 0;
    *fp = NULL;
    if (!path_stat(path, &size, NULL, NULL) || !(*fp = file_open(path, "rb"))) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    MkvReader r = {*fp, size, read_cb};
    char err[128];
    Mkv *m = mkv_open(&r, err, sizeof err);
    if (!m) fprintf(stderr, "%s: %s\n", path, err);
    return m;
}

static bool out_write(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, ctx) == n; }
static bool out_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    if (!file_seek(f, off) || fwrite(p, 1, n, f) != n) return false;
    return fseek(f, 0, SEEK_END) == 0;
}

static double now_s(void) { return (double)now_ms() / 1000.0; }

static const char *arg(int argc, char **argv, const char *name, const char *dflt) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return dflt;
}

static int g_sar_num, g_sar_den; /* remux --sar N:D: force a pixel aspect (testing pasp) */

/* -------------------------------------------------------- H.264 timing */

typedef struct {
    int64_t *pts; /* ns, decode order */
    int n, cap;
} PtsList;

static int cmp64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* Matroska keeps pts only; MP4 wants dts + composition offsets. dts = the
   sorted pts shifted back by the deepest reordering, so every cts >= 0. */
static void make_dts(const PtsList *l, int64_t *dts, int64_t *shift) {
    memcpy(dts, l->pts, (size_t)l->n * sizeof *dts);
    qsort(dts, (size_t)l->n, sizeof *dts, cmp64);
    int64_t s = 0;
    for (int i = 0; i < l->n; i++)
        if (dts[i] - l->pts[i] > s) s = dts[i] - l->pts[i];
    for (int i = 0; i < l->n; i++) dts[i] -= s;
    *shift = s;
}

static uint32_t ticks(int64_t ns, uint32_t ts) { return (uint32_t)((ns * (int64_t)ts + 500000000LL) / 1000000000LL); }

/* --------------------------------------------- slices: interleave by time */

typedef struct {
    int track;
    uint8_t *data;
    size_t n;
    uint32_t dur;
    int32_t cts;
    bool key;
    int64_t t; /* ns: dts for video, pts for audio */
} Pend;

typedef struct {
    Pend *p;
    int n, cap;
} PendList;

static void pend_add(PendList *l, int track, const void *data, size_t n, uint32_t dur, int32_t cts, bool key, int64_t t) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->p = xrealloc(l->p, (size_t)l->cap * sizeof *l->p);
    }
    Pend *e = &l->p[l->n++];
    e->track = track, e->data = xmalloc(n ? n : 1), e->n = n, e->dur = dur, e->cts = cts, e->key = key, e->t = t;
    memcpy(e->data, data, n);
}

/* Write everything before `until` (ns): first one track, then the other: half-second chunks. */
static bool flush_slice(Mp4 *m, PendList *l, int64_t until, int ntracks) {
    bool ok = true;
    for (int tr = 0; tr < ntracks; tr++)
        for (int i = 0; i < l->n; i++) {
            Pend *e = &l->p[i];
            if (e->track != tr || e->t >= until || !e->data) continue;
            ok = ok && mp4_sample(m, tr, e->data, e->n, e->dur, e->cts, e->key);
            free(e->data);
            e->data = NULL;
        }
    int w = 0;
    for (int i = 0; i < l->n; i++)
        if (l->p[i].data) l->p[w++] = l->p[i];
    l->n = w;
    return ok;
}

/* ------------------------------------------------------------------ remux */

static int cmd_remux(const char *in, const char *out, double seconds, const char *ac3src, const char *flac_out,
                     const char *pcm_out) {
    FILE *fp;
    Mkv *mk = open_mkv(in, &fp);
    if (!mk) return 1;
    const MkvTrack *vt = mkv_find_track(mk, MKV_VIDEO), *at = mkv_find_track(mk, MKV_AUDIO);
    if (!vt || strcmp(vt->codec, "V_MPEG4/ISO/AVC") || !vt->priv) {
        fprintf(stderr, "need an H.264 video track with CodecPrivate\n");
        return 1;
    }
    bool aac = !ac3src && at && !strncmp(at->codec, "A_AAC", 5) && at->priv;
    int64_t limit = seconds > 0 ? (int64_t)(seconds * 1e9) : INT64_MAX;
    /* pass 1: the video pts in decode order */
    PtsList pl = {0};
    int naudio = 0;
    MkvFrame f;
    while (mkv_next(mk, &f) == 1) {
        if (f.track == vt->number && f.pts != MKV_NOPTS && f.pts < limit) {
            if (pl.n == pl.cap) pl.cap = pl.cap ? pl.cap * 2 : 4096, pl.pts = xrealloc(pl.pts, (size_t)pl.cap * sizeof *pl.pts);
            pl.pts[pl.n++] = f.pts;
        } else if (aac && at && f.track == at->number && f.pts < limit) naudio++;
    }
    int64_t *dts = xmalloc((size_t)pl.n * sizeof *dts), shift = 0;
    make_dts(&pl, dts, &shift);
    int64_t frame_ns = vt->default_duration ? vt->default_duration : 41708333;
    uint32_t vts = 90000;

    /* audio from another file: AC-3 -> PCM -> FLAC */
    Flac *fl = NULL;
    FILE *ff = NULL, *pf = NULL;
    int16_t *pcm16 = NULL;
    int32_t *pcm32 = NULL;
    size_t npcm = 0;
    int rate = 48000, ch = 2;
    if (ac3src) {
        FILE *afp;
        Mkv *am = open_mkv(ac3src, &afp);
        if (!am) return 1;
        const MkvTrack *a3 = mkv_find_track(am, MKV_AUDIO);
        Ac3Dec *dec = ac3_open();
        ac3_set_layout(dec, AC3_LAYOUT_STEREO);
        float buf[AC3_SAMPLES * AC3_MAX_CHANNELS];
        size_t cap = 0;
        while (mkv_next(am, &f) == 1) {
            if (f.track != a3->number || (f.pts != MKV_NOPTS && f.pts >= limit)) continue;
            int c = 0;
            Ac3Info info;
            if (ac3_decode(dec, f.data, f.size, buf, &c, &info) < 0 && !c) continue;
            rate = info.sample_rate, ch = c;
            if (npcm + AC3_SAMPLES * (size_t)c > cap) cap = (npcm + AC3_SAMPLES * (size_t)c) * 2, pcm16 = xrealloc(pcm16, cap * sizeof *pcm16);
            ac3_to_s16(buf, pcm16 + npcm, AC3_SAMPLES * (size_t)c);
            npcm += AC3_SAMPLES * (size_t)c;
        }
        ac3_close(dec);
        mkv_close(am);
        fclose(afp);
        pcm32 = xmalloc((npcm + 1) * sizeof *pcm32);
        for (size_t i = 0; i < npcm; i++) pcm32[i] = pcm16[i];
        FlacConfig fc = {rate, ch, 16, 4096, 0};
        fl = flac_new(&fc);
        if (pcm_out && (pf = file_open(pcm_out, "wb"))) fwrite(pcm16, sizeof *pcm16, npcm, pf), fclose(pf);
        if (flac_out) ff = file_open(flac_out, "wb");
        printf("audio: %zu samples/channel of %d ch at %d Hz from %s\n", npcm / (size_t)ch, ch, rate, ac3src);
    }

    FILE *of = file_open(out, "w+b");
    if (!of) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    Mp4Out o = {of, out_write, out_write_at};
    int64_t samples = pl.n + (aac ? naudio : (int64_t)(npcm / (size_t)ch / 4096 + 2));
    Mp4 *m = mp4_new(&o, 0, mp4_moov_bound(samples, samples / 10 + 64));
    Mp4Track v = {.codec = MP4_H264, .timescale = vts, .config = vt->priv, .config_len = vt->priv_len, .width = vt->width,
                  .height = vt->height, .lang = "und", .start = (int64_t)ticks(shift, vts)};
    if (vt->disp_width && vt->disp_height && vt->disp_unit == 0) {
        v.sar_num = vt->disp_width * vt->height, v.sar_den = vt->disp_height * vt->width;
    }
    if (g_sar_num > 0 && g_sar_den > 0) v.sar_num = g_sar_num, v.sar_den = g_sar_den; /* --sar N:D */
    mp4_add_track(m, &v);
    if (aac) {
        Mp4Track a = {.codec = MP4_AAC, .timescale = (uint32_t)at->rate, .config = at->priv, .config_len = at->priv_len,
                      .channels = at->channels, .sample_rate = (int)at->rate};
        mp4_add_track(m, &a);
    } else if (fl) {
        uint8_t si[FLAC_STREAMINFO_BYTES];
        flac_streaminfo(fl, si);
        Mp4Track a = {.codec = MP4_FLAC, .timescale = (uint32_t)rate, .config = si, .config_len = sizeof si,
                      .channels = ch, .sample_rate = rate, .sample_bits = 16, .lang = "eng"};
        mp4_add_track(m, &a);
    }
    if (!mp4_begin(m)) return 1;
    double t0 = now_s();
    PendList pend = {0};
    int vi = 0, abase = 0;
    int64_t slice = 500000000, next_cut = slice;
    if (fl) { /* FLAC frames of 4096 samples, timed by their first sample */
        size_t per = (size_t)ch * 4096, frames = 0;
        uint8_t *fb = xmalloc(flac_frame_bound(fl));
        uint8_t hdr[FLAC_HEADER_BYTES];
        flac_header(fl, hdr);
        if (ff) fwrite(hdr, 1, sizeof hdr, ff); /* rewritten with the final STREAMINFO below */
        for (size_t off = 0; off < npcm; off += per, frames++) {
            int n = (int)((npcm - off) / (size_t)ch < 4096 ? (npcm - off) / (size_t)ch : 4096);
            long sz = flac_encode_frame(fl, pcm32 + off, n, fb, flac_frame_bound(fl));
            if (sz < 0) return 1;
            if (ff) fwrite(fb, 1, (size_t)sz, ff);
            pend_add(&pend, 1, fb, (size_t)sz, (uint32_t)n, 0, true, (int64_t)(frames * 4096) * 1000000000LL / rate);
        }
        free(fb);
        if (ff) {
            flac_header(fl, hdr);
            fseek(ff, 0, SEEK_SET);
            fwrite(hdr, 1, sizeof hdr, ff);
            fclose(ff);
        }
        uint8_t si[FLAC_STREAMINFO_BYTES];
        flac_streaminfo(fl, si);
        mp4_set_config(m, 1, si, sizeof si);
    }
    mkv_rewind(mk);
    while (mkv_next(mk, &f) == 1) {
        if (f.track == vt->number && f.pts != MKV_NOPTS && f.pts < limit && vi < pl.n) {
            int64_t d = vi + 1 < pl.n ? dts[vi + 1] - dts[vi] : frame_ns;
            uint32_t dur = ticks(dts[vi] + d, vts) - ticks(dts[vi], vts);
            pend_add(&pend, 0, f.data, f.size, dur, (int32_t)ticks(f.pts - dts[vi], vts), f.key, dts[vi]);
            vi++;
        } else if (aac && f.track == at->number && f.pts < limit) {
            pend_add(&pend, 1, f.data, f.size, 1024, 0, true, f.pts);
            abase++;
        }
        int64_t t = f.pts == MKV_NOPTS ? 0 : f.pts;
        if (t > next_cut + 1000000000LL) {
            if (!flush_slice(m, &pend, next_cut, 2)) return 1;
            next_cut += slice;
        }
    }
    while (pend.n) {
        if (!flush_slice(m, &pend, next_cut, 2)) return 1;
        next_cut += slice;
    }
    bool ok = mp4_finish(m);
    printf("%s: %d video + %d audio samples, %.1f MB, faststart %s, %.2f s\n", out, pl.n, aac ? abase : (int)(npcm / (size_t)ch / 4096 + 1),
           (double)mp4_bytes(m) / 1e6, mp4_faststart(m) ? "yes" : "NO", now_s() - t0);
    mp4_free(m);
    fclose(of);
    if (fl) flac_free(fl);
    free(pend.p);
    free(pl.pts);
    free(dts);
    free(pcm16);
    free(pcm32);
    mkv_close(mk);
    fclose(fp);
    return ok ? 0 : 1;
}

/* ----------------------------------------------------------- PNG output */

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    c ^= 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void put32be(Buf *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    buf_put(b, x, 4);
}
static void chunk(Buf *o, const char *type, const uint8_t *d, size_t n) {
    put32be(o, (uint32_t)n);
    size_t at = o->len;
    buf_put(o, type, 4);
    buf_put(o, d, n);
    put32be(o, crc(0, (const uint8_t *)o->p + at, n + 4));
}

static bool write_png(const char *path, const uint8_t *rgb, int w, int h) {
    Buf raw = {0}, z = {0}, out = {0};
    for (int y = 0; y < h; y++) {
        buf_putc(&raw, 0);
        buf_put(&raw, rgb + (size_t)y * (size_t)w * 3, (size_t)w * 3);
    }
    buf_putc(&z, 0x78);
    buf_putc(&z, 0x01);
    for (size_t off = 0; off < raw.len;) {
        size_t n = raw.len - off > 65535 ? 65535 : raw.len - off;
        uint8_t hdr[5] = {(uint8_t)(off + n == raw.len), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8)};
        buf_put(&z, hdr, 5);
        buf_put(&z, raw.p + off, n);
        off += n;
    }
    uint32_t a = 1, b2 = 0;
    for (size_t i = 0; i < raw.len; i++) a = (a + (uint8_t)raw.p[i]) % 65521, b2 = (b2 + a) % 65521;
    put32be(&z, b2 << 16 | a);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    buf_put(&out, sig, 8);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
                        (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    chunk(&out, "IHDR", ihdr, 13);
    chunk(&out, "IDAT", (const uint8_t *)z.p, z.len);
    chunk(&out, "IEND", NULL, 0);
    FILE *f = file_open(path, "wb");
    bool ok = f && fwrite(out.p, 1, out.len, f) == out.len;
    if (f) fclose(f);
    buf_free(&raw), buf_free(&z), buf_free(&out);
    return ok;
}

static uint8_t clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* A crop (x0,y0,w,h) of a 4:2:0 picture as RGB, BT.601 limited range, scaled up `zoom` times. */
static bool yuv_png(const char *path, const uint8_t *Y, const uint8_t *U, const uint8_t *V, int sy, int sc, int x0, int y0,
                    int w, int h, int zoom) {
    int W = w * zoom, H = h * zoom;
    uint8_t *rgb = xmalloc((size_t)W * (size_t)H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int px = x0 + x / zoom, py = y0 + y / zoom;
            int c = Y[(size_t)py * (size_t)sy + (size_t)px] - 16, d = U[(size_t)(py / 2) * (size_t)sc + (size_t)px / 2] - 128,
                e = V[(size_t)(py / 2) * (size_t)sc + (size_t)px / 2] - 128;
            uint8_t *p = rgb + ((size_t)y * (size_t)W + (size_t)x) * 3;
            p[0] = clamp255((298 * c + 409 * e + 128) >> 8);
            p[1] = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
            p[2] = clamp255((298 * c + 516 * d + 128) >> 8);
        }
    bool ok = write_png(path, rgb, W, H);
    free(rgb);
    return ok;
}

/* ------------------------------------------------- DVD video through deint */

typedef struct {
    FILE *fp;
    Mkv *m;
    const MkvTrack *vt;
    Mpeg2 *dec;
    bool key;
} Dvd;

static bool dvd_open(Dvd *d, const char *path, double at) {
    memset(d, 0, sizeof *d);
    if (!(d->m = open_mkv(path, &d->fp))) return false;
    d->vt = mkv_find_track(d->m, MKV_VIDEO);
    if (!d->vt || strcmp(d->vt->codec, "V_MPEG2")) {
        fprintf(stderr, "not an MPEG-2 video file\n");
        return false;
    }
    d->dec = mpeg2_new();
    if (d->vt->priv) mpeg2_decode(d->dec, d->vt->priv, d->vt->priv_len, MPEG2_NOPTS);
    if (at > 0) mkv_seek(d->m, (int64_t)(at * 1e9), 0);
    return true;
}

/* The next decoded frame, or false at the end. */
static bool dvd_frame(Dvd *d, Mpeg2Frame *f) {
    for (;;) {
        if (mpeg2_frame(d->dec, f)) return true;
        MkvFrame fr;
        int r;
        while ((r = mkv_next(d->m, &fr)) == 1 && (fr.track != d->vt->number || (!d->key && !(d->key = fr.key)))) {}
        if (r != 1) {
            mpeg2_flush(d->dec);
            return mpeg2_frame(d->dec, f);
        }
        mpeg2_decode(d->dec, fr.data, fr.size, fr.pts);
    }
}

static void dvd_close(Dvd *d) {
    if (d->dec) mpeg2_free(d->dec);
    if (d->m) mkv_close(d->m);
    if (d->fp) fclose(d->fp);
}

static DeintIn din(const Mpeg2Frame *f) {
    return (DeintIn){f->y, f->u, f->v, f->stride_y, f->stride_c, f->width, f->height, f->progressive_frame, f->top_field_first,
                     f->repeat_first_field, f->pts};
}

static int cmd_analyze(const char *path, double seconds) {
    Dvd d;
    if (!dvd_open(&d, path, 0)) return 1;
    DeintStats st = {0};
    Mpeg2Frame f;
    int64_t limit = seconds > 0 ? (int64_t)(seconds * 30000 / 1001) : INT64_MAX;
    double t0 = now_s();
    while (st.frames < limit && dvd_frame(&d, &f)) {
        DeintIn in = din(&f);
        deint_analyze(&st, &in, f.width, f.height);
    }
    int kind = deint_kind(&st);
    printf("%s\n  %lld frames (%.0f s): progressive-flagged %lld, rff %lld, combed %lld, moving %lld\n"
           "  10-frame windows: 3:2 cadence %lld, interlaced motion %lld, clean motion %lld, still %lld, mixed %lld\n"
           "  -> %s => mode %s  (%.1f s)\n",
           path, (long long)st.frames, st.frames * 1001.0 / 30000, (long long)st.progressive, (long long)st.rff,
           (long long)st.combed, (long long)st.moving, (long long)st.cadence, (long long)st.video, (long long)st.clean,
           (long long)st.still, (long long)st.mixed, deint_kind_name(kind),
           (const char *[]){"yadif", "bob", "ivtc", "pass"}[deint_mode_for(kind)], now_s() - t0);
    dvd_close(&d);
    return 0;
}

static int cmd_deint(const char *path, const char *dir, double at, int n) {
    Dvd d;
    if (!dvd_open(&d, path, at)) return 1;
    crc_init();
    dir_make(dir);
    Deint *iv = NULL, *ya = NULL;
    Mpeg2Frame f;
    int k = 0, wi = 0, wy = 0;
    char file[1024];
    for (; k < n && dvd_frame(&d, &f); k++) {
        if (!iv) iv = deint_new(f.width, f.height, DEINT_IVTC), ya = deint_new(f.width, f.height, DEINT_YADIF);
        int comb = deint_comb(f.y, f.y, f.stride_y, f.width, f.height);
        snprintf(file, sizeof file, "%s/in_%03d_%c_comb%03d.png", dir, k, f.type, comb);
        yuv_png(file, f.y, f.u, f.v, f.stride_y, f.stride_c, 0, 0, f.width, f.height, 1);
        DeintIn in = din(&f);
        deint_push(iv, &in);
        deint_push(ya, &in);
        DeintOut o;
        while (deint_pull(iv, &o)) {
            snprintf(file, sizeof file, "%s/ivtc_%03d%s.png", dir, wi++, o.interpolated ? "_yadif" : "");
            yuv_png(file, o.y, o.u, o.v, o.stride_y, o.stride_c, 0, 0, o.width, o.height, 1);
        }
        while (deint_pull(ya, &o)) {
            snprintf(file, sizeof file, "%s/yadif_%03d.png", dir, wy++);
            yuv_png(file, o.y, o.u, o.v, o.stride_y, o.stride_c, 0, 0, o.width, o.height, 1);
        }
    }
    const DeintCounters *c = deint_counters(iv);
    printf("%d frames -> %d IVTC (match c %lld p %lld n %lld, yadif %lld, dropped %lld), %d yadif frames in %s\n", k, wi,
           (long long)c->match_c, (long long)c->match_p, (long long)c->match_n, (long long)c->interpolated,
           (long long)c->dropped, wy, dir);
    deint_free(iv);
    deint_free(ya);
    dvd_close(&d);
    return 0;
}

static int cmd_bench(const char *path, double seconds, int repeat) {
    Dvd d;
    if (!dvd_open(&d, path, 0)) return 1;
    int limit = (int)(seconds * 30000 / 1001);
    /* decode once into memory, so only the deinterlacer is timed */
    Mpeg2Frame f;
    int n = 0, w = 0, h = 0;
    uint8_t **frames = NULL;
    DeintIn *ins = NULL;
    while (n < limit && dvd_frame(&d, &f)) {
        w = f.width, h = f.height;
        size_t ys = (size_t)w * (size_t)h, cs = ys / 4;
        frames = xrealloc(frames, (size_t)(n + 1) * sizeof *frames);
        ins = xrealloc(ins, (size_t)(n + 1) * sizeof *ins);
        uint8_t *p = frames[n] = xmalloc(ys + 2 * cs);
        for (int y = 0; y < h; y++) memcpy(p + (size_t)y * (size_t)w, f.y + (size_t)y * (size_t)f.stride_y, (size_t)w);
        for (int y = 0; y < h / 2; y++) {
            memcpy(p + ys + (size_t)y * (size_t)(w / 2), f.u + (size_t)y * (size_t)f.stride_c, (size_t)(w / 2));
            memcpy(p + ys + cs + (size_t)y * (size_t)(w / 2), f.v + (size_t)y * (size_t)f.stride_c, (size_t)(w / 2));
        }
        ins[n] = (DeintIn){p, p + ys, p + ys + cs, w, w / 2, w, h, f.progressive_frame, f.top_field_first, f.repeat_first_field,
                           f.pts};
        n++;
    }
    static const char *const names[] = {"yadif", "bob", "ivtc", "pass"};
    for (int mode = DEINT_YADIF; mode <= DEINT_PASS; mode++) {
        double t0 = now_s();
        int out = 0;
        for (int r = 0; r < repeat; r++) { /* repeats: long enough runs for pisim's CPU cap to bite */
            Deint *di = deint_new(w, h, mode);
            DeintOut o;
            for (int i = 0; i < n; i++) {
                deint_push(di, &ins[i]);
                while (deint_pull(di, &o)) out++;
            }
            deint_flush(di);
            while (deint_pull(di, &o)) out++;
            deint_free(di);
        }
        double t = now_s() - t0;
        printf("deint %-5s: %d frames in, %d out, %.0f input frames/s (%.1f x real time)\n", names[mode], n * repeat, out,
               n * repeat / t, n * repeat / t / (30000.0 / 1001));
    }
    double t0 = now_s();
    for (int r = 0; r < repeat; r++) {
        DeintStats st = {0};
        for (int i = 0; i < n; i++) deint_analyze(&st, &ins[i], w, h);
    }
    printf("analyze    : %.0f frames/s\n", n * repeat / (now_s() - t0));
    /* FLAC: the episode's own AC-3, decoded to stereo PCM first */
    mkv_rewind(d.m);
    const MkvTrack *a3 = mkv_find_track(d.m, MKV_AUDIO);
    Ac3Dec *dec = ac3_open();
    ac3_set_layout(dec, AC3_LAYOUT_STEREO);
    float buf[AC3_SAMPLES * AC3_MAX_CHANNELS];
    int32_t *pcm = NULL;
    size_t np = 0, cap = 0;
    int ch = 2, rate = 48000;
    MkvFrame fr;
    while (a3 && mkv_next(d.m, &fr) == 1 && np < (size_t)(seconds * 48000 * 2)) {
        if (fr.track != a3->number) continue;
        int c = 0;
        Ac3Info info;
        if (ac3_decode(dec, fr.data, fr.size, buf, &c, &info) < 0 && !c) continue;
        int16_t s16[AC3_SAMPLES * AC3_MAX_CHANNELS];
        ac3_to_s16(buf, s16, AC3_SAMPLES * (size_t)c);
        ch = c, rate = info.sample_rate;
        if (np + AC3_SAMPLES * (size_t)c > cap) cap = (np + AC3_SAMPLES * (size_t)c) * 2, pcm = xrealloc(pcm, cap * sizeof *pcm);
        for (size_t i = 0; i < AC3_SAMPLES * (size_t)c; i++) pcm[np + i] = s16[i];
        np += AC3_SAMPLES * (size_t)c;
    }
    ac3_close(dec);
    if (np) {
        FlacConfig fc = {rate, ch, 16, 4096, 0};
        Flac *fl = flac_new(&fc);
        uint8_t *fb = xmalloc(flac_frame_bound(fl));
        size_t bytes = 0;
        t0 = now_s();
        for (int r = 0; r < repeat; r++)
            for (size_t off = 0; off < np; off += 4096 * (size_t)ch) {
                int k = (int)((np - off) / (size_t)ch < 4096 ? (np - off) / (size_t)ch : 4096);
                bytes += (size_t)flac_encode_frame(fl, pcm + off, k, fb, flac_frame_bound(fl));
            }
        bytes /= (size_t)repeat;
        double t = (now_s() - t0) / repeat, dur = (double)np / ch / rate;
        printf("flac       : %.0f s of %d-ch %d Hz in %.2f s = %.0f x real time, %.0f kbit/s (%.0f%% of PCM)\n", dur, ch, rate, t,
               dur / t, bytes * 8 / dur / 1000, 100.0 * bytes / (np * 2.0));
        free(fb);
        flac_free(fl);
    }
    free(pcm);
    for (int i = 0; i < n; i++) free(frames[i]);
    free(frames);
    free(ins);
    dvd_close(&d);
    return 0;
}

int main(int argc, char **argv) {
    plat_init();
    const char *sar = arg(argc, argv, "--sar", NULL);
    if (sar) sscanf(sar, "%d:%d", &g_sar_num, &g_sar_den);
    if (argc >= 4 && !strcmp(argv[1], "remux")) return cmd_remux(argv[2], argv[3], atof(arg(argc, argv, "--seconds", "0")), NULL, NULL, NULL);
    if (argc >= 7 && !strcmp(argv[1], "flac"))
        return cmd_remux(argv[2], argv[4], atof(arg(argc, argv, "--seconds", "0")), argv[3], argv[5], argv[6]);
    if (argc >= 3 && !strcmp(argv[1], "analyze")) return cmd_analyze(argv[2], atof(arg(argc, argv, "--seconds", "0")));
    if (argc >= 4 && !strcmp(argv[1], "deint"))
        return cmd_deint(argv[2], argv[3], atof(arg(argc, argv, "--at", "0")), atoi(arg(argc, argv, "--n", "20")));
    if (argc >= 3 && !strcmp(argv[1], "bench")) return cmd_bench(argv[2], atof(arg(argc, argv, "--seconds", "60")), atoi(arg(argc, argv, "--repeat", "1")));
    fprintf(stderr, "usage: muxcheck remux|flac|analyze|deint|bench ... (see the top of tools/muxcheck.c)\n");
    return 2;
}
