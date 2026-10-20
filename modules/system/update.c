/* modules/system/update.c — updates from the System page (issue #73; see system.h).
 *
 * Hemma may not change its own program: on a Pi it runs as the user hemma with the system read-only
 * around it (hemma.service), and that stays so. What an admin asks for here - "is there a new
 * version?", "install it", "install this download" - goes as one line into <data>/update/request;
 * hemma-update.path starts the root helper (deploy/hemma-upgrade), which does it with only what the
 * Hemma project signed (this very program checks the signature: hemma --verify-release), keeps the
 * program and a copy of the database, and goes back to them when the new Hemma doesn't answer. Its
 * answers come back in /var/lib/hemma-update (root's; read here, never written): latest and result.
 *
 * Nothing leaves the home on its own: the daily check is off until an admin turns it on, and then
 * the helper asks github.com one thing - which version is newest. Without the helper (Windows, a Mac,
 * Hemma started by hand, an install from before 0.21.0) the page says how to update that install. */
#include "system.h"
#include "../../core/http.h"
#include "../../core/json.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../core/ui.h"

#define ANSWERS "/var/lib/hemma-update"
#define SET_CHECK "system.update_check" /* "1" = ask github.com once a day */
#define SET_ASKED "system.update_asked" /* the last request: "<unix> <what>" */
#define SET_SEEN "system.update_seen"   /* the unix of the result an admin put away */
#define CHECK_EVERY_S (24 * 3600)
#define CHECK_SILENT_S 120    /* a check unanswered this long: the helper isn't running */
#define INSTALL_SILENT_S 900  /* an install unanswered this long (or "installing" this long) */
#define PLAN_IDLE_S 60        /* an upload planned but quiet this long gives way to another */
#define STALL_MS 60000
#define FILE_MAX (64LL << 20) /* a download is about 2 MB */
#define RELEASES "https://github.com/kloparn/Hemma/releases"

/* ---- the download an admin is uploading (g_lock: its thread writes too) ---- */
typedef enum { UP_IDLE, UP_PLANNED, UP_RECEIVED } UpState;
static Lock g_lock = LOCK_INIT;
static UpState g_up;
static bool g_writing;
static char g_batch[17], g_up_name[120], g_up_who[64], g_up_why[200];
static int g_up_person;
static int64_t g_up_size, g_up_got, g_up_touched; /* touched: unix */

/* ---- event loop ---- */
static char g_ask[700], g_ans[700];
static bool g_test, g_present;
static int64_t g_present_at, g_read_at, g_os_at, g_shown_got = -1; /* monotonic ms; bytes */
static Buf g_latest_raw, g_result_raw;
static int64_t g_latest_mt, g_result_mt; /* the answers' mtimes (unix; 0 = none) */
static int64_t g_latest_at;              /* when github.com was asked */
static char g_latest[16], g_latest_why[240];
static char g_state[16], g_res_ver[16], g_res_words[320];
static int64_t g_res_at;
static int64_t g_asked_at, g_auto_try;
static char g_asked[40]; /* "check", "install 0.22.0", "install-file" */
static bool g_apt, g_unattended;
static int64_t g_os_last;

/* --------------------------------------------------------------- helpers */

static bool version_of(const char *s, long v[3]) {
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        long n = 0;
        for (int d = 0; *s >= '0' && *s <= '9'; d++, s++) {
            if (d == 5) return false;
            n = n * 10 + (*s - '0');
        }
        v[i] = n;
        if (i < 2 && *s++ != '.') return false;
    }
    return *s == '\0';
}

bool system_update_newer(const char *a, const char *b) {
    long x[3], y[3];
    if (!version_of(a, x) || !version_of(b, y)) return false;
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

static void path_in(char *out, size_t cap, const char *dir, const char *name) { snprintf(out, cap, "%s/%s", dir, name); }

static bool present(void) {
    return path_is_dir(g_ask) && path_is_dir(g_ans);
}

/* "<unix> <version>" or "<unix> - <why>" */
static void parse_latest(const char *s) {
    g_latest_at = atoll(s);
    g_latest[0] = g_latest_why[0] = '\0';
    const char *p = strchr(s, ' ');
    if (!p) return;
    p++;
    if (str_starts(p, "- ")) str_copy(g_latest_why, sizeof g_latest_why, p + 2);
    else {
        char v[16];
        str_ncopy(v, sizeof v, p, strcspn(p, " \r\n"));
        long x[3];
        if (version_of(v, x)) str_copy(g_latest, sizeof g_latest, v);
    }
    str_trim(g_latest_why);
}

/* "<state> <unix> <version|-> <words>" */
static void parse_result(const char *s) {
    g_state[0] = g_res_ver[0] = g_res_words[0] = '\0';
    g_res_at = 0;
    char state[16];
    str_ncopy(state, sizeof state, s, strcspn(s, " \r\n"));
    static const char *const known[] = {"fetching", "installing", "installed", "back", "refused", "broken"};
    bool ok = false;
    for (size_t i = 0; i < countof(known); i++) ok |= str_eq(state, known[i]);
    const char *p = strchr(s, ' ');
    if (!ok || !p) return;
    g_res_at = atoll(++p);
    p = strchr(p, ' ');
    if (!p) return;
    p++;
    char v[16];
    str_ncopy(v, sizeof v, p, strcspn(p, " \r\n"));
    long x[3];
    if (version_of(v, x)) str_copy(g_res_ver, sizeof g_res_ver, v);
    p = strchr(p, ' ');
    if (p) str_copy(g_res_words, sizeof g_res_words, p + 1);
    str_trim(g_res_words);
    str_copy(g_state, sizeof g_state, state);
}

/* One of the helper's answers, when it changed; true = it did. */
static bool read_answer(const char *name, Buf *raw, int64_t *mt) {
    char path[760];
    path_in(path, sizeof path, g_ans, name);
    int64_t m = 0;
    Buf now = {0};
    if (!path_stat(path, NULL, &m, NULL) || !file_read_all(path, &now, 4096)) m = 0, buf_reset(&now);
    bool changed = m != *mt || now.len != raw->len || (now.len && memcmp(now.p, raw->p, now.len) != 0);
    if (changed) {
        buf_reset(raw);
        if (now.len) buf_put(raw, now.p, now.len);
        *mt = m;
    }
    buf_free(&now);
    return changed;
}

static void read_answers(void) {
    bool changed = false;
    if (read_answer("latest", &g_latest_raw, &g_latest_mt)) {
        parse_latest(buf_cstr(&g_latest_raw));
        changed = true;
    }
    if (read_answer("result", &g_result_raw, &g_result_mt)) {
        parse_result(buf_cstr(&g_result_raw));
        changed = true;
        /* an answer to an uploaded download: the helper has its own copy - ours can go */
        if (str_eq(g_asked, "install-file") && g_result_mt >= g_asked_at && !str_eq(g_state, "fetching")) {
            char path[760];
            path_in(path, sizeof path, g_ask, "upload.tar.gz");
            file_remove(path);
        }
    }
    if (changed) app_changed_where("/system");
}

static void read_asked(void) {
    const char *s = setting(SET_ASKED, "");
    g_asked_at = atoll(s);
    const char *p = strchr(s, ' ');
    str_copy(g_asked, sizeof g_asked, p ? p + 1 : "");
}

/* What an admin asked for, as the request line the helper reads ("<what> <nonce>"). */
static bool ask(const char *what) {
    char nonce[17], line[80], path[760], set[80];
    rand_hex(nonce, 8);
    snprintf(line, sizeof line, "%s %s\n", what, nonce);
    path_in(path, sizeof path, g_ask, "request");
    if (!file_write_atomic(path, line, strlen(line))) {
        log_warn("updates: couldn't write %s", path);
        return false;
    }
    g_asked_at = now_sec();
    str_copy(g_asked, sizeof g_asked, what);
    snprintf(set, sizeof set, "%lld %s", (long long)g_asked_at, what);
    setting_set(SET_ASKED, set);
    return true;
}

static bool answered(void) { /* the helper's answer to the last request is in */
    if (!g_asked_at) return true;
    return str_eq(g_asked, "check") ? g_latest_mt >= g_asked_at : g_result_mt >= g_asked_at;
}

static bool installing(int64_t now) { /* an install under way, by our request or the helper's word */
    if (!answered() && !str_eq(g_asked, "check") && now - g_asked_at < INSTALL_SILENT_S) return true;
    return (str_eq(g_state, "fetching") || str_eq(g_state, "installing")) && now - g_res_at < INSTALL_SILENT_S;
}

static bool silent(int64_t now) { /* the helper never answered the last request */
    if (answered() || !g_asked_at) return false;
    int64_t wait = str_eq(g_asked, "check") ? CHECK_SILENT_S : INSTALL_SILENT_S;
    return now - g_asked_at >= wait && now - g_asked_at < 7 * 86400;
}

static bool out_now(void) { return g_latest[0] && system_update_newer(g_latest, HEMMA_VERSION); }

static bool result_unseen(void) {
    if (!g_state[0] || str_eq(g_state, "fetching") || str_eq(g_state, "installing")) return false;
    return g_res_at > setting_int(SET_SEEN, 0);
}

/* The operating system's own updates (Debian's unattended-upgrades): cached facts, looked at hourly. */
static void read_os(void) {
    g_apt = path_is_dir("/etc/apt");
    g_unattended = false;
    g_os_last = 0;
    if (!g_apt) return;
    static const char *const conf[] = {"/etc/apt/apt.conf.d/20auto-upgrades", "/etc/apt/apt.conf.d/10periodic"};
    for (size_t i = 0; i < countof(conf) && !g_unattended; i++) {
        Buf t = {0};
        if (file_read_all(conf[i], &t, 16384)) {
            const char *p = strstr(buf_cstr(&t), "Unattended-Upgrade");
            p = p ? strchr(p, '"') : NULL;
            g_unattended = p && p[1] == '1';
        }
        buf_free(&t);
    }
    int64_t mt;
    if (path_stat("/var/lib/apt/periodic/unattended-upgrades-stamp", NULL, &mt, NULL)) g_os_last = mt;
}

/* --------------------------------------------------------------- lifecycle */

void system_update_start(void) {
    g_test = false; /* (a test sets its own folders after the boot: system_test_update_dirs) */
    path_in(g_ask, sizeof g_ask, system_data_path(), "update");
    str_copy(g_ans, sizeof g_ans, ANSWERS);
    g_apt = g_unattended = false;
    g_os_last = 0;
    buf_reset(&g_latest_raw);
    buf_reset(&g_result_raw);
    g_latest_mt = g_result_mt = g_latest_at = g_res_at = 0;
    g_latest[0] = g_latest_why[0] = g_state[0] = g_res_ver[0] = g_res_words[0] = '\0';
    g_present_at = g_read_at = g_os_at = g_auto_try = 0;
    g_shown_got = -1;
    lock_take(&g_lock);
    g_up = UP_IDLE;
    g_up_name[0] = '\0';
    lock_give(&g_lock);
    read_asked();
    g_present = present();
    if (g_present) {
        read_answers();
        char path[760];
        path_in(path, sizeof path, g_ask, "upload.tar.gz.part"); /* (an upload the stop cut off) */
        file_remove(path);
    }
    if (!app_testing()) read_os(); /* (tests: system_test_update_os - never this machine's) */
}

void system_update_stop(void) {
    buf_free(&g_latest_raw);
    buf_free(&g_result_raw);
}

void system_update_tick(int64_t now_ms_) {
    int64_t now = now_sec();
    if (!g_present_at || now_ms_ - g_present_at >= 60000) {
        g_present_at = now_ms_ ? now_ms_ : 1;
        bool was = g_present;
        g_present = present();
        if (was != g_present) app_changed_where("/system");
    }
    if (!app_testing() && (!g_os_at || now_ms_ - g_os_at >= 3600000)) {
        g_os_at = now_ms_ ? now_ms_ : 1;
        read_os();
    }
    if (!g_present) return;
    if (g_test || !g_read_at || now_ms_ - g_read_at >= 2000) {
        g_read_at = now_ms_ ? now_ms_ : 1;
        read_answers();
    }
    /* an uploaded download is in: the helper takes it from here */
    lock_take(&g_lock);
    UpState st = g_up;
    int64_t got = g_up_got;
    char who[64], name[120];
    str_copy(who, sizeof who, g_up_who);
    str_copy(name, sizeof name, g_up_name);
    int person = g_up_person;
    if (st == UP_RECEIVED) g_up = UP_IDLE;
    lock_give(&g_lock);
    if (st == UP_RECEIVED) {
        if (ask("install-file")) {
            log_info("updates: %s uploaded %s - the update helper checks and installs it", who, name);
            activity_add(person, "⬆️", "/system/updates", "%s started an update of Hemma from a download", who);
        }
        app_changed_where("/system");
    } else if (st == UP_PLANNED && got != g_shown_got) {
        g_shown_got = got;
        app_changed_where("/system/updates");
    }
    /* the daily check, when an admin turned it on */
    if (setting_int(SET_CHECK, 0) == 1 && !installing(now) && now - g_auto_try >= 600) {
        int64_t last = MAX(g_latest_at, str_eq(g_asked, "check") ? g_asked_at : 0);
        if (now - last >= CHECK_EVERY_S) {
            g_auto_try = now; /* (a request that can't be written: tried again in ten minutes, not every tick) */
            ask("check");
        }
    }
}

/* ---------------------------------------------------------------- checks */

void system_update_checks(Health *h) {
    int64_t now = now_sec();
    char d[700], when[64];
    if (g_present) {
        if (result_unseen()) {
            system_when(g_res_at, now, when, sizeof when);
            if (str_eq(g_state, "installed")) {
                snprintf(d, sizeof d, "Updated to Hemma %s %s.", g_res_ver, when);
                health_add(h, HEALTH_OK, "Update", d, NULL, "/system/updates");
            } else if (str_eq(g_state, "back")) {
                snprintf(d, sizeof d, "Hemma %s didn't start (%s), so the update helper went back to this one and the "
                                      "data as it was before. Nothing was lost.",
                         g_res_ver[0] ? g_res_ver : "?", when);
                health_add(h, HEALTH_WARN, "Update", d, "See why on the Updates tab - and say so in an issue on GitHub.",
                           "/system/updates");
                health_tech(h, g_res_words);
            } else if (str_eq(g_state, "refused")) {
                snprintf(d, sizeof d, "An update wasn't installed (%s): %s.", when, g_res_words);
                health_add(h, HEALTH_WARN, "Update", d, "Nothing was changed. The Updates tab says more.", "/system/updates");
            } else {
                snprintf(d, sizeof d, "An update of Hemma failed and going back didn't work at first (%s).", when);
                health_add(h, HEALTH_WARN, "Update", d, "Hemma runs again now. See the Updates tab and the log.",
                           "/system/updates");
                health_tech(h, g_res_words);
            }
        }
        if (silent(now)) {
            health_add(h, HEALTH_WARN, "Updates", "The update helper didn't answer.",
                       "On the Pi: sudo systemctl enable --now hemma-update.path - or run sudo hemma-upgrade there.",
                       "/system/updates");
        } else if (out_now()) {
            snprintf(d, sizeof d, "Hemma %s is out (this is %s).", g_latest, HEMMA_VERSION);
            health_add(h, HEALTH_INFO, "Updates", d, "Install it on the Updates tab.", "/system/updates");
        } else if (g_latest[0]) {
            system_when(g_latest_at, now, when, sizeof when);
            snprintf(d, sizeof d, "This is the newest Hemma (checked %s).", when);
            health_add(h, HEALTH_OK, "Updates", d, NULL, "/system/updates");
        } else {
            health_add(h, HEALTH_OK, "Updates",
                       setting_int(SET_CHECK, 0) == 1 ? "Checked once a day." : "Checked when you ask, on the Updates tab.",
                       NULL, "/system/updates");
        }
    }
    if (g_apt && !g_unattended) {
        health_add(h, HEALTH_INFO, "Operating system updates",
                   "The operating system's security updates aren't installed by themselves.",
                   "To have them installed every day: sudo apt install unattended-upgrades", "/system/updates");
    }
}

/* --------------------------------------------------------------- the page */

static void how_by_hand(Buf *b) {
    const char *dl = plat_bundle();
    buf_puts(b, "<p>Updates from this page need Hemma installed with its installer (<code>install-pi.sh</code>, from "
                "version 0.21.0) on a Raspberry Pi or another Linux with systemd: then a root helper installs what you "
                "choose here.</p>");
    if (dl)
        H(b, "<p>Here: download <b>%s.tar.gz</b> from <a href=\"%s/latest\" target=\"_blank\" rel=\"noopener noreferrer\">"
             "the newest release</a> and install it the way you installed this one. A Pi or Linux with systemd: unpack "
             "it and run <code>sudo sh install-pi.sh ./%s</code> - from then on, updates come from here or with "
             "<code>sudo hemma-upgrade</code>.</p>",
          dl, RELEASES, str_eq(dl, "hemma-linux-x64") ? "hemma-linux" : dl);
    else
        H(b, "<p>Here: build the newest release from its source (<a href=\"%s/latest\" target=\"_blank\" "
             "rel=\"noopener noreferrer\">the newest release</a>, docs/windows.md).</p>",
          RELEASES);
}

static void result_notice(Buf *b, int64_t now) {
    char when[64];
    system_when(g_res_at, now, when, sizeof when);
    if (str_eq(g_state, "installed"))
        H(b, "<div class=\"notice system-up-note\" id=\"system-up-result\"><p><b>Updated to Hemma %s</b> (from %s), %s. "
             "<a href=\"%s/tag/v%s\" target=\"_blank\" rel=\"noopener noreferrer\">What's new</a></p>",
          g_res_ver, g_res_words[0] ? g_res_words : "the one before", when, RELEASES, g_res_ver);
    else if (str_eq(g_state, "back"))
        H(b, "<div class=\"notice system-up-note system-up-bad\" id=\"system-up-result\"><p><b>Hemma %s didn't start</b>, so "
             "the update helper went back to this one and the data as it was before (%s) - nothing was lost.</p>"
             "<p class=\"small\">%s</p>",
          g_res_ver[0] ? g_res_ver : "?", when, g_res_words);
    else if (str_eq(g_state, "refused"))
        H(b, "<div class=\"notice system-up-note system-up-bad\" id=\"system-up-result\"><p><b>Not installed</b> (%s): %s. "
             "Nothing was changed.</p>",
          when, g_res_words);
    else
        H(b, "<div class=\"notice system-up-note system-up-bad\" id=\"system-up-result\"><p><b>An update failed</b> (%s), "
             "and going back didn't work at first: %s. Hemma runs again now - the Log tab may say why.</p>",
          when, g_res_words);
    ui_act(b, "system.update_seen", "btn sm", "Got it", NULL, NULL, NULL);
    buf_puts(b, "</div>");
}

void system_update_page(Ctx *c, Buf *b) {
    (void)c;
    int64_t now = now_sec();
    char when[64], q[300];
    if (g_present && result_unseen()) result_notice(b, now);
    bool busy = g_present && installing(now);
    bool sent = busy && str_eq(g_asked, "install-file"); /* (an uploaded download) */
    if (busy) {
        const char *v = g_res_ver[0] && (str_eq(g_state, "fetching") || str_eq(g_state, "installing")) ? g_res_ver
                        : str_starts(g_asked, "install ")                                             ? g_asked + 8
                                                                                                      : "";
        char doing[80];
        if (str_eq(g_state, "installing") && answered()) snprintf(doing, sizeof doing, "Installing Hemma %s", v);
        else if (sent) str_copy(doing, sizeof doing, "Checking the download you sent");
        else snprintf(doing, sizeof doing, "Downloading and checking Hemma %s", v);
        H(b, "<p class=\"notice system-up-note\" id=\"system-up-busy\" role=\"status\">%s… Hemma restarts by itself "
             "when it's in; this page comes back on its own.</p>",
          doing);
    }

    ui_card(b, "This Hemma", NULL, NULL);
    const char *built = strchr(HEMMA_VERSION_FULL, '('); /* "0.21.0 (8a89a31c0131)": the commit */
    char commit[64] = "";
    if (built) str_ncopy(commit, sizeof commit, built + 1, strcspn(built + 1, ")"));
    H(b, "<p class=\"system-up-v\">Hemma <b>%s</b>", HEMMA_VERSION);
    if (commit[0]) H(b, " <span class=\"muted small\">built from %s</span>", commit);
    buf_puts(b, "</p>");
    if (!g_present) {
        how_by_hand(b);
        H(b, "<p><a class=\"btn\" href=\"%s\" target=\"_blank\" rel=\"noopener noreferrer\">Releases on GitHub</a></p>",
          RELEASES);
    }
    ui_card_end(b);

    if (g_present) {
        ui_card(b, "New versions", NULL, NULL);
        if (silent(now))
            buf_puts(b, "<p class=\"notice system-up-bad\" id=\"system-up-silent\">The update helper didn't answer. On the "
                        "Pi: <code>sudo systemctl enable --now hemma-update.path</code> - or update there with "
                        "<code>sudo hemma-upgrade</code>.</p>");
        else if (!answered() && str_eq(g_asked, "check"))
            buf_puts(b, "<p id=\"system-up-state\" role=\"status\">Asking github.com…</p>");
        else if (out_now()) {
            H(b, "<p id=\"system-up-state\"><b>Hemma %s is out</b> (this is %s). <a href=\"%s/tag/v%s\" target=\"_blank\" "
                 "rel=\"noopener noreferrer\">What's new</a></p>",
              g_latest, HEMMA_VERSION, RELEASES, g_latest);
            snprintf(q, sizeof q, "Install Hemma %s? Hemma restarts for a moment. If the new one doesn't start, the "
                                  "update helper goes back to this one by itself.", g_latest);
            ui_form_confirm(b, "system.update_install", "system-up-install", q);
            ui_hidden(b, "v", g_latest);
            H(b, "<button class=\"btn\"%r>Install Hemma %s</button>", busy ? " disabled" : "", g_latest);
            ui_form_end(b);
        } else if (g_latest[0]) {
            system_when(g_latest_at, now, when, sizeof when);
            H(b, "<p id=\"system-up-state\">This is the newest Hemma (checked %s).</p>", when);
        } else if (g_latest_why[0]) {
            system_when(g_latest_at, now, when, sizeof when);
            H(b, "<p id=\"system-up-state\" class=\"system-up-bad\">The check %s didn't work: %s.</p>", when, g_latest_why);
        } else buf_puts(b, "<p id=\"system-up-state\" class=\"muted\">Not checked yet.</p>");
        ui_form(b, "system.update_check", "inline");
        H(b, "<button class=\"btn ghost\"%r>Check now</button>", busy ? " disabled" : "");
        ui_form_end(b);
        ui_form(b, "system.update_auto", "system-up-auto");
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> Check once a day</label>",
          setting_int(SET_CHECK, 0) == 1 ? " checked" : "");
        ui_form_end(b);
        buf_puts(b, "<p class=\"muted small\">Checking asks github.com one thing: which version is newest. Nothing about "
                    "your home leaves it, and nothing is installed until you say so. Only what the Hemma project signed "
                    "is installed; the program and a copy of the data are kept, and if the new Hemma doesn't start, it "
                    "goes back to them by itself.</p>");
        ui_card_end(b);

        const char *dl = plat_bundle();
        ui_card(b, "Install from a file", NULL, NULL);
        char file[64] = "the download for this machine";
        if (dl) snprintf(file, sizeof file, "%s.tar.gz", dl);
        H(b, "<p>No internet here? Download <b>%s</b> from <a href=\"%s\" target=\"_blank\" "
             "rel=\"noopener noreferrer\">the releases page</a> on any computer, then choose it here.</p>",
          file, RELEASES);
        lock_take(&g_lock);
        bool receiving = g_up == UP_PLANNED && g_up_got > 0;
        int pct = g_up_size > 0 ? (int)(g_up_got * 100 / g_up_size) : 0;
        char upname[120], why[200];
        str_copy(upname, sizeof upname, g_up_name);
        str_copy(why, sizeof why, g_up_why);
        lock_give(&g_lock);
        if (receiving) H(b, "<p id=\"system-up-file\" role=\"status\">Receiving %s… %d%%</p>", upname, pct);
        else if (sent)
            H(b, "<p id=\"system-up-file\" role=\"status\">%s is in: the update helper checks it now - only a "
                 "download the Hemma project signed is installed.</p>",
              upname[0] ? upname : "The download");
        else if (why[0]) H(b, "<p id=\"system-up-file\" class=\"system-up-bad\">%s</p>", why);
        H(b, "<form class=\"system-up-form\" data-upload=\"/system/update\" data-empty=\"Choose the download first\">"
             "<label class=\"field\"><span>The download</span><input type=\"file\" name=\"bundle\" "
             "accept=\".gz,.tgz,application/gzip\"></label><button class=\"btn\"%r>Install</button></form>"
             "<p class=\"muted small system-up-nojs\">(Choosing a file here needs JavaScript.)</p>",
          busy ? " disabled" : "");
        ui_card_end(b);
    }

    if (g_apt) {
        ui_card(b, "The operating system", NULL, NULL);
        if (g_unattended && g_os_last) {
            system_when(g_os_last, now, when, sizeof when);
            H(b, "<p>Its security updates install by themselves (last looked for %s).</p>", when);
        } else if (g_unattended) buf_puts(b, "<p>Its security updates install by themselves.</p>");
        else
            buf_puts(b, "<p>Its security updates aren't installed by themselves. To have that done every day, run this "
                        "once on the machine:</p><p><code>sudo apt install unattended-upgrades</code></p>");
        ui_card_end(b);
    }
}

/* --------------------------------------------------------------- actions */

static bool admin_ok(Ctx *c) {
    if (is_admin(c)) return true;
    ctx_fail(c, "Only an admin can do that");
    return false;
}

static bool helper_ok(Ctx *c) {
    g_present = present();
    if (g_present) return true;
    ctx_fail(c, "This install has no update helper - the Updates tab says how to update it");
    return false;
}

void system_update_act_check(Ctx *c, Fields *f) {
    (void)f;
    if (!admin_ok(c) || !helper_ok(c)) return;
    if (installing(now_sec())) {
        ctx_fail(c, "An update is under way");
        return;
    }
    if (!ask("check")) {
        ctx_fail(c, "Couldn't ask the update helper (see the log)");
        return;
    }
    ctx_ok(c, "Asking github.com…");
}

void system_update_act_auto(Ctx *c, Fields *f) {
    if (!admin_ok(c)) return;
    bool on = str_eq(fields_str(f, "on"), "1");
    setting_set(SET_CHECK, on ? "1" : "0");
    log_info("updates: %s turned the daily check %s", c->me->name, on ? "on" : "off");
    ctx_ok(c, on ? "Hemma checks for a new version once a day" : "No more daily checks");
}

void system_update_act_install(Ctx *c, Fields *f) {
    if (!admin_ok(c) || !helper_ok(c)) return;
    const char *v = fields_str(f, "v");
    if (installing(now_sec())) {
        ctx_fail(c, "An update is under way");
        return;
    }
    if (!str_eq(v, g_latest) || !out_now()) {
        ctx_fail(c, "That isn't a newer version - check again");
        return;
    }
    char what[40];
    snprintf(what, sizeof what, "install %s", g_latest);
    if (!ask(what)) {
        ctx_fail(c, "Couldn't ask the update helper (see the log)");
        return;
    }
    log_info("updates: %s asked for Hemma %s", c->me->name, g_latest);
    activity_add(c->me->id, "⬆️", "/system/updates", "%s started the update to Hemma %s", c->me->name, g_latest);
    ctx_ok(c, "Downloading Hemma %s…", g_latest);
}

void system_update_act_seen(Ctx *c, Fields *f) {
    (void)f;
    if (!admin_ok(c)) return;
    char at[24];
    snprintf(at, sizeof at, "%lld", (long long)g_res_at);
    setting_set(SET_SEEN, at);
    ctx_ok(c, "Noted");
}

/* ------------------------------------------------ the uploaded download */

static void reply(Conn *conn, int status, const char *error, bool stop) {
    Buf b = {0};
    if (error) {
        buf_puts(&b, "{\"error\":");
        json_str(&b, error);
        buf_printf(&b, ",\"stop\":%s}", stop ? "true" : "false");
    } else buf_puts(&b, "{\"ok\":true}");
    conn_reply(conn, status, "application/json", b.p, b.len, "Cache-Control: no-store\r\n");
    buf_free(&b);
}

static void plan(Ctx *c, Conn *conn, HttpReq *r) {
    JsonTok tok[48];
    Json j;
    char name[300] = "";
    long long size = -1;
    if (r->body && json_parse(&j, r->body, r->body_len, tok, countof(tok)) && json_type(&j, 0) == JSON_OBJECT) {
        int files = json_get(&j, 0, "files");
        if (json_type(&j, files) == JSON_ARRAY && j.tok[files].count == 1) {
            int one = json_index(&j, files, 0);
            json_string(&j, json_index(&j, one, 0), name, sizeof name);
            json_integer(&j, json_index(&j, one, 1), &size);
        }
    }
    const char *base = strrchr(name, '/'), *err = NULL;
    base = base ? base + 1 : name;
    int64_t now = now_sec();
    if (!base[0] || size < 0) err = "Choose one file: the download for this machine";
    else if (!str_iends(base, ".tar.gz") && !str_iends(base, ".tgz")) err = "That isn't a Hemma download - they end in .tar.gz";
    else if (size == 0 || size > FILE_MAX) err = "That file is far too big for a Hemma download";
    else if (installing(now)) err = "An update is under way";
    if (err) {
        reply(conn, installing(now) ? 409 : 400, err, true);
        return;
    }
    lock_take(&g_lock);
    if (g_writing || (g_up == UP_PLANNED && now - g_up_touched < PLAN_IDLE_S && g_up_person != c->me->id)) {
        lock_give(&g_lock);
        reply(conn, 409, "Someone else is uploading a download right now", true);
        return;
    }
    rand_hex(g_batch, 8);
    g_up = UP_PLANNED;
    g_up_person = c->me->id;
    str_copy(g_up_who, sizeof g_up_who, c->me->name);
    str_copy(g_up_name, sizeof g_up_name, base);
    g_up_why[0] = '\0';
    g_up_size = size;
    g_up_got = 0;
    g_up_touched = now;
    Buf out = {0};
    buf_printf(&out, "{\"batch\":\"%s\",\"todo\":[[0,0]]}", g_batch);
    lock_give(&g_lock);
    conn_reply(conn, 200, "application/json", out.p, out.len, "Cache-Control: no-store\r\n");
    buf_free(&out);
}

typedef struct {
    sock_t s;
    Buf head, body;
    int64_t len;
    char part[760], full[760];
} Job;

static void job_reply(sock_t s, int status, const char *error) {
    Buf body = {0}, out = {0};
    if (error) {
        buf_puts(&body, "{\"error\":");
        json_str(&body, error);
        buf_puts(&body, ",\"stop\":true}");
    } else buf_puts(&body, "{\"ok\":true}");
    http_reply(&out, status, "application/json", body.p, body.len, "Cache-Control: no-store\r\n", false);
    const char *p = out.p;
    size_t n = out.len;
    for (int waited = 0; n && waited < 10000;) {
        long k = sock_send(s, p, n);
        if (k > 0) p += k, n -= (size_t)k, waited = 0;
        else if (k < 0 && sock_again(sock_errno())) {
            pollfd_t pf = {.fd = s, .events = POLLOUT};
            if (plat_poll(&pf, 1, 1000) == 0) waited += 1000;
        } else break;
    }
    buf_free(&body);
    buf_free(&out);
}

static void receive(void *arg) {
    Job *j = arg;
    const char *fail = NULL;
    int status = 200;
    bool dropped = false;
    sock_nonblock(j->s, true);
    if (j->head.len) {
        sock_send(j->s, j->head.p, j->head.len); /* (rare: what the connection still owed) */
    }
    FILE *f = file_open(j->part, "wb");
    if (!f) fail = "Couldn't save the file on the server", status = 500;
    int64_t got = 0;
    uint8_t magic[2] = {0, 0};
    if (f && j->body.len) {
        size_t n = (size_t)MIN((int64_t)j->body.len, j->len);
        if (fwrite(j->body.p, 1, n, f) != n) fail = "The server's disk is full", status = 507;
        memcpy(magic, j->body.p, MIN(n, (size_t)2));
        got = (int64_t)n;
    }
    char buf[16384];
    for (int waited = 0; !fail && got < j->len;) {
        long k = sock_recv(j->s, buf, (size_t)MIN((int64_t)sizeof buf, j->len - got));
        if (k > 0) {
            if (got < 2) memcpy(magic + got, buf, (size_t)MIN((int64_t)k, 2 - got));
            if (fwrite(buf, 1, (size_t)k, f) != (size_t)k) {
                fail = "The server's disk is full", status = 507;
                break;
            }
            got += k;
            waited = 0;
            lock_take(&g_lock);
            g_up_got = got;
            g_up_touched = now_sec();
            lock_give(&g_lock);
            continue;
        }
        if (k == 0 || !sock_again(sock_errno())) {
            dropped = true;
            fail = "The connection dropped", status = 0;
            break;
        }
        pollfd_t pf = {.fd = j->s, .events = POLLIN};
        int rc = plat_poll(&pf, 1, 1000);
        if (rc < 0) dropped = true, fail = "The connection dropped", status = 0;
        else if (rc == 0 && (waited += 1000) >= STALL_MS) fail = "Nothing arrived for a minute", status = 408;
    }
    if (f) {
        if (!file_sync(f) && !fail) fail = "The server couldn't save the file", status = 507;
        fclose(f);
    }
    if (!fail && (magic[0] != 0x1f || magic[1] != 0x8b)) fail = "That isn't a Hemma download (not a .tar.gz)", status = 415;
    if (!fail && !file_replace(j->part, j->full)) fail = "Couldn't save the file on the server", status = 500;
    if (fail) file_remove(j->part);
    lock_take(&g_lock);
    g_writing = false;
    g_up_got = got;
    if (!fail) g_up = UP_RECEIVED;
    else if (!dropped) { /* (a dropped connection: the browser sends it again) */
        g_up = UP_IDLE;
        str_copy(g_up_why, sizeof g_up_why, fail);
    }
    lock_give(&g_lock);
    if (status) job_reply(j->s, status, fail);
    sock_close(j->s);
    buf_free(&j->head);
    buf_free(&j->body);
    free(j);
    app_tick_soon();
}

static void put(Conn *conn, HttpReq *r, int me) {
    Fields q = {0};
    fields_parse(&q, r->query, strlen(r->query));
    long long at = fields_int(&q, "at", -1);
    fields_free(&q);
    lock_take(&g_lock);
    const char *err = NULL;
    int status = 409;
    if (g_up != UP_PLANNED || g_up_person != me) err = "That upload is over - choose the file again", status = 404;
    else if (g_writing) err = "busy";
    else if (at != 0 || r->content_length != g_up_size) err = "The file changed size - choose it again", status = 400;
    if (err) {
        lock_give(&g_lock);
        reply(conn, status, err, !str_eq(err, "busy"));
        return;
    }
    g_writing = true;
    g_up_got = 0;
    Job *j = xcalloc(1, sizeof *j);
    j->len = g_up_size;
    lock_give(&g_lock);
    path_in(j->part, sizeof j->part, g_ask, "upload.tar.gz.part");
    path_in(j->full, sizeof j->full, g_ask, "upload.tar.gz");
    Detached d;
    if (!r->streaming && r->body_len) buf_put(&j->body, r->body, r->body_len);
    conn_detach(conn, r, &d);
    j->s = d.fd;
    buf_put(&j->body, d.body.p, d.body.len);
    buf_free(&d.body);
    j->head = d.unsent;
    if (!thread_spawn(receive, j)) {
        lock_take(&g_lock);
        g_writing = false;
        lock_give(&g_lock);
        job_reply(j->s, 503, "busy");
        sock_close(j->s);
        buf_free(&j->head);
        buf_free(&j->body);
        free(j);
    }
}

bool system_update_http(Ctx *c, Conn *conn, HttpReq *r) {
    static const char pre[] = "/system/update/";
    if (!str_starts(r->path, pre)) return false;
    const char *rest = r->path + sizeof pre - 1;
    if (!is_admin(c)) {
        reply(conn, 403, "Only an admin can install updates", true);
        return true;
    }
    if (!http_same_origin(r)) {
        reply(conn, 403, "cross-site request refused", true);
        return true;
    }
    g_present = present();
    if (!g_present) {
        reply(conn, 409, "This install has no update helper - the Updates tab says how to update it", true);
        return true;
    }
    if (str_eq(rest, "plan")) {
        if (!str_eq(r->method, "POST") || r->streaming) reply(conn, 405, "POST only", true);
        else plan(c, conn, r);
        return true;
    }
    lock_take(&g_lock);
    bool ours = strlen(rest) == 18 && strncmp(rest, g_batch, 16) == 0 && str_eq(rest + 16, "/0") && g_batch[0];
    lock_give(&g_lock);
    if (!ours) {
        reply(conn, 404, "That upload is over - choose the file again", true);
        return true;
    }
    if (str_eq(r->method, "PUT")) put(conn, r, c->me->id);
    else if (str_eq(r->method, "GET")) {
        lock_take(&g_lock);
        bool writing = g_writing;
        lock_give(&g_lock);
        const char *s = writing ? "{\"at\":0,\"busy\":true}" : "{\"at\":0}"; /* (no resuming: a download is small) */
        conn_reply(conn, 200, "application/json", s, strlen(s), "Cache-Control: no-store\r\n");
    } else reply(conn, 405, "PUT or GET", true);
    return true;
}

/* ------------------------------------------------------------------ tests */

void system_test_update_dirs(const char *ask_dir, const char *answers) {
    g_test = ask_dir != NULL;
    str_copy(g_ask, sizeof g_ask, ask_dir ? ask_dir : "");
    str_copy(g_ans, sizeof g_ans, answers ? answers : "");
    g_present_at = g_read_at = 0;
    g_present = g_test && present();
    g_apt = false;
}

void system_test_update_os(bool apt, bool unattended, int64_t last) {
    g_apt = apt;
    g_unattended = unattended;
    g_os_last = last;
}
