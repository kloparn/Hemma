/* modules/media/convert.c — "play everywhere": convert a video once into an MP4
 * that every browser, iPhone, iPad, TV and Chromecast plays.
 *
 * The conversion itself is av/conv.c (MPEG-2 -> H.264, AC-3 -> AAC, H.264 and
 * AAC copied) - the same code the uploading browser runs as WebAssembly. Here:
 * files. The result is "<name>.web.mp4" next to the original, written as
 * "<name>.web.mp4.conv.part" (never an upload's resumable "<name>.web.mp4.part") and renamed
 * when complete - never over a file that is there; the library pairs the two and streams the
 * copy (library.c), so every player - the watch page, TV channels - plays it.
 * The queue below runs one file per worker thread, at background priority, with
 * progress on the library settings page. A file starts only when its original's size fits
 * on its disk next to what running uploads and conversions may still write (room.h), and
 * never while an upload is bringing the same copy. upload.c in turn leaves alone a copy being
 * made here (convert_making), and takes over one only queued (convert_give_way: it leaves the
 * queue - the upload brings it). A crash's leftover ".conv.part" goes at the next scan. */
#include "../../core/ui.h"
#include "../av/conv.h"
#include "convert.h"
#include "remove.h"
#include "room.h"

#define MAX_WORKERS 12

/* ===================================================================== what */

static bool container_is(const MediaProbe *p, const char *a, const char *b) {
    return str_eq(p->container, a) || (b && str_eq(p->container, b));
}

ConvertNeed convert_need(const MediaFile *f) {
    if (!f || f->type != MF_VIDEO || f->web || f->role == MF_WEBCOPY) return CONV_NONE;
    const MediaProbe *p = &f->probe;
    if (!container_is(p, "mkv", "webm")) return CONV_NONE; /* the demuxer we have */
    bool aac = !p->acodec[0] || str_eq(p->acodec, "aac"), ac3 = str_eq(p->acodec, "ac3");
    if (str_eq(p->vcodec, "mpeg2") && (aac || ac3)) return CONV_TRANSCODE;
    if (str_eq(p->vcodec, "h264") && ac3) return CONV_TRANSCODE; /* picture copied, sound re-encoded */
    if (str_eq(p->vcodec, "h264") && aac) return CONV_REMUX;
    /* HEVC stays HEVC (no decoder here): moved into an MP4, which Safari - iPhone, iPad, Mac - plays */
    if (str_eq(p->vcodec, "hevc") && ac3) return CONV_TRANSCODE;
    if (str_eq(p->vcodec, "hevc") && aac) return CONV_REMUX;
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

bool convert_file(ConvertJob *j) {
    j->err[0] = j->what[0] = '\0';
    atomic_store(&j->permille, 0);
    atomic_store(&j->out_mb, 0);
    char part[1100];
    convert_part_path(j->dst, part, sizeof part);
    if (path_stat(j->dst, NULL, NULL, NULL)) { /* made since it was queued (by hand?): that one stays */
        str_copy(j->err, sizeof j->err, "Its play-everywhere copy is there already");
        return false;
    }
    int64_t size = 0;
    Io io = {.src = file_open(j->src, "rb"), .j = j};
    if (!io.src || !path_stat(j->src, &size, NULL, NULL)) {
        if (io.src) fclose(io.src);
        str_copy(j->err, sizeof j->err, "Can't open the original");
        return false;
    }
    io.dst = file_open(part, "wb");
    if (!io.dst) {
        fclose(io.src);
        str_copy(j->err, sizeof j->err, "Can't create the new file next to the original");
        return false;
    }
    AvConvJob av = {.in = {&io, size, src_read},
                    .out = {&io, dst_write, dst_write_at},
                    .quality = j->quality,
                    .audio_kbps = j->audio_kbps,
                    .limit_ns = j->limit_ns,
                    .stop = &io.halt,
                    .progress = on_progress,
                    .progress_ctx = &io};
    bool ok = av_convert(&av);
    str_copy(j->what, sizeof j->what, av.what);
    if (!ok) str_copy(j->err, sizeof j->err, av.err);
    if (ok && (fflush(io.dst) != 0 || !file_sync(io.dst))) { /* (a stopped or failed one goes: no fsync) */
        str_copy(j->err, sizeof j->err, "Couldn't save the new file");
        ok = false;
    }
    fclose(io.dst);
    fclose(io.src);
    if (ok && path_stat(j->dst, NULL, NULL, NULL)) { /* never over a copy that arrived meanwhile */
        str_copy(j->err, sizeof j->err, "Another play-everywhere copy arrived meanwhile - that one stays");
        ok = false;
    }
    if (ok && !file_replace(part, j->dst)) {
        str_copy(j->err, sizeof j->err, "Couldn't rename the finished file");
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
    int64_t started, ended; /* unix seconds */
    bool settled; /* done, and its original dealt with ("delete each original once its copy is made") */
} ConvItem;

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
        out[n].bytes = MAX(it->need - ((int64_t)atomic_load(&it->job.out_mb) << 20), 0);
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
            free(it);
        } else g_items[w++] = it;
    }
    g_nitems = w;
    lock_give(&g_cl);
    return making;
}

void convert_drop_leftover(const char *path) {
    char part[1100];
    bool mine = false;
    lock_take(&g_cl); /* (held while deleting: nothing can be queued for it in between) */
    for (int i = 0; i < g_nitems && !mine; i++) {
        if (g_items[i]->state > CQ_RUNNING) continue;
        convert_part_path(g_items[i]->job.dst, part, sizeof part);
        mine = str_ieq(part, path);
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

/* "needs up to 1.0 GB, and the drive has 400 MB to spare (Hemma keeps 1 GB free)" */
static void set_no_room(ConvItem *it, const Want *w) { /* g_cl held */
    char need[24], tail[200];
    media_size_text(w->need, need, sizeof need);
    media_room_text(tail, sizeof tail, w->spare, w->held);
    snprintf(it->job.err, sizeof it->job.err, "needs up to %s, and %s", need, tail);
    it->state = CQ_NOROOM;
    it->ended = now_sec();
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
static ConvItem *pick(void) {
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
        if (it || !again) return it;
        for (int t = 0; fits < 0 && t < 2000 && !atomic_load(&g_cstop); t += 100) plat_sleep_ms(100);
    }
}

static void worker(void *arg) {
    (void)arg;
    thread_background(); /* the household's pages come first */
    for (ConvItem *it; (it = pick()) != NULL;) {
        log_info("convert: %s", it->job.src);
        bool ok = convert_file(&it->job);
        if (ok) log_info("convert: done %s (%s, %lld MB)", it->job.dst, it->job.what, (long long)(it->job.out_bytes >> 20));
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

typedef enum { EQ_ADDED, EQ_NO, EQ_ON_ITS_WAY, EQ_THERE, EQ_UPLOAD, EQ_N } Enq;

/* Event loop (so is everything that claims a file: upload plans and PUTs, this - one can't
   slip in between a check and a claim). The copy mustn't be there already (made since the
   last scan) nor be coming by upload; then the video joins the queue once. */
static Enq enqueue(const MediaFile *f) {
    char src[1024], dst[1024];
    if (!f || convert_need(f) == CONV_NONE || !media_full_path(f, src, sizeof src)) return EQ_NO;
    convert_web_path(src, dst, sizeof dst);
    if (path_stat(dst, NULL, NULL, NULL)) return EQ_THERE;
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
            if (it->id == f->id && it->state >= CQ_FAILED) free(it);
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
        it->need = f->size;
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
        if (drop) free(it);
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
        if (!out++) snprintf(why, cap, "%s %s", it->name, it->job.err);
    }
    lock_give(&g_cl);
    free(w);
    return out;
}

static int workers_wanted(void) {
    int n = plat_cpu_count() - 2; /* leave room for the household */
    return CLAMP(n, 1, MAX_WORKERS);
}

static void settle_finished(void);

void convert_act_start(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can convert videos");
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
    if (single) got[enqueue(media_file(one))]++;
    else {
        bool *ep = xcalloc((size_t)ix->nfiles + 1, sizeof *ep);
        for (int k = 0; k < ix->nepisodes; k++) { /* series in order */
            ep[ix->episodes[k]] = true;
            got[enqueue(&ix->files[ix->episodes[k]])]++;
        }
        for (int i = 0; i < ix->nfiles; i++)
            if (!ep[i] && bulk_need(&ix->files[i])) got[enqueue(&ix->files[i])]++;
        free(ep);
    }
    int added = got[EQ_ADDED], noroom = 0;
    char why[300] = "";
    if (idle && added) noroom = set_aside(why, sizeof why);

    lock_take(&g_cl);
    int want = MIN(workers_wanted(), g_running + count_state(CQ_WAITING));
    while (!app_testing() && g_running < want) { /* (tests start them with convert_test_start) */
        g_running++;
        if (!thread_spawn(worker, NULL)) {
            g_running--;
            break;
        }
    }
    int at_once = MAX(g_running, 1);
    lock_give(&g_cl);

    if (noroom && noroom == added) {
        if (noroom == 1) ctx_fail(c, "Not enough room on the drive: %s", why);
        else ctx_fail(c, "Not enough room on the drive for %d videos: %s", noroom, why);
    } else if (added) {
        char also[64] = "";
        if (noroom) snprintf(also, sizeof also, " - %d more %s on the drive", noroom, noroom == 1 ? "doesn't fit" : "don't fit");
        ctx_ok(c, "Converting %d video%s, %d at a time%s", added - noroom, added - noroom == 1 ? "" : "s", at_once, also);
    } else if (single && got[EQ_UPLOAD]) {
        ctx_info(c, "An upload is bringing this video's play-everywhere copy right now");
    } else if (single && got[EQ_THERE]) {
        ctx_info(c, "Its play-everywhere copy is there already - the library shows it in a moment");
        media_scan_start();
    } else if (single) {
        ctx_info(c, "That video plays everywhere already (or is on its way)");
    } else {
        ctx_info(c, got[EQ_UPLOAD] ? "Nothing to convert - the rest is being uploaded with copies right now"
                                   : "Nothing needs converting");
    }
    app_wake_in(500);
}

void convert_act_stop(Ctx *c, Fields *f) {
    (void)f;
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can do that");
        return;
    }
    atomic_store(&g_cstop, 1);
    lock_take(&g_cl);
    drop_items(CQ_WAITING);
    lock_give(&g_cl);
    ctx_ok(c, "Stopping - finished videos stay, the one in progress is dropped");
}

bool convert_replace_original(uint64_t id, const char *src, const char *dst) {
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
    settle_finished();
    uint64_t sig = 0; /* (unsigned: it wraps, by design) */
    int done = 0, running;
    lock_take(&g_cl);
    running = g_running;
    for (int i = 0; i < g_nitems; i++) {
        sig = sig * 31 + (uint64_t)g_items[i]->state * 7 + (uint64_t)atomic_load(&g_items[i]->job.permille) / 10; /* every 1% */
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
    for (int i = 0; i < g_nitems; i++) free(g_items[i]);
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
    ConvItem *it = pick(); /* (NULL: it counted the "worker" out) */
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
    ui_card(b, "Play everywhere", NULL, NULL);
    if (waiting + running) {
        H(b, "<p><b>Converting · %d of %d done</b></p><ul class=\"rows media-conv\">", done, done + waiting + running + failed);
        for (int i = 0; i < g_nitems; i++) {
            const ConvItem *it = g_items[i];
            if (it->state != CQ_RUNNING) continue;
            H(b, "<li class=\"row\"><span class=\"grow\">%s</span><progress class=\"media-up-bar\" max=\"1000\" value=\"%d\">"
                 "</progress></li>",
              it->name, atomic_load(&it->job.permille));
        }
        buf_puts(b, "</ul>");
    } else if (need) {
        char sz[24];
        double gb = (double)bytes / 1073741824.0;
        snprintf(sz, sizeof sz, gb >= 10 ? "%.0f GB" : "%.1f GB", gb);
        H(b, "<p>%d video%s (%s) %s in a format that browsers, iPhones or TVs can't all play.</p>", need,
          need == 1 ? "" : "s", sz, need == 1 ? "is" : "are");
        H(b, "<p class=\"muted media-small\">Hemma can make a copy of each that plays everywhere - once, next to the "
             "original (<code class=\"media-code\">name.web.mp4</code>). %s It runs in the background, %d at a time.</p>",
          remux == need ? "These only need a new container: quick, nothing is re-encoded."
                        : "DVD video is re-encoded to H.264 and Dolby sound to AAC; copies are about a third of the size.",
          workers_wanted());
    }
    if (done || failed) {
        H(b, "<p class=\"muted media-small\">%d converted", done);
        if (failed) H(b, ", %d failed", failed);
        buf_puts(b, ".</p>");
        if (failed) {
            buf_puts(b, "<ul class=\"media-up-list\">");
            for (int i = 0; i < g_nitems; i++)
                if (g_items[i]->state == CQ_FAILED)
                    H(b, "<li class=\"bad\">%s — %s</li>", g_items[i]->name, g_items[i]->job.err);
            buf_puts(b, "</ul>");
        }
    }
    if (noroom) { /* one line for all of them: they didn't fit for the same reason */
        const ConvItem *first = NULL;
        for (int i = 0; i < g_nitems && !first; i++)
            if (g_items[i]->state == CQ_NOROOM) first = g_items[i];
        H(b, "<p class=\"notice bad\" id=\"media-conv-room\">%d video%s didn't fit on the drive: %s %s. Make room, then "
             "convert again.</p>",
          noroom, noroom == 1 ? "" : "s", first->name, first->job.err);
    }
    lock_give(&g_cl);
    buf_puts(b, "<div class=\"modal-f\">");
    if (waiting + running) ui_act(b, "media.convert_stop", "btn", "Stop", NULL, NULL, NULL);
    else if (need) {
        char label[64];
        snprintf(label, sizeof label, "Convert %d video%s", need, need == 1 ? "" : "s");
        ui_act(b, "media.convert", "btn primary", label, NULL, NULL, NULL);
    }
    buf_puts(b, "</div>");

    /* originals their copies can stand in for: a button for those there, a setting for the next */
    buf_puts(b, "<div class=\"media-orig\">");
    if (extra) {
        char sz[24], q[240];
        double gb = (double)extra_bytes / 1073741824.0;
        snprintf(sz, sizeof sz, gb >= 10 ? "%.0f GB" : "%.1f GB", gb);
        H(b, "<p>%d video%s %s two versions, the original and a converted copy. The less compatible one%s "
             "take%s %s.</p>",
          extra, extra == 1 ? "" : "s", extra == 1 ? "has" : "have", extra == 1 ? "" : "s", extra == 1 ? "s" : "", sz);
        snprintf(q, sizeof q, "Delete %d file%s (%s)? For each video the version that plays in the most places "
                              "stays, and everyone's progress stays with it. This can't be undone.",
                 extra, extra == 1 ? "" : "s", sz);
        ui_form_confirm(b, "media.drop_originals", "inline", q);
        buf_puts(b, "<button class=\"btn danger\">Keep only the most compatible version</button>");
        ui_form_end(b);
    }
    ui_form(b, "media.drop_auto", "inline");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> Do that by itself "
         "after each conversion</label>",
      setting_int("media.drop_originals", 0) == 1 ? " checked" : "");
    ui_form_end(b);
    buf_puts(b, "<p class=\"muted media-small\">A copy keeps the picture and one sound track: other sound tracks (a "
                "director's commentary, other languages) go with the original.</p></div>");
    ui_card_end(b);
}
