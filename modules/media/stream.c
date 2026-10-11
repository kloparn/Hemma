/* modules/media/stream.c — HTTP endpoints for the media module:
 *
 *   /media/file/<id>      the video itself, with Range support (seeking)
 *   /media/img/<id>       poster / folder art
 *   /media/sub/<id>.vtt   subtitles, SRT converted to WebVTT on the fly
 *
 * Files are streamed by worker threads: the request is parsed on the event
 * loop, then the socket is detached and handed to a thread that sends the
 * headers and the byte range. Sends are non-blocking with a stall timeout,
 * so a paused or vanished client never pins a thread for long; the browser
 * simply asks for the next range when it needs it. */
#define _FILE_OFFSET_BITS 64 /* 64-bit fseeko/off_t for > 4 GB files on 32-bit Pi and mingw */
#include "library.h"
#include "../../core/plat.h"

#include <stdatomic.h>

#define CHUNK (256 * 1024)
#define MAX_STREAMS 24
#define STALL_MS 45000

static atomic_int g_active;
static atomic_bool g_stopping;

int media_streams_active(void) { return atomic_load(&g_active); }
void media_stream_stop(void) { atomic_store(&g_stopping, true); }
void media_stream_resume(void) { atomic_store(&g_stopping, false); }

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

/* -------------------------------------------------------------- sending */

static bool send_all(sock_t s, const char *p, size_t n) {
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
            if (rc == 0 && (waited += 1000) >= STALL_MS) return false; /* client stopped reading */
            continue;
        }
        return false; /* connection reset: the browser moved on */
    }
    return true;
}

/* Read + send through one fixed buffer (Windows, or where sendfile can't). */
static void send_by_copy(sock_t s, FILE *f, int64_t from, int64_t left) {
    if (!file_seek(f, from)) return;
    char *buf = xmalloc(CHUNK);
    while (left > 0) {
        size_t want = left < CHUNK ? (size_t)left : CHUNK;
        size_t got = fread(buf, 1, want, f);
        if (!got || !send_all(s, buf, got)) break;
        left -= (int64_t)got;
    }
    free(buf);
}

/* Zero-copy on Linux (the Pi): the kernel moves pages from the disk cache
   straight to the socket - no copy through us, no buffer, almost no CPU. */
static void send_range(sock_t s, FILE *f, int64_t from, int64_t left) {
    int waited = 0;
    while (left > 0) {
        if (atomic_load(&g_stopping)) return;
        long k = file_sendfile(s, f, from, (size_t)(left < (1 << 30) ? left : (1 << 30)));
        if (k == -2) { /* not available here */
            send_by_copy(s, f, from, left);
            return;
        }
        if (k > 0) {
            from += k;
            left -= k;
            waited = 0;
            continue;
        }
        if (k < 0) return; /* the browser moved on, or the file shrank */
        pollfd_t pf = {.fd = s, .events = POLLOUT};
        int rc = plat_poll(&pf, 1, 1000);
        if (rc < 0 || (rc > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL)))) return;
        if (rc == 0 && (waited += 1000) >= STALL_MS) return; /* paused player / vanished client */
    }
}

void media_stream_run(sock_t s, const char *path, int64_t from, int64_t len, const char *head, size_t head_len) {
    sock_nonblock(s, true);
    bool ok = send_all(s, head, head_len);
    FILE *f = ok && len > 0 ? file_open(path, "rb") : NULL;
    if (f) {
        send_range(s, f, from, len);
        fclose(f);
    }
    sock_shutdown_write(s);
    sock_close(s);
}

typedef struct {
    sock_t s;
    char path[1024];
    int64_t from, len;
    Buf head;
} Job;

static void stream_thread(void *arg) {
    Job *j = arg;
    media_stream_run(j->s, j->path, j->from, j->len, j->head.p, j->head.len);
    buf_free(&j->head);
    free(j);
    atomic_fetch_sub(&g_active, 1);
}

/* ------------------------------------------------------------ endpoints */

static void plain(Conn *conn, int status, const char *msg) {
    conn_reply(conn, status, "text/plain; charset=utf-8", msg, strlen(msg), NULL);
}

static bool serve_file(Conn *conn, HttpReq *r, const char *idhex, char want) {
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
    atomic_fetch_add(&g_active, 1);
    Detached d;
    conn_detach(conn, r, &d);
    j->s = d.fd;
    j->head = d.unsent; /* rare: answers to earlier pipelined requests go out first */
    buf_put(&j->head, head.p, head.len);
    buf_free(&head);
    buf_free(&d.body);
    if (!thread_spawn(stream_thread, j)) {
        atomic_fetch_sub(&g_active, 1);
        sock_close(j->s);
        buf_free(&j->head);
        free(j);
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
         sub = str_starts(r->path, "/media/sub/");
    if (!file && !img && !sub) return false;
    /* The household's videos (and home photos used as posters) are for signed-in
       people only; browsers send the session cookie with <video>/<img> requests. */
    if (!c->me) {
        plain(conn, 401, "sign in to Hemma to watch\n");
        return true;
    }
    if (file) return serve_file(conn, r, r->path + 12, MF_VIDEO);
    if (img) return serve_file(conn, r, r->path + 11, MF_IMAGE);
    return serve_sub(conn, r->path + 11);
}
