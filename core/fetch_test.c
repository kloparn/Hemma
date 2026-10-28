/* core/fetch.c's answer parser (1.4): what a plain-HTTP server sends back, read whole. (fetch_get itself goes
   out to the internet: never under tests.) */
#include "../tests/test.h"
#include "fetch.h"

static bool parse(const char *raw, size_t max, Fetched *f) { /* (f: the last answer, freed first - or {0}) */
    fetched_free(f);
    memset(f, 0, sizeof *f);
    return fetch_parse(raw, strlen(raw), max, f);
}

TEST(fetch_reads_an_answer_with_its_length) {
    Fetched f = {0};
    CHECK(parse("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\ncontent-length: 7\r\n\r\n{\"a\":1}extra", 100, &f));
    CHECK_INT(f.status, 200);
    CHECK_INT((int)f.body.len, 7);
    CHECK_STR(f.body.p, "{\"a\":1}"); /* (NUL-ended for parsers; what came after the length isn't the body) */
    CHECK(!parse("HTTP/1.1 200 OK\r\nContent-Length: 70\r\n\r\nshort", 100, &f)); /* cut short */
    CHECK_HAS(f.err, "cut short");
    CHECK(!parse("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n{\"a\":1}", 5, &f)); /* too big */
    CHECK(parse("HTTP/1.0 404 Not Found\r\n\r\nnothing", 100, &f)); /* (no length: to the end) */
    CHECK_INT(f.status, 404);
    CHECK_STR(f.body.p, "nothing");
    fetched_free(&f);
}

TEST(fetch_reads_a_chunked_answer_and_a_redirect) {
    Fetched f = {0};
    CHECK(parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n6;x=y\r\npedia \r\nE\r\nin \r\n\r\nchunks.\r\n0\r\n\r\n",
                100, &f));
    CHECK_STR(f.body.p, "Wikipedia in \r\n\r\nchunks.");
    CHECK(!parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n40\r\nshort\r\n", 100, &f));
    CHECK(!parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nWiki\r\n6\r\npedia \r\n0\r\n\r\n", 8, &f));
    CHECK_HAS(f.err, "too big");
    CHECK(parse("HTTP/1.1 301 Moved Permanently\r\nLocation: https://api.tvmaze.com/shows/84\r\nContent-Length: 0\r\n\r\n",
                100, &f));
    CHECK_INT(f.status, 301);
    CHECK_STR(f.location, "https://api.tvmaze.com/shows/84");
    CHECK(!parse("SSH-2.0-OpenSSH_9.6\r\n", 100, &f)); /* not HTTP at all */
    CHECK(!parse("HTTP/1.1 200 OK\r\nContent-Length: 2", 100, &f)); /* headers cut short */
    fetched_free(&f);
}
