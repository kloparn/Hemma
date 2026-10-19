/* End-to-end over real sockets: the whole app on a loopback port, driven like
 * a browser would — sign in, pages, the WebSocket wire, the JSON API, sign out.
 * (This layer once hid a bug where response headers picked up heap garbage.) */
#include "../tests/harness.h"
#include "assets.h"
#include "auth.h"
#include "codec.h"
#include "server.h"
#include "ws.h"

static volatile int g_done;
static void serve(void *arg) {
    (void)arg;
    app_serve();
    g_done = 1;
}

static sock_t dial(int port) {
    sock_t s = tcp_connect_nb(0x7F000001, port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) return SOCK_BAD;
    return s;
}

static void send_all(sock_t s, const char *p, size_t n) {
    while (n) {
        long k = sock_send(s, p, n);
        if (k > 0) p += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
}

/* One request on a fresh connection; returns the whole response (status line .. body). */
static void request(int port, const char *raw, Buf *out) {
    buf_reset(out);
    sock_t s = dial(port);
    if (s == SOCK_BAD) return;
    send_all(s, raw, strlen(raw));
    int64_t end = now_ms() + 4000;
    while (now_ms() < end) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[8192];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(out, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
        /* stop once Content-Length bytes of body arrived */
        const char *hdr_end = strstr(buf_cstr(out), "\r\n\r\n");
        const char *cl = strstr(buf_cstr(out), "Content-Length: ");
        if (hdr_end && cl && cl < hdr_end) {
            long long n = strtoll(cl + 16, NULL, 10);
            if ((long long)(out->len - (size_t)(hdr_end + 4 - out->p)) >= n) break;
        }
    }
    sock_close(s);
}

static const char *body_of(const Buf *resp) {
    const char *e = resp->p ? strstr(resp->p, "\r\n\r\n") : NULL;
    return e ? e + 4 : "";
}

/* The cookie value from a Set-Cookie header ("" if none). */
static void cookie_of(const Buf *resp, char *out, size_t cap) {
    out[0] = '\0';
    const char *c = resp->p ? strstr(resp->p, "Set-Cookie: hemma=") : NULL;
    if (!c) return;
    c += 18;
    size_t n = strcspn(c, ";\r\n");
    str_ncopy(out, cap, c, n);
}

static size_t client_frame(uint8_t *out, const char *p, size_t n) {
    size_t h = 0;
    out[h++] = 0x81;
    if (n < 126) out[h++] = (uint8_t)(0x80 | n);
    else {
        out[h++] = 0x80 | 126;
        out[h++] = (uint8_t)(n >> 8);
        out[h++] = (uint8_t)n;
    }
    uint8_t mask[4] = {9, 8, 7, 6};
    memcpy(out + h, mask, 4);
    h += 4;
    for (size_t i = 0; i < n; i++) out[h + i] = (uint8_t)(p[i] ^ mask[i & 3]);
    return h + n;
}

TEST(app_http_sign_in_pages_wire_and_sign_out) {
    if (!module_find("book")) return; /* uses the booking module for an action */
    th_boot();
    auth_configure("e2e-salt", 1000);
    int adam = th_person("Adam");
    Person p = *person_get(adam);
    str_copy(p.username, sizeof p.username, "adam");
    auth_hash("correct horse", p.pass, sizeof p.pass);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);

    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    Buf r = {0};
    char req[1024], cookie[64];

    /* signed out: every page is the sign-in page, and it's well-formed HTTP */
    request(port, "GET /book HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 200 OK");
    CHECK(str_starts(body_of(&r), "<!doctype html>")); /* nothing between the headers and the page */
    CHECK_HAS(body_of(&r), "action=\"/login\"");
    request(port, "GET /media/file/0123456789abcdef HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK(!strstr(buf_cstr(&r), "206 Partial")); /* module endpoints are closed to strangers */

    /* wrong password -> back to the form with an error */
    const char *bad = "username=adam&password=nope&next=%2Fbook";
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-www-form-urlencoded\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             strlen(bad), bad);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "303 See Other");
    CHECK_HAS(buf_cstr(&r), "Location: /login?e=1&next=%2Fbook"); /* (no typed name in the address: issue #100) */
    cookie_of(&r, cookie, sizeof cookie);
    CHECK_STR(cookie, "");

    /* a cross-site form post is refused */
    const char *good = "username=adam&password=correct+horse&next=%2Fbook&remember=1";
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\nOrigin: http://evil.example\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s",
             strlen(good), good);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "403");

    /* right password -> cookie + back to where we were */
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0) Chrome/130\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             strlen(good), good);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "Location: /book");
    CHECK_HAS(buf_cstr(&r), "HttpOnly");
    CHECK_HAS(buf_cstr(&r), "Max-Age=2592000"); /* an admin: a month (members: a year) */
    cookie_of(&r, cookie, sizeof cookie);
    CHECK_INT(strlen(cookie), 32);

    /* signed in: real pages */
    snprintf(req, sizeof req, "GET /book HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             cookie);
    request(port, req, &r);
    CHECK(str_starts(body_of(&r), "<!doctype html>"));
    CHECK_HAS(body_of(&r), "data-mod=\"book\"");
    CHECK_HAS(body_of(&r), "</html>");

    /* the JSON API with the same cookie */
    const char *json = "{\"name\":\"Car\"}";
    snprintf(req, sizeof req,
             "POST /api/book.preset HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nContent-Type: application/json\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             cookie, strlen(json), json);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "\"ok\":true");
    snprintf(req, sizeof req, "GET /api/person HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             cookie);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "\"username\":\"adam\"");
    CHECK(!strstr(body_of(&r), "pbkdf2")); /* password hashes never leave the server */

    /* the wire: upgrade with the cookie, say hello, get the live page */
    sock_t ws = dial(port);
    CHECK(ws != SOCK_BAD);
    snprintf(req, sizeof req,
             "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nCookie: hemma=%s\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
             cookie);
    send_all(ws, req, strlen(req));
    uint8_t frame[512];
    const char *hello = "{\"t\":\"hello\",\"url\":\"/book\"}";
    size_t fn = client_frame(frame, hello, strlen(hello));
    send_all(ws, (const char *)frame, fn);
    buf_reset(&r);
    for (int64_t end = now_ms() + 3000; now_ms() < end && !strstr(buf_cstr(&r), "Car");) {
        pollfd_t pf = {.fd = ws, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[16384];
        long k = sock_recv(ws, tmp, sizeof tmp);
        if (k > 0) buf_put(&r, tmp, (size_t)k);
        else if (k == 0) break;
    }
    CHECK_HAS(buf_cstr(&r), "101 Switching Protocols");
    CHECK_HAS(buf_cstr(&r), "[\"morph\",");
    CHECK_HAS(buf_cstr(&r), "Car"); /* the preset added over the API is on the live page */
    sock_close(ws);

    /* sign out: the session ends and the cookie is cleared */
    snprintf(req, sizeof req,
             "POST /logout HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             cookie);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "Max-Age=0");
    snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", cookie);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "action=\"/login\"");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* A Samsung TV's video player (2026) fetches the <video>'s file without the page's cookie: the
   address carries a key instead (ctx_url_key) - for that one path and session, endpoints only. */
TEST(app_http_address_keys_open_one_endpoint_for_one_session) {
    if (!module_find("media")) return; /* the endpoint that answers */
    th_boot();
    auth_configure("e2e-salt", 1000);
    int adam = th_person("Adam");
    Person p = *person_get(adam);
    str_copy(p.username, sizeof p.username, "adam");
    auth_hash("correct horse", p.pass, sizeof p.pass);
    p.role = ROLE_ADMIN; /* (no admin: strangers would get the welcome page, not the sign-in page) */
    tbl_put(&T_person, &p);

    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    Buf r = {0};
    char req[1024], cookie[64];
    const char *good = "username=adam&password=correct+horse";
    snprintf(req, sizeof req, "POST /login HTTP/1.1\r\nHost: t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
             strlen(good), good);
    request(port, req, &r);
    cookie_of(&r, cookie, sizeof cookie);
    CHECK_INT(strlen(cookie), 32);

    /* the key a page of this session puts on a video's address */
    char token[65];
    sha256_hex(cookie, strlen(cookie), token);
    Session *s = NULL;
    for (int i = 0; i < tbl_count(&T_session); i++)
        if (str_eq(((Session *)tbl_at(&T_session, i))->token, token)) s = tbl_at(&T_session, i);
    CHECK(s != NULL);
    g_fake_now = now_sec(); /* (one clock for the key and the server: no hour turning in between) */
    Ctx kc = {.sess = s, .ip = 0x7F000001}; /* made for a screen at this address */
    char key[CTX_KEY_LEN], page_key[CTX_KEY_LEN], elsewhere[CTX_KEY_LEN];
    ctx_url_key(&kc, "/media/file/0123456789abcdef", key);
    ctx_url_key(&kc, "/people", page_key);
    CHECK(key[0] && !str_eq(key, page_key));
    Ctx oc = {.sess = s, .ip = 0x0A000005};
    ctx_url_key(&oc, "/media/file/0123456789abcdef", elsewhere);

    /* no cookie, the key: the endpoint answers as Adam (an id not in the library: its own 404) */
    snprintf(req, sizeof req, "GET /media/file/0123456789abcdef?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             key);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 404");
    CHECK_HAS(body_of(&r), "not in the library");
    snprintf(req, sizeof req, "HEAD /media/file/0123456789abcdef?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             key);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 404");

    /* ...and nothing else: no key, another file, a changed key, a page, a form post */
    request(port, "GET /media/file/0123456789abcdef HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK_HAS(body_of(&r), "action=\"/login\"");
    snprintf(req, sizeof req, "GET /media/file/1111111111111111?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             key);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "action=\"/login\"");
    char bent[CTX_KEY_LEN];
    str_copy(bent, sizeof bent, key);
    if (bent[0]) bent[strlen(bent) - 1] = bent[strlen(bent) - 1] == '0' ? '1' : '0';
    snprintf(req, sizeof req, "GET /media/file/0123456789abcdef?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             bent);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "action=\"/login\"");
    snprintf(req, sizeof req, "GET /people?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", page_key);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "action=\"/login\""); /* a key never opens a page */
    snprintf(req, sizeof req,
             "POST /media/file/0123456789abcdef?k=%s HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             key);
    request(port, req, &r);
    CHECK(!strstr(body_of(&r), "not in the library"));

    /* issue #78: only from the address of the screen it was made for, and only for two days */
    snprintf(req, sizeof req, "GET /media/file/0123456789abcdef?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             elsewhere);
    request(port, req, &r); /* (it comes from 127.0.0.1) */
    CHECK_HAS(body_of(&r), "action=\"/login\"");
    snprintf(req, sizeof req, "GET /media/file/0123456789abcdef?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             key);
    g_fake_now += 47 * 3600; /* paused overnight, and the next evening: still fine */
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "not in the library");
    g_fake_now += 3 * 3600; /* past its 48 hours (to the hour) */
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "action=\"/login\"");
    g_fake_now -= 50 * 3600;

    /* signed out: the key dies with the session */
    snprintf(req, sizeof req,
             "POST /logout HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             cookie);
    request(port, req, &r);
    snprintf(req, sizeof req, "GET /media/file/0123456789abcdef?k=%s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             key);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "action=\"/login\"");
    g_fake_now = 0;

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* ---- the wire: who re-renders what ---- */

static int account(const char *name, const char *user, const char *pw, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash(pw, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

static void sign_in(int port, const char *form, char *cookie, size_t cap) {
    char req[512];
    Buf r = {0};
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(form),
             form);
    request(port, req, &r);
    cookie_of(&r, cookie, cap);
    buf_free(&r);
}

/* Reads what arrives until `needle` shows up (true) or `ms` pass; needle NULL = read for ms. */
static bool ws_wait(sock_t s, Buf *b, const char *needle, int ms) {
    for (int64_t end = now_ms() + ms; now_ms() < end;) {
        if (needle && b->len && strstr(buf_cstr(b), needle)) return true;
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 20) != 1) continue;
        char tmp[16384];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) {
            for (long i = 0; i < k; i++) if (!tmp[i]) tmp[i] = ' '; /* frame headers may hold NULs */
            buf_put(b, tmp, (size_t)k);
        } else if (k == 0 || !sock_again(sock_errno())) break;
    }
    return needle && strstr(buf_cstr(b), needle);
}

static sock_t ws_open(int port, const char *cookie) {
    sock_t s = dial(port);
    if (s == SOCK_BAD) return s;
    char req[512];
    snprintf(req, sizeof req,
             "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n%s%s%s"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
             cookie[0] ? "Cookie: hemma=" : "", cookie, cookie[0] ? "\r\n" : "");
    send_all(s, req, strlen(req));
    Buf b = {0};
    ws_wait(s, &b, "\r\n\r\n", 2000);
    buf_free(&b);
    return s;
}

static void ws_say(sock_t s, const char *json) {
    uint8_t f[1024];
    size_t n = client_frame(f, json, strlen(json));
    send_all(s, (const char *)f, n);
}

TEST(app_wire_renders_only_the_screens_that_changed) {
    /* review: hello/nav from any socket - signed out too - re-rendered every screen in the
       house, and every click morphed every other screen (the "Here now" tooltip) */
    if (!module_find("people")) return;
    th_boot();
    auth_configure("e2e-salt", 1000);
    g_fake_now = dt_make(2026, 10, 11, 12, 0); /* a minute that never ends: no minute tick re-renders */
    account("Adam", "adam", "correct horse", ROLE_ADMIN);
    account("Eva", "eva", "eva password", ROLE_MEMBER);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], eva[64];
    sign_in(port, "username=adam&password=correct+horse", adam, sizeof adam);
    sign_in(port, "username=eva&password=eva+password", eva, sizeof eva);
    CHECK_INT(strlen(adam), 32);
    CHECK_INT(strlen(eva), 32);

    /* adam's screen on the household page */
    Buf a = {0}, b = {0}, e = {0};
    sock_t sa = ws_open(port, adam);
    ws_say(sa, "{\"t\":\"hello\",\"url\":\"/people\"}");
    CHECK(ws_wait(sa, &a, "[\"morph\",", 3000));
    plat_sleep_ms(150);

    /* a stranger's socket: hello, navs, a refused action, close - only its own screen renders */
    long before = app_renders();
    sock_t sb = ws_open(port, "");
    ws_say(sb, "{\"t\":\"hello\",\"url\":\"/\"}");
    CHECK(ws_wait(sb, &b, "action=\\\"/login\\\"", 3000));
    char msg[200];
    for (int i = 0; i < 4; i++) {
        snprintf(msg, sizeof msg, "{\"t\":\"nav\",\"url\":\"/x%d\"}", i);
        ws_say(sb, msg);
        plat_sleep_ms(20);
    }
    ws_say(sb, "{\"t\":\"act\",\"a\":\"household.save\",\"id\":1,\"url\":\"/x3\",\"f\":[[\"name\",\"Pwned\"]]}");
    CHECK(ws_wait(sb, &b, "Sign in first", 3000));
    plat_sleep_ms(150);
    long stranger = app_renders() - before;
    CHECK(stranger >= 1 && stranger <= 6); /* its own: hello, navs, the refusal - never adam's too */
    CHECK_STR(app_name(), "Hemma");
    before = app_renders();
    sock_close(sb);
    plat_sleep_ms(300);
    CHECK_INT(app_renders() - before, 0); /* a stranger leaving changes nothing for anyone */

    /* presence still reaches other screens: eva opening and closing one */
    buf_reset(&a);
    sock_t se = ws_open(port, eva);
    ws_say(se, "{\"t\":\"hello\",\"url\":\"/more\"}");
    CHECK(ws_wait(se, &e, "[\"morph\",", 3000));
    CHECK(ws_wait(sa, &a, "@eva \xC2\xB7 online on 1 screen", 3000)); /* adam's household page: eva online */
    /* eva clicking around re-renders her screen and the pages that say where people are -
       not adam's other pages (none here: the household page is one of them) */
    buf_reset(&a);
    before = app_renders();
    ws_say(se, "{\"t\":\"nav\",\"url\":\"/people\"}");
    CHECK(ws_wait(se, &e, "[\"url\",\"/people\"", 3000));
    plat_sleep_ms(150);
    CHECK(app_renders() - before <= 2);
    buf_reset(&a);
    sock_close(se);
    CHECK(ws_wait(sa, &a, "[\"morph\",", 3000)); /* eva's screen closed: adam's page updates */

    /* a nav to where the screen already is: no render; a tap (push) just scrolls to the top */
    buf_reset(&a);
    plat_sleep_ms(150);
    before = app_renders();
    ws_say(sa, "{\"t\":\"nav\",\"url\":\"/people\",\"pop\":1}");
    ws_say(sa, "{\"t\":\"nav\",\"url\":\"/people\"}");
    CHECK(ws_wait(sa, &a, "[[\"url\",\"/people\",", 3000));
    CHECK_HAS(buf_cstr(&a), ",1]]");
    CHECK(!strstr(buf_cstr(&a), "[\"morph\","));
    CHECK_INT(app_renders() - before, 0);

    /* a flood of navs: a burst, then the screen catches up on the tick - with the last URL */
    buf_reset(&a);
    before = app_renders();
    int64_t t0 = now_ms();
    for (int i = 0; i < 60; i++) {
        snprintf(msg, sizeof msg, "{\"t\":\"nav\",\"url\":\"/nowhere?i=%d\"}", i);
        ws_say(sa, msg);
        plat_sleep_ms(3);
    }
    CHECK(ws_wait(sa, &a, "[\"url\",\"/nowhere?i=59\"", 3000));
    long flood = app_renders() - before, took = (long)(now_ms() - t0);
    /* 15 at once, then 5 a second, plus one catch-up per tick (about a second apart) */
    long allowed = 16 + took / 200 + took / 1000 + 2;
    if (flood < 1 || flood > allowed) test_fail(__FILE__, __LINE__, "%ld renders for 60 navs in %ld ms", flood, took);

    /* the first hello may say which page it holds (data-h): then nothing is resent */
    char req[300];
    Buf r = {0};
    snprintf(req, sizeof req, "GET /more HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", adam);
    request(port, req, &r);
    const char *dh = strstr(buf_cstr(&r), "data-h=\"");
    CHECK(dh != NULL);
    char h[17] = "";
    if (dh) str_ncopy(h, sizeof h, dh + 8, 16);
    CHECK_INT(strspn(h, "0123456789abcdef"), 16);
    sock_t sf = ws_open(port, adam);
    Buf f = {0};
    snprintf(msg, sizeof msg, "{\"t\":\"hello\",\"url\":\"/more\",\"h\":\"%s\"}", h);
    ws_say(sf, msg);
    CHECK(ws_wait(sf, &f, "[\"url\",\"/more\"", 3000));
    ws_wait(sf, &f, "[\"morph\",", 400);
    CHECK(!strstr(buf_cstr(&f), "[\"morph\","));
    sock_close(sf);
    sock_t sg = ws_open(port, adam); /* a page that isn't the one it holds gets the whole page */
    buf_reset(&f);
    ws_say(sg, "{\"t\":\"hello\",\"url\":\"/more\",\"h\":\"0000000000000000\"}");
    CHECK(ws_wait(sg, &f, "[\"morph\",", 3000));
    sock_close(sg);
    sock_close(sa);

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&a);
    buf_free(&b);
    buf_free(&e);
    buf_free(&f);
    buf_free(&r);
    th_halt();
}

TEST(app_api_needs_a_member_or_admin) {
    /* review: wall tablets (display accounts) could read every table over /api; the 401 went
       out as "401 Unknown" and asked strangers to "pick who you are" */
    th_boot();
    auth_configure("e2e-salt", 1000);
    account("Adam", "adam", "correct horse", ROLE_ADMIN);
    account("Hallway", "hallway", "tablet-pass-1", ROLE_DISPLAY);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], hall[64], req[300];
    sign_in(port, "username=adam&password=correct+horse", adam, sizeof adam);
    sign_in(port, "username=hallway&password=tablet-pass-1", hall, sizeof hall);
    Buf r = {0};
    request(port, "GET /api/person HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 401 Unauthorized\r\n"));
    CHECK_HAS(body_of(&r), "{\"error\":\"sign in first\"}");
    snprintf(req, sizeof req, "GET /api/person HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             hall);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 403 Forbidden\r\n"));
    CHECK(!strstr(body_of(&r), "adam"));
    snprintf(req, sizeof req, "GET /api HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", hall);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), " 403 ");
    snprintf(req, sizeof req, "GET /api/person HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             adam);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_HAS(body_of(&r), "\"username\":\"adam\"");
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

static void api_get(int port, const char *cookie, const char *path, Buf *r) {
    char req[300];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", path,
             cookie);
    request(port, req, r);
}

TEST(app_api_shows_members_only_their_own_preferences) {
    /* issue #5: every member could read everyone's preferences (who is on a temporary password)
       and watch history over /api; the owner's call: members their own rows, admins all */
    th_boot();
    auth_configure("e2e-salt", 1000);
    int adam = account("Adam", "adam", "correct horse", ROLE_ADMIN);
    int eva = account("Eva", "eva", "eva password", ROLE_MEMBER);
    pref_set(adam, "test.adam", "adams");
    pref_set(eva, "test.eva", "evas");
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char ca[64], ce[64], path[64], mine[32];
    sign_in(port, "username=adam&password=correct+horse", ca, sizeof ca);
    sign_in(port, "username=eva&password=eva+password", ce, sizeof ce);
    Buf r = {0};
    api_get(port, ce, "/api/pref", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_HAS(body_of(&r), "\"key\":\"test.eva\"");
    CHECK(!strstr(body_of(&r), "test.adam"));
    api_get(port, ca, "/api/pref", &r);
    CHECK_HAS(body_of(&r), "\"key\":\"test.eva\"");
    CHECK_HAS(body_of(&r), "\"key\":\"test.adam\"");
    /* one row by id: someone else's is "not found" for a member */
    snprintf(mine, sizeof mine, "\"person\":%d", eva);
    int seen = 0, hidden = 0;
    for (int id = 1; id <= 20; id++) {
        snprintf(path, sizeof path, "/api/pref/%d", id);
        api_get(port, ce, path, &r);
        if (str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n")) {
            CHECK_HAS(body_of(&r), mine);
            seen++;
        }
        api_get(port, ca, path, &r);
        if (str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n") && !strstr(body_of(&r), mine)) hidden++;
    }
    CHECK(seen >= 1 && hidden >= 1);
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* The upgrade's answer (101, or 503 when refused) for a socket from 127.0.0.1. */
static sock_t ws_try(int port, const char *cookie, Buf *reply) {
    sock_t s = dial(port);
    if (s == SOCK_BAD) return s;
    char req[512];
    snprintf(req, sizeof req,
             "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n%s%s%s"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
             cookie[0] ? "Cookie: hemma=" : "", cookie, cookie[0] ? "\r\n" : "");
    send_all(s, req, strlen(req));
    buf_reset(reply);
    ws_wait(s, reply, "\r\n\r\n", 2000);
    return s;
}

TEST(app_live_messages_and_signed_out_screens_are_bounded) {
    th_boot();
    auth_configure("e2e-salt", 1000);
    account("Adam", "adam", "correct horse", ROLE_ADMIN);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64];
    sign_in(port, "username=adam&password=correct+horse", adam, sizeof adam);
    Buf a = {0}, r = {0};

    /* issue #8: a "live" message ran any action, and other screens never heard of the change */
    sock_t sa = ws_open(port, adam);
    ws_say(sa, "{\"t\":\"hello\",\"url\":\"/\"}");
    CHECK(ws_wait(sa, &a, "[\"morph\",", 3000));
    ws_say(sa, "{\"t\":\"live\",\"a\":\"household.save\",\"url\":\"/\",\"f\":[[\"name\",\"Quietly\"]]}");
    ws_wait(sa, &a, NULL, 300);
    CHECK_STR(app_name(), "Hemma"); /* not a preview: nothing ran */
    ws_say(sa, "{\"t\":\"act\",\"a\":\"household.save\",\"id\":7,\"url\":\"/\",\"f\":[[\"name\",\"Openly\"]]}");
    CHECK(ws_wait(sa, &a, "[\"ack\",7,true]", 3000));
    CHECK_STR(app_name(), "Openly");
    /* issue #60: an error rides on the ack, with the field it's about - wire.js shows it there, not
       in a toast over it; a removal's toast carries its Undo */
    buf_reset(&a);
    ws_say(sa, "{\"t\":\"act\",\"a\":\"people.save\",\"id\":8,\"url\":\"/\",\"f\":[[\"name\",\" \"]]}");
    CHECK(ws_wait(sa, &a, "[\"ack\",8,false,\"A name, please\",\"name\"]", 3000));
    CHECK(!strstr(buf_cstr(&a), "[\"toast\",2,"));
    buf_reset(&a);
    ws_say(sa, "{\"t\":\"act\",\"a\":\"plan.shop_add\",\"id\":9,\"url\":\"/\",\"f\":[[\"q\",\"milk\"]]}");
    CHECK(ws_wait(sa, &a, "[\"ack\",9,true]", 3000));
    ws_say(sa, "{\"t\":\"act\",\"a\":\"plan.shop_delete\",\"id\":10,\"url\":\"/\",\"f\":[[\"id\",\"1\"]]}");
    CHECK(ws_wait(sa, &a, "[\"toast\",0,\"Removed Milk\",{\"undo\":\"", 3000));

    /* issue #5: signed-out screens (they stay open while they answer pings) get a few
       connections per address, so one host can't hold every slot */
    sock_t s[5];
    for (int i = 0; i < 4; i++) {
        s[i] = ws_try(port, "", &r);
        CHECK_HAS(buf_cstr(&r), "HTTP/1.1 101 ");
    }
    s[4] = ws_try(port, "", &r);
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 503 ");
    CHECK_HAS(buf_cstr(&r), "too many signed-out screens");
    sock_t mine = ws_try(port, adam, &r); /* signed in: not counted against strangers */
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 101 ");
    sock_close(s[0]);
    plat_sleep_ms(200);
    sock_t again = ws_try(port, "", &r); /* a slot came free */
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 101 ");
    for (int i = 1; i < 5; i++) sock_close(s[i]);
    sock_close(again);
    sock_close(mine);
    sock_close(sa);
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&a);
    buf_free(&r);
    th_halt();
}

/* The log lines a test sees (the server thread logs too: a lock) - those holding g_seen_want */
static Lock g_seen_lock = LOCK_INIT;
static const char *g_seen_want = "browser of ";
static int g_browser_lines;
static char g_browser_last[600];
static Buf g_seen_all; /* every one of them, a line each */
static void seen_sink(LogLevel level, int64_t ts, const char *line) {
    (void)level, (void)ts;
    if (!strstr(line, g_seen_want)) return;
    lock_take(&g_seen_lock);
    g_browser_lines++;
    str_copy(g_browser_last, sizeof g_browser_last, line);
    buf_printf(&g_seen_all, "%s\n", line);
    lock_give(&g_seen_lock);
}

static int browser_lines(char *last, size_t cap) {
    lock_take(&g_seen_lock);
    int n = g_browser_lines;
    if (last) str_copy(last, cap, g_browser_last);
    lock_give(&g_seen_lock);
    return n;
}

TEST(app_a_browser_reports_its_trouble_to_the_log) {
    /* a TV's browser has no console anyone can read: wire.js sends script errors and stuck videos */
    th_boot();
    auth_configure("e2e-salt", 1000);
    account("Living room TV", "tv", "tv password", ROLE_DISPLAY);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char tv[64], last[600];
    sign_in(port, "username=tv&password=tv+password", tv, sizeof tv);
    LogSink was = log_set_sink(seen_sink);
    LogLevel level = g_log_level;
    bool to_stderr = g_log_stderr;
    g_log_level = LOG_WARN; /* (tests run quiet: listen, without printing) */
    g_log_stderr = false;
    g_browser_lines = 0;
    buf_reset(&g_seen_all);
    Buf a = {0};
    sock_t st = ws_open(port, tv);
    ws_say(st, "{\"t\":\"hello\",\"url\":\"/tv/1\"}");
    CHECK(ws_wait(st, &a, "[\"morph\",", 3000));
    const char *stuck = "{\"t\":\"log\",\"url\":\"/tv/1\",\"m\":\"video tv-video hasn't started after 25 s\\nFAKE line\","
                        "\"ua\":\"FakeTV/6.0\"}";
    ws_say(st, stuck);
    for (int i = 0; i < 200 && !browser_lines(NULL, 0); i++) plat_sleep_ms(10);
    CHECK_INT(browser_lines(last, sizeof last), 1);
    CHECK_HAS(last, "browser of tv (127.0.0.1) on /tv/1: video tv-video hasn't started after 25 s");
    CHECK_HAS(last, " - FakeTV/6.0"); /* the browser's user agent: with its first line only */
    CHECK(!strchr(last, '\n'));      /* (a browser can't forge log lines) */
    /* issue #79: the same line again is only counted */
    for (int i = 0; i < 5; i++) ws_say(st, stuck);
    ws_say(st, "{\"t\":\"log\",\"url\":\"/\",\"m\":\"script error: x\",\"ua\":\"FakeTV/6.0\"}");
    for (int i = 0; i < 200 && browser_lines(NULL, 0) < 2; i++) plat_sleep_ms(10);
    CHECK_INT(browser_lines(last, sizeof last), 2);
    CHECK_HAS(last, "script error: x");
    CHECK(!strstr(last, "FakeTV"));
    /* ...and a reload or a reconnect doesn't start the count again: 20 lines an hour per person */
    sock_close(st);
    st = ws_open(port, tv);
    ws_say(st, "{\"t\":\"hello\",\"url\":\"/\"}");
    char msg[120];
    for (int i = 0; i < 30; i++) {
        snprintf(msg, sizeof msg, "{\"t\":\"log\",\"url\":\"/\",\"m\":\"line %d\"}", i);
        ws_say(st, msg);
    }
    ws_wait(st, &a, NULL, 400);
    CHECK_INT(browser_lines(last, sizeof last), 20);
    CHECK_HAS(last, "(no more from tv's browsers until ");
    /* issue #102: the note comes before the address - a long one pushed it past the end of the line */
    CHECK(strstr(last, "(no more from") && strstr(last, " on /") && strstr(last, "(no more from") < strstr(last, " on /"));
    sock_t again = ws_open(port, tv);
    ws_say(again, "{\"t\":\"hello\",\"url\":\"/\"}");
    ws_say(again, "{\"t\":\"log\",\"url\":\"/\",\"m\":\"another\"}");
    ws_wait(again, &a, NULL, 300);
    CHECK_INT(browser_lines(NULL, 0), 20);
    /* signed out: nothing */
    sock_t sx = ws_open(port, "");
    ws_say(sx, "{\"t\":\"hello\",\"url\":\"/\"}");
    ws_say(sx, "{\"t\":\"log\",\"url\":\"/\",\"m\":\"stranger\"}");
    ws_wait(sx, &a, NULL, 300);
    CHECK_INT(browser_lines(NULL, 0), 20);
    /* the hour over: what was left out is said at once - issue #102: not only when that browser
       writes again (a TV that went quiet never said how bad it was) - and lines come again */
    g_fake_now = now_sec() + 3601;
    for (int i = 0; i < 1000 && browser_lines(NULL, 0) < 21; i++) plat_sleep_ms(10); /* (a tick: up to 10 s on a slow box) */
    CHECK_INT(browser_lines(last, sizeof last), 21);
    CHECK_HAS(last, "browser of tv: 18 more lines since "); /* 5 repeats, 12 + 1 over */
    ws_say(again, "{\"t\":\"log\",\"url\":\"/\",\"m\":\"the next hour\"}");
    for (int i = 0; i < 200 && browser_lines(NULL, 0) < 22; i++) plat_sleep_ms(10);
    CHECK_INT(browser_lines(last, sizeof last), 22);
    CHECK_HAS(last, "the next hour");
    g_fake_now = 0;
    log_set_sink(was);
    g_log_level = level;
    g_log_stderr = to_stderr;
    sock_close(st);
    sock_close(again);
    sock_close(sx);
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&a);
    th_halt();
}

TEST(app_failed_sign_ins_never_log_what_was_typed) {
    /* issue #34: a password typed into the name box (a phone filled the wrong one) went into
       the log every admin reads. A name that is an account's shows as its username; anything
       else only as its length. */
    th_boot();
    auth_configure("e2e-salt", 1000);
    account("Adam", "adam", "correct horse", ROLE_ADMIN);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    LogSink was = log_set_sink(seen_sink);
    LogLevel level = g_log_level;
    bool to_stderr = g_log_stderr;
    g_log_level = LOG_INFO;
    g_log_stderr = false;
    g_seen_want = "failed ";
    g_browser_lines = 0;
    char cookie[64], last[600];
    sign_in(port, "username=correct+horse+battery&password=x", cookie, sizeof cookie);
    CHECK_INT(browser_lines(last, sizeof last), 1);
    CHECK_HAS(last, "failed sign-in for an unknown name (21 characters) from 127.0.0.1");
    CHECK(!strstr(last, "horse"));
    sign_in(port, "username=Adam&password=wrong", cookie, sizeof cookie); /* by name: the username */
    CHECK_INT(browser_lines(last, sizeof last), 2);
    CHECK_HAS(last, "failed sign-in for \"adam\" from 127.0.0.1");
    Buf r = {0};
    const char *forgot = "username=s3cret%21&code=AAAA-BBBB-CCCC&password=a+new+password&again=a+new+password";
    char req[512];
    snprintf(req, sizeof req, "POST /forgot HTTP/1.1\r\nHost: t\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
             strlen(forgot), forgot);
    request(port, req, &r);
    CHECK_INT(browser_lines(last, sizeof last), 3);
    CHECK_HAS(last, "failed password recovery for an unknown name (7 characters) from 127.0.0.1");
    CHECK(!strstr(last, "s3cret"));
    log_set_sink(was);
    g_log_level = level;
    g_log_stderr = to_stderr;
    g_seen_want = "browser of ";
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* Static files go out gzipped to browsers that say they take it (the build adds a ".gz" twin);
   the rest get them as they are. */
TEST(app_serves_assets_gzipped_to_browsers_that_take_it) {
    bool twin = false;
    for (int i = 0; i < g_nassets; i++) twin |= g_assets[i].path && !strcmp(g_assets[i].path, "/wire.js.gz");
    th_boot();
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    Buf r = {0};
    request(port, "GET /static/wire.js HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip, deflate, br\r\nConnection: close\r\n\r\n",
            &r);
    CHECK_HAS(buf_cstr(&r), "200 OK");
    CHECK_HAS(buf_cstr(&r), "Vary: Accept-Encoding");
    if (twin) { /* (a build without gzip at hand has no twins) */
        CHECK_HAS(buf_cstr(&r), "Content-Encoding: gzip");
        const unsigned char *b = (const unsigned char *)body_of(&r);
        CHECK(b[0] == 0x1f && b[1] == 0x8b);
    }
    request(port, "GET /static/wire.js HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK(!strstr(buf_cstr(&r), "Content-Encoding"));
    CHECK_HAS(body_of(&r), "Hemma's browser wire");
    request(port, "GET /static/wire.js.gz HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK_HAS(buf_cstr(&r), "404");
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

TEST(app_calendar_feed_answers_its_own_key_only) {
    /* issue #41: a phone's calendar fetches its link without Hemma's cookie - the key in the address is
       what lets it in, to that one calendar and nothing else */
    th_boot();
    auth_configure("e2e-salt", 1000);
    int adam = account("Adam", "adam", "correct horse", ROLE_ADMIN);
    pref_set(adam, PREF_CAL, "1");
    char path[96], bad[96], req[400];
    CHECK(cal_feed_path(adam, path, sizeof path));
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    Buf r = {0};
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: hemma.local\r\nConnection: close\r\n\r\n", path);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_HAS(buf_cstr(&r), "Content-Type: text/calendar; charset=utf-8\r\n");
    CHECK(str_starts(body_of(&r), "BEGIN:VCALENDAR\r\n"));
    CHECK(!strstr(buf_cstr(&r), "Set-Cookie"));
    /* one character off: nothing */
    str_copy(bad, sizeof bad, path);
    bad[strlen(bad) - 5] = bad[strlen(bad) - 5] == 'a' ? 'b' : 'a';
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: hemma.local\r\nConnection: close\r\n\r\n", bad);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    CHECK_HAS(body_of(&r), "Make a new calendar link");
    CHECK(!strstr(body_of(&r), "BEGIN:VCALENDAR"));
    /* someone else's name for Hemma (DNS rebinding) is refused before anything else */
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: calendar.example.com\r\nConnection: close\r\n\r\n", path);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 403 "));
    snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: hemma.local\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             path);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 405 "));
    /* a new link: the old address answers no more */
    pref_set(adam, PREF_CAL, "2");
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: hemma.local\r\nConnection: close\r\n\r\n", path);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

TEST(app_board_photos_are_for_the_household) {
    /* issue #47: a note's photo is served to people signed in, kept for good by the browser, and to
       nobody else; a made-up name gets nothing */
    th_boot();
    auth_configure("e2e-salt", 1000);
    int adam = account("Adam", "adam", "correct horse", ROLE_ADMIN);
    static const uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x0A, 0x00, 0x14, 0x03, 0x01,
                                  0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xD9};
    char enc[64];
    base64_encode(jpg, sizeof jpg, enc);
    Buf form = {0}, url = {0};
    buf_printf(&url, "data:image/jpeg;base64,%s", enc);
    buf_puts(&form, "text=Permit&photo=");
    url_encode(&form, url.p, false);
    CHECK(th_act(adam, "board.post", form.p).ok);
    const char *page = th_page(adam, "/board"), *at = strstr(page, "/board/photo/");
    char path[64] = "";
    if (at) snprintf(path, sizeof path, "%.33s", at);
    CHECK_INT((int)strlen(path), 33);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char cookie[64], req[400];
    sign_in(port, "username=adam&password=correct+horse", cookie, sizeof cookie);
    Buf r = {0};
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", path, cookie);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_HAS(buf_cstr(&r), "Content-Type: image/jpeg\r\n");
    CHECK_HAS(buf_cstr(&r), "immutable");
    const char *body = strstr(buf_cstr(&r), "\r\n\r\n");
    CHECK(body && r.len - (size_t)(body + 4 - r.p) == sizeof jpg && memcmp(body + 4, jpg, sizeof jpg) == 0);
    /* signed out: not the photo */
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", path);
    request(port, req, &r);
    CHECK(!strstr(buf_cstr(&r), "image/jpeg"));
    /* a name read_photo never makes */
    snprintf(req, sizeof req, "GET /board/photo/..%%2Fhemma.db HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             cookie);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 404 "));
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    buf_free(&form);
    buf_free(&url);
    th_halt();
}

TEST(app_wire_fields_keep_their_whole_length) {
    /* issue #47: a form field over the socket was cut at 4 KB without a word - a note's photo (~800 KB of
       base64) came out as its first 3 KB. Here: a JPEG with 6 KB of padding, 8 KB as base64. */
    if (!module_find("board")) return;
    th_boot();
    auth_configure("e2e-salt", 1000);
    account("Adam", "adam", "correct horse", ROLE_ADMIN);
    size_t pad = 6000, n = 2 + 4 + pad + 19 + 2;
    uint8_t *jpg = xmalloc(n);
    size_t o = 0;
    jpg[o++] = 0xFF, jpg[o++] = 0xD8;
    jpg[o++] = 0xFF, jpg[o++] = 0xE1, jpg[o++] = (uint8_t)((pad + 2) >> 8), jpg[o++] = (uint8_t)(pad + 2);
    for (size_t i = 0; i < pad; i++) jpg[o++] = (uint8_t)(i * 7);
    static const uint8_t sof[] = {0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x0A, 0x00, 0x14, 0x03,
                                  0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01};
    memcpy(jpg + o, sof, sizeof sof);
    o += sizeof sof;
    jpg[o++] = 0xFF, jpg[o++] = 0xD9;
    char *enc = xmalloc(4 * ((n + 2) / 3) + 1);
    base64_encode(jpg, n, enc);
    Buf msg = {0};
    buf_printf(&msg, "{\"t\":\"act\",\"a\":\"board.post\",\"id\":11,\"url\":\"/board\",\"f\":[[\"text\",\"Long\"],"
                     "[\"photo\",\"data:image/jpeg;base64,%s\"]]}",
               enc);
    CHECK(msg.len > 8000);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char cookie[64];
    sign_in(port, "username=adam&password=correct+horse", cookie, sizeof cookie);
    sock_t s = ws_open(port, cookie);
    uint8_t *frame = xmalloc(msg.len + 16);
    size_t fn = client_frame(frame, msg.p, msg.len);
    send_all(s, (const char *)frame, fn);
    Buf r = {0};
    CHECK(ws_wait(s, &r, "\"ack\",11,true", 3000));
    sock_close(s);
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    /* the whole photo was kept */
    const char *page = th_page(1, "/board"), *at = strstr(page, "/board/photo/");
    char path[700] = "";
    if (at) snprintf(path, sizeof path, "%s/board/%.20s", app_config()->data_dir, at + strlen("/board/photo/"));
    int64_t size = 0, mtime = 0;
    bool dir = false;
    CHECK(path[0] && path_stat(path, &size, &mtime, &dir));
    CHECK(size == (int64_t)n);
    free(jpg);
    free(enc);
    free(frame);
    buf_free(&msg);
    buf_free(&r);
    th_halt();
}
