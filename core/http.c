/* core/http.c — see http.h */
#include "http.h"
#include "codec.h"

/* -------------------------------------------------------------- requests */

static bool is_tchar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c);
}

static const char *find_header_end(const char *d, size_t len, size_t *hlen) {
    size_t lim = MIN(len, (size_t)HTTP_MAX_HEADER_BYTES);
    for (size_t i = 0; i + 1 < lim; i++) {
        if (d[i] != '\n') continue;
        if (d[i + 1] == '\n') {
            *hlen = i + 2;
            return d + i;
        }
        if (i + 2 < lim && d[i + 1] == '\r' && d[i + 2] == '\n') {
            *hlen = i + 3;
            return d + i;
        }
    }
    return NULL;
}

void http_req_free(HttpReq *req) {
    free(req->hbuf_);
    req->hbuf_ = NULL;
}

int http_parse(const char *data, size_t len, size_t max_body, HttpReq *req) {
    memset(req, 0, sizeof *req);
    /* tolerate a few stray CRLFs between pipelined requests (RFC 9112 2.2) */
    size_t lead = 0;
    while (lead < len && (data[lead] == '\r' || data[lead] == '\n')) lead++;
    if (lead > 64) return -400; /* not a stray CRLF: someone feeding us blank lines */
    if (lead == len) return 0;
    data += lead, len -= lead;

    size_t hlen = 0;
    if (!find_header_end(data, len, &hlen)) {
        if (memchr(data, '\0', MIN(len, (size_t)HTTP_MAX_HEADER_BYTES))) return -400;
        return len >= HTTP_MAX_HEADER_BYTES ? -431 : 0;
    }
    if (memchr(data, '\0', hlen)) return -400; /* the string handling below relies on this */

    char *h = xmalloc(hlen + 1);
    memcpy(h, data, hlen);
    h[hlen] = '\0';
    req->hbuf_ = h;

    /* request line */
    char *line_end = strchr(h, '\n');
    if (!line_end) goto bad;
    *line_end = '\0';
    if (line_end > h && line_end[-1] == '\r') line_end[-1] = '\0';
    char *sp1 = strchr(h, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || sp1 - h >= (long)sizeof req->method || sp1 == h) goto bad;
    for (char *p = h; p < sp1; p++)
        if (*p < 'A' || *p > 'Z') goto bad;
    memcpy(req->method, h, (size_t)(sp1 - h));
    size_t tlen = (size_t)(sp2 - sp1 - 1);
    if (tlen == 0 || sp1[1] != '/') goto bad;
    if (tlen >= sizeof req->target) {
        http_req_free(req);
        return -414;
    }
    memcpy(req->target, sp1 + 1, tlen);
    req->target[tlen] = '\0';
    for (char *p = req->target; *p; p++)
        if ((unsigned char)*p <= ' ' || *p == 0x7F) goto bad;
    const char *ver = sp2 + 1;
    if (strcmp(ver, "HTTP/1.1") == 0) req->minor = 1;
    else if (strcmp(ver, "HTTP/1.0") == 0) req->minor = 0;
    else {
        http_req_free(req);
        return -505;
    }

    /* path + query */
    char *q = strchr(req->target, '?');
    size_t plen = q ? (size_t)(q - req->target) : tlen;
    req->query = q ? q + 1 : "";
    if (plen >= sizeof req->path) goto bad;
    size_t decoded = url_decode(req->target, plen, req->path, false);
    if (strlen(req->path) != decoded) goto bad; /* embedded %00 */

    /* headers */
    char *p = line_end + 1;
    while (*p && *p != '\r' && *p != '\n') {
        char *eol = strchr(p, '\n');
        if (!eol) goto bad;
        *eol = '\0';
        if (eol > p && eol[-1] == '\r') eol[-1] = '\0';
        if (*p == ' ' || *p == '\t') goto bad; /* obsolete line folding */
        char *colon = strchr(p, ':');
        if (!colon || colon == p) goto bad;
        for (char *c = p; c < colon; c++)
            if (!is_tchar(*c)) goto bad;
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = '\0';
        if (req->nheaders == HTTP_MAX_HEADERS) {
            http_req_free(req);
            return -431;
        }
        req->headers[req->nheaders++] = (HttpHeader){p, v};
        p = eol + 1;
    }

    /* body */
    if (http_header(req, "Transfer-Encoding")) {
        http_req_free(req);
        return -501;
    }
    long long clen = 0;
    const char *cl = NULL;
    for (int i = 0; i < req->nheaders; i++) {
        if (!str_ieq(req->headers[i].name, "Content-Length")) continue;
        long long v;
        if (!str_int(req->headers[i].value, &v) || v < 0 || req->headers[i].value[0] == '+' ||
            req->headers[i].value[0] == '-')
            goto bad;
        if (cl && v != clen) goto bad; /* conflicting duplicates */
        cl = req->headers[i].value;
        clen = v;
    }
    req->content_length = clen;
    req->head_len = lead + hlen;
    if (req->minor == 1) req->keep_alive = !http_header_has(req, "Connection", "close");
    else req->keep_alive = http_header_has(req, "Connection", "keep-alive");
    if ((unsigned long long)clen > max_body) {
        /* Too big to buffer: the headers are complete and the body follows on
           the wire. Whoever handles it reads it straight from the socket. */
        req->streaming = true;
        req->total_len = req->head_len;
        return 2;
    }
    if (len - hlen < (size_t)clen) {
        http_req_free(req);
        return 0;
    }
    req->body = data + hlen;
    req->body_len = (size_t)clen;
    req->total_len = lead + hlen + (size_t)clen;
    return 1;

bad:
    http_req_free(req);
    return -400;
}

const char *http_header(const HttpReq *r, const char *name) {
    for (int i = 0; i < r->nheaders; i++)
        if (str_ieq(r->headers[i].name, name)) return r->headers[i].value;
    return NULL;
}

static bool span_ieq(const char *s, size_t n, const char *t) {
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = t[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b || !b) return false;
    }
    return t[n] == '\0';
}

bool http_header_has(const HttpReq *r, const char *name, const char *token) {
    for (int i = 0; i < r->nheaders; i++) {
        if (!str_ieq(r->headers[i].name, name)) continue;
        const char *s = r->headers[i].value;
        while (*s) {
            while (*s == ' ' || *s == '\t' || *s == ',') s++;
            const char *e = s;
            while (*e && *e != ',') e++;
            const char *te = e;
            while (te > s && (te[-1] == ' ' || te[-1] == '\t')) te--;
            if (span_ieq(s, (size_t)(te - s), token)) return true;
            s = e;
        }
    }
    return false;
}

bool http_same_origin(const HttpReq *r) {
    const char *o = http_header(r, "Origin");
    if (!o) return true;
    if (str_eq(o, "null")) return false;
    const char *host = http_header(r, "Host");
    const char *p = str_starts(o, "http://") ? o + 7 : str_starts(o, "https://") ? o + 8 : NULL;
    return p && host && str_ieq(p, host);
}

static bool ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

/* host[0..hl) equals the name e[0..n), or lies under it when e starts with '.' */
static bool host_matches(const char *host, size_t hl, const char *e, size_t n) {
    if (!n) return false;
    if (e[0] == '.') return hl > n && ieq_n(host + hl - n, e, n);
    return hl == n && ieq_n(host, e, n);
}

bool http_host_ok(const HttpReq *r, const char *extra) {
    static const char *const private_suffix[] = {".local", ".lan", ".home", ".home.arpa", ".internal", ".localdomain",
                                                 ".localhost", ".intranet", ".private", ".corp", ".fritz.box"};
    const char *h = http_header(r, "Host");
    if (!h || !*h) return true;                     /* HTTP/1.0 tools; browsers always send one */
    if (h[0] == '[') return strchr(h, ']') != NULL; /* IPv6 literal */
    size_t hl = strcspn(h, ":");
    if (hl && h[hl - 1] == '.') hl--; /* "hemma.local." */
    if (!hl || hl > 253) return false;
    bool digits_only = true, dot = false;
    for (size_t i = 0; i < hl; i++) {
        char ch = h[i];
        if (ch == '.') dot = true;
        else if (ch < '0' || ch > '9') digits_only = false;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' ||
              ch == '-' || ch == '_'))
            return false;
    }
    if (digits_only || !dot) return true; /* IPv4 literal, or a single-label name like "hemma" / "raspberrypi" */
    for (size_t i = 0; i < countof(private_suffix); i++)
        if (host_matches(h, hl, private_suffix[i], strlen(private_suffix[i]))) return true;
    for (const char *s = extra; s && *s;) { /* HEMMA_HOSTS: "hemma.example.se, .fritz.box" */
        while (*s == ',' || *s == ' ') s++;
        const char *e = s;
        while (*e && *e != ',' && *e != ' ') e++;
        if (host_matches(h, hl, s, (size_t)(e - s))) return true;
        s = e;
    }
    return false;
}

bool http_cookie(const HttpReq *r, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (int i = 0; i < r->nheaders; i++) {
        if (!str_ieq(r->headers[i].name, "Cookie")) continue;
        const char *s = r->headers[i].value;
        while (*s) {
            while (*s == ' ' || *s == ';') s++;
            const char *eq = strchr(s, '=');
            const char *semi = strchr(s, ';');
            if (!semi) semi = s + strlen(s);
            if (eq && eq < semi && (size_t)(eq - s) == nl && strncmp(s, name, nl) == 0) {
                str_ncopy(out, cap, eq + 1, (size_t)(semi - eq - 1));
                return true;
            }
            s = semi;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- fields */

char *utf8_scrub(char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        if (k == 1 && (unsigned char)s[i] >= 0x80) s[i] = '?';
        else if (cp < 0x20 && cp != '\n' && cp != '\t') s[i] = ' '; /* control chars -> space (\r too) */
        i += (size_t)k;
    }
    return s;
}

void fields_add(Fields *f, const char *k, const char *v) {
    if (f->n >= FIELDS_MAX) return;
    f->k[f->n] = xstrdup(k);
    f->v[f->n] = xstrdup(v);
    f->n++;
}

void fields_set(Fields *f, const char *k, const char *v) {
    for (int i = 0; i < f->n; i++) {
        if (strcmp(f->k[i], k) == 0) {
            free(f->v[i]);
            f->v[i] = xstrdup(v);
            return;
        }
    }
    fields_add(f, k, v);
}

void fields_parse(Fields *f, const char *s, size_t n) {
    size_t i = 0;
    while (i < n && f->n < FIELDS_MAX) {
        size_t j = i;
        while (j < n && s[j] != '&') j++;
        if (j > i) {
            const char *eq = memchr(s + i, '=', j - i);
            size_t kl = eq ? (size_t)(eq - (s + i)) : j - i;
            char *k = xmalloc(kl + 1);
            url_decode(s + i, kl, k, true);
            const char *vs = eq ? eq + 1 : s + j;
            size_t vl = (size_t)(s + j - vs);
            char *v = xmalloc(vl + 1);
            url_decode(vs, vl, v, true);
            utf8_scrub(k);
            utf8_scrub(v);
            f->k[f->n] = k;
            f->v[f->n] = v;
            f->n++;
        }
        i = j + 1;
    }
}

const char *fields_get(const Fields *f, const char *k) {
    for (int i = 0; i < f->n; i++)
        if (strcmp(f->k[i], k) == 0) return f->v[i];
    return NULL;
}

const char *fields_str(const Fields *f, const char *k) {
    const char *v = fields_get(f, k);
    return v ? v : "";
}

long long fields_int(const Fields *f, const char *k, long long dflt) { return str_int_or(fields_get(f, k), dflt); }

void fields_free(Fields *f) {
    for (int i = 0; i < f->n; i++) {
        free(f->k[i]);
        free(f->v[i]);
    }
    f->n = 0;
}

/* ------------------------------------------------------------- responses */

const char *http_status_text(int s) {
    switch (s) {
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Unknown";
    }
}

void http_head(Buf *out, int status, const char *ctype, long long content_len, const char *extra, bool keep_alive) {
    buf_printf(out, "HTTP/1.1 %d %s\r\n", status, http_status_text(status));
    buf_puts(out, "Server: hemma\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: same-origin\r\n");
    if (ctype && str_starts(ctype, "text/html"))
        buf_puts(out, "X-Frame-Options: DENY\r\nContent-Security-Policy: default-src 'self'; img-src 'self' data: blob:; "
                      "media-src 'self' blob:; style-src 'self' 'unsafe-inline'; script-src 'self' 'wasm-unsafe-eval'; "
                      "connect-src 'self'; "
                      "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n");
    if (ctype) buf_printf(out, "Content-Type: %s\r\n", ctype);
    if (content_len >= 0 && status != 204 && status != 304) buf_printf(out, "Content-Length: %lld\r\n", content_len);
    buf_puts(out, keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    if (extra) buf_puts(out, extra);
    buf_puts(out, "\r\n");
}

void http_reply(Buf *out, int status, const char *ctype, const void *body, size_t n, const char *extra, bool keep_alive) {
    http_head(out, status, ctype, (long long)n, extra, keep_alive);
    buf_put(out, body, n);
}

int http_range(const char *h, int64_t size, int64_t *from, int64_t *to) {
    if (!h || !str_istarts(h, "bytes=")) return 0;
    h += 6;
    if (strchr(h, ',')) return 0; /* multipart ranges: just send everything */
    const char *dash = strchr(h, '-');
    if (!dash) return 0;
    long long a = -1, b = -1;
    char tmp[32];
    size_t al = (size_t)(dash - h), bl = strlen(dash + 1);
    if (al >= sizeof tmp || bl >= sizeof tmp) return 0;
    if (al) {
        memcpy(tmp, h, al);
        tmp[al] = 0;
        if (!str_int(tmp, &a) || a < 0) return 0;
    }
    if (bl) {
        if (!str_int(dash + 1, &b) || b < 0) return 0;
    }
    if (!al && !bl) return 0;
    if (!al) { /* suffix: last b bytes */
        if (b == 0) return -1;
        *from = size > b ? size - b : 0;
        *to = size - 1;
        return size > 0 ? 1 : -1;
    }
    if (a >= size) return -1;
    *from = a;
    *to = (bl && b < size) ? b : size - 1;
    if (*to < *from) return 0;
    return 1;
}

const char *http_mime(const char *name) {
    static const struct {
        const char *ext, *type;
    } t[] = {
        {".html", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".webp", "image/webp"},
        {".gif", "image/gif"},
        {".ico", "image/x-icon"},
        {".txt", "text/plain; charset=utf-8"},
        {".vtt", "text/vtt; charset=utf-8"},
        {".mp4", "video/mp4"},
        {".m4v", "video/mp4"},
        {".mov", "video/quicktime"},
        {".webm", "video/webm"},
        {".mkv", "video/x-matroska"},
        {".mp3", "audio/mpeg"},
        {".m4a", "audio/mp4"},
        {".flac", "audio/flac"},
        {".ogg", "audio/ogg"},
        {".opus", "audio/ogg"},
        {".wav", "audio/wav"},
        {".webmanifest", "application/manifest+json"},
        {".woff2", "font/woff2"},
        {".wasm", "application/wasm"},
    };
    const char *dot = name ? strrchr(name, '.') : NULL;
    for (int i = 0; dot && i < countof(t); i++)
        if (str_ieq(dot, t[i].ext)) return t[i].type;
    return "application/octet-stream";
}
