/* modules/media/convert.c — "play everywhere": convert a video once into an MP4
 * that every browser, iPhone, iPad, TV and Chromecast plays.
 *
 * The conversion itself is av/conv.c (MPEG-2 -> H.264, AC-3 -> AAC, H.264, HEVC,
 * AV1 and AAC copied) - the same code the uploading browser runs as WebAssembly. Here:
 * files. The result is "<name>.web.mp4" next to the original, written as
 * "<name>.web.mp4.conv.part" (never an upload's resumable "<name>.web.mp4.part") and renamed
 * when complete - never over a file that is there; the library pairs the two and streams the
 * copy (library.c), so every player - the watch page, TV channels - plays it.
 * The queue below runs one file per worker thread, at background priority, with
 * progress on the library settings page. A file starts only when its original's size fits
 * on its disk next to what running uploads and conversions may still write (room.h), and
 * never while an upload is bringing the same copy. upload.c in turn leaves alone a copy being
 * made here (convert_making), and takes over one only queued (convert_give_way: it leaves the
 * queue - the upload brings it). A crash's leftover ".conv.part" goes at the next scan.
 *
 * (2.6) HEVC MADE H.264 (owner, 2026-10-31: "select a series i want to convert ... a season of a show, whole show, episode
 * or movie", "on hemma device or on logged device"): a job with ConvertJob.h264 decodes HEVC (av/hevc.c) and writes H.264,
 * which plays everywhere; its copy is the video's .web.mp4, replacing Hemma's own HEVC copy there (ConvertJob.replace -
 * an original never). convert_h264_card lists the HEVC videos by series; convert_h264_buttons gives a set's two ways:
 * this queue ("On Hemma's computer") or the browser's converter ("On this device": upload.c's "here" batches).
 *
 * Words (issue #46): a job's err is English (marked N_ here; the av library's own are its data) and
 * is translated where the card shows it; "no room" keeps its numbers, and the words are made then. */
#include "../../core/ui.h"
#include "../av/conv.h"
#include "convert.h"
#include "remove.h"
#include "room.h"
#include "upload.h"
#include "media.h"

#define MAX_WORKERS 32 /* (2.7: Full on a big computer - one video a core) */

/* ===================================================================== what */

static bool container_is(const MediaProbe *p, const char *a, const char *b) {
    return str_eq(p->container, a) || (b && str_eq(p->container, b));
}

ConvertNeed convert_need(const MediaFile *f) {
    if (!f || f->type != MF_VIDEO || f->web || f->role == MF_WEBCOPY) return CONV_NONE;
    const MediaProbe *p = &f->probe;
    /* the demuxers we have: Matroska, and (2.5) MP4/MOV - an MP4 needs a copy only for its sound (DuckTales: H.264 +
       Dolby in MP4s, silent in Chrome and Firefox); one with AAC or MP3 plays as it is */
    bool mkv = container_is(p, "mkv", "webm"), mp4 = container_is(p, "mp4", "mov");
    if (!mkv && !mp4) return CONV_NONE;
    /* (sound copied as it is: AAC, and - 2.5 - MP3, which every browser plays in an MP4 but only Chrome's in an MKV, and
       Opus, which Chrome, Firefox, Edge and Safari 17+ play in an MP4 - a WebM with it plays as it is) */
    bool aac = !p->acodec[0] || str_eq(p->acodec, "aac") || str_eq(p->acodec, "mp3") ||
               (str_eq(p->acodec, "opus") && !container_is(p, "webm", NULL)),
         ac3 = str_eq(p->acodec, "ac3") || str_eq(p->acodec, "eac3") ||
                                                             str_eq(p->acodec, "dts"); /* (sound decoded and made AAC) */
    if (str_eq(p->vcodec, "mpeg2") && (aac || ac3)) return mkv ? CONV_TRANSCODE : CONV_NONE;
    /* MPEG-4 Part 2 - DivX, Xvid (2.5): no browser plays it - decoded and made H.264, from an MKV (an AVI's track
       too) or an MP4 (Solsidan S06-S07). DivX 3 is the probe's "msmpeg4": another codec, never this one. */
    if (str_eq(p->vcodec, "mpeg4") && (aac || ac3)) return CONV_TRANSCODE;
    if (str_eq(p->vcodec, "h264") && ac3) return CONV_TRANSCODE; /* picture copied, sound re-encoded */
    if (str_eq(p->vcodec, "h264") && aac) return mkv ? CONV_REMUX : CONV_NONE;
    /* HEVC stays HEVC and AV1 AV1 (no decoder here): moved into an MP4, which Safari - iPhone, iPad, Mac - plays
       (AV1 only on the newest), never an MKV */
    bool kept = str_eq(p->vcodec, "hevc") || str_eq(p->vcodec, "av1");
    if (kept && ac3) return CONV_TRANSCODE;
    if (kept && aac) return mkv ? CONV_REMUX : CONV_NONE;
    return CONV_NONE;
}

/* "Convert all" leaves out a disc's "play all" titles: every episode again, hours of work for
   nothing (each can still be converted on its own from its page). */
static bool bulk_need(const MediaFile *f) { return f->role != MF_PLAYALL && convert_need(f) != CONV_NONE; }

void convert_web_path(const char *src, char *out, size_t cap) {
    const char *slash = strrchr(src, '/'), *dot = strrchr(src, '.');
    size_t stem = dot && (!slash || dot > slash) ? (size_t)(dot - src) : strlen(src);
    snprintf(out, cap, "%.*s.web.mp4", (int)stem, src);
}

void convert_part_path(const char *dst, char *out, size_t cap) { snprintf(out, cap, "%s.conv.part", dst); }

void convert_piece_path(const char *dst, int k, char *out, size_t cap) { snprintf(out, cap, "%s.p%d.conv.part", dst, k); }

const MediaFile *convert_h264_from(const MediaFile *f) {
    if (!f || f->type != MF_VIDEO || f->role == MF_WEBCOPY || f->role == MF_PLAYALL) return NULL;
    const MediaFile *p = media_playable(f);
    if (!str_eq(p->probe.vcodec, "hevc")) return NULL;
    bool readable = container_is(&f->probe, "mkv", "webm") || container_is(&f->probe, "mp4", "mov");
    return readable && str_eq(f->probe.vcodec, "hevc") ? f : p; /* (an original the converter can't read: from the copy) */
}

int convert_h264_set(const char *what, const MediaFile **out, int cap) {
    const MediaIndex *ix = media_lib();
    char key[16] = "", val[40] = "";
    uint64_t id = 0;
    int season = -1, n = 0;
    const char *se = strstr(what ? what : "", "season=");
    if (se) season = atoi(se + 7);
    if (what && *what) {
        const char *eq = strchr(what, '=');
        size_t kl = eq ? (size_t)(eq - what) : 0;
        if (eq && kl < sizeof key) memcpy(key, what, kl), key[kl] = '\0';
        if (eq) str_copy(val, sizeof val, eq + 1);
        char *amp = strchr(val, '&');
        if (amp) *amp = '\0';
    }
    bool one = str_eq(key, "id") && media_parse_id(val, &id), series = str_eq(key, "series") && media_parse_id(val, &id);
    if (what && *what && !one && !series && !str_eq(key, "films")) return 0;
#define ADD(f) do { const MediaFile *f_ = (f); if (n < cap && convert_h264_need(f_)) out[n++] = f_; } while (0)
    if (one) {
        const MediaFile *f = media_file(id);
        if (f) ADD(f);
        return n;
    }
    for (int k = 0; k < ix->nepisodes && !str_eq(key, "films"); k++) {
        const MediaFile *e = &ix->files[ix->episodes[k]];
        if (!series || (e->series == id && (season < 0 || e->name.season == season))) ADD(e);
    }
    if (!series)
        for (int k = 0; k < ix->nmovies; k++) ADD(&ix->files[ix->movies[k]]);
    if (!series && !str_eq(key, "films"))
        for (int k = 0; k < ix->nothers; k++) ADD(&ix->files[ix->others[k]]);
#undef ADD
    return n;
}

/* look: ask the disk whether a new copy's name is free (a copy made since the last scan) - what's queued or planned does;
   counting on a page goes by the scan (the card and Manage count every video at every render) */
static bool h264_paths(const MediaFile *f, char *src, size_t scap, char *dst, size_t dcap, bool *replace, bool look) {
    const MediaFile *s = convert_h264_from(f), *p = s ? media_playable(f) : NULL;
    if (!s) return false;
    if (!media_full_path(s, src, scap)) return false;
    *replace = true;
    /* its HEVC copy, made here: replaced. Not another version of the same name (2.5: "x.mp4" beside "x.mkv", the one
       that plays in more places standing for both) - that's an original: nothing until Keep only the most compatible
       version has left one of them */
    if (p != f) return media_is_web_copy(p->rel) && media_full_path(p, dst, dcap);
    if (str_ends(src, ".web.mp4")) { /* a copy alone (its original went): replaced where it is */
        str_copy(dst, dcap, src);
        return true;
    }
    *replace = false; /* an HEVC original (an MP4 that played on Apple devices as it was): a new copy beside it */
    convert_web_path(src, dst, dcap);
    return !look || !path_stat(dst, NULL, NULL, NULL);
}

bool convert_h264_paths(const MediaFile *f, char *src, size_t scap, char *dst, size_t dcap, bool *replace) {
    return h264_paths(f, src, scap, dst, dcap, replace, true);
}

bool convert_h264_need(const MediaFile *f) {
    char src[1024], dst[1024];
    bool replace;
    return h264_paths(f, src, sizeof src, dst, sizeof dst, &replace, false);
}

/* ================================================================= pipeline */

typedef struct {
    FILE *src, *dst;
    int64_t end; /* bytes appended to dst */
    ConvertJob *j;
    volatile int halt; /* what av_convert watches: set here, on the converting thread */
} Io;

/* The queue's stop flag is shared between threads (atomic); av_convert reads a plain int.
   Every read of the original passes one on to the other, so a stop is seen within a block. */
static void watch_stop(Io *io) {
    if (io->j->stop && atomic_load(io->j->stop)) io->halt = 1;
}

static bool src_read(void *ctx, int64_t off, void *dst, size_t n) {
    Io *io = ctx;
    watch_stop(io);
    return file_pread(io->src, off, dst, n);
}

static bool dst_write(void *ctx, const void *p, size_t n) {
    Io *io = ctx;
    if (n && fwrite(p, 1, n, io->dst) != n) return false;
    io->end += (int64_t)n;
    atomic_store(&io->j->out_mb, (int)(io->end >> 20));
    return true;
}

static bool dst_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    Io *io = ctx;
    bool ok = file_seek(io->dst, off) && (!n || fwrite(p, 1, n, io->dst) == n);
    return file_seek(io->dst, io->end) && ok;
}

static void on_progress(void *ctx, int permille) {
    Io *io = ctx;
    atomic_store(&io->j->permille, permille);
    watch_stop(io);
}

static bool save_new(const char *path, const void *data, size_t n) {
    if (file_write_atomic(path, data, n)) return true;
    log_warn("convert: couldn't save the subtitles %s", path);
    return false;
}

bool convert_save_sub(const char *video, const AvConvSub *s, ConvertSubs *seen) {
    if (!video || !s) return false;
    /* which of its language and flags it is in the file: the second gets ".2" (counted whether the
       first was saved or not, so a run again names every track as the first run did) */
    MediaSubTags t;
    media_sub_track_tags(s->lang, s->name, s->forced, &t);
    char key[40];
    media_sub_key(&t, key, sizeof key);
    int n = 1;
    for (int i = 0; seen && i < seen->n; i++) n += str_eq(seen->keys[i], key);
    if (seen && seen->n < AV_CONV_MAX_SUBS) str_copy(seen->keys[seen->n++], sizeof seen->keys[0], key);
    char path[1100], pics[1100];
    switch (s->kind) {
    case AV_SUB_TEXT:
        if (!s->srt || !s->len || !media_sub_sidecar(video, s->lang, s->name, s->forced, "srt", n, path, sizeof path))
            return false;
        return !path_stat(path, NULL, NULL, NULL) && save_new(path, s->srt, s->len); /* (one there stays) */
    case AV_SUB_PGS:
        if (!s->data || !s->size || !media_sub_sidecar(video, s->lang, s->name, s->forced, "sup", n, path, sizeof path))
            return false;
        return !path_stat(path, NULL, NULL, NULL) && save_new(path, s->data, s->size);
    case AV_SUB_VOBSUB: /* the pair, or nothing: the .idx last (the library takes a .sub only beside its .idx) */
        if (!s->data || !s->size || !s->idx || !s->idx_len ||
            !media_sub_sidecar(video, s->lang, s->name, s->forced, "idx", n, path, sizeof path))
            return false;
        str_copy(pics, sizeof pics, path);
        memcpy(pics + strlen(pics) - 3, "sub", 3);
        if (path_stat(path, NULL, NULL, NULL) || path_stat(pics, NULL, NULL, NULL)) return false;
        if (!save_new(pics, s->data, s->size)) return false;
        if (save_new(path, s->idx, s->idx_len)) return true;
        file_remove(pics);
        return false;
    }
    return false;
}

int convert_save_subs(const char *video, const AvConvSub *subs, int n) {
    ConvertSubs seen = {0};
    int saved = 0;
    for (int k = 0; subs && k < n; k++) saved += convert_save_sub(video, &subs[k], &seen);
    return saved;
}

/* (2.7) the parts of a copy made in parts, read for the join */
typedef struct {
    FILE *f;
} PieceIo;

static bool piece_read(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(((PieceIo *)ctx)->f, off, dst, n); }

static void remove_pieces(const char *dst, int n) {
    char path[1100];
    for (int k = 0; k < n; k++) convert_piece_path(dst, k, path, sizeof path), file_remove(path);
}

bool convert_file(ConvertJob *j) {
    j->err[0] = j->what[0] = '\0';
    j->subs = 0;
    atomic_store(&j->permille, 0);
    atomic_store(&j->out_mb, 0);
    char part[1100];
    bool piece = j->piece_to > 0; /* (2.7: one part of the picture, kept for the join) */
    if (piece) convert_piece_path(j->dst, j->piece, part, sizeof part);
    else convert_part_path(j->dst, part, sizeof part);
    j->replace &= str_ends(j->dst, ".web.mp4"); /* (what's replaced is a copy - never anything else) */
    if (!piece && !j->replace && path_stat(j->dst, NULL, NULL, NULL)) { /* made since it was queued (by hand?): that one stays */
        str_copy(j->err, sizeof j->err, N_("Its play-everywhere copy is there already"));
        return false;
    }
    int64_t size = 0;
    Io io = {.src = file_open(j->src, "rb"), .j = j};
    if (!io.src || !path_stat(j->src, &size, NULL, NULL)) {
        if (io.src) fclose(io.src);
        str_copy(j->err, sizeof j->err, N_("Can't open the original"));
        return false;
    }
    io.dst = file_open(part, "wb");
    if (!io.dst) {
        fclose(io.src);
        str_copy(j->err, sizeof j->err, N_("Can't create the new file next to the original"));
        return false;
    }
    AvConvJob av = {.in = {&io, size, src_read},
                    .out = {&io, dst_write, dst_write_at},
                    .quality = j->quality,
                    .audio_kbps = j->audio_kbps,
                    .limit_ns = j->limit_ns,
                    .h264 = j->h264,
                    .piece_from = j->piece_from,
                    .piece_to = j->piece_to,
                    .want_subs = !piece,
                    .stop = &io.halt,
                    .progress = on_progress,
                    .progress_ctx = &io};
    /* (2.7) the join: the parts' files, read as they are */
    PieceIo pio[AV_CONV_MAX_PIECES] = {{0}};
    MkvReader readers[AV_CONV_MAX_PIECES];
    bool opened = true;
    int np = CLAMP(j->npieces, 0, AV_CONV_MAX_PIECES);
    for (int k = 0; k < np; k++) {
        char path[1100];
        int64_t psize = 0;
        convert_piece_path(j->dst, k, path, sizeof path);
        pio[k].f = path_stat(path, &psize, NULL, NULL) ? file_open(path, "rb") : NULL;
        opened &= pio[k].f != NULL;
        readers[k] = (MkvReader){&pio[k], psize, piece_read};
    }
    if (np) av.pieces = readers, av.npieces = np;
    bool ok = opened && av_convert(&av);
    for (int k = 0; k < np; k++)
        if (pio[k].f) fclose(pio[k].f);
    /* next to the original (the library gives them to its copy too) - also when the file was refused for
       what it holds: then the converter took out just its subtitles, and the original still has them */
    j->subs = convert_save_subs(j->src, av.subs, av.nsubs);
    av_convert_free(&av);
    str_copy(j->what, sizeof j->what, av.what);
    if (!opened) str_copy(j->err, sizeof j->err, N_("A part of the copy went missing"));
    else if (!ok) str_copy(j->err, sizeof j->err, av.err);
    if (ok && (fflush(io.dst) != 0 || !file_sync(io.dst))) { /* (a stopped or failed one goes: no fsync) */
        str_copy(j->err, sizeof j->err, N_("Couldn't save the new file"));
        ok = false;
    }
    fclose(io.dst);
    fclose(io.src);
    if (np) remove_pieces(j->dst, np); /* (joined, or not: the parts are done with either way) */
    if (piece) {
        if (!ok) file_remove(part);
        return ok;
    }
    if (ok && !j->replace && path_stat(j->dst, NULL, NULL, NULL)) { /* never over a copy that arrived meanwhile */
        str_copy(j->err, sizeof j->err, N_("Another play-everywhere copy arrived meanwhile - that one stays"));
        ok = false;
    }
    if (ok && !file_replace(part, j->dst)) {
        str_copy(j->err, sizeof j->err, N_("Couldn't rename the finished file"));
        ok = false;
    }
    if (!ok) file_remove(part);
    else {
        j->out_bytes = av.out_bytes;
        atomic_store(&j->permille, 1000);
    }
    return ok;
}

/* ==================================================================== queue */

/* CQ_NOROOM: its copy didn't fit on the disk (and nothing running could make room) */
enum { CQ_WAITING, CQ_RUNNING, CQ_DONE, CQ_FAILED, CQ_NOROOM };

typedef struct {
    uint64_t id;
    char name[160];
    int state;
    ConvertJob job; /* a running worker writes job.permille / out_mb / err / what; the rest only under g_cl */
    char root[300]; /* its library folder: the disk its copy goes on */
    int64_t need;   /* the most its copy can take: the original's size */
    int64_t spare, held; /* CQ_NOROOM: the room there was on its disk then (the words come when shown) */
    int64_t started, ended; /* unix seconds */
    bool settled; /* done, and its original dealt with ("delete each original once its copy is made") */
    /* (2.7) Full throttle: made in parts side by side (split) - nparts jobs, next_part the next a worker takes, how many
       are done or failed; the worker finishing the last joins them (job, npieces). dur_ms = its length (the scan's) */
    ConvertJob *parts;
    int nparts, next_part, parts_done, parts_failed;
    bool joining;
    int64_t dur_ms;
} ConvItem;

#define PART_MIN_S CONVERT_PART_MIN_S /* (2.7) convert.h */

static void item_free(ConvItem *it) {
    free(it->parts);
    free(it);
}

/* (2.7) what a video made in parts has done: its parts (the first 90%), then the join; else its job's. g_cl held, or
   a worker's own reading (atomics) */
static int item_permille(const ConvItem *it) {
    if (!it->nparts) return atomic_load(&it->job.permille);
    int64_t sum = 0;
    for (int k = 0; k < it->nparts; k++) sum += atomic_load(&it->parts[k].permille);
    return (int)(sum * 9 / (10 * it->nparts)) + (it->joining ? atomic_load(&it->job.permille) / 10 : 0);
}

static int64_t item_out_mb(const ConvItem *it) { /* what it wrote so far: its parts' and the join's */
    int64_t mb = atomic_load(&it->job.out_mb);
    for (int k = 0; k < it->nparts; k++) mb += atomic_load(&it->parts[k].out_mb);
    return mb;
}

/* Items are allocated one by one so a worker's item never moves; only waiting
   and finished ones are ever freed. Lock order: g_pick, then upload.c's lock, then g_cl -
   upload.c may call in here holding its lock, so nothing here calls upload.c holding g_cl. */
static Lock g_cl = LOCK_INIT;
static Lock g_pick = LOCK_INIT; /* worker threads only: one at a time decides what fits */
static ConvItem **g_items;
static int g_nitems, g_capitems, g_running, g_done_seen;
static atomic_int g_cstop;
static atomic_bool g_drop; /* setting media.drop_originals, for the workers (the tick keeps it current) */
static uint64_t g_shown;
static int64_t g_rescan_at;

/* What running conversions may still write: their original's size, less what they wrote.
   g_cl held; `skip` (may be NULL) left out. */
static int running_holds(MediaHold *out, int max, const ConvItem *skip) {
    int n = 0;
    for (int i = 0; i < g_nitems && n < max; i++) {
        const ConvItem *it = g_items[i];
        if (it->state != CQ_RUNNING || it == skip) continue;
        str_copy(out[n].root, sizeof out[n].root, it->root);
        out[n].bytes = MAX(it->need - (item_out_mb(it) << 20), 0);
        n++;
    }
    return n;
}

int convert_holds(MediaHold *out, int max) {
    lock_take(&g_cl);
    int n = running_holds(out, max, NULL);
    lock_give(&g_cl);
    return n;
}

bool convert_making(const char *path) {
    bool mine = false;
    lock_take(&g_cl);
    for (int i = 0; i < g_nitems && !mine; i++) {
        const ConvItem *it = g_items[i];
        mine = it->state == CQ_RUNNING && (str_ieq(path, it->job.src) || str_ieq(path, it->job.dst));
    }
    lock_give(&g_cl);
    return mine;
}

bool convert_give_way(const char *path) {
    bool making = false;
    lock_take(&g_cl);
    int w = 0;
    for (int i = 0; i < g_nitems; i++) {
        ConvItem *it = g_items[i];
        bool hit = str_ieq(path, it->job.src) || str_ieq(path, it->job.dst);
        making |= hit && it->state == CQ_RUNNING;
        if (hit && it->state == CQ_WAITING) {
            log_info("convert: %s left the queue - its copy comes by upload", it->name);
            item_free(it);
        } else g_items[w++] = it;
    }
    g_nitems = w;
    lock_give(&g_cl);
    return making;
}

void convert_drop_leftover(const char *path) {
    char part[1100];
    bool mine = upload_has_part(path); /* (2.7: a device's part, waiting for its join - asked before g_cl: the lock order) */
    lock_take(&g_cl); /* (held while deleting: nothing can be queued for it in between) */
    for (int i = 0; i < g_nitems && !mine; i++) {
        if (g_items[i]->state > CQ_RUNNING) continue;
        convert_part_path(g_items[i]->job.dst, part, sizeof part);
        mine = str_ieq(part, path);
        for (int k = 0; k < g_items[i]->nparts && !mine; k++) { /* (2.7: its parts, made side by side) */
            convert_piece_path(g_items[i]->job.dst, k, part, sizeof part);
            mine = str_ieq(part, path);
        }
    }
    if (!mine && file_remove(path)) log_info("convert: removed %s, left over from a conversion that never finished", path);
    lock_give(&g_cl);
}

static int count_state(int state) { /* g_cl held */
    int n = 0;
    for (int i = 0; i < g_nitems; i++) n += g_items[i]->state == state;
    return n;
}

/* Copies just made whose originals the tick is about to delete ("Do that by itself after each
   conversion"): room on its way, as a running conversion's is. g_cl held. */
static int settling(void) {
    int n = 0;
    for (int i = 0; i < g_nitems && atomic_load(&g_drop); i++) n += g_items[i]->state == CQ_DONE && !g_items[i]->settled;
    return n;
}

void convert_trouble(int *noroom, int *failed) {
    lock_take(&g_cl);
    *noroom = count_state(CQ_NOROOM);
    *failed = count_state(CQ_FAILED);
    lock_give(&g_cl);
}

static ConvItem *waiting_item(uint64_t id) { /* g_cl held */
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i]->id == id && g_items[i]->state == CQ_WAITING) return g_items[i];
    return NULL;
}

/* A waiting conversion, as the room check sees it. */
typedef struct {
    uint64_t id;
    int64_t need;
    char root[300];
    int64_t spare, held; /* room on its disk: judge() */
} Want;

static Want *waiting_list(int *n) { /* g_cl held; in queue order; the caller frees it */
    int k = count_state(CQ_WAITING);
    Want *w = xcalloc((size_t)k + 1, sizeof *w);
    *n = 0;
    for (int i = 0; i < g_nitems; i++) {
        const ConvItem *it = g_items[i];
        if (it->state != CQ_WAITING) continue;
        w[*n].id = it->id;
        w[*n].need = it->need;
        str_copy(w[*n].root, sizeof w[*n].root, it->root);
        (*n)++;
    }
    return w;
}

/* Room for each on its disk past the holds - one look per library folder, and outside g_cl
   (asking a disk can take a moment; pages wait for g_cl). */
static void judge(Want *w, int n, const MediaHold *holds, int nh) {
    for (int i = 0; i < n; i++) {
        int k = 0;
        while (k < i && !str_eq(w[k].root, w[i].root)) k++;
        if (k < i) w[i].spare = w[k].spare, w[i].held = w[k].held;
        else w[i].spare = media_room(w[i].root, holds, nh, &w[i].held);
    }
}

static void set_no_room(ConvItem *it, const Want *w) { /* g_cl held */
    it->spare = w->spare;
    it->held = w->held;
    it->state = CQ_NOROOM;
    it->ended = now_sec();
}

/* "Show S01E03 needs up to 1.0 GB, and the drive has 400 MB to spare (Hemma keeps 1 GB free)" - in the
   language in use. g_cl held. */
static void no_room_text(const ConvItem *it, char *out, size_t cap) {
    char need[24], tail[300];
    media_size_text(it->need, need, sizeof need);
    media_room_text(tail, sizeof tail, it->spare, it->held);
    snprintf(out, cap, T("%s needs up to %s, and %s"), it->name, need, tail);
}

/* Does running `it` still fit, counted next to everything else that holds room now? */
static bool still_fits(const ConvItem *it) {
    MediaHold holds[MEDIA_MAX_HOLDS];
    int n = upload_holds(holds, MEDIA_MAX_HOLDS);
    lock_take(&g_cl);
    n += running_holds(holds + n, MEDIA_MAX_HOLDS - n, it);
    lock_give(&g_cl);
    return it->need <= media_room(it->root, holds, n, NULL); /* (root and need never change) */
}

/* For a worker: the next waiting video whose copy fits on its disk, marked running - or NULL,
   and the worker ends (g_running counts it out): nothing waiting, stopping, or nothing fits.
   While other conversions run (or a finished one's original is about to go), what doesn't fit
   waits for them: their hold goes when they finish, and copies come out smaller than their
   originals. With none, there is nothing to wait for: what's left is set aside as "no room"
   (not tried, not churned through). */
static int workers_wanted(void);
static atomic_int g_power;

/* (2.7) parts handed out and not finished, and parts still to hand out - of the videos made in parts. g_cl held */
static int parts_busy(void) {
    int n = 0;
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i]->state == CQ_RUNNING && g_items[i]->nparts)
            n += g_items[i]->next_part - g_items[i]->parts_done - g_items[i]->parts_failed;
    return n;
}
static int parts_waiting(void) {
    int n = 0;
    for (int i = 0; i < g_nitems; i++) {
        const ConvItem *it = g_items[i];
        if (it->state == CQ_RUNNING && it->nparts && !it->parts_failed) n += it->nparts - it->next_part;
    }
    return n;
}

/* (2.7; owner: "Full throttle ... processes the videos in the most effective way") A video being made H.264 from HEVC,
   on Full throttle, when fewer are waiting than workers could take: made in parts side by side - one more per idle
   worker, each at least PART_MIN_S long, AV_CONV_MAX_PIECES at most. The worker takes part 0 (returned), the tick starts
   workers for the rest; -1 = made whole, as ever. Its copy and its parts take room together: its need doubles. */
static int split(ConvItem *it) {
    if (atomic_load(&g_power) != CONVERT_FULL || !it->job.h264 || it->dur_ms < 2 * PART_MIN_S * 1000LL) return -1;
    lock_take(&g_cl);
    int busy = parts_busy() + parts_waiting(); /* (videos made whole count one each - this one too) */
    for (int i = 0; i < g_nitems; i++) busy += g_items[i]->state == CQ_RUNNING && !g_items[i]->nparts;
    int idle = workers_wanted() - busy - count_state(CQ_WAITING);
    int n = (int)MIN((int64_t)MIN(1 + idle, AV_CONV_MAX_PIECES), it->dur_ms / (PART_MIN_S * 1000LL));
    ConvertJob *parts = n >= 2 ? calloc((size_t)n, sizeof *parts) : NULL;
    if (parts) {
        int64_t len = it->dur_ms * 1000000LL;
        for (int k = 0; k < n; k++) {
            ConvertJob *p = &parts[k];
            str_copy(p->src, sizeof p->src, it->job.src);
            str_copy(p->dst, sizeof p->dst, it->job.dst);
            p->quality = it->job.quality, p->audio_kbps = it->job.audio_kbps, p->h264 = true, p->stop = it->job.stop;
            p->piece = k;
            p->piece_from = len / n * k;
            p->piece_to = k == n - 1 ? AV_CONV_TO_END : len / n * (k + 1);
        }
        it->parts = parts, it->nparts = n, it->next_part = 1;
        it->need *= 2;
    }
    lock_give(&g_cl);
    if (!parts) return -1;
    log_info("convert: %s in %d parts side by side (full throttle)", it->name, n);
    return 0;
}

static ConvItem *pick(int *part) {
    *part = -1;
    lock_take(&g_cl);
    if (g_running > workers_wanted()) { /* (2.7: a lower power tier was chosen - this worker stops after its video) */
        g_running--;
        lock_give(&g_cl);
        return NULL;
    }
    for (int i = 0; i < g_nitems && !atomic_load(&g_cstop); i++) { /* (2.7) a part of a video under way, first */
        ConvItem *it = g_items[i];
        if (it->state != CQ_RUNNING || !it->nparts || it->parts_failed || it->next_part >= it->nparts) continue;
        *part = it->next_part++;
        lock_give(&g_cl);
        return it;
    }
    lock_give(&g_cl);
    for (;;) {
        lock_take(&g_pick);
        MediaHold holds[MEDIA_MAX_HOLDS];
        int nh = upload_holds(holds, MEDIA_MAX_HOLDS), n = 0; /* (before g_cl: see the lock order) */
        lock_take(&g_cl);
        Want *w = atomic_load(&g_cstop) ? NULL : waiting_list(&n);
        int others = count_state(CQ_RUNNING) + settling();
        nh += running_holds(holds + nh, MEDIA_MAX_HOLDS - nh, NULL);
        lock_give(&g_cl);
        judge(w, n, holds, nh);
        int fits = -1;
        for (int i = 0; i < n && fits < 0; i++)
            if (w[i].need <= w[i].spare) fits = i;

        lock_take(&g_cl);
        ConvItem *it = fits >= 0 && !atomic_load(&g_cstop) ? waiting_item(w[fits].id) : NULL;
        bool again = fits >= 0 && n && !it; /* that one went meanwhile (Stop): look again */
        if (it) {
            it->state = CQ_RUNNING;
            it->started = now_sec();
            it->job.stop = &g_cstop;
        } else if (fits < 0 && n && others) {
            again = !app_testing(); /* tests have no worker to wait for */
        } else if (fits < 0 && n) {
            for (int i = 0; i < n; i++) {
                ConvItem *x = waiting_item(w[i].id);
                if (x) set_no_room(x, &w[i]);
            }
            log_warn("convert: %d video%s set aside - not enough room on the drive", n, n == 1 ? "" : "s");
            again = count_state(CQ_WAITING) > 0; /* queued meanwhile: those get their own look */
        }
        if (!it && !again) g_running--;
        lock_give(&g_cl);
        /* marked first, then checked again: an upload planned between our look and the mark
           can't count on the same room (upload.c checks again once its upload is registered) */
        if (it && !atomic_load(&g_cstop) && !still_fits(it)) {
            lock_take(&g_cl);
            it->state = CQ_WAITING;
            lock_give(&g_cl);
            it = NULL;
            again = true;
        }
        lock_give(&g_pick);
        free(w);
        if (it) *part = split(it); /* (2.7: Full throttle - part 0 of it, or the whole) */
        if (it || !again) return it;
        for (int t = 0; fits < 0 && t < 2000 && !atomic_load(&g_cstop); t += 100) plat_sleep_ms(100);
    }
}

/* (2.7) a part done (or not): counted; the last one to finish joins the parts - or, one having failed or the queue
   stopped, the video failed and its parts go. True when this worker is to join them. */
static bool part_finished(ConvItem *it, int part, bool ok) {
    lock_take(&g_cl);
    if (ok) it->parts_done++;
    else {
        if (!it->parts_failed) str_copy(it->job.err, sizeof it->job.err, it->parts[part].err);
        it->parts_failed++;
    }
    bool stopping = atomic_load(&g_cstop) != 0;
    bool last = it->parts_done + it->parts_failed == it->next_part &&
                (it->parts_failed || stopping || it->next_part == it->nparts);
    bool join = last && !it->parts_failed && !stopping;
    if (last && !join) {
        if (!it->parts_failed) str_copy(it->job.err, sizeof it->job.err, N_("Stopped"));
        it->state = CQ_FAILED;
        it->ended = now_sec();
    }
    if (join) it->joining = true;
    lock_give(&g_cl);
    if (last && !join) {
        remove_pieces(it->job.dst, it->nparts);
        log_warn("convert: %s - %s (made in %d parts)", it->job.src, it->job.err, it->nparts);
    }
    return join;
}

static void worker(void *arg) {
    (void)arg;
    thread_background(); /* the household's pages come first */
    int part;
    for (ConvItem *it; (it = pick(&part)) != NULL;) {
        if (part >= 0) { /* (2.7) a part of it, made side by side with the others */
            bool done = convert_file(&it->parts[part]);
            if (!part_finished(it, part, done)) continue;
            it->job.npieces = it->nparts; /* (the last part: this worker joins them) */
        }
        log_info("convert: %s", it->job.src);
        bool ok = convert_file(&it->job);
        if (ok)
            log_info("convert: done %s (%s, %lld MB, %d subtitle track%s saved)", it->job.dst, it->job.what,
                     (long long)(it->job.out_bytes >> 20), it->job.subs, it->job.subs == 1 ? "" : "s");
        else if (it->job.subs) /* (refused for what it holds: its subtitles came out all the same) */
            log_warn("convert: %s - %s (%d subtitle track%s saved)", it->job.src, it->job.err, it->job.subs,
                     it->job.subs == 1 ? "" : "s");
        else log_warn("convert: %s - %s", it->job.src, it->job.err);

        lock_take(&g_cl);
        it->state = ok ? CQ_DONE : CQ_FAILED;
        it->ended = now_sec();
        lock_give(&g_cl);
    }
}

bool convert_queued(uint64_t id) {
    lock_take(&g_cl);
    bool q = false;
    for (int i = 0; i < g_nitems && !q; i++) q = g_items[i]->id == id && g_items[i]->state <= CQ_RUNNING;
    lock_give(&g_cl);
    return q;
}

bool convert_busy(void) {
    lock_take(&g_cl);
    bool busy = g_running > 0;
    for (int i = 0; i < g_nitems && !busy; i++) busy = g_items[i]->state <= CQ_RUNNING;
    lock_give(&g_cl);
    return busy;
}

typedef enum { EQ_ADDED, EQ_NO, EQ_ON_ITS_WAY, EQ_THERE, EQ_UPLOAD, EQ_N } Enq;

/* (2.6) The room an H.264 copy of HEVC may take: the encoder's size follows the picture, not the source - 0.6-1.5 times
   the HEVC in real episodes (a Friends episode 45 MB a minute at 1080p, the most seen), more for a small x265 file. So
   half as much again as the source, and at least 50 MB a minute at 1080p (by its pixels) - the server writes on without
   asking again */
static int64_t h264_need(const MediaFile *f, int64_t size) {
    const MediaProbe *p = &media_playable(f)->probe;
    double px = p->width > 0 && p->height > 0 ? (double)p->width * p->height / (1920.0 * 1080.0) : 1.0;
    int64_t by_time = (int64_t)((double)p->duration_ms / 60000.0 * 50.0 * px * (1 << 20));
    return MAX(size + size / 2, by_time);
}

/* Event loop (so is everything that claims a file: upload plans and PUTs, this - one can't
   slip in between a check and a claim). The copy mustn't be there already (made since the
   last scan) nor be coming by upload; then the video joins the queue once. */
static Enq enqueue(const MediaFile *f, bool h264) {
    char src[1024], dst[1024];
    bool replace = false;
    int64_t size = 0;
    if (h264) { /* (2.6) HEVC made H.264: from its original or its copy, into its .web.mp4 */
        if (!convert_h264_paths(f, src, sizeof src, dst, sizeof dst, &replace) || !path_stat(src, &size, NULL, NULL))
            return EQ_NO;
    } else {
        if (!f || convert_need(f) == CONV_NONE || !media_full_path(f, src, sizeof src)) return EQ_NO;
        convert_web_path(src, dst, sizeof dst);
        if (path_stat(dst, NULL, NULL, NULL)) return EQ_THERE;
        size = f->size;
    }
    if (upload_claims(src) || upload_claims(dst)) return EQ_UPLOAD;
    Enq r = EQ_ADDED;
    lock_take(&g_cl);
    for (int i = 0; i < g_nitems && r == EQ_ADDED; i++) /* x.mkv and x.webm would both make x.web.mp4 */
        if (g_items[i]->state <= CQ_RUNNING && (g_items[i]->id == f->id || str_ieq(g_items[i]->job.dst, dst)))
            r = EQ_ON_ITS_WAY;
    if (r == EQ_ADDED) {
        int w = 0; /* its earlier failure (or "no room") is history once it's queued again */
        for (int i = 0; i < g_nitems; i++) {
            ConvItem *it = g_items[i];
            if (it->id == f->id && it->state >= CQ_FAILED) item_free(it);
            else g_items[w++] = it;
        }
        g_nitems = w;
        if (g_nitems == g_capitems) {
            g_capitems = g_capitems ? g_capitems * 2 : 64;
            g_items = xrealloc(g_items, (size_t)g_capitems * sizeof *g_items);
        }
        ConvItem *it = xcalloc(1, sizeof *it);
        g_items[g_nitems++] = it;
        it->id = f->id;
        it->state = CQ_WAITING;
        it->need = h264 ? h264_need(f, size) : size;
        it->job.h264 = h264, it->job.replace = replace;
        it->dur_ms = media_playable(f)->probe.duration_ms; /* (2.7: a long one may be made in parts) */
        str_copy(it->root, sizeof it->root, media_lib()->roots[f->root]);
        const MediaSeries *s = f->series ? media_series(f->series) : NULL;
        if (s && f->name.kind == MEDIA_EPISODE)
            snprintf(it->name, sizeof it->name, "%s S%02dE%02d", s->title, f->name.season, f->name.episode);
        else snprintf(it->name, sizeof it->name, "%s", f->name.title[0] ? f->name.title : f->rel);
        str_copy(it->job.src, sizeof it->job.src, src);
        str_copy(it->job.dst, sizeof it->job.dst, dst);
    }
    lock_give(&g_cl);
    return r;
}

/* g_cl held. CQ_WAITING: drop the waiting ones (stop); CQ_DONE: drop the finished ones (new run). */
static void drop_items(int which) {
    int w = 0;
    for (int i = 0; i < g_nitems; i++) {
        ConvItem *it = g_items[i];
        bool drop = which == CQ_WAITING ? it->state == CQ_WAITING : it->state >= CQ_DONE;
        if (drop) item_free(it);
        else g_items[w++] = it;
    }
    g_nitems = w;
}

/* Nothing converting (and no worker that could start): what doesn't fit even on its own is set
   aside right away, so the answer can say so. Returns how many; *why = the first one's reason. */
static int set_aside(char *why, size_t cap) {
    MediaHold holds[MEDIA_MAX_HOLDS];
    int nh = upload_holds(holds, MEDIA_MAX_HOLDS), n = 0, out = 0;
    lock_take(&g_cl);
    Want *w = waiting_list(&n);
    lock_give(&g_cl);
    judge(w, n, holds, nh);
    lock_take(&g_cl);
    for (int i = 0; i < n; i++) {
        ConvItem *it = w[i].need > w[i].spare ? waiting_item(w[i].id) : NULL;
        if (!it) continue;
        set_no_room(it, &w[i]);
        if (!out++) no_room_text(it, why, cap); /* (in the language of whoever pressed Convert) */
    }
    lock_give(&g_cl);
    free(w);
    return out;
}

/* ---- how hard conversions work (2.7; owner, 2026-11-01: "three tiers of processing the videos to play everywhere -
   low-power (~30%) - Medium-power (60%) - Full throttle (95%~100%) ... and to also think about the users choice", "a
   small description on what it would mean per machine, like how many cores would be used, how it processes it") ----
   Hemma's computer: setting media.power; a person's own devices ("On this device", the upload form): pref
   media.power_here. Medium is the default - what Hemma did before, about. */

static atomic_int g_power = CONVERT_MEDIUM; /* (media.power, for the workers: set on the loop by convert_tick/the action) */

int convert_power_of(const char *s) {
    return str_eq(s, "low") ? CONVERT_LOW : str_eq(s, "full") ? CONVERT_FULL : CONVERT_MEDIUM;
}

static const char *power_name(int tier) { return tier == CONVERT_LOW ? "low" : tier == CONVERT_FULL ? "full" : "medium"; }

int convert_power_workers(int tier, int cores, bool phone) {
    if (cores < 1) cores = 1;
    if (phone) return tier == CONVERT_FULL ? MIN(2, cores) : 1; /* (a phone: warm in a hand, its battery) */
    int n = tier == CONVERT_LOW ? (cores * 3 + 5) / 10 : tier == CONVERT_FULL ? cores : (cores * 6 + 5) / 10;
    return CLAMP(n, 1, MAX_WORKERS);
}

static int workers_wanted(void) { return convert_power_workers(atomic_load(&g_power), plat_cpu_count(), false); }

/* as many workers as the tier allows and the queue has work for (those over it stop after their video: pick);
   how many run now */
static int spawn_workers(void) {
    lock_take(&g_cl);
    int want = MIN(workers_wanted(), g_running + count_state(CQ_WAITING) + parts_waiting()); /* (2.7: and parts) */
    while (!app_testing() && g_running < want) { /* (tests start them with convert_test_start) */
        g_running++;
        if (!thread_spawn(worker, NULL)) {
            g_running--;
            break;
        }
    }
    int at_once = MAX(g_running, 1);
    lock_give(&g_cl);
    return at_once;
}

static void settle_finished(void);

void convert_act_start(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can convert videos"));
        return;
    }
    const MediaIndex *ix = media_lib();
    uint64_t one = 0;
    bool single = media_parse_id(fields_str(f, "id"), &one);
    int got[EQ_N] = {0};
    settle_finished(); /* a copy that finished a moment ago: its original goes first (before it's forgotten) */
    lock_take(&g_cl);
    bool idle = !g_running && !count_state(CQ_RUNNING);
    if (idle) { /* a fresh run: forget how the last one went */
        drop_items(CQ_DONE);
        g_done_seen = 0;
    }
    atomic_store(&g_cstop, 0);
    lock_give(&g_cl);
    bool h264 = str_eq(fields_str(f, "h264"), "1");
    uint64_t sid = 0;
    bool series = h264 && media_parse_id(fields_str(f, "series"), &sid), films = h264 && str_eq(fields_str(f, "films"), "1");
    int season = series && *fields_str(f, "season") ? atoi(fields_str(f, "season")) : -1;
    if (single) got[enqueue(media_file(one), h264)]++;
    else if (series) { /* (2.6) a series, or one of its seasons */
        for (int k = 0; k < ix->nepisodes; k++) {
            const MediaFile *e = &ix->files[ix->episodes[k]];
            if (e->series == sid && (season < 0 || e->name.season == season)) got[enqueue(e, true)]++;
        }
    } else if (films) {
        for (int k = 0; k < ix->nmovies; k++) got[enqueue(&ix->files[ix->movies[k]], true)]++;
    } else {
        bool *ep = xcalloc((size_t)ix->nfiles + 1, sizeof *ep);
        for (int k = 0; k < ix->nepisodes; k++) { /* series in order */
            ep[ix->episodes[k]] = true;
            got[enqueue(&ix->files[ix->episodes[k]], h264)]++;
        }
        for (int i = 0; i < ix->nfiles; i++)
            if (!ep[i] && (h264 ? ix->files[i].role == MF_MAIN && convert_h264_need(&ix->files[i]) /* (as the card counts) */
                                : bulk_need(&ix->files[i])))
                got[enqueue(&ix->files[i], h264)]++;
        free(ep);
    }
    int added = got[EQ_ADDED], noroom = 0;
    char why[600] = "";
    if (idle && added) noroom = set_aside(why, sizeof why);

    int at_once = spawn_workers();

    if (noroom && noroom == added) {
        if (noroom == 1) ctx_fail(c, T("Not enough room on the drive: %s"), why);
        else ctx_fail(c, T("Not enough room on the drive for %d videos: %s"), noroom, why); /* (2 or more) */
    } else if (added) {
        char said[200], also[120] = "";
        int n = added - noroom;
        snprintf(said, sizeof said, TN(n, "Converting %d video, %d at a time", "Converting %d videos, %d at a time"), n,
                 at_once);
        if (noroom) {
            str_copy(also, sizeof also, " - ");
            snprintf(also + 3, sizeof also - 3,
                     TN(noroom, "%d more doesn't fit on the drive", "%d more don't fit on the drive"), noroom);
        }
        ctx_ok(c, "%s%s", said, also);
    } else if (single && got[EQ_UPLOAD]) {
        ctx_info(c, "%s", T("An upload is bringing this video's play-everywhere copy right now"));
    } else if (single && got[EQ_THERE]) {
        ctx_info(c, "%s", T("Its play-everywhere copy is there already - the library shows it in a moment"));
        media_scan_start();
    } else if (single) {
        ctx_info(c, "%s", T("That video plays everywhere already (or is on its way)"));
    } else {
        ctx_info(c, "%s",
                 got[EQ_UPLOAD] ? T("Nothing to convert - the rest is being uploaded with copies right now")
                                : T("Nothing needs converting"));
    }
    app_wake_in(500);
}

void convert_act_stop(Ctx *c, Fields *f) {
    (void)f;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    atomic_store(&g_cstop, 1);
    lock_take(&g_cl);
    drop_items(CQ_WAITING);
    lock_give(&g_cl);
    ctx_ok(c, "%s", T("Stopping - finished videos stay, the one in progress is dropped"));
}

bool convert_replace_original(uint64_t id, const char *src, const char *dst) {
    /* (2.6) an H.264 copy made from Hemma's own HEVC copy (its original gone, or one the converter can't read) took that
       copy's place: src IS the new copy - there's no original here to drop */
    if (str_eq(src, dst) || str_ends(src, ".web.mp4")) return false;
    const MediaFile *o = media_file(id);
    MediaProbe pr;
    char rel[420];
    if (!o || !media_probe_file(dst, &pr) || !media_copy_lasts(o->probe.duration_ms, pr.duration_ms)) {
        log_warn("convert: kept the original %s - its copy doesn't last as long", src);
        return false;
    }
    if (media_compat_cost(&pr) > media_compat_cost(&o->probe)) { /* (never for copies made here) */
        log_warn("convert: kept the original %s - it plays in more places than its copy", src);
        return false;
    }
    convert_web_path(o->rel, rel, sizeof rel);
    if (!media_drop_original(src, o->id, media_make_id(media_lib()->roots[o->root], rel))) {
        log_warn("convert: couldn't delete the original %s", src);
        return false;
    }
    log_info("convert: deleted the original %s - its copy plays in its place", src);
    return true;
}

/* Setting "media.drop_originals": each finished copy takes its original's place - checked on
   the copy file itself, before the library has even seen it (the rescan comes after). Event
   loop only (the index, everyone's progress). An item counts as settled once its original is
   dealt with: until then a worker waits for the room that frees (pick). */
static void settle_finished(void) {
    typedef struct {
        uint64_t id;
        char src[1024], dst[1024];
    } Done;
    bool drop = setting_int("media.drop_originals", 0) == 1;
    atomic_store(&g_drop, drop);
    Done *todo = NULL;
    int n = 0;
    lock_take(&g_cl);
    for (int i = 0; i < g_nitems; i++) {
        ConvItem *it = g_items[i];
        if (it->state != CQ_DONE || it->settled) continue;
        if (!drop) {
            it->settled = true;
            continue;
        }
        todo = xrealloc(todo, (size_t)(n + 1) * sizeof *todo);
        todo[n].id = it->id;
        str_copy(todo[n].src, sizeof todo[n].src, it->job.src);
        str_copy(todo[n].dst, sizeof todo[n].dst, it->job.dst);
        n++;
    }
    lock_give(&g_cl);
    for (int i = 0; i < n; i++) convert_replace_original(todo[i].id, todo[i].src, todo[i].dst);
    lock_take(&g_cl);
    for (int i = 0; i < g_nitems && n; i++)
        for (int k = 0; k < n; k++)
            if (g_items[i]->state == CQ_DONE && g_items[i]->id == todo[k].id) g_items[i]->settled = true;
    lock_give(&g_cl);
    free(todo);
}

void convert_tick(int64_t now) {
    atomic_store(&g_power, convert_power_of(setting("media.power", ""))); /* (2.7: the workers read it) */
    lock_take(&g_cl);
    bool parts = parts_waiting() > 0;
    lock_give(&g_cl);
    if (parts) spawn_workers(); /* (2.7) a video split into parts: workers for them, started here, on the loop */
    settle_finished();
    uint64_t sig = 0; /* (unsigned: it wraps, by design) */
    int done = 0, running;
    lock_take(&g_cl);
    running = g_running;
    for (int i = 0; i < g_nitems; i++) {
        sig = sig * 31 + (uint64_t)g_items[i]->state * 7 + (uint64_t)item_permille(g_items[i]) / 10; /* every 1% */
        done += g_items[i]->state == CQ_DONE;
    }
    lock_give(&g_cl);
    if (running) app_wake_in(1000);
    if (sig != g_shown) {
        g_shown = sig;
        app_changed_where("/media");
    }
    if (done > g_done_seen) { /* finished copies join the library: rescan once things settle a little */
        g_done_seen = done;
        g_rescan_at = now + (running ? 30000 : 2000);
    }
    if (g_rescan_at && now >= g_rescan_at && !media_scanning()) {
        g_rescan_at = 0;
        media_scan_start();
    }
}

void convert_stop(void) {
    atomic_store(&g_cstop, 1);
    int n = 0;
    for (int waited = 0; waited < 8000; waited += 50) { /* let workers drop their .part files */
        lock_take(&g_cl);
        n = g_running;
        lock_give(&g_cl);
        if (!n) break;
        plat_sleep_ms(50);
    }
    if (n) return; /* a worker still on its way out keeps its item */
    lock_take(&g_cl); /* a fresh queue for the next start */
    for (int i = 0; i < g_nitems; i++) item_free(g_items[i]);
    g_nitems = g_done_seen = 0;
    lock_give(&g_cl);
    g_shown = 0;
    g_rescan_at = 0;
}

const char *convert_test_start(void) {
    static char name[160];
    lock_take(&g_cl);
    g_running++; /* as a worker would be */
    lock_give(&g_cl);
    int part;
    ConvItem *it = pick(&part); /* (NULL: it counted the "worker" out) */
    if (!it) return NULL;
    lock_take(&g_cl);
    g_running--; /* it stays running without one, until convert_test_finish */
    str_copy(name, sizeof name, it->name);
    lock_give(&g_cl);
    return name;
}

void convert_test_finish(bool ok) {
    lock_take(&g_cl);
    for (int i = 0; i < g_nitems; i++) {
        ConvItem *it = g_items[i];
        if (it->state != CQ_RUNNING) continue;
        it->state = ok ? CQ_DONE : CQ_FAILED;
        it->ended = now_sec();
        if (!ok) str_copy(it->job.err, sizeof it->job.err, "The test said so");
    }
    lock_give(&g_cl);
}

/* ===================================================================== page */

static void gb_text(int64_t bytes, char *out, size_t cap) { /* "12 GB", "1.5 GB" (a decimal comma in sv/es) */
    double gb = (double)bytes / 1073741824.0;
    snprintf(out, cap, gb >= 10 ? "%.0f GB" : "%.1f GB", gb);
    char *dot = strchr(out, '.');
    if (dot) *dot = i18n_decimal();
}

void convert_summary(ConvertSummary *s) {
    memset(s, 0, sizeof *s);
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++) s->need += bulk_need(&ix->files[i]);
    lock_take(&g_cl);
    s->busy = count_state(CQ_WAITING) + count_state(CQ_RUNNING);
    s->failed = count_state(CQ_FAILED);
    s->noroom = count_state(CQ_NOROOM);
    lock_give(&g_cl);
}

void convert_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    const MediaIndex *ix = media_lib();
    int need = 0, remux = 0, extra = 0;
    int64_t bytes = 0, extra_bytes = 0;
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *lesser = media_lesser_version(&ix->files[i], NULL);
        if (lesser) extra++, extra_bytes += lesser->size;
        if (!bulk_need(&ix->files[i])) continue;
        ConvertNeed n = convert_need(&ix->files[i]);
        need++;
        remux += n == CONV_REMUX;
        bytes += ix->files[i].size;
    }
    lock_take(&g_cl);
    int waiting = count_state(CQ_WAITING), running = count_state(CQ_RUNNING), done = count_state(CQ_DONE),
        failed = count_state(CQ_FAILED), noroom = count_state(CQ_NOROOM);
    if (!need && !g_nitems && !extra) {
        lock_give(&g_cl);
        return;
    }
    ui_card(b, T("Play everywhere"), NULL, NULL);
    if (waiting + running) {
        buf_puts(b, "<p><b>");
        H(b, T("Converting · %d of %d done"), done, done + waiting + running + failed);
        buf_puts(b, "</b></p><ul class=\"rows media-conv\">");
        for (int i = 0; i < g_nitems; i++) {
            const ConvItem *it = g_items[i];
            if (it->state != CQ_RUNNING) continue;
            H(b, "<li class=\"row\"><span class=\"grow\">%s", it->name);
            if (it->nparts) { /* (2.7: Full throttle - several cores on it) */
                buf_puts(b, " <small class=\"muted\">");
                H(b, TN(it->nparts, "in %d part", "in %d parts"), it->nparts);
                buf_puts(b, "</small>");
            }
            H(b, "</span><progress class=\"media-up-bar\" max=\"1000\" value=\"%d\"></progress></li>", item_permille(it));
        }
        buf_puts(b, "</ul>");
    } else if (need) {
        char sz[24];
        gb_text(bytes, sz, sizeof sz);
        buf_puts(b, "<p>");
        H(b,
          TN(need, "%d video (%s) is in a format that browsers, iPhones or TVs can't all play.",
             "%d videos (%s) are in a format that browsers, iPhones or TVs can't all play."),
          need, sz);
        buf_puts(b, "</p><p class=\"muted media-small\">");
        H(b, "%r %r ",
          T("Hemma can make a copy of each that plays everywhere - once, next to the original (<code "
            "class=\"media-code\">name.web.mp4</code>)."),
          remux == need ? T("These only need a new container: quick, nothing is re-encoded.")
                        : T("DVD video is re-encoded to H.264, and Dolby and DTS sound to AAC; copies are about a third "
                            "of the size."));
        H(b, T("It runs in the background, %d at a time."), workers_wanted());
        buf_puts(b, "</p>");
    }
    if (done || failed) {
        buf_puts(b, "<p class=\"muted media-small\">");
        H(b, T("%d converted"), done);
        if (failed) {
            buf_puts(b, ", ");
            H(b, T("%d failed"), failed);
        }
        buf_puts(b, ".</p>");
        if (failed) {
            buf_puts(b, "<ul class=\"media-up-list\">");
            for (int i = 0; i < g_nitems; i++) /* (the av library's own reasons are English: its data) */
                if (g_items[i]->state == CQ_FAILED)
                    H(b, "<li class=\"bad\">%s — %s</li>", g_items[i]->name, T(g_items[i]->job.err));
            buf_puts(b, "</ul>");
        }
    }
    if (noroom) { /* one line for all of them: they didn't fit for the same reason */
        const ConvItem *first = NULL;
        for (int i = 0; i < g_nitems && !first; i++)
            if (g_items[i]->state == CQ_NOROOM) first = g_items[i];
        char why[600];
        no_room_text(first, why, sizeof why);
        buf_puts(b, "<p class=\"notice bad\" id=\"media-conv-room\">");
        H(b,
          TN(noroom, "%d video didn't fit on the drive: %s. Make room, then convert again.",
             "%d videos didn't fit on the drive: %s. Make room, then convert again."),
          noroom, why);
        buf_puts(b, "</p>");
    }
    lock_give(&g_cl);
    buf_puts(b, "<div class=\"modal-f\">");
    if (waiting + running) ui_act(b, "media.convert_stop", "btn", T("Stop"), NULL, NULL, NULL);
    else if (need) {
        char label[120];
        snprintf(label, sizeof label, TN(need, "Convert %d video", "Convert %d videos"), need);
        ui_act(b, "media.convert", "btn primary", label, NULL, NULL, NULL);
    }
    buf_puts(b, "</div>");

    /* originals their copies can stand in for: a button for those there, a setting for the next */
    buf_puts(b, "<div class=\"media-orig\">");
    if (extra) {
        char sz[24], q[400];
        gb_text(extra_bytes, sz, sizeof sz);
        buf_puts(b, "<p>");
        H(b,
          TN(extra,
             "%d video has two versions, the original and a converted copy. The less compatible one takes %s.",
             "%d videos have two versions, the original and a converted copy. The less compatible ones take %s."),
          extra, sz);
        buf_puts(b, "</p>");
        snprintf(q, sizeof q,
                 TN(extra,
                    "Delete %d file (%s)? For each video the version that plays in the most places stays, and "
                    "everyone's progress stays with it. This can't be undone.",
                    "Delete %d files (%s)? For each video the version that plays in the most places stays, and "
                    "everyone's progress stays with it. This can't be undone."),
                 extra, sz);
        ui_form_confirm(b, "media.drop_originals", "inline", q);
        H(b, "<button class=\"btn danger\">%s</button>", T("Keep only the most compatible version"));
        ui_form_end(b);
    }
    ui_form(b, "media.drop_auto", "inline");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> %s</label>",
      setting_int("media.drop_originals", 0) == 1 ? " checked" : "", T("Do that by itself after each conversion"));
    ui_form_end(b);
    H(b, "<p class=\"muted media-small\">%r</p></div>",
      T("A copy keeps the picture and one sound track: other sound tracks (a director's commentary, other languages) "
        "go with the original."));
    ui_card_end(b);
}

/* ============================================== HEVC made H.264 (2.6) */

static void hidden_fields(Buf *b, const char *what) { /* "a=1&b=2" as hidden inputs */
    for (const char *p = what; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t n = amp ? (size_t)(amp - p) : strlen(p);
        char kv[120];
        if (n && n < sizeof kv) {
            memcpy(kv, p, n), kv[n] = '\0';
            char *eq = strchr(kv, '=');
            if (eq) *eq = '\0', H(b, "<input type=\"hidden\" name=\"%s\" value=\"%s\">", kv, eq + 1);
        }
        p = amp ? amp + 1 : NULL;
    }
}

void convert_h264_buttons(Ctx *c, Buf *b, const char *what, int count) {
    if (!is_admin(c) || count <= 0) return;
    buf_puts(b, "<span class=\"media-h264-go\">");
    ui_form(b, "media.convert", "inline");
    buf_puts(b, "<input type=\"hidden\" name=\"h264\" value=\"1\">");
    hidden_fields(b, what);
    H(b, "<button class=\"btn\">%s</button>", T("On Hemma's computer"));
    ui_form_end(b);
    if (upload_can_convert()) { /* (wire.js: the browser's converter, its copies sent like an upload's) */
        buf_puts(b, "<form class=\"inline media-h264-dev\" data-upload=\"/media/upload\" data-here=\"1\"");
        upload_converter_attrs(c, b);
        H(b, "><input type=\"hidden\" name=\"here\" value=\"%s\"><input type=\"hidden\" name=\"conv\" value=\"1\">%r"
             "<button class=\"btn primary\">%s</button></form>",
          what, c->me && upload_ram_chosen(c->me->id) ? "<input type=\"hidden\" name=\"ram\" value=\"1\">" : "",
          T("On this device")); /* (copies in memory, or in Blobs, as the upload form's box says) */
    }
    buf_puts(b, "</span>");
}

/* ---- the power tiers' card and action (2.7) ---- */

#define PREF_POWER_HERE "media.power_here"
static const char *const POWER_LABEL[] = {N_("Low power"), N_("Medium power"), N_("Full throttle")};

int convert_device_tier(Ctx *c) { return c->me ? convert_power_of(pref(c->me->id, PREF_POWER_HERE, "")) : CONVERT_MEDIUM; }

static int g_test_cores; /* (tests: every device has this many cores - convert_test_device_cores) */
void convert_test_device_cores(int cores) { g_test_cores = cores; }

int convert_device_workers(Ctx *c) {
    bool phone = false;
    int cores = !c->me ? 0 : g_test_cores ? g_test_cores : media_device_cores(c, &phone, NULL, 0);
    return cores > 0 ? convert_power_workers(convert_power_of(pref(c->me->id, PREF_POWER_HERE, "")), cores, phone) : 0;
}

/* one machine's three choices, each saying what it means there: how many cores, how it goes about it */
static void power_choices(Buf *b, const char *where, int tier, int cores, bool phone) {
    ui_form(b, "media.power", "stack media-power");
    ui_hidden(b, "where", where);
    for (int t = CONVERT_LOW; t <= CONVERT_FULL; t++) {
        int n = convert_power_workers(t, cores, phone);
        H(b, "<label class=\"check media-power-opt\"><input type=\"radio\" name=\"tier\" value=\"%s\" data-auto%r>"
             "<span><b>%s</b><small class=\"muted\">",
          power_name(t), t == tier ? " checked" : "", T(POWER_LABEL[t]));
        if (cores <= 0) /* (a device that hasn't said yet) */
            H(b, "%s",
              t == CONVERT_LOW      ? T("About a third of this device's cores, one video each.")
              : t == CONVERT_MEDIUM ? T("About two thirds of its cores, one video each. The default.")
                                    : T("All of its cores, a video split into parts when fewer are left than cores."));
        else if (phone)
            H(b, TN(n, "%d video at a time: a phone gets warm, and it's its battery.",
                    "%d videos at a time: a phone gets warm, and it's its battery."),
              n);
        else if (t == CONVERT_LOW)
            H(b, TN(cores, "%d of its %d core, one video each: the rest stays free for everything else.",
                    "%d of its %d cores, one video each: the rest stays free for everything else."),
              n, cores);
        else if (t == CONVERT_MEDIUM)
            H(b, TN(cores, "%d of its %d core, one video each. The default.", "%d of its %d cores, one video each. The default."),
              n, cores);
        else
            H(b, TN(cores,
                    "All %d core. When fewer videos are left than that, one being re-encoded is split into parts made side "
                    "by side, so none waits. The computer is busy - and warm - while it works.",
                    "All %d cores. When fewer videos are left than that, one being re-encoded is split into parts made side "
                    "by side, so none waits. The computer is busy - and warm - while it works."),
              cores);
        buf_puts(b, "</small></span></label>");
    }
    H(b, "<noscript><button class=\"btn\">%s</button></noscript>", T("Save"));
    ui_form_end(b);
}

void convert_power_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    ui_card(b, T("How hard to work"), NULL, NULL);
    H(b, "<p class=\"muted media-small\">%s</p>",
      T("Converting is heavy work. Choose how much of each computer it may take: Hemma's own, and the one you're using "
        "now - for On this device and the copies your uploads make."));
    int cores = plat_cpu_count();
    buf_puts(b, "<h3 class=\"media-power-h\">");
    H(b, TN(cores, "Hemma's computer · %d core", "Hemma's computer · %d cores"), cores);
    buf_puts(b, "</h3>");
    power_choices(b, "server", convert_power_of(setting("media.power", "")), cores, false);
    if (upload_can_convert() && c->me) { /* (the browser's converter is built in) */
        bool phone = false;
        char agent[64];
        int dc = media_device_cores(c, &phone, agent, sizeof agent);
        Buf name = {0};
        i18n_unpack(&name, agent);
        buf_puts(b, "<h3 class=\"media-power-h\">");
        if (dc) H(b, TN(dc, "This device · %s · %d core", "This device · %s · %d cores"), buf_cstr(&name), dc);
        else H(b, T("This device · %s"), buf_cstr(&name));
        buf_puts(b, "</h3>");
        buf_free(&name);
        power_choices(b, "here", convert_power_of(pref(c->me->id, PREF_POWER_HERE, "")), dc, phone);
    }
    ui_card_end(b);
}

/* media.power where=server|here tier=low|medium|full (admins: they're the ones who convert) */
void convert_act_power(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can convert videos"));
        return;
    }
    int tier = convert_power_of(fields_str(f, "tier"));
    if (str_eq(fields_str(f, "where"), "here")) {
        pref_set(c->me->id, PREF_POWER_HERE, power_name(tier));
        ctx_ok(c, T("This device: %s"), T(POWER_LABEL[tier]));
        return;
    }
    setting_set("media.power", power_name(tier));
    atomic_store(&g_power, tier);
    spawn_workers(); /* (a higher tier: more start now; a lower one: those over it stop after their video) */
    log_info("convert: %s chose %s for Hemma's computer (%d at a time)", c->me->name, power_name(tier), workers_wanted());
    ctx_ok(c, T("Hemma's computer: %s"), T(POWER_LABEL[tier]));
}

void convert_h264_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    const MediaIndex *ix = media_lib();
    int films = 0, total = 0, *per = xcalloc((size_t)ix->nseries + 1, sizeof *per);
    for (int k = 0; k < ix->nmovies; k++) films += convert_h264_need(&ix->files[ix->movies[k]]);
    total = films;
    for (int s = 0; s < ix->nseries; s++) {
        const MediaSeries *se = &ix->series[s];
        for (int k = se->first; k < se->first + se->count; k++) per[s] += convert_h264_need(&ix->files[ix->episodes[k]]);
        total += per[s];
    }
    for (int k = 0; k < ix->nothers; k++) total += convert_h264_need(&ix->files[ix->others[k]]);
    if (!total) { /* (what this device is making counts as done: its card stays while it works) */
        free(per);
        Buf run = {0};
        if (upload_here_card(c, &run, 0)) {
            ui_card(b, T("Play everywhere from HEVC"), NULL, NULL);
            buf_put(b, run.p, run.len);
            ui_card_end(b);
        }
        buf_free(&run);
        return;
    }
    ui_card(b, T("Play everywhere from HEVC"), NULL, NULL);
    upload_here_card(c, b, 0);
    buf_puts(b, "<p>");
    H(b,
      TN(total,
         "%d video is HEVC (x265): it plays on Apple devices, newer TVs and phones, and in browsers with HEVC hardware - "
         "not everywhere.",
         "%d videos are HEVC (x265): they play on Apple devices, newer TVs and phones, and in browsers with HEVC hardware - "
         "not everywhere."),
      total);
    buf_puts(b, "</p><p class=\"muted media-small\">");
    H(b, "%s ",
      T("Hemma can make an H.264 copy of each, which plays everywhere. It takes the place of the HEVC copy Hemma made "
        "(an original stays where it is), and it's about as big, or a little bigger."));
    int cores = plat_cpu_count();
    if (cores >= 8)
        H(b, T("It's heavy work: Hemma's computer (%d cores) can do it, a few at a time - or open this page on a strong "
               "computer and choose On this device."),
          cores);
    else
        H(b, T("It's heavy work: on Hemma's computer (%d cores) one episode can take hours. A laptop or desktop is much "
               "faster: open this page there and choose On this device - and keep the page open while it works."),
          cores);
    buf_puts(b, "</p><ul class=\"rows media-h264\">");
    char what[64];
    for (int s = 0; s < ix->nseries; s++) {
        if (!per[s]) continue;
        const MediaSeries *se = &ix->series[s];
        H(b, "<li class=\"row\" id=\"media-h264-%X\"><a class=\"grow\" href=\"/media/series/%X?manage=1\">%s</a>"
             "<span class=\"muted\">%d</span>",
          se->id, se->id, se->title, per[s]);
        snprintf(what, sizeof what, "series=%016llx", (unsigned long long)se->id);
        convert_h264_buttons(c, b, what, per[s]);
        buf_puts(b, "</li>");
    }
    if (films) {
        H(b, "<li class=\"row\" id=\"media-h264-films\"><span class=\"grow\">%s</span><span class=\"muted\">%d</span>",
          T("Movies"), films);
        convert_h264_buttons(c, b, "films=1", films);
        buf_puts(b, "</li>");
    }
    H(b, "<li class=\"row\" id=\"media-h264-all\"><b class=\"grow\">%s</b><span class=\"muted\">%d</span>", T("All of them"), total);
    convert_h264_buttons(c, b, "", total);
    buf_puts(b, "</li></ul>");
    ui_card_end(b);
    free(per);
}
