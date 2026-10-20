/* tools/ac3dump.c — check the AC-3 decoder (modules/av/ac3.c) on real files
 * without listening to them. Development aid, not part of the app.
 *
 *   zig cc -O2 -Imodules/av tools/ac3dump.c modules/av/ac3.c -o build/ac3dump.exe
 *
 *   ac3dump info  FILE                            AC-3 streams, first header, frames, duration
 *   ac3dump stats FILE [--from S] [--to S] [--track N] [--native]
 *   ac3dump wav   FILE OUT.wav [--from S] [--to S] [--track N]     (stereo Lo/Ro, 16 bit)
 *   ac3dump bench FILE [--seconds S] [--track N] [--native]
 *   ac3dump raw   FILE OUT.ac3 [--from S] [--to S] [--track N]     (the track as an elementary stream)
 *   ac3dump f32   FILE OUT.f32 [--from S] [--to S] [--track N] [--native]
 *                 (raw 32-bit float PCM: stereo Lo/Ro, or with --native every channel in WAV order
 *                  L R C LFE Ls Rs - what `ffmpeg -f f32le` writes, for comparing with it)
 *
 * FILE is a Matroska file (AC-3 or E-AC-3 frames come from a minimal block walker:
 * Tracks, Clusters, SimpleBlock/BlockGroup and all three lacings) or a raw
 * .ac3/.eac3 stream (sync word + CRC scan).
 *
 * `stats` decodes and reports: errors, per-channel level/peak/clipping, a
 * discontinuity measure at the 256-sample block edges (an IMDCT, window or
 * overlap-add bug shows up there as clicks), spectral balance, energy at the
 * block-rate harmonics (187.5 Hz comb), a gated loudness to compare with the
 * stream's dialnorm, which coding tools the encoder used, and whether every
 * frame's unused bits are zero (a strong sign of a bit-exact parse). */
#define _FILE_OFFSET_BITS 64
#include "ac3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI 3.14159265358979323846

static void die(const char *msg) {
    fprintf(stderr, "ac3dump: %s\n", msg);
    exit(1);
}

static void *grow(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die("out of memory");
    return q;
}

/* ------------------------------------------------------------ the frames */

typedef struct {
    uint8_t *data;
    size_t len, cap;
    struct Frame {
        size_t off;
        int size;
        double t; /* seconds */
    } *f;
    int n, capf;
} Frames;

static void add_frame(Frames *fr, const uint8_t *p, int size, double t) {
    if (fr->len + (size_t)size > fr->cap) {
        fr->cap = (fr->cap + (size_t)size) * 2;
        fr->data = grow(fr->data, fr->cap);
    }
    if (fr->n == fr->capf) {
        fr->capf = fr->capf ? fr->capf * 2 : 4096;
        fr->f = grow(fr->f, sizeof *fr->f * (size_t)fr->capf);
    }
    memcpy(fr->data + fr->len, p, (size_t)size);
    fr->f[fr->n++] = (struct Frame){fr->len, size, t};
    fr->len += (size_t)size;
}

/* ---------------------------------------------------- buffered file reads */

typedef struct {
    FILE *f;
    int64_t size, base;
    size_t len, cap;
    uint8_t *buf;
} Rd;

static const uint8_t *at(Rd *r, int64_t off, size_t n) {
    if (off < 0 || off + (int64_t)n > r->size) return NULL;
    if (off < r->base || off + (int64_t)n > r->base + (int64_t)r->len) {
        size_t want = n > (1u << 20) ? n : (1u << 20);
        if (want > r->cap) {
            r->cap = want;
            r->buf = grow(r->buf, want);
        }
        if (fseeko(r->f, (off_t)off, SEEK_SET) != 0) return NULL;
        r->len = fread(r->buf, 1, want, r->f);
        r->base = off;
        if (r->len < n) return NULL;
    }
    return r->buf + (off - r->base);
}

/* --------------------------------------------------------- Matroska walk */

typedef struct {
    int number;
    char codec[24];
} Track;

typedef struct {
    Rd *r;
    Track tracks[32];
    int ntracks, entry_number;
    char entry_codec[24];
    int want;           /* track to collect (0 = first A_AC3) */
    bool count_only;    /* info: count frames per AC-3 track */
    long counts[32];
    int64_t cluster_tc; /* ms (TimecodeScale assumed 1 ms) */
    double from, to;
    bool done;
    Frames *out;
    long gaps;
    double last_t, last_secs;
} Mkv;

static int vlen(uint8_t b) {
    for (int i = 0; i < 8; i++)
        if (b & (0x80 >> i)) return i + 1;
    return 0;
}

static bool read_id(Rd *r, int64_t *off, uint32_t *id) {
    const uint8_t *p = at(r, *off, 1);
    int n = p ? vlen(p[0]) : 0;
    if (!n || n > 4 || !(p = at(r, *off, (size_t)n))) return false;
    *id = 0;
    for (int i = 0; i < n; i++) *id = *id << 8 | p[i];
    *off += n;
    return true;
}

static bool read_vint(const uint8_t *p, size_t avail, int *used, int64_t *v) { /* -1 = unknown size */
    int n = avail ? vlen(p[0]) : 0;
    if (!n || (size_t)n > avail) return false;
    uint64_t x = p[0] & (0xFFu >> n);
    bool ones = x == (0xFFu >> n);
    for (int i = 1; i < n; i++) {
        x = x << 8 | p[i];
        ones = ones && p[i] == 0xFF;
    }
    *used = n;
    *v = ones ? -1 : (int64_t)x;
    return true;
}

static bool read_size(Rd *r, int64_t *off, int64_t *size) {
    const uint8_t *p = at(r, *off, 1);
    int n = p ? vlen(p[0]) : 0, used;
    if (!n || !(p = at(r, *off, (size_t)n)) || !read_vint(p, (size_t)n, &used, size)) return false;
    *off += used;
    return true;
}

static uint64_t read_uint(Rd *r, int64_t off, int64_t len) {
    const uint8_t *p = len > 0 && len <= 8 ? at(r, off, (size_t)len) : NULL;
    uint64_t v = 0;
    for (int i = 0; p && i < len; i++) v = v << 8 | p[i];
    return v;
}

static bool dolby(const char *codec) { return !strcmp(codec, "A_AC3") || !strcmp(codec, "A_EAC3"); }

static bool is_ac3(const Mkv *m, int number) {
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i].number == number) return dolby(m->tracks[i].codec);
    return false;
}

/* seconds of sound in the frame at p: 0 for E-AC-3 frames the decoder skips (other substreams) */
static double frame_secs(const uint8_t *p, size_t n) {
    Ac3Info in;
    if (!ac3_parse_header(p, n, &in) || !in.sample_rate) return 0.032;
    if (in.eac3 && (in.strmtyp == 1 || in.substreamid)) return 0;
    return (double)in.samples / in.sample_rate;
}

static int track_index(const Mkv *m, int number) {
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i].number == number) return i;
    return 0;
}

static void lace_frames(Mkv *m, const uint8_t *p, size_t n, double t) {
    /* a lace may hold one or several whole AC-3 frames */
    size_t i = 0;
    while (i + 6 <= n) {
        int size = ac3_frame_size(p + i, n - i);
        if (!size || i + (size_t)size > n) break;
        if (t >= m->to) {
            m->done = true;
            return;
        }
        double secs = frame_secs(p + i, (size_t)size);
        if (t >= m->from) {
            if (m->last_t >= 0 && secs > 0 && fabs(t - m->last_t - m->last_secs) > 0.0021) m->gaps++;
            if (secs > 0) m->last_t = t, m->last_secs = secs;
            add_frame(m->out, p + i, size, t);
        }
        i += (size_t)size;
        t += secs;
    }
}

static void block(Mkv *m, int64_t off, int64_t len) {
    const uint8_t *p = len > 4 && len < (64 << 20) ? at(m->r, off, (size_t)len) : NULL;
    int used;
    int64_t track;
    if (!p || !read_vint(p, (size_t)len, &used, &track) || track < 0) return;
    if (!is_ac3(m, (int)track)) return;
    if (m->count_only) {
        m->counts[track_index(m, (int)track)]++;
        return;
    }
    if (!m->want) m->want = (int)track; /* first AC-3 track */
    if (track != m->want) return;
    size_t i = (size_t)used;
    double t = (double)(m->cluster_tc + (int16_t)(p[i] << 8 | p[i + 1])) / 1000.0;
    int flags = p[i + 2], lacing = (flags >> 1) & 3;
    i += 3;
    size_t n = (size_t)len;
    if (!lacing) {
        lace_frames(m, p + i, n - i, t);
        return;
    }
    int count = p[i++] + 1;
    size_t sizes[256];
    size_t total = 0;
    if (lacing == 2) { /* fixed */
        for (int k = 0; k < count; k++) sizes[k] = (n - i) / (size_t)count;
    } else if (lacing == 1) { /* Xiph */
        for (int k = 0; k < count - 1; k++) {
            size_t s = 0;
            while (i < n && p[i] == 255) s += 255, i++;
            if (i >= n) return;
            s += p[i++];
            sizes[k] = s;
            total += s;
        }
        sizes[count - 1] = n - i - total;
    } else { /* EBML */
        int64_t s;
        if (!read_vint(p + i, n - i, &used, &s) || s < 0) return;
        i += (size_t)used;
        sizes[0] = (size_t)s;
        total = (size_t)s;
        for (int k = 1; k < count - 1; k++) {
            int64_t raw;
            if (!read_vint(p + i, n - i, &used, &raw)) return;
            i += (size_t)used;
            s += raw - ((1LL << (7 * used - 1)) - 1);
            sizes[k] = (size_t)s;
            total += (size_t)s;
        }
        sizes[count - 1] = n - i - total;
    }
    for (int k = 0; k < count && i < n; k++) {
        if (sizes[k] > n - i) return;
        lace_frames(m, p + i, sizes[k], t);
        if (m->done) return;
        for (size_t j = 0; j + 6 <= sizes[k];) { /* the lace's length in time */
            int fs = ac3_frame_size(p + i + j, sizes[k] - j);
            if (!fs) break;
            t += frame_secs(p + i + j, sizes[k] - j);
            j += (size_t)fs;
        }
        i += sizes[k];
    }
}

static void walk(Mkv *m, int64_t off, int64_t end) {
    while (off < end && !m->done) {
        uint32_t id;
        int64_t size;
        if (!read_id(m->r, &off, &id) || !read_size(m->r, &off, &size)) return;
        int64_t data = off, stop = size < 0 || data + size > end ? end : data + size;
        switch (id) {
        case 0x18538067: /* Segment */
        case 0x1F43B675: /* Cluster */
            if (id == 0x1F43B675) m->cluster_tc = 0;
            if (size < 0) { /* unknown size: its children follow at this level */
                off = data;
                continue;
            }
            walk(m, data, stop);
            break;
        case 0x1654AE6B: /* Tracks */
        case 0xA0:       /* BlockGroup */
            walk(m, data, stop);
            break;
        case 0xAE: /* TrackEntry */
            m->entry_number = 0;
            m->entry_codec[0] = 0;
            walk(m, data, stop);
            if (m->ntracks < 32) {
                m->tracks[m->ntracks].number = m->entry_number;
                memcpy(m->tracks[m->ntracks].codec, m->entry_codec, sizeof m->entry_codec);
                m->ntracks++;
            }
            break;
        case 0xD7: m->entry_number = (int)read_uint(m->r, data, size); break;
        case 0x86: {
            const uint8_t *p = size > 0 && size < 24 ? at(m->r, data, (size_t)size) : NULL;
            if (p) {
                memcpy(m->entry_codec, p, (size_t)size);
                m->entry_codec[size] = 0;
            }
            break;
        }
        case 0xE7: m->cluster_tc = (int64_t)read_uint(m->r, data, size); break;
        case 0xA3: /* SimpleBlock */
        case 0xA1: /* Block */
            block(m, data, stop - data);
            break;
        default: break;
        }
        off = stop;
    }
}

/* -------------------------------------------------------------- loading */

typedef struct {
    double from, to;
    int track;
    bool native, count_only;
} Opts;

static bool load(const char *path, const Opts *o, Frames *out, Mkv *m) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseeko(f, 0, SEEK_END);
    Rd r = {f, (int64_t)ftello(f), -1, 0, 0, NULL};
    memset(m, 0, sizeof *m);
    m->r = &r;
    m->want = o->track;
    m->count_only = o->count_only;
    m->from = o->from;
    m->to = o->to;
    m->out = out;
    m->last_t = -1;
    const uint8_t *p = at(&r, 0, 4);
    if (p && p[0] == 0x1A && p[1] == 0x45 && p[2] == 0xDF && p[3] == 0xA3) {
        walk(m, 0, r.size);
    } else { /* raw AC-3: scan for frames */
        int64_t off = 0;
        double t = 0;
        while (off < r.size && t < o->to) { /* small windows: at() keeps a 1 MB buffer, so most come from it */
            size_t n = (size_t)(r.size - off < (64 << 10) ? r.size - off : (64 << 10));
            const uint8_t *q = at(&r, off, n);
            if (!q) break;
            long k = ac3_find_frame(q, n, off + (int64_t)n >= r.size);
            if (k < 0) {
                off += (int64_t)n > AC3_MAX_FRAME ? (int64_t)n - AC3_MAX_FRAME : (int64_t)n;
                continue;
            }
            int size = ac3_frame_size(q + k, n - (size_t)k);
            if ((size_t)k + (size_t)size > n) { /* re-read with the frame at the start */
                off += k;
                continue;
            }
            if (t >= o->from) add_frame(out, q + k, size, t);
            t += frame_secs(q + k, (size_t)size);
            off += k + size;
        }
    }
    free(r.buf);
    fclose(f);
    return true;
}

/* ------------------------------------------------------------- analysis */

static void fft(double *re, double *im, int n) { /* in place, n power of 2 */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i];
            re[i] = re[j], re[j] = t;
            t = im[i];
            im[i] = im[j], im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * PI / len, wr = cos(a), wi = sin(a);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int u = i + k, v = i + k + len / 2;
                double tr = re[v] * cr - im[v] * ci, ti = re[v] * ci + im[v] * cr;
                re[v] = re[u] - tr, im[v] = im[u] - ti;
                re[u] += tr, im[u] += ti;
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

#define SPEC 4096

typedef struct {
    int ch;
    double sum2[6], peak[6];
    long clip[6], samples;
    double d2_edge[6], d2_mid[6], d2_edge_short[6];
    long n_edge, n_mid, n_edge_short[6];
    double prev[6][2]; /* last two samples per channel */
    bool have_prev;
    /* spectrum of the mono mix */
    double spec[SPEC / 2 + 1], win[SPEC], buf[SPEC];
    int fill, nspec;
    /* gated loudness: 400 ms block energies */
    double *blocks, blk_acc;
    int nblocks, capblocks, blk_fill;
} Acc;

static void spectrum_block(Acc *a) {
    static double re[SPEC], im[SPEC];
    for (int i = 0; i < SPEC; i++) re[i] = a->buf[i] * a->win[i], im[i] = 0;
    fft(re, im, SPEC);
    for (int i = 0; i <= SPEC / 2; i++) a->spec[i] += re[i] * re[i] + im[i] * im[i];
    a->nspec++;
}

/* pcm: `samples` frames x ch interleaved; mask: the decoder's short_mask per block */
static void analyse(Acc *a, const float *pcm, int samples, const uint8_t *short_mask, int rate) {
    int ch = a->ch;
    for (int n = 0; n < samples; n++) {
        bool edge = n % 256 == 0 && a->have_prev;
        int blk = n / 256;
        double mono = 0;
        for (int c = 0; c < ch; c++) {
            double x = pcm[n * ch + c];
            mono += x;
            a->sum2[c] += x * x;
            if (fabs(x) > a->peak[c]) a->peak[c] = fabs(x);
            if (fabs(x) >= 32767.0 / 32768.0) a->clip[c]++;
            if (a->have_prev || n >= 2) {
                double d2 = fabs(x - 2 * a->prev[c][1] + a->prev[c][0]);
                if (edge) {
                    a->d2_edge[c] += d2;
                    bool sh = (short_mask[blk] >> c & 1) || (blk > 0 && (short_mask[blk - 1] >> c & 1));
                    if (sh) a->d2_edge_short[c] += d2, a->n_edge_short[c]++;
                } else if (n % 256 > 1) {
                    a->d2_mid[c] += d2;
                }
            }
            a->prev[c][0] = a->prev[c][1];
            a->prev[c][1] = x;
        }
        if (edge) a->n_edge++;
        else if (n % 256 > 1) a->n_mid++;
        mono /= ch;
        a->buf[a->fill++] = mono;
        if (a->fill == SPEC) {
            spectrum_block(a);
            a->fill = 0;
        }
        a->blk_acc += mono * mono;
        if (++a->blk_fill == rate * 4 / 10) {
            if (a->nblocks == a->capblocks) {
                a->capblocks = a->capblocks ? a->capblocks * 2 : 1024;
                a->blocks = grow(a->blocks, sizeof(double) * (size_t)a->capblocks);
            }
            a->blocks[a->nblocks++] = a->blk_acc / a->blk_fill;
            a->blk_acc = 0;
            a->blk_fill = 0;
        }
    }
    a->have_prev = true;
    a->samples += samples;
}

static double db(double x) { return x > 1e-20 ? 10 * log10(x) : -200; }

static double gated_loudness(const Acc *a) { /* EBU R128-style gating, unweighted */
    double sum = 0;
    int n = 0;
    for (int i = 0; i < a->nblocks; i++)
        if (db(a->blocks[i]) > -70) sum += a->blocks[i], n++;
    if (!n) return -200;
    double rel = db(sum / n) - 10;
    sum = 0, n = 0;
    for (int i = 0; i < a->nblocks; i++)
        if (db(a->blocks[i]) > rel) sum += a->blocks[i], n++;
    return n ? db(sum / n) : -200;
}

/* ------------------------------------------------------------- commands */

static int decode_all(const Frames *fr, bool native, bool report, FILE *wav, long *wav_samples, FILE *f32) {
    Ac3Dec *d = ac3_open();
    if (!d) die("out of memory");
    ac3_set_layout(d, f32 && native ? AC3_LAYOUT_WAV : native ? AC3_LAYOUT_NATIVE : AC3_LAYOUT_STEREO);
    long skipped = 0, spx = 0, aht = 0;
    static float pcm[AC3_SAMPLES * AC3_MAX_CHANNELS];
    static int16_t s16[AC3_SAMPLES * AC3_MAX_CHANNELS];
    Acc *a = calloc(1, sizeof *a);
    if (!a) die("out of memory");
    for (int i = 0; i < SPEC; i++) a->win[i] = 0.5 - 0.5 * cos(2 * PI * i / SPEC);
    long errors[6] = {0}, tail_zero = 0, short_blocks = 0, cpl = 0, remat = 0, dba = 0, dith = 0, phase = 0, exps[4] = {0},
         baps[16] = {0}, free_min = 1 << 30, free_max = -(1 << 30);
    double free_sum = 0;
    static long err_line[4096], err_block[6];
    Ac3Info info = {0}, first = {0};
    int ok = 0;
    for (int i = 0; i < fr->n; i++) {
        int ch = 0;
        int rc = ac3_decode(d, fr->data + fr->f[i].off, (size_t)fr->f[i].size, pcm, &ch, &info);
        if (rc < 0) errors[-rc]++;
        if (rc == AC3_SKIP) skipped++;
        if (rc == AC3_ERR_BITSTREAM) {
            const Ac3Stats *st = ac3_stats(d);
            if (st->error_line > 0 && st->error_line < 4096) err_line[st->error_line]++;
            if (st->error_block >= 0 && st->error_block < 6) err_block[st->error_block]++;
        }
        if (!ch) continue;
        if (!ok++) first = info;
        const Ac3Stats *st = ac3_stats(d);
        if (rc == AC3_OK) {
            tail_zero += st->tail_zero;
            free_sum += st->bits_free;
            if (st->bits_free < free_min) free_min = st->bits_free;
            if (st->bits_free > free_max) free_max = st->bits_free;
            short_blocks += st->blocks_short, cpl += st->blocks_cpl, remat += st->blocks_remat;
            dba += st->blocks_dba, dith += st->blocks_dither, phase += st->blocks_phase, spx += st->blocks_spx, aht += st->blocks_aht;
            for (int k = 0; k < 4; k++) exps[k] += st->exp_strategy[k];
            for (int k = 0; k < 16; k++) baps[k] += st->bap[k];
        }
        if (report) {
            a->ch = ch;
            analyse(a, pcm, info.samples, st->short_mask, info.sample_rate);
        }
        if (wav) {
            ac3_to_s16(pcm, s16, (size_t)info.samples * (size_t)ch);
            fwrite(s16, sizeof(int16_t), (size_t)info.samples * (size_t)ch, wav);
            *wav_samples += info.samples;
        }
        if (f32) {
            fwrite(pcm, sizeof(float), (size_t)info.samples * (size_t)ch, f32);
            *wav_samples += info.samples;
        }
    }
    if (report && ok) {
        int ch = a->ch;
        long good = ok - errors[4] - errors[5];
        printf("stream     %s %d Hz, %d kbit/s, %s (acmod %d%s), dialnorm -%d dB, bsid %d, bsmod %d%s, %d blocks a frame\n",
               first.eac3 ? "E-AC-3" : "AC-3", first.sample_rate, first.bitrate, ac3_acmod_name(first.acmod, first.lfe),
               first.acmod, first.lfe ? " + LFE" : "", first.dialnorm, first.bsid, first.bsmod,
               first.dsurmod == 2 ? ", Dolby Surround" : "", first.blocks);
        printf("frames     %d decoded, %ld ok, CRC failures %ld, bitstream errors %ld, unsupported %ld, other %ld, "
               "skipped (other substreams) %ld\n",
               ok, good, errors[4], errors[5], errors[3], errors[1] + errors[2], skipped);
        if (errors[5]) {
            printf("errors at  blocks:");
            for (int k = 0; k < 6; k++) printf(" %ld", err_block[k]);
            printf("  ac3.c lines:");
            for (int k = 0; k < 4096; k++)
                if (err_line[k]) printf(" %d(%ld)", k, err_line[k]);
            printf("\n");
        }
        printf("parse      unused bits all zero in %ld/%ld frames (%.3f%%); free bits min %ld avg %.1f max %ld\n",
               tail_zero, good, good ? 100.0 * tail_zero / good : 0, free_min, good ? free_sum / good : 0, free_max);
        printf("tools      short blocks %ld ch-blocks, coupling %ld blocks (phase flags %ld), rematrixing %ld "
               "blocks, delta alloc %ld, dither %ld ch-blocks, spectral extension %ld blocks, AHT %ld ch-blocks\n",
               short_blocks, cpl, phase, remat, dba, dith, spx, aht);
        printf("exponents  reuse %ld, D15 %ld, D25 %ld, D45 %ld\n", exps[0], exps[1], exps[2], exps[3]);
        long total = 0;
        for (int k = 0; k < 16; k++) total += baps[k];
        printf("bap        ");
        for (int k = 0; k < 16; k++) printf("%d:%.1f%% ", k, total ? 100.0 * baps[k] / total : 0);
        printf("\n");
        printf("level      (%s layout)\n", ac3_layout_channels(first.acmod, first.lfe, AC3_LAYOUT_STEREO) == ch && !native ? "Lo/Ro" : "native");
        for (int c = 0; c < ch; c++) {
            const char *name = ac3_channel_name(first.acmod, first.lfe, native ? AC3_LAYOUT_NATIVE : AC3_LAYOUT_STEREO, c);
            double ratio = a->n_edge && a->d2_mid[c] > 0 ? (a->d2_edge[c] / a->n_edge) / (a->d2_mid[c] / a->n_mid) : 0;
            double rs = a->n_edge_short[c] && a->d2_mid[c] > 0
                            ? (a->d2_edge_short[c] / a->n_edge_short[c]) / (a->d2_mid[c] / a->n_mid)
                            : 0;
            printf("  %-4s rms %6.1f dBFS  peak %6.1f dBFS  clipped %ld  edge/mid 2nd-diff %.3f (short-block "
                   "edges %.3f over %ld)\n",
                   name, db(a->sum2[c] / a->samples), db(a->peak[c] * a->peak[c]), a->clip[c], ratio, rs,
                   a->n_edge_short[c]);
        }
        printf("loudness   gated %.1f dBFS (unweighted) vs dialnorm -%d\n", gated_loudness(a), first.dialnorm);
        if (a->nspec) {
            static const double edges[] = {22, 44, 88, 177, 355, 710, 1420, 2840, 5680, 11360, 22720};
            double band[10] = {0}, maxb = 0;
            int rate = first.sample_rate;
            for (int i = 1; i <= SPEC / 2; i++) {
                double f = (double)i * rate / SPEC;
                for (int b = 0; b < 10; b++)
                    if (f >= edges[b] && f < edges[b + 1]) band[b] += a->spec[i];
            }
            for (int b = 0; b < 10; b++)
                if (band[b] > maxb) maxb = band[b];
            printf("spectrum   octaves 31Hz..16kHz (dB re loudest):");
            for (int b = 0; b < 10; b++) printf(" %.0f", db(band[b] / maxb));
            printf("\n");
            /* block-rate comb: harmonics of rate/256 against their neighbourhood */
            int step = SPEC / 256; /* bins per harmonic */
            double worst = 0, sum_ratio = 0;
            int nh = 0, worst_k = 0;
            for (int k = 1; k * step + 8 < SPEC / 2 && (double)k * rate / 256 < 16000; k++) {
                int c = k * step;
                double nb = 0;
                for (int j = 3; j <= 7; j++) nb += a->spec[c - j] + a->spec[c + j];
                nb /= 10;
                double r = nb > 0 ? a->spec[c] / nb : 0;
                sum_ratio += r, nh++;
                if (r > worst) worst = r, worst_k = k;
            }
            printf("comb       block-rate harmonics vs neighbours: mean %.2f, worst %.2f at %.0f Hz\n",
                   nh ? sum_ratio / nh : 0, worst, (double)worst_k * rate / 256);
        }
        if (fr->n) printf("time       %.1f s .. %.1f s\n", fr->f[0].t, fr->f[0].t + (double)a->samples / first.sample_rate);
    }
    free(a->blocks);
    free(a);
    ac3_close(d);
    return ok;
}

static void wav_header(FILE *f, int rate, int ch, long samples) {
    uint32_t data = (uint32_t)(samples * ch * 2);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    uint16_t s = 1;
    memcpy(h + 20, &s, 2);
    s = (uint16_t)ch;
    memcpy(h + 22, &s, 2);
    v = (uint32_t)rate;
    memcpy(h + 24, &v, 4);
    v = (uint32_t)(rate * ch * 2);
    memcpy(h + 28, &v, 4);
    s = (uint16_t)(ch * 2);
    memcpy(h + 32, &s, 2);
    s = 16;
    memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: ac3dump info|stats|wav|f32|raw|bench FILE [OUT] [--from S] [--to S] [--seconds S] "
                        "[--track N] [--native]\n");
        return 2;
    }
    const char *cmd = argv[1], *path = argv[2], *out = NULL;
    Opts o = {0, 1e12, 0, false, false};
    int i = 3;
    if (!strcmp(cmd, "wav") || !strcmp(cmd, "raw") || !strcmp(cmd, "f32")) {
        if (argc < 4) die("wav/raw/f32 need an output file");
        out = argv[3];
        i = 4;
    }
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--from") && i + 1 < argc) o.from = atof(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i + 1 < argc) o.to = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) o.to = o.from + atof(argv[++i]);
        else if (!strcmp(argv[i], "--track") && i + 1 < argc) o.track = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--native")) o.native = true;
        else die("unknown option");
    }
    Frames fr = {0};
    Mkv m;
    if (!strcmp(cmd, "info")) {
        o.count_only = true;
        if (!load(path, &o, &fr, &m)) die("cannot open the file");
        for (int t = 0; t < m.ntracks; t++)
            printf("track %d: %s%s\n", m.tracks[t].number, m.tracks[t].codec,
                   dolby(m.tracks[t].codec) ? "" : " (not AC-3/E-AC-3)");
        Opts one = o;
        one.count_only = false;
        for (int t = 0; t < m.ntracks; t++) {
            if (!dolby(m.tracks[t].codec)) continue;
            Frames f2 = {0};
            Mkv m2;
            one.track = m.tracks[t].number;
            load(path, &one, &f2, &m2);
            Ac3Info in;
            if (f2.n && ac3_parse_header(f2.data, (size_t)f2.f[0].size, &in))
                printf("  %s track %d: %d frames = %.1f s, %d Hz, %d kbit/s, %s, dialnorm -%d, timestamp gaps %ld\n",
                       in.eac3 ? "E-AC-3" : "AC-3", one.track, f2.n, f2.f[f2.n - 1].t - f2.f[0].t, in.sample_rate,
                       in.bitrate, ac3_acmod_name(in.acmod, in.lfe), in.dialnorm, m2.gaps);
            free(f2.data);
            free(f2.f);
        }
        if (!m.ntracks) { /* raw stream: the counting pass above already collected it */
            free(fr.data);
            free(fr.f);
            fr = (Frames){0};
            o.count_only = false;
            load(path, &o, &fr, &m);
            Ac3Info in;
            if (fr.n && ac3_parse_header(fr.data, (size_t)fr.f[0].size, &in))
                printf("raw %s: %d frames = %.1f s, %d Hz, %d kbit/s, %s\n", in.eac3 ? "E-AC-3" : "AC-3", fr.n,
                       fr.f[fr.n - 1].t - fr.f[0].t, in.sample_rate, in.bitrate, ac3_acmod_name(in.acmod, in.lfe));
        }
        return 0;
    }
    if (!load(path, &o, &fr, &m)) die("cannot open the file");
    if (!fr.n) die("no AC-3 frames found");
    if (m.ntracks) printf("track      %d (of %d tracks), timestamp gaps %ld\n", m.want, m.ntracks, m.gaps);
    if (!strcmp(cmd, "stats")) {
        decode_all(&fr, o.native, true, NULL, NULL, NULL);
    } else if (!strcmp(cmd, "raw")) { /* the track as a raw AC-3 elementary stream */
        FILE *f = fopen(out, "wb");
        if (!f) die("cannot create the output file");
        fwrite(fr.data, 1, fr.len, f);
        fclose(f);
        printf("wrote %s: %d frames\n", out, fr.n);
    } else if (!strcmp(cmd, "wav")) {
        FILE *f = fopen(out, "wb");
        if (!f) die("cannot create the wav file");
        uint8_t zero[44] = {0};
        fwrite(zero, 1, 44, f);
        long samples = 0;
        decode_all(&fr, false, false, f, &samples, NULL);
        wav_header(f, 48000, 2, samples);
        fclose(f);
        printf("wrote %s: %.1f s stereo\n", out, samples / 48000.0);
    } else if (!strcmp(cmd, "f32")) {
        FILE *f = fopen(out, "wb");
        if (!f) die("cannot create the output file");
        long samples = 0;
        decode_all(&fr, o.native, false, NULL, &samples, f);
        fclose(f);
        printf("wrote %s: %.1f s, %s\n", out, samples / 48000.0, o.native ? "every channel (WAV order)" : "Lo/Ro");
    } else if (!strcmp(cmd, "bench")) {
        Ac3Dec *d = ac3_open();
        ac3_set_layout(d, o.native ? AC3_LAYOUT_NATIVE : AC3_LAYOUT_STEREO);
        static float pcm[AC3_SAMPLES * AC3_MAX_CHANNELS];
        clock_t t0 = clock();
        long frames = 0;
        double audio = 0;
        do {
            for (int k = 0; k < fr.n; k++) {
                int ch;
                Ac3Info in = {0};
                ac3_decode(d, fr.data + fr.f[k].off, (size_t)fr.f[k].size, pcm, &ch, &in);
                if (ch) audio += (double)in.samples / in.sample_rate;
            }
            frames += fr.n;
        } while (clock() - t0 < 2 * CLOCKS_PER_SEC);
        double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
        printf("bench      %ld frames (%.0f s of audio) in %.2f s: %.0fx real time, %.1f us per frame (%s)\n", frames,
               audio, secs, audio / secs, secs * 1e6 / frames, o.native ? "native layout" : "stereo downmix");
        ac3_close(d);
    } else {
        die("unknown command");
    }
    free(fr.data);
    free(fr.f);
    return 0;
}
