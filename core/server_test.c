/* End-to-end: the real event loop on a loopback port, driven by blocking
 * test clients from this thread. */
#include "../tests/test.h"
#include "server.h"
#include "ws.h"

static volatile int srv_done;
static volatile int ws_closed_count;

static void t_http(Conn *c, HttpReq *r) {
    if (str_eq(r->path, "/ws")) {
        conn_ws_upgrade(c, r);
    } else if (str_eq(r->path, "/hello")) {
        conn_reply(c, 200, "text/plain", "hello", 5, NULL);
    } else if (str_eq(r->path, "/echo")) {
        conn_reply(c, 200, "text/plain", r->body, r->body_len, NULL);
    } else if (str_eq(r->path, "/big") || str_eq(r->path, "/bigknown")) { /* more than the sockets' buffers take */
        static char big[16 << 20];
        if (str_eq(r->path, "/bigknown")) c->known = true; /* (as the app marks a signed-in request) */
        conn_reply(c, 200, "application/octet-stream", big, sizeof big, NULL);
    } else {
        conn_reply(c, 404, "text/plain", "nf", 2, NULL);
    }
}

static void t_ws_message(Conn *c, const char *m, size_t n) {
    Buf b = {0};
    buf_puts(&b, "echo:");
    buf_put(&b, m, n);
    conn_ws_send(c, b.p, b.len);
    buf_free(&b);
}

static void t_ws_closed(Conn *c) { ws_closed_count++; }

static const ServerHooks hooks = {t_http, t_ws_message, t_ws_closed, NULL, NULL};

static void server_thread(void *arg) {
    server_run(&hooks);
    server_shutdown();
    srv_done = 1;
}

static sock_t dial(int port) {
    sock_t s = tcp_connect_nb(0x7F000001, port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) return SOCK_BAD;
    return s;
}

static void send_all(sock_t s, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        long k = sock_send(s, c, n);
        if (k > 0) c += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
}

/* Read until `needle` shows up (or the peer closes / 3 s pass). Returns bytes read. */
static size_t recv_until(sock_t s, Buf *b, const char *needle, size_t needle_len) {
    int64_t end = now_ms() + 3000;
    while (now_ms() < end) {
        if (needle && b->len >= needle_len) {
            for (size_t i = 0; i + needle_len <= b->len; i++)
                if (memcmp(b->p + i, needle, needle_len) == 0) return b->len;
        }
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[4096];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(b, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    return b->len;
}

static size_t client_frame(uint8_t *out, int op, bool fin, const char *p, size_t n) {
    size_t h = 0;
    out[h++] = (uint8_t)((fin ? 0x80 : 0) | op);
    out[h++] = (uint8_t)(0x80 | n); /* n < 126 here */
    uint8_t mask[4] = {1, 2, 3, 4};
    memcpy(out + h, mask, 4);
    h += 4;
    for (size_t i = 0; i < n; i++) out[h + i] = (uint8_t)(p[i] ^ mask[i & 3]);
    return h + n;
}

TEST(server_end_to_end) {
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    CHECK(port > 0);
    srv_done = 0;
    CHECK(thread_spawn(server_thread, NULL));

    /* keep-alive: two requests on one connection, the second one pipelined with a third */
    sock_t s = dial(port);
    CHECK(s != SOCK_BAD);
    Buf b = {0};
    const char *get = "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n";
    send_all(s, get, strlen(get));
    recv_until(s, &b, "hello", 5);
    CHECK_HAS(buf_cstr(&b), "HTTP/1.1 200 OK");
    CHECK_HAS(buf_cstr(&b), "Connection: keep-alive");
    buf_reset(&b);
    const char *two = "GET /hello HTTP/1.1\r\n\r\nPOST /echo HTTP/1.1\r\nContent-Length: 6\r\n\r\nåäö!";
    /* deliver in dribbles to exercise incremental parsing */
    for (size_t i = 0; i < strlen(two); i += 7) {
        send_all(s, two + i, MIN(7, strlen(two) - i));
        plat_sleep_ms(2);
    }
    recv_until(s, &b, "åäö!", strlen("åäö!"));
    CHECK_HAS(buf_cstr(&b), "hello");
    CHECK_HAS(buf_cstr(&b), "Content-Length: 6\r\n");
    sock_close(s);

    /* malformed request: 400 and the server closes */
    s = dial(port);
    buf_reset(&b);
    send_all(s, "BROKEN\r\n\r\n", 10);
    recv_until(s, &b, NULL, 0);
    CHECK_HAS(buf_cstr(&b), "400 Bad Request");
    CHECK_HAS(buf_cstr(&b), "Connection: close");
    sock_close(s);

    /* WebSocket: handshake, fragmented message + ping, then close */
    s = dial(port);
    buf_reset(&b);
    const char *up = "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    send_all(s, up, strlen(up));
    recv_until(s, &b, "\r\n\r\n", 4);
    CHECK_HAS(buf_cstr(&b), "101 Switching Protocols");
    CHECK_HAS(buf_cstr(&b), "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    buf_reset(&b);
    uint8_t fr[256];
    size_t n = client_frame(fr, WS_TEXT, false, "hello ", 6);
    n += client_frame(fr + n, WS_PING, true, "pp", 2); /* control frames may interleave */
    n += client_frame(fr + n, WS_CONT, true, "world", 5);
    send_all(s, fr, n);
    recv_until(s, &b, "echo:hello world", 16);
    CHECK_HAS(buf_cstr(&b), "echo:hello world");
    CHECK((uint8_t)b.p[0] == 0x8A); /* the pong comes first */
    buf_reset(&b);
    n = client_frame(fr, WS_CLOSE, true, "\x03\xe8", 2);
    send_all(s, fr, n);
    recv_until(s, &b, "\x88\x02\x03\xe8", 4);
    CHECK((uint8_t)b.p[0] == 0x88);
    sock_close(s);

    /* HEAD: the headers of the GET, no body */
    s = dial(port);
    buf_reset(&b);
    const char *head = "HEAD /hello HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    send_all(s, head, strlen(head));
    recv_until(s, &b, NULL, 0);
    CHECK_HAS(buf_cstr(&b), "Content-Length: 5\r\n");
    CHECK(!strstr(buf_cstr(&b), "hello"));
    sock_close(s);

    /* a client that half-closes after its request still gets the answer */
    s = dial(port);
    buf_reset(&b);
    send_all(s, get, strlen(get));
    sock_shutdown_write(s);
    recv_until(s, &b, "hello", 5);
    CHECK_HAS(buf_cstr(&b), "hello");
    sock_close(s);

    /* two thousand pipelined requests from a client that isn't reading yet: the server
       pauses at its output limit (no unbounded buffering) and resumes as we read */
    s = dial(port);
    buf_reset(&b);
    Buf burst = {0};
    for (int i = 0; i < 2000; i++) buf_puts(&burst, get); /* ~64 KB in, ~340 KB out */
    send_all(s, burst.p, burst.len);
    int hellos = 0;
    for (int64_t end = now_ms() + 10000; hellos < 2000 && now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[16384];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k <= 0) break;
        buf_put(&b, tmp, (size_t)k);
        hellos = 0; /* count complete bodies so far */
        for (const char *p = b.p; (p = strstr(p, "\r\n\r\nhello")); p += 9) hellos++;
    }
    CHECK_INT(hellos, 2000);
    buf_free(&burst);
    sock_close(s);

    /* someone typing https:// into a phone: the TLS hello is hung up on, not answered */
    s = dial(port);
    buf_reset(&b);
    static const uint8_t hello_tls[] = {0x16, 0x03, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0xfc, 0x03, 0x03};
    send_all(s, hello_tls, sizeof hello_tls);
    recv_until(s, &b, NULL, 0);
    CHECK_INT(b.len, 0);
    sock_close(s);

    /* a bad upgrade gets a 400 */
    s = dial(port);
    buf_reset(&b);
    const char *noup = "GET /ws HTTP/1.1\r\nHost: t\r\n\r\n";
    send_all(s, noup, strlen(noup));
    recv_until(s, &b, NULL, 0);
    CHECK_HAS(buf_cstr(&b), "400");
    sock_close(s);

    for (int i = 0; i < 300 && ws_closed_count < 1; i++) plat_sleep_ms(10);
    CHECK_INT(ws_closed_count, 1);
    server_stop();
    for (int i = 0; i < 300 && !srv_done; i++) plat_sleep_ms(10);
    CHECK(srv_done);
    buf_free(&b);
}

/* Sends what it can; false once the peer is gone (a refused upload gets hung up on). */
static bool send_some(sock_t s, const char *p, size_t n) {
    for (int64_t end = now_ms() + 5000; n && now_ms() < end;) {
        long k = sock_send(s, p, n);
        if (k > 0) p += k, n -= (size_t)k;
        else if (k < 0 && !sock_again(sock_errno())) return false;
        else plat_sleep_ms(1);
    }
    return n == 0;
}

static void recv_at_least(sock_t s, Buf *b, size_t want) {
    for (int64_t end = now_ms() + 5000; b->len < want && now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[65536];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(b, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
}

TEST(server_bodies_share_a_memory_budget) {
    /* review: 1 MB bodies are buffered before anyone signs in; 400 connections of them took
       the server from 13 MB to 420 MB. Past the shared budget (2 MB here) a request gets 503. */
    server_set_buffer_budget(2u << 20);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    srv_done = 0;
    CHECK(thread_spawn(server_thread, NULL));
    static const char head[] = "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 1000000\r\n\r\n";
    char *body = xmalloc(1000000);
    memset(body, 'x', 1000000);
    sock_t s[3];
    for (int i = 0; i < 3; i++) { /* three bodies 80% there: ~1 MB of buffer each */
        s[i] = dial(port);
        CHECK(s[i] != SOCK_BAD);
        send_some(s[i], head, sizeof head - 1);
        send_some(s[i], body, 800000);
        plat_sleep_ms(100);
    }
    Buf b = {0};
    recv_until(s[2], &b, "\r\n\r\n", 4);
    CHECK_HAS(buf_cstr(&b), "HTTP/1.1 503 Service Unavailable");
    CHECK_HAS(buf_cstr(&b), "Retry-After: 5");
    CHECK_HAS(buf_cstr(&b), "Connection: close");
    sock_close(s[2]);
    /* the others carry on: the first one finishes and gets its echo */
    buf_reset(&b);
    CHECK(send_some(s[0], body + 800000, 200000));
    recv_at_least(s[0], &b, 1000000);
    CHECK_HAS(buf_cstr(&b), "HTTP/1.1 200 OK");
    CHECK_HAS(buf_cstr(&b), "Content-Length: 1000000\r\n");
    sock_close(s[0]);
    sock_close(s[1]);
    plat_sleep_ms(100);
    /* room again once those are done: a whole body goes through */
    sock_t d = dial(port);
    buf_reset(&b);
    send_some(d, head, sizeof head - 1);
    CHECK(send_some(d, body, 1000000));
    recv_at_least(d, &b, 1000000);
    CHECK_HAS(buf_cstr(&b), "HTTP/1.1 200 OK");
    sock_close(d);
    server_stop();
    for (int i = 0; i < 300 && !srv_done; i++) plat_sleep_ms(10);
    CHECK(srv_done);
    server_set_buffer_budget(0);
    free(body);
    buf_free(&b);
}

/* Is the server's end of s closed (it reads EOF or an error within ~1 s)? */
static bool closed_by_server(sock_t s) {
    for (int64_t end = now_ms() + 1000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[512];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k == 0 || (k < 0 && !sock_again(sock_errno()))) return true;
    }
    return false;
}

TEST(server_newcomers_take_the_place_of_idle_connections) {
    /* issue #5: one host with a dozen addresses held every slot with a request a minute, and
       nobody else could connect. Full, a newcomer takes an idle keep-alive connection's place
       (the longest idle first); screens (WebSockets) are never pushed out. */
    server_set_max_conns(4);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    srv_done = 0;
    CHECK(thread_spawn(server_thread, NULL));
    const char *get = "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n";
    const char *up = "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    Buf b = {0};
    sock_t ws = dial(port); /* a screen */
    send_all(ws, up, strlen(up));
    recv_until(ws, &b, "\r\n\r\n", 4);
    CHECK_HAS(buf_cstr(&b), "101 Switching Protocols");
    sock_t s[5];
    for (int i = 0; i < 3; i++) { /* three idle keep-alive connections: the server is full */
        s[i] = dial(port);
        buf_reset(&b);
        send_all(s[i], get, strlen(get));
        recv_until(s[i], &b, "hello", 5);
        CHECK_HAS(buf_cstr(&b), "200 OK");
        plat_sleep_ms(30);
    }
    s[3] = dial(port); /* a newcomer: served, in the place of the longest idle one */
    buf_reset(&b);
    send_all(s[3], get, strlen(get));
    recv_until(s[3], &b, "hello", 5);
    CHECK_HAS(buf_cstr(&b), "200 OK");
    CHECK(closed_by_server(s[0]));
    s[4] = dial(port); /* and another: the next longest idle one goes */
    buf_reset(&b);
    send_all(s[4], get, strlen(get));
    recv_until(s[4], &b, "hello", 5);
    CHECK_HAS(buf_cstr(&b), "200 OK");
    CHECK(closed_by_server(s[1]));
    /* the screen is still there */
    uint8_t fr[64];
    size_t n = client_frame(fr, WS_TEXT, true, "hi", 2);
    send_all(ws, fr, n);
    buf_reset(&b);
    recv_until(ws, &b, "echo:hi", 7);
    CHECK_HAS(buf_cstr(&b), "echo:hi");
    for (int i = 0; i < 5; i++) sock_close(s[i]);
    sock_close(ws);
    server_stop();
    for (int i = 0; i < 300 && !srv_done; i++) plat_sleep_ms(10);
    CHECK(srv_done);
    server_set_max_conns(0);
    buf_free(&b);
}

TEST(server_slow_readers_make_room_for_newcomers) {
    /* issue #32: connections that ask for something and then read it a few bytes now and then
       were never pushed out (they're owed a reply, and every byte restarts the stall clock), so
       a dozen addresses could hold every slot. Full, a newcomer takes a slow reader's place. */
    server_set_max_conns(4);
    server_set_slow_after(300);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    srv_done = 0;
    CHECK(thread_spawn(server_thread, NULL));
    /* three big replies in a row: more than the sockets' buffers can hold, so output stays queued */
    const char *big = "GET /big HTTP/1.1\r\nHost: t\r\n\r\nGET /big HTTP/1.1\r\nHost: t\r\n\r\n"
                      "GET /big HTTP/1.1\r\nHost: t\r\n\r\n",
               *get = "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n";
    sock_t s[4];
    for (int i = 0; i < 4; i++) { /* every slot: replies it never reads */
        s[i] = dial(port);
        CHECK(s[i] != SOCK_BAD);
        send_all(s[i], big, strlen(big));
    }
    plat_sleep_ms(800); /* past slow_after: none of them has moved a chunk since the start */
    sock_t n = dial(port);
    Buf b = {0};
    send_all(n, get, strlen(get));
    recv_until(n, &b, "hello", 5);
    CHECK_HAS(buf_cstr(&b), "200 OK");
    bool one_went = false;
    for (int i = 0; i < 4 && !one_went; i++) one_went = closed_by_server(s[i]);
    CHECK(one_went);
    sock_close(n);
    for (int i = 0; i < 4; i++) sock_close(s[i]);
    server_stop();
    for (int i = 0; i < 300 && !srv_done; i++) plat_sleep_ms(10);
    CHECK(srv_done);
    server_set_max_conns(0);
    server_set_slow_after(0);
    buf_free(&b);
}

/* Issue #91: reading just faster than the slow-reader rule (64 KB per 10 s) kept a slot for good -
   13 addresses at ~21 Mbit/s held all 400. Full, with nobody idle or slow, a newcomer takes the
   place of the longest-held connection of the address holding the most strangers' ones. */
static void busy_slots(const char *path, bool household) {
    server_set_max_conns(4); /* (slow_after stays 10 s: these replies are owed, not slow) */
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    srv_done = 0;
    CHECK(thread_spawn(server_thread, NULL));
    char one[80], req[300]; /* three big replies: more than the sockets' buffers hold - output stays queued */
    snprintf(one, sizeof one, "GET %s HTTP/1.1\r\nHost: t\r\n\r\n", path);
    snprintf(req, sizeof req, "%s%s%s", one, one, one);
    sock_t s[4];
    for (int i = 0; i < 4; i++) {
        s[i] = dial(port);
        CHECK(s[i] != SOCK_BAD);
        send_all(s[i], req, strlen(req));
    }
    plat_sleep_ms(300); /* (all four being answered) */
    const char *get = "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n";
    sock_t n = dial(port);
    Buf b = {0};
    send_all(n, get, strlen(get));
    if (household) { /* (a second is plenty: room is made at once, or not at all) */
        for (int64_t end = now_ms() + 1000; now_ms() < end && !strstr(buf_cstr(&b), "hello");) {
            pollfd_t pf = {.fd = n, .events = POLLIN};
            if (plat_poll(&pf, 1, 50) != 1) continue;
            char tmp[512];
            long k = sock_recv(n, tmp, sizeof tmp);
            if (k <= 0) break;
            buf_put(&b, tmp, (size_t)k);
        }
    } else {
        recv_until(n, &b, "hello", 5);
    }
    if (household) { /* the household's connections stay while they're busy: the newcomer waits */
        CHECK(!strstr(buf_cstr(&b), "200 OK"));
    } else {
        int went = 0; /* (reading them drains them - idle then: only once the newcomer is in) */
        for (int i = 0; i < 4; i++) went += closed_by_server(s[i]);
        CHECK_HAS(buf_cstr(&b), "200 OK");
        CHECK_INT(went, 1); /* the longest-held one */
    }
    sock_close(n);
    for (int i = 0; i < 4; i++) sock_close(s[i]);
    server_stop();
    for (int i = 0; i < 300 && !srv_done; i++) plat_sleep_ms(10);
    CHECK(srv_done);
    server_set_max_conns(0);
    buf_free(&b);
}

TEST(server_busy_strangers_make_room_for_newcomers) { busy_slots("/big", false); }

TEST(server_never_pushes_out_the_households_busy_connections) { busy_slots("/bigknown", true); }
