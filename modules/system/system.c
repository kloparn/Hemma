/* modules/system — "is everything working?", for admins.
 *
 * /system         checks in plain words: the core's own (version and uptime, the address to
 *                 open, saving, disk space, the password secret, sign-in, the log) and every
 *                 module's health hook (core/app.h) - plus a getting-started list that ticks
 *                 itself from real data until it's hidden
 * /system/log     the server's own log, live (log.c): filter, search, download, detailed logging
 * /system/help    where to look when something's wrong, on this machine, and what to send
 * /system/log.txt the log as a file (the http hook)
 * The home dashboard gets a small card only while a check needs a look.
 *
 * Admins only (MOD_ADMIN): log lines carry addresses and usernames. Other modules never link
 * here - they report through their health hook. */
#include "system.h"
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/ui.h"

#define SHOW_LINES 200       /* newest log lines on the page (all of them in the download) */
#define LIVE_EVERY_MS 1500   /* new log lines re-render /system screens at most this often */
#define SET_SALT "system.salt_saved"
#define SET_HIDDEN "system.start_hidden"
#define SET_PHONE "system.phone"

static int64_t g_started; /* unix seconds */
static LogLevel g_level_at_start = LOG_INFO;
static bool g_level_known;
static char g_addr[3][96];
static int g_naddr;
static bool g_addr_fixed; /* tests */
static int64_t g_addr_at, g_disk_at, g_phone_at, g_shown_at; /* monotonic ms */
static int64_t g_free = -1, g_total = -1, g_test_free = -1;
static uint32_t g_shown_seq;

/* --------------------------------------------------------------- helpers */

static void size_text(int64_t n, char *out, size_t cap) {
    double g = (double)n / 1073741824.0;
    if (g >= 1024) snprintf(out, cap, "%.1f TB", g / 1024);
    else if (g >= 10) snprintf(out, cap, "%.0f GB", g);
    else if (g >= 1) snprintf(out, cap, "%.1f GB", g);
    else snprintf(out, cap, "%lld MB", (long long)(n >> 20));
}

static void span_text(int64_t s, char *out, size_t cap) { /* "12 min", "5 h", "3 days" */
    if (s < 60) snprintf(out, cap, "less than a minute");
    else if (s < 3600) snprintf(out, cap, "%lld min", (long long)(s / 60));
    else if (s < 2 * 86400) snprintf(out, cap, "%lld h", (long long)(s / 3600));
    else snprintf(out, cap, "%lld days", (long long)(s / 86400));
}

static void when_text(int64_t t, int64_t now, char *out, size_t cap) { /* "14:30" today, else "Mon 12 Oct 14:30" */
    DateTime d = dt_local(t);
    if (day_start(t) == day_start(now)) snprintf(out, cap, "%02d:%02d", d.hour % 100, d.min % 100);
    else
        snprintf(out, cap, "%s %d %s %02d:%02d", weekday_name(d.wday, true), d.day % 100, month_name(d.mon, true),
                 d.hour % 100, d.min % 100);
}

static const char *env_path(void) {
    const char *p = app_config()->env_file;
    return p && *p ? p : ".env";
}

static const char *data_path(void) {
    const char *p = app_config()->data_dir;
    return p && *p ? p : "data";
}

static bool relative(const char *p) { return !(p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':')); }

/* How Hemma runs here, from what the process was started with (not how it was built): systemd
   gives services an INVOCATION_ID, launchd its agents an XPC_SERVICE_NAME. */
typedef enum { RUN_TERMINAL, RUN_SYSTEMD, RUN_LAUNCHD, RUN_WINDOWS } RunKind;

static RunKind run_kind(void) {
    const char *v = plat_getenv("INVOCATION_ID");
    if (v && *v) return RUN_SYSTEMD;
    v = plat_getenv("XPC_SERVICE_NAME");
    if (v && str_starts(v, "local.hemma")) return RUN_LAUNCHD;
    v = plat_getenv("OS");
    if (v && str_eq(v, "Windows_NT")) return RUN_WINDOWS;
    return RUN_TERMINAL;
}

static void refresh_addresses(int64_t now) {
    g_addr_at = now ? now : 1;
    if (!g_addr_fixed) g_naddr = app_addresses(g_addr, countof(g_addr));
}

static void refresh_disk(int64_t now) {
    g_disk_at = now ? now : 1;
    if (g_test_free >= 0) {
        g_free = g_test_free;
        g_total = MAX(g_test_free, 100LL << 30);
    } else if (!path_disk_space(data_path(), &g_free, &g_total)) {
        g_free = g_total = -1;
    }
}

static bool phone_seen(void) {
    if (setting_int(SET_PHONE, 0) == 1) return true;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        const Session *s = tbl_at(&T_session, i);
        if (strstr(s->agent, "iPhone") || strstr(s->agent, "iPad") || strstr(s->agent, "Android")) return true;
    }
    return false;
}

static bool local_only(void) { /* listening on 127.x: no other device can reach it */
    const char *host = app_config()->host;
    uint32_t bound = 0;
    return host && ip_parse(host, &bound) && (bound >> 24) == 127;
}

/* ---------------------------------------------------------------- checks */

static void own_checks(Ctx *c, Health *h) {
    char d[320], todo[240], a[64], b[64];
    int64_t now = now_sec();

    /* Hemma itself */
    int64_t last = 0;
    bool unclean = system_log_unclean(&last);
    span_text(now - g_started, a, sizeof a);
    when_text(g_started, now, b, sizeof b);
    int n = snprintf(d, sizeof d, "Version %s, running for %s (since %s).", HEMMA_VERSION_FULL, a, b);
    if (unclean && n > 0 && (size_t)n < sizeof d) {
        when_text(last, now, a, sizeof a);
        snprintf(d + n, sizeof d - (size_t)n,
                 " The time before, it stopped without warning after %s - a power cut or a crash? The log has the lines "
                 "from before.",
                 a);
    }
    health_add(h, unclean ? HEALTH_INFO : HEALTH_OK, "Hemma", d, NULL, unclean ? "/system/log" : NULL);

    /* where to open it */
    if (!g_addr_at && !app_testing()) refresh_addresses(now_ms());
    if (local_only()) {
        snprintf(d, sizeof d, "Only this computer can open Hemma: it listens on %s.", app_config()->host);
        health_add(h, app_dev_mode() ? HEALTH_INFO : HEALTH_WARN, "Address", d,
                   "Start it without --host (or with --host 0.0.0.0) so phones and the TV can open it too.", NULL);
    } else if (!g_naddr) {
        health_add(h, HEALTH_WARN, "Address", "Hemma can't find its address on the home network.",
                   "Check this computer's network cable or Wi-Fi - other devices need it to find Hemma.", NULL);
    } else {
        snprintf(d, sizeof d, "Phones, computers and the TV open %s%s%s. Tip: reserve that address in your router so "
                              "it never changes (docs/home-network.md).",
                 g_addr[0], g_naddr > 1 ? " or " : "", g_naddr > 1 ? g_addr[1] : "");
        health_add(h, HEALTH_OK, "Address", d, NULL, NULL);
    }

    /* saving */
    if (store_failed()) {
        snprintf(d, sizeof d, "Saving changes to the disk failed - is it full? Recent changes are only in memory "
                              "and will be lost when Hemma stops.");
        snprintf(todo, sizeof todo, "Make room on the disk with %s, then restart Hemma. The log says what failed.",
                 data_path());
        health_add(h, HEALTH_BAD, "Saving", d, todo, "/system/log?f=warn");
    } else {
        snprintf(d, sizeof d, "Every change is saved in %s/hemma.db.", data_path());
        health_add(h, HEALTH_OK, "Saving", d, NULL, NULL);
    }

    /* room where the database lives */
    if (!g_disk_at) refresh_disk(now_ms());
    if (g_free >= 0 && g_total > 0) {
        size_text(g_free, a, sizeof a);
        int used = (int)CLAMP((g_total - g_free) * 100 / g_total, 0, 100);
        /* what's left counts, not the share: 5% of a big disk is plenty for a database and a log */
        int st = g_free < (300LL << 20) ? HEALTH_BAD : g_free < (2LL << 30) ? HEALTH_WARN : HEALTH_OK;
        snprintf(d, sizeof d, "%s free on the disk with Hemma's data (%d%% used).", a, used);
        health_add(h, st, "Disk space", d,
                   st ? "Make room on that disk: when it's full, Hemma can't save changes (old videos? the Watch "
                        "library settings show what takes room)."
                      : NULL,
                   NULL);
    }

    /* signing in */
    if (!auth_ready()) {
        snprintf(todo, sizeof todo, "Add it to the settings file (%s) - hemma --gen-salt makes one - and restart Hemma.",
                 env_path());
        health_add(h, HEALTH_BAD, "Sign-in",
                   "There's no password secret (HEMMA_AUTH_SALT), so nobody can sign in or be added.", todo, NULL);
    } else if (auth_salt_mismatch()) {
        snprintf(todo, sizeof todo, "Put the original HEMMA_AUTH_SALT line back into %s and restart Hemma "
                                    "(docs/accounts.md, \"Locked out\").",
                 env_path());
        health_add(h, HEALTH_BAD, "Sign-in",
                   "The password secret in the settings file (HEMMA_AUTH_SALT) isn't the one the passwords were made "
                   "with, so nobody can sign in with a password.",
                   todo, NULL);
    } else if (auth_setup_open()) {
        health_add(h, HEALTH_WARN, "Sign-in",
                   "The welcome page is still open: whoever has the setup code can make the first admin.",
                   "Open Hemma and create the admin account - the setup code is in the log.", "/system/log");
    } else {
        int can = 0, admins = 0;
        for (int i = 0; i < account_count(); i++) {
            const Person *p = account_at(i);
            if (!p->username[0] || !p->pass[0]) continue;
            can++;
            admins += p->role == ROLE_ADMIN;
        }
        snprintf(d, sizeof d, "%d account%s can sign in, %d of them admin%s.%s", can, can == 1 ? "" : "s", admins,
                 admins == 1 ? "" : "s",
                 admins == 1 ? " Tip: a second admin can help if you're ever locked out." : "");
        health_add(h, HEALTH_OK, "Sign-in", d, NULL, NULL);
    }

    /* the log */
    char why[120];
    if (system_log_write_failed(why, sizeof why)) {
        snprintf(d, sizeof d, "Hemma can't keep its log file (%s): %s.", system_log_path(), why);
        health_add(h, HEALTH_WARN, "Log", d,
                   "Check that the data folder can be written to and the disk isn't full. The log below still works.",
                   "/system/log");
    }
    int errs = system_log_errors_since(now - 86400);
    if (errs) {
        snprintf(d, sizeof d, "%d problem%s logged in the last day.", errs, errs == 1 ? "" : "s");
        health_add(h, HEALTH_WARN, "Problems in the log", d, "Have a look: each line says what went wrong.",
                   "/system/log?f=warn");
    } else {
        health_add(h, HEALTH_OK, "Problems in the log", "None logged since Hemma started.", NULL, "/system/log");
    }
}

/* The core's checks first, then every module's (their steps come along: see the list below). */
static void collect(Ctx *c, Health *h) {
    own_checks(c, h);
    health_collect(c, h);
}

static int count_looks(const Health *h) {
    int n = 0;
    for (int i = 0; i < h->n; i++) n += !h->items[i].step && h->items[i].state >= HEALTH_WARN;
    return n;
}

static void check_row(Buf *b, const HealthItem *it) {
    static const char *const cls[] = {"ok", "info", "warn", "bad"};
    static const char *const badge[] = {"<span class=\"badge ok\">OK</span>", "<span class=\"badge\">Note</span>",
                                        "<span class=\"badge warn\">Needs a look</span>",
                                        "<span class=\"badge bad\">Problem</span>"};
    uint64_t key = hash64_str(it->title) ^ (hash64_str(it->module ? it->module : "system") * 31);
    H(b, "<li class=\"row system-check system-%s\" id=\"system-c%X\"><div class=\"row-main\"><span class=\"system-ct\">%r"
         "<b>%s</b></span>",
      cls[it->state], key, badge[it->state], it->title);
    if (it->detail[0]) H(b, "<span class=\"system-cd\">%s</span>", it->detail);
    if (it->todo[0] && it->state != HEALTH_OK) {
        if (it->link[0]) H(b, "<a class=\"system-todo\" href=\"%s\">%s</a>", it->link, it->todo);
        else H(b, "<span class=\"system-todo\">%s</span>", it->todo);
    } else if (it->link[0]) {
        H(b, "<a class=\"system-more\" href=\"%s\">Have a look</a>", it->link);
    }
    buf_puts(b, "</div></li>");
}

static void checks_card(Buf *b, const Health *h) {
    buf_puts(b, "<section class=\"card system-checks-card\" id=\"system-checks\"><header class=\"card-h\"><h2>Checks"
                "</h2></header><ul class=\"rows system-checks\">");
    for (int st = HEALTH_BAD; st >= HEALTH_WARN; st--) /* what needs a look first */
        for (int i = 0; i < h->n; i++)
            if (!h->items[i].step && h->items[i].state == st) check_row(b, &h->items[i]);
    for (int i = 0; i < h->n; i++)
        if (!h->items[i].step && h->items[i].state < HEALTH_WARN) check_row(b, &h->items[i]);
    buf_puts(b, "</ul></section>");
}

/* ------------------------------------------------------- getting started */

typedef struct {
    bool done, salt;
    char title[80], detail[400], link[160];
} Step;

static void step(Step *s, int *n, int max, bool done, const char *title, const char *detail, const char *link) {
    if (*n >= max) return;
    Step *x = &s[(*n)++];
    memset(x, 0, sizeof *x);
    x->done = done;
    str_copy(x->title, sizeof x->title, title);
    str_copy(x->detail, sizeof x->detail, detail);
    str_copy(x->link, sizeof x->link, link);
}

/* In order: name the home, the household, what modules ask for (media: a library folder), a
   phone, recovery codes, a copy of the secret. Each ticks itself from what Hemma knows -
   except the secret's copy, which only you can know about ("Done"). */
static int build_steps(Ctx *c, Step *s, int max, const Health *h) {
    int n = 0;
    char d[400];
    step(s, &n, max, setting("household.name", NULL) != NULL, "Name your home",
         "The name at the top of every page - \"The Svenssons\", \"Villa Kulla\"...", "/people");
    step(s, &n, max, person_count() > 1, "Add the household",
         "Everyone who lives here, even the ones too young to sign in: each gets a colour, and a sign-in if they like.",
         "/people?add=1");
    for (int i = 0; i < h->n; i++)
        if (h->items[i].step) step(s, &n, max, h->items[i].state == HEALTH_OK, h->items[i].title, h->items[i].detail,
                                   h->items[i].link);
    snprintf(d, sizeof d, "On a phone at home, open %s and sign in. Tip: add it to the home screen - it opens like an "
                          "app.",
             g_naddr && !local_only() ? g_addr[0] : "Hemma's address (see Address in the Checks)");
    step(s, &n, max, phone_seen(), "Open Hemma on a phone", d, NULL);
    step(s, &n, max, c->me && auth_codes_left(c->me->id) > 0, "Save your recovery codes",
         "They get you back in if you forget your password. Keep them somewhere safe, away from this server.",
         "/people#people-codes-h");
    snprintf(d, sizeof d, "Every password depends on the HEMMA_AUTH_SALT line in the settings file (%s%s). Keep a copy "
                          "of that line somewhere safe, apart from Hemma's data - a password manager, or a note in a "
                          "drawer. A backup of the database can't sign anyone in without it.%s",
             env_path(), relative(env_path()) ? ", in the folder Hemma was started from" : "",
             run_kind() == RUN_SYSTEMD ? " On the server: sudo grep HEMMA_AUTH_SALT /etc/hemma/hemma.env" : "");
    int before = n;
    step(s, &n, max, setting_int(SET_SALT, 0) == 1, "Keep a copy of the password secret", d, NULL);
    if (n > before) s[n - 1].salt = true;
    return n;
}

static void steps_card(Ctx *c, Buf *b, const Health *h) {
    Step s[16];
    int n = build_steps(c, s, countof(s), h), done = 0;
    for (int i = 0; i < n; i++) done += s[i].done;
    H(b, "<section class=\"card system-start\" id=\"system-start\"><header class=\"card-h\"><h2>Getting started</h2>"
         "<span class=\"muted small\">%d of %d done</span></header><ol class=\"rows system-steps\">",
      done, n);
    for (int i = 0; i < n; i++) {
        H(b, "<li class=\"row system-step%r\" id=\"system-s%d\"><span class=\"system-tick\" title=\"%s\">",
          s[i].done ? " system-done" : "", i, s[i].done ? "Done" : "Still to do");
        if (s[i].done) ui_icon(b, "check");
        buf_puts(b, "</span><div class=\"row-main\">");
        if (s[i].link[0] && !s[i].done) H(b, "<a class=\"system-st\" href=\"%s\">%s</a>", s[i].link, s[i].title);
        else H(b, "<b class=\"system-st\">%s</b>", s[i].title);
        if (!s[i].done) H(b, "<small>%s</small>", s[i].detail);
        if (s[i].salt && !s[i].done) {
            buf_puts(b, "<div class=\"system-step-a\">");
            ui_act(b, "system.salt_saved", "btn sm", "Done - I have a copy", NULL, NULL, NULL);
            buf_puts(b, "</div>");
        }
        buf_puts(b, "</div></li>");
    }
    buf_puts(b, "</ol><div class=\"system-start-f\">");
    if (done == n) buf_puts(b, "<p>All done - Hemma is set up.</p>");
    ui_act(b, "system.start_hide", done == n ? "btn" : "btn ghost sm", "Hide this", NULL, NULL, NULL);
    buf_puts(b, "</div></section>");
}

/* ----------------------------------------------------------------- pages */

static void tabs(Buf *b, const char *on) {
    static const char *const href[] = {"/system", "/system/log", "/system/help"};
    static const char *const label[] = {"Checks", "Log", "Help"};
    buf_puts(b, "<nav class=\"seg system-tabs\" aria-label=\"System\">");
    for (int i = 0; i < 3; i++)
        H(b, "<a href=\"%s\"%r>%s</a>", href[i], str_eq(on, label[i]) ? " class=\"on\" aria-current=\"page\"" : "",
          label[i]);
    buf_puts(b, "</nav>");
}

static void page_checks(Ctx *c, Buf *b) {
    Health h = {0};
    collect(c, &h);
    int worst = health_worst(&h), looks = count_looks(&h);
    char up[64];
    span_text(now_sec() - g_started, up, sizeof up);
    if (worst == HEALTH_BAD)
        H(b, "<p class=\"notice bad system-sum\" id=\"system-sum\" role=\"alert\">Something isn't working - %d "
             "thing%r below need%r a look.</p>",
          looks, looks == 1 ? "" : "s", looks == 1 ? "s" : "");
    else if (worst == HEALTH_WARN)
        H(b, "<p class=\"notice system-sum system-look\" id=\"system-sum\">%d thing%r below need%r a look. Everything "
             "else works.</p>",
          looks, looks == 1 ? "" : "s", looks == 1 ? "s" : "");
    else
        H(b, "<p class=\"notice system-sum system-good\" id=\"system-sum\">Everything works. Hemma %s has been running for "
             "%s.</p>",
          HEMMA_VERSION, up);
    bool start = setting_int(SET_HIDDEN, 0) != 1;
    if (looks || !start) checks_card(b, &h);
    if (start) steps_card(c, b, &h);
    if (!looks && start) checks_card(b, &h);
    if (!start) {
        buf_puts(b, "<div class=\"system-again\">");
        ui_act(b, "system.start_show", "btn ghost sm", "Show the getting-started list again", NULL, NULL, NULL);
        buf_puts(b, "</div>");
    }
    health_free(&h);
}

static const char *query(const Ctx *c) {
    const char *q = ctx_live(c, "system.q");
    return *q ? q : ctx_q(c, "q");
}

static void line_time(Buf *b, int64_t ts, int64_t now) {
    DateTime d = dt_local(ts);
    if (day_start(ts) != day_start(now)) H(b, "%d %s ", d.day, month_name(d.mon, true));
    H(b, "%02d:%02d:%02d", d.hour, d.min, d.sec);
}

static void log_line(Buf *b, const LogLine *l, int64_t now) {
    if (l->level == '-') {
        H(b, "<li class=\"system-div\" id=\"system-l%L\"><span>%s · ", (long long)l->seq, l->text);
        line_time(b, l->ts, now);
        buf_puts(b, "</span></li>");
        return;
    }
    const char *cls = l->level == 'E' ? " system-e" : l->level == 'W' ? " system-w" : l->level == 'D' ? " system-d" : "";
    H(b, "<li class=\"system-line%r%r\" id=\"system-l%L\"><time>", cls, l->old ? " system-old" : "", (long long)l->seq);
    line_time(b, l->ts, now);
    buf_puts(b, "</time>");
    if (l->level == 'E') buf_puts(b, "<span class=\"badge bad\">problem</span>");
    else if (l->level == 'W') buf_puts(b, "<span class=\"badge warn\">warning</span>");
    else if (l->level == 'D') buf_puts(b, "<span class=\"badge\">detail</span>");
    bool rule = strlen(l->text) >= 10 && strspn(l->text, "-") == strlen(l->text); /* the start-up block's ----- */
    H(b, "<span class=\"system-tx%r\">%s</span></li>", rule ? " system-rule" : "", l->text);
}

static void page_log(Ctx *c, Buf *b) {
    bool problems = str_eq(ctx_q(c, "f"), "warn");
    const char *q = query(c);
    Buf u = {0};
    buf_puts(b, "<div class=\"toolbar system-tools\"><div class=\"chips\">");
    ctx_url(c, &u, "f", NULL, NULL);
    H(b, "<a class=\"chip%r\" href=\"%s\">All</a>", problems ? "" : " on", buf_cstr(&u));
    buf_reset(&u);
    ctx_url(c, &u, "f", "warn", NULL);
    H(b, "<a class=\"chip%r\" href=\"%s\">Warnings &amp; problems</a></div>", problems ? " on" : "", buf_cstr(&u));
    buf_free(&u);
    buf_puts(b, "<form class=\"system-search\" method=\"get\" action=\"/system/log\" role=\"search\">");
    if (problems) ui_hidden(b, "f", "warn");
    H(b, "<input type=\"search\" name=\"q\" value=\"%s\" maxlength=\"100\" placeholder=\"Search the log…\" "
         "autocomplete=\"off\" spellcheck=\"false\" data-live=\"system.search\" aria-label=\"Search the log\"></form>"
         "<a class=\"btn\" href=\"/system/log.txt\" data-native download>Download the log</a></div>",
      q);
    ui_form(b, "system.verbose", "system-verbose");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> Detailed logging until "
         "Hemma restarts</label>",
      g_log_level <= LOG_DEBUG ? " checked" : "");
    ui_form_end(b);

    static LogLine lines[SHOW_LINES];
    int n = 0, matched = system_log_find(lines, SHOW_LINES, &n, problems, q);
    int64_t now = now_sec();
    if (!matched)
        H(b, "<p class=\"muted system-n\" id=\"system-n\">%s</p>",
          *q ? "Nothing in the log matches that." : problems ? "No warnings or problems in the log." : "Nothing logged yet.");
    else
        H(b, "<p class=\"muted small system-n\" id=\"system-n\">Newest first%r · %d line%r%r%r</p>",
          matched > SHOW_LINES ? ", the latest 200" : "", matched, matched == 1 ? "" : "s",
          *q ? " match" : problems ? " with warnings or problems" : "", matched > SHOW_LINES ? " (all of them in the download)" : "");
    buf_puts(b, "<ol class=\"system-log\" id=\"system-log\">");
    /* newest first - but lines logged in the same second (a block such as the start-up message)
       read top to bottom */
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && lines[j].ts == lines[i].ts && lines[j].level != '-' && lines[i].level != '-') j++;
        for (int k = j - 1; k >= i; k--) log_line(b, &lines[k], now);
        i = j;
    }
    buf_puts(b, "</ol>");
    system_log_free(lines, n);
}

static void cmd(Buf *b, const char *command, const char *what) {
    H(b, "<li><code>%s</code><span>%s</span></li>", command, what);
}

static void path_row(Buf *b, const char *label, const char *path, const char *what) {
    H(b, "<li><b>%s</b><code>%s</code><span>%s%s</span></li>", label, path, what,
      relative(path) ? " (in the folder Hemma was started from)" : "");
}

static void page_help(Ctx *c, Buf *b) {
    RunKind k = run_kind();
    char tmp[800];
    const char *lpath = system_log_path()[0] ? system_log_path() : "hemma.log in the data folder";

    ui_card(b, "Where to look", NULL, NULL);
    buf_puts(b, "<p>The <a href=\"/system/log\">Log</a> tab shows what Hemma is doing, live - turn on detailed logging "
                "there while you chase a problem. The <a href=\"/system\">Checks</a> say in plain words what works and "
                "what doesn't.</p>");
    if (k == RUN_SYSTEMD) {
        buf_puts(b, "<p>On the server, where Hemma runs as the service <code>hemma</code>:</p><ul class=\"system-cmds\">");
        cmd(b, "systemctl status hemma", "running or not, and its last lines");
        cmd(b, "journalctl -u hemma -f", "follow the log live (Ctrl+C stops watching)");
        cmd(b, "journalctl -u hemma -n 100 --no-pager", "the last 100 lines");
        cmd(b, "sudo systemctl restart hemma", "restart it (after changing the settings file)");
        buf_puts(b, "</ul>");
    } else if (k == RUN_LAUNCHD) {
        buf_puts(b, "<p>On the Mac, where Hemma starts when you log in:</p><ul class=\"system-cmds\">");
        cmd(b, "tail -f ~/Hemma/hemma.log", "follow what it prints (Ctrl+C stops watching)");
        cmd(b, "launchctl kickstart -k gui/$(id -u)/local.hemma", "restart it");
        buf_puts(b, "</ul>");
    } else if (k == RUN_WINDOWS) {
        buf_puts(b, "<p>The window Hemma runs in shows the same lines. Started in the background (docs/windows.md)? Then "
                    "follow its file in PowerShell:</p><ul class=\"system-cmds\">");
        snprintf(tmp, sizeof tmp, "Get-Content \"%s\" -Tail 20 -Wait", lpath);
        cmd(b, tmp, "follow the log (Ctrl+C stops watching)");
        buf_puts(b, "</ul>");
    } else {
        buf_puts(b, "<p>The window Hemma was started from shows every line, as it happens. Ctrl+C there stops Hemma.</p>");
    }
    H(b, "<p>Hemma keeps its own copy too: <code>%s</code>, and the one before it (<code>hemma.log.1</code>, next to "
         "it). <a href=\"/system/log.txt\" data-native download>The download</a> holds both.</p>",
      lpath);
    ui_card_end(b);

    ui_card(b, "Where things are", NULL, NULL);
    buf_puts(b, "<ul class=\"system-paths\">");
    path_row(b, "Settings", env_path(), "the settings file, with the password secret (HEMMA_AUTH_SALT)");
    path_row(b, "Data", data_path(), "the database (hemma.db) and the log");
    H(b, "<li><b>Version</b><code>Hemma %s</code><span>", HEMMA_VERSION_FULL);
    when_text(g_started, now_sec(), tmp, sizeof tmp);
    H(b, "running since %s</span></li></ul>", tmp);
    ui_card_end(b);

    ui_card(b, "When something's wrong", NULL, NULL);
    buf_puts(b,
             "<ul class=\"system-faq\">"
             "<li><b>A phone or the TV can't open Hemma.</b> Use the address on the Checks tab and type the "
             "<code>http://</code> part (not https). The device must be on the home Wi-Fi; a Windows PC running Hemma "
             "must let it through its firewall. (docs/home-network.md)</li>"
             "<li><b>Nobody can sign in.</b> The Checks tab says if the password secret changed. One forgotten "
             "password: a recovery code, or <code>hemma --reset-password NAME</code> on the server. (docs/accounts.md)"
             "</li>"
             "<li><b>Uploads stop or can't be saved.</b> The Checks say whether Hemma may write into each library "
             "folder and how much room is left.</li>"
             "<li><b>A video won't play.</b> Watch → Library settings converts it into a copy that plays everywhere."
             "</li>"
             "<li><b>Hemma doesn't start.</b> Its last lines say why - usually another program on the same port, or "
             "a typo in the settings file.</li></ul>"
             "<p class=\"muted small\">More in docs/troubleshooting.md, in Hemma's folder or its repository.</p>");
    ui_card_end(b);

    ui_card(b, "Asking for help", NULL, NULL);
    H(b, "<p>Send the version (<b>Hemma %s</b>), what you did and what happened, and the log:</p>",
      HEMMA_VERSION_FULL);
    buf_puts(b, "<p><a class=\"btn\" href=\"/system/log.txt\" data-native download>Download the log</a></p>"
                "<p class=\"muted small\">The log names devices and people at home: share it only with someone you "
                "trust.</p>");
    if (g_naddr)
        H(b, "<p class=\"muted small\">For scripts: <code>%s/healthz</code> answers <code>ok</code> while Hemma runs.</p>",
          g_addr[0]);
    ui_card_end(b);
}

static void page(Ctx *c, Buf *b) {
    if (!is_admin(c)) { /* (MOD_ADMIN keeps everyone else out already) */
        ui_empty(b, "🔒", "Admins only", NULL);
        return;
    }
    bool is_log = str_eq(c->path, "/system/log"), help = str_eq(c->path, "/system/help");
    if (!is_log && !help && !str_eq(c->path, "/system") && !str_eq(c->path, "/system/")) {
        ctx_title(c, "Not found");
        ui_empty(b, "🧭", "Nothing here", "That page doesn't exist (anymore).");
        return;
    }
    ctx_title(c, is_log ? "Log" : help ? "Help" : "System");
    ui_head(b, "System", is_log ? "What Hemma is doing, live" : help ? "When something isn't right" : "Is everything working?");
    ui_head_end(b);
    tabs(b, is_log ? "Log" : help ? "Help" : "Checks");
    if (is_log) page_log(c, b);
    else if (help) {
        buf_puts(b, "<div class=\"system-help\">");
        page_help(c, b);
        buf_puts(b, "</div>");
    } else page_checks(c, b);
}

/* ---------------------------------------------------------------- widget */

/* The home dashboard: a card only while something needs a look - nothing when all is well. */
static void widget(Ctx *c, Buf *b, int where) {
    if (where != WIDGET_HOME || !is_admin(c)) return;
    Health h = {0};
    collect(c, &h);
    int worst = health_worst(&h);
    if (worst >= HEALTH_WARN) {
        H(b, "<section class=\"card system-widget%r\" id=\"system-widget\"><header class=\"card-h\"><h2>%s</h2><a "
             "class=\"more\" href=\"/system\">System</a></header><ul class=\"system-wl\">",
          worst == HEALTH_BAD ? " system-widget-bad" : "",
          worst == HEALTH_BAD ? "Something isn't working" : "Something needs a look");
        int shown = 0, more = 0;
        for (int st = HEALTH_BAD; st >= HEALTH_WARN; st--)
            for (int i = 0; i < h.n; i++) {
                const HealthItem *it = &h.items[i];
                if (it->step || it->state != st) continue;
                if (shown++ >= 3) {
                    more++;
                    continue;
                }
                H(b, "<li><b>%s</b> <span>%s</span></li>", it->title, it->todo[0] ? it->todo : it->detail);
            }
        if (more) H(b, "<li class=\"muted\">and %d more</li>", more);
        buf_puts(b, "</ul></section>");
    }
    /* A new install: the next step to take, one tap from home, until the list is done or hidden */
    if (setting_int(SET_HIDDEN, 0) != 1) {
        Step s[16];
        int n = build_steps(c, s, countof(s), &h), done = 0, next = -1;
        for (int i = 0; i < n; i++) {
            done += s[i].done;
            if (!s[i].done && next < 0) next = i;
        }
        if (next >= 0)
            H(b, "<section class=\"card system-start\" id=\"system-start\"><header class=\"card-h\"><h2>Getting "
                 "started</h2><a class=\"more\" href=\"/system\">%d of %d done</a></header><p>Next: <a href=\"%s\">"
                 "<b>%s</b></a></p></section>",
              done, n, s[next].link[0] ? s[next].link : "/system", s[next].title);
    }
    health_free(&h);
}

/* --------------------------------------------------------------- actions */

static void act_search(Ctx *c, Fields *f) {
    char q[100];
    str_copy(q, sizeof q, fields_str(f, "q"));
    ctx_set_live(c, "system.q", q);
}

static bool admin_only(Ctx *c) {
    if (is_admin(c)) return true;
    ctx_fail(c, "Only an admin can do that");
    return false;
}

static void act_verbose(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    if (str_eq(fields_str(f, "on"), "1")) {
        g_log_level = LOG_DEBUG;
        log_info("system: %s turned on detailed logging until Hemma restarts", c->me->name);
        ctx_ok(c, "Detailed logging is on until Hemma restarts");
    } else {
        log_info("system: %s turned detailed logging off", c->me->name);
        g_log_level = g_level_at_start <= LOG_DEBUG ? LOG_INFO : g_level_at_start;
        ctx_ok(c, "Detailed logging is off");
    }
}

static void act_salt_saved(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set(SET_SALT, "1");
    ctx_ok(c, "Ticked off - keep that copy safe");
}

static void act_start_hide(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set(SET_HIDDEN, "1");
    ctx_ok(c, "Hidden - it can come back at the bottom of this page");
}

static void act_start_show(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set(SET_HIDDEN, "0");
}

static const Action actions[] = {
    {"system.search", act_search, true},  {"system.verbose", act_verbose},     {"system.salt_saved", act_salt_saved},
    {"system.start_hide", act_start_hide}, {"system.start_show", act_start_show}, {0},
};

/* ------------------------------------------------------------- endpoints */

/* /system/log.txt: the log file(s) as a download. Admins only (MOD_ADMIN, and checked again). */
static bool http(Ctx *c, Conn *conn, HttpReq *r) {
    if (r->streaming || !str_eq(r->path, "/system/log.txt") || !is_admin(c)) return false;
    if (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD")) {
        conn_reply(conn, 405, "text/plain", "GET only\n", 9, "Allow: GET, HEAD\r\n");
        return true;
    }
    log_info("system: %s downloaded the log", c->me->name);
    Buf body = {0};
    system_log_text(&body);
    DateTime d = dt_local(now_sec());
    char extra[300];
    snprintf(extra, sizeof extra,
             "Content-Disposition: attachment; filename=\"hemma-log-%04d-%02d-%02d-%02d%02d.txt\"\r\n"
             "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n",
             d.year % 10000, d.mon % 100, d.day % 100, d.hour % 100, d.min % 100);
    conn_reply(conn, 200, "text/plain; charset=utf-8", body.p ? body.p : "", body.len, extra);
    buf_free(&body);
    return true;
}

/* -------------------------------------------------------------- lifecycle */

static void start(void) {
    g_started = now_sec();
    if (!g_level_known) {
        g_level_at_start = g_log_level;
        g_level_known = true;
    }
    system_log_start(data_path());
    g_disk_at = g_addr_at = g_phone_at = g_shown_at = 0;
    g_free = g_total = -1;
    g_shown_seq = system_log_seq();
    if (!app_testing()) refresh_addresses(now_ms());
    else { /* each test sets its own */
        g_addr_fixed = false;
        g_naddr = 0;
        g_test_free = -1;
    }
}

static void stop(void) { system_log_stop(); }

static void tick(int64_t now) {
    system_log_tick(now, false);
    /* new lines: screens on the System pages see them - at most every 1.5 s, so a flood of log
       lines can't keep re-rendering screens */
    uint32_t seq = system_log_seq();
    if (seq != g_shown_seq) {
        if (!g_shown_at || now - g_shown_at >= LIVE_EVERY_MS) {
            g_shown_seq = seq;
            g_shown_at = now ? now : 1;
            app_changed_where("/system");
        } else {
            app_wake_in((int)(LIVE_EVERY_MS - (now - g_shown_at)));
        }
    }
    if (!app_testing() && !g_addr_fixed && (!g_addr_at || now - g_addr_at >= 60000)) refresh_addresses(now);
    if (!g_disk_at || now - g_disk_at >= 30000) refresh_disk(now);
    if (!g_phone_at || now - g_phone_at >= 60000) { /* once it was seen, it stays ticked */
        g_phone_at = now ? now : 1;
        if (setting_int(SET_PHONE, 0) != 1 && phone_seen()) setting_set(SET_PHONE, "1");
    }
}

/* ------------------------------------------------------------------ tests */

void system_test_addresses(const char *a, const char *b) {
    g_addr_fixed = a != NULL;
    g_naddr = 0;
    if (a && *a) str_copy(g_addr[g_naddr++], sizeof g_addr[0], a);
    if (a && *a && b && *b) str_copy(g_addr[g_naddr++], sizeof g_addr[0], b);
}

void system_test_free_space(int64_t bytes) {
    g_test_free = bytes;
    g_disk_at = 0;
}

static const Module system_mod = {
    .id = "system",
    .title = "System",
    .icon = "pulse",
    .nav = 95,
    .flags = MOD_ADMIN,
    .start = start,
    .stop = stop,
    .tick = tick,
    .page = page,
    .actions = actions,
    .http = http,
    .widget = widget,
    .widget_order = 1,
};
MODULE(system_mod)
