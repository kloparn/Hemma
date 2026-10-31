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

/* ------------------------------------------------------------------ https */

static Lock g_curl_lock = LOCK_INIT;
static int g_curl_state; /* 0 not looked yet, 1 found, -1 none */
static char g_curl[400];

static bool runnable(const char *path) {
    bool dir = false;
    return path_stat(path, NULL, NULL, &dir) && !dir;
}

const char *fetch_curl(void) {
    lock_take(&g_curl_lock);
    if (!g_curl_state) {
        g_curl_state = -1;
        const char *win = plat_getenv("SystemRoot"); /* (Windows 10 1803 and later carry curl.exe there) */
        static const char *const unix_places[] = {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "/opt/homebrew/bin/curl"};
        if (win && *win) snprintf(g_curl, sizeof g_curl, "%s\\System32\\curl.exe", win);
        if (win && *win && runnable(g_curl)) g_curl_state = 1;
        for (int i = 0; g_curl_state < 0 && i < (int)countof(unix_places); i++)
            if (runnable(unix_places[i])) {
                str_copy(g_curl, sizeof g_curl, unix_places[i]);
                g_curl_state = 1;
            }
    }
    const char *c = g_curl_state > 0 ? g_curl : NULL;
    lock_give(&g_curl_lock);
    return c;
}

void fetch_test_curl(const char *path) {
    lock_take(&g_curl_lock);
    g_curl_state = !path ? 0 : *path ? 1 : -1;
    str_copy(g_curl, sizeof g_curl, path ? path : "");
    lock_give(&g_curl_lock);
}

int fetch_curl_status(const char *headers, size_t n) {
    int status = 0;
    for (const char *p = headers; p && p < headers + n;) {
        const char *eol = memchr(p, '\n', (size_t)(headers + n - p));
        int s = 0;
        if ((size_t)(headers + n - p) > 8 && !strncmp(p, "HTTP/", 5) && sscanf(p, "HTTP/%*s %d", &s) == 1 && s >= 100 && s <= 999)
            status = s;
        p = eol ? eol + 1 : NULL;
    }
    return status;
}

bool fetch_https(const char *url, size_t max, int timeout_ms, Fetched *out) {
    memset(out, 0, sizeof *out);
    const char *curl = fetch_curl();
    if (!curl) return fail(out, "there's no curl on this computer (https needs it)", NULL);
    if (strncmp(url, "https://", 8) != 0) return fail(out, "not an https address: %s", url);
    const char *tmp = plat_getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = plat_getenv("TEMP");
    if (!tmp || !*tmp) tmp = plat_getenv("TMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    char hex[17], heads[600], body[600], errs[600], secs[16], most[24], ua[96];
    rand_hex(hex, 8);
    snprintf(heads, sizeof heads, "%s/hemma-fetch-%s.h", tmp, hex);
    snprintf(body, sizeof body, "%s/hemma-fetch-%s.b", tmp, hex);
    snprintf(errs, sizeof errs, "%s/hemma-fetch-%s.e", tmp, hex);
    snprintf(secs, sizeof secs, "%d", timeout_ms / 1000 > 0 ? timeout_ms / 1000 : 1);
    snprintf(most, sizeof most, "%zu", max);
    snprintf(ua, sizeof ua, "Hemma/%s (+https://github.com/kloparn/Hemma)", hemma_version());
    const char *argv[] = {curl, "-sS", "-L", "--max-redirs", "3", "--proto", "=https", "--proto-redir", "=https",
                          "--connect-timeout", "10", "--max-time", secs, "--max-filesize", most, "-A", ua,
                          "-D", heads, "-o", body, "--stderr", errs, "--url", url, NULL};
    int rc = plat_run(argv, timeout_ms + 5000);
    out->exit = rc;
    Buf h = {0}, e = {0};
    file_read_all(heads, &h, 256 << 10);
    bool ok = false;
    if (rc == 0) {
        out->status = fetch_curl_status(h.p ? h.p : "", h.len);
        if (!out->status) fail(out, "curl gave no answer from %s", url);
        else if (!file_read_all(body, &out->body, max) && path_stat(body, NULL, NULL, NULL)) fail(out, "the answer was too big", NULL);
        else ok = true; /* (no body file: an empty answer) */
    } else if (rc == 63) {
        fail(out, "the answer was too big", NULL);
    } else {
        file_read_all(errs, &e, 4096);
        char line[160] = "", code[16];
        if (e.p) str_ncopy(line, sizeof line, e.p, strcspn(e.p, "\r\n"));
        snprintf(code, sizeof code, "%d", rc);
        if (rc < 0) fail(out, "curl didn't finish in time (or couldn't start)", NULL);
        else if (line[0]) fail(out, "%s", line);
        else fail(out, "curl stopped (exit %s)", code);
    }
    if (ok) {
        buf_putc(&out->body, '\0'); /* (a C string for parsers; not counted) */
        out->body.len--;
    } else {
        buf_free(&out->body);
    }
    file_remove(heads);
    file_remove(body);
    file_remove(errs);
    buf_free(&h);
    buf_free(&e);
    return ok;
}
