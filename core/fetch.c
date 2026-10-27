/* core/fetch.c — one plain-HTTP GET (fetch.h). */
#include "fetch.h"
#include "app.h" /* hemma_version */
#include "plat.h"

void fetched_free(Fetched *f) { buf_free(&f->body); }

static bool ieq_n(const char *a, const char *b, size_t n) { /* ASCII, case-insensitive */
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

static bool fail(Fetched *out, const char *fmt, const char *arg) {
    out->status = 0;
    snprintf(out->err, sizeof out->err, fmt, arg ? arg : "");
    return false;
}

/* a header's value (case-insensitive name) in head[0..n), or NULL */
static const char *header(const char *head, size_t n, const char *name, size_t *len) {
    size_t nl = strlen(name);
    for (const char *p = head; p && p < head + n;) {
        const char *eol = memchr(p, '\n', (size_t)(head + n - p));
        size_t line = eol ? (size_t)(eol - p) : (size_t)(head + n - p);
        if (line > nl && p[nl] == ':' && ieq_n(p, name, nl)) {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            *len = (size_t)(p + line - v);
            while (*len && (v[*len - 1] == '\r' || v[*len - 1] == ' ')) (*len)--;
            return v;
        }
        p = eol ? eol + 1 : NULL;
    }
    return NULL;
}

bool fetch_parse(const char *raw, size_t n, size_t max, Fetched *out) {
    buf_reset(&out->body);
    out->err[0] = out->location[0] = '\0';
    int major = 0, minor = 0, status = 0;
    if (n < 12 || sscanf(raw, "HTTP/%d.%d %d", &major, &minor, &status) != 3 || status < 100 || status > 999)
        return fail(out, "not an HTTP answer", NULL);
    const char *end = NULL;
    for (size_t i = 0; i + 3 < n && !end; i++)
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') end = raw + i;
    if (!end) return fail(out, "the answer's headers were cut short", NULL);
    size_t hn = (size_t)(end - raw), vl = 0;
    const char *body = end + 4, *stop = raw + n;
    const char *loc = header(raw, hn, "location", &vl);
    if (loc) str_ncopy(out->location, sizeof out->location, loc, vl);
    const char *te = header(raw, hn, "transfer-encoding", &vl);
    if (te && vl >= 7 && ieq_n(te + vl - 7, "chunked", 7)) {
        for (const char *p = body;;) { /* <hex size>[;ext]\r\n<data>\r\n ... 0\r\n */
            char *after;
            unsigned long long k = strtoull(p, &after, 16);
            const char *lf = memchr(p, '\n', (size_t)(stop - p));
            if (after == p || !lf) return fail(out, "a chunk was cut short", NULL);
            p = lf + 1;
            if (!k) break;
            if (k > (unsigned long long)(stop - p) || out->body.len + k > max)
                return fail(out, k > (unsigned long long)(stop - p) ? "a chunk was cut short" : "the answer was too big", NULL);
            buf_put(&out->body, p, (size_t)k);
            p += k;
            if (p < stop && *p == '\r') p++;
            if (p < stop && *p == '\n') p++;
        }
    } else {
        size_t have = (size_t)(stop - body), want = have;
        const char *cl = header(raw, hn, "content-length", &vl);
        if (cl) {
            char num[24];
            str_ncopy(num, sizeof num, cl, vl);
            long long len = 0;
            if (!str_int(num, &len) || len < 0) return fail(out, "a bad Content-Length", NULL);
            if ((size_t)len > have) return fail(out, "the answer was cut short", NULL);
            want = (size_t)len;
        }
        if (want > max) return fail(out, "the answer was too big", NULL);
        buf_put(&out->body, body, want);
    }
    buf_putc(&out->body, '\0'); /* (a C string for parsers; not counted) */
    out->body.len--;
    out->status = status;
    return true;
}

/* poll one socket; false = timed out or failed */
static bool wait_for(sock_t s, short what, int64_t deadline) {
    int64_t left = deadline - now_ms();
    if (left <= 0) return false;
    pollfd_t p = {0};
    p.fd = s;
    p.events = what;
    return plat_poll(&p, 1, (int)left) > 0 && (p.revents & (what | POLLHUP | POLLERR));
}

bool fetch_get(const char *host, int port, const char *path, size_t max, int timeout_ms, Fetched *out) {
    memset(out, 0, sizeof *out);
    int64_t deadline = now_ms() + timeout_ms;
    uint32_t ip = 0;
    if (!plat_resolve(host, &ip)) return fail(out, "%s couldn't be found (no internet, or no DNS?)", host);
    sock_t s = tcp_connect_nb(ip, port);
    if (s == SOCK_BAD) return fail(out, "couldn't connect to %s", host);
    if (!wait_for(s, POLLOUT, deadline) || !tcp_connect_done(s)) {
        sock_close(s);
        return fail(out, "couldn't connect to %s", host);
    }
    Buf req = {0}, got = {0};
    buf_printf(&req,
               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Hemma/%s (+https://github.com/kloparn/Hemma)\r\n"
               "Accept: application/json\r\nConnection: close\r\n\r\n",
               path, host, hemma_version());
    bool ok = true;
    for (size_t sent = 0; ok && sent < req.len;) {
        long k = sock_send(s, req.p + sent, req.len - sent);
        if (k > 0) sent += (size_t)k;
        else ok = k < 0 && sock_again(sock_errno()) && wait_for(s, POLLOUT, deadline);
    }
    size_t cap = max + (64 << 10); /* (the body, and room for the headers) */
    while (ok) {
        char chunk[16384];
        if (!wait_for(s, POLLIN, deadline)) {
            ok = false;
            break;
        }
        long k = sock_recv(s, chunk, sizeof chunk);
        if (k == 0) break; /* closed: all of it */
        if (k < 0) {
            ok = sock_again(sock_errno());
            continue;
        }
        buf_put(&got, chunk, (size_t)k);
        if (got.len > cap) ok = false;
    }
    sock_close(s);
    buf_free(&req);
    if (!ok) {
        bool big = got.len > cap;
        buf_free(&got);
        return fail(out, big ? "%s answered too much" : "%s didn't answer in time", host);
    }
    bool parsed = fetch_parse(got.p ? got.p : "", got.len, max, out);
    buf_free(&got);
    return parsed;
}
