/* core/fetch.h — Hemma's own way out to the internet (1.4): one plain-HTTP GET, on a thread.
 *
 * For a module that looks something up for the household once an admin has switched it on (media's
 * genres: TVmaze, OMDb - modules/media/lookup.c). There's no TLS in Hemma (no library but SQLite), so only
 * services that answer over plain http:// can be asked, and nothing secret should go this way. It BLOCKS
 * (the name lookup, connecting, reading - each within the timeout): never call it on the event loop. */
#ifndef HEMMA_FETCH_H
#define HEMMA_FETCH_H
#include "base.h"

typedef struct {
    int status;    /* the server's answer (200, 404 ...); 0 = none came: err says why */
    Buf body;      /* its body, de-chunked */
    char location[300]; /* a redirect's Location (followed by the caller, if it wants: maybe https) */
    char err[160]; /* in English, for the log */
    int exit;      /* fetch_https: curl's exit code (6, 7, 28: no network; -1: it didn't finish or start) */
} Fetched;

/* GET http://host[:port]path ("/search/shows?q=x"). max = the most body bytes taken (a longer answer is an
   error). true = an answer came (any status). Free with fetched_free. */
bool fetch_get(const char *host, int port, const char *path, size_t max, int timeout_ms, Fetched *out);
void fetched_free(Fetched *f);
/* The parser alone (tests): a whole response as it came off the socket (the server closed it). */
bool fetch_parse(const char *raw, size_t n, size_t max, Fetched *out);

/* HTTPS (2.5; owner, 2026-10-30: film posters from Wikipedia, which answers only over https - "use curl for https"):
   GET an https:// address through the computer's OWN curl (fetch_curl), which checks the certificate with the
   computer's own list - Hemma still has no TLS of its own. Redirects followed (3 at most, https only); the answer's
   status and body as fetch_get's (location: none). It runs a program and waits: threads only, never the loop. */
bool fetch_https(const char *url, size_t max, int timeout_ms, Fetched *out);
const char *fetch_curl(void); /* curl's path on this computer (Windows 10+: System32's), NULL = none */
/* Tests: curl's -D file (each answer's headers, redirects first) -> the last answer's status, 0 = none; and a
   curl to pretend ("" = none here, NULL = look again). */
int fetch_curl_status(const char *headers, size_t n);
void fetch_test_curl(const char *path);

#endif
