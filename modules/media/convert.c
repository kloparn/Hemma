/* modules/media/convert.c — "play everywhere": convert a video once into an MP4
 * that every browser, iPhone, iPad, TV and Chromecast plays.
 *
 * The conversion itself is av/conv.c (MPEG-2 -> H.264, AC-3 -> AAC, H.264 and
 * AAC copied) - the same code the uploading browser runs as WebAssembly. Here:
 * files. The result is "<name>.web.mp4" next to the original, written as ".part"
 * and renamed when complete; the library pairs the two and streams the copy
 * (library.c), so every player - the watch page, TV channels - plays it.
 * The queue below runs one file per worker thread, at background priority, with
 * progress on the library settings page. */
#include "../../core/ui.h"
#include "../av/conv.h"
#include "convert.h"
#include "remove.h"

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

/* ================================================================= pipeline */

typedef struct {
    FILE *f;
    int64_t end;
} Dst;

static bool src_read(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

static bool dst_write(void *ctx, const void *p, size_t n) {
    Dst *d = ctx;
    if (n && fwrite(p, 1, n, d->f) != n) return false;
    d->end += (int64_t)n;
    return true;
}

static bool dst_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    Dst *d = ctx;
    bool ok = file_seek(d->f, off) && (!n || fwrite(p, 1, n, d->f) == n);
    return file_seek(d->f, d->end) && ok;
}

static void on_progress(void *ctx, int permille) { ((ConvertJob *)ctx)->permille = permille; }

bool convert_file(ConvertJob *j) {
    j->err[0] = j->what[0] = '\0';
    j->permille = 0;
    char part[1100];
    snprintf(part, sizeof part, "%s.part", j->dst);
    int64_t size = 0;
    FILE *src = file_open(j->src, "rb");
    if (!src || !path_stat(j->src, &size, NULL, NULL)) {
        if (src) fclose(src);
        str_copy(j->err, sizeof j->err, "Can't open the original");
        return false;
    }
    Dst dst = {file_open(part, "wb"), 0};
    if (!dst.f) {
        fclose(src);
        str_copy(j->err, sizeof j->err, "Can't create the new file next to the original");
        return false;
    }
    AvConvJob av = {.in = {src, size, src_read},
                    .out = {&dst, dst_write, dst_write_at},
                    .quality = j->quality,
                    .audio_kbps = j->audio_kbps,
                    .limit_ns = j->limit_ns,
                    .stop = j->stop,
                    .progress = on_progress,
                    .progress_ctx = j};
    bool ok = av_convert(&av);
    str_copy(j->what, sizeof j->what, av.what);
    if (!ok) str_copy(j->err, sizeof j->err, av.err);
    if ((fflush(dst.f) != 0 || !file_sync(dst.f)) && ok) {
        str_copy(j->err, sizeof j->err, "Couldn't save the new file");
        ok = false;
    }
    fclose(dst.f);
    fclose(src);
    if (ok && !file_replace(part, j->dst)) {
        str_copy(j->err, sizeof j->err, "Couldn't rename the finished file");
        ok = false;
    }
    if (!ok) file_remove(part);
    else {
        j->out_bytes = av.out_bytes;
        j->permille = 1000;
    }
    return ok;
}

/* ==================================================================== queue */

enum { CQ_WAITING, CQ_RUNNING, CQ_DONE, CQ_FAILED };

typedef struct {
    uint64_t id;
    char name[160];
    int state;
    ConvertJob job; /* a running worker writes job.permille / err / what; the rest only under g_cl */
    int64_t started, ended; /* unix seconds */
    bool settled; /* done, and its original dealt with ("delete each original once its copy is made") */
} ConvItem;

/* Items are allocated one by one so a worker's item never moves; only waiting
   and finished ones are ever freed. */
static Lock g_cl = LOCK_INIT;
static ConvItem **g_items;
static int g_nitems, g_capitems, g_running, g_done_seen;
static volatile int g_cstop;
static int64_t g_shown, g_rescan_at;

static void worker(void *arg) {
    (void)arg;
    thread_background(); /* the household's pages come first */
    for (;;) {
        lock_take(&g_cl);
        ConvItem *it = NULL;
        for (int i = 0; i < g_nitems && !g_cstop && !it; i++)
            if (g_items[i]->state == CQ_WAITING) it = g_items[i];
        if (!it) {
            g_running--;
            lock_give(&g_cl);
            return;
        }
        it->state = CQ_RUNNING;
        it->started = now_sec();
        it->job.stop = &g_cstop;
        lock_give(&g_cl);

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

static bool enqueue(const MediaFile *f) { /* g_cl held */
    char src[1024];
    if (convert_need(f) == CONV_NONE || !media_full_path(f, src, sizeof src)) return false;
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i]->id == f->id && g_items[i]->state <= CQ_RUNNING) return false;
    if (g_nitems == g_capitems) {
        g_capitems = g_capitems ? g_capitems * 2 : 64;
        g_items = xrealloc(g_items, (size_t)g_capitems * sizeof *g_items);
    }
    ConvItem *it = xcalloc(1, sizeof *it);
    g_items[g_nitems++] = it;
    it->id = f->id;
    it->state = CQ_WAITING;
    const MediaSeries *s = f->series ? media_series(f->series) : NULL;
    if (s && f->name.kind == MEDIA_EPISODE)
        snprintf(it->name, sizeof it->name, "%s S%02dE%02d", s->title, f->name.season, f->name.episode);
    else snprintf(it->name, sizeof it->name, "%s", f->name.title[0] ? f->name.title : f->rel);
    str_copy(it->job.src, sizeof it->job.src, src);
    convert_web_path(src, it->job.dst, sizeof it->job.dst);
    return true;
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

static int workers_wanted(void) {
    int n = plat_cpu_count() - 2; /* leave room for the household */
    return CLAMP(n, 1, MAX_WORKERS);
}

void convert_act_start(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can convert videos");
        return;
    }
    if (app_testing()) { /* no worker threads under tests */
        ctx_fail(c, "Not while testing");
        return;
    }
    const MediaIndex *ix = media_lib();
    uint64_t one = 0;
    bool single = media_parse_id(fields_str(f, "id"), &one);
    int added = 0;
    lock_take(&g_cl);
    if (!g_running) { /* a fresh run: forget how the last one went */
        drop_items(CQ_DONE);
        g_done_seen = 0;
    }
    g_cstop = 0;
    if (single) added += enqueue(media_file(one));
    else {
        for (int k = 0; k < ix->nepisodes; k++) added += enqueue(&ix->files[ix->episodes[k]]); /* series in order */
        for (int i = 0; i < ix->nfiles; i++)
            if (bulk_need(&ix->files[i])) added += enqueue(&ix->files[i]);
    }
    int waiting = 0;
    for (int i = 0; i < g_nitems; i++) waiting += g_items[i]->state == CQ_WAITING;
    int want = MIN(workers_wanted(), g_running + waiting);
    while (g_running < want) {
        g_running++;
        if (!thread_spawn(worker, NULL)) {
            g_running--;
            break;
        }
    }
    int at_once = g_running;
    lock_give(&g_cl);
    if (!added) ctx_info(c, single ? "That video plays everywhere already (or is on its way)" : "Nothing needs converting");
    else ctx_ok(c, "Converting %d video%s, %d at a time", added, added == 1 ? "" : "s", MAX(at_once, 1));
    app_wake_in(500);
}

void convert_act_stop(Ctx *c, Fields *f) {
    (void)f;
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can do that");
        return;
    }
    g_cstop = 1;
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
   the copy file itself, before the library has even seen it (the rescan comes after). */
static void settle_finished(void) {
    typedef struct {
        uint64_t id;
        char src[1024], dst[1024];
    } Done;
    bool drop = setting_int("media.drop_originals", 0) == 1;
    Done *todo = NULL;
    int n = 0;
    lock_take(&g_cl);
    for (int i = 0; i < g_nitems; i++) {
        ConvItem *it = g_items[i];
        if (it->state != CQ_DONE || it->settled) continue;
        it->settled = true;
        if (!drop) continue;
        todo = xrealloc(todo, (size_t)(n + 1) * sizeof *todo);
        todo[n].id = it->id;
        str_copy(todo[n].src, sizeof todo[n].src, it->job.src);
        str_copy(todo[n].dst, sizeof todo[n].dst, it->job.dst);
        n++;
    }
    lock_give(&g_cl);
    for (int i = 0; i < n; i++) convert_replace_original(todo[i].id, todo[i].src, todo[i].dst);
    free(todo);
}

void convert_tick(int64_t now) {
    settle_finished();
    int64_t sig = 0;
    int done = 0, running;
    lock_take(&g_cl);
    running = g_running;
    for (int i = 0; i < g_nitems; i++) {
        sig = sig * 31 + g_items[i]->state * 7 + g_items[i]->job.permille / 10; /* every 1% */
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
    g_cstop = 1;
    for (int waited = 0; waited < 8000; waited += 50) { /* let workers drop their .part files */
        lock_take(&g_cl);
        int n = g_running;
        lock_give(&g_cl);
        if (!n) break;
        plat_sleep_ms(50);
    }
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
    int waiting = 0, running = 0, done = 0, failed = 0;
    for (int i = 0; i < g_nitems; i++) {
        waiting += g_items[i]->state == CQ_WAITING;
        running += g_items[i]->state == CQ_RUNNING;
        done += g_items[i]->state == CQ_DONE;
        failed += g_items[i]->state == CQ_FAILED;
    }
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
              it->name, it->job.permille);
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
