/* modules/system in Swedish and Spanish (issue #46): every tab in its interesting states - the checks
 * with problems and without, the getting-started list, a time zone change, the log, backups, updates
 * through the helper (and by hand), help - the admin's home card, and what the buttons answer. */
#include "../../tests/harness.h"
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/tar.h"
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

static int all_well(void) {
    auth_configure("system-i18n-salt", 1000);
    int adam = admin("Adam", "adam", "correct horse");
    system_test_addresses("http://192.168.1.50:8080", "http://hemma.local:8080");
    system_test_free_space(50LL << 30);
    return adam;
}

/* ------------------------------------------------- what isn't the System module's own */

/* Other modules' words on these pages (their titles in the navigation, their checks and getting-started
   steps through the health hook) and the core's (a rescue report, the time-zone previews) are theirs to
   translate: allowed here like names. Plus words spelled the same: "System" and "Version" in Swedish. */
#define MAX_ALLOW 512
static const char *g_allow[MAX_ALLOW + 1];
static char *g_kept[MAX_ALLOW];
static int g_nallow;

static void allow(const char *s) {
    if (!s || !*s || g_nallow >= MAX_ALLOW) return;
    char *t = xstrdup(s);
    str_trim(t);
    g_kept[g_nallow] = t;
    g_allow[g_nallow++] = t;
    g_allow[g_nallow] = NULL;
}

static void allow_free(void) {
    for (int i = 0; i < g_nallow; i++) free(g_kept[i]);
    g_nallow = 0;
    g_allow[0] = NULL;
}

/* (in English: the person's language isn't set yet when this runs) */
static void allow_others(int person) {
    allow_free();
    static const char *const data[] = {"Adam", "Eva", "A", "Villa Kulla", "System", "Version", "hemma", "db", "Dentist",
                                       /* the core's shell around every page: its own to translate */
                                       "Skip to content", "More", NULL};
    for (int i = 0; data[i]; i++) allow(data[i]);
    for (int i = 0; i < module_count(); i++)
        if (!str_eq(module_at(i)->id, "system")) allow(module_at(i)->title);
    if (plat_bundle()) { /* (the download's file name for this machine: "hemma-linux-x64.tar.gz") */
        char file[96];
        snprintf(file, sizeof file, "%s.tar.gz", plat_bundle());
        allow(file);
    }
    Ctx c;
    app_ctx(&c, person, "/system");
    Health h = {0};
    health_collect(&c, &h);
    for (int i = 0; i < h.n; i++) {
        allow(h.items[i].title);
        allow(h.items[i].detail);
        allow(h.items[i].todo);
        allow(h.items[i].tech);
    }
    health_free(&h);
    char said[768];
    snprintf(said, sizeof said, "Hemma %s", HEMMA_VERSION_FULL); /* (the help's version line: a commit stamp) */
    allow(said);
    ZoneChange zc;
    Rezone z;
    if (zone_change(&zc) && zc.retimed < 0 && zone_retime(false, &z))
        for (int k = 0; k < z.notes; k++) allow(z.note[k]);
    app_ctx_free(&c);
}

static int untranslated(int person, const char *url, Buf *report) {
    allow_others(person);
    return th_untranslated(person, url, g_allow, report);
}

#define SPEAKS(person, url)                                                                                            \
    do {                                                                                                               \
        Buf r_ = {0};                                                                                                  \
        int n_ = untranslated(person, url, &r_);                                                                       \
        if (n_) printf("%s", buf_cstr(&r_));                                                                           \
        CHECK_INT(n_, 0);                                                                                              \
        buf_free(&r_);                                                                                                 \
    } while (0)

/* An action's toast in Swedish and Spanish: not the English one. */
static void toast_speaks(int person, const char *action, const char *form) {
    char en[240];
    ThResult r = th_act(person, action, form);
    str_copy(en, sizeof en, r.toast);
    for (int l = 1; l <= 2; l++) {
        pref_set(person, PREF_LANG, l == 1 ? "sv" : "es");
        r = th_act(person, action, form);
        if (!r.toast[0] || str_eq(r.toast, en)) printf("%s (%s): toast \"%s\"\n", action, l == 1 ? "sv" : "es", r.toast);
        CHECK(r.toast[0] && !str_eq(r.toast, en));
    }
    pref_set(person, PREF_LANG, "");
}

/* ------------------------------------------------------------------ checks */

TEST(system_checks_speak_swedish_and_spanish) {
    th_boot();
    int adam = all_well();
    system_log_reset();
    SPEAKS(adam, "/system"); /* the getting-started list first, every check OK */

    /* things that need a look: the secret, the disk, the log, the address, a Pi's UTC */
    LogLevel level = g_log_level;
    bool quiet = g_log_stderr;
    g_log_stderr = false;
    g_log_level = LOG_INFO;
    system_log_reset();
    log_error("store: writing person failed: disk I/O error");
    setting_set("auth.salt_check", "0123456789abcdef");
    system_test_free_space(1LL << 30);
    SPEAKS(adam, "/system");
    system_test_free_space(100LL << 20); /* (under 300 MB) */
    app_test_host("127.0.0.1");
    SPEAKS(adam, "/system");
    app_test_host(NULL);
    system_test_addresses("", NULL);
    SPEAKS(adam, "/system");
    system_test_addresses("http://192.168.1.50:8080", NULL);
    setting_set("auth.salt_check", "");
    system_test_free_space(50LL << 30);

    /* nobody can sign in yet, then no secret at all */
    Person p = *person_get(adam);
    p.pass[0] = '\0';
    tbl_put(&T_person, &p);
    SPEAKS(adam, "/system");
    auth_configure("", 1000);
    SPEAKS(adam, "/system");
    auth_configure("system-i18n-salt", 1000);
    auth_hash("correct horse", p.pass, sizeof p.pass);
    tbl_put(&T_person, &p);
    system_log_reset();
    g_log_level = level;
    g_log_stderr = quiet;

    /* the list done and put away */
    setting_set("household.name", "Villa Kulla");
    CHECK(th_act(adam, "system.salt_saved", "").ok);
    CHECK(th_act(adam, "system.start_hide", "").ok);
    SPEAKS(adam, "/system");
    CHECK(th_act(adam, "system.start_show", "").ok);

    /* a second admin, a member, two addresses: the plural words */
    admin("Eva", "eva", "battery staple");
    system_test_addresses("http://192.168.1.50:8080", "http://hemma.local:8080");
    SPEAKS(adam, "/system");
    SPEAKS(adam, "/system/nope");
    th_halt();
}

TEST(system_time_zone_speaks_swedish_and_spanish) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    plat_set_tz("UTC0");
    g_fake_now = dt_make(2026, 10, 7, 10, 0);
    th_boot_in(dir);
    int adam = all_well();
    SPEAKS(adam, "/system"); /* a Pi's own UTC */
    if (module_find("plan")) {
        CHECK(th_act(adam, "plan.event_add", "q=dentist+13%2F10+14%3A30").ok);
        CHECK(th_act(adam, "plan.event_add", "q=midsummer+party+24%2F10").ok);
    }
    th_halt();
    plat_set_tz("XXX-2");
    g_fake_now += 3600;
    th_boot_in(dir);
    all_well();
    SPEAKS(adam, "/system"); /* the change, what moving back would do, the choice */
    if (module_find("plan")) {
        CHECK(th_act(adam, "system.tz_retime", "").ok);
        SPEAKS(adam, "/system"); /* moved back */
    }
    toast_speaks(adam, "system.tz_seen", "");
    SPEAKS(adam, "/system");
    g_fake_now = 0;
    plat_set_tz(NULL);
    th_halt();
}

/* ---------------------------------------------------------------- the log */

TEST(system_log_speaks_swedish_and_spanish) {
    th_boot();
    int adam = all_well();
    LogLevel level = g_log_level;
    bool quiet = g_log_stderr;
    g_log_stderr = false;
    g_log_level = LOG_DEBUG;
    system_log_reset();
    SPEAKS(adam, "/system/log"); /* nothing logged yet */
    static const char *const lines[] = {"Library scan done: 178 episodes", "Disk nearly FULL on /srv/media",
                                        "Saving failed: disk I/O error", "a detail for the curious"};
    log_info("%s", lines[0]);
    log_warn("%s", lines[1]);
    log_error("%s", lines[2]);
    log_debug("%s", lines[3]);
    CHECK(th_act(adam, "system.verbose", "on=1").ok);
    allow_others(adam);
    Buf r = {0};
    static const char *const urls[] = {"/system/log", "/system/log?f=warn", "/system/log?q=library",
                                       "/system/log?q=nothing+like+it"};
    int n = 0;
    for (size_t i = 0; i < countof(urls); i++) {
        allow_others(adam);
        for (size_t k = 0; k < countof(lines); k++) allow(lines[k]);
        /* (log lines are the log's: English, as they are - and so is what the actions just logged) */
        allow("system: Adam turned on detailed logging until Hemma restarts");
        n += th_untranslated(adam, urls[i], g_allow, &r);
    }
    if (n) printf("%s", buf_cstr(&r));
    CHECK_INT(n, 0);
    buf_free(&r);
    toast_speaks(adam, "system.verbose", "on=1");
    toast_speaks(adam, "system.verbose", "");
    g_log_level = level;
    g_log_stderr = quiet;
    system_log_reset();
    th_halt();
}

TEST(system_log_dividers_speak_swedish_and_spanish) {
    th_boot();
    char dir[600], path[700];
    str_copy(dir, sizeof dir, app_config()->data_dir);
    snprintf(path, sizeof path, "%s/hemma.log", dir);
    th_halt();
    static const char crashed[] = "2026-10-12 10:00:00 - Hemma 0.2.0 started\n"
                                  "2026-10-12 10:05:00 - Hemma stopped\n"
                                  "2026-10-12 10:06:00 - Hemma 0.2.0 (abc) started\n";
    CHECK(file_write_atomic(path, crashed, sizeof crashed - 1));
    system_log_reset();
    app_set_testing(true);
    CHECK(app_boot(dir));
    int adam = all_well();
    SPEAKS(adam, "/system/log"); /* "Hemma restarted - it had stopped without warning", the old marks */
    SPEAKS(adam, "/system"); /* the Hemma check: it stopped without warning */
    pref_set(adam, PREF_LANG, "sv");
    CHECK_HAS(th_page(adam, "/system/log"), "Hemma 0.2.0 startade");
    pref_set(adam, PREF_LANG, "");
    th_halt();
    system_log_reset();
}

/* ---------------------------------------------------------------- backups */

TEST(system_backups_speak_swedish_and_spanish) {
    char dir[512], bk[600], p[700], form[80];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(bk, sizeof bk, "%s/backups", dir);
    th_boot_in(dir);
    int adam = all_well();
    SPEAKS(adam, "/system/backups"); /* not looked in yet */
    SPEAKS(adam, "/system");         /* "No backup yet" */
    toast_speaks(adam, "system.backup_now", "");
    CHECK(dir_make(bk));
    snprintf(p, sizeof p, "%s/hemma-2026-09-01-0330.db", bk);
    CHECK(file_write_atomic(p, "x", 1)); /* (one that won't check out) */
    CHECK(th_act(adam, "system.backup_now", "").ok);
    SPEAKS(adam, "/system/backups"); /* the list, "Backed up and checked" */
    SPEAKS(adam, "/system");         /* the last one, how many kept */
    CHECK(th_act(adam, "system.backup_restore", "name=hemma-2026-09-01-0330.db").ok);
    SPEAKS(adam, "/system/backups"); /* "Not put back: ..." (the core's reason, as data) */
    toast_speaks(adam, "system.backup_restore", "name=hemma-2026-01-01-0000.db");
    toast_speaks(adam, "system.backup_dir", "dir=relative%2Fpath");
    toast_speaks(adam, "system.backup_dir", "dir=");
    /* one to go back to: waiting for the restart */
    char name[40] = "";
    DirIter *it = dir_open(bk);
    DirEntry e;
    while (it && dir_next(it, &e))
        if (str_starts(e.name, "hemma-") && str_ends(e.name, ".db") && !str_eq(e.name, "hemma-2026-09-01-0330.db"))
            str_copy(name, sizeof name, e.name);
    dir_close(it);
    CHECK(name[0]);
    snprintf(form, sizeof form, "name=%s", name);
    CHECK(th_act(adam, "system.backup_restore", form).ok);
    SPEAKS(adam, "/system/backups");
    SPEAKS(adam, "/system"); /* "Going back to a backup" */
    toast_speaks(adam, "system.backup_cancel", "");
    /* a nightly one that failed */
    char fail[200];
    snprintf(fail, sizeof fail, "%lld|", (long long)now_sec() + 5);
    i18n_pack(fail + strlen(fail), sizeof fail - strlen(fail),
              N_("the folder %s can't be made - is the disk there, and may Hemma write to it?"), "/mnt/x");
    setting_set(SET_BACKUP_FAIL, fail);
    SPEAKS(adam, "/system");
    pref_set(adam, PREF_LANG, "es");
    CHECK_HAS(th_page(adam, "/system"), "no se puede crear la carpeta /mnt/x");
    pref_set(adam, PREF_LANG, "");
    /* the secret in the download, in the language of whoever downloads it */
    snprintf(p, sizeof p, "%s/%s", bk, name);
    Buf out = {0};
    Lang was = i18n_use(LANG_SV);
    CHECK(system_backup_bundle(p, "s3cret", 1760000000, &out));
    i18n_use(was);
    const void *txt;
    size_t n;
    CHECK(tar_find(out.p, out.len, "hemma-secret.txt", &txt, &n));
    Buf t = {0};
    buf_put(&t, txt, n);
    CHECK_HAS(buf_cstr(&t), "HEMLIGT - förvara den här filen som ett lösenord.");
    CHECK_HAS(buf_cstr(&t), "\nHEMMA_AUTH_SALT=s3cret\n");
    buf_free(&t);
    buf_free(&out);
    th_halt();
}

/* ---------------------------------------------------------------- updates */

static void answer(const char *ans, const char *name, const char *line) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s", ans, name);
    CHECK(file_write_atomic(path, line, strlen(line)));
}

TEST(system_updates_speak_swedish_and_spanish) {
    th_boot();
    int adam = all_well();
    SPEAKS(adam, "/system/updates"); /* no helper: how to update by hand */
    toast_speaks(adam, "system.update_check", "");
    system_test_update_os(true, false, 0);
    SPEAKS(adam, "/system/updates"); /* + the operating system's own, off */
    SPEAKS(adam, "/system");
    system_test_update_os(true, true, now_sec() - 3600);
    SPEAKS(adam, "/system/updates");
    system_test_update_os(true, true, 0);
    SPEAKS(adam, "/system/updates");

    char ask[600], ans[700], line[200];
    snprintf(ask, sizeof ask, "%s/update", test_tmpdir());
    snprintf(ans, sizeof ans, "%s/answers", ask);
    CHECK(dir_make(ask) && dir_make(ans));
    system_test_update_dirs(ask, ans);
    SPEAKS(adam, "/system/updates"); /* not checked yet */
    SPEAKS(adam, "/system");
    toast_speaks(adam, "system.update_auto", "on=1");
    toast_speaks(adam, "system.update_auto", "");
    int64_t t = now_sec();
    g_fake_now = t;
    CHECK(th_act(adam, "system.update_check", "").ok);
    SPEAKS(adam, "/system/updates"); /* asking github.com */
    g_fake_now = t + 200;
    SPEAKS(adam, "/system/updates"); /* the helper didn't answer */
    SPEAKS(adam, "/system");
    g_fake_now = 0;

    /* the helper's words are its own (English data): shown as they are */
    static const char *const words[] = {"github.com can't be reached", "Hemma 99.0.0 didn't start within 60 seconds",
                                        "the download doesn't say its version", "the old program didn't answer either",
                                        NULL};
    snprintf(line, sizeof line, "%lld - %s\n", (long long)now_sec(), words[0]);
    answer(ans, "latest", line);
    system_update_tick(now_ms());
    allow_others(adam);
    for (int i = 0; words[i]; i++) allow(words[i]);
    Buf r = {0};
    int n = th_untranslated(adam, "/system/updates", g_allow, &r); /* the check didn't work */

    snprintf(line, sizeof line, "%lld 99.0.0\n", (long long)now_sec());
    answer(ans, "latest", line);
    system_update_tick(now_ms());
    n += th_untranslated(adam, "/system/updates", g_allow, &r); /* a new one is out */
    n += th_untranslated(adam, "/system", g_allow, &r);
    toast_speaks(adam, "system.update_install", "v=98.0.0");
    CHECK(th_act(adam, "system.update_install", "v=99.0.0").ok);
    n += th_untranslated(adam, "/system/updates", g_allow, &r); /* downloading and checking */
    static const char *const results[] = {"back %lld 99.0.0 Hemma 99.0.0 didn't start within 60 seconds\n",
                                          "refused %lld - the download doesn't say its version\n",
                                          "broken %lld 99.0.0 the old program didn't answer either\n",
                                          "installed %lld 99.0.0 0.21.0\n", "installed %lld 99.0.0\n"};
    for (size_t i = 0; i < countof(results); i++) {
        snprintf(line, sizeof line, results[i], (long long)now_sec() + 1 + (long long)i);
        answer(ans, "result", line);
        system_update_tick(now_ms());
        allow_others(adam);
        for (int k = 0; words[k]; k++) allow(words[k]);
        n += th_untranslated(adam, "/system/updates", g_allow, &r);
        n += th_untranslated(adam, "/system", g_allow, &r);
    }
    if (n) printf("%s", buf_cstr(&r));
    CHECK_INT(n, 0);
    buf_free(&r);
    toast_speaks(adam, "system.update_seen", "");
    system_test_update_os(false, false, 0);
    th_halt();
}

/* ------------------------------------------------------------------- help */

TEST(system_help_speaks_swedish_and_spanish) {
    th_boot();
    int adam = all_well();
    SPEAKS(adam, "/system/help");
    th_halt();
}

/* ------------------------------------------------------- the admin's home card */

/* The text between tags, trimmed, that has a letter in it. */
static int words_of(const char *h, char out[][300], int max) {
    int n = 0;
    for (const char *p = h; *p && n < max;) {
        if (*p == '<') {
            const char *e = strchr(p, '>');
            if (!e) break;
            p = e + 1;
            continue;
        }
        const char *e = strchr(p, '<');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char s[300];
        str_ncopy(s, sizeof s, p, len);
        str_trim(s);
        bool letter = false;
        for (const char *q = s; *q && !letter; q++) letter = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z');
        if (letter) str_copy(out[n++], 300, s);
        p += len;
    }
    return n;
}

static void card(int person, Lang lang, Buf *out) {
    pref_set(person, PREF_LANG, i18n_code(lang));
    Ctx c;
    app_ctx(&c, person, "/");
    buf_reset(out);
    module_find("system")->widget(&c, out, WIDGET_HOME);
    app_ctx_free(&c);
    pref_set(person, PREF_LANG, "");
}

static int card_untranslated(int person) {
    static char en[64][300], other[64][300];
    Buf b = {0};
    card(person, LANG_EN, &b);
    int ne = words_of(buf_cstr(&b), en, 64), bad = 0;
    allow_others(person);
    for (int l = LANG_SV; l <= LANG_ES; l++) {
        card(person, (Lang)l, &b);
        int no = words_of(buf_cstr(&b), other, 64);
        for (int i = 0; i < no; i++) {
            bool same = false, allowed = false;
            for (int k = 0; k < ne && !same; k++) same = str_eq(en[k], other[i]);
            for (int k = 0; k < g_nallow && !allowed; k++) allowed = str_eq(g_allow[k], other[i]);
            if (same && !allowed) {
                printf("the home card (%s): \"%s\"\n", i18n_code((Lang)l), other[i]);
                bad++;
            }
        }
    }
    buf_free(&b);
    return bad;
}

TEST(system_home_card_speaks_swedish_and_spanish) {
    th_boot();
    int adam = all_well();
    system_log_reset();
    CHECK_INT(card_untranslated(adam), 0); /* the next getting-started step */
    setting_set("auth.salt_check", "0123456789abcdef");
    system_test_free_space(1LL << 30);
    CHECK_INT(card_untranslated(adam), 0); /* something isn't working */
    setting_set("auth.salt_check", "");
    CHECK_INT(card_untranslated(adam), 0); /* something needs a look */
    pref_set(adam, PREF_LANG, "sv");
    Buf b = {0};
    card(adam, LANG_SV, &b);
    CHECK_HAS(buf_cstr(&b), "Något behöver ses över");
    CHECK_HAS(buf_cstr(&b), "Nästa: <a href=\"/people\"><b>Namnge ditt hem</b></a>");
    buf_free(&b);
    th_halt();
}

