/* modules/av/wasm/avconv.c — av/conv.c for the browser: built to WebAssembly
 * (build.ps1 / Makefile -> av.wasm, served as /static/av.wasm) and run by
 * web/conv.js in a Web Worker, one file per worker. The uploading device makes
 * the play-everywhere copy itself, so only the copy crosses the network and the
 * server never holds the original.
 *
 * Not part of the server build (build scripts compile the .c files directly in
 * modules/<id>, not subfolders). Built with: zig cc -target wasm32-wasi
 * -mexec-model=reactor -msimd128 -O2 -s -Wl,-z,stack-size=8388608 -Imodules/av
 * (av_convert alone keeps ~40 KB on the stack), this file plus the av sources the
 * converter uses (conv mkv mpeg2 mpeg4 hevc hevc_ctu hevc_dsp deint h264enc ac3 dts aac mp4).
 *
 * The page side (web/conv.js) provides four functions:
 *   hemma.read(off, dst, n)       n bytes of the original at off into memory[dst]; 1 = ok
 *   hemma.write(p, n)             append memory[p .. p+n) to the copy; 1 = ok
 *   hemma.write_at(off, p, n)     overwrite earlier bytes of the copy; 1 = ok
 *   hemma.progress(permille)      0..1000
 * and calls _initialize(), then conv_run(size, quality, audio_kbps, h264) (h264: 2.6, AvConvJob.h264 - HEVC made H.264):
 *   1 = done, 0 = failed, 2 = the file holds something we can't convert (send the original);
 *   3 = (2.5) an MP4/MOV that plays everywhere as it is: nothing made, send the original - without a word;
 *   conv_err() says why (UTF-8, NUL-terminated). (2.7) conv_part(size, quality, from_ms, to_ms): one part of an HEVC
 *   picture made H.264 alone (to_ms < 0 = to its end) - Full throttle, its parts made side by side, joined by the server.
 * After a 1, the file's subtitle tracks: conv_subs() of them, each with conv_sub_kind(i) (0 = text,
 *   1 = PGS, 2 = VobSub), conv_sub_lang(i), conv_sub_name(i) (NUL-terminated) and conv_sub_forced(i);
 *   text: conv_sub_text(i) / conv_sub_len(i) (SRT, UTF-8); pictures: conv_sub_data(i) / conv_sub_size(i)
 *   (the .sup file, or VobSub's .sub) and, for VobSub, conv_sub_idx(i) / conv_sub_idx_len(i) (the .idx
 *   text) - the page sends them to the server, which saves them next to the copy. After a 2 there are
 *   none: the original goes to the server, which takes its subtitles out itself (skip_refused_subs).
 * conv_subs_only(size) instead reads only the subtitle tracks (1 = read, 0 = not), for a video that stays
 * behind; the same conv_sub_* tell them. */
#include <stdlib.h>
#include <string.h>

#include "../conv.h"

#define IMPORT(name) __attribute__((import_module("hemma"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

IMPORT("read") int js_read(double off, void *dst, int n);
IMPORT("write") int js_write(const void *p, int n);
IMPORT("write_at") int js_write_at(double off, const void *p, int n);
IMPORT("progress") void js_progress(int permille);

/* ---- input: the page reads a File, which costs a call into JS per read, while
   the demuxer asks for a few bytes at a time - so read whole 1 MB blocks */

#define BLOCK (1 << 20)
#define NBLOCKS 4 /* the survey jumps to 25/50/75% and back: a few blocks keep both ends warm */

typedef struct {
    int64_t at; /* -1 = empty */
    int len;
    unsigned used;
    unsigned char *p;
} Block;

static Block g_blocks[NBLOCKS];
static unsigned g_clock;

static Block *block_for(int64_t base, int64_t size) {
    Block *victim = &g_blocks[0];
    for (int i = 0; i < NBLOCKS; i++) {
        if (g_blocks[i].p && g_blocks[i].at == base) {
            g_blocks[i].used = ++g_clock;
            return &g_blocks[i];
        }
        if (g_blocks[i].used < victim->used) victim = &g_blocks[i];
    }
    if (!victim->p && !(victim->p = malloc(BLOCK))) return NULL;
    int64_t left = size - base;
    victim->len = (int)(left < BLOCK ? left : BLOCK);
    victim->at = -1;
    if (victim->len <= 0 || !js_read((double)base, victim->p, victim->len)) return NULL;
    victim->at = base;
    victim->used = ++g_clock;
    return victim;
}

static bool src_read(void *ctx, int64_t off, void *dst, size_t n) {
    int64_t size = *(const int64_t *)ctx;
    unsigned char *out = dst;
    if (off < 0 || (int64_t)n > size - off) return false;
    if (n >= BLOCK) return js_read((double)off, dst, (int)n) == 1; /* big reads: no point copying twice */
    while (n) {
        int64_t base = off / BLOCK * BLOCK;
        Block *b = block_for(base, size);
        if (!b) return false;
        size_t skip = (size_t)(off - base), take = (size_t)b->len - skip;
        if (take > n) take = n;
        memcpy(out, b->p + skip, take);
        out += take, off += (int64_t)take, n -= take;
    }
    return true;
}

/* ---- output: gathered into 4 MB pieces so the page handles a few dozen, not millions */

#define OUTBUF (4 << 20)

static unsigned char *g_out;
static size_t g_nout;
static int64_t g_flushed; /* bytes handed to the page */

static bool flush_out(void) {
    if (g_nout && !js_write(g_out, (int)g_nout)) return false;
    g_flushed += (int64_t)g_nout;
    g_nout = 0;
    return true;
}

static bool dst_write(void *ctx, const void *p, size_t n) {
    (void)ctx;
    if (g_nout + n > OUTBUF && !flush_out()) return false;
    if (n > OUTBUF) { /* never happens with SD frames, but stay correct */
        if (!js_write(p, (int)n)) return false;
        g_flushed += (int64_t)n;
        return true;
    }
    memcpy(g_out + g_nout, p, n);
    g_nout += n;
    return true;
}

static bool dst_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    (void)ctx;
    if (off >= g_flushed && off + (int64_t)n <= g_flushed + (int64_t)g_nout) {
        memcpy(g_out + (off - g_flushed), p, n);
        return true;
    }
    return flush_out() && js_write_at((double)off, p, (int)n) == 1;
}

/* ---- the job */

static AvConvJob g_job;

static void on_progress(void *ctx, int permille) {
    (void)ctx;
    js_progress(permille);
}

EXPORT("conv_run") int conv_run(double size, int quality, int audio_kbps, int h264) {
    static int64_t total;
    total = (int64_t)size;
    if (!g_out && !(g_out = malloc(OUTBUF))) return 0;
    g_nout = 0;
    g_flushed = 0;
    for (int i = 0; i < NBLOCKS; i++) g_blocks[i].at = -1, g_blocks[i].used = 0;
    av_convert_free(&g_job); /* a previous file's subtitles */
    memset(&g_job, 0, sizeof g_job);
    g_job.in = (MkvReader){&total, total, src_read};
    g_job.out = (Mp4Out){NULL, dst_write, dst_write_at};
    g_job.quality = quality;
    g_job.audio_kbps = audio_kbps;
    g_job.h264 = h264 != 0;
    g_job.want_subs = true;
    g_job.skip_refused_subs = true; /* (a refused file goes as it is: no second read of it here) */
    g_job.progress = on_progress;
    bool ok = av_convert(&g_job) && flush_out();
    if (ok) return 1;
    av_convert_free(&g_job);
    if (!g_job.err[0]) strcpy(g_job.err, "Couldn't hand the copy to the page");
    return g_job.as_is ? 3 : g_job.unsupported ? 2 : 0;
}

/* (2.7) Full throttle on this device: one part of an HEVC video's picture made H.264 (AvConvJob.piece_from/_to, from and
   to in ms after its first picture; to < 0 = its end) - an MP4 of that picture alone, for the server to join. As
   conv_run's answers. */
EXPORT("conv_part") int conv_part(double size, int quality, double from_ms, double to_ms) {
    static int64_t total;
    total = (int64_t)size;
    if (!g_out && !(g_out = malloc(OUTBUF))) return 0;
    g_nout = 0;
    g_flushed = 0;
    for (int i = 0; i < NBLOCKS; i++) g_blocks[i].at = -1, g_blocks[i].used = 0;
    av_convert_free(&g_job);
    memset(&g_job, 0, sizeof g_job);
    g_job.in = (MkvReader){&total, total, src_read};
    g_job.out = (Mp4Out){NULL, dst_write, dst_write_at};
    g_job.quality = quality;
    g_job.h264 = true;
    g_job.piece_from = from_ms > 0 ? (int64_t)from_ms * 1000000 : 0;
    int64_t to = (int64_t)to_ms * 1000000;
    g_job.piece_to = to_ms < 0 ? AV_CONV_TO_END : to > 0 ? to : 1;
    g_job.progress = on_progress;
    bool ok = av_convert(&g_job) && flush_out();
    if (ok) return 1;
    if (!g_job.err[0]) strcpy(g_job.err, "Couldn't hand the copy to the page");
    return g_job.unsupported ? 2 : 0;
}

/* Only the subtitles (1.3: a video the library has already gives the subtitles it lacks): 1 = read (conv_subs()
   may be 0), 0 = not (conv_err() says why). Nothing is written. */
EXPORT("conv_subs_only") int conv_subs_only(double size) {
    static int64_t total;
    total = (int64_t)size;
    for (int i = 0; i < NBLOCKS; i++) g_blocks[i].at = -1, g_blocks[i].used = 0;
    av_convert_free(&g_job);
    memset(&g_job, 0, sizeof g_job);
    g_job.in = (MkvReader){&total, total, src_read};
    g_job.want_subs = true;
    g_job.progress = on_progress;
    if (av_subtitles(&g_job)) return 1;
    av_convert_free(&g_job);
    if (!g_job.err[0]) strcpy(g_job.err, "Couldn't read its subtitles");
    return 0;
}

EXPORT("conv_err") const char *conv_err(void) { return g_job.err; }

static const AvConvSub *sub(int i) { return i >= 0 && i < g_job.nsubs ? &g_job.subs[i] : NULL; }
EXPORT("conv_subs") int conv_subs(void) { return g_job.nsubs; }
EXPORT("conv_sub_text") const char *conv_sub_text(int i) { return sub(i) ? sub(i)->srt : ""; }
EXPORT("conv_sub_len") int conv_sub_len(int i) { return sub(i) ? (int)sub(i)->len : 0; }
EXPORT("conv_sub_lang") const char *conv_sub_lang(int i) { return sub(i) ? sub(i)->lang : ""; }
EXPORT("conv_sub_name") const char *conv_sub_name(int i) { return sub(i) ? sub(i)->name : ""; }
EXPORT("conv_sub_forced") int conv_sub_forced(int i) { return sub(i) && sub(i)->forced; }
EXPORT("conv_sub_kind") int conv_sub_kind(int i) { return sub(i) ? (int)sub(i)->kind : 0; }
EXPORT("conv_sub_data") const uint8_t *conv_sub_data(int i) { return sub(i) ? sub(i)->data : NULL; }
EXPORT("conv_sub_size") int conv_sub_size(int i) { return sub(i) ? (int)sub(i)->size : 0; }
EXPORT("conv_sub_idx") const char *conv_sub_idx(int i) { return sub(i) && sub(i)->idx ? sub(i)->idx : ""; }
EXPORT("conv_sub_idx_len") int conv_sub_idx_len(int i) { return sub(i) ? (int)sub(i)->idx_len : 0; }
