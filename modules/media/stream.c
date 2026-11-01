/* modules/media/stream.c — HTTP endpoints for the media module:
 *
 *   /media/file/<id>      the video itself, with Range support (seeking); ?download=1 = as a download - and an
 *                         audiobook's file (1.6) the same way. An MP4 with 2+ sound tracks (2.2) is sent as an MP4
 *                         holding one of them (?a=<n>, else the file's default - mediainfo.h media_view), built
 *                         on the stream's thread and kept for the next ranges (MEDIA_VIEW_CACHE); a download is
 *                         the whole file, every sound in it
 *   /media/cover/<id>     the picture inside an audiobook's file (1.6)
 *   /media/zip/<series>   a season (?s=<season>) or the whole series as one .zip (download.h); an audiobook of
 *                         parts by its id the same way (1.6)
 *   /media/img/<id>       poster / folder art
 *   /media/sub/<id>.vtt   subtitles, SRT converted to WebVTT on the fly
 *   /media/subpic/<id>.json, /media/subpic/<id>/<n>.png   picture subtitles (.sup, .idx + .sub): the cues,
 *                         and one drawn (contract C5) - the player lays them over the video
 *
 * Files are streamed by worker threads: the request is parsed on the event
 * loop, then the socket is detached and handed to a thread that sends the
 * headers and the byte range. Sends are non-blocking with a stall timeout,
 * so a paused or vanished client never pins a thread for long; the browser
 * simply asks for the next range when it needs it.
 *
 * Downloads give way to people watching (download.h): while an ordinary video stream runs, each
 * download sends at most MEDIA_DOWNLOAD_SLOW bytes a second - looked at again every second. */
#define _FILE_OFFSET_BITS 64 /* 64-bit fseeko/off_t for > 4 GB files on 32-bit Pi and mingw */
#include "download.h"
#include "library.h"
#include "media.h"
#include "subtake.h"
#include "zip.h"
#include "../../core/plat.h"
#include "../av/subpic.h"

#include <stdatomic.h>

#define CHUNK (256 * 1024)
#define MAX_STREAMS 24
#define STALL_MS 45000
#define DOWNLOAD_STALL_MS (5 * 60 * 1000) /* a download paused in the browser: a zip can't resume, so wait longer */

static atomic_int g_active;
static atomic_bool g_stopping;
static atomic_int g_watching;  /* ordinary video streams: someone is watching */
static atomic_int g_downloads; /* downloads running (files and zips) */
static atomic_int g_zips;      /* zips among them */
static atomic_int g_fake_zips, g_fake_watching; /* tests */
static atomic_int g_pace_cap; /* tests: bytes a second, 0 = MEDIA_DOWNLOAD_SLOW (an int: a Pi 1 has no 64-bit atomics) */

static void views_drop(void);

int media_streams_active(void) { return atomic_load(&g_active); }
void media_stream_stop(void) {
    atomic_store(&g_stopping, true);
    media_subpic_drop();
    views_drop();
}
void media_stream_resume(void) {
    atomic_store(&g_stopping, false);
    media_subpic_drop();
    views_drop();
}
int media_downloads_active(void) { return atomic_load(&g_downloads); }
int media_zips_active(void) { return atomic_load(&g_zips); }
int media_watching_active(void) { return atomic_load(&g_watching) + atomic_load(&g_fake_watching); }
void media_zip_test_busy(int zips) { atomic_store(&g_fake_zips, zips); }
void media_download_test_pace(int64_t cap, int watching) {
    atomic_store(&g_pace_cap, (int)CLAMP(cap, 0, 1 << 30));
    atomic_store(&g_fake_watching, watching);
}

/* --------------------------------------------------------------- ranges */

MediaRange media_range(const char *header, int64_t size) {
    MediaRange r = {200, 0, size - 1, size};
    int64_t a = 0, z = 0;
    int k = http_range(header, size, &a, &z);
    if (k > 0) r = (MediaRange){206, a, z, z - a + 1};
    else if (k < 0) r = (MediaRange){416, 0, -1, 0};
    return r;
}

void media_range_head(Buf *out, const MediaRange *r, int64_t size, const char *ctype, const char *cache) {
    buf_printf(out, "HTTP/1.1 %d %s\r\n", r->status, http_status_text(r->status));
    buf_puts(out, "Server: hemma\r\nX-Content-Type-Options: nosniff\r\nAccept-Ranges: bytes\r\nConnection: close\r\n");
    if (r->status == 416) {
        buf_printf(out, "Content-Range: bytes */%lld\r\nContent-Length: 0\r\n", (long long)size);
    } else {
        buf_printf(out, "Content-Type: %s\r\nContent-Length: %lld\r\n", ctype, (long long)r->len);
        if (r->status == 206)
            buf_printf(out, "Content-Range: bytes %lld-%lld/%lld\r\n", (long long)r->from, (long long)r->to,
                       (long long)size);
    }
    if (cache) buf_printf(out, "Cache-Control: %s\r\n", cache);
    buf_puts(out, "\r\n");
}

/* ------------------------------------------------------- giving way */

typedef struct {
    bool on;               /* a download: gives way while someone watches */
    int64_t window, sent;  /* this second (now_ms) and the bytes sent in it */
} Pace;

/* bytes a second for each download now: 0 = no limit (nobody is watching) */
static int64_t pace_cap(void) {
    if (media_watching_active() <= 0) return 0;
    int c = atomic_load(&g_pace_cap);
    return c > 0 ? (int64_t)c : MEDIA_DOWNLOAD_SLOW;
}

/* How much of *n may go now: all of it while nobody watches, else what's left of this second's share -
   waiting for the next second when it's used up (whether anyone still watches is asked again then).
   false = Hemma is stopping. */
static bool pace(Pace *p, size_t *n) {
    if (!p || !p->on) return true;
    for (;;) {
        if (atomic_load(&g_stopping)) return false;
        int64_t cap = pace_cap(), now = now_ms();
        if (!cap) return true;
        if (now - p->window >= 1000 || now < p->window) p->window = now, p->sent = 0;
        if (p->sent < cap) {
            if ((int64_t)*n > cap - p->sent) *n = (size_t)(cap - p->sent);
            return true;
        }
        plat_sleep_ms((int)CLAMP(1000 - (now - p->window), 1, 100));
    }
}

/* -------------------------------------------------------------- sending */

static bool send_wait(sock_t s, const char *p, size_t n, int stall_ms) {
    int waited = 0;
    while (n) {
        if (atomic_load(&g_stopping)) return false;
        long k = sock_send(s, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            waited = 0;
            continue;
        }
        if (k < 0 && sock_again(sock_errno())) {
            pollfd_t pf = {.fd = s, .events = POLLOUT};
            int rc = plat_poll(&pf, 1, 1000);
            if (rc < 0 || (rc > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL)))) return false;
            if (rc == 0 && (waited += 1000) >= stall_ms) return false; /* client stopped reading */
            continue;
        }
        return false; /* connection reset: the browser moved on */
    }
    return true;
}

static bool send_all(sock_t s, const char *p, size_t n) { return send_wait(s, p, n, STALL_MS); }

/* send_wait, a download's share at a time */
static bool send_paced(sock_t s, const char *p, size_t n, int stall_ms, Pace *pc) {
    while (n) {
        size_t k = n;
        if (!pace(pc, &k) || !send_wait(s, p, k, stall_ms)) return false;
        if (pc) pc->sent += (int64_t)k;
        p += k;
        n -= k;
    }
    return true;
}

/* Read + send through one fixed buffer (Windows, or where sendfile can't). true = all of it went. */
static bool send_by_copy(sock_t s, FILE *f, int64_t from, int64_t left, int stall_ms, Pace *pc) {
    if (!file_seek(f, from)) return false;
    char *buf = xmalloc(CHUNK);
    while (left > 0) {
        size_t want = left < CHUNK ? (size_t)left : CHUNK;
        size_t got = fread(buf, 1, want, f);
        if (!got || !send_paced(s, buf, got, stall_ms, pc)) break;
        left -= (int64_t)got;
    }
    free(buf);
    return left <= 0;
}

/* Zero-copy on Linux (the Pi): the kernel moves pages from the disk cache
   straight to the socket - no copy through us, no buffer, almost no CPU. true = all of it went. */
static bool send_range(sock_t s, FILE *f, int64_t from, int64_t left, int stall_ms, Pace *pc) {
    int waited = 0;
    while (left > 0) {
        if (atomic_load(&g_stopping)) return false;
        size_t want = (size_t)(left < (1 << 30) ? left : (1 << 30));
        if (!pace(pc, &want)) return false;
        long k = file_sendfile(s, f, from, want);
        if (k == -2) return send_by_copy(s, f, from, left, stall_ms, pc); /* not available here */
        if (k > 0) {
            from += k;
            left -= k;
            if (pc) pc->sent += k;
            waited = 0;
            continue;
        }
        if (k < 0) return false; /* the browser moved on, or the file shrank */
        pollfd_t pf = {.fd = s, .events = POLLOUT};
        int rc = plat_poll(&pf, 1, 1000);
        if (rc < 0 || (rc > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL)))) return false;
        if (rc == 0 && (waited += 1000) >= stall_ms) return false; /* paused player / vanished client */
    }
    return true;
}

static void stream_run(sock_t s, const char *path, int64_t from, int64_t len, const char *head, size_t head_len,
                       int stall_ms, Pace *pc) {
    sock_nonblock(s, true);
    bool ok = send_all(s, head, head_len);
    FILE *f = ok && len > 0 ? file_open(path, "rb") : NULL;
    if (f) {
        send_range(s, f, from, len, stall_ms, pc);
        fclose(f);
    }
    sock_shutdown_write(s);
    sock_close(s);
}

void media_stream_run(sock_t s, const char *path, int64_t from, int64_t len, const char *head, size_t head_len) {
    stream_run(s, path, from, len, head, head_len, STALL_MS, NULL);
}

enum { JOB_OTHER, JOB_WATCH, JOB_DOWNLOAD };

typedef struct {
    sock_t s;
    char path[1024];
    int64_t from, len;
    Buf head;
    int kind; /* JOB_*: what it counts as */
} Job;

static void count_job(int kind, int d) {
    if (kind == JOB_WATCH) atomic_fetch_add(&g_watching, d);
    if (kind == JOB_DOWNLOAD) atomic_fetch_add(&g_downloads, d);
    atomic_fetch_add(&g_active, d);
}

static void stream_thread(void *arg) {
    Job *j = arg;
    Pace pc = {.on = j->kind == JOB_DOWNLOAD};
    stream_run(j->s, j->path, j->from, j->len, j->head.p, j->head.len, pc.on ? DOWNLOAD_STALL_MS : STALL_MS, &pc);
    count_job(j->kind, -1);
    buf_free_on_loop(&j->head); /* (the event loop's: base.h free_on_loop) */
    free_on_loop(j);
}

/* ------------------------------------------------- one sound of many (2.2) */

/* Views kept for the ranges that follow (a browser asks for many while it plays): by the file's id, size and mtime
   and the sound asked for. Built and freed on stream threads only - never on the event loop - each by whoever
   holds it last (users). A file that gets no view is remembered too (VIEW_BAD), so it's read and logged once. */
#define MEDIA_VIEW_CACHE 4
#define VIEW_BAD 16

typedef struct {
    uint64_t id;
    int64_t size, mtime;
    int sound;
    MediaView *v;
    int users;
    bool dead; /* out of the cache: freed by its last user */
    uint64_t used;
} ViewEnt;

static ViewEnt *g_views[MEDIA_VIEW_CACHE];
static struct {
    uint64_t id;
    int64_t size, mtime;
} g_view_bad[VIEW_BAD];
static int g_view_nbad;
static uint64_t g_view_seq;
static Lock g_viewl = LOCK_INIT;
static atomic_int g_view_builds;

int media_view_test_builds(void) { return atomic_load(&g_view_builds); } /* tests: views made since the start */

static void view_ent_free(ViewEnt *e) {
    if (!e) return;
    media_view_free(e->v);
    free(e);
}

static void views_drop(void) {
    lock_take(&g_viewl);
    ViewEnt *gone[MEDIA_VIEW_CACHE] = {0};
    for (int i = 0; i < MEDIA_VIEW_CACHE; i++) {
        ViewEnt *e = g_views[i];
        g_views[i] = NULL;
        if (e && e->users) e->dead = true;
        else gone[i] = e;
    }
    g_view_nbad = 0;
    atomic_store(&g_view_builds, 0);
    lock_give(&g_viewl);
    for (int i = 0; i < MEDIA_VIEW_CACHE; i++) view_ent_free(gone[i]);
}

typedef struct {
    sock_t s;
    char path[1024];
    uint64_t id;
    int64_t size, mtime;
    int sound;           /* asked for: 0, 1 ...; -1 = the file's default */
    char range[128];     /* the Range header ("" = none) */
    bool head_only;
    const char *ctype, *cache; /* (static strings) */
    Buf head;            /* answers to earlier pipelined requests (the event loop's), sent first */
} ViewJob;

static bool file_at(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

static ViewEnt *view_find(const ViewJob *j) { /* (g_viewl held) */
    for (int i = 0; i < MEDIA_VIEW_CACHE; i++) {
        ViewEnt *e = g_views[i];
        if (e && e->id == j->id && e->size == j->size && e->mtime == j->mtime && e->sound == j->sound) {
            e->users++;
            e->used = ++g_view_seq;
            return e;
        }
    }
    return NULL;
}

/* The view this job asks for: the cache's, else made now from f. NULL = none can be made (the whole file goes). */
static ViewEnt *view_get(const ViewJob *j, FILE *f) {
    lock_take(&g_viewl);
    ViewEnt *e = view_find(j);
    bool bad = false;
    for (int i = 0; i < g_view_nbad && !e && !bad; i++)
        bad = g_view_bad[i].id == j->id && g_view_bad[i].size == j->size && g_view_bad[i].mtime == j->mtime;
    lock_give(&g_viewl);
    if (e || bad) return e;

    char err[120] = "";
    MediaReader r = {f, j->size, file_at};
    MediaView *v = media_view_new(&r, j->sound, err, sizeof err);
    atomic_fetch_add(&g_view_builds, 1);
    ViewEnt *mine = v ? xcalloc(1, sizeof *mine) : NULL, *evict = NULL;
    lock_take(&g_viewl);
    if (!v) {
        if (g_view_nbad == VIEW_BAD) g_view_nbad = 0; /* (the oldest go: tried again, logged again) */
        g_view_bad[g_view_nbad].id = j->id;
        g_view_bad[g_view_nbad].size = j->size;
        g_view_bad[g_view_nbad].mtime = j->mtime;
        g_view_nbad++;
        lock_give(&g_viewl);
        log_warn("media: %s goes out with every sound track in it: no view of one (%s)", j->path, err);
        return NULL;
    }
    if ((e = view_find(j))) { /* another stream made it meanwhile */
        lock_give(&g_viewl);
        media_view_free(v);
        free(mine);
        return e;
    }
    *mine = (ViewEnt){j->id, j->size, j->mtime, j->sound, v, 1, false, ++g_view_seq};
    int slot = -1;
    for (int i = 0; i < MEDIA_VIEW_CACHE && slot < 0; i++)
        if (!g_views[i]) slot = i;
    bool full = slot < 0;
    for (int i = 0; full && i < MEDIA_VIEW_CACHE; i++) /* else the one used longest ago that nobody holds */
        if (!g_views[i]->users && (slot < 0 || g_views[i]->used < g_views[slot]->used)) slot = i;
    if (slot < 0) mine->dead = true; /* every one in use: this one isn't kept */
    else evict = g_views[slot], g_views[slot] = mine;
    lock_give(&g_viewl);
    view_ent_free(evict);
    return mine;
}

static void view_put(ViewEnt *e) {
    if (!e) return;
    lock_take(&g_viewl);
    bool last = --e->users == 0 && e->dead;
    lock_give(&g_viewl);
    if (last) view_ent_free(e);
}

/* A view's bytes [from, from + left): its header from memory, the rest straight from the file */
static bool send_view(sock_t s, FILE *f, const MediaView *v, int64_t from, int64_t left) {
    MediaViewPart pt;
    while (left > 0 && media_view_part(v, from, &pt)) {
        int64_t k = pt.len < left ? pt.len : left;
        if (pt.mem ? !send_all(s, (const char *)pt.mem, (size_t)k) : !send_range(s, f, pt.file_off, k, STALL_MS, NULL))
            return false;
        from += k;
        left -= k;
    }
    return left <= 0;
}

static void view_thread(void *arg) {
    ViewJob *j = arg;
    FILE *f = file_open(j->path, "rb");
    ViewEnt *e = f ? view_get(j, f) : NULL;
    int64_t total = e ? media_view_size(e->v) : j->size;
    MediaRange rg = media_range(j->range[0] ? j->range : NULL, total);
    Buf head = {0};
    media_range_head(&head, &rg, total, j->ctype, j->cache);
    sock_nonblock(j->s, true);
    bool ok = send_all(j->s, j->head.p, j->head.len) && send_all(j->s, head.p, head.len);
    if (ok && f && !j->head_only && rg.status != 416 && rg.len > 0) {
        if (e) send_view(j->s, f, e->v, rg.from, rg.len);
        else send_range(j->s, f, rg.from, rg.len, STALL_MS, NULL);
    }
    view_put(e);
    if (f) fclose(f);
    buf_free(&head);
    sock_shutdown_write(j->s);
    sock_close(j->s);
    count_job(JOB_WATCH, -1);
    buf_free_on_loop(&j->head); /* (the event loop's: base.h free_on_loop) */
    free_on_loop(j);
}

/* An MP4 with several sounds: everything - even the head of a HEAD request, whose length is the view's - is
   worked out on a thread (the view may need its moov read: megabytes, from a USB disk). */
static bool serve_view(Conn *conn, HttpReq *r, const char *path, uint64_t id, int64_t size, int64_t mtime, int sound,
                       const char *ctype, const char *cache) {
    if (atomic_load(&g_active) >= MAX_STREAMS) {
        conn_reply(conn, 503, "text/plain; charset=utf-8", "busy, try again\n", 16, "Retry-After: 2\r\n");
        return true;
    }
    ViewJob *j = xcalloc(1, sizeof *j);
    str_copy(j->path, sizeof j->path, path);
    j->id = id;
    j->size = size;
    j->mtime = mtime;
    j->sound = sound;
    const char *rh = http_header(r, "Range");
    if (rh && strlen(rh) < sizeof j->range) str_copy(j->range, sizeof j->range, rh);
    j->head_only = str_eq(r->method, "HEAD");
    j->ctype = ctype;
    j->cache = cache;
    count_job(JOB_WATCH, +1);
    Detached d;
    conn_detach(conn, r, &d);
    j->s = d.fd;
    j->head = d.unsent;
    buf_free(&d.body);
    if (!thread_spawn(view_thread, j)) {
        count_job(JOB_WATCH, -1);
        sock_close(j->s);
        buf_free(&j->head);
        free(j);
    }
    return true;
}

/* ?a=<n>: the sound asked for (0 .. MEDIA_MAX_SOUNDS * 8); anything else -1 = the file's default */
static int sound_asked(const char *a) {
    if (!a || !*a || strlen(a) > 2) return -1;
    int n = 0;
    for (const char *p = a; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        n = n * 10 + (*p - '0');
    }
    return n;
}

/* ------------------------------------------------------------ endpoints */

static void plain(Conn *conn, int status, const char *msg) {
    conn_reply(conn, status, "text/plain; charset=utf-8", msg, strlen(msg), NULL);
}

/* a sentence for people (in the request's language), on its own line */
static void say(Conn *conn, int status, const char *text, const char *extra) {
    Buf b = {0};
    buf_printf(&b, "%s\n", text);
    conn_reply(conn, status, "text/plain; charset=utf-8", b.p, b.len, extra);
    buf_free(&b);
}

/* The bytes [base, base + size) of path as one answer - a whole file, or the picture inside an audiobook (1.6) -
   Range and HEAD as for any file, sent by a worker thread. kind: JOB_*; dl: the Content-Disposition line of a
   download (NULL = an ordinary request). */
static bool serve_bytes(Conn *conn, HttpReq *r, const char *path, int64_t base, int64_t size, const char *ctype,
                        const char *cache, const char *dl, int kind);

static bool get_only(Conn *conn, HttpReq *r) {
    if (str_eq(r->method, "GET") || str_eq(r->method, "HEAD")) return true;
    conn_reply(conn, 405, "text/plain; charset=utf-8", "GET only\n", 9, "Allow: GET, HEAD\r\n");
    return false;
}

/* dl: the Content-Disposition line of a download (NULL = an ordinary request); sound: ?a= (sound_asked) */
static bool serve_file(Conn *conn, HttpReq *r, const char *idhex, char want, const char *dl, int sound) {
    if (!get_only(conn, r)) return true;
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(idhex, &id) ? media_file(id) : NULL;
    bool exact = want == 'X'; /* (2.6: ?src=1 - the file itself) */
    if (exact) want = MF_VIDEO;
    else if (f && want == MF_VIDEO) f = media_playable(f); /* its play-everywhere copy, when converted */
    if (f && want == MF_VIDEO && f->type == MF_AUDIO) want = MF_AUDIO; /* (an audiobook's part: the same address) */
    char path[1024];
    int64_t size = 0, mtime = 0;
    bool dir = false;
    if (!f || f->type != want) {
        plain(conn, 404, "not in the library\n");
        return true;
    }
    if (!media_full_path(f, path, sizeof path) || !path_stat(path, &size, &mtime, &dir) || dir) {
        plain(conn, 404, "the file has gone - rescan the library?\n");
        return true;
    }
    const char *ctype = want == MF_IMAGE                                     ? http_mime(f->rel)
                        : want == MF_AUDIO && str_eq(f->probe.container, "mp3") ? "audio/mpeg"
                        : want == MF_AUDIO                                   ? "audio/mp4"
                        : f->probe.container[0]                              ? media_mime(f->probe.container)
                                                                             : http_mime(f->rel);
    /* a picture URL carries its version (?v=): a new picture is a new URL, so it can stay cached */
    const char *cache = want != MF_IMAGE                ? "no-cache"
                        : strstr(r->query, "v=") ? "private, max-age=31536000, immutable"
                                                  : "private, max-age=86400";
    /* several sounds in an MP4 (2.2): one of them, unless it's a download (which takes them all) - or ?src=1: a device
       converting it reads the file itself, every sound, at the size its plan says */
    if (want == MF_VIDEO && !dl && !exact && f->probe.nsounds >= 2 &&
        (str_eq(f->probe.container, "mp4") || str_eq(f->probe.container, "mov")))
        return serve_view(conn, r, path, f->id, size, mtime, sound, ctype, cache);
    return serve_bytes(conn, r, path, 0, size, ctype, cache, dl,
                       dl ? JOB_DOWNLOAD : want == MF_VIDEO ? JOB_WATCH : JOB_OTHER);
}

/* /media/cover/<file id>: the picture inside an audiobook's file (1.6) - its place found by the scan, so nothing
   is read but the picture's own bytes */
static bool serve_cover(Conn *conn, HttpReq *r, const char *idhex) {
    if (!get_only(conn, r)) return true;
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(idhex, &id) ? media_file(id) : NULL;
    const MediaTags *t = f && f->type == MF_AUDIO ? media_tags(f->id) : NULL;
    char path[1024];
    int64_t size = 0;
    if (!t || t->cover_len <= 0 || !media_full_path(f, path, sizeof path) || !path_stat(path, &size, NULL, NULL) ||
        size != f->size || t->cover_off < 0 || t->cover_off + t->cover_len > size) {
        plain(conn, 404, "no picture\n");
        return true;
    }
    return serve_bytes(conn, r, path, t->cover_off, t->cover_len, t->cover_type,
                       strstr(r->query, "v=") ? "private, max-age=31536000, immutable" : "private, max-age=86400", NULL,
                       JOB_OTHER);
}

static bool serve_bytes(Conn *conn, HttpReq *r, const char *path, int64_t base, int64_t size, const char *ctype,
                        const char *cache, const char *dl, int kind) {
    MediaRange rg = media_range(http_header(r, "Range"), size);
    Buf head = {0};
    media_range_head(&head, &rg, size, ctype, cache);
    if (dl && *dl) { /* (before the blank line that ends the head) */
        head.len -= 2;
        buf_puts(&head, dl);
        buf_puts(&head, "\r\n");
    }
    if (str_eq(r->method, "HEAD") || rg.status == 416 || rg.len <= 0) {
        conn->keep_alive = false;
        conn_write(conn, head.p, head.len);
        conn_close(conn);
        buf_free(&head);
        return true;
    }
    if (atomic_load(&g_active) >= MAX_STREAMS) {
        buf_free(&head);
        conn_reply(conn, 503, "text/plain; charset=utf-8", "busy, try again\n", 16, "Retry-After: 2\r\n");
        return true;
    }
    Job *j = xcalloc(1, sizeof *j);
    str_copy(j->path, sizeof j->path, path);
    j->from = base + rg.from;
    j->len = rg.len;
    j->kind = kind;
    count_job(j->kind, +1);
    Detached d;
    conn_detach(conn, r, &d);
    j->s = d.fd;
    j->head = d.unsent; /* rare: answers to earlier pipelined requests go out first */
    buf_put(&j->head, head.p, head.len);
    buf_free(&head);
    buf_free(&d.body);
    if (!thread_spawn(stream_thread, j)) {
        count_job(j->kind, -1);
        sock_close(j->s);
        buf_free(&j->head);
        free(j);
    }
    return true;
}

/* ------------------------------------------------------------------ zips */

typedef struct {
    char *path, *name;
} ZipPart;

typedef struct {
    sock_t s;
    Buf head;        /* the connection's unsent bytes (pipelined answers), then this answer's head */
    Buf disposition; /* its Content-Disposition line */
    bool head_only;  /* HEAD: the head, no archive */
    ZipPart *parts;
    int n;
    Pace pace;
    FILE *f; /* the part being read */
    int open;
} ZipJob;

static size_t zip_read(void *ctx, int i, void *buf, size_t cap) {
    ZipJob *j = ctx;
    if (j->open != i) {
        if (j->f) fclose(j->f);
        j->f = i >= 0 && i < j->n ? file_open(j->parts[i].path, "rb") : NULL;
        j->open = i;
    }
    return j->f ? fread(buf, 1, cap, j->f) : 0;
}

static bool zip_send(void *ctx, const void *p, size_t n) {
    ZipJob *j = ctx;
    return send_paced(j->s, p, n, DOWNLOAD_STALL_MS, &j->pace);
}

static void zip_thread(void *arg) {
    ZipJob *j = arg;
    /* the files as they are now: the size the archive says must be the one sent (a file that has gone
       since the scan is left out) */
    ZipEntry *e = xcalloc((size_t)(j->n ? j->n : 1), sizeof *e);
    int n = 0;
    for (int i = 0; i < j->n; i++) {
        int64_t size = 0, mtime = 0;
        bool dir = false;
        if (!path_stat(j->parts[i].path, &size, &mtime, &dir) || dir || size < 0) {
            log_warn("media: %s is left out of a download: it has gone", j->parts[i].path);
            free_on_loop(j->parts[i].path); /* (the event loop's: base.h free_on_loop) */
            free_on_loop(j->parts[i].name);
            continue;
        }
        j->parts[n] = j->parts[i];
        DateTime t = dt_local(mtime);
        e[n] = (ZipEntry){.name = j->parts[n].name, .size = (uint64_t)size};
        zip_dos_time(t.year, t.mon, t.day, t.hour, t.min, t.sec, &e[n].dos_time, &e[n].dos_date);
        n++;
    }
    j->n = n;
    Buf head = {0}; /* (this thread's own: growing the event loop's j->head would free its old bytes here) */
    buf_put(&head, j->head.p, j->head.len);
    buf_printf(&head,
               "HTTP/1.1 200 OK\r\nServer: hemma\r\nX-Content-Type-Options: nosniff\r\nContent-Type: application/zip\r\n"
               "Content-Length: %llu\r\nAccept-Ranges: none\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
               (unsigned long long)zip_size(e, n), j->disposition.p ? j->disposition.p : "");
    sock_nonblock(j->s, true);
    if (send_wait(j->s, head.p, head.len, DOWNLOAD_STALL_MS) && !j->head_only) {
        char *buf = xmalloc(CHUNK);
        ZipIo io = {zip_read, zip_send, j};
        j->open = -1;
        if (!zip_write(e, n, &io, buf, CHUNK)) log_info("media: a download stopped before its end");
        free(buf);
    }
    if (j->f) fclose(j->f);
    sock_shutdown_write(j->s);
    sock_close(j->s);
    for (int i = 0; i < j->n; i++) free_on_loop(j->parts[i].path), free_on_loop(j->parts[i].name);
    free_on_loop(j->parts); /* (the job and all it came with are the event loop's: base.h free_on_loop) */
    free(e);
    buf_free(&head);
    buf_free_on_loop(&j->head);
    buf_free_on_loop(&j->disposition);
    free_on_loop(j);
    atomic_fetch_sub(&g_zips, 1);
    count_job(JOB_DOWNLOAD, -1);
}

static bool serve_zip(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD")) {
        conn_reply(conn, 405, "text/plain; charset=utf-8", "GET only\n", 9, "Allow: GET, HEAD\r\n");
        return true;
    }
    if (!media_grown_up(c)) {
        say(conn, 403, T("Only grown-ups can download videos."), NULL);
        return true;
    }
    uint64_t id = 0;
    const MediaSeries *s = media_parse_id(r->path + 11, &id) ? media_series(id) : NULL;
    const MediaBook *bk = s ? NULL : media_book(id); /* (1.6: an audiobook in parts - "<Title>/<each part>") */
    if (bk && !media_kids_book_ok(c, bk)) bk = NULL;
    if (!s && !bk) {
        say(conn, 404, T("This series isn't in the library"), NULL);
        return true;
    }
    const char *sq = ctx_q(c, "s");
    long long season = -1;
    int n = 0;
    MediaZipItem *items = NULL;
    char zname[300], folder[200];
    if (bk) {
        media_safe_name(bk->title, folder, sizeof folder);
        snprintf(zname, sizeof zname, "%s.zip", folder);
        items = xcalloc((size_t)bk->count, sizeof *items);
        for (; n < bk->count; n++) {
            const MediaFile *f = media_book_part(bk, n);
            const char *base = strrchr(f->rel, '/');
            items[n].file = f->id;
            items[n].size = f->size;
            snprintf(items[n].name, sizeof items[n].name, "%s/%s", folder, base ? base + 1 : f->rel);
        }
    } else if (!*sq || (str_int(sq, &season) && season >= 0 && season < 10000)) {
        items = media_zip_items(s, *sq ? (int)season : -1, &n);
        media_zip_name(s, *sq ? (int)season : -1, zname, sizeof zname);
    }
    if (!n) {
        free(items);
        say(conn, 404, T("That season isn't in the library"), NULL);
        return true;
    }
    if (atomic_load(&g_zips) + atomic_load(&g_fake_zips) >= MEDIA_MAX_ZIPS) {
        free(items);
        say(conn, 503, T("Two seasons are being downloaded already - try again when one of them is done."),
            "Retry-After: 60\r\n");
        return true;
    }
    ZipJob *j = xcalloc(1, sizeof *j);
    j->parts = xcalloc((size_t)n, sizeof *j->parts);
    for (int i = 0; i < n; i++) {
        char path[1024];
        const MediaFile *f = media_file(items[i].file);
        if (!f || !media_full_path(f, path, sizeof path)) continue;
        j->parts[j->n].path = xstrdup(path);
        j->parts[j->n].name = xstrdup(items[i].name);
        j->n++;
    }
    free(items);
    media_content_disposition(&j->disposition, zname);
    j->head_only = str_eq(r->method, "HEAD");
    j->pace.on = true;
    if (!j->head_only) log_info("media: %s downloads %s (%d files)", c->me->username, zname, j->n);
    atomic_fetch_add(&g_zips, 1);
    count_job(JOB_DOWNLOAD, +1);
    Detached d;
    conn_detach(conn, r, &d);
    j->s = d.fd;
    j->head = d.unsent;
    buf_free(&d.body);
    if (!thread_spawn(zip_thread, j)) {
        sock_close(j->s);
        for (int i = 0; i < j->n; i++) free(j->parts[i].path), free(j->parts[i].name);
        free(j->parts);
        buf_free(&j->head);
        buf_free(&j->disposition);
        free(j);
        atomic_fetch_sub(&g_zips, 1);
        count_job(JOB_DOWNLOAD, -1);
    }
    return true;
}

bool media_sub_vtt(const MediaFile *sub, Buf *out) {
    char path[1024];
    Buf raw = {0};
    bool ok = false;
    if (sub && sub->type == MF_SUB && media_full_path(sub, path, sizeof path) && file_read_all(path, &raw, 8u << 20)) {
        if (str_iends(sub->rel, ".vtt")) {
            buf_put(out, raw.p, raw.len);
            ok = raw.len >= 6;
        } else {
            ok = srt_to_vtt(raw.p ? raw.p : "", raw.len, out);
        }
    }
    buf_free(&raw);
    return ok;
}

static bool serve_sub(Conn *conn, const char *idhex) {
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(idhex, &id) ? media_file(id) : NULL;
    Buf vtt = {0};
    if (media_sub_vtt(f, &vtt))
        conn_reply(conn, 200, "text/vtt; charset=utf-8", vtt.p, vtt.len, "Cache-Control: no-cache\r\n");
    else plain(conn, 404, "no such subtitle\n");
    buf_free(&vtt);
    return true;
}

/* ------------------------------------------------- picture subtitles (C5) */

/* Blu-ray PGS (.sup) and DVD VobSub (.idx + its .sub) can't be a <track>: the player draws them as
   pictures. GET /media/subpic/<id>.json gives the cues ({"w","h"} = the frame they're placed on, then
   [start_ms, end_ms, x, y, w, h] each), GET /media/subpic/<id>/<n>.png cue n drawn. Opening a file indexes
   it - a film's .sup is ~30 MB of segment headers on a USB disk - so the last few opened stay (by id, size
   and mtime: a file made again is opened again); each answer runs on a thread of its own. The files are
   open only while they're read: nothing is held between requests (Windows can't delete an open file). */
#define PIC_JOBS 4             /* answered at once (each may draw a picture up to 4096 x 4096: 16 MB) */
#define PIC_IDX_MAX (4u << 20) /* the most an .idx may be (a film's is ~100 KB) */

typedef struct {
    FILE *f; /* the pictures' file, while it's read */
} PicIo;

typedef struct {
    uint64_t id;
    int64_t size, mtime; /* the .sup / .idx as it was opened */
    char data[1024];     /* the file the pictures are in: the .sup, or the .idx's .sub */
    SubPic *sp;
    PicIo io;            /* the reader's ctx (subpic keeps a copy of the reader) */
    int users;           /* threads using it (g_picl) */
    uint64_t used;       /* when last asked for: g_pic_seq then (a clock's milliseconds tie on Linux) */
    bool dead;           /* out of the cache while still in use: its last user frees it */
} PicEnt;

static PicEnt *g_pics[MEDIA_SUBPIC_CACHE];
static Lock g_picl = LOCK_INIT;   /* the cache */
static Lock g_picuse = LOCK_INIT; /* every subpic_* call on a cached one (a SubPic is one thread's at a time) */
static atomic_int g_pic_jobs, g_pic_opens;
static uint64_t g_pic_seq; /* (g_picl) */

int media_subpic_test_opens(void) { return atomic_load(&g_pic_opens); }

static bool pic_read(void *ctx, int64_t off, void *dst, size_t n) {
    PicIo *io = ctx;
    return io->f && file_pread(io->f, off, dst, n);
}

static void pic_free(PicEnt *e) {
    if (!e) return;
    subpic_close(e->sp);
    free(e);
}

void media_subpic_drop(void) {
    lock_take(&g_picl);
    for (int i = 0; i < MEDIA_SUBPIC_CACHE; i++) {
        PicEnt *e = g_pics[i];
        g_pics[i] = NULL;
        if (e && e->users) e->dead = true;
        else pic_free(e);
    }
    atomic_store(&g_pic_opens, 0);
    lock_give(&g_picl);
}

typedef struct {
    sock_t s;
    Buf head;            /* the connection's unsent bytes (pipelined answers), then this answer */
    uint64_t id;
    int64_t size, mtime; /* of the .sup / .idx, now */
    char path[1024];     /* the .sup / .idx */
    char data[1024];     /* where its pictures are: the .sup itself, or the .idx's .sub */
    bool vobsub, head_only;
    int n; /* the cue to draw; -1 = the cue list */
} PicJob;

static PicEnt *pic_open(const PicJob *j) {
    PicEnt *e = xcalloc(1, sizeof *e);
    e->id = j->id;
    e->size = j->size;
    e->mtime = j->mtime;
    str_copy(e->data, sizeof e->data, j->data);
    int64_t dsize = 0;
    Buf idx = {0};
    char err[160] = "";
    if (!path_stat(e->data, &dsize, NULL, NULL) || (j->vobsub && !file_read_all(j->path, &idx, PIC_IDX_MAX)) ||
        !(e->io.f = file_open(e->data, "rb"))) {
        buf_free(&idx);
        free(e);
        return NULL;
    }
    MkvReader rd = {&e->io, dsize, pic_read};
    e->sp = j->vobsub ? subpic_open_vobsub(idx.p ? idx.p : "", idx.len, &rd, err, sizeof err)
                      : subpic_open_sup(&rd, err, sizeof err);
    fclose(e->io.f);
    e->io.f = NULL;
    buf_free(&idx);
    if (!e->sp) {
        log_info("media: picture subtitles %s: %s", j->path, err);
        free(e);
        return NULL;
    }
    atomic_fetch_add(&g_pic_opens, 1);
    return e;
}

static PicEnt *pic_cached(const PicJob *j) { /* g_picl held */
    for (int i = 0; i < MEDIA_SUBPIC_CACHE; i++) {
        PicEnt *e = g_pics[i];
        if (e && e->id == j->id && e->size == j->size && e->mtime == j->mtime) return e;
    }
    return NULL;
}

/* The opened file, from the cache or opened now (and kept: the least lately used goes) - NULL when it
   can't be read. pic_put() it when done. */
static PicEnt *pic_get(const PicJob *j) {
    lock_take(&g_picl);
    PicEnt *e = pic_cached(j);
    if (e) e->users++, e->used = ++g_pic_seq;
    lock_give(&g_picl);
    if (e) return e;
    PicEnt *made = pic_open(j), *out = NULL; /* (outside the lock: it reads the whole file's headers) */
    if (!made) return NULL;
    lock_take(&g_picl);
    e = pic_cached(j); /* (opened by another thread meanwhile: that one) */
    if (!e) {
        int slot = -1;
        for (int i = 0; i < MEDIA_SUBPIC_CACHE && slot < 0; i++)
            if (!g_pics[i]) slot = i;
        if (slot < 0) { /* full: the least lately used goes - one not in use before one that is */
            slot = 0;
            for (int i = 1; i < MEDIA_SUBPIC_CACHE; i++) {
                const PicEnt *a = g_pics[i], *b = g_pics[slot];
                if (!a->users != !b->users ? !a->users : a->used < b->used) slot = i;
            }
        }
        out = g_pics[slot];
        if (out && out->users) out->dead = true, out = NULL; /* (its last user frees it) */
        g_pics[slot] = e = made;
        made = NULL;
    }
    e->users++;
    e->used = ++g_pic_seq;
    lock_give(&g_picl);
    pic_free(made);
    pic_free(out);
    return e;
}

static void pic_put(PicEnt *e) {
    lock_take(&g_picl);
    bool last = --e->users == 0 && e->dead;
    lock_give(&g_picl);
    if (last) pic_free(e);
}

static void pic_answer(PicJob *j, int status, const char *ctype, const void *body, size_t n) {
    Buf out = {0}; /* (this thread's own: growing the event loop's j->head would free its old bytes here) */
    buf_put(&out, j->head.p, j->head.len);
    buf_printf(&out,
               "HTTP/1.1 %d %s\r\nServer: hemma\r\nX-Content-Type-Options: nosniff\r\nContent-Type: %s\r\n"
               "Content-Length: %lld\r\nCache-Control: %s\r\nConnection: close\r\n\r\n",
               status, http_status_text(status), ctype, (long long)n, status == 200 ? "private, max-age=86400" : "no-cache");
    if (!j->head_only && n) buf_put(&out, body, n);
    sock_nonblock(j->s, true);
    send_all(j->s, out.p, out.len);
    buf_free(&out);
    sock_shutdown_write(j->s);
    sock_close(j->s);
}

/* Cue n's picture as a PNG into out (g_picuse held); false when there's no such cue or it can't be drawn */
static bool pic_png(PicEnt *e, int n, Buf *out) {
    const SubPicCue *c = subpic_cue(e->sp, n);
    if (!c || c->w < 1 || c->h < 1 || c->w > SUBPIC_MAX_SIDE || c->h > SUBPIC_MAX_SIDE) return false;
    int w = c->w, h = c->h;
    size_t npx = (size_t)w * (size_t)h, len = 0;
    uint8_t *px = malloc(npx), *png = NULL;
    uint32_t pal[256];
    int ncolors = 0;
    e->io.f = px ? file_open(e->data, "rb") : NULL;
    if (e->io.f && subpic_render(e->sp, n, px, npx, pal, &ncolors)) png = png_indexed(px, w, h, pal, ncolors, &len);
    if (e->io.f) fclose(e->io.f);
    e->io.f = NULL;
    free(px);
    if (png) buf_put(out, png, len);
    free(png);
    return png != NULL;
}

static void pic_thread(void *arg) {
    PicJob *j = arg;
    PicEnt *e = pic_get(j);
    Buf body = {0};
    bool ok = false;
    if (e) {
        lock_take(&g_picuse);
        if (j->n < 0) {
            int fw = 0, fh = 0;
            subpic_frame(e->sp, &fw, &fh);
            buf_printf(&body, "{\"w\":%d,\"h\":%d,\"cues\":[", fw, fh);
            for (int i = 0; i < subpic_count(e->sp); i++) {
                const SubPicCue *c = subpic_cue(e->sp, i);
                buf_printf(&body, "%s[%lld,%lld,%d,%d,%d,%d]", i ? "," : "", (long long)c->start_ms, (long long)c->end_ms,
                           c->x, c->y, c->w, c->h);
            }
            buf_puts(&body, "]}");
            ok = true;
        } else {
            ok = pic_png(e, j->n, &body);
        }
        lock_give(&g_picuse);
        pic_put(e);
    }
    if (ok) pic_answer(j, 200, j->n < 0 ? "application/json" : "image/png", body.p, body.len);
    else {
        const char *why = !e ? "these subtitles can't be read\n" : "no such picture\n";
        pic_answer(j, 404, "text/plain; charset=utf-8", why, strlen(why));
    }
    buf_free(&body);
    buf_free_on_loop(&j->head); /* (the event loop's: base.h free_on_loop) */
    free_on_loop(j);
    atomic_fetch_sub(&g_pic_jobs, 1);
    count_job(JOB_OTHER, -1);
}

/* "<16 hex>.json" -> -1, "<16 hex>/<n>.png" -> n; -2 = neither */
static int subpic_which(const char *p) {
    if (strlen(p) < 16) return -2;
    p += 16;
    if (str_eq(p, ".json")) return -1;
    if (*p++ != '/' || *p < '0' || *p > '9') return -2;
    long n = 0;
    for (; *p >= '0' && *p <= '9'; p++)
        if ((n = n * 10 + (*p - '0')) >= SUBPIC_MAX_CUES) return -2;
    return str_eq(p, ".png") ? (int)n : -2;
}

static bool serve_subpic(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD")) {
        conn_reply(conn, 405, "text/plain; charset=utf-8", "GET only\n", 9, "Allow: GET, HEAD\r\n");
        return true;
    }
    const char *p = r->path + 14; /* after "/media/subpic/" */
    uint64_t id = 0;
    int n = subpic_which(p);
    const MediaFile *f = n > -2 && media_parse_id(p, &id) ? media_file(id) : NULL;
    bool sup = f && str_iends(f->rel, ".sup"), idx = f && str_iends(f->rel, ".idx");
    if (!f || f->type != MF_SUB || (!sup && !idx) || !media_kids_ok(c, f)) {
        plain(conn, 404, "no such picture subtitle\n");
        return true;
    }
    PicJob *j = xcalloc(1, sizeof *j);
    bool dir = false;
    if (!media_full_path(f, j->path, sizeof j->path) || !path_stat(j->path, &j->size, &j->mtime, &dir) || dir) {
        free(j);
        plain(conn, 404, "the file has gone - rescan the library?\n");
        return true;
    }
    str_copy(j->data, sizeof j->data, j->path);
    if (idx) { /* its pictures: the .sub of the same name beside it */
        size_t k = strlen(j->data) - 3;
        bool there = false;
        for (int up = 0; up < 2 && !there; up++) {
            memcpy(j->data + k, up ? "SUB" : "sub", 3);
            there = path_stat(j->data, NULL, NULL, &dir) && !dir;
        }
        if (!there) {
            free(j);
            plain(conn, 404, "the .sub beside it has gone - rescan the library?\n");
            return true;
        }
    }
    if (atomic_load(&g_pic_jobs) >= PIC_JOBS || atomic_load(&g_active) >= MAX_STREAMS) {
        free(j);
        conn_reply(conn, 503, "text/plain; charset=utf-8", "busy, try again\n", 16, "Retry-After: 1\r\n");
        return true;
    }
    j->id = id;
    j->vobsub = idx;
    j->n = n;
    j->head_only = str_eq(r->method, "HEAD");
    atomic_fetch_add(&g_pic_jobs, 1);
    count_job(JOB_OTHER, +1);
    Detached d;
    conn_detach(conn, r, &d);
    j->s = d.fd;
    j->head = d.unsent;
    buf_free(&d.body);
    if (!thread_spawn(pic_thread, j)) {
        sock_close(j->s);
        buf_free(&j->head);
        free(j);
        atomic_fetch_sub(&g_pic_jobs, 1);
        count_job(JOB_OTHER, -1);
    }
    return true;
}

bool media_http(Ctx *c, Conn *conn, HttpReq *r) {
    bool file = str_starts(r->path, "/media/file/"), img = str_starts(r->path, "/media/img/"),
         sub = str_starts(r->path, "/media/sub/"), zip = str_starts(r->path, "/media/zip/"),
         pic = str_starts(r->path, "/media/subpic/"), cover = str_starts(r->path, "/media/cover/"),
         offline = str_starts(r->path, "/media/offline/");
    if (!file && !img && !sub && !zip && !pic && !cover && !offline) return false;
    /* The household's videos (and home photos used as posters) are for signed-in
       people only; browsers send the session cookie with <video>/<img> requests. */
    if (!c->me) {
        plain(conn, 401, "sign in to Hemma to watch\n");
        return true;
    }
    if (offline) return media_offline_http(c, conn, r); /* (the app's manifests: offline.c, 2.3) */
    if (zip) return serve_zip(c, conn, r);
    if (pic) return serve_subpic(c, conn, r);
    if (cover) {
        uint64_t id = 0;
        const MediaFile *f = media_parse_id(r->path + 13, &id) ? media_file(id) : NULL;
        if (!media_kids_ok(c, f)) {
            plain(conn, 404, "no picture\n");
            return true;
        }
        return serve_cover(conn, r, r->path + 13);
    }
    if (file) {
        uint64_t id = 0;
        const MediaFile *f = media_parse_id(r->path + 12, &id) ? media_file(id) : NULL;
        if (!media_kids_ok(c, f)) { /* (a child, or a TV with a child picked: not what's marked - issue #44) */
            plain(conn, 404, "not in the library\n");
            return true;
        }
        if (str_eq(ctx_q(c, "src"), "1") && is_admin(c)) /* (2.6) that very file, not its copy: what a device converts */
            return serve_file(conn, r, r->path + 12, 'X', NULL, -1);
        if (!str_eq(ctx_q(c, "download"), "1")) /* (?a=: which sound of an MP4 with several - 2.2) */
            return serve_file(conn, r, r->path + 12, MF_VIDEO, NULL, sound_asked(ctx_q(c, "a")));
        if (!media_grown_up(c)) { /* (the owner: downloads are for grown-ups - not children, guests or a TV) */
            say(conn, 403, T("Only grown-ups can download videos."), NULL);
            return true;
        }
        Buf dl = {0};
        if (f && (f->type == MF_VIDEO || f->type == MF_AUDIO)) { /* (an audiobook's file: its own name) */
            char name[600];
            if (f->type == MF_AUDIO) str_copy(name, sizeof name, strrchr(f->rel, '/') ? strrchr(f->rel, '/') + 1 : f->rel);
            else media_download_name(f, name, sizeof name);
            media_content_disposition(&dl, name);
            if (!str_eq(r->method, "HEAD") && !http_header(r, "Range"))
                log_info("media: %s downloads %s", c->me->username, name);
        }
        bool taken = serve_file(conn, r, r->path + 12, MF_VIDEO, buf_cstr(&dl), -1);
        buf_free(&dl);
        return taken;
    }
    if (img) return serve_file(conn, r, r->path + 11, MF_IMAGE, NULL, -1);
    return serve_sub(conn, r->path + 11);
}
