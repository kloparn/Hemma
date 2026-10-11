#include "../tests/test.h"
#include "http.h"

static int parse(const char *s, HttpReq *r) { return http_parse(s, strlen(s), 1 << 20, r); }

TEST(http_parses_get_with_query_and_headers) {
    HttpReq r;
    const char *s = "GET /book%20x?r=2&w=2026-10-05 HTTP/1.1\r\nHost: hemma.local\r\nCookie: a=1; sid=abc123; z=9\r\n"
                    "Connection: keep-alive, Upgrade\r\nX-Pad:   spaced  \r\n\r\n";
    CHECK_INT(parse(s, &r), 1);
    CHECK_STR(r.method, "GET");
    CHECK_STR(r.path, "/book x");
    CHECK_STR(r.query, "r=2&w=2026-10-05");
    CHECK_STR(r.target, "/book%20x?r=2&w=2026-10-05");
    CHECK_STR(http_header(&r, "host"), "hemma.local");
    CHECK_STR(http_header(&r, "x-pad"), "spaced");
    CHECK(http_header_has(&r, "Connection", "upgrade"));
    CHECK(!http_header_has(&r, "Connection", "close"));
    char sid[40];
    CHECK(http_cookie(&r, "sid", sid, sizeof sid));
    CHECK_STR(sid, "abc123");
    CHECK(!http_cookie(&r, "id", sid, sizeof sid));
    CHECK(r.keep_alive);
    CHECK_INT(r.total_len, strlen(s));
    http_req_free(&r);
}

TEST(http_waits_for_complete_requests) {
    HttpReq r;
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: x\r\n", &r), 0);
    const char *post = "POST /a/book.create HTTP/1.1\r\nContent-Length: 11\r\n\r\nres=2&x=åä";
    CHECK_INT(http_parse(post, strlen(post) - 3, 1 << 20, &r), 0); /* body incomplete */
    CHECK_INT(parse(post, &r), 1);
    CHECK_INT(r.body_len, 11);
    CHECK(memcmp(r.body, "res=2&x=åä", 11) == 0);
    http_req_free(&r);
}

TEST(http_pipelined_requests_parse_one_at_a_time) {
    const char *two = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.0\r\n\r\n";
    HttpReq r;
    CHECK_INT(parse(two, &r), 1);
    CHECK_STR(r.path, "/a");
    size_t used = r.total_len;
    http_req_free(&r);
    CHECK_INT(http_parse(two + used, strlen(two) - used, 1 << 20, &r), 1);
    CHECK_STR(r.path, "/b");
    CHECK(!r.keep_alive); /* HTTP/1.0 default */
    http_req_free(&r);
}

TEST(http_rejects_malformed_requests) {
    HttpReq r;
    const char *bad[] = {"get / HTTP/1.1\r\n\r\n",          "GET  / HTTP/1.1\r\n\r\n",
                         "GET nopath HTTP/1.1\r\n\r\n",     "GET / HTTP/1.1\r\nNoColon\r\n\r\n",
                         "GET / HTTP/1.1\r\n folded: x\r\n\r\n", "GET / HTTP/1.1\r\nBad Name: x\r\n\r\n",
                         "GET /%00 HTTP/1.1\r\n\r\n",       "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                         "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx"};
    for (int i = 0; i < countof(bad); i++) {
        int rc = parse(bad[i], &r);
        if (rc != -400) {
            test_fail(__FILE__, __LINE__, "expected 400 for #%d, got %d", i, rc);
            return;
        }
    }
    CHECK_INT(parse("GET / HTTP/2.0\r\n\r\n", &r), -505);
    CHECK_INT(parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &r), -501);
    char big[20000];
    memset(big, 'a', sizeof big);
    memcpy(big, "GET /", 5);
    CHECK_INT(http_parse(big, sizeof big, 1 << 20, &r), -431);
}

TEST(http_streams_bodies_too_big_to_buffer) {
    HttpReq r;
    const char *put = "PUT /up HTTP/1.1\r\nContent-Length: 5000000000\r\n\r\nfirst bytes";
    CHECK_INT(http_parse(put, strlen(put), 1 << 20, &r), 2); /* headers done, body follows on the wire */
    CHECK(r.streaming);
    CHECK(r.body == NULL);
    CHECK(r.content_length == 5000000000LL);
    CHECK_INT(r.head_len, strlen(put) - strlen("first bytes"));
    CHECK_INT(r.total_len, r.head_len);
    http_req_free(&r);
    CHECK_INT(http_parse("POST / HTTP/1.1\r\nContent-Length: 999\r\n\r\n", 40, 10, &r), 2);
    http_req_free(&r);
}

TEST(http_rejects_nul_bytes_and_crlf_floods) {
    /* a NUL anywhere in the header block once crashed the parser (strchr stopped short) */
    static const struct {
        const char *s;
        size_t n;
    } nul[] = {
        {"\0\r\n\r\n", 5},
        {"GET /\0 HTTP/1.1\r\n\r\n", 19},
        {"GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n", 29},
        {"\0GET / HTTP/1.1\r\n\r\n", 19},
        {"GET", 4}, /* "GET\0": incomplete but already hopeless */
    };
    for (int i = 0; i < (int)countof(nul); i++) {
        HttpReq r;
        int rc = http_parse(nul[i].s, nul[i].n, 1 << 20, &r);
        if (rc != -400) test_fail(__FILE__, __LINE__, "NUL case #%d: expected -400, got %d", i, rc);
    }
    char crlf[200];
    memset(crlf, '\n', sizeof crlf);
    HttpReq r;
    CHECK_INT(http_parse(crlf, 10, 1 << 20, &r), 0); /* a few stray newlines: wait */
    CHECK_INT(http_parse(crlf, sizeof crlf, 1 << 20, &r), -400); /* a flood: refuse, don't buffer forever */
}

static bool host_ok(const char *host, const char *extra) {
    HttpReq r;
    memset(&r, 0, sizeof r);
    r.headers[0] = (HttpHeader){"Host", host};
    r.nheaders = 1;
    return http_host_ok(&r, extra);
}

TEST(http_host_check_refuses_rebinding_names) {
    CHECK(host_ok("192.168.0.10:8080", NULL));
    CHECK(host_ok("127.0.0.1", NULL));
    CHECK(host_ok("[::1]:8080", NULL));
    CHECK(host_ok("localhost:8080", NULL));
    CHECK(host_ok("hemma", NULL));
    CHECK(host_ok("raspberrypi:8080", NULL));
    CHECK(host_ok("hemma.local", NULL));
    CHECK(host_ok("Hemma.LAN:80", NULL));
    CHECK(host_ok("pi.home.arpa", NULL));
    CHECK(host_ok("hemma.local.", NULL));
    CHECK(host_ok("nas.fritz.box", NULL));
    CHECK(!host_ok("evil.example", NULL)); /* a public name pointing at us: DNS rebinding */
    CHECK(!host_ok("hemma.local.evil.example", NULL));
    CHECK(!host_ok("bad host", NULL));
    CHECK(!host_ok("evil.example", "hemma.example.se, .home.example"));
    CHECK(host_ok("hemma.example.se:8080", "hemma.example.se, .home.example"));
    CHECK(host_ok("tv.home.example", "hemma.example.se, .home.example"));
    CHECK(!host_ok("home.example", ".home.example")); /* ".x" matches names under x only */
}

TEST(http_tolerates_bare_newlines_and_leading_crlf) {
    HttpReq r;
    CHECK_INT(parse("\r\nGET /x HTTP/1.1\nHost: a\n\n", &r), 1);
    CHECK_STR(r.path, "/x");
    CHECK_STR(http_header(&r, "Host"), "a");
    http_req_free(&r);
}

TEST(http_fields_parse_and_scrub) {
    Fields f = {0};
    const char *body = "res=2&note=ikea+trip%21&empty=&flag&dup=1&dup=2&bad=%FF%FE&nl=a%0D%0Ab";
    fields_parse(&f, body, strlen(body));
    CHECK_STR(fields_str(&f, "res"), "2");
    CHECK_STR(fields_str(&f, "note"), "ikea trip!");
    CHECK_STR(fields_str(&f, "empty"), "");
    CHECK(fields_get(&f, "flag") != NULL);
    CHECK_STR(fields_str(&f, "dup"), "1");
    CHECK_STR(fields_str(&f, "bad"), "??");
    CHECK_STR(fields_str(&f, "nl"), "a \nb");
    CHECK_INT(fields_int(&f, "res", 0), 2);
    CHECK_INT(fields_int(&f, "note", 7), 7);
    CHECK(fields_get(&f, "nope") == NULL);
    CHECK_STR(fields_str(&f, "nope"), "");
    fields_set(&f, "res", "3");
    CHECK_STR(fields_str(&f, "res"), "3");
    fields_free(&f);
    CHECK_INT(f.n, 0);
}

TEST(http_response_formatting) {
    Buf b = {0};
    http_reply(&b, 404, "text/plain", "nope", 4, "Cache-Control: no-store\r\n", false);
    const char *s = buf_cstr(&b);
    CHECK(str_starts(s, "HTTP/1.1 404 Not Found\r\n"));
    CHECK_HAS(s, "Content-Length: 4\r\n");
    CHECK_HAS(s, "Connection: close\r\n");
    CHECK_HAS(s, "Cache-Control: no-store\r\n\r\nnope");
    buf_free(&b);
}

TEST(http_range_header) {
    int64_t a, z;
    CHECK_INT(http_range("bytes=0-", 1000, &a, &z), 1);
    CHECK(a == 0 && z == 999);
    CHECK_INT(http_range("bytes=100-199", 1000, &a, &z), 1);
    CHECK(a == 100 && z == 199);
    CHECK_INT(http_range("bytes=900-5000", 1000, &a, &z), 1);
    CHECK(a == 900 && z == 999);
    CHECK_INT(http_range("bytes=-100", 1000, &a, &z), 1);
    CHECK(a == 900 && z == 999);
    CHECK_INT(http_range("bytes=1000-", 1000, &a, &z), -1);
    CHECK_INT(http_range("bytes=5-1", 1000, &a, &z), 0);
    CHECK_INT(http_range("bytes=0-1,5-6", 1000, &a, &z), 0);
    CHECK_INT(http_range("items=0-1", 1000, &a, &z), 0);
    CHECK_INT(http_range(NULL, 1000, &a, &z), 0);
}

TEST(http_mime_types) {
    CHECK_STR(http_mime("a/b/Movie.MKV"), "video/x-matroska");
    CHECK_STR(http_mime("x.vtt"), "text/vtt; charset=utf-8");
    CHECK_STR(http_mime("noext"), "application/octet-stream");
}

TEST(http_status_texts_cover_what_hemma_sends) {
    CHECK_STR(http_status_text(401), "Unauthorized"); /* review: the API's 401 went out as "401 Unknown" */
    CHECK_STR(http_status_text(403), "Forbidden");
    CHECK_STR(http_status_text(503), "Service Unavailable");
    CHECK_STR(http_status_text(799), "Unknown");
}
