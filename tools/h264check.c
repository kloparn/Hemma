/* tools/h264check.c — try modules/av/h264enc on real or synthetic video (dev aid, not part of the app).
 *
 *   h264check encode <file.mkv> --out DIR [--at SEC] [--frames N] [--qp Q] [--preset fast|medium]
 *                    [--deint weave|blend] [--keyint K] [--png EVERY] [--half]
 *       decode MPEG-2 (modules/av/mkv + mpeg2), encode, write DIR/out.h264 (Annex-B),
 *       DIR/recon.yuv (I420, what any decoder must show), DIR/frames.json (per-frame
 *       offset/size/key), DIR/meta.json and PNGs of the reconstruction; print speed,
 *       bitrate and PSNR against the decoded source.
 *   h264check synth --w W --h H --out DIR [--frames N] [--qp Q] [--preset ...] [--keyint K]
 *       the same on generated pictures (any even size, e.g. not a multiple of 16).
 *   h264check bench <file.mkv> [--at SEC] [--frames N] [--qp Q] [--preset ...]
 *       decode first, then time the encoder alone.
 *
 * Build (from the project root):
 *   zig cc -O2 -std=gnu11 -Icore tools/h264check.c modules/av/h264enc.c modules/av/mkv.c \
 *          modules/av/mpeg2.c core/base.c core/plat.c -lws2_32 -liphlpapi -lbcrypt -o h264check.exe */
#include "../core/plat.h"
#include "../modules/av/h264enc.h"
#include "../modules/av/mkv.h"
#include "../modules/av/mpeg2.h"

#include <math.h>

/* ------------------------------------------------------------- arguments */

static const char *arg_str(int argc, char **argv, const char *name) {
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}
static double arg_num(int argc, char **argv, const char *name, double dflt) {
    const char *s = arg_str(argc, argv, name);
    return s ? atof(s) : dflt;
}
static bool arg_flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], name)) return true;
    return false;
}

/* ------------------------------------------------------------------- PNG */

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
static void put32(Buf *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    buf_put(b, x, 4);
}
static void chunk(Buf *out, const char *type, const uint8_t *data, size_t n) {
    put32(out, (uint32_t)n);
    size_t at = out->len;
    buf_put(out, type, 4);
    if (n) buf_put(out, data, n);
    put32(out, crc(0, (const uint8_t *)out->p + at, n + 4));
}
static bool write_png(const char *path, const uint8_t *rgb, int w, int h) {
    Buf raw = {0}, z = {0}, out = {0};
    for (int y = 0; y < h; y++) {
        buf_putc(&raw, 0);
        buf_put(&raw, rgb + (size_t)y * w * 3, (size_t)w * 3);
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
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.len; i++) {
        a = (a + (uint8_t)raw.p[i]) % 65521;
        b = (b + a) % 65521;
    }
    put32(&z, b << 16 | a);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    buf_put(&out, sig, 8);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                        8, 2, 0, 0, 0};
    chunk(&out, "IHDR", ihdr, 13);
    chunk(&out, "IDAT", (const uint8_t *)z.p, z.len);
    chunk(&out, "IEND", NULL, 0);
    FILE *f = file_open(path, "wb");
    bool ok = f && fwrite(out.p, 1, out.len, f) == out.len;
    if (f) fclose(f);
    buf_free(&raw);
    buf_free(&z);
    buf_free(&out);
    return ok;
}
static uint8_t clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static bool yuv_png(const char *path, const uint8_t *y, const uint8_t *u, const uint8_t *v, int sy, int sc, int w, int h,
                    bool half) {
    int step = half ? 2 : 1, ow = w / step, oh = h / step;
    uint8_t *rgb = xmalloc((size_t)ow * oh * 3);
    for (int j = 0; j < oh; j++)
        for (int i = 0; i < ow; i++) {
            int sx = i * step, syy = j * step;
            int c = y[(size_t)syy * sy + sx] - 16, d = u[(size_t)(syy / 2) * sc + sx / 2] - 128,
                e = v[(size_t)(syy / 2) * sc + sx / 2] - 128;
            uint8_t *p = rgb + ((size_t)j * ow + i) * 3;
            p[0] = clamp255((298 * c + 409 * e + 128) >> 8);
            p[1] = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
            p[2] = clamp255((298 * c + 516 * d + 128) >> 8);
        }
    bool ok = write_png(path, rgb, ow, oh);
    free(rgb);
    return ok;
}

/* ----------------------------------------------------------------- input */

typedef struct {
    int w, h;
    uint8_t *y, *u, *v; /* I420, tight */
} Pic;

static void pic_alloc(Pic *p, int w, int h) {
    p->w = w;
    p->h = h;
    p->y = xmalloc((size_t)w * h * 3 / 2);
    p->u = p->y + (size_t)w * h;
    p->v = p->u + (size_t)w * h / 4;
}

static bool file_read_at_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

typedef struct {
    FILE *fp;
    Mkv *m;
    const MkvTrack *video;
    Mpeg2 *dec;
} Src;

static bool src_open(Src *s, const char *path, double at) {
    memset(s, 0, sizeof *s);
    int64_t size = 0;
    bool dir = false;
    if (!path_stat(path, &size, NULL, &dir) || dir || !(s->fp = file_open(path, "rb"))) {
        fprintf(stderr, "cannot open %s\n", path);
        return false;
    }
    MkvReader r = {s->fp, size, file_read_at_cb};
    char err[128];
    if (!(s->m = mkv_open(&r, err, sizeof err))) {
        fprintf(stderr, "%s: %s\n", path, err);
        return false;
    }
    s->video = mkv_find_track(s->m, MKV_VIDEO);
    if (!s->video || (strcmp(s->video->codec, "V_MPEG2") && strcmp(s->video->codec, "V_MPEG1"))) {
        fprintf(stderr, "no MPEG-1/2 video track\n");
        return false;
    }
    s->dec = mpeg2_new();
    if (s->video->priv_len) mpeg2_decode(s->dec, s->video->priv, s->video->priv_len, MPEG2_NOPTS);
    if (at > 0) {
        mkv_seek(s->m, (int64_t)(at * 1e9), s->video->number);
        mpeg2_reset(s->dec);
    }
    return true;
}

static bool src_next(Src *s, Mpeg2Frame *out) {
    for (;;) {
        if (mpeg2_frame(s->dec, out)) return true;
        MkvFrame f;
        int rc;
        while ((rc = mkv_next(s->m, &f)) == 1 && f.track != s->video->number) {}
        if (rc != 1) {
            mpeg2_flush(s->dec);
            return mpeg2_frame(s->dec, out);
        }
        mpeg2_decode(s->dec, f.data, f.size, f.pts);
    }
}

static void src_close(Src *s) {
    if (s->dec) mpeg2_free(s->dec);
    if (s->m) mkv_close(s->m);
    if (s->fp) fclose(s->fp);
}

/* Copy a decoded frame (picture size) into p; "blend" averages neighbouring
   lines, a crude stand-in for real deinterlacing. */
static void take_frame(Pic *p, const Mpeg2Frame *fr, bool blend) {
    for (int y = 0; y < p->h; y++) {
        const uint8_t *a = fr->y + (size_t)y * fr->stride_y, *b = fr->y + (size_t)(y + 1 < p->h ? y + 1 : y) * fr->stride_y;
        uint8_t *d = p->y + (size_t)y * p->w;
        for (int x = 0; x < p->w; x++) d[x] = blend ? (uint8_t)((a[x] + b[x] + 1) >> 1) : a[x];
    }
    for (int y = 0; y < p->h / 2; y++) {
        memcpy(p->u + (size_t)y * (p->w / 2), fr->u + (size_t)y * fr->stride_c, (size_t)p->w / 2);
        memcpy(p->v + (size_t)y * (p->w / 2), fr->v + (size_t)y * fr->stride_c, (size_t)p->w / 2);
    }
}

/* A moving test pattern: gradients, a bouncing disc, scrolling stripes and grain. */
static void synth_frame(Pic *p, int t) {
    uint32_t seed = 12345u + (uint32_t)t * 7919u;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            int v = 40 + (x * 120) / (p->w ? p->w : 1) + (y * 60) / (p->h ? p->h : 1);
            int cx = (t * 3) % (p->w + 40) - 20, cy = p->h / 2 + (int)(p->h / 4 * sin(t * 0.15));
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) < (p->h / 6 + 4) * (p->h / 6 + 4)) v = 220;
            if (((x + t * 2) / 8) % 2 == 0 && y > p->h * 3 / 4) v -= 50;
            seed = seed * 1103515245u + 12345u;
            v += (int)((seed >> 16) & 7) - 3;
            p->y[(size_t)y * p->w + x] = clamp255(v);
        }
    for (int y = 0; y < p->h / 2; y++)
        for (int x = 0; x < p->w / 2; x++) {
            p->u[(size_t)y * (p->w / 2) + x] = clamp255(128 + (x - p->w / 4) / 2 + t % 20);
            p->v[(size_t)y * (p->w / 2) + x] = clamp255(128 + (y - p->h / 4) / 2 - t % 15);
        }
}

/* ---------------------------------------------------------------- stats */

typedef struct {
    double sse[3];
    long samples[3];
    size_t bytes;
    int frames, idr;
    double encode_s;
} Stats;

static void add_psnr(Stats *st, const Pic *src, const H264Enc *enc) {
    const uint8_t *y, *u, *v;
    int sy, sc;
    h264enc_recon(enc, &y, &u, &v, &sy, &sc);
    const uint8_t *planes[3] = {y, u, v}, *orig[3] = {src->y, src->u, src->v};
    int ws[3] = {src->w, src->w / 2, src->w / 2}, hs[3] = {src->h, src->h / 2, src->h / 2}, ss[3] = {sy, sc, sc};
    for (int c = 0; c < 3; c++)
        for (int j = 0; j < hs[c]; j++)
            for (int i = 0; i < ws[c]; i++) {
                int d = planes[c][(size_t)j * ss[c] + i] - orig[c][(size_t)j * ws[c] + i];
                st->sse[c] += d * d;
                st->samples[c]++;
            }
}

static double psnr(double sse, long n) { return sse > 0 ? 10 * log10(255.0 * 255.0 * n / sse) : 99; }

/* ---------------------------------------------------------------- output */

typedef struct {
    FILE *h264, *yuv;
    Buf index;
    char dir[600];
    int png_every;
    bool half;
} Out;

static bool out_open(Out *o, const char *dir, int png_every, bool half) {
    memset(o, 0, sizeof *o);
    str_copy(o->dir, sizeof o->dir, dir);
    o->png_every = png_every;
    o->half = half;
    dir_make(dir);
    char p[700];
    snprintf(p, sizeof p, "%s/out.h264", dir);
    o->h264 = file_open(p, "wb");
    snprintf(p, sizeof p, "%s/recon.yuv", dir);
    o->yuv = file_open(p, "wb");
    buf_puts(&o->index, "[");
    return o->h264 && o->yuv;
}

static void out_frame(Out *o, const H264Enc *enc, const H264Packet *pk, int w, int h, long offset) {
    fwrite(pk->data, 1, pk->size, o->h264);
    buf_printf(&o->index, "%s[%ld,%zu,%d]", pk->frame ? "," : "", offset, pk->size, pk->idr ? 1 : 0);
    const uint8_t *y, *u, *v;
    int sy, sc;
    h264enc_recon(enc, &y, &u, &v, &sy, &sc);
    for (int j = 0; j < h; j++) fwrite(y + (size_t)j * sy, 1, (size_t)w, o->yuv);
    for (int j = 0; j < h / 2; j++) fwrite(u + (size_t)j * sc, 1, (size_t)w / 2, o->yuv);
    for (int j = 0; j < h / 2; j++) fwrite(v + (size_t)j * sc, 1, (size_t)w / 2, o->yuv);
    if (o->png_every > 0 && pk->frame % o->png_every == 0) {
        char p[700];
        snprintf(p, sizeof p, "%s/recon_%04d.png", o->dir, pk->frame);
        yuv_png(p, y, u, v, sy, sc, w, h, o->half);
    }
}

static void out_close(Out *o, const H264Enc *enc, int w, int h, int frames, int fps_num, int fps_den) {
    buf_puts(&o->index, "]");
    char p[700], codec[16];
    snprintf(p, sizeof p, "%s/frames.json", o->dir);
    file_write_atomic(p, o->index.p, o->index.len);
    h264enc_codec(enc, codec);
    Buf meta = {0};
    buf_printf(&meta, "{\"width\":%d,\"height\":%d,\"frames\":%d,\"codec\":\"%s\",\"fps_num\":%d,\"fps_den\":%d}", w, h,
               frames, codec, fps_num, fps_den);
    snprintf(p, sizeof p, "%s/meta.json", o->dir);
    file_write_atomic(p, meta.p, meta.len);
    buf_free(&meta);
    buf_free(&o->index);
    if (o->h264) fclose(o->h264);
    if (o->yuv) fclose(o->yuv);
}

static void print_stats(const Stats *st, int fps_num, int fps_den) {
    double secs = st->frames * (double)fps_den / fps_num;
    double kbps = secs > 0 ? st->bytes * 8 / secs / 1000 : 0;
    double all = (st->sse[0] + st->sse[1] + st->sse[2]);
    long n = st->samples[0] + st->samples[1] + st->samples[2];
    printf("frames %d (%d IDR), %.0f kbit/s, PSNR Y %.2f U %.2f V %.2f all %.2f dB, encode %.1f fps\n", st->frames, st->idr,
           kbps, psnr(st->sse[0], st->samples[0]), psnr(st->sse[1], st->samples[1]), psnr(st->sse[2], st->samples[2]),
           psnr(all, n), st->encode_s > 0 ? st->frames / st->encode_s : 0);
}

static H264Preset preset_of(const char *s) { return s && !strcmp(s, "medium") ? H264_MEDIUM : H264_FAST; }

static int sar_of(const Mpeg2Frame *fr, int *sw, int *sh) {
    /* pixel aspect = display aspect / storage aspect */
    long a = (long)fr->dar_num * fr->height, b = (long)fr->dar_den * fr->width;
    if (a <= 0 || b <= 0) {
        *sw = *sh = 1;
        return 0;
    }
    long x = a, y = b;
    while (y) {
        long t = x % y;
        x = y;
        y = t;
    }
    *sw = (int)(a / x);
    *sh = (int)(b / x);
    return 1;
}

/* ------------------------------------------------------------- commands */

static int cmd_encode(const char *path, int argc, char **argv, bool bench) {
    Src s;
    if (!src_open(&s, path, arg_num(argc, argv, "--at", 0))) return 1;
    int frames = (int)arg_num(argc, argv, "--frames", 300);
    bool blend = arg_str(argc, argv, "--deint") && !strcmp(arg_str(argc, argv, "--deint"), "blend");
    Mpeg2Frame fr;
    if (!src_next(&s, &fr)) {
        fprintf(stderr, "no frames\n");
        return 1;
    }
    int w = fr.width & ~1, h = fr.height & ~1;
    H264Params hp = {0};
    hp.width = w;
    hp.height = h;
    hp.fps_num = fr.rate_num;
    hp.fps_den = fr.rate_den;
    sar_of(&fr, &hp.sar_w, &hp.sar_h);
    hp.quality = (int)arg_num(argc, argv, "--qp", 23);
    hp.keyint = (int)arg_num(argc, argv, "--keyint", 0);
    hp.preset = preset_of(arg_str(argc, argv, "--preset"));
    hp.annexb = true;
    hp.colour = fr.matrix == 1 ? 709 : 601;
    H264Enc *enc = h264enc_new(&hp);
    if (!enc) {
        fprintf(stderr, "encoder refused %dx%d\n", w, h);
        return 1;
    }
    /* bench: decode everything first so only the encoder is timed */
    Pic *pics = xcalloc((size_t)frames, sizeof *pics);
    int n = 0;
    do {
        pic_alloc(&pics[n], w, h);
        take_frame(&pics[n], &fr, blend);
        n++;
    } while (n < frames && src_next(&s, &fr));
    src_close(&s);
    Out o;
    bool write = !bench;
    if (write && !out_open(&o, arg_str(argc, argv, "--out") ? arg_str(argc, argv, "--out") : ".",
                           (int)arg_num(argc, argv, "--png", 0), arg_flag(argc, argv, "--half"))) {
        fprintf(stderr, "cannot write output\n");
        return 1;
    }
    Stats st = {0};
    long offset = 0;
    for (int i = 0; i < n; i++) {
        H264Picture pic = {pics[i].y, pics[i].u, pics[i].v, w, w / 2, w / 2, i};
        H264Packet pk;
        int64_t t0 = now_ms();
        if (!h264enc_encode(enc, &pic, &pk)) {
            fprintf(stderr, "encode failed\n");
            return 1;
        }
        st.encode_s += (now_ms() - t0) / 1000.0;
        st.frames++;
        st.idr += pk.idr;
        st.bytes += pk.size;
        add_psnr(&st, &pics[i], enc);
        if (write) out_frame(&o, enc, &pk, w, h, offset);
        offset += (long)pk.size;
    }
    if (write) out_close(&o, enc, w, h, n, hp.fps_num, hp.fps_den);
    char codec[16];
    h264enc_codec(enc, codec);
    printf("%dx%d %s sar %d:%d, qp %d, %s preset: ", w, h, codec, hp.sar_w, hp.sar_h, hp.quality,
           hp.preset == H264_MEDIUM ? "medium" : "fast");
    print_stats(&st, hp.fps_num, hp.fps_den);
    for (int i = 0; i < n; i++) free(pics[i].y);
    free(pics);
    h264enc_free(enc);
    return 0;
}

static int cmd_synth(int argc, char **argv) {
    int w = (int)arg_num(argc, argv, "--w", 176) & ~1, h = (int)arg_num(argc, argv, "--h", 144) & ~1;
    int frames = (int)arg_num(argc, argv, "--frames", 30);
    H264Params hp = {0};
    hp.width = w;
    hp.height = h;
    hp.fps_num = 25;
    hp.fps_den = 1;
    hp.quality = (int)arg_num(argc, argv, "--qp", 26);
    hp.keyint = (int)arg_num(argc, argv, "--keyint", 0);
    hp.preset = preset_of(arg_str(argc, argv, "--preset"));
    hp.annexb = true;
    H264Enc *enc = h264enc_new(&hp);
    if (!enc) {
        fprintf(stderr, "encoder refused %dx%d\n", w, h);
        return 1;
    }
    Out o;
    if (!out_open(&o, arg_str(argc, argv, "--out") ? arg_str(argc, argv, "--out") : ".", (int)arg_num(argc, argv, "--png", 0),
                  false)) {
        fprintf(stderr, "cannot write output\n");
        return 1;
    }
    Pic p;
    pic_alloc(&p, w, h);
    Stats st = {0};
    long offset = 0;
    for (int i = 0; i < frames; i++) {
        synth_frame(&p, i);
        H264Picture pic = {p.y, p.u, p.v, w, w / 2, w / 2, i};
        H264Packet pk;
        int64_t t0 = now_ms();
        if (!h264enc_encode(enc, &pic, &pk)) return 1;
        st.encode_s += (now_ms() - t0) / 1000.0;
        st.frames++;
        st.idr += pk.idr;
        st.bytes += pk.size;
        add_psnr(&st, &p, enc);
        out_frame(&o, enc, &pk, w, h, offset);
        offset += (long)pk.size;
    }
    out_close(&o, enc, w, h, frames, 25, 1);
    char codec[16];
    h264enc_codec(enc, codec);
    printf("synthetic %dx%d %s qp %d: ", w, h, codec, hp.quality);
    print_stats(&st, 25, 1);
    free(p.y);
    h264enc_free(enc);
    return 0;
}

int main(int argc, char **argv) {
    plat_init();
    crc_init();
    if (argc >= 2 && !strcmp(argv[1], "synth")) return cmd_synth(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "encode")) return cmd_encode(argv[2], argc, argv, false);
    if (argc >= 3 && !strcmp(argv[1], "bench")) return cmd_encode(argv[2], argc, argv, true);
    fprintf(stderr, "usage: h264check encode|bench <file.mkv> [--out DIR] [--at S] [--frames N] [--qp Q]\n"
                    "                 [--preset fast|medium] [--deint weave|blend] [--keyint K] [--png EVERY] [--half]\n"
                    "       h264check synth --w W --h H [--frames N] [--qp Q] --out DIR\n");
    return 2;
}
