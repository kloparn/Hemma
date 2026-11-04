/* modules/media/intros.c — a series' intros, found by themselves (2.9; owner, 2026-11-03: "are we able to automatically
 * determine when the intro is? ... a 'skip' button", "it's perfect when no-one is watching we process stuff").
 *
 * The intro is the stretch of sound the episodes of a season share (modules/av/intro: fingerprints of each
 * episode's first minutes, compared in pairs). This job does it a season at a time, in the background: every
 * episode of a season (seasons 1 and up; not the specials, not a disc's extras) is read - the file the player
 * plays, its first INTRO_READ_S at most (less than half the episode) -, the season's fingerprints are matched,
 * and each episode gets a row in the raw table media_intro: found (from_ms, to_ms), none, or unreadable (its sound
 * in a format Hemma can't decode yet: MP3, Opus, Vorbis ... - `why`, in English, for the card and the log). A
 * season of fewer than four episodes borrows its neighbours' (the season before's last, the next one's first) to
 * compare with. An admin's own times (Manage -> Intro: state HAND) are never replaced.
 *
 * Never in the way: one season at a time on one thread at background priority, and only while nobody is watching
 * (stream.c; not for WATCH_QUIET_MS after the last stream either), no conversion is queued or running, the
 * subtitles' job isn't reading (the same disk), no library folder is away and the setting is on (INTROS_SETTING,
 * admins, Library settings). A season stopped half-way keeps what it read in memory (PRINTS: by file, size and
 * mtime) and goes on from there the next time; Hemma restarting reads it again.
 *
 * Rows are by the episode's path below its library folder (a disk mounted elsewhere isn't read again), with its
 * size and mtime (a changed file - and so its season - is done again). The table has its own format (setting
 * media.intro_format): a new one drops every row found by itself, the admins' stay. A season with a new episode
 * is done again whole: its rows may move a little. The player asks media_intro_of: the watch page's "Skip intro". */
#include "intros.h"

#include "../../core/db.h"
#include "../../core/ui.h"
#include "../av/intro.h"
#include "../av/mkv.h"
#include "convert.h"
#include "download.h"
#include "media.h"
#include "mediainfo.h"
#include "subtake.h"

#include <stdatomic.h>

#define INTROS_FORMAT 1           /* raise it when finding changes: every row found by itself goes */
#define WATCH_QUIET_MS 60000      /* nobody has watched for this long before reading goes on */
#define START_AFTER_MS 120000     /* a start (a Pi booting) settles first - and the subtitles' job goes first */
#define INTRO_READ_S 600          /* an episode's first ten minutes at most ... */
#define INTRO_READ_SHARE 45       /* ... and at most this % of it */
#define PRINTS 48                 /* episodes' fingerprints kept in memory (~50 KB each) */
#define GUESTS 3                  /* a small season borrows this many episodes each side */

#define SCHEMA                                                                                                             \
    "CREATE TABLE IF NOT EXISTS media_intro (rel TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL, "         \
    "state INTEGER NOT NULL, from_ms INTEGER NOT NULL DEFAULT -1, to_ms INTEGER NOT NULL DEFAULT -1, "                      \
    "votes INTEGER NOT NULL DEFAULT 0, why TEXT NOT NULL DEFAULT '', at INTEGER NOT NULL DEFAULT 0)"

enum { ST_UNREADABLE = -1, ST_NONE = 0, ST_FOUND = 1, ST_HAND = 2 };
enum { WAIT_NONE, WAIT_OFF, WAIT_WATCH, WAIT_CONVERT, WAIT_AWAY, WAIT_START, WAIT_SUBS };

/* What's known, in memory (event loop): by hash64 of rel, sorted */
typedef struct {
    uint64_t h;
    int64_t size, mtime, from, to;
    int state;
} Mark;

static Mark *g_marks;
static int g_nmarks, g_capmarks;

/* One episode of a job */
typedef struct {
    char rel[400];               /* the episode's own: its row */
    char path[1024], path2[1024]; /* the file the player plays; its original when that's another file ("" = none) */
    int64_t size, mtime;         /* the episode's own (its row) */
    int64_t key_size, key_mtime; /* the file read (its fingerprint in memory) */
    int read_s;
    bool target, hand;           /* a row is written (guests only lend their fingerprints); an admin's row stays */
    IntroPrint print;            /* read by the worker, or from memory */
    int rc;                      /* INTRO_READ_* once read */
    bool read;
    char why[160];
    int64_t from, to;
    int votes;
} JobEp;

typedef struct {
    char what[200]; /* "Show season 2", for the log */
    uint64_t series;
    int season;
    JobEp *ep;
    int n;
    bool done;   /* matched: rows to write (else stopped: what was read is kept) */
    bool failed; /* not stopped, yet an episode couldn't be opened (gone, a disk away): not again until the next scan */
    int64_t ms;
} Job;

/* fingerprints in memory, by the file read */
typedef struct {
    uint64_t h;
    int64_t size, mtime, used;
    IntroPrint p;
} Kept;

static Kept g_kept[PRINTS];
static int64_t g_use;

static Lock g_lock = LOCK_INIT;
static Job *g_back; /* a job the worker finished or stopped (g_lock) */
static atomic_bool g_running;
static atomic_int g_halt, g_quit, g_reads;
static atomic_bool g_saw_watch;

static unsigned g_gen = (unsigned)-1, g_idle_gen = (unsigned)-1; /* the index counted / one with nothing left to start */
static uint64_t *g_tried; /* seasons a run went through in this index (series ^ season hashed): not again until it changes */
static int g_ntried, g_captried;
static int g_total, g_done, g_found, g_unreadable;
static int g_wait, g_read_s;
static int64_t g_started, g_last_watch;
static uint64_t g_shown;
static char g_last_why[160];

/* ================================================================== what's known */

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

static void mark_set(const char *rel, int64_t size, int64_t mtime, int state, int64_t from, int64_t to) {
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
    g_marks[i] = (Mark){h, size, mtime, from, to, state};
}

static void mark_drop(const char *rel) {
    int i = mark_find(hash64_str(rel));
    if (i < 0) return;
    memmove(g_marks + i, g_marks + i + 1, (size_t)(g_nmarks - i - 1) * sizeof *g_marks);
    g_nmarks--;
}

/* Its row as the file is now (an admin's always; NULL = none, or the file changed since) */
static const Mark *mark_of(const MediaFile *f) {
    int i = mark_find(hash64_str(f->rel));
    if (i < 0) return NULL;
    const Mark *m = &g_marks[i];
    return m->state == ST_HAND || (m->size == f->size && m->mtime == f->mtime) ? m : NULL;
}

/* An episode of a season (not the specials; a disc's play-all and bonus clips are extras, not episodes) */
static bool eligible(const MediaFile *f) {
    return f && f->type == MF_VIDEO && f->role == MF_MAIN && f->name.kind == MEDIA_EPISODE && f->name.season > 0;
}

static void recount(void) {
    const MediaIndex *ix = media_lib();
    g_gen = media_lib_gen();
    g_total = g_done = g_found = g_unreadable = 0;
    for (int i = 0; i < ix->nepisodes; i++) {
        const MediaFile *f = &ix->files[ix->episodes[i]];
        if (!eligible(f)) continue;
        g_total++;
        const Mark *m = mark_of(f);
        if (!m) continue;
        g_done++;
        g_found += m->state == ST_FOUND || (m->state == ST_HAND && m->from >= 0);
        g_unreadable += m->state == ST_UNREADABLE;
    }
}

static void counted(void) {
    if (g_gen == media_lib_gen()) return;
    g_ntried = 0; /* (a new index: every season may be tried again) */
    recount();
}

bool media_intro_of(const MediaFile *f, int64_t *from_ms, int64_t *to_ms) {
    if (!eligible(f)) return false;
    const Mark *m = mark_of(f);
    if (!m || (m->state != ST_FOUND && m->state != ST_HAND) || m->from < 0 || m->to <= m->from) return false;
    *from_ms = m->from, *to_ms = m->to;
    return true;
}

/* ================================================================== fingerprints in memory */

static Kept *kept_find(uint64_t h, int64_t size, int64_t mtime) {
    for (int i = 0; i < PRINTS; i++)
        if (g_kept[i].p.n && g_kept[i].h == h && g_kept[i].size == size && g_kept[i].mtime == mtime) return &g_kept[i];
    return NULL;
}

static bool print_copy(const IntroPrint *from, IntroPrint *to) {
    *to = *from;
    to->v = malloc(sizeof(uint32_t) * (size_t)from->n);
    to->loud = malloc((size_t)from->n);
    if (!to->v || !to->loud) {
        intro_print_free(to);
        return false;
    }
    memcpy(to->v, from->v, sizeof(uint32_t) * (size_t)from->n);
    memcpy(to->loud, from->loud, (size_t)from->n);
    return true;
}

static void kept_put(const char *path, int64_t size, int64_t mtime, IntroPrint *p) { /* takes p */
    uint64_t h = hash64_str(path);
    Kept *k = kept_find(h, size, mtime);
    if (!k) {
        k = &g_kept[0];
        for (int i = 1; i < PRINTS; i++)
            if (g_kept[i].used < k->used) k = &g_kept[i];
        intro_print_free(&k->p);
    } else {
        intro_print_free(&k->p);
    }
    *k = (Kept){h, size, mtime, ++g_use, *p};
    memset(p, 0, sizeof *p);
}

static void kept_free(void) {
    for (int i = 0; i < PRINTS; i++) intro_print_free(&g_kept[i].p);
    memset(g_kept, 0, sizeof g_kept);
}

/* ================================================================== a job */

static void job_free(Job *j) {
    if (!j) return;
    for (int i = 0; i < j->n; i++) intro_print_free(&j->ep[i].print);
    free(j->ep);
    free(j);
}

static bool add_ep(Job *j, int cap, const MediaFile *f, bool target) {
    if (j->n >= cap) return false;
    JobEp *e = &j->ep[j->n];
    memset(e, 0, sizeof *e);
    const MediaFile *pf = media_playable(f);
    if (!media_full_path(pf, e->path, sizeof e->path)) return false;
    if (pf != f && !media_full_path(f, e->path2, sizeof e->path2)) e->path2[0] = 0;
    str_copy(e->rel, sizeof e->rel, f->rel);
    e->size = f->size, e->mtime = f->mtime;
    e->key_size = pf->size, e->key_mtime = pf->mtime;
    int64_t dur = pf->probe.duration_ms ? pf->probe.duration_ms : f->probe.duration_ms;
    int s = dur > 0 ? (int)(dur / 1000 * INTRO_READ_SHARE / 100) : INTRO_READ_S;
    e->read_s = g_read_s ? g_read_s : CLAMP(s, 60, INTRO_READ_S);
    e->target = target;
    const Mark *m = target ? mark_of(f) : NULL;
    e->hand = m && m->state == ST_HAND;
    e->from = e->to = -1;
    Kept *k = kept_find(hash64_str(e->path), e->key_size, e->key_mtime);
    if (k && print_copy(&k->p, &e->print)) {
        k->used = ++g_use;
        e->read = true, e->rc = INTRO_READ_OK;
    }
    j->n++;
    return true;
}

/* A season of a series: its episodes, and - fewer than four - its neighbours' to compare with */
static Job *job_for(const MediaSeries *s, int season) {
    int cap = 0;
    for (int i = 0; i < s->count; i++) cap += eligible(media_episode(s, i)) && media_episode(s, i)->name.season == season;
    if (!cap) return NULL;
    Job *j = xcalloc(1, sizeof *j);
    j->ep = xcalloc((size_t)(cap + 2 * GUESTS), sizeof *j->ep);
    j->series = s->id, j->season = season;
    snprintf(j->what, sizeof j->what, "%s season %d", s->title, season);
    int first = -1, last = -1;
    for (int i = 0; i < s->count; i++) {
        const MediaFile *f = media_episode(s, i);
        if (!eligible(f) || f->name.season != season) continue;
        if (first < 0) first = i;
        last = i;
        add_ep(j, cap + 2 * GUESTS, f, true);
    }
    if (j->n < 4) { /* guests: the episodes before and after in the series (its other seasons) */
        for (int i = first - 1, k = 0; i >= 0 && k < GUESTS; i--)
            if (eligible(media_episode(s, i)) && add_ep(j, cap + 2 * GUESTS, media_episode(s, i), false)) k++;
        for (int i = last + 1, k = 0; i < s->count && k < GUESTS; i++)
            if (eligible(media_episode(s, i)) && add_ep(j, cap + 2 * GUESTS, media_episode(s, i), false)) k++;
    }
    return j;
}

/* ================================================================== the worker */

static bool must_stop(void *ctx) {
    (void)ctx;
    if (media_watching_active() > 0) atomic_store(&g_saw_watch, true);
    return atomic_load(&g_halt) || atomic_load(&g_quit) || atomic_load(&g_saw_watch);
}

static bool io_read(void *ctx, int64_t off, void *dst, size_t n) { return !must_stop(NULL) && file_pread((FILE *)ctx, off, dst, n); }

static int read_path(const char *path, int seconds, IntroPrint *out, char *why, size_t cap) {
    int64_t size = 0;
    FILE *f = path_stat(path, &size, NULL, NULL) ? file_open(path, "rb") : NULL;
    if (!f) {
        snprintf(why, cap, "it couldn't be opened");
        return INTRO_READ_STOPPED; /* (gone meanwhile, a disk away: the next scan says) */
    }
    MkvReader rd = {f, size, io_read};
    IntroReadOpts o = {seconds, must_stop, NULL};
    int rc = intro_read(&rd, &o, out, why, cap);
    fclose(f);
    return rc;
}

static void read_one(JobEp *e) {
    atomic_fetch_add(&g_reads, 1);
    e->rc = read_path(e->path, e->read_s, &e->print, e->why, sizeof e->why);
    if ((e->rc == INTRO_READ_CODEC || e->rc == INTRO_READ_BROKEN) && e->path2[0]) { /* the original's sound, then */
        char why2[160];
        IntroPrint p;
        int rc = read_path(e->path2, e->read_s, &p, why2, sizeof why2);
        if (rc == INTRO_READ_OK || rc == INTRO_READ_STOPPED) {
            intro_print_free(&e->print);
            e->print = p, e->rc = rc;
            if (rc == INTRO_READ_STOPPED) e->why[0] = 0;
        }
    }
    e->read = e->rc != INTRO_READ_STOPPED;
}

/* Read what isn't, then match - or stop where it got to */
static void run_job(Job *j) {
    int64_t t0 = now_ms();
    for (int i = 0; i < j->n && !must_stop(NULL); i++)
        if (!j->ep[i].read) read_one(&j->ep[i]);
    bool all = true;
    for (int i = 0; i < j->n; i++) all &= j->ep[i].read;
    if (all && !must_stop(NULL)) {
        const IntroPrint **pp = xcalloc((size_t)j->n, sizeof *pp);
        IntroFound *found = xcalloc((size_t)j->n, sizeof *found);
        for (int i = 0; i < j->n; i++) pp[i] = j->ep[i].rc == INTRO_READ_OK && j->ep[i].print.n ? &j->ep[i].print : NULL;
        intro_find(pp, j->n, NULL, found);
        for (int i = 0; i < j->n; i++) j->ep[i].from = found[i].from_ms, j->ep[i].to = found[i].to_ms, j->ep[i].votes = found[i].votes;
        free(pp);
        free(found);
        j->done = true;
    }
    j->failed = !j->done && !all && !must_stop(NULL);
    j->ms = now_ms() - t0;
}

static void worker(void *arg) {
    Job *j = arg;
    thread_background(); /* the household's pages and streams come first */
    run_job(j);
    lock_take(&g_lock);
    g_back = j; /* (the event loop made it: it frees it - free_on_loop's rule, the loop's own) */
    lock_give(&g_lock);
    atomic_store(&g_running, false);
    app_tick_soon();
}

/* ================================================================== the tick */

static int wait_reason(int64_t now, bool quiet) {
    if (setting_int(INTROS_SETTING, 1) != 1) return WAIT_OFF;
    if (media_lib_away()) return WAIT_AWAY;
    if (convert_busy()) return WAIT_CONVERT;
    if (subtake_busy()) return WAIT_SUBS;
    bool saw = atomic_exchange(&g_saw_watch, false);
    if (saw || media_watching_active() > 0) {
        g_last_watch = now ? now : 1;
        return WAIT_WATCH;
    }
    if (quiet && g_last_watch && now - g_last_watch < WATCH_QUIET_MS) return WAIT_WATCH;
    return WAIT_NONE;
}

static void put_row(const JobEp *e, int state, int64_t from, int64_t to, int votes, const char *why) {
    sqlite3_stmt *st = db_prep("INSERT OR REPLACE INTO media_intro (rel, size, mtime, state, from_ms, to_ms, votes, why, at) "
                               "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (st) {
        sqlite3_bind_text(st, 1, e->rel, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, e->size);
        sqlite3_bind_int64(st, 3, e->mtime);
        sqlite3_bind_int(st, 4, state);
        sqlite3_bind_int64(st, 5, from);
        sqlite3_bind_int64(st, 6, to);
        sqlite3_bind_int(st, 7, votes);
        sqlite3_bind_text(st, 8, why ? why : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 9, now_sec());
        sqlite3_step(st);
    }
    mark_set(e->rel, e->size, e->mtime, state, from, to);
}

static void tried(uint64_t series, int season) {
    if (g_ntried == g_captried) {
        g_captried = g_captried ? g_captried * 2 : 64;
        g_tried = xrealloc(g_tried, (size_t)g_captried * sizeof *g_tried);
    }
    g_tried[g_ntried++] = series ^ ((uint64_t)(unsigned)season * 0x9E3779B97F4A7C15ull);
}

static bool was_tried(uint64_t series, int season) {
    uint64_t h = series ^ ((uint64_t)(unsigned)season * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < g_ntried; i++)
        if (g_tried[i] == h) return true;
    return false;
}

/* A job back from the worker: its fingerprints into memory, its rows (when it went through) */
static void take_back(Job *j) {
    if (!j) return;
    if (j->failed) {
        tried(j->series, j->season);
        for (int i = 0; i < j->n; i++)
            if (!j->ep[i].read) {
                log_warn("media: intros: %s waits for the next scan - %s: %s", j->what, j->ep[i].rel, j->ep[i].why);
                break;
            }
    }
    if (j->done) {
        int targets = 0, found = 0, unreadable = 0;
        int64_t len = 0;
        db_begin();
        for (int i = 0; i < j->n; i++) {
            JobEp *e = &j->ep[i];
            if (!e->target || e->hand) continue;
            targets++;
            if (e->rc != INTRO_READ_OK) {
                put_row(e, ST_UNREADABLE, -1, -1, 0, e->why);
                unreadable++;
                str_copy(g_last_why, sizeof g_last_why, e->why);
                log_info("media: intros: %s can't be listened to - %s", e->rel, e->why);
            } else if (e->from >= 0) {
                put_row(e, ST_FOUND, e->from, e->to, e->votes, NULL);
                found++;
                len += e->to - e->from;
            } else {
                put_row(e, ST_NONE, -1, -1, 0, NULL);
            }
        }
        db_commit();
        tried(j->series, j->season);
        if (found)
            log_info("media: intros: %s - found in %d of %d episodes (%.1f s on average), %.0f s", j->what, found, targets,
                     len / 1000.0 / found, j->ms / 1000.0);
        else
            log_info("media: intros: %s - none found in %d episodes%s, %.0f s", j->what, targets,
                     unreadable == targets ? " (none could be listened to)" : "", j->ms / 1000.0);
    }
    for (int i = 0; i < j->n; i++) /* (stopped or done: what was read is kept for the next time) */
        if (j->ep[i].rc == INTRO_READ_OK && j->ep[i].print.n)
            kept_put(j->ep[i].path, j->ep[i].key_size, j->ep[i].key_mtime, &j->ep[i].print);
    job_free(j);
    recount();
}

static void take_results(void) {
    lock_take(&g_lock);
    Job *j = g_back;
    g_back = NULL;
    lock_give(&g_lock);
    take_back(j);
}

/* The next season with an episode not done: the newest series first (what the household added lately is what it
   watches next), its seasons in order */
static Job *next_job(void) {
    const MediaIndex *ix = media_lib();
    int *order = xcalloc((size_t)ix->nseries + 1, sizeof *order);
    for (int i = 0; i < ix->nseries; i++) order[i] = i;
    for (int i = 1; i < ix->nseries; i++) /* (insertion sort by newest: a few hundred series) */
        for (int k = i; k > 0 && ix->series[order[k]].newest > ix->series[order[k - 1]].newest; k--) {
            int t = order[k];
            order[k] = order[k - 1], order[k - 1] = t;
        }
    Job *j = NULL;
    for (int k = 0; k < ix->nseries && !j; k++) {
        const MediaSeries *s = &ix->series[order[k]];
        for (int i = 0; i < s->count && !j; i++) {
            const MediaFile *f = media_episode(s, i);
            if (!eligible(f) || mark_of(f) || was_tried(s->id, f->name.season)) continue;
            j = job_for(s, f->name.season);
            if (!j) tried(s->id, f->name.season);
        }
    }
    free(order);
    return j;
}

static void start_run(void) {
    Job *j = next_job();
    if (!j) { /* (every season left was tried: not again until the library changes) */
        g_idle_gen = media_lib_gen();
        return;
    }
    atomic_store(&g_halt, 0);
    atomic_store(&g_running, true);
    log_info("media: intros: listening to %s (%d episode%s)", j->what, j->n, j->n == 1 ? "" : "s");
    if (!thread_spawn(worker, j)) {
        atomic_store(&g_running, false);
        job_free(j);
        log_error("media: intros: cannot start a thread");
    }
}

void intros_tick(int64_t now) {
    bool running = atomic_load(&g_running);
    if (!running) take_results();
    counted();
    g_wait = wait_reason(now, true);
    if (!g_wait && now - g_started < START_AFTER_MS) g_wait = WAIT_START;
    if (running && g_wait && g_wait != WAIT_START) atomic_store(&g_halt, 1);
    else if (!running && !g_wait && g_done < g_total && g_idle_gen != media_lib_gen() && !app_testing()) start_run();
    running = atomic_load(&g_running);
    if (running) app_wake_in(1000); /* (its results, and stopping it in time when someone starts watching) */
    uint64_t sig = ((uint64_t)(unsigned)g_total << 40) ^ ((uint64_t)(unsigned)g_done << 20) ^ (uint64_t)(unsigned)g_found ^
                   ((uint64_t)(g_wait * 2 + running) << 60);
    if (sig != g_shown) {
        g_shown = sig;
        app_changed_where("/media/settings");
    }
}

bool intros_busy(void) { return atomic_load(&g_running); }

void intros_start(void) {
    atomic_store(&g_quit, 0);
    atomic_store(&g_halt, 0);
    atomic_store(&g_saw_watch, false);
    atomic_store(&g_reads, 0);
    g_nmarks = 0;
    g_ntried = 0;
    g_gen = g_idle_gen = (unsigned)-1;
    g_total = g_done = g_found = g_unreadable = 0;
    g_wait = WAIT_START;
    g_started = now_ms();
    g_last_watch = 0;
    g_shown = 0;
    g_read_s = 0;
    g_last_why[0] = 0;
    db_exec(SCHEMA);
    if (setting_int("media.intro_format", 0) != INTROS_FORMAT) { /* finding changed: what it found goes, the admins' stay */
        if (db_exec("DELETE FROM media_intro WHERE state <> 2")) setting_set_int("media.intro_format", INTROS_FORMAT);
    }
    sqlite3_stmt *st = db_prep("SELECT rel, size, mtime, state, from_ms, to_ms FROM media_intro");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        const char *rel = (const char *)sqlite3_column_text(st, 0);
        if (rel)
            mark_set(rel, sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2), sqlite3_column_int(st, 3),
                     sqlite3_column_int64(st, 4), sqlite3_column_int64(st, 5));
    }
    if (st) sqlite3_reset(st);
}

void intros_stop(void) {
    atomic_store(&g_quit, 1);
    for (int waited = 0; waited < 8000 && atomic_load(&g_running); waited += 50) plat_sleep_ms(50);
    if (!atomic_load(&g_running)) {
        lock_take(&g_lock);
        job_free(g_back); /* (rows not written: that season is done again) */
        g_back = NULL;
        lock_give(&g_lock);
    }
    kept_free();
    free(g_marks);
    g_marks = NULL;
    g_nmarks = g_capmarks = 0;
    free(g_tried);
    g_tried = NULL;
    g_ntried = g_captried = 0;
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

void intros_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    counted();
    if (!g_total) return; /* no series in the library */
    bool on = setting_int(INTROS_SETTING, 1) == 1;
    char done[24], all[24], found[24], bad[24];
    group(g_done, done, sizeof done);
    group(g_total, all, sizeof all);
    group(g_found, found, sizeof found);
    group(g_unreadable, bad, sizeof bad);
    ui_card(b, T("Skip intros"), NULL, NULL);
    H(b, "<p class=\"muted media-small\">%s</p><p id=\"media-intros\">",
      T("Hemma listens to the start of every episode, in the background while nobody watches, and finds the intro the "
        "episodes of a season share. The player then offers Skip intro. You can fix one on the series' Manage page."));
    if (g_done < g_total && on) {
        int w = atomic_load(&g_running) ? WAIT_NONE : g_wait;
        H(b,
          w == WAIT_WATCH     ? T("Listening for intros: %s of %s episodes done - waiting while someone watches")
          : w == WAIT_CONVERT ? T("Listening for intros: %s of %s episodes done - waiting for the conversions to finish")
          : w == WAIT_SUBS    ? T("Listening for intros: %s of %s episodes done - waiting for the subtitles to be taken out")
          : w == WAIT_AWAY    ? T("Listening for intros: %s of %s episodes done - waiting for a library folder that's missing")
                              : T("Listening for intros: %s of %s episodes done (waits while someone watches)"),
          done, all);
        if (g_found) {
            buf_puts(b, "<br><span class=\"muted\">");
            H(b, TN(g_found, "An intro found in %s episode so far", "Intros found in %s episodes so far"), found);
            buf_puts(b, "</span>");
        }
    } else if (g_done < g_total) {
        H(b, T("Switched off - %s of %s episodes done so far."), done, all);
    } else if (g_found) {
        H(b, TN(g_found, "An intro found in %s episode", "Intros found in %s episodes"), found);
    } else {
        H(b, "%s", T("No intros found."));
    }
    if (g_unreadable) {
        buf_puts(b, "<br><span class=\"muted\">");
        H(b, TN(g_unreadable, "%s episode's sound can't be listened to yet", "%s episodes' sound can't be listened to yet"), bad);
        if (g_last_why[0]) H(b, " (%s)", g_last_why); /* (English, as the log says it) */
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</p>");
    ui_form(b, "media.intros", "inline");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> %s</label>"
         "<noscript><button class=\"btn sm\">%s</button></noscript>",
      on ? " checked" : "", T("Find intros by itself"), T("Save"));
    ui_form_end(b);
    ui_card_end(b);
}

void intros_act_switch(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    bool on = str_eq(fields_str(f, "on"), "1");
    setting_set_int(INTROS_SETTING, on);
    if (!on) atomic_store(&g_halt, 1); /* (the season being read keeps what it read) */
    else g_ntried = 0, g_idle_gen = (unsigned)-1;
    app_wake_in(200);
    ctx_ok(c, "%s", on ? T("Hemma finds the intros by itself") : T("Hemma stops looking for intros"));
}

/* ================================================================== Manage */

static void clock_ms(char *out, size_t cap, int64_t ms) { /* "1:02", "1:02:03" */
    int64_t s = (ms + 500) / 1000;
    if (s >= 3600) snprintf(out, cap, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else snprintf(out, cap, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
}

/* "1:02", "62", "1:02.5", "0:01:02" -> ms; -1 = not a time */
static int64_t parse_clock(const char *s) {
    double parts[3] = {0};
    int n = 0;
    const char *p = s;
    while (*p == ' ') p++;
    if (!*p) return -1;
    while (*p && n < 3) {
        char *end;
        double v = strtod(p, &end);
        if (end == p || v < 0) return -1;
        parts[n++] = v;
        p = end;
        while (*p == ' ') p++;
        if (*p == ':') p++;
        else if (*p) return -1;
    }
    if (*p) return -1;
    double sec = n == 1 ? parts[0] : n == 2 ? parts[0] * 60 + parts[1] : parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (sec > 24 * 3600.0) return -1;
    return (int64_t)(sec * 1000 + 0.5);
}

void media_intro_note(Buf *b, const MediaFile *e) {
    if (!eligible(e)) return;
    const Mark *m = mark_of(e);
    if (!m) return;
    char a[16], z[16];
    if ((m->state == ST_FOUND || m->state == ST_HAND) && m->from >= 0) {
        clock_ms(a, sizeof a, m->from);
        clock_ms(z, sizeof z, m->to);
        buf_puts(b, " · ");
        H(b, T("Intro %s–%s"), a, z);
    } else if (m->state == ST_NONE || m->state == ST_HAND) {
        H(b, " · %s", T("No intro"));
    }
}

void media_intro_dialog(Ctx *c, Buf *b, const MediaFile *e, const char *close) {
    if (!is_admin(c) || !eligible(e)) return;
    const Mark *m = mark_of(e);
    char a[16] = "", z[16] = "";
    bool have = m && (m->state == ST_FOUND || m->state == ST_HAND) && m->from >= 0;
    if (have) clock_ms(a, sizeof a, m->from), clock_ms(z, sizeof z, m->to);
    ui_modal(b, T("The intro"), close);
    buf_puts(b, "<p class=\"muted\">");
    if (!m) H(b, "%s", T("Hemma hasn't listened to this episode yet - it does when nobody watches."));
    else if (m->state == ST_HAND) H(b, "%s", have ? T("You set this intro by hand.") : T("You said this episode has no intro."));
    else if (m->state == ST_FOUND) H(b, "%s", T("Hemma found this intro by itself: the stretch this season's episodes share."));
    else if (m->state == ST_NONE) H(b, "%s", T("Hemma found no intro in this episode."));
    else H(b, "%s", T("Hemma couldn't listen to this episode's sound."));
    buf_puts(b, "</p>");
    ui_form(b, "media.intro", "stack");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"how\" value=\"set\">"
         "<div class=\"field-row\"><label class=\"field\"><span>%s</span><input name=\"from\" inputmode=\"decimal\" "
         "placeholder=\"1:02\" maxlength=\"12\" value=\"%s\" required></label>",
      e->id, T("Starts at"), a);
    H(b, "<label class=\"field\"><span>%s</span><input name=\"to\" inputmode=\"decimal\" placeholder=\"1:34\" maxlength=\"12\" "
         "value=\"%s\" required></label></div>",
      T("Ends at"), z);
    H(b, "<div class=\"modal-f\"><a class=\"btn\" href=\"%s\">%s</a><button class=\"btn primary\">%s</button></div>", close,
      T("Cancel"), T("Save"));
    ui_form_end(b);
    buf_puts(b, "<div class=\"ph-a\">");
    ui_form(b, "media.intro", "inline");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"how\" value=\"none\">"
         "<button class=\"btn\">%s</button>",
      e->id, T("It has no intro"));
    ui_form_end(b);
    if (m && m->state == ST_HAND) {
        ui_form(b, "media.intro", "inline");
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"how\" value=\"auto\">"
             "<button class=\"btn\">%s</button>",
          e->id, T("Let Hemma find it"));
        ui_form_end(b);
    }
    buf_puts(b, "</div>");
    ui_modal_end(b);
}

void intros_act_set(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    uint64_t id = 0;
    const MediaFile *e = media_parse_id(fields_str(f, "id"), &id) ? media_file(id) : NULL;
    if (!eligible(e)) {
        ctx_fail(c, "%s", T("That episode isn't in the library any more"));
        return;
    }
    const char *how = fields_str(f, "how");
    JobEp row = {0};
    str_copy(row.rel, sizeof row.rel, e->rel);
    row.size = e->size, row.mtime = e->mtime;
    char back[128];
    snprintf(back, sizeof back, "/media/series/%016llx?manage=1&s=%d", (unsigned long long)e->series, e->name.season);
    if (str_eq(how, "auto")) {
        sqlite3_stmt *st = db_prep("DELETE FROM media_intro WHERE rel = ?");
        if (st) {
            sqlite3_bind_text(st, 1, e->rel, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
        }
        mark_drop(e->rel);
        g_ntried = 0, g_idle_gen = (unsigned)-1;
        recount();
        log_info("media: intros: %s left %s's intro to Hemma again", c->me->name, e->rel);
        ctx_ok(c, "%s", T("Hemma finds this episode's intro again when nobody watches"));
    } else if (str_eq(how, "none")) {
        put_row(&row, ST_HAND, -1, -1, 0, NULL);
        recount();
        log_info("media: intros: %s said %s has no intro", c->me->name, e->rel);
        ctx_ok(c, "%s", T("No Skip intro for this episode"));
    } else {
        int64_t from = parse_clock(fields_str(f, "from")), to = parse_clock(fields_str(f, "to"));
        int64_t dur = e->probe.duration_ms ? e->probe.duration_ms : media_playable(e)->probe.duration_ms;
        if (from < 0) {
            ctx_fail_at(c, "from", "%s", T("Write it as minutes:seconds, like 1:02"));
            return;
        }
        if (to < 0) {
            ctx_fail_at(c, "to", "%s", T("Write it as minutes:seconds, like 1:34"));
            return;
        }
        if (to - from < 2000) {
            ctx_fail_at(c, "to", "%s", T("It ends before it starts - or right after"));
            return;
        }
        if (dur > 0 && to > dur) {
            ctx_fail_at(c, "to", "%s", T("That's past the end of the episode"));
            return;
        }
        put_row(&row, ST_HAND, from, to, 0, NULL);
        recount();
        char a[16], z[16];
        clock_ms(a, sizeof a, from);
        clock_ms(z, sizeof z, to);
        log_info("media: intros: %s set %s's intro to %s-%s", c->me->name, e->rel, a, z);
        ctx_ok(c, T("Skip intro goes from %s to %s"), a, z);
    }
    ctx_redirect(c, "%s", back);
    app_changed();
}

/* ================================================================== tests */

int intros_test_step(void) {
    int64_t now = now_ms();
    take_results();
    counted();
    if (wait_reason(now, false)) return -1;
    Job *j = next_job();
    if (!j) return 0;
    atomic_store(&g_halt, 0);
    run_job(j);
    take_back(j);
    return 1;
}

void intros_test_read_s(int seconds) { g_read_s = seconds; }
int intros_test_reads(void) { return atomic_load(&g_reads); }
