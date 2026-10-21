/* modules/system — "is everything working?", for admins.
 *
 * /system         checks in plain words: the core's own (version and uptime, the address to
 *                 open, saving, disk space, the password secret, sign-in, the log) and every
 *                 module's health hook (core/app.h) - plus a getting-started list that ticks
 *                 itself from real data until it's hidden
 * /system/log     the server's own log, live (log.c): filter, search, download, detailed logging
 * /system/backups the nightly backups: back up now, download, go back to one (backup.c)
 * /system/updates the version, new ones (checked when asked, or daily when allowed), install one or
 *                 an uploaded download - through a root helper, with a way back (update.c)
 * /system/help    where to look when something's wrong, on this machine, and what to send
 * /system/log.txt the log as a file (the http hook); /system/backups/<name>.tar a backup to keep
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

static void span_text(int64_t s, char *out, size_t cap) { /* "12 min", "5 h", "3 days" */
    if (s < 60) str_copy(out, cap, T("less than a minute"));
    else if (s < 3600) snprintf(out, cap, "%lld min", (long long)(s / 60));
    else if (s < 2 * 86400) snprintf(out, cap, "%lld h", (long long)(s / 3600));
    else snprintf(out, cap, TN(s / 86400, "%lld day", "%lld days"), (long long)(s / 86400));
}

/* A number printed with "%.1f": Swedish and Spanish write a decimal comma. */
static void decimal_mark(char *s) {
    char c = i18n_decimal();
    for (; c != '.' && *s; s++)
        if (*s == '.') *s = c;
}

/* The zone's offsets in words, in the reader's language ("UTC+01:00 in January, UTC+02:00 in July") -
   core/zone.c's zone_words says it in English. */
static void zone_say(const char *sig, char *out, size_t cap) {
    char jan[16];
    const char *slash = strchr(sig, '/');
    if (slash) {
        str_ncopy(jan, sizeof jan, sig, (size_t)(slash - sig));
        snprintf(out, cap, T("UTC%s in January, UTC%s in July"), jan, slash + 1);
    } else {
        snprintf(out, cap, T("UTC%s all year"), sig);
    }
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

void system_when(int64_t t, int64_t now, char *out, size_t cap) { when_text(t, now, out, cap); }
const char *system_data_path(void) { return data_path(); }

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

bool system_restarts_itself(void) {
    RunKind k = run_kind();
    return k == RUN_SYSTEMD || k == RUN_LAUNCHD;
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

static bool local_only(void) { return app_local_only(); } /* (127.x or localhost: no other device can reach it) */

/* ---------------------------------------------------------------- checks */

static void own_checks(Ctx *c, Health *h) {
    char d[320], todo[240], a[64], b[64];
    int64_t now = now_sec();

    /* Hemma itself */
    int64_t last = 0;
    bool unclean = system_log_unclean(&last);
    span_text(now - g_started, a, sizeof a);
    when_text(g_started, now, b, sizeof b);
    int n = snprintf(d, sizeof d, T("Version %s, running for %s (since %s)."), HEMMA_VERSION_FULL, a, b);
    if (unclean && n > 0 && (size_t)n + 1 < sizeof d) {
        when_text(last, now, a, sizeof a);
        d[n] = ' ';
        snprintf(d + n + 1, sizeof d - (size_t)n - 1,
                 T("The time before, it stopped without warning after %s - a power cut or a crash? The log has the "
                   "lines from before."),
                 a);
    }
    health_add(h, unclean ? HEALTH_INFO : HEALTH_OK, "Hemma", d, NULL, unclean ? "/system/log" : NULL);

    /* where to open it */
    if (!g_addr_at && !app_testing()) refresh_addresses(now_ms());
    if (local_only()) {
        /* plain words first, the flag folded away (issue #62) */
        health_add(h, app_dev_mode() ? HEALTH_INFO : HEALTH_WARN, T("Address"),
                   T("Only this computer can open Hemma: phones and the TV can't reach it."),
                   T("Let the rest of the home in: start Hemma so it listens on the home network "
                     "(docs/troubleshooting.md)."),
                   NULL);
        snprintf(d, sizeof d, T("It listens on %s. Start it without --host, or with --host 0.0.0.0 (HEMMA_HOST=0.0.0.0 "
                                "in the settings file)."),
                 app_config()->host);
        health_tech(h, d);
    } else if (!g_naddr) {
        health_add(h, HEALTH_WARN, T("Address"), T("Hemma can't find its address on the home network."),
                   T("Check this computer's network cable or Wi-Fi - other devices need it to find Hemma."), NULL);
    } else {
        if (g_naddr > 1)
            snprintf(d, sizeof d, T("Phones, computers and the TV open %s or %s. Tip: reserve that address in your "
                                    "router so it never changes (docs/home-network.md)."),
                     g_addr[0], g_addr[1]);
        else
            snprintf(d, sizeof d, T("Phones, computers and the TV open %s. Tip: reserve that address in your router so "
                                    "it never changes (docs/home-network.md)."),
                     g_addr[0]);
        health_add(h, HEALTH_OK, T("Address"), d, NULL, NULL);
    }

    /* the clock and its time zone (issue #39): wrong on day one is easy to fix, wrong for a week isn't */
    char sig[32], words[80], hm[6];
    zone_sig(sig, sizeof sig);
    zone_say(sig, words, sizeof words);
    fmt_hm(now, hm);
    ZoneChange zc;
    if (zone_change(&zc)) {
        char a[80], b[80];
        zone_say(zc.from, a, sizeof a);
        zone_say(zc.to, b, sizeof b);
        Rezone z;
        int typed = zc.retimed < 0 ? zone_retime(false, &z) : 0;
        Buf t = {0}; /* (whole sentences, one after the other) */
        buf_printf(&t, T("The time zone changed on %s, from %s to %s."), zc.day, a, b);
        if (zc.kept) {
            buf_puts(&t, " ");
            buf_printf(&t, T("Things that last whole days (all-day events, chores' days, whole-day bookings) kept their "
                             "days (%d)."),
                       zc.kept);
        }
        buf_puts(&t, " ");
        if (zc.retimed >= 0)
            buf_printf(&t, TN(zc.retimed, "%d time typed before then was moved back to the time typed.",
                              "%d times typed before then were moved back to the times typed."),
                       zc.retimed);
        else if (typed)
            buf_printf(&t, TN(typed, "%d booking or event typed before then shows shifted by the difference - right if "
                                     "Hemma moved house, wrong if the old zone was.",
                              "%d bookings and events typed before then show shifted by the difference - right if "
                              "Hemma moved house, wrong if the old zone was."),
                       typed);
        else buf_puts(&t, T("Nothing typed before then shows at another time."));
        health_add(h, typed ? HEALTH_WARN : HEALTH_INFO, T("Time zone"), buf_cstr(&t),
                   typed ? T("Was the old zone wrong (a Raspberry Pi left on its own UTC)? Then move them back to the "
                             "times they were typed, above. If not, \"Got it\".")
                         : NULL,
                   NULL);
        buf_free(&t);
    } else if (str_eq(sig, "+00:00")) {
        snprintf(d, sizeof d, T("The clock says %s, in UTC all year - a Raspberry Pi's own setting, not many homes' "
                                "time."),
                 hm);
        health_add(h, HEALTH_INFO, T("Time zone"), d,
                   T("If that isn't the time at home, set the zone now, before people add things: sudo raspi-config, "
                     "Localisation Options, Timezone. Then restart Hemma."),
                   NULL);
    } else {
        snprintf(d, sizeof d, T("The clock says %s (%s)."), hm, words);
        health_add(h, HEALTH_OK, T("Time zone"), d, NULL, NULL);
    }

    /* the database: damaged and rescued at the start (store_open), or damaged now */
    char rescued[768]; /* (the core's report, kept packed: in this admin's language) */
    i18n_unpack_to(rescued, sizeof rescued, setting(SETTING_RESCUED, ""));
    if (rescued[0])
        health_add(h, HEALTH_BAD, T("Database rescued"), rescued,
                   T("Going back to a backup from before (Backups) brings the lost rows back - and loses what was "
                     "added since. Otherwise carry on: everything that could be read is here."),
                   "/system/backups");

    system_backup_checks(h); /* (backup.c) */
    system_update_checks(h); /* (update.c) */

    /* saving */
    if (store_damaged()) {
        health_add(h, HEALTH_BAD, T("Saving"),
                   T("Saving failed: the database file is damaged. Recent changes are only in memory."),
                   T("Restart Hemma: at the start it copies the damaged file aside and rescues every row it can still "
                     "read into a fresh one."),
                   "/system/log?f=warn");
    } else if (store_failed()) {
        snprintf(todo, sizeof todo, T("Make room on the disk with %s, then restart Hemma. The log says what failed."),
                 data_path());
        health_add(h, HEALTH_BAD, T("Saving"),
                   T("Saving changes to the disk failed - is it full? Recent changes are only in memory and will be "
                     "lost when Hemma stops."),
                   todo, "/system/log?f=warn");
    } else {
        snprintf(d, sizeof d, T("Every change is saved in %s/hemma.db."), data_path());
        health_add(h, HEALTH_OK, T("Saving"), d, NULL, NULL);
    }

    /* signing in */
    if (!auth_ready()) {
        snprintf(todo, sizeof todo, T("Add one to the settings file (%s) and restart Hemma."), env_path());
        health_add(h, HEALTH_BAD, T("Sign-in"), T("There's no password secret, so nobody can sign in or be added."), todo,
                   NULL);
        health_tech(h, T("The secret is the line HEMMA_AUTH_SALT=... in the settings file; hemma --gen-salt makes one."));
    } else if (auth_salt_mismatch()) {
        snprintf(todo, sizeof todo, T("Put the original line back into %s and restart Hemma (docs/accounts.md, "
                                      "\"Locked out\")."),
                 env_path());
        health_add(h, HEALTH_BAD, T("Sign-in"),
                   T("The password secret (a line in the settings file) isn't the one the passwords were made with, so "
                     "nobody can sign in with a password."),
                   todo, NULL);
        health_tech(h, T("The secret is the line HEMMA_AUTH_SALT=... in the settings file."));
    } else if (auth_setup_open()) {
        health_add(h, HEALTH_WARN, T("Sign-in"),
                   T("The welcome page is still open: whoever has the setup code can make the first admin."),
                   T("Open Hemma and create the admin account - the setup code is in the log."), "/system/log");
    } else {
        int can = 0, admins = 0;
        for (int i = 0; i < account_count(); i++) {
            const Person *p = account_at(i);
            if (!p->username[0] || !p->pass[0] || person_signin_over(p, now_sec())) continue; /* (a guest's ended) */
            can++;
            admins += p->role == ROLE_ADMIN;
        }
        const char *fmt = can == 1 ? (admins == 1 ? T("%d account can sign in, %d of them admin.")
                                                  : T("%d account can sign in, %d of them admins."))
                                   : (admins == 1 ? T("%d accounts can sign in, %d of them admin.")
                                                  : T("%d accounts can sign in, %d of them admins."));
        int k = snprintf(d, sizeof d, fmt, can, admins);
        if (admins == 1 && k > 0 && (size_t)k + 1 < sizeof d)
            snprintf(d + k, sizeof d - (size_t)k, " %s", T("Tip: a second admin can help if you're ever locked out."));
        health_add(h, HEALTH_OK, T("Sign-in"), d, NULL, NULL);
    }

    /* the log */
    char why[120];
    if (system_log_write_failed(why, sizeof why)) { /* (why: log.c's English, N_-marked) */
        snprintf(d, sizeof d, T("Hemma can't keep its log file (%s): %s."), system_log_path(), T(why));
        health_add(h, HEALTH_WARN, T("Log"), d,
                   T("Check that the data folder can be written to and the disk isn't full. The log below still works."),
                   "/system/log");
    }
    int errs = system_log_errors_since(now - 86400);
    if (errs) {
        snprintf(d, sizeof d, TN(errs, "%d problem logged in the last day.", "%d problems logged in the last day."), errs);
        health_add(h, HEALTH_WARN, T("Problems in the log"), d, T("Have a look: each line says what went wrong."),
                   "/system/log?f=warn");
    } else {
        health_add(h, HEALTH_OK, T("Problems in the log"), T("None logged since Hemma started."), NULL, "/system/log");
    }
}

/* Sizes as the disk rows compare them: one decimal up to 100 GB, so "11.8 GB free" and "needs
   12.0 GB" can't both round to "12 GB" (issue #95). */
static void room_words(int64_t n, char *out, size_t cap) {
    double g = (double)n / 1073741824.0;
    if (g >= 1024) snprintf(out, cap, "%.1f TB", g / 1024);
    else if (g >= 100) snprintf(out, cap, "%.0f GB", g);
    else if (g >= 1) snprintf(out, cap, "%.1f GB", g);
    else snprintf(out, cap, "%lld MB", (long long)(n >> 20));
    decimal_mark(out);
}

#define DATA_WANT (2LL << 30)   /* free on the data's disk: room to save changes and keep the log */
#define DATA_BAD (300LL << 20)  /* ... under this, saving fails soon */

typedef struct {
    int64_t free, total, want;
    bool data;
    char what[160], where[300];
    const HealthDiskRow *wish; /* the one that wants the most (NULL: the data folder's own wish) */
} Disk;

static void add_what(Disk *d, const char *what) {
    if (strstr(d->what, what)) return; /* (two library folders on one disk) */
    if (!d->what[0]) {
        str_copy(d->what, sizeof d->what, what);
        return;
    }
    char was[sizeof d->what];
    str_copy(was, sizeof was, d->what);
    snprintf(d->what, sizeof d->what, T("%s and %s"), was, what);
}

/* One "Disk space" row per disk (issue #95): the data folder's, and each one a module keeps
   things on (health_disk) - one disk once (the module says when it's the data's; others: the
   same size, and free space within 64 MB), naming what's on it, held to the biggest wish. */
static void disk_rows(Health *h) {
    if (!g_disk_at) refresh_disk(now_ms());
    Disk disks[16];
    int nd = 0;
    if (g_free >= 0 && g_total > 0) {
        Disk *d = &disks[nd++];
        memset(d, 0, sizeof *d);
        d->free = g_free;
        d->total = g_total;
        d->want = DATA_WANT;
        d->data = true;
        add_what(d, T("Hemma's data"));
    }
    for (int i = 0; i < h->ndisks; i++) {
        const HealthDiskRow *r = &h->disks[i];
        int k = -1;
        for (int j = 0; j < nd && k < 0; j++) {
            if (r->data_disk ? disks[j].data
                             : !disks[j].data && disks[j].total == r->total && llabs(disks[j].free - r->free) < (64LL << 20))
                k = j;
        }
        if (k < 0) {
            if (nd == countof(disks) || r->free < 0) continue;
            k = nd++;
            memset(&disks[k], 0, sizeof disks[k]);
            disks[k].free = r->free;
            disks[k].total = r->total;
            str_copy(disks[k].where, sizeof disks[k].where, r->path);
        }
        add_what(&disks[k], r->what);
        if (r->want > disks[k].want) {
            disks[k].want = r->want;
            disks[k].wish = r;
        }
    }
    for (int i = 0; i < nd; i++) {
        const Disk *d = &disks[i];
        char fr[24], wt[24], title[80], text[320], who[80];
        room_words(d->free, fr, sizeof fr);
        room_words(d->want, wt, sizeof wt);
        int used = d->total > 0 ? (int)CLAMP((d->total - d->free) * 100 / d->total, 0, 100) : 0;
        int st = d->data && d->free < DATA_BAD ? HEALTH_BAD : d->free < d->want ? HEALTH_WARN : HEALTH_OK;
        int n = snprintf(text, sizeof text, T("%s free on the disk with %s (%d%% used)."), fr, d->what, used);
        if (st && n > 0 && (size_t)n + 1 < sizeof text) {
            text[n++] = ' ';
            if (d->wish) {
                str_copy(who, sizeof who, d->wish->what);
                if (who[0] >= 'a' && who[0] <= 'z') who[0] = (char)(who[0] - 'a' + 'A');
                snprintf(text + n, sizeof text - (size_t)n, T("%s wants %s free %s."), who, wt, d->wish->why);
            } else if (st == HEALTH_BAD) {
                snprintf(text + n, sizeof text - (size_t)n,
                         T("Hemma wants %s free to save changes and keep its log - under 300 MB, saving fails soon."), wt);
            } else {
                snprintf(text + n, sizeof text - (size_t)n, T("Hemma wants %s free to save changes and keep its log."), wt);
            }
        }
        if (d->data) str_copy(title, sizeof title, T("Disk space"));
        else snprintf(title, sizeof title, T("Disk space (%.60s)"), d->where);
        const char *todo = st == HEALTH_OK ? NULL
                           : d->wish   ? d->wish->todo
                                       : T("Make room on that disk: when it's full, Hemma can't save changes (old "
                                           "videos? the Watch library settings show what takes room).");
        health_add(h, st, title, text, todo, st && d->wish ? d->wish->link : NULL);
    }
}

/* The core's checks first, then every module's (their steps come along: see the list below), then
   the disks they all use. */
static void collect(Ctx *c, Health *h) {
    own_checks(c, h);
    health_collect(c, h);
    disk_rows(h);
}

static int count_looks(const Health *h) {
    int n = 0;
    for (int i = 0; i < h->n; i++) n += !h->items[i].step && h->items[i].state >= HEALTH_WARN;
    return n;
}

/* The flag, variable or command behind a check, folded away: the household reads plain words (issue #62). */
static void tech_more(Buf *b, const char *tech) {
    H(b, "<details class=\"system-tech\"><summary>%s</summary><span>%s</span></details>", T("Technical details"), tech);
}

static void check_row(Buf *b, const HealthItem *it) {
    static const char *const cls[] = {"ok", "info", "warn", "bad"};
    static const char *const badge_cls[] = {"badge ok", "badge", "badge warn", "badge bad"};
    static const char *const badge[] = {"OK", NC("badge", "Note"), N_("Needs a look"), N_("Problem")};
    uint64_t key = hash64_str(it->title) ^ (hash64_str(it->module ? it->module : "system") * 31);
    H(b, "<li class=\"row system-check system-%s\" id=\"system-c%X\"><div class=\"row-main\"><span class=\"system-ct\">"
         "<span class=\"%s\">%s</span><b>%s</b></span>",
      cls[it->state], key, badge_cls[it->state], it->state == HEALTH_OK ? badge[0] : T(badge[it->state]), it->title);
    if (it->detail[0]) H(b, "<span class=\"system-cd\">%s</span>", it->detail);
    if (it->todo[0] && it->state != HEALTH_OK) {
        if (it->link[0]) H(b, "<a class=\"system-todo\" href=\"%s\">%s</a>", it->link, it->todo);
        else H(b, "<span class=\"system-todo\">%s</span>", it->todo);
    } else if (it->link[0]) {
        H(b, "<a class=\"system-more\" href=\"%s\">%s</a>", it->link, T("Have a look"));
    }
    if (it->tech[0] && it->state != HEALTH_OK) tech_more(b, it->tech);
    buf_puts(b, "</div></li>");
}

static void checks_card(Buf *b, const Health *h) {
    H(b, "<section class=\"card system-checks-card\" id=\"system-checks\"><header class=\"card-h\"><h2>%s"
         "</h2></header><ul class=\"rows system-checks\">",
      T("Checks"));
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
    char title[80], detail[600], link[160], tech[200]; /* tech: folded under "Technical details" (issue #62) */
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
    char d[600];
    step(s, &n, max, setting("household.name", NULL) != NULL, T("Name your home"),
         T("The name at the top of every page - \"The Svenssons\", \"Villa Kulla\"..."), "/people");
    step(s, &n, max, person_count() > 1, T("Add the household"),
         T("Everyone who lives here, even the ones too young to sign in: each gets a colour, and a sign-in if they "
           "like."),
         "/people?add=1");
    for (int i = 0; i < h->n; i++) /* (other modules' steps: in the reader's language already) */
        if (h->items[i].step) step(s, &n, max, h->items[i].state == HEALTH_OK, h->items[i].title, h->items[i].detail,
                                   h->items[i].link);
    if (g_naddr && !local_only())
        snprintf(d, sizeof d, T("On a phone at home, open %s and sign in. Tip: add it to the home screen - it opens like "
                                "an app."),
                 g_addr[0]);
    else
        str_copy(d, sizeof d, T("On a phone at home, open Hemma's address (see Address in the Checks) and sign in. Tip: "
                                "add it to the home screen - it opens like an app."));
    step(s, &n, max, phone_seen(), T("Open Hemma on a phone"), d, NULL);
    step(s, &n, max, c->me && auth_codes_left(c->me->id) > 0, T("Save your recovery codes"),
         T("They get you back in if you forget your password. Keep them somewhere safe, away from this server."),
         "/people#people-codes-h");
    if (relative(env_path()))
        snprintf(d, sizeof d, T("Every password depends on the password secret (a line in the settings file, %s, in the "
                                "folder Hemma was started from). Keep a copy of that line somewhere safe, apart from "
                                "Hemma's data - a password manager, or a note in a drawer. The nightly backups can't "
                                "sign anyone in on a new install without it (a Download from Backups has it in)."),
                 env_path());
    else
        snprintf(d, sizeof d, T("Every password depends on the password secret (a line in the settings file, %s). Keep "
                                "a copy of that line somewhere safe, apart from Hemma's data - a password manager, or a "
                                "note in a drawer. The nightly backups can't sign anyone in on a new install without it "
                                "(a Download from Backups has it in)."),
                 env_path());
    int before = n;
    step(s, &n, max, setting_int(SET_SALT, 0) == 1, T("Keep a copy of the password secret"), d, NULL);
    if (n > before) {
        s[n - 1].salt = true;
        str_copy(s[n - 1].tech, sizeof s[n - 1].tech,
                 run_kind() == RUN_SYSTEMD ? T("The line starts HEMMA_AUTH_SALT=. On the server: sudo grep "
                                               "HEMMA_AUTH_SALT /etc/hemma/hemma.env")
                                           : T("The line starts HEMMA_AUTH_SALT=."));
    }
    return n;
}

static void steps_card(Ctx *c, Buf *b, const Health *h) {
    Step s[16];
    int n = build_steps(c, s, countof(s), h), done = 0;
    for (int i = 0; i < n; i++) done += s[i].done;
    H(b, "<section class=\"card system-start\" id=\"system-start\"><header class=\"card-h\"><h2>%s</h2>"
         "<span class=\"muted small\">",
      T("Getting started"));
    H(b, T("%d of %d done"), done, n);
    buf_puts(b, "</span></header><ol class=\"rows system-steps\">");
    for (int i = 0; i < n; i++) {
        H(b, "<li class=\"row system-step%r\" id=\"system-s%d\"><span class=\"system-tick\" title=\"%s\">",
          s[i].done ? " system-done" : "", i, s[i].done ? T("Done") : T("Still to do"));
        if (s[i].done) ui_icon(b, "check");
        buf_puts(b, "</span><div class=\"row-main\">");
        if (s[i].link[0] && !s[i].done) H(b, "<a class=\"system-st\" href=\"%s\">%s</a>", s[i].link, s[i].title);
        else H(b, "<b class=\"system-st\">%s</b>", s[i].title);
        if (!s[i].done) H(b, "<small>%s</small>", s[i].detail);
        if (!s[i].done && s[i].tech[0]) tech_more(b, s[i].tech);
        if (s[i].salt && !s[i].done) {
            buf_puts(b, "<div class=\"system-step-a\">");
            ui_act(b, "system.salt_saved", "btn sm", T("Done - I have a copy"), NULL, NULL, NULL);
            buf_puts(b, "</div>");
        }
        buf_puts(b, "</div></li>");
    }
    buf_puts(b, "</ol><div class=\"system-start-f\">");
    if (done == n) H(b, "<p>%s</p>", T("All done - Hemma is set up."));
    ui_act(b, "system.start_hide", done == n ? "btn" : "btn ghost sm", T("Hide this"), NULL, NULL, NULL);
    buf_puts(b, "</div></section>");
}

/* ----------------------------------------------------------------- pages */

static void tabs(Buf *b, const char *on) {
    static const char *const href[] = {"/system", "/system/log", "/system/backups", "/system/updates", "/system/help"};
    static const char *const label[] = {N_("Checks"), N_("Log"), N_("Backups"), N_("Updates"), N_("Help")};
    H(b, "<nav class=\"seg system-tabs\" aria-label=\"%s\">", T("System"));
    for (int i = 0; i < 5; i++)
        H(b, "<a href=\"%s\"%r>%s</a>", href[i], str_eq(on, label[i]) ? " class=\"on\" aria-current=\"page\"" : "",
          T(label[i]));
    buf_puts(b, "</nav>");
}

static void page_checks(Ctx *c, Buf *b) {
    Health h = {0};
    collect(c, &h);
    int worst = health_worst(&h), looks = count_looks(&h);
    char up[64];
    span_text(now_sec() - g_started, up, sizeof up);
    if (worst == HEALTH_BAD) {
        buf_puts(b, "<p class=\"notice bad system-sum\" id=\"system-sum\" role=\"alert\">");
        H(b, TN(looks, "Something isn't working - %d thing below needs a look.",
                "Something isn't working - %d things below need a look."),
          looks);
        buf_puts(b, "</p>");
    } else if (worst == HEALTH_WARN) {
        buf_puts(b, "<p class=\"notice system-sum system-look\" id=\"system-sum\">");
        H(b, TN(looks, "%d thing below needs a look. Everything else works.",
                "%d things below need a look. Everything else works."),
          looks);
        buf_puts(b, "</p>");
    } else {
        buf_puts(b, "<p class=\"notice system-sum system-good\" id=\"system-sum\">");
        H(b, T("Everything works. Hemma %s has been running for %s."), HEMMA_VERSION, up);
        buf_puts(b, "</p>");
    }
    if (setting(SETTING_RESCUED, "")[0]) { /* (the check says what happened; this puts it away) */
        buf_puts(b, "<div class=\"system-again\">");
        ui_act(b, "system.rescued_seen", "btn sm", T("Got it - I've read about the rescued database"), NULL, NULL, NULL);
        buf_puts(b, "</div>");
    }
    ZoneChange zc;
    if (zone_change(&zc)) { /* issue #39: what moving them back would do, then the choice */
        buf_puts(b, "<div class=\"system-again\" id=\"system-zone\">");
        Rezone z;
        int typed = zc.retimed < 0 ? zone_retime(false, &z) : 0;
        if (typed) {
            buf_puts(b, "<p>");
            H(b, T("The time zone changed on %s. Moved back to the times they were typed, these would show:"), zc.day);
            buf_puts(b, "</p><ul class=\"system-zone\">");
            for (int k = 0; k < z.notes; k++) H(b, "<li>%s</li>", z.note[k]); /* (core/zone.c's previews) */
            if (typed > z.notes) {
                buf_puts(b, "<li class=\"muted\">");
                H(b, T("and %d more"), typed - z.notes);
                buf_puts(b, "</li>");
            }
            buf_puts(b, "</ul><div class=\"system-zone-do\">");
            char ask[300];
            snprintf(ask, sizeof ask, TN(typed, "Move %d booking or event back to the time typed?",
                                         "Move %d bookings and events back to the times typed?"),
                     typed);
            ui_form_confirm(b, "system.tz_retime", "inline", ask);
            H(b, "<button class=\"btn sm primary\">%s</button>", T("Move them back to the times typed"));
            ui_form_end(b);
            ui_act(b, "system.tz_seen", "btn sm", T("Got it - they're right as they are"), NULL, NULL, NULL);
            buf_puts(b, "</div>");
        } else ui_act(b, "system.tz_seen", "btn sm", T("Got it - I've read about the time zone"), NULL, NULL, NULL);
        buf_puts(b, "</div>");
    }
    bool start = setting_int(SET_HIDDEN, 0) != 1;
    if (looks || !start) checks_card(b, &h);
    if (start) steps_card(c, b, &h);
    if (!looks && start) checks_card(b, &h);
    if (!start) {
        buf_puts(b, "<div class=\"system-again\">");
        ui_act(b, "system.start_show", "btn ghost sm", T("Show the getting-started list again"), NULL, NULL, NULL);
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

/* A divider: log.c's marks ("Hemma restarted", "Hemma stopped", and "Hemma 0.24.0 (...) started" read back from
   the file) are English in the ring and the file; on the page, in the reader's language. */
static void divider_text(Buf *b, const char *t) {
    size_t n = strlen(t);
    if (n > 14 && str_starts(t, "Hemma ") && str_ends(t, " started")) {
        char v[96];
        str_ncopy(v, sizeof v, t + 6, n - 14);
        H(b, T("Hemma %s started"), v);
    } else {
        H(b, "%s", T(t));
    }
}

static void log_line(Buf *b, const LogLine *l, int64_t now) {
    if (l->level == '-') {
        H(b, "<li class=\"system-div\" id=\"system-l%L\"><span>", (long long)l->seq);
        divider_text(b, l->text);
        buf_puts(b, " · ");
        line_time(b, l->ts, now);
        buf_puts(b, "</span></li>");
        return;
    }
    const char *cls = l->level == 'E' ? " system-e" : l->level == 'W' ? " system-w" : l->level == 'D' ? " system-d" : "";
    H(b, "<li class=\"system-line%r%r\" id=\"system-l%L\"><time>", cls, l->old ? " system-old" : "", (long long)l->seq);
    line_time(b, l->ts, now);
    buf_puts(b, "</time>");
    if (l->level == 'E') H(b, "<span class=\"badge bad\">%s</span>", T("problem"));
    else if (l->level == 'W') H(b, "<span class=\"badge warn\">%s</span>", T("warning"));
    else if (l->level == 'D') H(b, "<span class=\"badge\">%s</span>", T("detail"));
    bool rule = strlen(l->text) >= 10 && strspn(l->text, "-") == strlen(l->text); /* the start-up block's ----- */
    H(b, "<span class=\"system-tx%r\">%s</span></li>", rule ? " system-rule" : "", l->text);
}

static void page_log(Ctx *c, Buf *b) {
    bool problems = str_eq(ctx_q(c, "f"), "warn");
    const char *q = query(c);
    Buf u = {0};
    buf_puts(b, "<div class=\"toolbar system-tools\"><div class=\"chips\">");
    ctx_url(c, &u, "f", NULL, NULL);
    H(b, "<a class=\"chip%r\" href=\"%s\">%s</a>", problems ? "" : " on", buf_cstr(&u), T("All"));
    buf_reset(&u);
    ctx_url(c, &u, "f", "warn", NULL);
    H(b, "<a class=\"chip%r\" href=\"%s\">%s</a></div>", problems ? " on" : "", buf_cstr(&u), T("Warnings & problems"));
    buf_free(&u);
    buf_puts(b, "<form class=\"system-search\" method=\"get\" action=\"/system/log\" role=\"search\">");
    if (problems) ui_hidden(b, "f", "warn");
    H(b, "<input type=\"search\" name=\"q\" value=\"%s\" maxlength=\"100\" placeholder=\"%s\" "
         "autocomplete=\"off\" spellcheck=\"false\" data-live=\"system.search\" aria-label=\"%s\"></form>"
         "<a class=\"btn\" href=\"/system/log.txt\" data-native download>%s</a></div>",
      q, T("Search the log…"), T("Search the log"), T("Download the log"));
    ui_form(b, "system.verbose", "system-verbose");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> %s</label>",
      g_log_level <= LOG_DEBUG ? " checked" : "", T("Detailed logging until Hemma restarts"));
    ui_form_end(b);

    static LogLine lines[SHOW_LINES];
    int n = 0, matched = system_log_find(lines, SHOW_LINES, &n, problems, q);
    int64_t now = now_sec();
    if (!matched) {
        H(b, "<p class=\"muted system-n\" id=\"system-n\">%s</p>",
          *q ? T("Nothing in the log matches that.") : problems ? T("No warnings or problems in the log.")
                                                         : T("Nothing logged yet."));
    } else {
        /* (one whole phrase per case: "Newest first · 4 lines") */
        const char *say;
        if (matched > SHOW_LINES)
            say = *q        ? T("Newest first, the latest 200 · %d lines match (all of them in the download)")
                  : problems ? T("Newest first, the latest 200 · %d lines with warnings or problems (all of them in the "
                                 "download)")
                             : T("Newest first, the latest 200 · %d lines (all of them in the download)");
        else
            say = *q        ? TN(matched, "Newest first · %d line matches", "Newest first · %d lines match")
                  : problems ? TN(matched, "Newest first · %d line with warnings or problems",
                                  "Newest first · %d lines with warnings or problems")
                             : TN(matched, "Newest first · %d line", "Newest first · %d lines");
        buf_puts(b, "<p class=\"muted small system-n\" id=\"system-n\">");
        H(b, say, matched);
        buf_puts(b, "</p>");
    }
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
    H(b, "<li><b>%s</b><code>%s</code><span>", label, path);
    if (relative(path)) H(b, T("%s (in the folder Hemma was started from)"), what);
    else H(b, "%s", what);
    buf_puts(b, "</span></li>");
}

static void page_help(Ctx *c, Buf *b) {
    RunKind k = run_kind();
    char tmp[800];
    const char *lpath = system_log_path()[0] ? system_log_path() : NULL;

    ui_card(b, T("Where to look"), NULL, NULL);
    H(b, "<p>%r</p>", T("The <a href=\"/system/log\">Log</a> tab shows what Hemma is doing, live - turn on detailed "
                        "logging there while you chase a problem. The <a href=\"/system\">Checks</a> say in plain words "
                        "what works and what doesn't."));
    if (k == RUN_SYSTEMD) {
        H(b, "<p>%r</p><ul class=\"system-cmds\">",
          T("On the server, where Hemma runs as the service <code>hemma</code>:"));
        cmd(b, "systemctl status hemma", T("running or not, and its last lines"));
        cmd(b, "journalctl -u hemma -f", T("follow the log live (Ctrl+C stops watching)"));
        cmd(b, "journalctl -u hemma -n 100 --no-pager", T("the last 100 lines"));
        cmd(b, "sudo systemctl restart hemma", T("restart it (after changing the settings file)"));
        buf_puts(b, "</ul>");
    } else if (k == RUN_LAUNCHD) {
        H(b, "<p>%s</p><ul class=\"system-cmds\">", T("On the Mac, where Hemma starts when you log in:"));
        cmd(b, "tail -f ~/Hemma/hemma.log", T("follow what it prints (Ctrl+C stops watching)"));
        cmd(b, "launchctl kickstart -k gui/$(id -u)/local.hemma", T("restart it"));
        buf_puts(b, "</ul>");
    } else if (k == RUN_WINDOWS) {
        H(b, "<p>%s</p><ul class=\"system-cmds\">",
          T("The window Hemma runs in shows the same lines. Started in the background (docs/windows.md)? Then follow its "
            "file in PowerShell:"));
        snprintf(tmp, sizeof tmp, "Get-Content \"%s\" -Tail 20 -Wait", lpath ? lpath : "hemma.log");
        cmd(b, tmp, T("follow the log (Ctrl+C stops watching)"));
        buf_puts(b, "</ul>");
    } else {
        H(b, "<p>%s</p>", T("The window Hemma was started from shows every line, as it happens. Ctrl+C there stops "
                            "Hemma."));
    }
    buf_puts(b, "<p>");
    if (lpath)
        H(b, T("Hemma keeps its own copy too: <code>%s</code>, and the one before it (<code>hemma.log.1</code>, next to "
               "it). <a href=\"/system/log.txt\" data-native download>The download</a> holds both."),
          lpath);
    else
        H(b, "%r", T("Hemma keeps its own copy too: hemma.log in the data folder, and the one before it "
                     "(<code>hemma.log.1</code>, next to it). <a href=\"/system/log.txt\" data-native download>The "
                     "download</a> holds both."));
    buf_puts(b, "</p>");
    ui_card_end(b);

    ui_card(b, T("Where things are"), NULL, NULL);
    buf_puts(b, "<ul class=\"system-paths\">");
    path_row(b, T("Settings"), env_path(), T("the settings file, with the password secret (HEMMA_AUTH_SALT)"));
    path_row(b, T("Data"), data_path(), T("the database (hemma.db) and the log"));
    H(b, "<li><b>%s</b><code>Hemma %s</code><span>", T("Version"), HEMMA_VERSION_FULL);
    when_text(g_started, now_sec(), tmp, sizeof tmp);
    H(b, T("running since %s"), tmp);
    buf_puts(b, "</span></li></ul>");
    ui_card_end(b);

    static const char *const faq[] = {
        N_("<b>A phone or the TV can't open Hemma.</b> Use the address on the Checks tab and type the "
           "<code>http://</code> part (not https). The device must be on the home Wi-Fi; a Windows PC running Hemma "
           "must let it through its firewall. (docs/home-network.md)"),
        N_("<b>Nobody can sign in.</b> The Checks tab says if the password secret changed. One forgotten password: a "
           "recovery code, or <code>hemma --reset-password NAME</code> on the server. (docs/accounts.md)"),
        N_("<b>Uploads stop or can't be saved.</b> The Checks say whether Hemma may write into each library folder and "
           "how much room is left."),
        N_("<b>A video won't play.</b> Watch → Library settings converts it into a copy that plays everywhere."),
        N_("<b>Hemma doesn't start.</b> Its last lines say why - usually another program on the same port, or a typo in "
           "the settings file."),
    };
    ui_card(b, T("When something's wrong"), NULL, NULL);
    buf_puts(b, "<ul class=\"system-faq\">");
    for (size_t i = 0; i < countof(faq); i++) H(b, "<li>%r</li>", T(faq[i]));
    H(b, "</ul><p class=\"muted small\">%s</p>", T("More in docs/troubleshooting.md, in Hemma's folder or its repository."));
    ui_card_end(b);

    ui_card(b, T("Asking for help"), NULL, NULL);
    buf_puts(b, "<p>");
    H(b, T("Send the version (<b>Hemma %s</b>), what you did and what happened, and the log:"), HEMMA_VERSION_FULL);
    H(b, "</p><p><a class=\"btn\" href=\"/system/log.txt\" data-native download>%s</a></p><p class=\"muted small\">%s</p>",
      T("Download the log"), T("The log names devices and people at home: share it only with someone you trust."));
    if (g_naddr) {
        buf_puts(b, "<p class=\"muted small\">");
        H(b, T("For scripts: <code>%s/healthz</code> answers <code>ok</code> while Hemma runs."), g_addr[0]);
        buf_puts(b, "</p>");
    }
    ui_card_end(b);
}

static void page(Ctx *c, Buf *b) {
    if (!is_admin(c)) { /* (MOD_ADMIN keeps everyone else out already) */
        ui_empty(b, "🔒", T("Admins only"), NULL);
        return;
    }
    bool is_log = str_eq(c->path, "/system/log"), help = str_eq(c->path, "/system/help");
    bool backups = str_eq(c->path, "/system/backups"), updates = str_eq(c->path, "/system/updates");
    if (!is_log && !help && !backups && !updates && !str_eq(c->path, "/system") && !str_eq(c->path, "/system/")) {
        ctx_title(c, "%s", T("Not found"));
        ui_empty(b, "🧭", T("Nothing here"), T("That page doesn't exist (anymore)."));
        return;
    }
    ctx_title(c, "%s", is_log ? T("Log") : help ? T("Help") : backups ? T("Backups") : updates ? T("Updates") : T("System"));
    ui_head(b, T("System"),
            is_log    ? T("What Hemma is doing, live")
            : help    ? T("When something isn't right")
            : backups ? T("Copies of everything Hemma knows")
            : updates ? T("New versions of Hemma, checked before they go in")
                      : T("Is everything working?"));
    ui_head_end(b);
    tabs(b, is_log ? "Log" : help ? "Help" : backups ? "Backups" : updates ? "Updates" : "Checks");
    if (is_log) page_log(c, b);
    else if (updates) {
        buf_puts(b, "<div class=\"system-up\">");
        system_update_page(c, b);
        buf_puts(b, "</div>");
    }
    else if (backups) {
        buf_puts(b, "<div class=\"system-bk\">");
        system_backup_page(c, b);
        buf_puts(b, "</div>");
    }
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
             "class=\"more\" href=\"/system\">%s</a></header><ul class=\"system-wl\">",
          worst == HEALTH_BAD ? " system-widget-bad" : "",
          worst == HEALTH_BAD ? T("Something isn't working") : T("Something needs a look"), T("System"));
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
        if (more) {
            buf_puts(b, "<li class=\"muted\">");
            H(b, T("and %d more"), more);
            buf_puts(b, "</li>");
        }
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
        if (next >= 0) {
            H(b, "<section class=\"card system-start\" id=\"system-start\"><header class=\"card-h\"><h2>%s</h2><a "
                 "class=\"more\" href=\"/system\">",
              T("Getting started"));
            H(b, T("%d of %d done"), done, n);
            buf_puts(b, "</a></header><p>");
            H(b, T("Next: <a href=\"%s\"><b>%s</b></a>"), s[next].link[0] ? s[next].link : "/system", s[next].title);
            buf_puts(b, "</p></section>");
        }
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
    ctx_fail(c, "%s", T("Only an admin can do that"));
    return false;
}

static void act_verbose(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    if (str_eq(fields_str(f, "on"), "1")) {
        g_log_level = LOG_DEBUG;
        log_info("system: %s turned on detailed logging until Hemma restarts", c->me->name);
        ctx_ok(c, "%s", T("Detailed logging is on until Hemma restarts"));
    } else {
        log_info("system: %s turned detailed logging off", c->me->name);
        g_log_level = g_level_at_start <= LOG_DEBUG ? LOG_INFO : g_level_at_start;
        ctx_ok(c, "%s", T("Detailed logging is off"));
    }
}

static void act_salt_saved(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set(SET_SALT, "1");
    ctx_ok(c, "%s", T("Ticked off - keep that copy safe"));
}

static void act_tz_seen(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    zone_seen();
    ctx_ok(c, "%s", T("Noted"));
}

/* Issue #39: the old zone was wrong - what was typed then shows at the times typed again */
static void act_tz_retime(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    ZoneChange zc;
    if (!zone_change(&zc) || zc.retimed >= 0) {
        ctx_fail(c, "%s", T("Nothing to move: that's been done, or put away"));
        return;
    }
    Rezone z;
    int n = zone_retime(true, &z);
    if (n == 1)
        activity_add(c->me->id, "🕑", "/system",
                     N_("%s moved %d time typed before the time zone changed back to the times typed"), c->me->name, n);
    else
        activity_add(c->me->id, "🕑", "/system",
                     N_("%s moved %d times typed before the time zone changed back to the times typed"), c->me->name, n);
    ctx_ok(c, T("Moved %d back to the times typed"), n);
}

static void act_rescued_seen(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set(SETTING_RESCUED, "");
    ctx_ok(c, "%s", T("Noted - the damaged file stays in the data folder until you delete it"));
}

static void act_start_hide(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set(SET_HIDDEN, "1");
    ctx_ok(c, "%s", T("Hidden - it can come back at the bottom of this page"));
}

static void act_start_show(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set(SET_HIDDEN, "0");
}

static const Action actions[] = {
    {"system.search", act_search, true},  {"system.verbose", act_verbose},     {"system.salt_saved", act_salt_saved},
    {"system.start_hide", act_start_hide}, {"system.start_show", act_start_show}, {"system.rescued_seen", act_rescued_seen},
    {"system.tz_seen", act_tz_seen},
    {"system.tz_retime", act_tz_retime},
    {"system.backup_now", system_backup_act_now}, {"system.backup_restore", system_backup_act_restore},
    {"system.backup_cancel", system_backup_act_cancel}, {"system.backup_dir", system_backup_act_dir},
    {"system.restored_seen", system_backup_act_restored_seen},
    {"system.update_check", system_update_act_check}, {"system.update_auto", system_update_act_auto},
    {"system.update_install", system_update_act_install}, {"system.update_seen", system_update_act_seen}, {0},
};

/* ------------------------------------------------------------- endpoints */

/* /system/log.txt: the log file(s) as a download. Admins only (MOD_ADMIN, and checked again). */
static bool http(Ctx *c, Conn *conn, HttpReq *r) {
    if (system_backup_http(c, conn, r) || system_update_http(c, conn, r)) return true;
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
    system_backup_start();
    system_update_start();
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

static void stop(void) {
    system_update_stop();
    system_backup_stop();
    system_log_stop();
}

static void tick(int64_t now) {
    system_log_tick(now, false);
    system_backup_tick(now);
    system_update_tick(now);
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
    .title = N_("System"),
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
