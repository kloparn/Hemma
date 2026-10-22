/* modules/media/stream.c — HTTP endpoints for the media module:
 *
 *   /media/file/<id>      the video itself, with Range support (seeking); ?download=1 = as a download
 *   /media/zip/<series>   a season (?s=<season>) or the whole series as one .zip (download.h)
 *   /media/img/<id>       poster / folder art
 *   /media/sub/<id>.vtt   subtitles, SRT converted to WebVTT on the fly
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
#include "zip.h"
#include "../../core/plat.h"

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

int media_streams_active(void) { return atomic_load(&g_active); }
void media_stream_stop(void) { atomic_store(&g_stopping, true); }
void media_stream_resume(void) { atomic_store(&g_stopping, false); }
int media_downloads_active(void) { return atomic_load(&g_downloads); }
int media_zips_active(void) { return atomic_load(&g_zips); }
int media_watching_active(void) { return atomic_load(&g_watching); }
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
    if (atomic_load(&g_watching) + atomic_load(&g_fake_watching) <= 0) return 0;
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

/* Read + send through one fixed buffer (Windows, or where sendfile can't). */
static void send_by_copy(sock_t s, FILE *f, int64_t from, int64_t left, int stall_ms, Pace *pc) {
    if (!file_seek(f, from)) return;
    char *buf = xmalloc(CHUNK);
    while (left > 0) {
        size_t want = left < CHUNK ? (size_t)left : CHUNK;
        size_t got = fread(buf, 1, want, f);
        if (!got || !send_paced(s, buf, got, stall_ms, pc)) break;
        left -= (int64_t)got;
    }
    free(buf);
}

/* Zero-copy on Linux (the Pi): the kernel moves pages from the disk cache
   straight to the socket - no copy through us, no buffer, almost no CPU. */
static void send_range(sock_t s, FILE *f, int64_t from, int64_t left, int stall_ms, Pace *pc) {
    int waited = 0;
    while (left > 0) {
        if (atomic_load(&g_stopping)) return;
        size_t want = (size_t)(left < (1 << 30) ? left : (1 << 30));
        if (!pace(pc, &want)) return;
        long k = file_sendfile(s, f, from, want);
        if (k == -2) { /* not available here */
            send_by_copy(s, f, from, left, stall_ms, pc);
            return;
        }
        if (k > 0) {
            from += k;
            left -= k;
            if (pc) pc->sent += k;
            waited = 0;
            continue;
        }
        if (k < 0) return; /* the browser moved on, or the file shrank */
        pollfd_t pf = {.fd = s, .events = POLLOUT};
        int rc = plat_poll(&pf, 1, 1000);
        if (rc < 0 || (rc > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL)))) return;
        if (rc == 0 && (waited += 1000) >= stall_ms) return; /* paused player / vanished client */
    }
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
    buf_free(&j->head);
    free(j);
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

/* dl: the Content-Disposition line of a download (NULL = an ordinary request) */
static bool serve_file(Conn *conn, HttpReq *r, const char *idhex, char want, const char *dl) {
    if (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD")) {
        conn_reply(conn, 405, "text/plain; charset=utf-8", "GET only\n", 9, "Allow: GET, HEAD\r\n");
        return true;
    }
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(idhex, &id) ? media_file(id) : NULL;
    if (f && want == MF_VIDEO) f = media_playable(f); /* its play-everywhere copy, when converted */
    char path[1024];
    int64_t size = 0;
    bool dir = false;
    if (!f || f->type != want) {
        plain(conn, 404, "not in the library\n");
        return true;
    }
    if (!media_full_path(f, path, sizeof path) || !path_stat(path, &size, NULL, &dir) || dir) {
        plain(conn, 404, "the file has gone - rescan the library?\n");
        return true;
    }
    const char *ctype = want == MF_IMAGE                ? http_mime(f->rel)
                        : f->probe.container[0] ? media_mime(f->probe.container)
                                                : http_mime(f->rel);
    MediaRange rg = media_range(http_header(r, "Range"), size);
    Buf head = {0};
    /* a picture URL carries its version (?v=): a new picture is a new URL, so it can stay cached */
    const char *cache = want != MF_IMAGE                ? "no-cache"
                        : strstr(r->query, "v=") ? "private, max-age=31536000, immutable"
                                                  : "private, max-age=86400";
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
    j->from = rg.from;
    j->len = rg.len;
    j->kind = dl ? JOB_DOWNLOAD : want == MF_VIDEO ? JOB_WATCH : JOB_OTHER;
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
            free(j->parts[i].path);
            free(j->parts[i].name);
            continue;
        }
        j->parts[n] = j->parts[i];
        DateTime t = dt_local(mtime);
        e[n] = (ZipEntry){.name = j->parts[n].name, .size = (uint64_t)size};
        zip_dos_time(t.year, t.mon, t.day, t.hour, t.min, t.sec, &e[n].dos_time, &e[n].dos_date);
        n++;
    }
    j->n = n;
    buf_printf(&j->head,
               "HTTP/1.1 200 OK\r\nServer: hemma\r\nX-Content-Type-Options: nosniff\r\nContent-Type: application/zip\r\n"
               "Content-Length: %llu\r\nAccept-Ranges: none\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
               (unsigned long long)zip_size(e, n), buf_cstr(&j->disposition));
    sock_nonblock(j->s, true);
    if (send_wait(j->s, j->head.p, j->head.len, DOWNLOAD_STALL_MS) && !j->head_only) {
        char *buf = xmalloc(CHUNK);
        ZipIo io = {zip_read, zip_send, j};
        j->open = -1;
        if (!zip_write(e, n, &io, buf, CHUNK)) log_info("media: a download stopped before its end");
        free(buf);
    }
    if (j->f) fclose(j->f);
    sock_shutdown_write(j->s);
    sock_close(j->s);
    for (int i = 0; i < j->n; i++) free(j->parts[i].path), free(j->parts[i].name);
    free(j->parts);
    free(e);
    buf_free(&j->head);
    buf_free(&j->disposition);
    free(j);
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
    if (!s) {
        say(conn, 404, T("This series isn't in the library"), NULL);
        return true;
    }
    const char *sq = ctx_q(c, "s");
    long long season = -1;
    int n = 0;
    MediaZipItem *items = NULL;
    if (!*sq || (str_int(sq, &season) && season >= 0 && season < 10000))
        items = media_zip_items(s, *sq ? (int)season : -1, &n);
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
    char zname[300];
    media_zip_name(s, *sq ? (int)season : -1, zname, sizeof zname);
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

bool media_http(Ctx *c, Conn *conn, HttpReq *r) {
    bool file = str_starts(r->path, "/media/file/"), img = str_starts(r->path, "/media/img/"),
         sub = str_starts(r->path, "/media/sub/"), zip = str_starts(r->path, "/media/zip/");
    if (!file && !img && !sub && !zip) return false;
    /* The household's videos (and home photos used as posters) are for signed-in
       people only; browsers send the session cookie with <video>/<img> requests. */
    if (!c->me) {
        plain(conn, 401, "sign in to Hemma to watch\n");
        return true;
    }
    if (zip) return serve_zip(c, conn, r);
    if (file) {
        uint64_t id = 0;
        const MediaFile *f = media_parse_id(r->path + 12, &id) ? media_file(id) : NULL;
        if (!media_kids_ok(c, f)) { /* (a child, or a TV with a child picked: not what's marked - issue #44) */
            plain(conn, 404, "not in the library\n");
            return true;
        }
        if (!str_eq(ctx_q(c, "download"), "1")) return serve_file(conn, r, r->path + 12, MF_VIDEO, NULL);
        if (!media_grown_up(c)) { /* (the owner: downloads are for grown-ups - not children, guests or a TV) */
            say(conn, 403, T("Only grown-ups can download videos."), NULL);
            return true;
        }
        Buf dl = {0};
        if (f && f->type == MF_VIDEO) {
            char name[600];
            media_download_name(f, name, sizeof name);
            media_content_disposition(&dl, name);
            if (!str_eq(r->method, "HEAD") && !http_header(r, "Range"))
                log_info("media: %s downloads %s", c->me->username, name);
        }
        bool taken = serve_file(conn, r, r->path + 12, MF_VIDEO, buf_cstr(&dl));
        buf_free(&dl);
        return taken;
    }
    if (img) return serve_file(conn, r, r->path + 11, MF_IMAGE, NULL);
    return serve_sub(conn, r->path + 11);
}
