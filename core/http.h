/* core/http.h — HTTP/1.1 request parsing, form fields, cookies, responses.
 * Transport-free: works on byte buffers so it is easy to test. */
#ifndef HEMMA_HTTP_H
#define HEMMA_HTTP_H

#include "base.h"

#define HTTP_MAX_HEADER_BYTES 16384
#define HTTP_MAX_HEADERS 48

typedef struct {
    const char *name, *value;
} HttpHeader;

typedef struct {
    char method[8];
    char target[2048]; /* raw request target, e.g. "/book?r=2&w=2026-10-05" */
    char path[1024];   /* percent-decoded path without the query */
    const char *query; /* raw query (points into target, "" if none) */
    HttpHeader headers[HTTP_MAX_HEADERS];
    int nheaders;
    const char *body; /* points into the caller's buffer (NULL while streaming) */
    size_t body_len;
    size_t total_len; /* bytes of the request (headers + body; headers only while streaming) */
    size_t head_len;  /* bytes of the header block */
    long long content_length;
    bool streaming; /* body larger than max_body: it follows on the wire (see http_parse) */
    bool keep_alive;
    int minor; /* HTTP/1.minor */
    char *hbuf_;      /* private copy of the header block */
} HttpReq;

/* 1 = complete request, 0 = need more bytes, -status on error (400, 414, 431, 501, 505).
   2 = headers complete but Content-Length exceeds max_body: req->streaming is set,
   body is NULL and the content_length bytes after head_len are still to come. */
int http_parse(const char *data, size_t len, size_t max_body, HttpReq *req);
void http_req_free(HttpReq *req);
const char *http_header(const HttpReq *r, const char *name); /* case-insensitive; NULL if absent */
bool http_header_has(const HttpReq *r, const char *name, const char *token); /* comma-list contains token */
bool http_cookie(const HttpReq *r, const char *name, char *out, size_t cap);
/* No Origin header, or Origin's host equals Host: refuses cross-site form posts and WebSockets. */
bool http_same_origin(const HttpReq *r);
/* Host is an IP literal, a single-label name ("hemma"), a private-use name
   ("hemma.local", ".lan", ".home.arpa", ...) or listed in `extra` (comma list;
   ".suffix" entries match subdomains). Refuses DNS-rebinding names. */
bool http_host_ok(const HttpReq *r, const char *extra);

/* ---- form/query fields: owns copies of every key and value ---- */
#define FIELDS_MAX 64
typedef struct {
    int n;
    char *k[FIELDS_MAX], *v[FIELDS_MAX];
} Fields;

void fields_parse(Fields *f, const char *urlencoded, size_t n); /* appends; '+' = space; invalid UTF-8 cleaned */
void fields_add(Fields *f, const char *k, const char *v);       /* appends copies (ignored when full) */
void fields_set(Fields *f, const char *k, const char *v);       /* replace first or append */
const char *fields_get(const Fields *f, const char *k);         /* first value or NULL */
const char *fields_str(const Fields *f, const char *k);         /* first value or "" */
long long fields_int(const Fields *f, const char *k, long long dflt);
void fields_free(Fields *f);

/* ---- responses ---- */
const char *http_status_text(int status);
/* Status line + standard headers (+ extra, each line ending in \r\n) + blank line. */
void http_head(Buf *out, int status, const char *ctype, long long content_len, const char *extra, bool keep_alive);
void http_reply(Buf *out, int status, const char *ctype, const void *body, size_t n, const char *extra, bool keep_alive);

/* Range: "bytes=a-b" / "bytes=a-" / "bytes=-n" against a resource of `size` bytes.
 * Returns 1 = valid range (sets [*from, *to] inclusive), 0 = no/ignored header, -1 = unsatisfiable. */
int http_range(const char *header, int64_t size, int64_t *from, int64_t *to);

/* Content type guessed from a file name's extension. */
const char *http_mime(const char *filename);

/* Strip invalid UTF-8 (replaced by '?') in place; returns s. */
char *utf8_scrub(char *s);

#endif
