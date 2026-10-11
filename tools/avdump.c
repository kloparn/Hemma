/* tools/avdump.c — look inside video files with modules/av (dev aid, not part of the app).
 *
 *   avdump info   <file>                                   tracks, duration, cues
 *   avdump frames <file> [--at SEC] [--n K] --png DIR [--half]
 *                                                          decode MPEG-2 frames to PNGs
 *   avdump bench  <file> [--at SEC] [--seconds S]          decode speed (frames/s)
 *   avdump fuzz   <file> [--blocks N] [--rate R] [--container PPM]
 *                                                          damaged real data (build with UBSan)
 *
 * Build (from the project root):
 *   zig cc -O2 -std=gnu11 -Icore tools/avdump.c modules/av/mkv.c modules/av/mpeg2.c \
 *          core/base.c core/plat.c -lws2_32 -liphlpapi -lbcrypt -o avdump.exe */
#include "../core/plat.h"
#include "../modules/av/mkv.h"
#include "../modules/av/mpeg2.h"

#include <stdarg.h>

/* ----------------------------------------------------------------- input */

static bool file_read_at_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

static Mkv *open_mkv(const char *path, FILE **fp) {
    int64_t size = 0;
    bool dir = false;
    *fp = NULL;
    if (!path_stat(path, &size, NULL, &dir) || dir || !(*fp = file_open(path, "rb"))) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    MkvReader r = {*fp, size, file_read_at_cb};
    char err[128];
    Mkv *m = mkv_open(&r, err, sizeof err);
    if (!m) fprintf(stderr, "%s: %s\n", path, err);
    return m;
}

static const char *type_name(int t) {
    switch (t) {
    case MKV_VIDEO: return "video";
    case MKV_AUDIO: return "audio";
    case MKV_SUBTITLE: return "subtitle";
    case MKV_BUTTONS: return "buttons";
    default: return "other";
    }
}

static void fmt_time(int64_t ns, char *out, size_t cap) {
    if (ns == MKV_NOPTS) {
        snprintf(out, cap, "--:--:--.---");
        return;
    }
    int64_t ms = ns / 1000000;
    snprintf(out, cap, "%s%02d:%02d:%02d.%03d", ms < 0 ? "-" : "", (int)(llabs(ms) / 3600000), (int)(llabs(ms) / 60000 % 60),
             (int)(llabs(ms) / 1000 % 60), (int)(llabs(ms) % 1000));
}

static int cmd_info(const char *path) {
    FILE *fp;
    Mkv *m = open_mkv(path, &fp);
    if (!m) return 1;
    char dur[32];
    fmt_time(mkv_duration(m), dur, sizeof dur);
    printf("%s\n  doctype %s, duration %s, %d cues\n  title \"%s\"\n  written by \"%s\"\n", path, mkv_doctype(m), dur,
           mkv_cue_count(m), mkv_title(m), mkv_writing_app(m));
    for (int i = 0; i < mkv_track_count(m); i++) {
        const MkvTrack *t = mkv_track(m, i);
        printf("  #%d %-8s %-18s lang=%-5s%s%s%s", t->number, type_name(t->type), t->codec, t->lang,
               t->is_default ? " default" : "", t->forced ? " forced" : "", t->enabled ? "" : " disabled");
        if (t->type == MKV_VIDEO)
            printf(" %dx%d display %dx%d%s", t->width, t->height, t->disp_width, t->disp_height,
                   t->interlaced == 1 ? " interlaced" : "");
        if (t->type == MKV_AUDIO) printf(" %.0f Hz %d ch", t->rate, t->channels);
        if (t->default_duration) printf(" frame %.3f ms", t->default_duration / 1e6);
        if (t->priv_len) printf(" private %zu B", t->priv_len);
        if (t->strip_len) printf(" strip %zu B", t->strip_len);
        if (t->compressed) printf(" COMPRESSED");
        if (t->name[0]) printf(" \"%s\"", t->name);
        printf("\n");
    }
    /* first frames per track */
    int seen[MKV_MAX_TRACKS] = {0};
    MkvFrame f;
    long total = 0, keys = 0;
    for (int k = 0; k < 4000 && mkv_next(m, &f) == 1; k++) {
        total++;
        keys += f.key;
        for (int i = 0; i < mkv_track_count(m); i++) {
            if (mkv_track(m, i)->number != f.track || seen[i] >= 3) continue;
            char ts[32];
            fmt_time(f.pts, ts, sizeof ts);
            printf("    track %d frame at %s %6zu B%s @%lld\n", f.track, ts, f.size, f.key ? " key" : "", (long long)f.pos);
            seen[i]++;
        }
    }
    printf("  (%ld frames read, %ld keyframes)\n", total, keys);
    mkv_close(m);
    fclose(fp);
    return 0;
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
    buf_put(out, data, n);
    put32(out, crc(0, (const uint8_t *)out->p + at, n + 4));
}

/* RGB rows -> PNG with stored (uncompressed) deflate blocks. */
static bool write_png(const char *path, const uint8_t *rgb, int w, int h) {
    Buf raw = {0}, z = {0}, out = {0};
    for (int y = 0; y < h; y++) {
        buf_putc(&raw, 0); /* filter: none */
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

/* BT.601 limited range YUV 4:2:0 -> RGB; half = every other pixel/line. */
static bool frame_png(const Mpeg2Frame *fr, const char *path, bool half) {
    int step = half ? 2 : 1, w = fr->width / step, h = fr->height / step;
    uint8_t *rgb = xmalloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = x * step, sy = y * step;
            int c = fr->y[(size_t)sy * fr->stride_y + sx] - 16;
            int dd = fr->u[(size_t)(sy / 2) * fr->stride_c + sx / 2] - 128;
            int e = fr->v[(size_t)(sy / 2) * fr->stride_c + sx / 2] - 128;
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = clamp255((298 * c + 409 * e + 128) >> 8);
            p[1] = clamp255((298 * c - 100 * dd - 208 * e + 128) >> 8);
            p[2] = clamp255((298 * c + 516 * dd + 128) >> 8);
        }
    }
    bool ok = write_png(path, rgb, w, h);
    free(rgb);
    return ok;
}

/* --------------------------------------------------------------- decoding */

typedef struct {
    FILE *fp;
    Mkv *m;
    const MkvTrack *video;
    Mpeg2 *dec;
    long frames_in, frames_out, concealed;
    int64_t demux_ms;
} Run;

static bool run_open(Run *r, const char *path) {
    memset(r, 0, sizeof *r);
    if (!(r->m = open_mkv(path, &r->fp))) return false;
    r->video = mkv_find_track(r->m, MKV_VIDEO);
    if (!r->video || (strcmp(r->video->codec, "V_MPEG2") != 0 && strcmp(r->video->codec, "V_MPEG1") != 0)) {
        fprintf(stderr, "no MPEG-1/2 video track (video codec: %s)\n", r->video ? r->video->codec : "none");
        return false;
    }
    r->dec = mpeg2_new();
    if (r->video->priv_len) mpeg2_decode(r->dec, r->video->priv, r->video->priv_len, MPEG2_NOPTS);
    return true;
}

static void run_close(Run *r) {
    mpeg2_free(r->dec);
    if (r->m) mkv_close(r->m);
    if (r->fp) fclose(r->fp);
}

static void run_seek(Run *r, double at) {
    if (at <= 0) return;
    mkv_seek(r->m, (int64_t)(at * 1e9), r->video->number);
    mpeg2_reset(r->dec);
}

/* Feed video frames until the decoder has output; false at the end. */
static bool run_next(Run *r, Mpeg2Frame *out) {
    for (;;) {
        if (mpeg2_frame(r->dec, out)) {
            r->frames_out++;
            r->concealed += out->concealed;
            return true;
        }
        MkvFrame f;
        int64_t t0 = now_ms();
        int rc;
        while ((rc = mkv_next(r->m, &f)) == 1 && f.track != r->video->number) {}
        r->demux_ms += now_ms() - t0;
        if (rc != 1) {
            mpeg2_flush(r->dec);
            if (mpeg2_frame(r->dec, out)) {
                r->frames_out++;
                return true;
            }
            return false;
        }
        r->frames_in++;
        mpeg2_decode(r->dec, f.data, f.size, f.pts);
    }
}

static double arg_num(int argc, char **argv, const char *name, double dflt) {
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return atof(argv[i + 1]);
    return dflt;
}

static const char *arg_str(int argc, char **argv, const char *name) {
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static bool arg_flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], name)) return true;
    return false;
}

static int cmd_frames(const char *path, int argc, char **argv) {
    double at = arg_num(argc, argv, "--at", 0);
    int n = (int)arg_num(argc, argv, "--n", 6);
    const char *dir = arg_str(argc, argv, "--png");
    bool half = arg_flag(argc, argv, "--half");
    Run r;
    if (!run_open(&r, path)) {
        run_close(&r);
        return 1;
    }
    if (dir) dir_make(dir);
    crc_init();
    run_seek(&r, at);
    Mpeg2Frame fr;
    int written = 0;
    while (written < n && run_next(&r, &fr)) {
        if (fr.pts != MPEG2_NOPTS && fr.pts < (int64_t)(at * 1e9) - 1000000) continue; /* before the target */
        char ts[32], file[1024];
        fmt_time(fr.pts, ts, sizeof ts);
        printf("frame %c %s  %dx%d (coded %dx%d, display %dx%d, DAR %d:%d, %d/%d fps)%s%s%s concealed=%d\n", fr.type, ts,
               fr.width, fr.height, fr.coded_width, fr.coded_height, fr.disp_width, fr.disp_height, fr.dar_num,
               fr.dar_den, fr.rate_num, fr.rate_den, fr.progressive_frame ? " progressive" : " interlaced",
               fr.top_field_first ? " tff" : "", fr.repeat_first_field ? " rff" : "", fr.concealed);
        if (dir) {
            snprintf(file, sizeof file, "%s/frame_%03d_%c.png", dir, written, fr.type);
            if (!frame_png(&fr, file, half)) fprintf(stderr, "cannot write %s\n", file);
        }
        written++;
    }
    printf("%ld blocks in, %ld frames out, %ld macroblocks concealed\n", r.frames_in, r.frames_out, r.concealed);
    run_close(&r);
    return 0;
}

static int cmd_bench(const char *path, int argc, char **argv) {
    double at = arg_num(argc, argv, "--at", 0), seconds = arg_num(argc, argv, "--seconds", 60);
    Run r;
    if (!run_open(&r, path)) {
        run_close(&r);
        return 1;
    }
    run_seek(&r, at);
    Mpeg2Frame fr;
    int64_t t0 = now_ms(), first = MPEG2_NOPTS, last = MPEG2_NOPTS;
    uint64_t sum = 0;
    while (run_next(&r, &fr)) {
        if (fr.pts != MPEG2_NOPTS) {
            if (first == MPEG2_NOPTS) first = fr.pts;
            last = fr.pts;
        }
        sum += fr.y[(size_t)(fr.height / 2) * fr.stride_y + fr.width / 2]; /* touch the output */
        if (first != MPEG2_NOPTS && last - first >= (int64_t)(seconds * 1e9)) break;
    }
    int64_t ms = now_ms() - t0;
    double video_s = first == MPEG2_NOPTS ? 0 : (double)(last - first) / 1e9;
    double fps = ms ? r.frames_out * 1000.0 / ms : 0;
    printf("%ld frames (%.1f s of video) in %lld ms: %.0f frames/s, %.1fx real time (demux %lld ms), concealed MBs %ld "
           "[%llu]\n",
           r.frames_out, video_s, (long long)ms, fps, ms ? video_s * 1000.0 / ms : 0, (long long)r.demux_ms, r.concealed,
           (unsigned long long)(sum & 1));
    run_close(&r);
    return 0;
}

/* ------------------------------------------------------------------ fuzz */
/* Real streams, damaged: bit flips in the bytes the demuxer reads and in the
   video blocks the decoder gets, truncated blocks, random seeks. Build with
   -fsanitize=undefined -fsanitize-trap=undefined: surviving is the test. */

static uint32_t fz_state = 0x9E3779B9u;
static uint32_t fz(void) {
    fz_state ^= fz_state << 13;
    fz_state ^= fz_state >> 17;
    fz_state ^= fz_state << 5;
    return fz_state;
}

typedef struct {
    FILE *fp;
    uint32_t per_million; /* flipped bytes per million read */
    long flips;
} FuzzFile;

static bool fuzz_read(void *ctx, int64_t off, void *dst, size_t n) {
    FuzzFile *f = ctx;
    if (!file_pread(f->fp, off, dst, n)) return false;
    uint64_t expect = (uint64_t)n * f->per_million;
    int k = (int)(expect / 1000000) + (fz() % 1000000 < expect % 1000000);
    for (; k > 0; k--, f->flips++) ((uint8_t *)dst)[fz() % n] ^= (uint8_t)(1u << (fz() % 8));
    return true;
}

static int cmd_fuzz(const char *path, int argc, char **argv) {
    long blocks = (long)arg_num(argc, argv, "--blocks", 20000);
    double rate = arg_num(argc, argv, "--rate", 0.2); /* share of video blocks damaged */
    int64_t size = 0;
    if (!path_stat(path, &size, NULL, NULL)) return 1;
    FuzzFile ff = {file_open(path, "rb"), (uint32_t)arg_num(argc, argv, "--container", 2), 0};
    if (!ff.fp) return 1;
    MkvReader rd = {&ff, size, fuzz_read};
    char err[128];
    Mkv *m = mkv_open(&rd, err, sizeof err);
    if (!m) {
        fprintf(stderr, "open: %s\n", err);
        return 1;
    }
    const MkvTrack *v = mkv_find_track(m, MKV_VIDEO);
    if (!v) return 1;
    Mpeg2 *d = mpeg2_new();
    if (v->priv_len) mpeg2_decode(d, v->priv, v->priv_len, MPEG2_NOPTS);
    uint8_t *buf = NULL;
    size_t cap = 0;
    long fed = 0, damaged = 0, out = 0, concealed = 0, seeks = 0, ends = 0;
    int64_t dur = mkv_duration(m);
    MkvFrame f;
    while (fed < blocks) {
        int rc = mkv_next(m, &f);
        if (rc != 1 || fz() % 4000 == 0) { /* end of file (or now and then): jump somewhere */
            ends += rc != 1;
            mkv_seek(m, dur > 0 ? (int64_t)(fz() % (uint32_t)(dur / 1000000)) * 1000000 : 0, 0);
            mpeg2_reset(d);
            seeks++;
            continue;
        }
        if (f.track != v->number) continue;
        size_t n = f.size;
        if (n + 1 > cap) buf = xrealloc(buf, cap = n * 2 + 1);
        memcpy(buf, f.data, n);
        if (n && fz() % 1000 < (uint32_t)(rate * 1000)) {
            damaged++;
            switch (fz() % 4) {
            case 0: /* a few bit flips */
                for (int k = 1 + (int)(fz() % 8); k > 0; k--) buf[fz() % n] ^= (uint8_t)(1u << (fz() % 8));
                break;
            case 1: n = fz() % n; break; /* truncated */
            case 2: { /* a run of garbage */
                size_t at = fz() % n, len = 1 + fz() % 64;
                for (size_t k = at; k < n && k < at + len; k++) buf[k] = (uint8_t)fz();
                break;
            }
            default: /* a start code where none belongs */
                if (n > 8) {
                    size_t at = fz() % (n - 4);
                    buf[at] = buf[at + 1] = 0, buf[at + 2] = 1, buf[at + 3] = (uint8_t)(fz() % 0xB9);
                }
            }
        }
        mpeg2_decode(d, buf, n, f.pts);
        fed++;
        Mpeg2Frame fr;
        while (mpeg2_frame(d, &fr)) {
            out++;
            concealed += fr.concealed;
            volatile uint8_t sink = fr.y[(size_t)(fr.height - 1) * fr.stride_y + fr.width - 1];
            (void)sink;
        }
    }
    printf("fuzz: %ld blocks fed (%ld damaged), %ld container bit flips, %ld seeks (%ld at the end), %ld frames out, "
           "%ld macroblocks concealed - no crash\n",
           fed, damaged, ff.flips, seeks, ends, out, concealed);
    free(buf);
    mpeg2_free(d);
    mkv_close(m);
    fclose(ff.fp);
    return 0;
}

int main(int argc, char **argv) {
    plat_init();
    if (argc < 3) {
        fprintf(stderr, "usage: avdump info|frames|bench|fuzz <file> [options]\n");
        return 2;
    }
    if (!strcmp(argv[1], "info")) return cmd_info(argv[2]);
    if (!strcmp(argv[1], "frames")) return cmd_frames(argv[2], argc - 3, argv + 3);
    if (!strcmp(argv[1], "bench")) return cmd_bench(argv[2], argc - 3, argv + 3);
    if (!strcmp(argv[1], "fuzz")) return cmd_fuzz(argv[2], argc - 3, argv + 3);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
