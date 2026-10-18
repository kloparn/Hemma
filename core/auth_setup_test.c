/* First run (the welcome page), recovery codes, "Forgot your password?" and the
 * server-side reset (hemma --reset-password) - unit tests plus end-to-end runs over
 * a loopback socket, driven like a browser without JavaScript would. */
#include "../tests/harness.h"
#include "auth.h"
#include "db.h"
#include "env.h"
#include "server.h"

static int with_login(const char *name, const char *user, const char *pw, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash(pw, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* The plain codes for `person`, made for a session nobody else has. */
static void make_codes(int person, char codes[AUTH_CODES][AUTH_CODE_LEN]) {
    Session s = {0};
    snprintf(s.token, sizeof s.token, "unit-%d", person);
    s.person = person;
    auth_codes_new(person, &s);
    auth_codes_fresh(&s, codes);
}

static bool raw_has(const void *p, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp((const char *)p + i, needle, k) == 0) return true;
    return false;
}

/* ------------------------------------------------------------- first run */

TEST(auth_setup_page_only_while_there_is_no_admin) {
    th_boot();
    auth_configure("pepper", 1000);
    CHECK(auth_setup_open());
    const char *h = th_page(0, "/book?w=2026-10-12"); /* any signed-out visit */
    CHECK_HAS(h, "data-mod=\"auth\"");
    CHECK_HAS(h, "Welcome to Hemma");
    CHECK_HAS(h, "action=\"/setup\"");
    CHECK_HAS(h, "autocomplete=\"new-password\"");
    CHECK(!strstr(h, "action=\"/login\""));
    h = th_page(0, "/setup?e=again&name=Alex+D&u=alex&home=Villa");
    CHECK_HAS(h, "The two passwords don't match.");
    CHECK_HAS(h, "value=\"Alex D\""); /* typed values come back (never the passwords) */
    CHECK_HAS(h, "value=\"alex\"");
    CHECK_HAS(h, "value=\"Villa\"");
    CHECK_HAS(th_page(0, "/setup?e=wait&w=120"), "Wait 2 minutes");
    /* old data: someone can sign in, but there was never an admin - offered, with a way past it */
    with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    CHECK(auth_setup_open());
    CHECK_HAS(th_page(0, "/"), "href=\"/login\">I already have an account");
    CHECK_HAS(th_page(0, "/login"), "action=\"/login\"");
    /* an admin who can sign in: gone, wherever you look */
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    CHECK(!auth_setup_open());
    h = th_page(0, "/setup");
    CHECK(!strstr(h, "action=\"/setup\""));
    CHECK_HAS(h, "action=\"/login\"");
    /* without the server salt nothing can be hashed: the page says what to do */
    auth_configure("", 1000);
    CHECK(!auth_setup_open());
    CHECK_HAS(th_page(0, "/"), "hemma --gen-salt");
    th_halt();
}

/* Issue #87: a rescue that got no admin back showed "Welcome to Hemma" - set it up again, over the
   household's data - or "Nobody can sign in", and the rescue notice was for admins only. */
TEST(auth_no_welcome_page_after_a_rescue) {
    th_boot();
    auth_configure("pepper", 1000);
    CHECK(auth_setup_open());
    setting_set(SETTING_RESCUED, "2026-10-17 09:30: Hemma's data was damaged (parts of it couldn't be read). Nothing "
                                 "was deleted: the damaged file is kept as hemma.db.damaged-2026-10-17-0930.");
    CHECK(!auth_setup_open());
    const char *h = th_page(0, "/");
    CHECK_HAS(h, "<h1>Hemma's data was damaged</h1>");
    CHECK_HAS(h, "kept as hemma.db.damaged-2026-10-17-0930");
    CHECK_HAS(h, "docs/troubleshooting.md");
    CHECK(!strstr(h, "Welcome to Hemma"));
    CHECK(!strstr(h, "action=\"/setup\""));
    CHECK(!strstr(h, "href=\"/login\">Sign in</a>")); /* (nobody can) */
    CHECK(!strstr(th_page(0, "/setup"), "action=\"/setup\""));
    /* members came back: they find the sign-in page, which says things may be missing ... */
    int eva = with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    CHECK_HAS(th_page(0, "/book"), "href=\"/login\">Sign in</a>");
    h = th_page(0, "/login");
    CHECK_HAS(h, "action=\"/login\"");
    CHECK_HAS(h, "some things may be missing");
    /* ... and, signed in, hear that no admin came back */
    CHECK_HAS(th_page(eva, "/board"), "no admin account came back");
    /* an admin again (Locked out): sign-in as usual, the details for admins */
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    h = th_page(0, "/");
    CHECK_HAS(h, "action=\"/login\"");
    CHECK_HAS(h, "some things may be missing");
    CHECK(!strstr(th_page(eva, "/board"), "no admin account came back"));
    CHECK_HAS(th_page(adam, "/board"), "See what happened");
    th_halt();
}

static void clear_admin_env(void) {
    plat_setenv("HEMMA_ADMIN_USER", "", true);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "", true);
    plat_setenv("HEMMA_ADMIN_NAME", "", true);
    plat_setenv("HEMMA_ADMIN_RESET", "", true);
}

TEST(auth_setup_closed_by_the_environment_bootstrap_for_good) {
    th_boot();
    auth_configure("pepper", 1000);
    plat_setenv("HEMMA_ADMIN_USER", "boss", true);
    auth_bootstrap_admin(); /* a username without a password: the browser creates the admin instead */
    CHECK(auth_setup_open());
    plat_setenv("HEMMA_ADMIN_PASSWORD", "dev-password-123", true);
    auth_bootstrap_admin();
    CHECK(person_by_username("boss") != NULL);
    CHECK(!auth_setup_open());
    CHECK_HAS(th_page(0, "/"), "action=\"/login\"");
    /* the household had an admin: even when it is gone, nobody gets to make themselves one */
    tbl_del(&T_person, person_by_username("boss")->id);
    CHECK(!auth_setup_open());
    const char *h = th_page(0, "/");
    CHECK_HAS(h, "Nobody can sign in");
    CHECK_HAS(h, "docs/accounts.md");
    CHECK(!strstr(h, "action=\"/setup\""));
    clear_admin_env();
    th_halt();
}

/* ---- end to end: a loopback server on its own thread ---- */

static volatile int g_done;
static void serve(void *arg) {
    (void)arg;
    app_serve();
    g_done = 1;
}

static int start_server(void) {
    if (!server_listen("127.0.0.1", 0)) return 0;
    g_done = 0;
    if (!thread_spawn(serve, NULL)) return 0;
    return server_port();
}

static bool stop_server(int port) {
    server_stop();
    sock_t poke = tcp_connect_nb(0x7F000001, port); /* wake a loop asleep until its next tick */
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    if (poke != SOCK_BAD) sock_close(poke);
    server_shutdown();
    return g_done;
}

static sock_t dial(int port) {
    sock_t s = tcp_connect_nb(0x7F000001, port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) {
        if (s != SOCK_BAD) sock_close(s);
        return SOCK_BAD;
    }
    return s;
}

static void send_all(sock_t s, const char *p) {
    for (size_t n = strlen(p); n;) {
        long k = sock_send(s, p, n);
        if (k > 0) p += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
}

/* The whole response ("Connection: close"): until the server closes or Content-Length arrived. */
static void recv_reply(sock_t s, Buf *out) {
    buf_reset(out);
    for (int64_t end = now_ms() + 4000; now_ms() < end;) {
        pollfd_t pr = {.fd = s, .events = POLLIN};
        if (plat_poll(&pr, 1, 50) != 1) continue;
        char tmp[8192];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(out, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
        const char *hdr_end = strstr(buf_cstr(out), "\r\n\r\n"), *cl = strstr(buf_cstr(out), "Content-Length: ");
        if (hdr_end && cl && cl < hdr_end &&
            (long long)(out->len - (size_t)(hdr_end + 4 - out->p)) >= strtoll(cl + 16, NULL, 10))
            break;
    }
    sock_close(s);
}

static void form_request(Buf *req, const char *path, const char *cookie, const char *origin, const char *form) {
    buf_reset(req);
    buf_printf(req, "POST %s HTTP/1.1\r\nHost: t\r\nUser-Agent: Mozilla/5.0 (iPhone) Safari/605.1\r\n", path);
    if (cookie && *cookie) buf_printf(req, "Cookie: hemma=%s\r\n", cookie);
    if (origin) buf_printf(req, "Origin: %s\r\n", origin);
    buf_printf(req,
               "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
               strlen(form), form);
}

static void post(int port, const char *path, const char *cookie, const char *form, Buf *out) {
    Buf req = {0};
    form_request(&req, path, cookie, NULL, form);
    buf_reset(out);
    sock_t s = dial(port);
    if (s != SOCK_BAD) {
        send_all(s, buf_cstr(&req));
        recv_reply(s, out);
    }
    buf_free(&req);
}

static void get(int port, const char *path, const char *cookie, Buf *out) {
    char req[600];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: t\r\n%s%s%sConnection: close\r\n\r\n", path,
             cookie && *cookie ? "Cookie: hemma=" : "", cookie && *cookie ? cookie : "", cookie && *cookie ? "\r\n" : "");
    buf_reset(out);
    sock_t s = dial(port);
    if (s != SOCK_BAD) {
        send_all(s, req);
        recv_reply(s, out);
    }
}

static void cookie_of(Buf *r, char *out, size_t cap) {
    out[0] = '\0';
    const char *c = r->p ? strstr(buf_cstr(r), "Set-Cookie: hemma=") : NULL;
    if (c) str_ncopy(out, cap, c + 18, strcspn(c + 18, ";\r\n"));
}

static bool signed_in(int port, const char *cookie) {
    Buf r = {0};
    get(port, "/", cookie, &r);
    bool in = strstr(buf_cstr(&r), " 200 ") && !strstr(buf_cstr(&r), "action=\"/login\"");
    buf_free(&r);
    return in;
}

#define SETUP_FORM "name=Alex+Doe&username=Alex&password=correct+horse+1&again=correct+horse+1&home=Villa+Test&remember=1"

/* The setup code in force, read once before the server thread starts (the store isn't shared
   across threads), and a form with it added. */
static char g_code[16];
static const char *with_code(const char *form) {
    static char bufs[2][512];
    static int k;
    char *out = bufs[k++ & 1];
    snprintf(out, 512, "%s&code=%s", form, g_code);
    return out;
}

TEST(auth_setup_creates_the_admin_with_codes_and_signs_in) {
    if (!module_find("people")) return; /* the codes are shown on the household's page */
    th_boot();
    auth_configure("e2e-salt", 1000);
    str_copy(g_code, sizeof g_code, auth_setup_code());
    CHECK_INT(strlen(g_code), 9); /* "7KQ4-M2XP" */
    CHECK_STR(auth_setup_code(), g_code); /* the same until it's used */
    int port = start_server();
    CHECK(port > 0);
    Buf r = {0}, req = {0};
    char cookie[64] = "", first_code[AUTH_CODE_LEN] = "";

    get(port, "/", NULL, &r);
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 200 OK");
    CHECK_HAS(buf_cstr(&r), "action=\"/setup\"");
    /* another site can't post the form */
    form_request(&req, "/setup", NULL, "http://evil.example", with_code(SETUP_FORM));
    sock_t s = dial(port);
    CHECK(s != SOCK_BAD);
    send_all(s, buf_cstr(&req));
    recv_reply(s, &r);
    CHECK_HAS(buf_cstr(&r), " 403 ");
    /* mistakes go back to the form with what was typed (never the passwords) */
    post(port, "/setup", NULL, with_code("name=Alex&username=Alex&password=correct+horse+1&again=correct+horse+2"), &r);
    CHECK_HAS(buf_cstr(&r), "Location: /setup?e=again&name=Alex&u=alex&home=\r\n");
    post(port, "/setup", NULL, with_code("name=Alex&username=a+b&password=correct+horse+1&again=correct+horse+1"), &r);
    CHECK_HAS(buf_cstr(&r), "Location: /setup?e=user&");
    post(port, "/setup", NULL, with_code("name=+&username=alex&password=correct+horse+1&again=correct+horse+1"), &r);
    CHECK_HAS(buf_cstr(&r), "Location: /setup?e=name&");
    post(port, "/setup", NULL, with_code("name=Alex&username=alex&password=short&again=short"), &r);
    CHECK_HAS(buf_cstr(&r), "Location: /setup?e=short&");
    /* without the setup code - or a wrong one - nobody becomes the admin */
    post(port, "/setup", NULL, "name=Alex&username=alex&password=correct+horse+1&again=correct+horse+1", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /setup?e=setup&");
    post(port, "/setup", NULL, "name=Alex&username=alex&password=correct+horse+1&again=correct+horse+1&code=2222-2222", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /setup?e=setup&");
    cookie_of(&r, cookie, sizeof cookie);
    CHECK_STR(cookie, "");

    /* the real thing (the code typed in lower case, without its dash): signed in, on to the recovery codes */
    char typed[16], form[300];
    int t = 0;
    for (const char *p = g_code; *p; p++)
        if (*p != '-') typed[t++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    typed[t] = '\0';
    snprintf(form, sizeof form, "%s&code=%s", SETUP_FORM, typed);
    post(port, "/setup", NULL, form, &r);
    CHECK_HAS(buf_cstr(&r), "303 See Other");
    CHECK_HAS(buf_cstr(&r), "Location: /people/codes\r\n");
    CHECK_HAS(buf_cstr(&r), "HttpOnly");
    CHECK_HAS(buf_cstr(&r), "Max-Age=2592000"); /* an admin: a month (ADMIN_SESSION_DAYS), not a year */
    cookie_of(&r, cookie, sizeof cookie);
    CHECK_INT(strlen(cookie), 32);
    get(port, "/people/codes", cookie, &r);
    CHECK_HAS(buf_cstr(&r), "Your recovery codes");
    CHECK_HAS(buf_cstr(&r), "Save these now");
    int shown = 0;
    for (const char *p = buf_cstr(&r); (p = strstr(p, "<li><code>")) != NULL; p += 10, shown++)
        if (!shown) str_ncopy(first_code, sizeof first_code, p + 10, AUTH_CODE_LEN - 1);
    CHECK_INT(shown, AUTH_CODES);
    CHECK_INT(strlen(first_code), 14);
    get(port, "/people/codes.txt", cookie, &r);
    CHECK_HAS(buf_cstr(&r), "Content-Disposition: attachment; filename=\"hemma-recovery-codes.txt\"");
    CHECK_HAS(buf_cstr(&r), first_code);
    CHECK_HAS(buf_cstr(&r), "username: alex");
    get(port, "/people/codes.txt", NULL, &r); /* not for strangers */
    CHECK(!strstr(buf_cstr(&r), first_code));
    get(port, "/setup", cookie, &r); /* signed in: nothing to set up */
    CHECK_HAS(buf_cstr(&r), "Location: /\r\n");

    /* the welcome page is closed for good - also to a crafted POST */
    get(port, "/", NULL, &r);
    CHECK(!strstr(buf_cstr(&r), "action=\"/setup\""));
    CHECK_HAS(buf_cstr(&r), "action=\"/login\"");
    post(port, "/setup", NULL, with_code("name=Mallory&username=mallory&password=takeover-123&again=takeover-123"), &r);
    CHECK_HAS(buf_cstr(&r), "Location: /login?e=4\r\n");
    CHECK(!strstr(buf_cstr(&r), "Set-Cookie"));

    /* "I've saved them" (a plain form POST without JavaScript): the codes are gone from the server */
    form_request(&req, "/a/people.codes_saved", cookie, "http://t", "");
    s = dial(port);
    CHECK(s != SOCK_BAD);
    send_all(s, buf_cstr(&req));
    recv_reply(s, &r);
    CHECK_HAS(buf_cstr(&r), "303");
    get(port, "/people/codes", cookie, &r);
    CHECK_HAS(buf_cstr(&r), "only shown right after");
    CHECK(!strstr(buf_cstr(&r), first_code));
    CHECK(stop_server(port));

    CHECK_INT(account_count(), 1);
    Person *alex = person_by_username("alex");
    CHECK(alex != NULL);
    CHECK_STR(alex->name, "Alex Doe");
    CHECK_INT(alex->role, ROLE_ADMIN);
    CHECK(auth_check("correct horse 1", alex->pass));
    CHECK(alex->last_login > 0);
    CHECK_INT(setting_int("auth.bootstrapped", 0), 1); /* the environment bootstrap won't add another */
    CHECK_STR(app_name(), "Villa Test");
    CHECK_INT(auth_codes_left(alex->id), AUTH_CODES);
    CHECK(person_by_username("mallory") == NULL);
    CHECK_STR(auth_setup_code(), ""); /* used: gone for good */
    CHECK_STR(setting("auth.setup_code", ""), "");
    buf_free(&r);
    buf_free(&req);
    th_halt();
}

TEST(auth_setup_racing_submissions_make_one_admin) {
    th_boot();
    auth_configure("e2e-salt", 1000);
    str_copy(g_code, sizeof g_code, auth_setup_code());
    int port = start_server();
    CHECK(port > 0);
    Buf a = {0}, b = {0}, ra = {0}, rb = {0};
    form_request(&a, "/setup", NULL, NULL, with_code("name=Ann&username=ann&password=first-one-123&again=first-one-123"));
    form_request(&b, "/setup", NULL, NULL, with_code("name=Bo&username=bo&password=second-one-123&again=second-one-123"));
    sock_t sa = dial(port), sb = dial(port);
    CHECK(sa != SOCK_BAD && sb != SOCK_BAD);
    send_all(sa, buf_cstr(&a)); /* both on the wire before either answer is read */
    send_all(sb, buf_cstr(&b));
    recv_reply(sa, &ra);
    recv_reply(sb, &rb);
    CHECK(stop_server(port));
    int won = !!strstr(buf_cstr(&ra), "Set-Cookie: hemma=") + !!strstr(buf_cstr(&rb), "Set-Cookie: hemma=");
    CHECK_INT(won, 1);
    CHECK(strstr(buf_cstr(&ra), "Location: /login?e=4") || strstr(buf_cstr(&rb), "Location: /login?e=4"));
    CHECK_INT(account_count(), 1);
    CHECK((person_by_username("ann") != NULL) != (person_by_username("bo") != NULL));
    buf_free(&a);
    buf_free(&b);
    buf_free(&ra);
    buf_free(&rb);
    th_halt();
}

/* -------------------------------------------------------- recovery codes */

TEST(auth_recovery_codes_work_once) {
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    make_codes(adam, codes);
    CHECK_INT(auth_codes_left(adam), AUTH_CODES);
    for (int i = 0; i < AUTH_CODES; i++) {
        /* 7KQ4-M2XP-9RTA: 12 symbols without 0 O 1 I, in groups of four */
        CHECK_INT(strlen(codes[i]), 14);
        CHECK(codes[i][4] == '-' && codes[i][9] == '-');
        for (int k = 0; k < 14; k++)
            if (k != 4 && k != 9) CHECK(strchr("23456789ABCDEFGHJKLMNPQRSTUVWXYZ", codes[i][k]) && codes[i][k]);
        for (int j = 0; j < i; j++) CHECK(strcmp(codes[i], codes[j]) != 0);
    }
    /* stored only as hashes: not in memory rows, not in the database */
    Table *t = store_table("recovery_code");
    CHECK(t != NULL && t->api_hidden);
    CHECK_INT(tbl_count(t), AUTH_CODES);
    char plain[13];
    for (int i = 0; i < AUTH_CODES; i++) {
        snprintf(plain, sizeof plain, "%.4s%.4s%.4s", codes[i], codes[i] + 5, codes[i] + 10);
        for (int k = 0; k < tbl_count(t); k++) {
            CHECK(!raw_has(tbl_at(t, k), t->rec_size, codes[i]));
            CHECK(!raw_has(tbl_at(t, k), t->rec_size, plain));
        }
        sqlite3_stmt *st = db_prep("SELECT count(*) FROM recovery_code WHERE instr(hash, ?) > 0");
        sqlite3_bind_text(st, 1, plain, -1, SQLITE_TRANSIENT);
        CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
        sqlite3_reset(st);
    }
    /* only the device that asked sees them */
    Session other = {0};
    str_copy(other.token, sizeof other.token, "someone-else");
    other.person = adam;
    char none[AUTH_CODES][AUTH_CODE_LEN];
    CHECK_INT(auth_codes_fresh(&other, none), 0);

    int64_t t0 = 1800000000;
    int who = 0, wait = 0;
    CHECK_INT(auth_recover("Adam", codes[0], 0x0A000001, t0, &who, &wait), LOGIN_OK);
    CHECK_INT(who, adam);
    CHECK_INT(auth_codes_left(adam), AUTH_CODES - 1);
    CHECK_INT(auth_recover("adam", codes[0], 0x0A000001, t0 + 1, &who, &wait), LOGIN_WRONG); /* used up */
    /* typed loosely: lower case, spaces instead of dashes */
    char loose[24];
    snprintf(loose, sizeof loose, " %.4s %.4s %.4s ", codes[1], codes[1] + 5, codes[1] + 10);
    for (char *p = loose; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    CHECK_INT(auth_recover("adam", loose, 0x0A000001, t0 + 2, &who, &wait), LOGIN_OK);
    CHECK_INT(auth_recover("eva", codes[2], 0x0A000001, t0 + 3, &who, &wait), LOGIN_WRONG); /* Adam's, not Eva's */
    /* new codes: the old ones stop working */
    char fresh[AUTH_CODES][AUTH_CODE_LEN];
    make_codes(adam, fresh);
    CHECK_INT(auth_codes_left(adam), AUTH_CODES);
    CHECK_INT(auth_recover("adam", codes[3], 0x0A000001, t0 + 4, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_recover("adam", fresh[0], 0x0A000001, t0 + 5, &who, &wait), LOGIN_OK);
    /* "I've saved them": the plain codes are gone from memory */
    Session mine = {0};
    snprintf(mine.token, sizeof mine.token, "unit-%d", adam);
    mine.person = adam;
    CHECK_INT(auth_codes_fresh(&mine, none), AUTH_CODES);
    auth_codes_seen(&mine);
    CHECK_INT(auth_codes_fresh(&mine, none), 0);
    /* a person removed (or without a sign-in) takes their codes along */
    auth_codes_drop(adam);
    CHECK_INT(auth_codes_left(adam), 0);
    th_halt();
}

TEST(auth_recovery_is_throttled_and_never_tells_who_exists) {
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int tablet = with_login("Hallway", "hallway", "tablet-pass-1", ROLE_DISPLAY);
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    make_codes(adam, codes);
    /* display accounts have no codes */
    Session ts = {0};
    str_copy(ts.token, sizeof ts.token, "tablet");
    ts.person = tablet;
    CHECK(!auth_codes_new(tablet, &ts));
    CHECK_INT(auth_codes_left(tablet), 0);
    int64_t t0 = 1800000000;
    int who = 0, wait = 0;
    for (int i = 0; i < 5; i++)
        CHECK_INT(auth_recover("adam", "AAAA-BBBB-CCCC", 0x0A000002, t0 + i, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_recover("adam", codes[0], 0x0A000002, t0 + 6, &who, &wait), LOGIN_THROTTLED); /* even the right one */
    CHECK(wait > 0 && wait <= 60);
    CHECK_INT(auth_codes_left(adam), AUTH_CODES); /* a refused try uses nothing up */
    /* one budget with signing in */
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000002, t0 + 7, &who, &wait), LOGIN_THROTTLED);
    /* unknown usernames, display accounts and junk look exactly like a wrong code */
    CHECK_INT(auth_recover("nobody", codes[0], 0x0A000003, t0 + 8, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_recover("hallway", codes[0], 0x0A000003, t0 + 9, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_recover("adam", "", 0x0A000004, t0 + 10, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_recover("adam", "7KQ4-M2XP-9RTA-7KQ4-M2XP-9RTA-7KQ4-M2XP-9RTA", 0x0A000004, t0 + 11, &who, &wait),
              LOGIN_WRONG);
    CHECK_INT(auth_recover(NULL, NULL, 0x0A000004, t0 + 12, &who, &wait), LOGIN_WRONG);
    /* the lock is per address (and per account at a higher limit): elsewhere the code works */
    CHECK_INT(auth_recover("adam", codes[0], 0x0A000005, t0 + 13, &who, &wait), LOGIN_OK);
    /* the forgot page reads the same either way */
    CHECK_HAS(th_page(0, "/forgot?e=code&u=nobody"), "That username and recovery code don't match.");
    CHECK_HAS(th_page(0, "/forgot?e=code&u=adam"), "That username and recovery code don't match.");
    auth_configure("", 1000);
    CHECK_INT(auth_recover("adam", codes[1], 0x0A000005, t0 + 14, &who, &wait), LOGIN_UNAVAILABLE);
    th_halt();
}

TEST(auth_forgot_password_end_to_end) {
    if (!module_find("people")) return;
    th_boot();
    auth_configure("e2e-salt", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    char codes[AUTH_CODES][AUTH_CODE_LEN], phone[33], laptop[33];
    make_codes(adam, codes);
    session_create(adam, true, "Mozilla/5.0 (iPhone) Safari/605.1", phone);
    session_create(adam, true, "Mozilla/5.0 (Windows NT 10.0) Chrome/130", laptop);
    int port = start_server();
    CHECK(port > 0);
    Buf r = {0}, form = {0}, req = {0};
    char cookie[64];
    CHECK(signed_in(port, phone));

    get(port, "/login", NULL, &r);
    CHECK_HAS(buf_cstr(&r), "href=\"/forgot\"");
    get(port, "/forgot", NULL, &r);
    CHECK_HAS(buf_cstr(&r), "action=\"/forgot\"");
    CHECK_HAS(buf_cstr(&r), "No codes left?");
    get(port, "/forgot", phone, &r); /* signed in: to the account page */
    CHECK_HAS(buf_cstr(&r), "Location: /people\r\n");

    buf_printf(&form, "username=adam&code=%s&password=brand+new+pw&again=brand+new+pw&remember=1", codes[0]);
    form_request(&req, "/forgot", NULL, "http://evil.example", buf_cstr(&form));
    sock_t s = dial(port);
    CHECK(s != SOCK_BAD);
    send_all(s, buf_cstr(&req));
    recv_reply(s, &r);
    CHECK_HAS(buf_cstr(&r), " 403 ");
    post(port, "/forgot", NULL, "username=adam&code=AAAA-BBBB-CCCC&password=brand+new+pw&again=other+new+pw", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /forgot?e=again\r\n"); /* issue #100: no typed name in the address ... */
    get(port, "/forgot?e=again", NULL, &r);
    CHECK_HAS(buf_cstr(&r), "value=\"adam\""); /* ... the box has it back from the server */
    post(port, "/forgot", NULL, "username=adam&code=AAAA-BBBB-CCCC&password=brand+new+pw&again=brand+new+pw", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /forgot?e=code\r\n");
    post(port, "/forgot", NULL, "username=nobody&code=AAAA-BBBB-CCCC&password=brand+new+pw&again=brand+new+pw", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /forgot?e=code\r\n"); /* the same answer */
    /* a password typed into the name box: in no address, and gone from the box a minute later */
    post(port, "/login", NULL, "username=my+secret+pw&password=x", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /login?e=1&next=");
    CHECK(!strstr(buf_cstr(&r), "secret"));
    get(port, "/login?e=1", NULL, &r);
    CHECK_HAS(buf_cstr(&r), "value=\"my secret pw\"");
    g_fake_now = now_sec() + 61;
    get(port, "/login?e=1", NULL, &r);
    CHECK(!strstr(buf_cstr(&r), "my secret pw"));
    g_fake_now = 0;
    CHECK(signed_in(port, phone)); /* failures change nothing */

    post(port, "/forgot", NULL, buf_cstr(&form), &r);
    CHECK_HAS(buf_cstr(&r), "Location: /people?recovered=1\r\n");
    cookie_of(&r, cookie, sizeof cookie);
    CHECK_INT(strlen(cookie), 32);
    CHECK(signed_in(port, cookie));
    CHECK(!signed_in(port, phone)); /* every other session ended */
    CHECK(!signed_in(port, laptop));
    get(port, "/people?recovered=1", cookie, &r);
    CHECK_HAS(buf_cstr(&r), "You have 7 recovery codes left.");
    post(port, "/forgot", NULL, buf_cstr(&form), &r); /* the same code twice: no */
    CHECK_HAS(buf_cstr(&r), "Location: /forgot?e=code\r\n");
    post(port, "/login", NULL, "username=adam&password=correct+horse", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /login?e=1");
    post(port, "/login", NULL, "username=adam&password=brand+new+pw", &r);
    CHECK_HAS(buf_cstr(&r), "Set-Cookie: hemma=");
    CHECK(stop_server(port));

    CHECK(auth_check("brand new pw", person_get(adam)->pass));
    CHECK_INT(auth_codes_left(adam), AUTH_CODES - 1);
    buf_free(&r);
    buf_free(&form);
    buf_free(&req);
    th_halt();
}

/* ------------------------------------------------- hemma --reset-password */

TEST(auth_cli_reset_password) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    app_set_testing(true);
    CHECK(app_boot(dir));
    auth_configure("pepper", 1000);
    int bob = with_login("Bob", "bob", "old password 1", ROLE_MEMBER);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    char sid[33];
    session_create(bob, true, NULL, sid);
    session_create(bob, false, NULL, sid);
    app_halt();

    /* Hemma stopped: the request waits for the next start */
    Buf out = {0};
    char temp[24];
    CHECK_INT(auth_reset_cli(dir, " BOB ", 0, &out, temp), 0);
    CHECK_INT(strlen(temp), 19); /* xxxx-xxxx-xxxx-xxxx */
    CHECK(auth_password_problem(temp) == NULL);
    CHECK_HAS(buf_cstr(&out), temp);
    CHECK_HAS(buf_cstr(&out), "when Hemma starts");
    CHECK(app_boot(dir));
    CHECK(auth_check(temp, person_by_username("bob")->pass));
    CHECK_INT(tbl_count(&T_session), 0); /* signed out everywhere */
    CHECK(auth_temporary(bob));
    CHECK_HAS(th_page(bob, "/people"), "You signed in with a temporary password");
    /* picking their own password clears the mark */
    char f[160];
    snprintf(f, sizeof f, "current=%s&new=brand+new+pw&again=brand+new+pw", temp);
    CHECK(th_act(bob, "people.password", f).ok);
    CHECK(!auth_temporary(bob));
    CHECK(!strstr(th_page(bob, "/people"), "temporary password"));

    /* Hemma running: another connection's commit wakes the next tick */
    session_create(bob, true, NULL, sid);
    auth_tick(); /* the tick before: nothing new */
    CHECK_INT(auth_reset_cli(dir, "bob", 0, &out, temp), 0);
    CHECK(auth_check("brand new pw", person_by_username("bob")->pass)); /* not yet */
    auth_tick();
    CHECK(auth_check(temp, person_by_username("bob")->pass));
    CHECK_INT(tbl_count(&T_session), 0);
    CHECK(auth_temporary(bob));
    sqlite3_stmt *st = db_prep("SELECT count(*) FROM auth_reset");
    CHECK(st && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0); /* applied once */
    sqlite3_reset(st);

    /* mistakes say what to do */
    CHECK_INT(auth_reset_cli(dir, "nobody", 0, &out, NULL), 1);
    CHECK_HAS(buf_cstr(&out), "No account has the username \"nobody\"");
    CHECK_HAS(buf_cstr(&out), "adam (admin), bob");
    CHECK_INT(auth_reset_cli("no-such-folder/at-all", "bob", 0, &out, NULL), 1);
    CHECK_HAS(buf_cstr(&out), "no Hemma database");
    auth_configure("", 1000);
    CHECK_INT(auth_reset_cli(dir, "bob", 0, &out, NULL), 1);
    CHECK_HAS(buf_cstr(&out), "HEMMA_AUTH_SALT");
    buf_free(&out);
    th_halt();
}

/* The installer's code (HEMMA_SETUP_CODE in the settings file) wins over Hemma's own; one that
   isn't a code is ignored. Never on the page itself. */
TEST(auth_setup_code_from_the_settings_file) {
    th_boot();
    auth_configure("e2e-salt", 1000);
    plat_setenv("HEMMA_SETUP_CODE", "abcd efgh", true);
    CHECK_STR(auth_setup_code(), "ABCD-EFGH");
    plat_setenv("HEMMA_SETUP_CODE", "not-a-code", true);
    const char *own = auth_setup_code();
    CHECK_INT(strlen(own), 9);
    CHECK(strcmp(own, "ABCD-EFGH") != 0);
    char mine[16];
    str_copy(mine, sizeof mine, own);
    const char *page = th_page(0, "/");
    CHECK_HAS(page, "name=\"code\"");
    CHECK_HAS(page, "Where's the setup code?");
    CHECK(!strstr(page, mine));
    char made[16];
    auth_new_setup_code(made);
    CHECK_INT(strlen(made), 9);
    CHECK(made[4] == '-');
    plat_setenv("HEMMA_SETUP_CODE", "", true);
    th_halt();
}
