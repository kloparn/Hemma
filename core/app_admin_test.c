/* MOD_ADMIN (a section only admins have: navigation, pages, raw endpoints) and the public
 * /healthz - the first through the harness and over real sockets, with a module of its own. */
#include "../tests/harness.h"
#include "i18n.h"
#include "server.h"

static volatile int g_hook_calls;
static bool g_on; /* the side block and agenda items only while these tests run (the module stays registered) */

static void backstage_page(Ctx *c, Buf *b) { buf_puts(b, "<p id=\"zz-backstage\">the admins' corner</p>"); }

static bool backstage_http(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_eq(r->path, "/zzbackstage/raw")) return false;
    g_hook_calls++;
    conn_reply(conn, 200, "text/plain", "backstage raw\n", 14, NULL);
    return true;
}

static void backstage_side(Ctx *c, Buf *b) {
    if (g_on) buf_puts(b, "<div id=\"zz-backstage-side\">backstage side</div>");
}

static void backstage_agenda(Ctx *c, int64_t from, int64_t to, Agenda *out) {
    if (!g_on) return;
    AgendaItem it = {.start = from, .end = from + 3600};
    str_copy(it.title, sizeof it.title, "Backstage rehearsal");
    agenda_add(out, &it);
}

static void backstage_poke(Ctx *c, Fields *f) {
    if (!is_admin(c)) ctx_fail(c, "Only an admin can do that");
}

static const Action backstage_actions[] = {{"zzbackstage.poke", backstage_poke}, {0}};

static const Module backstage = {
    .id = "zzbackstage",
    .title = "Backstage",
    .icon = "settings",
    .nav = 999,
    .flags = MOD_ADMIN,
    .page = backstage_page,
    .actions = backstage_actions,
    .http = backstage_http,
    .agenda = backstage_agenda,
    .side = backstage_side,
};

/* (it stays in the navigation of later tests in this process: in their languages too - issue #46) */
static const Tr BACKSTAGE_TR[] = {{"Backstage", "Bakom scenen", "Entre bastidores"}};
TRANSLATIONS(app_admin_test, BACKSTAGE_TR)

static void with_backstage(void) { /* registered once, before a boot (it stays: last in the nav, admins only) */
    static bool done;
    if (!done) module_register(&backstage);
    done = true;
    g_on = true;
}

static int backstage_times(int person) {
    Ctx c;
    app_ctx(&c, person, "/");
    Agenda ag = {0};
    agenda_collect(&c, dt_make(2026, 10, 12, 0, 0), dt_make(2026, 10, 13, 0, 0), NULL, &ag);
    int n = 0;
    for (int i = 0; i < ag.n; i++) n += str_eq(ag.items[i].title, "Backstage rehearsal");
    agenda_free(&ag);
    app_ctx_free(&c);
    return n;
}

static int person_as(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

TEST(app_admin_sections_are_only_there_for_admins) {
    with_backstage();
    th_boot();
    int adam = person_as("Adam", ROLE_ADMIN), eva = person_as("Eva", ROLE_MEMBER);
    const char *html = th_page(eva, "/");
    CHECK(!strstr(html, "href=\"/zzbackstage\"")); /* not in the navigation */
    CHECK(!strstr(th_page(eva, "/more"), "/zzbackstage"));
    html = th_page(eva, "/zzbackstage");
    CHECK_HAS(html, "Nothing here");
    CHECK(!strstr(html, "zz-backstage"));
    CHECK(!strstr(th_page(eva, "/zzbackstage/deeper"), "zz-backstage"));
    CHECK(!strstr(th_page(0, "/zzbackstage"), "zz-backstage")); /* signed out: the sign-in page */
    CHECK(!strstr(th_page(eva, "/"), "zz-backstage-side")); /* nor its sidebar block */
    CHECK(!strstr(th_page(eva, "/more"), "zz-backstage-side"));
    CHECK_INT(backstage_times(eva), 0); /* nor its times in shared calendars */

    CHECK_HAS(th_page(adam, "/"), "href=\"/zzbackstage\"");
    CHECK_HAS(th_page(adam, "/more"), "href=\"/zzbackstage\"");
    CHECK_HAS(th_page(adam, "/zzbackstage"), "id=\"zz-backstage\"");
    CHECK_HAS(th_page(adam, "/"), "id=\"zz-backstage-side\"");
    CHECK_HAS(th_page(adam, "/more"), "id=\"zz-backstage-side\"");
    CHECK_INT(backstage_times(adam), 1);
    g_on = false;
    th_halt();
}

/* ------------------------------------------------- over real sockets */

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

/* One request on a fresh connection; the whole response, until the server closes it. */
static void request(int port, const char *raw, Buf *out) {
    buf_reset(out);
    sock_t s = dial(port);
    if (s == SOCK_BAD) return;
    for (size_t n = strlen(raw); n;) {
        long k = sock_send(s, raw, n);
        if (k > 0) raw += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
    int64_t end = now_ms() + 4000;
    while (now_ms() < end) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[8192];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(out, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
    buf_cstr(out);
}

static const char *body_of(const Buf *resp) {
    const char *e = resp->p ? strstr(resp->p, "\r\n\r\n") : NULL;
    return e ? e + 4 : "";
}

static bool start_server(int *port) {
    if (!server_listen("127.0.0.1", 0)) return false;
    *port = server_port();
    g_done = 0;
    return thread_spawn(serve, NULL);
}

static bool stop_server(void) {
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    server_shutdown();
    return g_done;
}

TEST(app_admin_sections_keep_their_endpoints_from_everyone_else) {
    with_backstage();
    th_boot();
    int adam = person_as("Adam", ROLE_ADMIN), eva = person_as("Eva", ROLE_MEMBER);
    char ca[33], ce[33], req[300];
    CHECK(session_create(adam, false, "test", ca));
    CHECK(session_create(eva, false, "test", ce));
    int port = 0;
    CHECK(start_server(&port));
    Buf r = {0};
    g_hook_calls = 0;
    snprintf(req, sizeof req, "GET /zzbackstage/raw HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             ce);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 200 ");
    CHECK_HAS(body_of(&r), "Nothing here"); /* a member gets the page that isn't there */
    CHECK(!strstr(body_of(&r), "backstage raw"));
    request(port, "GET /zzbackstage/raw HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK(!strstr(body_of(&r), "backstage raw")); /* signed out */
    CHECK_INT(g_hook_calls, 0);
    snprintf(req, sizeof req, "GET /zzbackstage/raw HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             ca);
    request(port, req, &r);
    CHECK_STR(body_of(&r), "backstage raw\n");
    CHECK_INT(g_hook_calls, 1);
    /* /api lists an admin section's actions to admins only (the actions check is_admin themselves) */
    snprintf(req, sizeof req, "GET /api HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", ce);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "\"actions\":[");
    CHECK(!strstr(body_of(&r), "zzbackstage.poke"));
    snprintf(req, sizeof req, "GET /api HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", ca);
    request(port, req, &r);
    CHECK_HAS(body_of(&r), "\"zzbackstage.poke\"");
    g_on = false;
    CHECK(stop_server());
    buf_free(&r);
    th_halt();
}

TEST(app_healthz_says_ok_and_nothing_more) {
    th_boot();
    int port = 0;
    CHECK(start_server(&port));
    Buf r = {0};
    request(port, "GET /healthz HTTP/1.1\r\nHost: 192.168.1.50\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_HAS(buf_cstr(&r), "Content-Type: text/plain; charset=utf-8\r\n");
    CHECK_HAS(buf_cstr(&r), "Cache-Control: no-store\r\n");
    CHECK_STR(body_of(&r), "ok\n"); /* no version, no names, no sign-in page */
    CHECK(!strstr(buf_cstr(&r), "Set-Cookie"));
    request(port, "HEAD /healthz HTTP/1.1\r\nHost: hemma.local\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_STR(body_of(&r), "");
    request(port, "POST /healthz HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 405 "));
    /* the host guard comes first: a stranger's name pointing here learns nothing either */
    request(port, "GET /healthz HTTP/1.1\r\nHost: evil.example.com\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 403 "));
    CHECK(!strstr(body_of(&r), "ok\n") || strstr(body_of(&r), "Hemma doesn't answer"));
    CHECK(stop_server());
    buf_free(&r);
    th_halt();
}

/* 2.0: the Android app finds Hemma by asking GET /hemma.json - who it is, and nothing more */
TEST(app_hemma_json_says_which_hemma_this_is) {
    char data[512];
    str_copy(data, sizeof data, test_tmpdir());
    th_boot_in(data);
    setting_set("household.name", "Björk \"by the lake\"");
    char id[17];
    str_copy(id, sizeof id, app_instance());
    CHECK_INT((int)strlen(id), 16);
    int port = 0;
    CHECK(start_server(&port));
    Buf r = {0};
    request(port, "GET /hemma.json HTTP/1.1\r\nHost: 192.168.1.50\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_HAS(buf_cstr(&r), "Content-Type: application/json\r\n");
    CHECK_HAS(buf_cstr(&r), "Cache-Control: no-store\r\n");
    char want[200];
    snprintf(want, sizeof want, "{\"hemma\":\"%s\",\"id\":\"%s\",\"name\":\"Björk \\\"by the lake\\\"\"}\n", HEMMA_VERSION, id);
    CHECK_STR(body_of(&r), want); /* (signed out: no cookie, no sign-in page) */
    CHECK(!strstr(buf_cstr(&r), "Set-Cookie"));
    request(port, "POST /hemma.json HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 405 "));
    request(port, "GET /hemma.json HTTP/1.1\r\nHost: evil.example.com\r\nConnection: close\r\n\r\n", &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 403 ")); /* (the host guard first) */
    CHECK(stop_server());
    buf_free(&r);
    th_halt();
    /* the same Hemma after a restart: the same id */
    th_boot_in(data);
    CHECK_STR(app_instance(), id);
    th_halt();
}

/* 2.0: a sign-in from the app is named as one - and pages know it (session_in_app) */
TEST(app_knows_a_sign_in_from_the_app) {
    th_boot();
    int eva = person_as("Eva", ROLE_MEMBER);
    char sid[33];
    CHECK(session_create(eva, false,
                         "Mozilla/5.0 (Linux; Android 17; Pixel 10 Build/X; wv) AppleWebKit/537.36 (KHTML, like Gecko) "
                         "Version/4.0 Chrome/140.0.0.0 Mobile Safari/537.36 HemmaApp/2.0.0",
                         sid));
    CHECK(session_create(eva, false, "Mozilla/5.0 (Linux; Android 17) Chrome/140.0.0.0 Mobile Safari/537.36", sid));
    int in_app = 0, browser = 0;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        Buf said = {0};
        i18n_unpack(&said, s->agent);
        if (session_in_app(s)) {
            in_app++;
            CHECK_STR(buf_cstr(&said), "The Hemma app on Android");
        } else {
            browser++;
            CHECK_STR(buf_cstr(&said), "Chrome on Android");
        }
        buf_free(&said);
    }
    CHECK(in_app == 1 && browser == 1);
    th_halt();
}
