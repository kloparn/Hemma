/* modules/media/subtake.c — the subtitles inside the library's MKVs, taken out once, by themselves (1.2).
 *
 * A video converted since 0.24 has its subtitle tracks beside it as files (convert.c). One never converted
 * - it plays as it is, or the converter refuses it (AV1, DTS, Vorbis) - or converted before then keeps them
 * locked inside, where the player can't offer them. This job reads every MKV of the library once, in the
 * background, and saves its subtitle tracks next to it the way convert.c does (contract C1: x.en.srt,
 * x.en.sup, x.en.idx + x.en.sub - never over a file there), so the player offers them and other players
 * (VLC, Plex) find them too.
 *
 * Never in the way: one file at a time on one thread at background priority, and only while nobody is
 * watching (stream.c; not for WATCH_QUIET_MS after the last stream either), no conversion is queued or
 * running (convert.c), no library folder is away (an unplugged disk: library.c) and the setting is on
 * (SUBTAKE_SETTING, admins, Library settings). A file being read when that changes is dropped half-way
 * and read again from its start later. Reading is the cost: a whole file once (a Pi's ~1,000 MKVs, 300 GB
 * on a USB disk: hours - once). A file holding no subtitle track the converter takes, or whose every track
 * is beside it already, is done after its headers.
 *
 * What's done is remembered in the raw table media_subtake (db.h): the file's path below its library
 * folder (a disk mounted elsewhere isn't read again), its size and mtime (a changed file is read again),
 * how many tracks it holds and how many were saved. The table has a format number of its own (setting
 * media.subtake_format, as library.c's cache): a new one reads everything again. The library rescans when
 * a run ends and every RESCAN_MS while one goes, so the new subtitles show up. Words (issue #46): the card
 * is made when it's shown; the thread and the log speak English. */
#include "../../core/db.h"
#include "../../core/ui.h"
#include "../av/conv.h"
#include "../av/mkv.h"
#include "convert.h"
#include "download.h"
#include "library.h"
#include "subtake.h"

#include <stdatomic.h>

#define SUBTAKE_FORMAT 1          /* raise it when what's taken changes: every file is read again */
#define WATCH_QUIET_MS 60000      /* nobody has watched for this long before reading goes on */
#define START_AFTER_MS 60000      /* a start (a Pi booting) settles first */
#define RESCAN_MS (5 * 60 * 1000) /* while a run goes, new subtitles join the library this often at most */

#define SCHEMA                                                                                                     \
    "CREATE TABLE IF NOT EXISTS media_subtake (rel TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL, " \
    "tracks INTEGER NOT NULL DEFAULT 0, saved INTEGER NOT NULL DEFAULT 0, at INTEGER NOT NULL DEFAULT 0)"

/* why it isn't reading now (the card says it) */
enum { WAIT_NONE, WAIT_OFF, WAIT_WATCH, WAIT_CONVERT, WAIT_AWAY, WAIT_START };

typedef struct {
    char path[1024], rel[400];
    int64_t size, mtime;
} Todo;

typedef struct {
    char rel[400];
    int64_t size, mtime;
    int tracks, saved;
} Done;

/* What's done, in memory (event loop): by hash64 of rel, sorted */
typedef struct {
    uint64_t h;
    int64_t size, mtime;
    int tracks;
} Mark;

static Mark *g_marks;
static int g_nmarks, g_capmarks;

/* the worker: one at a time */
typedef struct {
    Todo *todo;
    int n;
} Run;

static Lock g_lock = LOCK_INIT;
static Done *g_done; /* results for the tick (g_lock) */
static int g_ndone, g_capdone;
static atomic_bool g_running;  /* a worker thread is out */
static atomic_bool g_finished; /* ... and it went through its whole list (not stopped) */
static atomic_int g_halt;      /* stop soon: someone watches, a conversion, switched off */
static atomic_int g_quit;      /* Hemma is stopping */
static atomic_bool g_saw_watch; /* the worker saw someone watching (the tick takes it: g_last_watch) */
static atomic_int g_reads, g_opens, g_trip;

/* the tick's view */
static unsigned g_gen = (unsigned)-1, g_idle_gen = (unsigned)-1; /* the index counted / a run went through */
static int g_total, g_checked, g_found;
static int g_wait;
static int64_t g_started, g_last_watch, g_dirty_at;
static uint64_t g_shown;

/* ================================================================== what's done */

static int mark_find(uint64_t h) { /* its index, or -(where it goes) - 1 */
    int lo = 0, hi = g_nmarks - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_marks[mid].h == h) return mid;
        if (g_marks[mid].h < h) lo = mid + 1;
        else hi = mid - 1;
    }
    return -lo - 1;
}

static void mark_set(const char *rel, int64_t size, int64_t mtime, int tracks) {
    uint64_t h = hash64_str(rel);
    int i = mark_find(h);
    if (i < 0) {
        i = -i - 1;
        if (g_nmarks == g_capmarks) {
            g_capmarks = g_capmarks ? g_capmarks * 2 : 256;
            g_marks = xrealloc(g_marks, (size_t)g_capmarks * sizeof *g_marks);
        }
        memmove(g_marks + i + 1, g_marks + i, (size_t)(g_nmarks - i) * sizeof *g_marks);
        g_nmarks++;
    }
    g_marks[i] = (Mark){h, size, mtime, tracks};
}

/* done, as the file is now: its row (NULL = not, or it changed since) */
static const Mark *mark_of(const MediaFile *f) {
    int i = mark_find(hash64_str(f->rel));
    return i >= 0 && g_marks[i].size == f->size && g_marks[i].mtime == f->mtime ? &g_marks[i] : NULL;
}

/* An MKV of the library: originals and the household's own, never a disc's "play all" (every episode again,
   or just its first MB) */
static bool eligible(const MediaFile *f) {
    return f->type == MF_VIDEO && f->role != MF_PLAYALL && f->role != MF_WEBCOPY &&
           (str_iends(f->rel, ".mkv") || str_iends(f->rel, ".webm"));
}

static void recount(void) {
    const MediaIndex *ix = media_lib();
    g_gen = media_lib_gen();
    g_total = g_checked = g_found = 0;
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        if (!eligible(f)) continue;
        g_total++;
        const Mark *m = mark_of(f);
        if (!m) continue;
        g_checked++;
        g_found += m->tracks > 0;
    }
}

static void counted(void) {
    if (g_gen != media_lib_gen()) recount();
}

/* The files still to read, newest first (what the household added lately is what it watches next) */
static Todo *todo_list(int *n, int max) {
    const MediaIndex *ix = media_lib();
    Todo *t = xcalloc((size_t)(g_total - g_checked > 0 ? MIN(g_total - g_checked, max) : 1), sizeof *t);
    bool *seen = xcalloc((size_t)ix->nfiles + 1, sizeof *seen);
    *n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < (pass ? ix->nfiles : ix->nrecent) && *n < max && *n < g_total - g_checked; k++) {
            int i = pass ? k : ix->recent[k];
            const MediaFile *f = &ix->files[i];
            if (seen[i] || !eligible(f) || mark_of(f)) continue;
            seen[i] = true;
            Todo *d = &t[*n];
            if (!media_full_path(f, d->path, sizeof d->path)) continue;
            str_copy(d->rel, sizeof d->rel, f->rel);
            d->size = f->size;
            d->mtime = f->mtime;
            (*n)++;
        }
    free(seen);
    return t;
}

/* =================================================================== one file */

typedef struct {
    FILE *f;
    volatile int halt; /* what av_subtitles watches */
    bool whole;        /* reading the whole file (av_subtitles), not just its headers */
    int reads;         /* ... reads so far (tests: subtake_test_halt_after) */
} Io;

/* Stop now? (someone started watching - the tick hears of it too, for WATCH_QUIET_MS - the tick says so,
   Hemma stops) */
static bool must_stop(void) {
    if (media_watching_active() > 0) atomic_store(&g_saw_watch, true);
    return atomic_load(&g_halt) || atomic_load(&g_quit) || atomic_load(&g_saw_watch);
}

static bool io_read(void *ctx, int64_t off, void *dst, size_t n) {
    Io *io = ctx;
    int trip = atomic_load(&g_trip);
    if (must_stop() || (trip && io->whole && ++io->reads > trip)) io->halt = 1;
    return !io->halt && file_pread(io->f, off, dst, n);
}

static void on_progress(void *ctx, int permille) {
    (void)permille;
    Io *io = ctx;
    if (must_stop()) io->halt = 1;
}

static void put_done(const Todo *t, int tracks, int saved) {
    lock_take(&g_lock);
    if (g_ndone == g_capdone) {
        g_capdone = g_capdone ? g_capdone * 2 : 16;
        g_done = xrealloc(g_done, (size_t)g_capdone * sizeof *g_done);
    }
    Done *d = &g_done[g_ndone++];
    str_copy(d->rel, sizeof d->rel, t->rel);
    d->size = t->size;
    d->mtime = t->mtime;
    d->tracks = tracks;
    d->saved = saved;
    lock_give(&g_lock);
}

/* A track the converter takes (as av/conv.c chooses them: listed in AV_CONV_CODECS, not compressed) */
static bool taken(const MkvTrack *t) {
    const AvCodec *k = av_conv_codec(t->codec);
    return t->type == MKV_SUBTITLE && !t->compressed && k && k->type == MKV_SUBTITLE;
}

/* Is the file convert_save_sub would make of track t already beside the video? (numbered as it numbers them) */
static bool beside(const char *video, const MkvTrack *t, ConvertSubs *seen) {
    MediaSubTags tags;
    media_sub_track_tags(t->lang, t->name, t->forced, &tags);
    char key[40], path[1100];
    media_sub_key(&tags, key, sizeof key);
    int n = 1;
    for (int i = 0; i < seen->n; i++) n += str_eq(seen->keys[i], key);
    if (seen->n < AV_CONV_MAX_SUBS) str_copy(seen->keys[seen->n++], sizeof seen->keys[0], key);
    const AvCodec *k = av_conv_codec(t->codec);
    const char *ext = k->how == AV_TEXT ? "srt" : str_eq(k->id, "S_HDMV/PGS") ? "sup" : "idx";
    if (!media_sub_sidecar(video, t->lang, t->name, t->forced, ext, n, path, sizeof path)) return true; /* (can't be) */
    if (!path_stat(path, NULL, NULL, NULL)) return false;
    if (!str_eq(ext, "idx")) return true;
    memcpy(path + strlen(path) - 3, "sub", 3);
    return path_stat(path, NULL, NULL, NULL);
}

/* The file as the scan saw it (not gone, not changed since) */
static bool as_scanned(const Todo *t) {
    int64_t size = 0, mtime = 0;
    bool dir = false;
    return path_stat(t->path, &size, &mtime, &dir) && !dir && size == t->size && mtime == t->mtime;
}

enum { TAKE_DONE, TAKE_LATER, TAKE_CLOSED }; /* done / stopped, gone or changed: read again / can't be opened */

/* Read one file's subtitles out and save them beside it; the result goes to the tick. A file stopped
   half-way (someone started watching ...), gone or changed since the scan gives none: it's read again. One
   that can't be read as an MKV is done (with nothing) - unless it has gone meanwhile (a disk unplugged). */
static int take_one(const Todo *t) {
    if (!as_scanned(t)) return TAKE_LATER;
    Io io = {.f = file_open(t->path, "rb")};
    if (!io.f) return TAKE_CLOSED;
    MkvReader rd = {&io, t->size, io_read};
    char err[160] = "";
    atomic_fetch_add(&g_opens, 1);
    Mkv *m = mkv_open(&rd, err, sizeof err);
    if (io.halt) {
        if (m) mkv_close(m);
        fclose(io.f);
        return TAKE_LATER;
    }
    if (!m) { /* not Matroska after all (a name only): nothing to take, ever */
        fclose(io.f);
        if (!as_scanned(t)) return TAKE_LATER;
        log_info("media: subtitles: %s isn't a readable MKV (%s)", t->rel, err);
        put_done(t, 0, 0);
        return TAKE_DONE;
    }
    int tracks = 0;
    bool all_there = true;
    ConvertSubs seen = {0};
    for (int i = 0; i < mkv_track_count(m) && tracks < AV_CONV_MAX_SUBS; i++) {
        const MkvTrack *tr = mkv_track(m, i);
        if (!taken(tr)) continue;
        tracks++;
        all_there &= beside(t->path, tr, &seen); /* (every one asked: the numbering follows the file's order) */
    }
    mkv_close(m);
    if (!tracks || all_there) { /* the headers were enough */
        fclose(io.f);
        put_done(t, tracks, 0);
        return TAKE_DONE;
    }
    AvConvJob j = {.in = rd, .stop = &io.halt, .progress = on_progress, .progress_ctx = &io};
    io.whole = true;
    atomic_fetch_add(&g_reads, 1);
    bool ok = av_subtitles(&j);
    int saved = ok && !io.halt ? convert_save_subs(t->path, j.subs, j.nsubs) : 0, found = j.nsubs;
    av_convert_free(&j);
    fclose(io.f);
    if (io.halt || (!ok && !as_scanned(t))) return TAKE_LATER; /* read again later, from the start */
    if (!ok) {
        log_warn("media: subtitles: couldn't read %s - %s", t->rel, j.err);
        put_done(t, 0, 0);
        return TAKE_DONE;
    }
    if (saved) log_info("media: subtitles: %d track%s saved from %s", saved, saved == 1 ? "" : "s", t->rel);
    put_done(t, found, saved);
    return TAKE_DONE;
}

static void worker(void *arg) {
    Run *r = arg;
    thread_background(); /* the household's pages and streams come first */
    int i = 0, closed = 0, first = -1;
    for (; i < r->n && !must_stop(); i++) /* (stopped: the rest - and the one stopped half-way - wait for the next run) */
        if (take_one(&r->todo[i]) == TAKE_CLOSED && !closed++) first = i;
    if (closed) /* (once a run: they're tried again at the next one) */
        log_warn("media: subtitles: %d video%s couldn't be opened, e.g. %s", closed, closed == 1 ? "" : "s",
                 r->todo[first].path);
    atomic_store(&g_finished, i == r->n && !must_stop());
    free(r->todo);
    free(r);
    atomic_store(&g_running, false);
    app_tick_soon();
}

/* ================================================================== the tick */

static int wait_reason(int64_t now, bool quiet) {
    if (setting_int(SUBTAKE_SETTING, 1) != 1) return WAIT_OFF;
    if (media_lib_away()) return WAIT_AWAY;
    if (convert_busy()) return WAIT_CONVERT;
    bool saw = atomic_exchange(&g_saw_watch, false); /* (a stream the worker saw may be over already) */
    if (saw || media_watching_active() > 0) {
        g_last_watch = now ? now : 1;
        return WAIT_WATCH;
    }
    if (quiet && g_last_watch && now - g_last_watch < WATCH_QUIET_MS) return WAIT_WATCH;
    return WAIT_NONE;
}

static void rescan(void) {
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

/* The worker's results into the table (event loop: db.h) */
static void take_results(int64_t now) {
    lock_take(&g_lock);
    Done *d = g_done;
    int n = g_ndone;
    g_done = NULL;
    g_ndone = g_capdone = 0;
    lock_give(&g_lock);
    if (!n) return;
    db_begin();
    for (int i = 0; i < n; i++) {
        sqlite3_stmt *st = db_prep("INSERT OR REPLACE INTO media_subtake (rel, size, mtime, tracks, saved, at) VALUES (?, ?, ?, ?, ?, ?)");
        if (st) {
            sqlite3_bind_text(st, 1, d[i].rel, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, d[i].size);
            sqlite3_bind_int64(st, 3, d[i].mtime);
            sqlite3_bind_int(st, 4, d[i].tracks);
            sqlite3_bind_int(st, 5, d[i].saved);
            sqlite3_bind_int64(st, 6, now_sec());
            sqlite3_step(st);
        }
        mark_set(d[i].rel, d[i].size, d[i].mtime, d[i].tracks);
        if (d[i].saved && !g_dirty_at) g_dirty_at = now ? now : 1;
    }
    db_commit();
    free(d);
    recount();
}

static void start_run(void) {
    Run *r = xcalloc(1, sizeof *r);
    r->todo = todo_list(&r->n, INT32_MAX);
    if (!r->n) {
        free(r->todo);
        free(r);
        g_idle_gen = media_lib_gen();
        return;
    }
    atomic_store(&g_halt, 0);
    atomic_store(&g_finished, false);
    atomic_store(&g_running, true);
    log_info("media: subtitles: looking inside %d video%s", r->n, r->n == 1 ? "" : "s");
    if (!thread_spawn(worker, r)) {
        atomic_store(&g_running, false);
        free(r->todo);
        free(r);
        log_error("media: subtitles: cannot start a thread");
    }
}

void subtake_tick(int64_t now) {
    /* (first: a worker seen ended has put all its results, so they're taken just below) */
    bool running = atomic_load(&g_running), finished = !running && atomic_exchange(&g_finished, false);
    take_results(now);
    counted();
    if (finished) { /* went through its list: the rest (changed, gone) waits for a scan */
        g_idle_gen = media_lib_gen();
        if (g_checked == g_total)
            log_info("media: subtitles: every video looked inside - %d of %d hold subtitles", g_found, g_total);
    }
    g_wait = wait_reason(now, true);
    if (!g_wait && now - g_started < START_AFTER_MS) g_wait = WAIT_START;
    if (running && g_wait && g_wait != WAIT_START) atomic_store(&g_halt, 1);
    else if (!running && !g_wait && g_checked < g_total && g_idle_gen != media_lib_gen() && !app_testing()) start_run();
    running = atomic_load(&g_running);
    if (running) app_wake_in(1000); /* (its results, and stopping it in time when someone starts watching) */
    /* the new subtitles join the library: when a run ends, else every RESCAN_MS */
    if (g_dirty_at && !media_scanning() && (!running || now - g_dirty_at >= RESCAN_MS)) {
        g_dirty_at = 0;
        rescan();
    }
    uint64_t sig = ((uint64_t)(unsigned)g_total << 40) ^ ((uint64_t)(unsigned)g_checked << 20) ^ (uint64_t)(unsigned)g_found ^
                   ((uint64_t)(g_wait * 2 + running) << 60);
    if (sig != g_shown) {
        g_shown = sig;
        app_changed_where("/media/settings");
    }
}

void subtake_start(void) {
    atomic_store(&g_quit, 0);
    atomic_store(&g_halt, 0);
    atomic_store(&g_finished, false);
    atomic_store(&g_saw_watch, false);
    atomic_store(&g_reads, 0);
    atomic_store(&g_opens, 0);
    atomic_store(&g_trip, 0);
    g_nmarks = 0;
    g_gen = g_idle_gen = (unsigned)-1;
    g_total = g_checked = g_found = 0;
    g_wait = WAIT_START;
    g_started = now_ms();
    g_last_watch = g_dirty_at = 0;
    g_shown = 0;
    /* the table's own format: an older one goes, and everything is read again */
    bool current = setting_int("media.subtake_format", SUBTAKE_FORMAT) == SUBTAKE_FORMAT;
    if (!current && db_exec("DROP TABLE IF EXISTS media_subtake")) {
        log_info("media: subtitles: what's taken out changed - every video is looked inside again");
        current = true;
    }
    if (current && setting_int("media.subtake_format", 0) != SUBTAKE_FORMAT) setting_set_int("media.subtake_format", SUBTAKE_FORMAT);
    db_exec(SCHEMA);
    sqlite3_stmt *st = db_prep("SELECT rel, size, mtime, tracks FROM media_subtake");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(st, 0);
        if (rel) mark_set(rel, sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2), sqlite3_column_int(st, 3));
    }
    if (st) sqlite3_reset(st);
}

void subtake_stop(void) {
    atomic_store(&g_quit, 1);
    for (int waited = 0; waited < 8000 && atomic_load(&g_running); waited += 50) plat_sleep_ms(50);
    lock_take(&g_lock); /* (results not taken: those files are read again) */
    free(g_done);
    g_done = NULL;
    g_ndone = g_capdone = 0;
    lock_give(&g_lock);
    free(g_marks);
    g_marks = NULL;
    g_nmarks = g_capmarks = 0;
}

/* ================================================================== the card */

static void group(long long n, char *out, size_t cap) { /* 1,034 · 1 034 · 1.034 */
    char digits[24];
    int k = snprintf(digits, sizeof digits, "%lld", n);
    Buf b = {0};
    i18n_group(&b, digits, (size_t)k);
    str_copy(out, cap, buf_cstr(&b));
    buf_free(&b);
}

void subtake_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    counted();
    bool on = setting_int(SUBTAKE_SETTING, 1) == 1;
    if (!g_total) return; /* no MKV in the library: nothing to take out */
    char done[24], all[24], found[24];
    group(g_checked, done, sizeof done);
    group(g_total, all, sizeof all);
    group(g_found, found, sizeof found);
    ui_card(b, T("Subtitles inside videos"), NULL, NULL);
    H(b, "<p class=\"muted media-small\">%s</p><p id=\"media-subtake\">",
      T("Many MKV videos carry their subtitles inside the file, where only some players find them. Hemma takes them "
        "out once, in the background, and saves them next to each video - so they show here and in other players "
        "too."));
    if (g_checked < g_total && on) {
        int w = atomic_load(&g_running) ? WAIT_NONE : g_wait;
        H(b,
          w == WAIT_WATCH     ? T("Taking subtitles out of videos: %s of %s checked - waiting while someone watches")
          : w == WAIT_CONVERT ? T("Taking subtitles out of videos: %s of %s checked - waiting for the conversions to finish")
          : w == WAIT_AWAY    ? T("Taking subtitles out of videos: %s of %s checked - waiting for a library folder that's missing")
                              : T("Taking subtitles out of videos: %s of %s checked (waits while someone watches)"),
          done, all);
        if (g_found) {
            buf_puts(b, "<br><span class=\"muted\">");
            H(b, TN(g_found, "Subtitles found in %s video so far", "Subtitles found in %s videos so far"), found);
            buf_puts(b, "</span>");
        }
    } else if (g_checked < g_total) {
        H(b, T("Switched off - %s of %s videos checked so far."), done, all);
    } else if (g_found) {
        H(b, TN(g_found, "Subtitles found in %s video", "Subtitles found in %s videos"), found);
    } else {
        H(b, "%s", T("No subtitles found inside the videos."));
    }
    buf_puts(b, "</p>");
    ui_form(b, "media.take_subs", "inline");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> %s</label>"
         "<noscript><button class=\"btn sm\">%s</button></noscript>",
      on ? " checked" : "", T("Take subtitles out of videos by themselves"), T("Save"));
    ui_form_end(b);
    ui_card_end(b);
}

void subtake_act_switch(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    bool on = str_eq(fields_str(f, "on"), "1");
    setting_set_int(SUBTAKE_SETTING, on);
    if (!on) atomic_store(&g_halt, 1); /* (the file being read goes back on the list) */
    else g_idle_gen = (unsigned)-1;
    app_wake_in(200);
    ctx_ok(c, "%s", on ? T("Hemma takes the subtitles out of videos by itself") : T("Subtitles stay inside the videos"));
}

/* ================================================================== tests */

int subtake_test_step(void) {
    int64_t now = now_ms();
    take_results(now);
    counted();
    if (wait_reason(now, false)) return -1;
    int n = 0;
    Todo *t = todo_list(&n, 1);
    if (!n) {
        free(t);
        if (g_dirty_at) {
            g_dirty_at = 0;
            rescan();
        }
        return 0;
    }
    atomic_store(&g_halt, 0);
    take_one(t);
    free(t);
    take_results(now);
    return 1;
}

int subtake_test_reads(void) { return atomic_load(&g_reads); }
int subtake_test_opens(void) { return atomic_load(&g_opens); }
void subtake_test_halt_after(int reads) { atomic_store(&g_trip, reads); }
