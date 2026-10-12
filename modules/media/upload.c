/* modules/media/upload.c — admins add videos to the library from any browser.
 *
 *   POST /media/upload/plan  {"root":"0","into":"Series","files":[["Show/Season 1/x.mkv",123],...]}
 *        -> {"batch":"<id>","todo":[[i,offset],[i,offset,end],...],"skipped":n,"total":bytes}  or  {"error":"...","stop":true}
 *        (with an end, only the bytes before it are wanted: a play-all's header)
 *   PUT  /media/upload/<batch>/<i>?at=<offset>   file i's bytes from offset, streamed to disk
 *   GET  /media/upload/<batch>/<i>               {"at":<bytes saved>}: where to resume
 *
 * With "conv":"1" (the browser can run the converter, and the box is ticked) a video
 * browsers can't all play comes as a copy the browser makes first: its todo entry is
 * {"i":<entry>,"f":<the browser's file>} and the browser, while converting and after,
 *   POST /media/upload/<batch>/<i>?pm=<0..1000>      progress      -> {"ok":true} | {"stop":true,...}
 *   POST /media/upload/<batch>/<i>?size=<bytes>      the copy's size -> {"at":0}: then PUT it
 *   POST /media/upload/<batch>/<i>?original=1&why=.. it couldn't   -> {"at":n} (PUT the original) | {"skip":true}
 *
 * web/wire.js drives it (form[data-upload]): one plan, then one PUT per file.
 * Files are written as "<name>.part" and renamed when complete, so the library
 * never sees half a file and a dropped upload resumes where it stopped. Bytes
 * go socket -> disk on a worker thread through one fixed buffer: memory stays
 * flat whatever the file size, and the event loop never waits for the disk.
 *
 * Admins only, same-origin only, every name checked part by part, and a plan
 * that doesn't fit on the disk is refused before a byte is sent (room.h: 1 GB stays
 * free, and running conversions keep the room they need; each PUT checks again). A copy
 * the server's own converter is making is never taken; one only queued there leaves the
 * converter's queue - the upload brings it. The converter asks here too (upload_claims).
 * The page is rendered here like any other, so progress shows on every screen. */
#include <stdatomic.h>

#include "../../core/assets.h"
#include "../../core/json.h"
#include "../../core/ui.h"
#include "convert.h"
#include "room.h"
#include "upload.h"

#define MAX_BATCHES 4
#define MAX_FILES 5000
#define MAX_WORKERS 2
#define CHUNK (512 * 1024)
#define STALL_MS 60000
#define KEEP_DONE_S (60 * 60)      /* finished uploads stay on screen this long */
#define FORGET_IDLE_S (24 * 3600)  /* an abandoned one is forgotten after a day */
#define ART_MAX (25LL << 20)       /* a poster, not a photo archive */
#define PLAYALL_HEAD (1LL << 20)   /* of a play-all, just this much: its header, with the duration */

enum { UP_QUEUED, UP_SKIPPED, UP_WRITING, UP_DONE, UP_FAILED };
/* a play-everywhere copy the browser makes first: size unknown yet / known / it couldn't, the original goes */
enum { CV_NONE, CV_WAIT, CV_READY, CV_ORIGINAL };

typedef struct {
    char rel[400]; /* below the root, '/' separators, checked */
    int64_t size, saved;
    int state;
    bool head;    /* only the first `size` bytes of the browser's file: a play-all's header */
    char why[96]; /* why it was skipped or failed (or, CV_ORIGINAL, why no copy was made) */
    int src;      /* the browser's file this comes from: its own index, or the original's for an added copy */
    int conv, pm; /* CV_*; the browser's progress converting it, 0..1000 */
    bool extra;   /* a copy added next to its original ("also send originals"): dropped when it can't be made */
    bool dupe;    /* skipped: the library has this episode or film under another name */
    bool on_server; /* skipped midway: the server's converter makes this copy (answered like a sent file) */
    int64_t orig; /* the original's size (copies) */
    char ext[8];  /* the original's extension, to fall back to its name */
} UpFile;

typedef struct {
    uint64_t id;
    int person;
    uint64_t art; /* a picture for this series/film (one file, replaces the old one); 0 = videos */
    char root[300];
    UpFile *files;
    int nfiles;
    int64_t total, moved;         /* bytes to send in this batch / received so far */
    int64_t started, last, ended; /* unix seconds */
    int64_t busy; /* the browser's last sign of work on it (a PUT, a copy's progress): 0 = none yet */
    int workers, current, done, skipped, failed;
    bool stopped;
    double rate;                 /* bytes per second, smoothed (main thread) */
    int64_t rate_at, rate_moved; /* last speed sample: monotonic ms, moved */
} Batch;

/* Workers update progress, so everything below is guarded by g_lock. Lock order: g_lock may
   be held while asking convert.c (which takes its own), never the other way round. */
static Lock g_lock = LOCK_INIT;
static Batch *g_batch[MAX_BATCHES];
static int g_workers;
/* files saved ever - a finished batch can be replaced before a tick sees it - so the tick
   rescans when these grow */
static int g_files_saved, g_pictures_saved;
static atomic_bool g_halt;
/* main thread only */
static int64_t g_rescan_at, g_rescan_first, g_since; /* g_since: when this server started */
static uint64_t g_shown;
static int g_done_seen, g_pictures_seen;

void upload_test_free_space(int64_t bytes) { media_test_free_space(bytes); }

/* ---------------------------------------------------------------- helpers */

static const Asset *asset_named(const char *path) { /* a file baked into this binary (tools/embed.c) */
    for (int i = 0; i < g_nassets; i++)
        if (g_assets[i].path && str_eq(g_assets[i].path, path)) return &g_assets[i];
    return NULL;
}

static bool ieq3(const char *a, const char *b) {
    for (int i = 0; i < 3; i++)
        if ((a[i] | 0x20) != b[i]) return false;
    return true;
}

/* One path part as Windows, macOS and Linux will all store it. */
static bool part_ok(const char *s, size_t n) {
    if (!n || n > 200) return false;
    if (s[0] == ' ' || s[n - 1] == ' ' || s[n - 1] == '.') return false; /* Windows drops these silently */
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7F || strchr("\\:*?\"<>|", ch)) return false;
    }
    size_t stem = strcspn(s, ".");
    if (stem > n) stem = n;
    if (stem == 3 && (ieq3(s, "con") || ieq3(s, "prn") || ieq3(s, "aux") || ieq3(s, "nul"))) return false;
    if (stem == 4 && (ieq3(s, "com") || ieq3(s, "lpt")) && s[3] >= '1' && s[3] <= '9') return false;
    return utf8_valid(s, n);
}

bool upload_clean_rel(const char *in, char *out, size_t cap) {
    if (!in || !*in || *in == '/' || *in == '\\') return false; /* nothing absolute */
    Buf b = {0};
    bool ok = true;
    for (const char *p = in; ok && *p;) {
        size_t n = strcspn(p, "/\\");
        ok = part_ok(p, n); /* also refuses "", "." and ".." (they end in '.') */
        if (ok) {
            if (b.len) buf_putc(&b, '/');
            buf_put(&b, p, n);
        }
        p += n;
        if (*p) p++;
    }
    ok = ok && b.len && b.len < cap;
    if (ok) memcpy(out, b.p, b.len + 1);
    buf_free(&b);
    return ok;
}

static void join(char *out, size_t cap, const char *root, const char *rel) {
    size_t rl = strlen(root);
    bool slash = rl && (root[rl - 1] == '/' || root[rl - 1] == '\\');
    snprintf(out, cap, "%s%s%s", root, slash ? "" : "/", rel);
}

static const char *tail2(const char *rel) { /* "Show/Season 1/Disc - 1/x.mkv" -> "Disc - 1/x.mkv" */
    const char *last = strrchr(rel, '/');
    if (!last) return rel;
    for (const char *p = last - 1; p >= rel; p--)
        if (*p == '/') return p + 1;
    return rel;
}

static bool junk(const char *rel) { /* system clutter that comes along with a picked folder */
    const char *name = strrchr(rel, '/');
    name = name ? name + 1 : rel;
    return name[0] == '.' || str_ieq(name, "Thumbs.db") || str_ieq(name, "desktop.ini");
}

static int64_t part_size(const Batch *b, const UpFile *f, int64_t *mtime) {
    char path[1024];
    int64_t size = 0;
    join(path, sizeof path - 8, b->root, f->rel);
    strcat(path, ".part");
    return path_stat(path, &size, mtime, NULL) ? size : 0;
}

static void drop_part(const Batch *b, const UpFile *f) {
    char path[1024];
    join(path, sizeof path - 8, b->root, f->rel);
    strcat(path, ".part");
    file_remove(path);
}

/* Where file f continues: its half-sent ".part", unless that can't be this file - longer than
   it (a leftover from another file of that name), a picture (always one name, poster.jpg,
   so a leftover is likely another picture), or a copy's ".part" from before this server
   started (older Hemmas' converter wrote its half-made copies as "x.web.mp4.part" too: never
   resumed from; a copy whose upload a restart cut off is sent again): then the leftover goes
   and it starts over. */
static int64_t resume_point(const Batch *b, const UpFile *f) {
    int64_t mtime = 0, have = part_size(b, f, &mtime);
    if (have && (have > f->size || b->art || (media_is_web_copy(f->rel) && mtime < g_since))) {
        drop_part(b, f);
        have = 0;
    }
    return have;
}

static Batch *find(uint64_t id) {
    for (int i = 0; i < MAX_BATCHES; i++)
        if (g_batch[i] && g_batch[i]->id == id) return g_batch[i];
    return NULL;
}

static void batch_free(Batch *b) {
    free(b->files);
    free(b);
}

static bool batch_open(const Batch *b) {
    if (b->stopped) return false;
    for (int i = 0; i < b->nfiles; i++)
        if (b->files[i].state == UP_QUEUED || b->files[i].state == UP_WRITING) return true;
    return false;
}

/* --------------------------------------------------------------- replies */

static void reply(Conn *conn, int status, const Buf *json) {
    conn_reply(conn, status, "application/json", json->p ? json->p : "{}", json->len, "Cache-Control: no-store\r\n");
}

static void reply_err(Conn *conn, int status, bool stop, const char *msg) {
    Buf j = {0};
    buf_puts(&j, "{\"error\":");
    json_str(&j, msg);
    buf_printf(&j, ",\"stop\":%s}", stop ? "true" : "false");
    reply(conn, status, &j);
    buf_free(&j);
}

/* Worker side: send on a socket that is no longer the event loop's (5 s at most). */
static void worker_send(sock_t s, const char *p, size_t n) {
    for (int waited = 0; n && waited < 5000;) {
        long k = sock_send(s, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        if (k < 0 && !sock_again(sock_errno())) break;
        pollfd_t pf = {.fd = s, .events = POLLOUT};
        if (plat_poll(&pf, 1, 500) == 0) waited += 500;
    }
}

/* After refusing a body midway: read (and drop) what the browser is still sending for a
   moment, so it gets to read the answer ("disk full") instead of a reset connection. */
static void drain(sock_t s, int64_t left) {
    char tmp[16384];
    left = MIN(left, 64LL << 20);
    for (int64_t end = now_ms() + 2000; left > 0 && now_ms() < end;) {
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) {
            left -= k;
            continue;
        }
        if (k == 0 || !sock_again(sock_errno())) break;
        pollfd_t pf = {.fd = s, .events = POLLIN};
        plat_poll(&pf, 1, 100);
    }
}

static void worker_reply(sock_t s, int status, const char *json) {
    Buf b = {0};
    http_reply(&b, status, "application/json", json, strlen(json), "Cache-Control: no-store\r\n", false);
    worker_send(s, b.p, b.len);
    buf_free(&b);
}

/* DVD rips carry a "play all" title per disc: every episode on the disc again, as one
   file (the owner's season 1 has four, 27 GB of duplicates). Sizes are all the plan
   knows, so: a video is a play-all when it is 2.5× bigger than anything else in its
   folder and at least three folder-mates of between a twelfth and half its size add
   up to about it (0.7-1.4×). A movie next to small trailers, a two-part film, a long
   special among episodes or an episode next to bonus clips never match. Folder-mates
   count whether they still need sending or are on the server already.
   The play-all's body stays behind, but an MKV's first MB goes as "<name>.playall.mkv":
   its header says how long the disc is, and that is how the library tells a 23-minute
   featurette from an episode once the play-all itself is missing (library.c). */
static bool same_dir(const char *a, const char *b) {
    const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
    size_t la = sa ? (size_t)(sa - a) : 0, lb = sb ? (size_t)(sb - b) : 0;
    return la == lb && strncmp(a, b, la) == 0;
}

/* A subtitle or picture that goes with a video the library already has ("x.en.srt", "x.jpg",
   "x-poster.jpg" next to x.mkv) stays behind with it - unless a video that is sent shares the name. */
static bool goes_with(const char *rel, const char *video) {
    size_t sl = media_stem_len(video);
    return same_dir(rel, video) && strncmp(rel, video, sl) == 0 && (rel[sl] == '.' || rel[sl] == '-');
}

static void skip_companions(Batch *b) {
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED || media_is_video_ext(f->rel)) continue;
        const UpFile *with = NULL;
        bool kept = false;
        for (int k = 0; k < b->nfiles && !kept; k++) {
            const UpFile *v = &b->files[k];
            if (k == i || !media_is_video_ext(v->rel) || !goes_with(f->rel, v->rel)) continue;
            if (v->state == UP_QUEUED) kept = true;
            else if (v->dupe && !with) with = v;
        }
        if (with && !kept) {
            char why[440];
            const char *name = strrchr(with->rel, '/');
            snprintf(why, sizeof why, "goes with %s, which the library has", name ? name + 1 : with->rel);
            str_copy(f->why, sizeof f->why, why);
            f->state = UP_SKIPPED;
        }
    }
}

static bool original_video(const UpFile *f) { return f->size > 0 && media_is_video_ext(f->rel) && !media_is_web_copy(f->rel); }

static void play_alls(Batch *b) {
    int *parts = xcalloc((size_t)b->nfiles + 1, sizeof *parts); /* decided first: sizes change below */
    for (int i = 0; i < b->nfiles; i++) {
        const UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED || !original_video(f) || f->size < (64LL << 20)) continue;
        int n = 0;
        int64_t sum = 0, biggest = 0;
        for (int k = 0; k < b->nfiles; k++) {
            const UpFile *o = &b->files[k];
            if (k == i || !original_video(o) || !same_dir(f->rel, o->rel)) continue;
            biggest = MAX(biggest, o->size);
            if (o->size * 12 >= f->size && o->size * 2 <= f->size) {
                n++;
                sum += o->size;
            }
        }
        /* ...and it dwarfs everything else in the folder (an episode next to a few bonus
           clips that happen to add up to it does not) */
        if (n >= 3 && sum * 10 >= f->size * 7 && sum * 10 <= f->size * 14 && f->size * 10 >= biggest * 25) parts[i] = n;
    }
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        if (!parts[i]) continue;
        char head[440], full[1024];
        size_t stem = strlen(f->rel) - 4;
        snprintf(head, sizeof head, "%.*s.playall.mkv", (int)stem, f->rel);
        if (!str_iends(f->rel, ".mkv") || strlen(head) > 380) {
            f->state = UP_SKIPPED;
            snprintf(f->why, sizeof f->why, "looks like a \"play all\" copy of the %d episodes next to it", parts[i]);
            continue;
        }
        str_copy(f->rel, sizeof f->rel, head);
        f->size = MIN(f->size, PLAYALL_HEAD);
        f->head = true;
        int64_t have = 0;
        bool dir = false;
        join(full, sizeof full, b->root, f->rel);
        if (path_stat(full, &have, NULL, &dir)) {
            f->state = UP_SKIPPED;
            if (dir || have != f->size) str_copy(f->why, sizeof f->why, "a different file with that name is already there");
            else
                snprintf(f->why, sizeof f->why, "a \"play all\" copy of the %d episodes next to it - its header is there",
                         parts[i]);
            continue;
        }
        f->saved = resume_point(b, f);
    }
    free(parts);
}

/* Is root/rel another upload's? Being written by any, or still to come in someone else's open
   upload (a person's own earlier upload is replaced by their new plan - unless it's sending,
   and then the new plan is refused anyway). */
#define ACTIVE_S 30 /* an upload whose browser did something this recently is running */

/* Running: sending now, or its browser worked on it in the last ACTIVE_S seconds (making
   copies, between files). A closed tab's upload stops counting after that, and picking the
   same files again resumes it. Pictures don't count. Lock held. */
static bool running(const Batch *o) {
    return o && !o->art && (o->workers || (batch_open(o) && o->busy && now_sec() - o->busy < ACTIVE_S));
}

/* Is root/rel another upload's? Being written, or still to come in a running upload. (A
   person's earlier upload that isn't running is replaced by their new plan.) */
static bool claimed(const char *root, int me, const char *rel) {
    (void)me;
    bool mine = false;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES && !mine; s++) {
        const Batch *o = g_batch[s];
        if (!o || o->stopped || !str_eq(o->root, root)) continue;
        bool live = running(o);
        for (int p = 0; p < o->nfiles && !mine; p++) {
            const UpFile *x = &o->files[p];
            mine = (x->state == UP_WRITING || (x->state == UP_QUEUED && live)) && str_eq(x->rel, rel);
        }
    }
    lock_give(&g_lock);
    return mine;
}

/* Running, or planned a moment ago and not yet heard from: what the converter must keep its
   hands off, and whose bytes still to come hold room on the disk (lock held). */
static bool live(const Batch *o) {
    return o && !o->art && !o->stopped && batch_open(o) && (running(o) || now_sec() - o->started < ACTIVE_S);
}

/* What live uploads have left to send (lock held), except `skip` and `me`'s own uploads that
   aren't running (a new plan of theirs replaces those). */
static int holds_locked(MediaHold *out, int max, const Batch *skip, int me) {
    int n = 0;
    for (int s = 0; s < MAX_BATCHES && n < max; s++) {
        const Batch *o = g_batch[s];
        if (!live(o) || o == skip || (me && o->person == me && !running(o))) continue;
        str_copy(out[n].root, sizeof out[n].root, o->root);
        out[n].bytes = MAX(o->total - o->moved, 0);
        n++;
    }
    return n;
}

int upload_holds(MediaHold *out, int max) {
    lock_take(&g_lock);
    int n = holds_locked(out, max, NULL, 0);
    lock_give(&g_lock);
    return n;
}

/* Spare room for upload b's files below root: past the reserve, running conversions and other
   live uploads (`me`: leave out that person's uploads a new plan replaces). The holds are
   copied under the lock, the disk is asked outside it: a slow disk mustn't stall the uploads
   or the pages. Lock NOT held. */
static int64_t spare_for(const Batch *b, const char *root, int me, int64_t *held) {
    MediaHold holds[MEDIA_MAX_HOLDS];
    lock_take(&g_lock);
    int n = holds_locked(holds, MEDIA_MAX_HOLDS, b, me);
    lock_give(&g_lock);
    n += convert_holds(holds + n, MEDIA_MAX_HOLDS - n);
    return media_room(root, holds, n, held);
}

/* "No room for the copy: it needs 4 GB, and the drive has 2 GB to spare (Hemma keeps 1 GB free)" */
static void no_room(char *out, size_t cap, const char *what, int64_t need, int64_t spare, int64_t held) {
    char n[24], tail[200];
    media_size_text(need, n, sizeof n);
    media_room_text(tail, sizeof tail, spare, held);
    snprintf(out, cap, "No room for %s: it needs %s, and %s", what, n, tail);
}

bool upload_claims(const char *path) {
    bool mine = false;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES && !mine; s++) {
        const Batch *o = g_batch[s];
        size_t rl = o ? strlen(o->root) : 0;
        if (!o || !str_istarts(path, o->root)) continue;
        const char *rel = path + rl; /* path = root/rel, as join() makes it */
        if (rl && o->root[rl - 1] != '/' && *rel++ != '/') continue;
        bool coming = live(o);
        for (int p = 0; p < o->nfiles && !mine; p++) {
            const UpFile *x = &o->files[p];
            mine = (x->state == UP_WRITING || (x->state == UP_QUEUED && coming)) && str_ieq(x->rel, rel);
        }
    }
    lock_give(&g_lock);
    return mine;
}

static bool listed(const Batch *b, const char *rel) {
    for (int k = 0; k < b->nfiles; k++)
        if (str_eq(b->files[k].rel, rel)) return true;
    return false;
}

/* A folder converted by Hemma holds "x.mkv" next to "x.web.mp4": the copy is what plays,
   at about a third of the size, so unless asked for the originals only the copy goes. */
static void skip_originals(Batch *b) {
    for (int i = 0; i < b->nfiles; i++) {
        UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED || !media_is_video_ext(f->rel) || media_is_web_copy(f->rel)) continue;
        char copy[420];
        convert_web_path(f->rel, copy, sizeof copy);
        if (listed(b, copy)) {
            f->state = UP_SKIPPED;
            str_copy(f->why, sizeof f->why, "its play-everywhere copy (.web.mp4) goes instead");
        }
    }
}

/* Without such a copy, an MKV (DVD rips, Dolby sound - what browsers can't all play) is
   converted by the uploading browser itself: web/conv.js runs av/conv.c as WebAssembly on
   that device, and only "x.web.mp4" travels - a third of the bytes over the network, and
   the original never takes room here. Its size is known once the copy exists (?size=);
   one the converter can't handle comes as it is (?original=1). With "also send originals"
   the copy is an added entry next to the original. */
static bool convertible(const Batch *b, const UpFile *f, char *copy, size_t cap) {
    if (f->state != UP_QUEUED || f->head || f->conv || !original_video(f)) return false;
    if (!str_iends(f->rel, ".mkv") && !str_iends(f->rel, ".webm")) return false; /* the demuxer we have */
    convert_web_path(f->rel, copy, cap);
    return strlen(copy) <= 380 && !listed(b, copy); /* a copy among the chosen files goes as it is */
}

static void convert_here(Batch *b, bool keep_originals, int me) {
    char copy[420], full[1024];
    int n = b->nfiles, add = 0;
    for (int i = 0; i < n; i++) add += convertible(b, &b->files[i], copy, sizeof copy);
    if (!add) return;
    if (keep_originals) {
        b->files = xrealloc(b->files, (size_t)(n + add) * sizeof *b->files);
        memset(b->files + n, 0, (size_t)add * sizeof *b->files);
    }
    for (int i = 0; i < n; i++) {
        if (!convertible(b, &b->files[i], copy, sizeof copy)) continue; /* (fewer than counted, never more) */
        UpFile *o = &b->files[i], *f = keep_originals ? &b->files[b->nfiles++] : o;
        f->orig = o->size;
        str_copy(f->ext, sizeof f->ext, strrchr(o->rel, '.') + 1);
        f->src = i;
        f->extra = f != o;
        str_copy(f->rel, sizeof f->rel, copy);
        f->size = f->saved = 0;
        f->pm = 0;
        join(full, sizeof full, b->root, copy);
        bool there = path_stat(full, NULL, NULL, NULL), server = !there && convert_making(full);
        if (there || server || claimed(b->root, me, copy)) {
            f->state = UP_SKIPPED;
            str_copy(f->why, sizeof f->why,
                     there    ? "its play-everywhere copy is already there"
                     : server ? "Hemma is making its play-everywhere copy on the server right now"
                              : "another upload is sending its copy right now");
            continue;
        }
        f->state = UP_QUEUED;
        f->conv = CV_WAIT;
    }
}

static int64_t copy_guess(const UpFile *f) { return f->orig / 2; } /* DVD copies come out at about a third: half is on the safe side */

/* Bytes a queued file still has to send (a copy not made yet: a guess). */
static int64_t left_to_send(const UpFile *f) { return f->conv == CV_WAIT ? copy_guess(f) : MAX(f->size - f->saved, 0); }

#define MAKING_IT "Hemma is making this copy on the server right now"

/* The server's converter is making this copy, so it comes from there: a plan leaves it out, and
   an upload that went quiet and comes back hears "sent" for it and goes on with the rest. Its
   ".part" from an earlier try goes. Lock held (or b not shared yet), and f has no worker. */
static void leave_to_server(Batch *b, UpFile *f, const char *why) {
    b->total -= left_to_send(f);
    drop_part(b, f);
    f->state = UP_SKIPPED;
    f->on_server = true;
    b->skipped++;
    str_copy(f->why, sizeof f->why, why);
}

/* Room on the disk for what b has to send (spare_for); false with the reason in err. Lock NOT held. */
static bool room_for(const Batch *b, int me, char *err, size_t cap) {
    int64_t held = 0, spare = spare_for(b, b->root, me, &held);
    if (b->total <= spare) return true;
    char need[24], tail[200];
    media_size_text(b->total, need, sizeof need);
    media_room_text(tail, sizeof tail, spare, held);
    snprintf(err, cap, "Not enough room: this needs %s, and %s", need, tail);
    return false;
}

/* ------------------------------------------------------------------- plan */

/* {"art":"<series or film id>","files":[["My picture.jpeg",123]]}: one picture, saved as
   "<show folder>/poster.jpg" or "<film folder>/<film name>.jpg" over whatever was there. */
static bool art_plan(Json *j, int at, Batch **out, char *err, size_t cap) {
    char hex[40] = "", dir[400], stem[400], raw[1024] = "";
    uint64_t id = 0;
    int root = -1;
    json_string(j, at, hex, sizeof hex);
    if (!media_parse_id(hex, &id) || !media_art_target(id, &root, dir, sizeof dir, stem, sizeof stem) ||
        root < 0 || root >= media_lib()->nroots) {
        snprintf(err, cap, "Pictures go with a series or a film in the library - this one isn't there any more");
        return false;
    }
    int arr = json_get(j, 0, "files");
    int n = json_type(j, arr) == JSON_ARRAY ? j->tok[arr].count : 0;
    if (n != 1) {
        snprintf(err, cap, n ? "Choose just one picture" : "Choose a picture first");
        return false;
    }
    long long size = -1;
    int k = json_child(j, arr);
    json_string(j, json_index(j, k, 0), raw, sizeof raw);
    json_integer(j, json_index(j, k, 1), &size);
    if (!media_is_image_ext(raw)) {
        snprintf(err, cap, "Pictures can be JPEG, PNG or WebP");
        return false;
    }
    if (size <= 0 || size > ART_MAX) {
        snprintf(err, cap, size > 0 ? "That picture is too big - 25 MB at most" : "That picture is empty");
        return false;
    }
    char ext[8];
    str_copy(ext, sizeof ext, strrchr(raw, '.') + 1);
    for (char *p = ext; *p; p++) *p = (char)(*p | 0x20);
    if (str_eq(ext, "jpeg")) str_copy(ext, sizeof ext, "jpg");
    Batch *b = xcalloc(1, sizeof *b);
    b->files = xcalloc(1, sizeof *b->files);
    b->nfiles = 1;
    b->art = id;
    str_copy(b->root, sizeof b->root, media_lib()->roots[root]);
    UpFile *f = &b->files[0];
    f->size = size;
    if (*dir) snprintf(f->rel, sizeof f->rel, "%s/%s.%s", dir, stem, ext);
    else snprintf(f->rel, sizeof f->rel, "%s.%s", stem, ext);
    f->state = UP_QUEUED;
    f->saved = resume_point(b, f);
    *out = b;
    return true;
}

static void plan(Ctx *c, Conn *conn, HttpReq *r) {
    int maxtok = 64 + MAX_FILES * 3;
    JsonTok *tok = xmalloc((size_t)maxtok * sizeof *tok);
    Json j;
    Batch *b = NULL;
    char err[200] = "";
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS);
    if (!r->body || !json_parse(&j, r->body, r->body_len, tok, maxtok) || json_type(&j, 0) != JSON_OBJECT) {
        snprintf(err, sizeof err, "That upload list didn't make sense (more than %d files?)", MAX_FILES);
        goto out;
    }
    int art = json_get(&j, 0, "art");
    if (art >= 0) {
        if (!art_plan(&j, art, &b, err, sizeof err)) goto out;
        goto planned;
    }
    char tmp[1024];
    long long ri = -1;
    int rt = json_get(&j, 0, "root");
    if (json_type(&j, rt) == JSON_STRING && json_string(&j, rt, tmp, sizeof tmp)) ri = str_int_or(tmp, -1);
    else json_integer(&j, rt, &ri);
    if (ri < 0 || ri >= nroots) {
        snprintf(err, sizeof err, "Choose which library folder to upload into");
        goto out;
    }
    if (!path_is_dir(roots[ri])) {
        snprintf(err, sizeof err, "Can't reach the library folder %s right now", roots[ri]);
        goto out;
    }
    char into[256] = "";
    if (json_string(&j, json_get(&j, 0, "into"), tmp, sizeof tmp)) {
        str_trim(tmp);
        if (*tmp && !upload_clean_rel(tmp, into, 200)) {
            snprintf(err, sizeof err, "The folder name \"%.80s\" has characters a disk can't store", tmp);
            goto out;
        }
    }
    int arr = json_get(&j, 0, "files");
    int n = json_type(&j, arr) == JSON_ARRAY ? j.tok[arr].count : 0;
    if (n <= 0) {
        snprintf(err, sizeof err, "No files were chosen");
        goto out;
    }
    if (n > MAX_FILES) {
        snprintf(err, sizeof err, "That's more than %d files - upload a few folders at a time", MAX_FILES);
        goto out;
    }

    b = xcalloc(1, sizeof *b);
    b->files = xcalloc((size_t)n, sizeof *b->files);
    b->nfiles = n;
    str_copy(b->root, sizeof b->root, roots[ri]);
    /* episodes and films the library has under another name stay behind (before any converting) */
    bool dupes = json_string(&j, json_get(&j, 0, "dupes"), tmp, sizeof tmp) && str_eq(tmp, "1");
    int i = 0;
    for (int k = json_child(&j, arr); k >= 0 && i < n; k = json_next(&j, arr, k), i++) {
        UpFile *f = &b->files[i];
        long long size = -1;
        char raw[1024] = "", clean[400], full[1024];
        json_string(&j, json_index(&j, k, 0), raw, sizeof raw);
        json_integer(&j, json_index(&j, k, 1), &size);
        f->state = UP_SKIPPED;
        f->src = i;
        str_copy(f->rel, sizeof f->rel, raw);
        if (size < 0) {
            str_copy(f->why, sizeof f->why, "the browser didn't say how big it is");
            continue;
        }
        f->size = size;
        if (!upload_clean_rel(raw, clean, sizeof clean)) {
            str_copy(f->why, sizeof f->why, "its name has characters a disk can't store");
            continue;
        }
        if (*into) snprintf(f->rel, sizeof f->rel, "%s/%s", into, clean);
        else str_copy(f->rel, sizeof f->rel, clean);
        if (strlen(f->rel) > 380) {
            str_copy(f->why, sizeof f->why, "the path is too long");
            continue;
        }
        if (junk(f->rel)) {
            str_copy(f->why, sizeof f->why, "a system file");
            continue;
        }
        if (!media_is_video_ext(f->rel) && !media_is_sub_ext(f->rel) && !media_is_image_ext(f->rel)) {
            str_copy(f->why, sizeof f->why, "not a video, subtitle or poster");
            continue;
        }
        bool twice = false;
        for (int p = 0; p < i && !twice; p++) twice = b->files[p].state != UP_SKIPPED && str_eq(b->files[p].rel, f->rel);
        if (twice) {
            str_copy(f->why, sizeof f->why, "it's on the list twice");
            continue;
        }
        if (claimed(b->root, c->me->id, f->rel)) {
            str_copy(f->why, sizeof f->why, "another upload is sending it right now");
            continue;
        }
        int64_t have = 0;
        bool dir = false;
        join(full, sizeof full, b->root, f->rel);
        if (path_stat(full, &have, NULL, &dir)) {
            str_copy(f->why, sizeof f->why,
                     dir               ? "a folder on the server has that name"
                     : have == f->size ? "it's already there"
                                       : "a different file with that name is already there");
            continue;
        }
        if (media_is_video_ext(f->rel) && convert_making(full)) { /* the server is making this copy: one writer */
            str_copy(f->why, sizeof f->why, MAKING_IT);
            continue;
        }
        char found[400], why[440];
        if (!dupes && media_is_video_ext(f->rel) && media_have(b->root, f->rel, found, sizeof found)) {
            snprintf(why, sizeof why, "the library has it: %s", tail2(found));
            str_copy(f->why, sizeof f->why, why); /* (cut at a whole character) */
            f->dupe = true;
            continue;
        }
        f->state = UP_QUEUED;
        f->saved = resume_point(b, f);
    }
    skip_companions(b);
    play_alls(b);
    bool originals = json_string(&j, json_get(&j, 0, "originals"), tmp, sizeof tmp) && str_eq(tmp, "1");
    if (!originals) skip_originals(b);
    if (json_string(&j, json_get(&j, 0, "conv"), tmp, sizeof tmp) && str_eq(tmp, "1")) convert_here(b, originals, c->me->id);
planned:
    for (int k = 0; k < b->nfiles; k++) {
        const UpFile *f = &b->files[k];
        b->skipped += f->state == UP_SKIPPED;
        if (f->state == UP_QUEUED) b->total += left_to_send(f);
    }
    if (!room_for(b, c->me->id, err, sizeof err)) goto out;

    /* One upload at a time (owner's rule): while one is running - anyone's, in any tab - a new
       one is refused, unless "upload anyway" was ticked; then both run, and the files the running
       one still has to send were skipped above (claimed). A person's earlier upload that isn't
       running is replaced (picking the same files resumes it). A picture only replaces their
       previous picture, so one can be set while videos go up. */
    bool force = json_string(&j, json_get(&j, 0, "force"), tmp, sizeof tmp) && str_eq(tmp, "1");
    lock_take(&g_lock);
    int slot = -1;
    const Batch *other = NULL;
    for (int s = 0; s < MAX_BATCHES && !b->art; s++)
        if (running(g_batch[s])) other = g_batch[s];
    if (other && !force) {
        const Person *p = person_get(other->person);
        int todo = other->nfiles - other->skipped;
        if (other->person == c->me->id)
            snprintf(err, sizeof err, "Your upload is still running (%d of %d files, in another tab?) - let it finish, stop it, "
                                      "or tick \"Upload anyway\"", other->done, todo);
        else
            snprintf(err, sizeof err, "%s's upload is running (%d of %d files) - let it finish, stop it, or tick \"Upload "
                                      "anyway\"", p ? p->name : "Someone", other->done, todo);
    }
    for (int s = 0; s < MAX_BATCHES && !err[0]; s++) {
        Batch *o = g_batch[s];
        if (o && o->person == c->me->id && !o->art == !b->art && !running(o) && !o->workers) {
            batch_free(o);
            g_batch[s] = NULL;
        }
    }
    for (int s = 0; s < MAX_BATCHES && slot < 0 && !err[0]; s++)
        if (!g_batch[s]) slot = s;
    for (int s = 0; s < MAX_BATCHES && slot < 0 && !err[0]; s++) /* full: drop a finished one */
        if (g_batch[s] && !g_batch[s]->workers && !batch_open(g_batch[s])) {
            batch_free(g_batch[s]);
            g_batch[s] = NULL;
            slot = s;
        }
    if (slot < 0 && !err[0]) snprintf(err, sizeof err, "Too many uploads at once - try again in a while");
    if (!err[0]) {
        rand_bytes(&b->id, sizeof b->id);
        b->person = c->me->id;
        b->started = b->last = now_sec();
        b->current = -1;
        g_batch[slot] = b;
    }
    lock_give(&g_lock);
    if (err[0]) goto out;

    /* Registered, so now a claim that conversions see. The room once more, with all that holds
       room now: a conversion that started since the first look can't count on the same room
       (convert.c checks again after marking one running, too). */
    if (!b->art && !room_for(b, c->me->id, err, sizeof err)) {
        lock_take(&g_lock);
        g_batch[slot] = NULL; /* (nobody has its id yet) */
        lock_give(&g_lock);
        goto out;
    }
    /* Conversions still waiting to make a copy this brings leave the queue: the upload's copy
       does instead. One that started meanwhile keeps it. */
    lock_take(&g_lock);
    for (int k = 0; k < b->nfiles && !b->art; k++) {
        UpFile *f = &b->files[k];
        char full[1024];
        if (f->state != UP_QUEUED || !media_is_video_ext(f->rel)) continue;
        join(full, sizeof full, b->root, f->rel);
        if (convert_give_way(full)) leave_to_server(b, f, MAKING_IT);
    }
    lock_give(&g_lock);

    Buf out = {0};
    char hex[17];
    media_id_hex(b->id, hex);
    buf_printf(&out, "{\"batch\":\"%s\",\"todo\":[", hex);
    int k = 0;
    for (i = 0; i < b->nfiles; i++) {
        const UpFile *f = &b->files[i];
        if (f->state != UP_QUEUED) continue;
        if (f->conv == CV_WAIT) { /* the browser converts its file f, then sends the copy as entry i */
            buf_printf(&out, "%s{\"i\":%d,\"f\":%d}", k++ ? "," : "", i, f->src);
            continue;
        }
        buf_printf(&out, "%s[%d,%lld", k++ ? "," : "", i, (long long)f->saved);
        if (f->head) buf_printf(&out, ",%lld", (long long)f->size); /* the browser sends bytes [saved, size) */
        buf_putc(&out, ']');
    }
    buf_printf(&out, "],\"skipped\":%d,\"total\":%lld}", b->skipped, (long long)b->total);
    reply(conn, 200, &out);
    buf_free(&out);
    if (!k) { /* nothing to send: done already */
        lock_take(&g_lock);
        b->ended = now_sec();
        lock_give(&g_lock);
    }
    if (b->art) log_info("upload: %s sets the picture %s/%s", c->me->name, b->root, b->files[0].rel);
    else
        log_info("upload: %s plans %d files (%d skipped) into %s", c->me->name, b->nfiles - b->skipped, b->skipped,
                 b->root);
    b = NULL; /* owned by g_batch now */
    app_changed_where("/media");

out:
    if (err[0]) reply_err(conn, 422, true, err);
    if (b) batch_free(b);
    free(tok);
}

/* ----------------------------------------------------------------- worker */

typedef struct {
    Batch *b;
    int i;
    sock_t s;
    Buf head, body; /* unsent output to flush first / body bytes already received */
    int64_t at, len;
    char full[1024], part[1040];
} Job;

static void progress(Job *j, int64_t n) {
    lock_take(&g_lock);
    j->b->moved += n;
    j->b->files[j->i].saved += n;
    j->b->last = j->b->busy = now_sec();
    lock_give(&g_lock);
}

static bool halted(const Batch *b) { /* the module stops, or someone pressed Stop on this upload */
    if (atomic_load(&g_halt)) return true;
    lock_take(&g_lock);
    bool s = b->stopped;
    lock_give(&g_lock);
    return s;
}

/* A picture has to start like a JPEG, PNG or WebP file (whatever its name says). */
static bool picture_bytes(const char *path) {
    unsigned char h[12] = {0};
    FILE *f = file_open(path, "rb");
    if (!f) return false;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF) return true;
    if (n >= 8 && memcmp(h, "\x89PNG\r\n\x1a\n", 8) == 0) return true;
    return n >= 12 && memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WEBP", 4) == 0;
}

static void worker(void *arg) {
    Job *j = arg;
    Batch *b = j->b;
    const char *fail = NULL;
    int status = 200;
    bool stop = false;
    sock_nonblock(j->s, true);
    if (j->head.len) worker_send(j->s, j->head.p, j->head.len); /* rare: answers to earlier pipelined requests */

    char dir[1024];
    str_copy(dir, sizeof dir, j->full);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    FILE *f = NULL;
    if (!dir_make(dir)) fail = "Couldn't create the folder on the server", status = 500, stop = true;
    else if (!(f = file_open(j->part, j->at ? "r+b" : "wb"))) fail = "Couldn't create the file on the server", status = 500, stop = true;
    else if (j->at && !file_seek(f, j->at)) fail = "Couldn't continue the file", status = 409;

    int64_t got = 0;
    if (!fail && j->body.len) {
        size_t n = (size_t)MIN((int64_t)j->body.len, j->len);
        if (fwrite(j->body.p, 1, n, f) != n) fail = "The server's disk is full", status = 507, stop = true;
        else {
            got += (int64_t)n;
            progress(j, (int64_t)n);
        }
    }
    char *buf = fail ? NULL : xmalloc(CHUNK);
    for (int waited = 0; !fail && got < j->len;) {
        if (halted(b)) {
            fail = "The upload was stopped", status = 410, stop = true;
            break;
        }
        long k = sock_recv(j->s, buf, (size_t)MIN((int64_t)CHUNK, j->len - got));
        if (k > 0) {
            if (fwrite(buf, 1, (size_t)k, f) != (size_t)k) {
                fail = "The server's disk is full", status = 507, stop = true;
                break;
            }
            got += k;
            waited = 0;
            progress(j, k);
            continue;
        }
        if (k == 0 || !sock_again(sock_errno())) {
            fail = "The connection dropped", status = 0; /* the browser resumes from what we saved */
            break;
        }
        pollfd_t pf = {.fd = j->s, .events = POLLIN};
        int rc = plat_poll(&pf, 1, 1000);
        if (rc < 0) fail = "The connection dropped", status = 0;
        else if (rc == 0 && (waited += 1000) >= STALL_MS) fail = "Nothing arrived for a minute", status = 408;
    }
    free(buf);
    if (f) {
        if (!file_sync(f) && !fail) fail = "The server couldn't save the file", status = 507, stop = true;
        fclose(f);
    }
    bool complete = !fail && got == j->len;
    if (complete && b->art && !picture_bytes(j->part)) {
        fail = "That file isn't a JPEG, PNG or WebP picture", status = 415, stop = true;
        file_remove(j->part);
        complete = false;
    }
    if (complete) {
        if (!b->art && path_stat(j->full, NULL, NULL, NULL))
            fail = "A file with that name appeared meanwhile", status = 409, stop = true;
        else if (!file_replace(j->part, j->full)) fail = "Couldn't finish the file", status = 500, stop = true;
        complete = !fail;
    }
    if (complete && b->art) { /* the new picture wins: drop "poster.png" when "poster.jpg" came, and the like */
        char dir[1024], stem[512];
        str_copy(dir, sizeof dir, j->full);
        char *name = strrchr(dir, '/');
        if (name) {
            *name++ = '\0';
            str_copy(stem, sizeof stem, name);
            char *dot = strrchr(stem, '.');
            if (dot) *dot = '\0';
            media_art_prune(dir, stem, name);
        }
    }

    if (status) {
        Buf out = {0};
        if (complete) buf_puts(&out, "{\"ok\":true}");
        else {
            buf_puts(&out, "{\"error\":");
            json_str(&out, fail);
            buf_printf(&out, ",\"stop\":%s}", stop ? "true" : "false");
        }
        worker_reply(j->s, complete ? 200 : status, buf_cstr(&out));
        buf_free(&out);
    }
    sock_shutdown_write(j->s);
    if (status && !complete) drain(j->s, j->len - got);
    sock_close(j->s);

    lock_take(&g_lock);
    UpFile *uf = &b->files[j->i];
    uf->saved = j->at + got;
    if (complete) {
        uf->state = UP_DONE;
        b->done++;
        g_files_saved += b->art == 0;
        g_pictures_saved += b->art != 0;
    } else if (stop && status != 410) {
        uf->state = UP_FAILED;
        str_copy(uf->why, sizeof uf->why, fail);
        b->failed++;
    } else {
        uf->state = UP_QUEUED; /* resumable */
    }
    if (b->current == j->i) b->current = -1;
    b->workers--;
    g_workers--;
    b->last = b->busy = now_sec();
    lock_give(&g_lock);
    if (complete) log_info("upload: saved %s", j->full);
    else log_warn("upload: %s - %s", j->full, fail);
    app_tick_soon(); /* the tick shows it now (a new picture: at once), not at the next idle tick */
    buf_free(&j->head);
    buf_free(&j->body);
    free(j);
}

/* -------------------------------------------------------------- endpoints */

#define MADE_MEANWHILE "Hemma started making this copy on the server meanwhile"

static void reply_ok(Conn *conn) {
    Buf ok = {0};
    buf_puts(&ok, "{\"ok\":true}");
    reply(conn, 200, &ok);
    buf_free(&ok);
}

static void put(Ctx *c, Conn *conn, HttpReq *r, uint64_t id, int i) {
    lock_take(&g_lock);
    Batch *b = find(id);
    const char *err = NULL;
    char failed[96];
    int status = 409;
    bool stop = true;
    if (!b || b->person != c->me->id) err = "That upload is over - start it again to continue", status = 404;
    else if (b->stopped) err = "The upload was stopped", status = 410;
    else if (i < 0 || i >= b->nfiles || (b->files[i].state == UP_SKIPPED && !b->files[i].on_server))
        err = "That file isn't part of the upload", status = 404;
    else if (b->files[i].state == UP_FAILED) { /* (disk full and the like): say so, don't take it again */
        str_copy(failed, sizeof failed, b->files[i].why);
        err = failed;
    }
    else if (b->files[i].state == UP_DONE || b->files[i].on_server) err = NULL; /* answered below */
    else if (b->files[i].conv == CV_WAIT) err = "That copy isn't made yet", status = 409;
    else if (b->files[i].state == UP_WRITING || g_workers >= MAX_WORKERS) err = "busy", status = 503, stop = false;
    if (err || !b) {
        lock_give(&g_lock);
        reply_err(conn, status, stop, err ? err : "?");
        return;
    }
    UpFile *f = &b->files[i];
    char full[1024];
    join(full, sizeof full, b->root, f->rel);
    /* (an upload back from a quiet spell: a conversion queued meanwhile for this copy leaves the
       queue; one already running keeps it, and this file is answered as sent) */
    if (f->state == UP_DONE || f->on_server || (!b->art && media_is_video_ext(f->rel) && convert_give_way(full))) {
        if (f->state != UP_DONE && !f->on_server) leave_to_server(b, f, MADE_MEANWHILE);
        lock_give(&g_lock);
        reply_ok(conn);
        app_changed_where("/media");
        return;
    }
    Fields q = {0};
    fields_parse(&q, r->query, strlen(r->query));
    int64_t at = fields_int(&q, "at", -1);
    fields_free(&q);
    int64_t disk = resume_point(b, f);
    if (at != disk || at > f->size) { /* tell the browser where we really are */
        lock_give(&g_lock);
        Buf out = {0};
        buf_printf(&out, "{\"error\":\"resume from %lld\",\"at\":%lld}", (long long)disk, (long long)disk);
        reply(conn, 409, &out);
        buf_free(&out);
        return;
    }
    if (r->content_length != f->size - at) {
        lock_give(&g_lock);
        reply_err(conn, 400, true,
                  f->head ? "Hemma was updated meanwhile - reload the page and start the upload again"
                          : "The file changed size since the upload started - start it again");
        return;
    }
    /* Room for the rest: an upload holds room only while it's live, so after a quiet spell (a
       laptop asleep) conversions may have used it. (The disk is asked outside the lock; nothing
       else touches this file meanwhile - claims and PUTs are all on the event loop.) */
    char root[300];
    str_copy(root, sizeof root, b->root);
    lock_give(&g_lock);
    int64_t held = 0, spare = spare_for(b, root, 0, &held);
    lock_take(&g_lock);
    if (f->size - at > spare) {
        char msg[300];
        no_room(msg, sizeof msg, "the rest of this file", f->size - at, spare, held);
        f->state = UP_FAILED;
        str_copy(f->why, sizeof f->why, "not enough room on the drive (Hemma keeps 1 GB free)");
        b->failed++;
        lock_give(&g_lock);
        reply_err(conn, 507, true, msg);
        app_changed_where("/media");
        return;
    }
    Job *j = xcalloc(1, sizeof *j);
    j->b = b;
    j->i = i;
    j->at = at;
    j->len = f->size - at;
    join(j->full, sizeof j->full, b->root, f->rel);
    snprintf(j->part, sizeof j->part, "%s.part", j->full);
    f->state = UP_WRITING;
    f->saved = at;
    b->current = i;
    b->workers++;
    g_workers++;
    b->last = b->busy = now_sec();
    lock_give(&g_lock);

    Detached d;
    if (!r->streaming && r->body_len) buf_put(&j->body, r->body, r->body_len); /* small file: already here */
    conn_detach(conn, r, &d);
    j->s = d.fd;
    buf_put(&j->body, d.body.p, d.body.len);
    buf_free(&d.body);
    j->head = d.unsent;
    if (!thread_spawn(worker, j)) {
        lock_take(&g_lock);
        f->state = UP_QUEUED;
        b->workers--;
        g_workers--;
        lock_give(&g_lock);
        worker_reply(j->s, 503, "{\"error\":\"busy\",\"stop\":false}");
        sock_close(j->s);
        buf_free(&j->head);
        buf_free(&j->body);
        free(j);
    }
}

static void where(Ctx *c, Conn *conn, uint64_t id, int i) {
    lock_take(&g_lock);
    Batch *b = find(id);
    Buf out = {0};
    int status = 200;
    if (!b || b->person != c->me->id) {
        status = 404;
        buf_puts(&out, "{\"error\":\"That upload is over - start it again to continue\",\"stop\":true}");
    } else if (b->stopped) {
        status = 410;
        buf_puts(&out, "{\"error\":\"The upload was stopped\",\"stop\":true}");
    } else if (i < 0 || i >= b->nfiles || (b->files[i].state == UP_SKIPPED && !b->files[i].on_server)) {
        status = 404;
        buf_puts(&out, "{\"error\":\"That file isn't part of the upload\",\"stop\":true}");
    } else if (b->files[i].state == UP_FAILED) {
        status = 409;
        buf_puts(&out, "{\"error\":");
        json_str(&out, b->files[i].why);
        buf_puts(&out, ",\"stop\":true}");
    } else {
        const UpFile *f = &b->files[i];
        int64_t at = f->state == UP_DONE || f->on_server ? f->size
                     : f->state == UP_WRITING           ? f->saved
                                                        : resume_point(b, f);
        buf_printf(&out, "{\"at\":%lld%s}", (long long)at, f->state == UP_WRITING ? ",\"busy\":true" : "");
    }
    lock_give(&g_lock);
    reply(conn, status, &out);
    buf_free(&out);
}

/* The browser's side of a copy it makes (see the top): progress, the finished size, or "it
   couldn't" - then the original goes under its own name, unless it's on the list anyway. */
static void converted(Ctx *c, Conn *conn, HttpReq *r, uint64_t id, int i) {
    Fields q = {0};
    fields_parse(&q, r->query, strlen(r->query));
    long long pm = fields_int(&q, "pm", -1), size = fields_int(&q, "size", -1);
    bool original = str_eq(fields_str(&q, "original"), "1");
    char why[64], msg[300];
    str_copy(why, sizeof why, fields_str(&q, "why"));
    fields_free(&q);

    int64_t spare = INT64_MAX / 4, held = 0; /* for a copy or an original to come: asked before the lock */
    if (size >= 0 || original) {
        char root[300] = "";
        lock_take(&g_lock);
        const Batch *sb = find(id);
        if (sb) str_copy(root, sizeof root, sb->root);
        lock_give(&g_lock);
        if (sb) spare = spare_for(sb, root, 0, &held);
    }

    Buf out = {0};
    int status = 200;
    const char *err = NULL;
    lock_take(&g_lock);
    Batch *b = find(id);
    UpFile *f = b && i >= 0 && i < b->nfiles ? &b->files[i] : NULL;
    if (!b || b->person != c->me->id) err = "That upload is over - start it again to continue", status = 404;
    else if (b->stopped) err = "The upload was stopped", status = 410;
    else if (pm >= 0 && size < 0 && !original) { /* progress - also a late one, after the copy went: fine */
        if (f && f->conv == CV_WAIT && f->state == UP_QUEUED) f->pm = (int)CLAMP(pm, 0, 999);
        b->last = b->busy = now_sec();
        buf_puts(&out, "{\"ok\":true}");
    } else if (!f || !f->conv || f->state != UP_QUEUED) err = "That file isn't waiting for a copy", status = 404;
    else if (size >= 0) { /* the copy is ready: size it, then the browser sends it */
        char full[1024], part[1100];
        join(full, sizeof full, b->root, f->rel);
        snprintf(part, sizeof part, "%s.part", full);
        if (f->conv == CV_READY && size == f->size) { /* asked again (a lost answer): carry on */
            buf_printf(&out, "{\"at\":%lld}", (long long)resume_point(b, f));
        } else if (f->conv != CV_WAIT) {
            err = "That file is going as it is", status = 409;
        } else if (size <= 0 || size > (1LL << 40)) {
            err = "The browser's copy came out empty", status = 422;
        } else if (convert_give_way(full)) { /* the server's converter started on it meanwhile */
            leave_to_server(b, f, MADE_MEANWHILE);
            buf_puts(&out, "{\"skip\":true}");
        } else if (size > spare) {
            no_room(msg, sizeof msg, "the copy", size, spare, held);
            err = msg, status = 507;
        } else {
            b->total += size - copy_guess(f);
            f->size = size;
            f->saved = 0;
            f->conv = CV_READY;
            f->pm = 1000;
            file_remove(part); /* a copy from an earlier try: this one starts over */
            b->last = b->busy = now_sec();
            buf_puts(&out, "{\"at\":0}");
        }
    } else if (original && f->conv == CV_WAIT) { /* couldn't convert: the original goes as it is */
        char rel[420], full[1024];
        snprintf(rel, sizeof rel, "%.*s.%s", (int)(strlen(f->rel) - 8), f->rel, f->ext); /* - ".web.mp4" */
        join(full, sizeof full, b->root, rel);
        int64_t have = 0;
        bool dir = false, taken = f->extra || path_stat(full, &have, NULL, &dir);
        if (!taken && f->orig > spare) {
            no_room(msg, sizeof msg, "the original", f->orig, spare, held);
            err = msg, status = 507;
        } else if (taken) {
            b->total -= copy_guess(f);
            f->state = UP_SKIPPED;
            b->skipped++;
            snprintf(f->why, sizeof f->why, "%s", f->extra ? "couldn't be converted here - its original is on the list"
                                                 : dir || have != f->orig ? "couldn't be converted here, and a different file has its name"
                                                                          : "couldn't be converted here, and its original is already there");
            buf_puts(&out, "{\"skip\":true}");
        } else {
            b->total -= copy_guess(f);
            str_copy(f->rel, sizeof f->rel, rel);
            f->size = f->orig;
            f->conv = CV_ORIGINAL;
            f->saved = resume_point(b, f);
            b->total += f->size - f->saved;
            snprintf(f->why, sizeof f->why, "%s", why[0] ? why : "the converter couldn't read it");
            buf_printf(&out, "{\"at\":%lld}", (long long)f->saved);
        }
        if (!err) b->last = b->busy = now_sec();
    } else if (original && f->conv == CV_ORIGINAL) { /* asked again */
        buf_printf(&out, "{\"at\":%lld}", (long long)resume_point(b, f));
    } else {
        err = "pm, size or original", status = 400;
    }
    lock_give(&g_lock);
    if (!err) app_changed_where("/media"); /* progress bars, copies ready */
    if (err) reply_err(conn, status, true, err);
    else reply(conn, 200, &out);
    buf_free(&out);
}

bool upload_http(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_starts(r->path, "/media/upload/")) return false;
    const char *rest = r->path + 14;
    if (!is_admin(c)) {
        reply_err(conn, 403, true, "Only an admin can add videos");
        return true;
    }
    if (!http_same_origin(r)) {
        reply_err(conn, 403, true, "cross-site request refused");
        return true;
    }
    if (str_eq(rest, "plan")) {
        if (!str_eq(r->method, "POST")) reply_err(conn, 405, true, "POST only");
        else plan(c, conn, r);
        return true;
    }
    uint64_t id = 0;
    char hex[17];
    size_t hl = strcspn(rest, "/");
    str_ncopy(hex, sizeof hex, rest, hl);
    long long i = rest[hl] == '/' ? str_int_or(rest + hl + 1, -1) : -1;
    if (hl != 16 || !media_parse_id(hex, &id) || i < 0 || i >= 2 * MAX_FILES) {
        reply_err(conn, 404, true, "No such upload");
        return true;
    }
    if (str_eq(r->method, "PUT")) put(c, conn, r, id, (int)i);
    else if (str_eq(r->method, "GET")) where(c, conn, id, (int)i);
    else if (str_eq(r->method, "POST")) converted(c, conn, r, id, (int)i);
    else reply_err(conn, 405, true, "PUT, GET or POST");
    return true;
}

/* --------------------------------------------------------------- the page */

static Batch *batch_of(int person) { /* the latest of this person's video uploads (lock held) */
    Batch *best = NULL;
    for (int s = 0; s < MAX_BATCHES; s++)
        if (g_batch[s] && !g_batch[s]->art && g_batch[s]->person == person &&
            (!best || g_batch[s]->started > best->started))
            best = g_batch[s];
    return best;
}

void upload_art_status(Ctx *c, Buf *b, uint64_t id) {
    if (!c->me || !id) return;
    lock_take(&g_lock);
    const Batch *u = NULL;
    for (int s = 0; s < MAX_BATCHES; s++)
        if (g_batch[s] && g_batch[s]->art == id && g_batch[s]->person == c->me->id &&
            (!u || g_batch[s]->started > u->started))
            u = g_batch[s];
    const UpFile *f = u ? &u->files[0] : NULL;
    if (!f) {
    } else if (f->state == UP_FAILED) {
        H(b, "<p class=\"notice bad\" id=\"media-pic-st\">%s</p>", f->why);
    } else if (f->state == UP_DONE) {
        if (!u->ended || c->now - u->ended < 120)
            buf_puts(b, "<p class=\"notice\" id=\"media-pic-st\">Saved - the new picture shows everywhere in a moment.</p>");
    } else if (!u->stopped) {
        int pct = f->size ? (int)(f->saved * 100 / f->size) : 0;
        H(b, "<p class=\"notice\" id=\"media-pic-st\">Sending the picture… %d%%</p>", pct);
    }
    lock_give(&g_lock);
}

/* How far a batch is, 0..1000. With copies being made it counts work, not bytes: making a
   copy is most of it (weighed by its original's size), sending it the rest. */
static int batch_permille(const Batch *u) {
    double all = 0, done = 0;
    for (int i = 0; i < u->nfiles; i++) {
        const UpFile *f = &u->files[i];
        if (f->state == UP_SKIPPED) continue;
        bool copy = f->conv == CV_WAIT || f->conv == CV_READY;
        double w = (double)MAX(copy ? f->orig : f->size, 1), sent = f->size ? (double)f->saved / (double)f->size : 0;
        all += w;
        done += w * (f->state == UP_DONE || f->state == UP_FAILED ? 1
                     : f->conv == CV_WAIT                         ? 0.85 * f->pm / 1000.0
                     : f->conv == CV_READY                        ? 0.85 + 0.15 * sent
                                                                  : sent);
    }
    return all > 0 ? (int)(done * 1000 / all) : 1000;
}

static void render_batch(Ctx *c, Buf *b, const Batch *u) {
    char moved[24], total[24], speed[24];
    media_size_text(u->moved, moved, sizeof moved);
    media_size_text(u->total, total, sizeof total);
    int todo = u->nfiles - u->skipped, copies = 0, pending = 0, as_is = 0, heads = 0;
    for (int i = 0; i < u->nfiles; i++) {
        const UpFile *f = &u->files[i];
        copies += f->conv != CV_NONE;
        pending += f->conv == CV_WAIT && f->state == UP_QUEUED;
        as_is += f->conv == CV_ORIGINAL;
        heads += f->head && f->state != UP_SKIPPED;
    }
    bool open = batch_open(u);
    bool waiting = open && !u->workers && c->now - u->last > 20;
    H(b, "<section class=\"media-up\" id=\"media-up-%X\">", u->id);
    if (u->stopped) H(b, "<h3>Stopped · %d of %d files sent</h3>", u->done, todo);
    else if (!open) H(b, "<h3>Finished · %d file%s sent</h3>", u->done, u->done == 1 ? "" : "s");
    else if (waiting) H(b, "<h3>Waiting for the browser · %d of %d files sent</h3>", u->done, todo);
    else H(b, "<h3>Uploading · %d of %d files</h3>", u->done, todo);
    int per_mille = copies ? batch_permille(u) : u->total ? (int)(u->moved * 1000 / u->total) : 1000;
    H(b, "<progress class=\"media-up-bar\" max=\"1000\" value=\"%d\"></progress><p class=\"media-up-nums\">%s of %s%s",
      MIN(per_mille, 1000), moved, pending ? "about " : "", total);
    if (open && !waiting && u->rate > 1 && pending) {
        media_size_text((int64_t)u->rate, speed, sizeof speed);
        H(b, " · %s/s", speed);
    } else if (open && !waiting && u->rate > 1) {
        media_size_text((int64_t)u->rate, speed, sizeof speed);
        H(b, " · %s/s", speed);
        int64_t left = (int64_t)((double)(u->total - u->moved) / u->rate);
        if (left > 0) {
            buf_puts(b, " · about ");
            ui_duration(b, MAX(left, 60));
            buf_puts(b, " left");
        }
    } else if (!open && u->ended >= u->started && u->done) {
        int64_t took = MAX(u->ended - u->started, 1);
        if (took < 60) H(b, " · took %d s", (int)took);
        else {
            buf_puts(b, " · took ");
            ui_duration(b, took);
        }
    }
    buf_puts(b, "</p>");
    if (open && u->current >= 0) {
        const UpFile *f = &u->files[u->current];
        int pct = f->size ? (int)(f->saved * 100 / f->size) : 100;
        H(b, "<p class=\"muted media-up-now\">Now: <code class=\"media-code\">%s</code> · %d%%</p>", f->rel, pct);
    }
    if (open && copies) {
        H(b, "<p class=\"muted media-up-now\">Made on the sending device: %d of %d play-everywhere copies ready</p>",
          copies - pending, copies);
        int shown = 0;
        for (int i = 0; i < u->nfiles && shown < 12; i++) {
            const UpFile *f = &u->files[i];
            if (f->conv != CV_WAIT || f->state != UP_QUEUED || f->pm <= 0) continue;
            H(b, "%r<li class=\"row\"><span class=\"grow\"><code class=\"media-code\">%s</code></span>"
                 "<progress class=\"media-up-bar\" max=\"1000\" value=\"%d\"></progress></li>",
              shown++ ? "" : "<ul class=\"rows media-conv\">", tail2(f->rel), f->pm);
        }
        if (shown) buf_puts(b, "</ul>");
    }
    if (waiting)
        buf_puts(b, "<p class=\"muted media-small\">If the browser that started it was closed, choose the same folder "
                    "again below - what's already on the server is skipped, and half-sent files continue where "
                    "they stopped.</p>");
    if (u->failed || u->skipped || as_is || heads) {
        H(b, "<details class=\"media-up-list\"><summary>%d skipped", u->skipped);
        if (heads) H(b, ", %d play-all header%s", heads, heads == 1 ? "" : "s");
        if (as_is) H(b, ", %d sent unconverted", as_is);
        if (u->failed) H(b, ", %d failed", u->failed);
        buf_puts(b, "</summary><ul>");
        int shown = 0;
        for (int i = 0; i < u->nfiles && shown < 200; i++) {
            const UpFile *f = &u->files[i];
            if (f->state == UP_SKIPPED || f->state == UP_FAILED)
                H(b, "<li%r><code class=\"media-code\">%s</code> — %s</li>", f->state == UP_FAILED ? " class=\"bad\"" : "",
                  f->rel, f->why);
            else if (f->conv == CV_ORIGINAL)
                H(b, "<li><code class=\"media-code\">%s</code> — sent as it is: %s</li>", f->rel, f->why);
            else if (f->head) /* a long special can look like one: this is where you'd notice */
                H(b, "<li><code class=\"media-code\">%s</code> — only its first MB: it looks like a \"play all\" copy of the "
                     "episodes next to it (if it isn't, upload it on its own)</li>",
                  f->rel);
            else continue;
            shown++;
        }
        buf_puts(b, "</ul></details>");
    }
    if (open) {
        ui_form_confirm(b, "media.upload_stop", "inline", "Stop the upload? Everything already sent stays on the server.");
        H(b, "<input type=\"hidden\" name=\"batch\" value=\"%X\"><button class=\"btn\">Stop</button>", u->id);
        ui_form_end(b);
    }
    buf_puts(b, "</section>");
}

void upload_page(Ctx *c, Buf *b) {
    ctx_title(c, "Add videos");
    buf_puts(b, "<a class=\"media-back\" href=\"/media/settings\">");
    ui_icon(b, "left");
    buf_puts(b, "Library</a>");
    ui_head(b, "Add videos", "Send movies and series from this device to the library");
    ui_head_end(b);
    if (!is_admin(c)) {
        ui_empty(b, "🔒", "Admins only", "Ask an admin to add videos to the library.");
        return;
    }
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS);
    if (!nroots) {
        ui_empty(b, "📁", "Choose a library folder first",
                 "Uploads go into one of the library's folders. Add one in the library settings.");
        buf_puts(b, "<p class=\"center\"><a class=\"btn primary\" href=\"/media/settings\">Library settings</a></p>");
        return;
    }
    /* The form comes first and keeps its place: progress cards appear after it,
       so live updates never rebuild the form (and lose the chosen files). */
    const Asset *wasm = asset_named("/av.wasm"), *worker = asset_named("/conv.js");
    bool here = wasm && worker; /* the browser converter is built in (it needs zig to build) */
    bool busy_now = false; /* someone's upload is running: a new one needs "upload anyway" */
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++) busy_now |= running(g_batch[s]);
    lock_give(&g_lock);
    buf_puts(b, "<div class=\"grid2\">");
    ui_card(b, "Upload", NULL, NULL);
    buf_puts(b, "<form class=\"stack\" data-upload=\"/media/upload\" data-empty=\"Choose a folder or some files first\"");
    if (here) H(b, " data-convert=\"/static/conv.js?v=%s\" data-wasm=\"/static/av.wasm?v=%s\"", worker->etag, wasm->etag);
    buf_puts(b, "><label class=\"field\"><span>Save into</span><select name=\"root\">");
    for (int i = 0; i < nroots; i++) H(b, "<option value=\"%d\">%s</option>", i, roots[i]);
    buf_puts(b, "</select></label>"
                "<label class=\"field\"><span>Inside the folder (optional)</span><input name=\"into\" maxlength=\"120\" "
                "placeholder=\"e.g. Series\" spellcheck=\"false\"></label>"
                "<label class=\"field\"><span>A whole folder</span><input type=\"file\" name=\"folder\" "
                "webkitdirectory multiple></label>"
                "<label class=\"field\"><span>…or single files</span><input type=\"file\" name=\"files\" multiple "
                "accept=\"video/*,.mkv,.avi,.m2ts,.ts,.srt,.vtt,.jpg,.jpeg,.png,.webp\"></label>");
    if (here)
        buf_puts(b, "<label class=\"check\"><input type=\"checkbox\" name=\"conv\" value=\"1\" checked> Make videos play "
                    "everywhere on this device first (recommended)</label>");
    buf_puts(b, "<label class=\"check\"><input type=\"checkbox\" name=\"originals\" value=\"1\"> Also send the "
                "originals</label>"
                "<label class=\"check\"><input type=\"checkbox\" name=\"dupes\" value=\"1\"> Also send episodes and "
                "films the library already has under another name</label>"
                "<p class=\"muted media-small\">A folder keeps its layout, so <code class=\"media-code\">My Show/"
                "Season 1/…</code> arrives just like that. Files already on the server are skipped - and so are "
                "episodes and films the library already has under another name (the same show, season and episode; "
                "the same film, year and part), before anything is converted. If the connection drops it carries on "
                "where it stopped.</p>");
    if (here)
        buf_puts(b, "<p class=\"muted media-small\">Videos that not every phone, TV or browser can play (DVD rips, MKV "
                    "files, Dolby sound) are converted right here, on this device, and only the converted copy is sent: "
                    "about a third of the size, and it plays everywhere. That keeps this device busy while it runs - a "
                    "computer does it much faster than a phone. ");
    else
        buf_puts(b, "<p class=\"muted media-small\">This Hemma was built without the video converter, so videos are "
                    "sent as they are (convert them under Library settings afterwards). ");
    buf_puts(b, "A folder that already holds a converted <code class=\"media-code\">x.web.mp4</code> sends just that. A "
                "DVD's \"play all\" title (every episode again, in one file) stays behind: only its first MB goes, so "
                "the library can still tell episodes from bonus clips. Keep this tab open while it runs - you can use "
                "other pages meanwhile.</p>");
    /* always there, hidden while nothing runs: live updates then never rebuild the form (and lose the files) */
    H(b, "<label class=\"check\" id=\"media-up-force\"%r><input type=\"checkbox\" name=\"force\" value=\"1\"> Upload "
         "anyway, alongside the upload that's running (its files are left to it)</label>",
      busy_now ? "" : " hidden");
    buf_puts(b, "<div class=\"modal-f\"><button class=\"btn primary\">Upload</button></div></form>");
    ui_card_end(b);
    lock_take(&g_lock);
    const Batch *mine = batch_of(c->me->id);
    if (mine) {
        ui_card(b, "Your upload", NULL, NULL);
        render_batch(c, b, mine);
        ui_card_end(b);
    }
    for (int s = 0; s < MAX_BATCHES; s++) { /* other admins' uploads, so nobody is surprised */
        const Batch *o = g_batch[s];
        if (!o || o == mine || o->art || !batch_open(o)) continue;
        const Person *p = person_get(o->person);
        ui_card(b, p ? p->name : "Someone", NULL, NULL);
        render_batch(c, b, o);
        ui_card_end(b);
    }
    lock_give(&g_lock);
    buf_puts(b, "</div>");
}

void upload_summary(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    lock_take(&g_lock);
    int open = 0, files = 0, done = 0;
    for (int s = 0; s < MAX_BATCHES; s++) {
        const Batch *o = g_batch[s];
        if (!o || o->art || !batch_open(o)) continue;
        open++;
        files += o->nfiles - o->skipped;
        done += o->done;
    }
    lock_give(&g_lock);
    if (open) H(b, "<p class=\"media-small\">Uploading now: %d of %d files sent.</p>", done, files);
}

void upload_act_stop(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can do that");
        return;
    }
    /* the upload whose Stop was pressed (another admin's card has its own); without one, your own */
    uint64_t which = 0;
    bool one = media_parse_id(fields_str(f, "batch"), &which);
    int stopped = 0;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++) {
        Batch *o = g_batch[s];
        if (o && batch_open(o) && (one ? o->id == which : o->person == c->me->id)) {
            o->stopped = true;
            o->ended = now_sec();
            stopped++;
        }
    }
    lock_give(&g_lock);
    if (stopped) ctx_ok(c, "Upload stopped - what was sent stays");
    else ctx_info(c, "Nothing is uploading");
}

/* -------------------------------------------------------------- lifecycle */

void upload_start(void) {
    atomic_store(&g_halt, false);
    g_since = now_sec();
}

void upload_tick(int64_t now) {
    uint64_t sig = 0; /* (unsigned: it wraps, by design) */
    int saved = 0, pictures = 0;
    bool active = false;
    int finished_person = 0, finished_files = 0;
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++) {
        Batch *b = g_batch[s];
        if (!b) continue;
        if (b->workers) { /* speed: smoothed over ~3 s */
            active = true;
            if (b->rate_at && now - b->rate_at >= 900) {
                double inst = (double)(b->moved - b->rate_moved) * 1000.0 / (double)(now - b->rate_at);
                b->rate = b->rate > 0 ? b->rate * 0.6 + inst * 0.4 : inst;
            }
            if (!b->rate_at || now - b->rate_at >= 900) {
                b->rate_at = now;
                b->rate_moved = b->moved;
            }
        } else {
            b->rate_at = 0;
        }
        bool open = batch_open(b);
        if (!open && !b->ended && !b->workers) {
            b->ended = now_sec();
            if (!b->art) { /* a picture shows itself; no toast */
                finished_person = b->person;
                finished_files = b->done;
            }
        }
        int64_t idle = now_sec() - (b->ended ? b->ended : b->last);
        if (!b->workers && ((b->ended && idle > KEEP_DONE_S) || idle > FORGET_IDLE_S)) {
            batch_free(b);
            g_batch[s] = NULL;
            sig++;
            continue;
        }
        sig = sig * 31 + (uint64_t)b->moved + (uint64_t)b->done * 7 + (uint64_t)b->failed * 13 +
              (uint64_t)b->workers * 17 + b->stopped + (b->ended != 0) * 3u;
    }
    saved = g_files_saved;
    pictures = g_pictures_saved;
    lock_give(&g_lock);
    if (pictures > g_pictures_seen) media_scan_start(); /* a new picture: show it now, not in 20 s */
    g_pictures_seen = pictures;

    if (sig != g_shown) { /* progress moved: refresh the screens showing it */
        g_shown = sig;
        app_changed_where("/media");
    }
    if (active) app_wake_in(1000); /* keep the numbers moving even with no other reason to tick */
    if (finished_person)
        app_notify_person(finished_person, 0, "Upload finished: %d file%s added", finished_files,
                          finished_files == 1 ? "" : "s");
    /* new files: rescan once things settle (and at least every few minutes during a long upload) */
    if (saved > g_done_seen) {
        g_rescan_at = now + 20000;
        if (!g_rescan_first) g_rescan_first = now;
    }
    g_done_seen = saved;
    if (g_rescan_at && (now >= g_rescan_at || now - g_rescan_first > 180000) && !media_scanning()) {
        g_rescan_at = g_rescan_first = 0;
        media_scan_start();
    }
}

void upload_stop(void) {
    atomic_store(&g_halt, true);
    for (int waited = 0; waited < 3000; waited += 50) { /* let workers close their files */
        lock_take(&g_lock);
        int n = g_workers;
        lock_give(&g_lock);
        if (!n) break;
        plat_sleep_ms(50);
    }
    lock_take(&g_lock);
    for (int s = 0; s < MAX_BATCHES; s++)
        if (g_batch[s] && !g_batch[s]->workers) {
            batch_free(g_batch[s]);
            g_batch[s] = NULL;
        }
    g_files_saved = g_pictures_saved = 0;
    lock_give(&g_lock);
    g_rescan_at = g_rescan_first = 0;
    g_shown = 0;
    g_done_seen = g_pictures_seen = 0;
}
