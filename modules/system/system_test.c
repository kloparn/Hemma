/* modules/system: the log (ring, threads, file, rotation, restarts, download), the checks, the
 * getting-started list, the home card, and that it all stays with the admins. */
#include "../../tests/harness.h"
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../core/tar.h"
#include "system.h"

static LogLevel g_was_level;
static bool g_was_stderr;

/* Lines reach the ring from here on (the terminal stays quiet); the ring starts empty. */
static void logging_on(LogLevel level) {
    g_was_level = g_log_level;
    g_was_stderr = g_log_stderr;
    g_log_stderr = false;
    g_log_level = level;
    system_log_reset();
}

static void logging_off(void) {
    g_log_level = g_was_level;
    g_log_stderr = g_was_stderr;
    system_log_reset();
}

static int admin(const char *name, const char *user, const char *password) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    if (user) {
        str_copy(p.username, sizeof p.username, user);
        auth_hash(password, p.pass, sizeof p.pass);
    }
    tbl_put(&T_person, &p);
    return id;
}

/* A household where everything works: an admin who can sign in, an address on the LAN, room
   on the disk, nothing in the log. */
static int all_well(void) {
    auth_configure("system-test-salt", 1000);
    int adam = admin("Adam", "adam", "correct horse");
    system_test_addresses("http://192.168.1.50:8080", "http://hemma.local:8080");
    system_test_free_space(50LL << 30);
    return adam;
}

/* ------------------------------------------------------------------ log */

static volatile int g_worker_done;
static void worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) log_info("from a worker thread %d", i);
    g_worker_done = 1;
}

TEST(system_log_keeps_lines_from_any_thread) {
    th_boot();
    logging_on(LOG_INFO);
    uint32_t before = system_log_seq();
    log_info("hello from the loop");
    log_debug("not at this level");
    g_worker_done = 0;
    CHECK(thread_spawn(worker, NULL));
    for (int i = 0; i < 1000 && !g_worker_done; i++) plat_sleep_ms(2);
    CHECK(g_worker_done);
    CHECK_INT(system_log_seq() - before, 51);
    LogLine l[4];
    int n = 0;
    CHECK_INT(system_log_find(l, 4, &n, false, "thread 49"), 1);
    CHECK_INT(n, 1);
    CHECK_STR(l[0].text, "from a worker thread 49");
    CHECK(l[0].level == 'I' && !l[0].old);
    system_log_free(l, n);
    CHECK_INT(system_log_find(l, 4, &n, false, "not at this level"), 0);
    /* control characters were cleaned on the way in: one line stays one line */
    log_warn("a visitor typed \"%s\"", "x\n12:00:00 E forged");
    CHECK_INT(system_log_find(l, 4, &n, false, "forged"), 1);
    CHECK_STR(l[0].text, "a visitor typed \"x\\n12:00:00 E forged\"");
    system_log_free(l, n);
    logging_off();
    th_halt();
}

TEST(system_log_ring_keeps_the_newest) {
    th_boot();
    logging_on(LOG_INFO);
    for (int i = 0; i < SYSTEM_LOG_RING + 150; i++) log_info("line %d", i);
    CHECK_INT(system_log_count(), SYSTEM_LOG_RING);
    static LogLine all[SYSTEM_LOG_RING];
    int n = 0;
    CHECK_INT(system_log_find(all, SYSTEM_LOG_RING, &n, false, NULL), SYSTEM_LOG_RING);
    CHECK_INT(n, SYSTEM_LOG_RING);
    CHECK_STR(all[0].text, "line 2149"); /* newest first */
    CHECK_STR(all[n - 1].text, "line 150"); /* the oldest 150 made room */
    CHECK(all[0].seq == all[1].seq + 1);
    system_log_free(all, n);
    CHECK_INT(system_log_find(all, 4, &n, false, "line 0"), 0);
    system_log_free(all, n);
    logging_off();
    th_halt();
}

TEST(system_log_filters_and_searches) {
    th_boot();
    int adam = all_well();
    logging_on(LOG_INFO);
    log_info("Library scan done: 178 episodes");
    log_warn("Disk nearly FULL on /srv/media");
    log_error("Saving failed: disk I/O error");
    log_info("Ångström says <script>alert(1)</script>");
    LogLine l[8];
    int n = 0;
    CHECK_INT(system_log_find(l, 8, &n, true, NULL), 2); /* warnings and problems */
    CHECK(n == 2 && l[0].level == 'E' && l[1].level == 'W');
    system_log_free(l, n);
    CHECK_INT(system_log_find(l, 8, &n, false, "full"), 1); /* any case */
    system_log_free(l, n);
    CHECK_INT(system_log_find(l, 8, &n, false, "ÅNGSTRÖM"), 1); /* Swedish letters too */
    system_log_free(l, n);
    CHECK_INT(system_log_find(l, 8, &n, true, "saving"), 1);
    system_log_free(l, n);
    CHECK_INT(system_log_find(l, 8, &n, true, "library"), 0);
    system_log_free(l, n);

    const char *html = th_page(adam, "/system/log?f=warn");
    CHECK_HAS(html, "Saving failed");
    CHECK(!strstr(html, "Library scan done"));
    CHECK_HAS(html, "class=\"chip on\""); /* "Warnings & problems" */
    html = th_page(adam, "/system/log?q=library");
    CHECK_HAS(html, "Library scan done");
    CHECK(!strstr(html, "Saving failed"));
    html = th_page(adam, "/system/log");
    CHECK_HAS(html, "&lt;script&gt;alert(1)&lt;/script&gt;"); /* lines are text, never markup */
    CHECK(!strstr(html, "<script>alert"));
    CHECK_HAS(html, "data-live=\"system.search\"");
    CHECK_HAS(html, "href=\"/system/log.txt\"");
    CHECK_HAS(html, "4 lines");
    logging_off();
    th_halt();
}

TEST(system_detailed_logging_is_an_admins_switch) {
    th_boot();
    int adam = all_well(), eva = th_person("Eva");
    logging_on(LOG_INFO);
    CHECK(!th_act(eva, "system.verbose", "on=1").ok);
    CHECK(g_log_level == LOG_INFO);
    CHECK(th_act(adam, "system.verbose", "on=1").ok);
    CHECK(g_log_level == LOG_DEBUG);
    CHECK_HAS(th_page(adam, "/system/log"), "data-auto checked");
    log_debug("a detail for the curious");
    LogLine l[2];
    int n = 0;
    CHECK_INT(system_log_find(l, 2, &n, false, "for the curious"), 1);
    CHECK(l[0].level == 'D');
    system_log_free(l, n);
    CHECK(th_act(adam, "system.verbose", "").ok);
    CHECK(g_log_level != LOG_DEBUG);
    logging_off();
    th_halt();
}

TEST(system_log_file_rotates_and_survives_a_restart) {
    th_boot();
    char dir[600], path[700], old[720];
    str_copy(dir, sizeof dir, app_config()->data_dir);
    snprintf(path, sizeof path, "%s/hemma.log", dir);
    snprintf(old, sizeof old, "%s.1", path);
    logging_on(LOG_INFO);
    log_info("written to the file");
    system_log_tick(now_ms(), true);
    Buf f = {0};
    CHECK(file_read_all(path, &f, 1 << 20));
    CHECK_HAS(buf_cstr(&f), " I written to the file\n");
    buf_free(&f);

    /* past its limit the file becomes hemma.log.1 */
    system_log_set_limit(700);
    for (int i = 0; i < 40; i++) {
        log_info("filler line %02d, long enough to fill the file", i);
        if (i % 5 == 4) system_log_tick(now_ms(), true);
    }
    int64_t size = 0;
    CHECK(path_stat(old, &size, NULL, NULL));
    CHECK(path_stat(path, &size, NULL, NULL) && size > 0 && size <= 700);
    log_warn("the last words");

    /* a restart: the new process reads the end of the file back, behind a divider */
    LogLevel level = g_log_level;
    g_log_level = LOG_QUIET; /* (so the ring holds only what came from the file) */
    th_halt();               /* writes the last words and a "stopped" mark */
    system_log_reset();      /* a new process remembers nothing */
    app_set_testing(true);
    CHECK(app_boot(dir));
    LogLine l[4];
    int n = 0;
    system_log_find(l, 4, &n, false, NULL);
    CHECK_INT(n, 4);
    CHECK(l[0].level == '-' && !l[0].old);
    CHECK_STR(l[0].text, "Hemma restarted");
    CHECK(l[1].level == '-' && l[1].old);
    CHECK_STR(l[1].text, "Hemma stopped");
    CHECK(l[2].level == 'W' && l[2].old);
    CHECK_STR(l[2].text, "the last words");
    CHECK_STR(l[3].text, "filler line 39, long enough to fill the file");
    system_log_free(l, n);
    CHECK(!system_log_unclean(NULL)); /* it said goodbye */
    g_log_level = level;

    int adam = all_well();
    const char *html = th_page(adam, "/system/log");
    CHECK_HAS(html, "Hemma restarted");
    CHECK_HAS(html, "system-old");
    /* the download holds both files */
    Buf all = {0};
    system_log_text(&all);
    CHECK_HAS(buf_cstr(&all), "W the last words");
    CHECK_HAS(buf_cstr(&all), "filler line 25");
    CHECK_HAS(buf_cstr(&all), "- Hemma stopped");
    buf_free(&all);
    system_log_set_limit(0);
    logging_off();
    th_halt();
}

TEST(system_log_notices_a_run_that_never_said_goodbye) {
    th_boot();
    char dir[600], path[700];
    str_copy(dir, sizeof dir, app_config()->data_dir);
    snprintf(path, sizeof path, "%s/hemma.log", dir);
    th_halt();
    static const char crashed[] = "2026-10-12 10:00:00 - Hemma 0.2.0 started\n"
                                  "2026-10-12 10:05:00 I serving\n"
                                  "2026-10-12 10:06:00 E store: writing person failed: disk I/O error\n";
    CHECK(file_write_atomic(path, crashed, sizeof crashed - 1));
    system_log_reset();
    app_set_testing(true);
    CHECK(app_boot(dir));
    int64_t last = 0;
    CHECK(system_log_unclean(&last));
    CHECK(last == dt_make(2026, 10, 12, 10, 6));
    LogLine l[2];
    int n = 0;
    system_log_find(l, 2, &n, false, NULL);
    CHECK_INT(n, 2);
    CHECK(l[0].level == '-');
    CHECK_HAS(l[0].text, "stopped without warning");
    CHECK(l[1].level == 'E' && l[1].old && l[1].ts == last);
    system_log_free(l, n);
    CHECK_INT(system_log_errors_since(0), 0); /* the last run's problems aren't this run's */
    int adam = all_well();
    CHECK_HAS(th_page(adam, "/system"), "it stopped without warning");
    th_halt();
}

/* ------------------------------------------------------------- download */

static volatile int g_done;
static void serve(void *arg) {
    (void)arg;
    app_serve();
    g_done = 1;
}

static void request(int port, const char *raw, Buf *out) {
    buf_reset(out);
    sock_t s = tcp_connect_nb(0x7F000001, port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) {
        if (s != SOCK_BAD) sock_close(s);
        return;
    }
    for (size_t n = strlen(raw); n;) {
        long k = sock_send(s, raw, n);
        if (k > 0) raw += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
    int64_t end = now_ms() + 4000;
    while (now_ms() < end) { /* Connection: close - until the server hangs up */
        pollfd_t in = {.fd = s, .events = POLLIN};
        if (plat_poll(&in, 1, 50) != 1) continue;
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

TEST(system_log_downloads_for_admins_only) {
    th_boot();
    logging_on(LOG_INFO);
    int adam = admin("Adam", NULL, NULL), eva = th_person("Eva");
    char ca[33], ce[33], req[300];
    CHECK(session_create(adam, false, "test", ca));
    CHECK(session_create(eva, false, "test", ce));
    log_info("a line worth downloading");
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    Buf r = {0};
    snprintf(req, sizeof req, "GET /system/log.txt HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             ca);
    request(port, req, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 OK\r\n"));
    CHECK_HAS(buf_cstr(&r), "Content-Type: text/plain; charset=utf-8\r\n");
    CHECK_HAS(buf_cstr(&r), "Content-Disposition: attachment; filename=\"hemma-log-");
    CHECK_HAS(body_of(&r), " I a line worth downloading\n");
    snprintf(req, sizeof req, "GET /system/log.txt HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n",
             ce);
    request(port, req, &r);
    CHECK_HAS(buf_cstr(&r), "HTTP/1.1 200 ");
    CHECK(!strstr(buf_cstr(&r), "Content-Disposition"));
    CHECK_HAS(body_of(&r), "Nothing here"); /* a member: as if there were no such thing */
    CHECK(!strstr(body_of(&r), "worth downloading"));
    request(port, "GET /system/log.txt HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &r);
    CHECK(!strstr(buf_cstr(&r), "Content-Disposition"));
    CHECK(!strstr(body_of(&r), "worth downloading")); /* signed out */
    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&r);
    logging_off();
    th_halt();
}

/* --------------------------------------------------------------- checks */

TEST(system_checks_say_what_works_and_what_doesnt) {
    th_boot();
    int adam = all_well();
    const char *html = th_page(adam, "/system");
    CHECK_HAS(html, "Everything works");
    CHECK_HAS(html, "http://192.168.1.50:8080");
    char version[64];
    snprintf(version, sizeof version, "Version %s", HEMMA_VERSION);
    CHECK_HAS(html, version);
    CHECK_HAS(html, "1 account can sign in, 1 of them admin.");

    /* started with another secret than the passwords were made with */
    setting_set("auth.salt_check", "0123456789abcdef");
    html = th_page(adam, "/system");
    CHECK_HAS(html, "Something isn't working");
    CHECK_HAS(html, "the one the passwords were made with");
    CHECK_HAS(html, "badge bad");
    setting_set("auth.salt_check", "");

    /* the disk with the data fills up */
    system_test_free_space(1LL << 30);
    html = th_page(adam, "/system");
    CHECK_HAS(html, "1 thing below needs a look");
    CHECK_HAS(html, "1.0 GB free on the disk with Hemma");
    system_test_free_space(100LL << 20);
    CHECK_HAS(th_page(adam, "/system"), "Something isn't working");
    system_test_free_space(50LL << 30);

    /* no address on the home network */
    system_test_addresses("", NULL);
    CHECK_HAS(th_page(adam, "/system"), "can&#39;t find its address on the home network");
    system_test_addresses("http://192.168.1.50:8080", NULL);

    /* a problem in the log */
    logging_on(LOG_INFO);
    log_error("store: writing person failed: disk I/O error");
    html = th_page(adam, "/system");
    CHECK_HAS(html, "1 problem logged in the last day.");
    CHECK_HAS(html, "href=\"/system/log?f=warn\"");
    logging_off();
    CHECK_HAS(th_page(adam, "/system"), "Everything works");

    /* nobody can sign in as an admin yet: the welcome page is open */
    Person p = *person_get(adam);
    p.pass[0] = '\0';
    tbl_put(&T_person, &p);
    CHECK_HAS(th_page(adam, "/system"), "The welcome page is still open");

    /* no secret at all */
    auth_configure("", 1000);
    CHECK_HAS(th_page(adam, "/system"), "nobody can sign in or be added");
    th_halt();
}

TEST(system_getting_started_ticks_itself) {
    th_boot();
    int adam = all_well();
    bool media = module_find("media") != NULL;
    int steps = media ? 6 : 5;
    char want[40];
    const char *html = th_page(adam, "/system");
    CHECK_HAS(html, "Getting started");
    snprintf(want, sizeof want, "0 of %d done", steps);
    CHECK_HAS(html, want);
    /* the admin's home page points at the next step; members see none of it */
    html = th_page(adam, "/");
    CHECK_HAS(html, "id=\"system-start\"");
    CHECK_HAS(html, "Next: <a href=\"/people\"><b>Name your home</b></a>");
    setting_set("household.name", "Villa Kulla");
    snprintf(want, sizeof want, "1 of %d done", steps);
    CHECK_HAS(th_page(adam, "/system"), want);
    int eva = th_person("Eva");
    CHECK(!strstr(th_page(eva, "/"), "system-start"));
    snprintf(want, sizeof want, "2 of %d done", steps);
    CHECK_HAS(th_page(adam, "/system"), want);
    char sid[33];
    CHECK(session_create(adam, true,
                         "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like "
                         "Gecko) Version/17.0 Mobile/15E148 Safari/604.1",
                         sid));
    snprintf(want, sizeof want, "3 of %d done", steps);
    CHECK_HAS(th_page(adam, "/system"), want);
    CHECK(auth_codes_new(adam, NULL));
    snprintf(want, sizeof want, "4 of %d done", steps);
    CHECK_HAS(th_page(adam, "/system"), want);
    CHECK(!th_act(eva, "system.salt_saved", "").ok); /* only you know you kept a copy - and only admins say so */
    CHECK_HAS(th_page(adam, "/system"), "data-a=\"system.salt_saved\"");
    CHECK(th_act(adam, "system.salt_saved", "").ok);
    snprintf(want, sizeof want, "5 of %d done", steps);
    CHECK_HAS(th_page(adam, "/system"), want);
    if (media) { /* the media module's step, through its health hook */
        char form[700];
        snprintf(form, sizeof form, "roots=%s", test_tmpdir());
        CHECK(th_act(adam, "media.roots", form).ok);
    }
    html = th_page(adam, "/system");
    snprintf(want, sizeof want, "%d of %d done", steps, steps);
    CHECK_HAS(html, want);
    CHECK_HAS(html, "All done");
    CHECK(!strstr(th_page(adam, "/"), "id=\"system-start\"")); /* all done: no card at home */
    CHECK(th_act(adam, "system.start_hide", "").ok);
    html = th_page(adam, "/system");
    CHECK(!strstr(html, "Getting started"));
    CHECK_HAS(html, "Show the getting-started list again");
    CHECK(!th_act(eva, "system.start_show", "").ok);
    CHECK(th_act(adam, "system.start_show", "").ok);
    CHECK_HAS(th_page(adam, "/system"), "Getting started");
    th_halt();
}

/* Listening on this computer only: plain words on home and System, the flag folded away (issue #62). */
TEST(system_address_check_speaks_plainly) {
    th_boot();
    int adam = all_well();
    system_log_reset();
    app_test_host("127.0.0.1");
    const char *html = th_page(adam, "/");
    CHECK_HAS(html, "Let the rest of the home in");
    CHECK(!strstr(html, "--host"));
    html = th_page(adam, "/system");
    CHECK_HAS(html, "Only this computer can open Hemma: phones and the TV can&#39;t reach it.");
    CHECK_HAS(html, "<summary>Technical details</summary><span>It listens on 127.0.0.1. Start it without --host");
    app_test_host(NULL);
    CHECK(!strstr(th_page(adam, "/system"), "Technical details</summary><span>It listens"));
    th_halt();
}

TEST(system_home_card_only_when_something_needs_a_look) {
    th_boot();
    int adam = all_well(), eva = th_person("Eva");
    system_log_reset();
    CHECK(!strstr(th_page(adam, "/"), "system-widget")); /* all is well: nothing */
    setting_set("auth.salt_check", "0123456789abcdef");
    const char *html = th_page(adam, "/");
    CHECK_HAS(html, "id=\"system-widget\"");
    CHECK_HAS(html, "Something isn&#39;t working");
    CHECK_HAS(html, "Put the original line back into");
    CHECK(!strstr(html, "HEMMA_AUTH_SALT")); /* plain words on the home card (issue #62) ... */
    html = th_page(adam, "/system"); /* ... the variable folded away on System */
    CHECK_HAS(html, "The password secret (a line in the settings file) isn&#39;t the one");
    CHECK_HAS(html, "<details class=\"system-tech\"><summary>Technical details</summary><span>The secret is the line "
                    "HEMMA_AUTH_SALT=");
    CHECK(!strstr(th_page(eva, "/"), "system-widget")); /* members never see it */
    setting_set("auth.salt_check", "");
    system_test_free_space(1LL << 30);
    CHECK_HAS(th_page(adam, "/"), "Something needs a look");
    system_test_free_space(50LL << 30);
    CHECK(!strstr(th_page(adam, "/"), "system-widget"));
    th_halt();
}

TEST(system_says_when_the_database_was_rescued) {
    /* issue #37: a damaged database used to start with what happened to be readable, and nothing said */
    char dir[512], path[600];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(path, sizeof path, "%s/hemma.db", dir);
    th_boot_in(dir);
    int adam = all_well(), eva = th_person("Eva");
    for (int i = 0; i < 300; i++)
        activity_add(adam, "📌", "", "news-line-%03d: something happened at home, told at some length so that "
                                     "the lines fill a good number of the database's pages", i);
    th_halt(); /* (a clean stop: Hemma notes how many rows there are) */
    /* the page holding line 150 turns to rubbish, as an SD card does after a power cut */
    Buf f = {0};
    CHECK(file_read_all(path, &f, 64u << 20));
    size_t at = 0, n = strlen("news-line-150:");
    while (at + n <= f.len && memcmp(f.p + at, "news-line-150:", n) != 0) at++;
    CHECK(at + n <= f.len);
    memset(f.p + at / 4096 * 4096, 0xA5, 4096);
    CHECK(file_write_atomic(path, f.p, f.len));
    th_boot_in(dir);
    auth_configure("system-test-salt", 1000);
    const char *h = th_page(adam, "/system");
    CHECK_HAS(h, "<b>Database rescued</b>");
    CHECK_HAS(h, "Nothing was deleted: the damaged file is kept as hemma.db.damaged-");
    CHECK_HAS(h, " rows were kept - news on Home: "); /* (issue #88: in the household's words) */
    CHECK_HAS(h, "system.rescued_seen");
    CHECK_HAS(th_page(adam, "/"), "Hemma rescued what it could still read");
    CHECK(!strstr(th_page(eva, "/"), "rescued what it could")); /* members: nothing they can do about it */
    CHECK(!th_act(eva, "system.rescued_seen", "").ok);
    CHECK(th_act(adam, "system.rescued_seen", "").ok);
    CHECK(!strstr(th_page(adam, "/system"), "Database rescued"));
    CHECK(!strstr(th_page(adam, "/"), "rescued what it could"));
    th_halt();
    buf_free(&f);
}

TEST(system_watches_the_time_zone) {
    /* issue #39: a Pi left on UTC for a week and then corrected showed that week's bookings hours off -
       and nothing anywhere said which zone the clock was in */
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    zone_test_sig("+00:00");
    th_boot_in(dir);
    int adam = all_well();
    const char *h = th_page(adam, "/system");
    CHECK_HAS(h, "in UTC all year");
    CHECK_HAS(h, "sudo raspi-config");
    th_halt();
    zone_test_sig("+01:00/+02:00"); /* set to Stockholm, and Hemma restarted */
    th_boot_in(dir);
    all_well();
    h = th_page(adam, "/system");
    CHECK_HAS(h, "The time zone changed on ");
    CHECK_HAS(h, "from UTC+00:00 all year to UTC+01:00 in January, UTC+02:00 in July");
    CHECK_HAS(h, "Nothing typed before then shows at another time."); /* (a household with nothing in it yet) */
    CHECK(!strstr(h, "system.tz_retime"));
    CHECK_HAS(h, "system.tz_seen");
    CHECK(th_act(adam, "system.tz_seen", "").ok);
    h = th_page(adam, "/system");
    CHECK(!strstr(h, "The time zone changed"));
    CHECK_HAS(h, "(UTC+01:00 in January, UTC+02:00 in July)");
    th_halt(); /* the same zone at the next start: nothing new */
    th_boot_in(dir);
    all_well();
    CHECK(!strstr(th_page(adam, "/system"), "The time zone changed"));
    zone_test_sig("");
    th_halt();
}

TEST(system_is_only_for_admins) {
    th_boot();
    int adam = all_well(), eva = th_person("Eva");
    CHECK(!strstr(th_page(eva, "/"), "href=\"/system\""));
    CHECK_HAS(th_page(eva, "/system"), "Nothing here");
    CHECK(!strstr(th_page(eva, "/system/log"), "id=\"system-log\""));
    CHECK(!strstr(th_page(eva, "/system/help"), "Where things are"));
    CHECK_HAS(th_page(adam, "/"), "href=\"/system\"");
    CHECK_HAS(th_page(adam, "/system/log"), "id=\"system-log\"");
    CHECK_HAS(th_page(adam, "/system/nope"), "Nothing here");
    const Module *m = module_find("system");
    CHECK(m && (m->flags & MOD_ADMIN));
    for (const Action *a = m->actions; a->name; a++) /* only the search box runs from "live" messages */
        CHECK(a->live == str_eq(a->name, "system.search"));
    th_halt();
}

TEST(system_help_says_where_this_hemma_keeps_things) {
    th_boot();
    int adam = all_well();
    const char *html = th_page(adam, "/system/help");
    CHECK_HAS(html, app_config()->data_dir);
    CHECK_HAS(html, "/hemma.log");
    CHECK_HAS(html, "Where things are");
    CHECK_HAS(html, "Download the log");
    char hemma[64];
    snprintf(hemma, sizeof hemma, "Hemma %s", HEMMA_VERSION);
    CHECK_HAS(html, hemma);
    CHECK_HAS(html, "http://192.168.1.50:8080/healthz");
    th_halt();
}

/* --------------------------------------------------------- backups (issue #35) */

static bool file_there(const char *dir, const char *name) {
    char p[800];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return path_stat(p, NULL, NULL, NULL);
}

/* The backup in folder bk made today (its name), "" = none. */
static void todays(const char *bk, char out[40]) {
    char day[11];
    fmt_ymd(now_sec(), day);
    out[0] = '\0';
    DirIter *it = dir_open(bk);
    DirEntry e;
    while (it && dir_next(it, &e))
        if (str_starts(e.name, "hemma-") && strncmp(e.name + 6, day, 10) == 0 && str_ends(e.name, ".db"))
            snprintf(out, 40, "%s", e.name);
    dir_close(it);
}

TEST(system_backs_up_and_keeps_seven_days) {
    char dir[512], bk[600], p[700], name[40];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(bk, sizeof bk, "%s/backups", dir);
    th_boot_in(dir);
    int adam = all_well(), eva = th_person("Eva");
    /* a library folder on the same disk as the data: not where backups go */
    setting_set("media.roots", test_tmpdir());
    /* older backups (made up: only their names count), nine days and a second one on the last day */
    CHECK(dir_make(bk));
    for (int d = 1; d <= 9; d++) {
        snprintf(p, sizeof p, "%s/hemma-2026-09-%02d-0330.db", bk, d);
        CHECK(file_write_atomic(p, "x", 1));
    }
    snprintf(p, sizeof p, "%s/hemma-2026-09-09-0100.db", bk);
    CHECK(file_write_atomic(p, "x", 1));
    snprintf(p, sizeof p, "%s/hemma-2026-09-09-0330.db.part", bk);
    CHECK(file_write_atomic(p, "x", 1));
    snprintf(p, sizeof p, "%s/my-own-notes.db", bk);
    CHECK(file_write_atomic(p, "x", 1));

    CHECK(!th_act(eva, "system.backup_now", "").ok); /* (admins only) */
    CHECK(th_act(adam, "system.backup_now", "").ok);
    todays(bk, name);
    CHECK(name[0]);
    /* today + the six newest days before it stay; the newest of each day */
    for (int d = 4; d <= 9; d++) {
        snprintf(p, sizeof p, "hemma-2026-09-%02d-0330.db", d);
        CHECK(file_there(bk, p));
    }
    CHECK(!file_there(bk, "hemma-2026-09-03-0330.db"));
    CHECK(!file_there(bk, "hemma-2026-09-01-0330.db"));
    CHECK(!file_there(bk, "hemma-2026-09-09-0100.db"));
    CHECK(!file_there(bk, "hemma-2026-09-09-0330.db.part"));
    CHECK(file_there(bk, "my-own-notes.db")); /* (never anything Hemma didn't name) */

    const char *h = th_page(adam, "/system/backups");
    CHECK_HAS(h, "Backed up and checked: hemma-");
    CHECK_HAS(h, "href=\"/system/backups/hemma-2026-09-09-0330.tar\"");
    CHECK_HAS(h, "data-a=\"system.backup_restore\"");
    CHECK_HAS(h, "/backups</code>");
    h = th_page(adam, "/system");
    CHECK_HAS(h, "<b>Backups</b>");
    CHECK_HAS(h, "(checked, ");
    CHECK_HAS(h, ", 7 kept in ");
    CHECK(!strstr(th_page(eva, "/system/backups"), "system.backup_now")); /* members: no System page */
    th_halt();
}

TEST(system_goes_back_to_a_backup) {
    char dir[512], bk[600], name[40], form[80];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(bk, sizeof bk, "%s/backups", dir);
    th_boot_in(dir);
    int adam = all_well();
    CHECK(th_act(adam, "system.backup_now", "").ok);
    todays(bk, name);
    CHECK(name[0]);
    th_person("Leo"); /* (after the backup) */
    snprintf(form, sizeof form, "name=%s", name);
    CHECK(!th_act(adam, "system.backup_restore", "name=hemma-2026-01-01-0000.db").ok); /* not in the list */
    CHECK(th_act(adam, "system.backup_restore", form).ok);
    const char *h = th_page(adam, "/system/backups");
    CHECK_HAS(h, "waits to be put back.</b> Restart Hemma");
    CHECK_HAS(h, "system.backup_cancel");
    CHECK_HAS(th_page(adam, "/system"), "<b>Going back to a backup</b>");
    CHECK(th_act(adam, "system.backup_cancel", "").ok);
    CHECK(!strstr(th_page(adam, "/system/backups"), "waits to be put back"));
    /* again - and this time Hemma starts again: the backup is back, without Leo */
    CHECK(th_act(adam, "system.backup_restore", form).ok);
    th_halt();
    th_boot_in(dir);
    auth_configure("system-test-salt", 1000);
    bool leo = false;
    for (int i = 0; i < person_count(); i++) leo |= str_eq(person_at(i)->name, "Leo");
    CHECK(!leo);
    h = th_page(adam, "/system");
    CHECK_HAS(h, "<b>Backup put back</b>");
    CHECK_HAS(h, "was put back (");
    CHECK_HAS(th_page(adam, "/system/backups"), "system.restored_seen");
    CHECK(th_act(adam, "system.restored_seen", "").ok);
    CHECK(!strstr(th_page(adam, "/system"), "Backup put back"));
    th_halt();
}

TEST(system_backup_that_does_not_check_out_is_not_put_back) {
    char dir[512], bk[600], p[700];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(bk, sizeof bk, "%s/backups", dir);
    th_boot_in(dir);
    int adam = all_well();
    CHECK(th_act(adam, "system.backup_now", "").ok);
    snprintf(p, sizeof p, "%s/hemma-2026-09-01-0330.db", bk);
    CHECK(file_write_atomic(p, "x", 1));
    CHECK(th_act(adam, "system.backup_now", "").ok); /* (lists it) */
    CHECK(th_act(adam, "system.backup_restore", "name=hemma-2026-09-01-0330.db").ok);
    const char *h = th_page(adam, "/system/backups");
    CHECK_HAS(h, "Not put back: that backup can&#39;t be used");
    CHECK(!strstr(h, "waits to be put back"));
    th_halt();
}

TEST(system_backup_download_carries_the_secret) {
    char dir[512], bk[600], name[40], path[700], tarp[700];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(bk, sizeof bk, "%s/backups", dir);
    th_boot_in(dir);
    int adam = all_well();
    CHECK(th_act(adam, "system.backup_now", "").ok);
    todays(bk, name);
    snprintf(path, sizeof path, "%s/%s", bk, name);
    Buf out = {0}, db = {0};
    CHECK(system_backup_bundle(path, "s3cret-salt", 1760000000, &out));
    const void *p;
    size_t n;
    CHECK(file_read_all(path, &db, 64u << 20));
    CHECK(tar_find(out.p, out.len, "hemma.db", &p, &n) && n == db.len && memcmp(p, db.p, n) == 0);
    CHECK(tar_find(out.p, out.len, "hemma-secret.txt", &p, &n));
    Buf txt = {0};
    buf_put(&txt, p, n);
    CHECK_HAS(txt.p, "SECRET - keep this file like a password.");
    CHECK_HAS(txt.p, "\nHEMMA_AUTH_SALT=s3cret-salt\n");
    /* ... and hemma --restore reads the secret back on a new install */
    snprintf(tarp, sizeof tarp, "%s/download.tar", dir);
    CHECK(file_write_atomic(tarp, out.p, out.len));
    char secret[64], why[300];
    CHECK(store_stage_restore(test_tmpdir(), tarp, "download.tar", secret, sizeof secret, why, sizeof why));
    CHECK_STR(secret, "s3cret-salt");
    buf_free(&txt);
    buf_free(&db);
    buf_free(&out);
    th_halt();
}

TEST(system_backup_due_at_night_or_when_late) {
    int64_t night = dt_make(2026, 10, 17, 3, 30), noon = dt_make(2026, 10, 17, 12, 0);
    CHECK(!system_backup_due(noon, 0, noon - 60));          /* just started: not yet */
    CHECK(system_backup_due(noon, 0, noon - 200));          /* none yet: a few minutes after the start */
    CHECK(!system_backup_due(noon, noon - 21 * 3600, 0));   /* 21 h old at noon: wait for the night */
    CHECK(system_backup_due(night, night - 21 * 3600, 0));  /* ... at 03:xx */
    CHECK(!system_backup_due(night, night - 3600, 0));      /* made an hour ago */
    CHECK(system_backup_due(noon, noon - 27 * 3600, 0));    /* a night missed (the computer was off) */
}

TEST(system_backup_folder_can_be_chosen) {
    char dir[512], mine[600], name[40];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(mine, sizeof mine, "%s/elsewhere", test_tmpdir());
    th_boot_in(dir);
    int adam = all_well();
    CHECK(!th_act(adam, "system.backup_dir", "dir=relative%2Fpath").ok); /* (the whole path, or Hemma chooses) */
    CHECK(th_act(adam, "system.backup_dir", "dir=%2Fsrv%2Fhemma-backups%2F").ok);
    CHECK_STR(setting(SET_BACKUP_DIR, ""), "/srv/hemma-backups"); /* (no trailing slash) */
    CHECK(th_act(adam, "system.backup_dir", "dir=").ok);
    CHECK_STR(setting(SET_BACKUP_DIR, "x"), "");
    /* the next backup goes to the folder chosen (here the test's own, which may be a relative path) */
    setting_set(SET_BACKUP_DIR, mine);
    CHECK(th_act(adam, "system.backup_now", "").ok);
    todays(mine, name);
    CHECK(name[0]);
    th_halt();
}

/* ------------------------------------------------------- one row per disk (issue #95) */

static HealthDisk g_disks[2];
static int g_ndisks;
static void disky_health(Ctx *c, Health *h) {
    for (int i = 0; i < g_ndisks; i++) health_disk(h, &g_disks[i]);
}
static const Module disky = {.id = "zzdisky", .title = "Disky", .icon = "settings", .nav = 998, .flags = MOD_ADMIN,
                             .health = disky_health};

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

/* The library and the data on one disk said "OK" and "nearly full" at once - two checks, two rules,
   two roundings. Now: one row per disk, naming what's on it, the numbers it compares. */
TEST(system_says_disk_space_once_per_disk) {
    static bool registered;
    if (!registered) module_register(&disky);
    registered = true;
    th_boot();
    int adam = all_well();
    g_ndisks = 1;
    g_disks[0] = (HealthDisk){.what = "the test library", .path = "/srv/test", .free = 3LL << 30, .total = 100LL << 30,
                              .data_disk = true, .want = 7LL << 30, .why = "to hold a test (6.0 GB)",
                              .todo = "Make room for the test.", .link = "/zzdisky"};
    system_test_free_space(3LL << 30);
    const char *h = th_page(adam, "/system");
    CHECK_INT(count_of(h, "<b>Disk space"), 1);
    CHECK_HAS(h, "3.0 GB free on the disk with Hemma&#39;s data and the test library (97% used). The test library "
                 "wants 7.0 GB free to hold a test (6.0 GB).");
    CHECK_HAS(h, "Make room for the test.");
    CHECK_HAS(h, "1 thing below needs a look");
    /* room enough: one OK row, the same words */
    system_test_free_space(12LL << 30);
    h = th_page(adam, "/system");
    CHECK_INT(count_of(h, "<b>Disk space"), 1);
    CHECK_HAS(h, "12.0 GB free on the disk with Hemma&#39;s data and the test library");
    CHECK_HAS(h, "Everything works");
    /* the data's own rule still holds on the shared disk: under 300 MB, saving fails soon */
    system_test_free_space(100LL << 20);
    CHECK_HAS(th_page(adam, "/system"), "Something isn't working");
    system_test_free_space(50LL << 30);
    /* a disk of its own: a row of its own */
    g_disks[0].data_disk = false;
    g_disks[0].free = 1700LL << 30;
    g_disks[0].total = 2000LL << 30;
    h = th_page(adam, "/system");
    CHECK_INT(count_of(h, "<b>Disk space"), 2);
    CHECK_HAS(h, "<b>Disk space (/srv/test)</b>");
    CHECK_HAS(h, "1.7 TB free on the disk with the test library (15% used).");
    g_ndisks = 0;
    th_halt();
}

/* Issue #39, part 2: after the change the System page says what moving typed times back would do, an admin
   chooses, and it's done once */
TEST(system_moves_typed_times_back_when_asked) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    plat_set_tz("UTC0");
    g_fake_now = dt_make(2026, 10, 7, 10, 0);
    th_boot_in(dir);
    int adam = all_well(), eva = th_person("Eva");
    CHECK(th_act(adam, "plan.event_add", "q=dentist+13%2F10+14%3A30").ok);
    CHECK(th_act(adam, "plan.event_add", "q=midsummer+party+24%2F10").ok);
    th_halt();
    plat_set_tz("XXX-2");
    g_fake_now += 3600;
    th_boot_in(dir);
    all_well();
    const char *h = th_page(adam, "/system");
    CHECK_HAS(h, "kept their days (1)");
    CHECK_HAS(h, "1 booking or event typed before then shows shifted by the difference");
    CHECK_HAS(h, "Dentist · Tue 13 Oct 16:30 → 14:30");
    CHECK_HAS(h, "system.tz_retime");
    CHECK(!th_act(eva, "system.tz_retime", "").ok); /* admins only */
    ThResult r = th_act(adam, "system.tz_retime", "");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Moved 1 back to the times typed");
    h = th_page(adam, "/system");
    CHECK_HAS(h, "1 time typed before then was moved back to the time typed.");
    CHECK(!strstr(h, "system.tz_retime"));
    CHECK(!th_act(adam, "system.tz_retime", "").ok); /* once */
    CHECK_HAS(th_page(adam, "/plan/cal?day=2026-10-13"), "14:30");
    CHECK(th_act(adam, "system.tz_seen", "").ok);
    CHECK(!strstr(th_page(adam, "/system"), "The time zone changed"));
    g_fake_now = 0;
    plat_set_tz(NULL);
    th_halt();
}
