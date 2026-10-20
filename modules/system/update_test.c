/* modules/system/update.c: the Updates tab - by hand without the helper, checks and installs through
 * it (its request file and its answers), the daily check only when allowed, a silent helper, and a
 * download uploaded over a real socket. The helper itself is deploy/hemma-upgrade (a shell script). */
#include "../../tests/harness.h"
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "system.h"

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

/* the helper's two folders, made: ask (Hemma's) and answers (the helper's) */
static void helper(char *ask, char *ans, size_t cap) {
    const char *t = test_tmpdir();
    snprintf(ask, cap, "%s/update", t);
    snprintf(ans, cap, "%s/answers", t);
    CHECK(dir_make(ask));
    CHECK(dir_make(ans));
    system_test_update_dirs(ask, ans);
}

static void answer(const char *ans, const char *name, const char *line) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s", ans, name);
    CHECK(file_write_atomic(path, line, strlen(line)));
}

/* the request the helper would find: its words without the nonce ("" = none) */
static void request(const char *ask, char *out, size_t cap) {
    char path[700];
    Buf b = {0};
    snprintf(path, sizeof path, "%s/request", ask);
    out[0] = '\0';
    if (file_read_all(path, &b, 200)) {
        const char *s = buf_cstr(&b), *sp = strrchr(s, ' ');
        size_t n = sp ? (size_t)(sp - s) : 0;
        if (sp && strlen(sp + 1) == 17 && sp[17] == '\n') str_ncopy(out, cap, s, n); /* " <16 hex>\n" */
    }
    buf_free(&b);
}

TEST(system_update_versions_compare) {
    CHECK(system_update_newer("0.21.0", "0.20.9"));
    CHECK(system_update_newer("0.10.0", "0.9.9"));
    CHECK(system_update_newer("1.0.0", "0.99.99"));
    CHECK(!system_update_newer("0.21.0", "0.21.0"));
    CHECK(!system_update_newer("0.20.0", "0.21.0"));
    CHECK(!system_update_newer("1.0", "0.1.0"));
    CHECK(!system_update_newer("0.21.0x", "0.1.0"));
    CHECK(!system_update_newer("a.b.c", "0.1.0"));
    CHECK(!system_update_newer("0.21.0", ""));
    CHECK(!system_update_newer("123456.0.0", "0.1.0"));
}

TEST(system_updates_by_hand_without_the_helper) {
    th_boot();
    int adam = admin("Adam", NULL, NULL), eva = th_person("Eva");
    const char *h = th_page(adam, "/system/updates");
    CHECK_HAS(h, "Hemma <b>");
    CHECK_HAS(h, HEMMA_VERSION);
    CHECK_HAS(h, "Updates from this page need Hemma installed with its installer");
    CHECK_HAS(h, "https://github.com/kloparn/Hemma/releases");
    CHECK(!strstr(h, "data-upload"));
    CHECK(!strstr(h, "system.update_check"));
    ThResult r = th_act(adam, "system.update_check", "");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "no update helper");
    CHECK(!th_act(eva, "system.update_check", "").ok);
    CHECK(!strstr(th_page(eva, "/system/updates"), "Updates from this page")); /* (admins only) */
    CHECK_HAS(th_page(adam, "/system"), "href=\"/system/updates\"");
    th_halt();
}

TEST(system_updates_check_and_install_through_the_helper) {
    th_boot();
    int adam = admin("Adam", NULL, NULL), eva = th_person("Eva");
    char ask[600], ans[600], req[64];
    helper(ask, ans, sizeof ask);
    const char *h = th_page(adam, "/system/updates");
    CHECK_HAS(h, "Not checked yet.");
    CHECK_HAS(h, "data-a=\"system.update_check\"");
    CHECK_HAS(h, "data-upload=\"/system/update\"");
    CHECK(!strstr(h, "Updates from this page need"));

    CHECK(!th_act(eva, "system.update_check", "").ok);
    CHECK(th_act(adam, "system.update_check", "").ok);
    request(ask, req, sizeof req);
    CHECK_STR(req, "check");
    CHECK_HAS(th_page(adam, "/system/updates"), "Asking github.com");

    /* the helper's answer: a newer version is out */
    char line[200];
    snprintf(line, sizeof line, "%lld 99.0.0\n", (long long)now_sec());
    answer(ans, "latest", line);
    system_update_tick(now_ms());
    h = th_page(adam, "/system/updates");
    CHECK_HAS(h, "<b>Hemma 99.0.0 is out</b>");
    CHECK_HAS(h, "releases/tag/v99.0.0");
    CHECK_HAS(h, "data-a=\"system.update_install\"");
    CHECK_HAS(h, "name=\"v\" value=\"99.0.0\"");
    CHECK_HAS(th_page(adam, "/system"), "Hemma 99.0.0 is out (this is ");

    /* only that version, only an admin, one at a time */
    ThResult r = th_act(adam, "system.update_install", "v=98.0.0");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "isn't a newer version");
    CHECK(!th_act(eva, "system.update_install", "v=99.0.0").ok);
    CHECK(th_act(adam, "system.update_install", "v=99.0.0").ok);
    request(ask, req, sizeof req);
    CHECK_STR(req, "install 99.0.0");
    CHECK_HAS(th_page(adam, "/system/updates"), "Downloading and checking Hemma 99.0.0");
    r = th_act(adam, "system.update_install", "v=99.0.0");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "under way");
    CHECK(!th_act(adam, "system.update_check", "").ok);

    /* it didn't start: the helper went back */
    snprintf(line, sizeof line, "back %lld 99.0.0 Hemma 99.0.0 didn't start within 60 seconds\n", (long long)now_sec() + 1);
    answer(ans, "result", line);
    system_update_tick(now_ms());
    h = th_page(adam, "/system/updates");
    CHECK_HAS(h, "<b>Hemma 99.0.0 didn't start</b>");
    CHECK_HAS(h, "nothing was lost");
    CHECK_HAS(h, "start within 60 seconds"); /* (the helper's words, escaped) */
    CHECK_HAS(h, "data-a=\"system.update_seen\"");
    CHECK(!strstr(h, "Downloading and checking"));
    CHECK_HAS(th_page(adam, "/system"), "so the update helper went back to this one");
    CHECK(th_act(adam, "system.update_seen", "").ok);
    CHECK(!strstr(th_page(adam, "/system/updates"), "didn't start</b>"));

    /* a second try that went in */
    snprintf(line, sizeof line, "installed %lld 99.0.0 0.21.0\n", (long long)now_sec() + 2);
    answer(ans, "result", line);
    system_update_tick(now_ms());
    h = th_page(adam, "/system/updates");
    CHECK_HAS(h, "<b>Updated to Hemma 99.0.0</b> (from 0.21.0)");
    CHECK_HAS(th_page(adam, "/system"), "Updated to Hemma 99.0.0");
    /* nonsense from the answers folder shows nothing */
    answer(ans, "result", "<script> 1 2 3\n");
    answer(ans, "latest", "x\n");
    system_update_tick(now_ms());
    h = th_page(adam, "/system/updates");
    CHECK(!strstr(h, "<script>"));
    CHECK(!strstr(h, "Updated to"));
    CHECK(!strstr(h, " is out</b>"));
    th_halt();
}

TEST(system_updates_check_daily_only_when_allowed) {
    th_boot();
    int adam = admin("Adam", NULL, NULL);
    char ask[600], ans[600], req[64], path[700];
    helper(ask, ans, sizeof ask);
    snprintf(path, sizeof path, "%s/request", ask);
    system_update_tick(now_ms());
    CHECK(!path_stat(path, NULL, NULL, NULL)); /* off by default: nothing leaves the home */
    CHECK(th_act(adam, "system.update_auto", "on=1").ok);
    CHECK_HAS(th_page(adam, "/system/updates"), "data-auto checked> Check once a day");
    system_update_tick(now_ms());
    request(ask, req, sizeof req);
    CHECK_STR(req, "check");
    /* answered: nothing more until a day has gone by */
    CHECK(file_remove(path));
    char line[100];
    int64_t t = now_sec();
    snprintf(line, sizeof line, "%lld 0.0.1\n", (long long)t);
    answer(ans, "latest", line);
    g_fake_now = t + 3600;
    system_update_tick(now_ms());
    CHECK(!path_stat(path, NULL, NULL, NULL));
    CHECK_HAS(th_page(adam, "/system/updates"), "This is the newest Hemma");
    g_fake_now = t + 25 * 3600;
    system_update_tick(now_ms());
    request(ask, req, sizeof req);
    CHECK_STR(req, "check");
    /* turned off again */
    CHECK(file_remove(path));
    CHECK(th_act(adam, "system.update_auto", "").ok);
    g_fake_now = t + 50 * 3600;
    system_update_tick(now_ms());
    CHECK(!path_stat(path, NULL, NULL, NULL));
    g_fake_now = 0;
    th_halt();
}

TEST(system_updates_say_when_the_helper_is_silent) {
    th_boot();
    int adam = admin("Adam", NULL, NULL);
    char ask[600], ans[600];
    helper(ask, ans, sizeof ask);
    int64_t t = now_sec();
    g_fake_now = t;
    CHECK(th_act(adam, "system.update_check", "").ok);
    g_fake_now = t + 30;
    CHECK(!strstr(th_page(adam, "/system/updates"), "didn't answer"));
    g_fake_now = t + 200;
    CHECK_HAS(th_page(adam, "/system/updates"), "The update helper didn't answer");
    CHECK_HAS(th_page(adam, "/system"), "hemma-update.path");
    g_fake_now = 0;
    th_halt();
}

TEST(system_updates_show_the_operating_systems_own) {
    th_boot();
    int adam = admin("Adam", NULL, NULL);
    CHECK(!strstr(th_page(adam, "/system/updates"), "The operating system"));
    system_test_update_os(true, false, 0);
    CHECK_HAS(th_page(adam, "/system/updates"), "sudo apt install unattended-upgrades");
    CHECK_HAS(th_page(adam, "/system"), "Operating system updates");
    system_test_update_os(true, true, now_sec() - 3600);
    CHECK_HAS(th_page(adam, "/system/updates"), "Its security updates install by themselves (last looked for");
    CHECK(!strstr(th_page(adam, "/system"), "Operating system updates"));
    system_test_update_os(false, false, 0);
    th_halt();
}

/* ------------------------------------------------- a download, uploaded */

static volatile int g_done;
static int g_port;
static void serve(void *arg) {
    (void)arg;
    app_serve();
    g_done = 1;
}

static sock_t dial(void) {
    sock_t s = tcp_connect_nb(0x7F000001, g_port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) return SOCK_BAD;
    return s;
}

static void send_all(sock_t s, const void *p, size_t n) {
    const char *c = p;
    for (int64_t end = now_ms() + 5000; n && now_ms() < end;) {
        long k = sock_send(s, c, n);
        if (k > 0) c += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
}

static void call(const char *method, const char *path, const char *cookie, const char *extra, const void *body,
                 size_t n, Buf *resp) {
    char head[1024];
    snprintf(head, sizeof head,
             "%s %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n",
             method, path, cookie, extra ? extra : "", n);
    buf_reset(resp);
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, head, strlen(head));
    send_all(s, body, n);
    for (int64_t end = now_ms() + 5000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[4096];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(resp, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
}

static void sign_in(const char *user, const char *pass, char *cookie, size_t cap) {
    char form[200];
    snprintf(form, sizeof form, "username=%s&password=%s&next=%%2F", user, pass);
    Buf r = {0};
    call("POST", "/login", "-", "Content-Type: application/x-www-form-urlencoded\r\n", form, strlen(form), &r);
    const char *c = strstr(buf_cstr(&r), "Set-Cookie: hemma=");
    cookie[0] = '\0';
    if (c) str_ncopy(cookie, cap, c + 18, strcspn(c + 18, ";\r\n"));
    buf_free(&r);
}

static void batch_of(const Buf *r, char out[17]) {
    const char *b = strstr(buf_cstr((Buf *)r), "\"batch\":\"");
    out[0] = '\0';
    if (b) str_ncopy(out, 17, b + 9, 16);
}

TEST(system_update_upload_reaches_the_helper) {
    th_boot();
    auth_configure("update-test-salt", 1000);
    admin("Adam", "adam", "pw-adam-123");
    int eva = th_person("Eva");
    Person p = *person_get(eva);
    str_copy(p.username, sizeof p.username, "eva");
    auth_hash("pw-eva-1234", p.pass, sizeof p.pass);
    tbl_put(&T_person, &p);
    char ask[600], ans[600], req[64], path[700];
    helper(ask, ans, sizeof ask);
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char adam[64], ev[64];
    sign_in("adam", "pw-adam-123", adam, sizeof adam);
    sign_in("eva", "pw-eva-1234", ev, sizeof ev);
    CHECK_INT(strlen(adam), 32);
    const char *json = "Content-Type: application/json\r\n";
    Buf r = {0};

    /* 1.5 MB "download": over the 1 MB the server buffers, so it streams to disk */
    size_t n = 3u << 19;
    char *gz = xmalloc(n);
    for (size_t i = 0; i < n; i++) gz[i] = (char)(i * 7 + (i >> 8));
    gz[0] = (char)0x1f, gz[1] = (char)0x8b;
    char plan[200];
    snprintf(plan, sizeof plan, "{\"files\":[[\"hemma-pi64.tar.gz\",%zu]]}", n);

    call("POST", "/system/update/plan", ev, json, plan, strlen(plan), &r); /* a member: no such thing */
    CHECK(!strstr(buf_cstr(&r), "\"batch\""));
    call("POST", "/system/update/plan", adam, "Origin: http://evil.example\r\n", plan, strlen(plan), &r);
    CHECK_HAS(buf_cstr(&r), "403");
    const char *zip = "{\"files\":[[\"hemma.zip\",100]]}";
    call("POST", "/system/update/plan", adam, json, zip, strlen(zip), &r);
    CHECK_HAS(buf_cstr(&r), "they end in .tar.gz");
    const char *huge = "{\"files\":[[\"hemma-pi64.tar.gz\",999999999]]}";
    call("POST", "/system/update/plan", adam, json, huge, strlen(huge), &r);
    CHECK_HAS(buf_cstr(&r), "far too big");

    call("POST", "/system/update/plan", adam, json, plan, strlen(plan), &r);
    CHECK_HAS(buf_cstr(&r), "200 OK");
    CHECK_HAS(buf_cstr(&r), "\"todo\":[[0,0]]");
    char batch[17], url[96];
    batch_of(&r, batch);
    CHECK_INT(strlen(batch), 16);
    snprintf(url, sizeof url, "/system/update/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, gz, n, &r);
    CHECK_HAS(buf_cstr(&r), "{\"ok\":true}");
    snprintf(path, sizeof path, "%s/upload.tar.gz", ask);
    Buf got = {0};
    CHECK(file_read_all(path, &got, n + 1));
    CHECK(got.len == n && memcmp(got.p, gz, n) == 0);
    buf_free(&got);
    /* the server's tick hands it to the helper */
    req[0] = '\0';
    for (int i = 0; i < 400 && !req[0]; i++, plat_sleep_ms(10)) request(ask, req, sizeof req);
    CHECK_STR(req, "install-file");

    /* while the helper hasn't answered, another upload waits */
    snprintf(plan, sizeof plan, "{\"files\":[[\"hemma-pi64.tar.gz\",%zu]]}", (size_t)1000);
    call("POST", "/system/update/plan", adam, json, plan, strlen(plan), &r);
    CHECK_HAS(buf_cstr(&r), "An update is under way");
    char line[100];
    snprintf(line, sizeof line, "refused %lld - the download doesn't say its version\n", (long long)now_sec() + 1);
    answer(ans, "result", line);
    app_tick_soon(); /* (the server reads it at its next tick) */
    for (int i = 0; i < 400 && path_stat(path, NULL, NULL, NULL); i++) plat_sleep_ms(10);
    CHECK(!path_stat(path, NULL, NULL, NULL)); /* (the helper has its own copy: ours went) */

    /* not a .tar.gz inside: refused, nothing kept */
    gz[0] = 'P', gz[1] = 'K';
    call("POST", "/system/update/plan", adam, json, plan, strlen(plan), &r);
    batch_of(&r, batch);
    CHECK_INT(strlen(batch), 16);
    snprintf(url, sizeof url, "/system/update/%s/0?at=0", batch);
    call("PUT", url, adam, NULL, gz, 1000, &r);
    CHECK_HAS(buf_cstr(&r), "isn't a Hemma download");
    CHECK(!path_stat(path, NULL, NULL, NULL));
    call("PUT", url, adam, NULL, gz, 1000, &r); /* (that upload is over) */
    CHECK_HAS(buf_cstr(&r), "404");

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    free(gz);
    buf_free(&r);
    th_halt();
}
