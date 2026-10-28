/* modules/system/backup.c — automatic, checked, restorable backups of the database (issue #35;
 * see system.h). The event loop decides and shows; a worker thread does the disk work (copy,
 * check, list, rotate, stage a restore, the download), one job at a time, and hands its results
 * back on the tick. Under tests a job runs at once, on the loop. */
#include "system.h"
#include "../../core/env.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../core/tar.h"
#include "../../core/ui.h"

#include <stdatomic.h>

#define MAX_LIST 64
#define MAX_ROOTS 8
#define QUIT_AFTER_MS 2000 /* a restore: time for the page to say so before Hemma stops */
#define RETRY_S 3600       /* a failed nightly backup: the next try */
#define MAX_DOWNLOADS 2

typedef struct {
    char name[32]; /* hemma-YYYY-MM-DD-HHMM.db */
    int64_t size, mtime;
} Entry;

typedef enum { JOB_LIST, JOB_BACKUP, JOB_STAGE } JobKind;

typedef struct {
    JobKind kind;
    bool manual;
    char who[64];  /* who asked (the log), "" = the nightly one */
    char name[32]; /* JOB_BACKUP: the new one; JOB_STAGE: the one to put back */
    char data[600], chosen[600], roots[MAX_ROOTS][300];
    int nroots;
    /* results */
    bool ok, same_disk, known_disk;
    char why[400], where[700];
    long long rows;
    int64_t bytes;
    Entry list[MAX_LIST];
    int n;
} Job;

static Lock g_lock = LOCK_INIT;
static Job *g_done; /* a finished job, waiting for the tick (g_lock) */
static atomic_int g_downloads;

/* event loop only */
static bool g_busy;
static JobKind g_busy_kind;
static Entry g_list[MAX_LIST];
static int g_n;
static char g_where[700];
static bool g_same_disk, g_known_disk, g_listed;
static int64_t g_next_try, g_quit_at, g_started;
static char g_restoring[32]; /* staged and about to restart */
/* what the last job a person asked for came to: the kind, and its value (a name, or the reason - plain, or
   i18n_pack'ed) - put into words when the page is shown */
typedef enum { NOTE_NONE, NOTE_DONE, NOTE_FAILED, NOTE_NOT_BACK } NoteKind;
static NoteKind g_note;
static char g_note_arg[512];
static bool g_waiting;        /* a backup is staged (refreshed on events and every minute: checks show cached facts) */
static char g_wait_label[64];
static int64_t g_wait_at;

/* ---------------------------------------------------------------- names */

static bool valid_name(const char *s) { /* hemma-2026-10-17-0330.db */
    static const char pat[] = "hemma-####-##-##-####.db";
    if (strlen(s) != sizeof pat - 1) return false;
    for (size_t i = 0; pat[i]; i++)
        if (pat[i] == '#' ? (s[i] < '0' || s[i] > '9') : s[i] != pat[i]) return false;
    return true;
}

static int64_t name_time(const char *n) { /* the local time in the name */
    int y = atoi(n + 6), mo = atoi(n + 11), d = atoi(n + 14), hm = atoi(n + 17);
    return dt_make(y, mo, d, hm / 100, hm % 100);
}

static void make_name(int64_t now, char out[32]) {
    DateTime d = dt_local(now);
    snprintf(out, 32, "hemma-%04d-%02d-%02d-%02d%02d.db", d.year % 10000, d.mon % 100, d.day % 100, d.hour % 100,
             d.min % 100);
}

static int newest_first(const void *a, const void *b) { return strcmp(((const Entry *)b)->name, ((const Entry *)a)->name); }

/* ------------------------------------------------------------ the worker */

/* The same disk: same size, and free space within 64 MB (as media/room.c). */
static bool same_disk(const char *a, const char *b, bool *known) {
    int64_t fa, ta, fb, tb;
    *known = path_disk_space(a, &fa, &ta) && path_disk_space(b, &fb, &tb);
    return *known && ta == tb && (fa > fb ? fa - fb : fb - fa) < (64LL << 20);
}

static void resolve(Job *j) {
    j->where[0] = '\0';
    if (j->chosen[0]) str_copy(j->where, sizeof j->where, j->chosen);
    for (int i = 0; i < j->nroots && !j->where[0]; i++) {
        bool known;
        if (path_is_dir(j->roots[i]) && !same_disk(j->roots[i], j->data, &known) && known)
            snprintf(j->where, sizeof j->where, "%s/.hemma-backups", j->roots[i]);
    }
    if (!j->where[0]) snprintf(j->where, sizeof j->where, "%s/backups", j->data);
}

static void list_dir(Job *j) {
    j->n = 0;
    DirIter *it = dir_open(j->where);
    DirEntry e;
    while (it && dir_next(it, &e))
        if (!e.is_dir && valid_name(e.name) && j->n < MAX_LIST) {
            Entry *x = &j->list[j->n++];
            str_copy(x->name, sizeof x->name, e.name);
            x->size = e.size;
            x->mtime = e.mtime;
        }
    dir_close(it);
    qsort(j->list, (size_t)j->n, sizeof *j->list, newest_first);
    if (path_is_dir(j->where)) j->same_disk = same_disk(j->where, j->data, &j->known_disk);
}

/* The newest of each of the last BACKUP_DAYS days stay; older ones and leftovers of failed copies go.
   Only names Hemma gives its backups are ever touched. */
static void rotate(Job *j) {
    list_dir(j);
    char days[BACKUP_DAYS][11];
    int nd = 0;
    for (int i = 0; i < j->n; i++) {
        const char *day = j->list[i].name + 6;
        bool seen = false;
        for (int k = 0; k < nd; k++) seen |= memcmp(days[k], day, 10) == 0;
        if (!seen && nd < BACKUP_DAYS) {
            memcpy(days[nd], day, 10);
            days[nd++][10] = '\0';
            continue;
        }
        char p[740];
        snprintf(p, sizeof p, "%s/%s", j->where, j->list[i].name);
        if (file_remove(p)) log_info("backups: removed %s (the newest of the last %d days stay)", j->list[i].name, BACKUP_DAYS);
    }
    DirIter *it = dir_open(j->where);
    DirEntry e;
    while (it && dir_next(it, &e)) {
        size_t n = strlen(e.name);
        if (n > 5 && memcmp(e.name + n - 5, ".part", 5) == 0 && str_starts(e.name, "hemma-")) {
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", j->where, e.name);
            file_remove(p);
        }
    }
    dir_close(it);
    list_dir(j);
}

static void run(Job *j) {
    resolve(j);
    if (j->kind == JOB_BACKUP) {
        char to[740];
        snprintf(to, sizeof to, "%s/%s", j->where, j->name);
        j->ok = dir_make(j->where);
        if (!j->ok) { /* (kept for later, in whoever's language reads it: i18n_pack) */
            char where[300];
            str_copy(where, sizeof where, j->where);
            i18n_pack(j->why, sizeof j->why, N_("the folder %s can't be made - is the disk there, and may Hemma write to it?"),
                      where);
        }
        if (j->ok) j->ok = store_backup(to, &j->rows, j->why, sizeof j->why);
        int64_t size = 0;
        if (j->ok && path_stat(to, &size, NULL, NULL)) j->bytes = size;
        if (j->ok) rotate(j);
        else list_dir(j);
    } else if (j->kind == JOB_STAGE) {
        char from[740];
        snprintf(from, sizeof from, "%s/%s", j->where, j->name);
        j->ok = store_stage_restore(j->data, from, j->name, NULL, 0, j->why, sizeof j->why);
        list_dir(j);
    } else {
        list_dir(j);
        j->ok = true;
    }
}

static void finished(Job *j) {
    lock_take(&g_lock);
    free(g_done); /* (never two: the loop starts one job at a time) */
    g_done = j;
    lock_give(&g_lock);
}

static void worker(void *arg) {
    thread_background();
    run(arg);
    finished(arg);
    app_tick_soon();
}

/* ---------------------------------------------------------- the event loop */

static const char *data_dir(void) { return system_data_path(); }

static void refresh_waiting(int64_t now) {
    g_waiting = store_restore_waiting(data_dir(), g_wait_label, sizeof g_wait_label);
    g_wait_at = now ? now : 1;
}

static bool start_job(JobKind kind, bool manual, const char *who, const char *name) {
    if (g_busy) return false;
    Job *j = xcalloc(1, sizeof *j);
    j->kind = kind;
    j->manual = manual;
    str_copy(j->who, sizeof j->who, who ? who : "");
    str_copy(j->name, sizeof j->name, name ? name : "");
    str_copy(j->data, sizeof j->data, data_dir());
    str_copy(j->chosen, sizeof j->chosen, setting(SET_BACKUP_DIR, ""));
    const char *s = setting("media.roots", ""); /* the library folders, one a line (as media/library.c) */
    while (*s && j->nroots < MAX_ROOTS) {
        size_t len = strcspn(s, "\n");
        str_ncopy(j->roots[j->nroots], sizeof j->roots[0], s, len);
        str_trim(j->roots[j->nroots]);
        if (j->roots[j->nroots][0]) j->nroots++;
        s += len;
        if (*s == '\n') s++;
    }
    g_busy = true;
    g_busy_kind = kind;
    if (app_testing()) {
        run(j);
        finished(j);
        system_backup_tick(now_ms());
    } else if (!thread_spawn(worker, j)) {
        g_busy = false;
        free(j);
        return false;
    }
    app_changed_where("/system");
    return true;
}

static void take_result(int64_t now_ms_) {
    lock_take(&g_lock);
    Job *j = g_done;
    g_done = NULL;
    lock_give(&g_lock);
    if (!j) return;
    g_busy = false;
    memcpy(g_list, j->list, sizeof g_list);
    g_n = j->n;
    str_copy(g_where, sizeof g_where, j->where);
    g_same_disk = j->same_disk;
    g_known_disk = j->known_disk;
    g_listed = true;
    int64_t now = now_sec();
    char line[700], why_en[400];
    Lang was = i18n_use(LANG_EN); /* (the log's words; under tests this runs inside someone's action) */
    i18n_unpack_to(why_en, sizeof why_en, j->why);
    i18n_use(was);
    if (j->kind == JOB_BACKUP && j->ok) {
        snprintf(line, sizeof line, "%lld|%s|%lld|%lld", (long long)now, j->name, j->rows, (long long)j->bytes);
        setting_set(SET_BACKUP_LAST, line);
        setting_set(SET_BACKUP_FAIL, "");
        log_info("backups: %s made and checked (%lld rows, %lld KB) in %s%s%s", j->name, j->rows,
                 (long long)(j->bytes >> 10), j->where, j->who[0] ? " - asked for by " : "", j->who);
        if (j->manual) {
            g_note = NOTE_DONE;
            str_copy(g_note_arg, sizeof g_note_arg, j->name);
        }
    } else if (j->kind == JOB_BACKUP) {
        snprintf(line, sizeof line, "%lld|%s", (long long)now, j->why); /* (the reason as kept: shown in words later) */
        setting_set(SET_BACKUP_FAIL, line);
        g_next_try = now + RETRY_S;
        log_error("backups: %s failed: %s", j->name, why_en);
        if (j->manual) {
            g_note = NOTE_FAILED;
            str_copy(g_note_arg, sizeof g_note_arg, j->why);
        }
    } else if (j->kind == JOB_STAGE && j->ok) {
        str_copy(g_restoring, sizeof g_restoring, j->name);
        refresh_waiting(now_ms_);
        if (system_restarts_itself() && !app_testing()) {
            g_quit_at = now_ms_ + QUIT_AFTER_MS;
            app_wake_in(QUIT_AFTER_MS);
            log_warn("backups: %s chose to go back to %s - Hemma restarts to put it in place", j->who, j->name);
        } else {
            log_warn("backups: %s chose to go back to %s - it's put in place when Hemma next starts", j->who, j->name);
        }
    } else if (j->kind == JOB_STAGE) {
        g_note = NOTE_NOT_BACK;
        str_copy(g_note_arg, sizeof g_note_arg, j->why);
        log_error("backups: going back to %s failed: %s", j->name, why_en);
    }
    free(j);
    app_changed_where("/system");
}

bool system_backup_due(int64_t now, int64_t last, int64_t started) {
    if (!last) return now - started >= 180; /* (the first: a few minutes after the start) */
    if (now - last >= 26 * 3600) return true;
    return now - last >= 20 * 3600 && dt_local(now).hour == 3;
}

void system_backup_tick(int64_t now) {
    take_result(now);
    if (g_quit_at && now >= g_quit_at) {
        g_quit_at = 0;
        app_quit(HEMMA_EXIT_RESTART);
        return;
    }
    if (!g_wait_at || now - g_wait_at >= 60000) refresh_waiting(now); /* (hemma --restore while Hemma runs) */
    int64_t t = now_sec(), last = atoll(setting(SET_BACKUP_LAST, "0"));
    if (!g_busy && !g_restoring[0] && !g_waiting && !app_testing() && t >= g_next_try &&
        system_backup_due(t, last, g_started)) {
        char name[32];
        make_name(t, name);
        if (!start_job(JOB_BACKUP, false, "", name)) g_next_try = t + 60;
    }
}

void system_backup_start(void) {
    g_started = now_sec();
    g_next_try = g_quit_at = 0;
    g_restoring[0] = g_note_arg[0] = '\0';
    g_note = NOTE_NONE;
    g_busy = g_listed = false;
    g_n = 0;
    refresh_waiting(now_ms());
    if (!app_testing()) start_job(JOB_LIST, false, "", "");
}

void system_backup_stop(void) {
    lock_take(&g_lock);
    free(g_done);
    g_done = NULL;
    lock_give(&g_lock);
}

/* ------------------------------------------------------------- the check */

static void size_words(int64_t n, char *out, size_t cap) {
    if (n >= (10LL << 20)) snprintf(out, cap, "%lld MB", (long long)(n >> 20));
    else if (n >= (1LL << 20)) snprintf(out, cap, "%.1f MB", (double)n / 1048576.0);
    else snprintf(out, cap, "%lld KB", (long long)((n + 1023) >> 10));
    char mark = i18n_decimal(); /* (a decimal comma in Swedish and Spanish) */
    for (char *s = out; mark != '.' && *s; s++)
        if (*s == '.') *s = mark;
}

void system_backup_checks(Health *h) {
    char d[900], when[64], why[400];
    int64_t now = now_sec();
    char restored[768]; /* (the core's words, kept packed: store_restore_packed) */
    i18n_unpack_to(restored, sizeof restored, setting(SETTING_RESTORED, ""));
    if (restored[0]) health_add(h, HEALTH_INFO, T("Backup put back"), restored, NULL, "/system/backups");
    if (g_waiting && !g_quit_at) {
        snprintf(d, sizeof d, T("The backup %s waits to be put back: Hemma does it at its next start."), g_wait_label);
        health_add(h, HEALTH_WARN, T("Going back to a backup"), d,
                   T("Restart Hemma to finish it - or say \"Don't put it back\" on the Backups tab."), "/system/backups");
    }
    const char *last = setting(SET_BACKUP_LAST, ""), *fail = setting(SET_BACKUP_FAIL, "");
    int64_t at = atoll(last), failed = atoll(fail);
    const char *bar = strchr(fail, '|');
    if (failed && failed >= at) {
        system_when(failed, now, when, sizeof when);
        i18n_unpack_to(why, sizeof why, bar ? bar + 1 : "?");
        snprintf(d, sizeof d, T("The backup at %s failed: %s"), when, why);
        health_add(h, HEALTH_BAD, T("Backups"), d, T("See the Backups tab: is the folder there, and may Hemma write to it?"),
                   "/system/backups");
        return;
    }
    if (!at) {
        health_add(h, HEALTH_INFO, T("Backups"),
                   T("No backup yet: the first one is made a few minutes after Hemma starts, then one every night."), NULL,
                   "/system/backups");
        return;
    }
    system_when(at, now, when, sizeof when);
    const char *p = strchr(last, '|');
    p = p ? strchr(p + 1, '|') : NULL;
    long long rows = p ? atoll(p + 1) : -1;
    /* (one whole sentence per case: whether the rows are known, whether the folder was listed, one kept or more) */
    if (rows >= 0 && g_listed) {
        if (g_n == 1)
            snprintf(d, sizeof d, TN(rows, "The last one: %s (checked, %lld row), the only one kept in %s.",
                                     "The last one: %s (checked, %lld rows), the only one kept in %s."),
                     when, rows, g_where);
        else
            snprintf(d, sizeof d, TN(rows, "The last one: %s (checked, %lld row), %d kept in %s.",
                                     "The last one: %s (checked, %lld rows), %d kept in %s."),
                     when, rows, g_n, g_where);
    } else if (rows >= 0) {
        snprintf(d, sizeof d, TN(rows, "The last one: %s (checked, %lld row).", "The last one: %s (checked, %lld rows)."),
                 when, rows);
    } else if (g_listed) {
        if (g_n == 1) snprintf(d, sizeof d, T("The last one: %s (checked), the only one kept in %s."), when, g_where);
        else snprintf(d, sizeof d, T("The last one: %s (checked), %d kept in %s."), when, g_n, g_where);
    } else {
        snprintf(d, sizeof d, T("The last one: %s (checked)."), when);
    }
    if (now - at > 2 * 86400)
        health_add(h, HEALTH_WARN, T("Backups"), d,
                   T("Back up now on the Backups tab - and see in the log why the nightly one didn't run."),
                   "/system/backups");
    else if (g_listed && g_known_disk && g_same_disk)
        health_add(h, HEALTH_INFO, T("Backups"), d,
                   T("They're on the same disk as the data: if that disk dies, they go with it. A folder on another disk "
                     "(a USB disk) keeps them safe - choose one on the Backups tab."),
                   "/system/backups");
    else health_add(h, HEALTH_OK, T("Backups"), d, NULL, "/system/backups");
}

/* -------------------------------------------------------------- the page */

void system_backup_page(Ctx *c, Buf *b) {
    int64_t now = now_sec();
    char when[64], size[32], q[600];
    char restored[768];
    i18n_unpack_to(restored, sizeof restored, setting(SETTING_RESTORED, ""));
    if (restored[0]) {
        H(b, "<div class=\"notice system-bk-note\" id=\"system-bk-restored\"><p>%s</p>", restored);
        ui_act(b, "system.restored_seen", "btn sm", T("Got it"), NULL, NULL, NULL);
        buf_puts(b, "</div>");
    }
    bool waiting = g_waiting;
    if (g_quit_at) {
        buf_puts(b, "<p class=\"notice system-bk-note\" id=\"system-bk-wait\" role=\"status\">");
        H(b, T("Going back to %s: Hemma restarts now to put it in place. This page comes back in a few seconds."),
          g_restoring);
        buf_puts(b, "</p>");
    } else if (waiting) {
        buf_puts(b, "<div class=\"notice system-bk-note\" id=\"system-bk-wait\"><p>");
        H(b, T("<b>%s waits to be put back.</b> Restart Hemma to finish it: it's put in place as Hemma starts, and "
               "what's there now is kept aside."),
          g_wait_label);
        buf_puts(b, "</p>");
        ui_act(b, "system.backup_cancel", "btn sm", T("Don't put it back"), NULL, NULL, NULL);
        buf_puts(b, "</div>");
    }
    if (g_busy) {
        H(b, "<p class=\"notice system-bk-note\" id=\"system-bk-busy\" role=\"status\">%s</p>",
          g_busy_kind == JOB_BACKUP  ? T("Backing up…")
          : g_busy_kind == JOB_STAGE ? T("Getting the backup ready…")
                                     : T("Looking in the backup folder…"));
    } else if (g_note != NOTE_NONE) {
        char arg[512];
        i18n_unpack_to(arg, sizeof arg, g_note_arg); /* (a reason kept for later; a name or the core's words as they are) */
        buf_puts(b, "<p class=\"notice system-bk-note\" id=\"system-bk-said\">");
        if (g_note == NOTE_DONE) H(b, T("Backed up and checked: %s."), arg);
        else if (g_note == NOTE_FAILED) H(b, T("The backup failed: %s"), arg);
        else H(b, T("Not put back: %s"), arg);
        buf_puts(b, "</p>");
    }

    ui_card(b, T("Backups"), NULL, NULL);
    buf_puts(b, "<p>");
    H(b, T("Every night Hemma copies its database, checks the copy, and keeps the newest of the last %d days."),
      BACKUP_DAYS);
    buf_puts(b, "</p>");
    if (g_listed) {
        buf_puts(b, "<p class=\"small\">");
        H(b, !g_known_disk ? T("In <code>%s</code>")
             : g_same_disk ? T("In <code>%s</code> - on the same disk as the data")
                           : T("In <code>%s</code> - on another disk than the data"),
          g_where);
        buf_puts(b, "</p>");
    }
    ui_form(b, "system.backup_now", "system-bk-now");
    H(b, "<button class=\"btn\"%r>%s</button>", g_busy || g_quit_at ? " disabled" : "", T("Back up now"));
    ui_form_end(b);
    if (!g_n) {
        if (g_listed) H(b, "<p class=\"muted\">%s</p>", T("No backups yet."));
    } else buf_puts(b, "<ul class=\"rows system-bk-list\" id=\"system-bk-list\">");
    for (int i = 0; i < g_n; i++) {
        const Entry *e = &g_list[i];
        system_when(name_time(e->name), now, when, sizeof when);
        size_words(e->size, size, sizeof size);
        H(b, "<li class=\"row\" id=\"system-bk-%s\"><div class=\"row-main\"><b>%s</b><small class=\"muted\">%s · %s"
             "</small></div><div class=\"system-bk-a\">",
          e->name + 6, when, e->name, size);
        char tar[80];
        snprintf(tar, sizeof tar, "/system/backups/%.*s.tar", (int)(strlen(e->name) - 3), e->name);
        H(b, "<a class=\"btn sm\" href=\"%s\" data-native download title=\"%s\">%s</a>", tar,
          T("Includes the password secret"), T("Download"));
        snprintf(q, sizeof q, T("Go back to the backup of %s? Everything changed since then is lost - what's there now "
                                "is kept aside as a copy. Hemma restarts to do it."),
                 when);
        ui_form_confirm(b, "system.backup_restore", "system-bk-restore", q);
        ui_hidden(b, "name", e->name);
        H(b, "<button class=\"btn sm ghost\"%r>%s</button>", g_busy || g_quit_at || waiting ? " disabled" : "",
          T("Go back to this"));
        ui_form_end(b);
        buf_puts(b, "</div></li>");
    }
    if (g_n) buf_puts(b, "</ul>");
    ui_card_end(b);

    ui_card(b, T("A new memory card or computer"), NULL, NULL);
    H(b, "<p>%r</p><p class=\"muted small\">%s</p>",
      T("<b>Download</b> gives you a backup as one file: the database and the password secret "
        "(<code>HEMMA_AUTH_SALT</code>) every password is made with. Keep it like a password. On a new memory card or "
        "another computer, install Hemma, then <code>hemma --restore</code> that file - docs/raspberry-pi.md, "
        "\"Backups\", has the steps."),
      T("The nightly backups leave the secret out: on their own they can't sign anyone in on a new install - keep a "
        "copy of the secret (Getting started) or a download."));
    ui_card_end(b);

    ui_card(b, T("Where they go"), NULL, NULL);
    ui_form(b, "system.backup_dir", "stack system-bk-dir");
    char note[600];
    snprintf(note, sizeof note, T("Empty: Hemma chooses - a hidden .hemma-backups folder in the first library folder on "
                                  "another disk than the data, else %s/backups. Hemma must be allowed to write there (on "
                                  "a Pi: a folder you gave install-pi.sh)."),
             data_dir());
    H(b, "<label class=\"field\"><span>%s</span><input name=\"dir\" value=\"%s\" maxlength=\"500\" "
         "spellcheck=\"false\" autocomplete=\"off\" placeholder=\"%s\"><small class=\"muted\">%s</small></label>",
      T("Backup folder"), setting(SET_BACKUP_DIR, ""), T("Hemma chooses"), note);
    H(b, "<button class=\"btn\">%s</button>", T("Save"));
    ui_form_end(b);
    ui_card_end(b);
}

/* --------------------------------------------------------------- actions */

static bool admin_ok(Ctx *c) {
    if (is_admin(c)) return true;
    ctx_fail(c, "%s", T("Only an admin can do that"));
    return false;
}

void system_backup_act_now(Ctx *c, Fields *f) {
    if (!admin_ok(c)) return;
    if (g_quit_at) {
        ctx_fail(c, "%s", T("Hemma is restarting to go back to a backup"));
        return;
    }
    char name[32];
    make_name(now_sec(), name);
    g_note = NOTE_NONE;
    if (!start_job(JOB_BACKUP, true, c->me->name, name)) {
        ctx_fail(c, "%s", T("Busy with the backups right now - try again in a moment"));
        return;
    }
    ctx_ok(c, "%s", T("Backing up…"));
}

void system_backup_act_restore(Ctx *c, Fields *f) {
    if (!admin_ok(c)) return;
    const char *name = fields_str(f, "name");
    bool known = false;
    for (int i = 0; i < g_n; i++) known |= str_eq(g_list[i].name, name);
    if (!valid_name(name) || !known) {
        ctx_fail(c, "%s", T("That backup isn't there any more"));
        return;
    }
    if (g_quit_at) {
        ctx_fail(c, "%s", T("Hemma is already restarting to go back to a backup"));
        return;
    }
    g_note = NOTE_NONE;
    if (!start_job(JOB_STAGE, true, c->me->name, name)) {
        ctx_fail(c, "%s", T("Busy with the backups right now - try again in a moment"));
        return;
    }
    ctx_ok(c, "%s", system_restarts_itself() ? T("Getting it ready - then Hemma restarts") : T("Getting it ready…"));
}

void system_backup_act_cancel(Ctx *c, Fields *f) {
    if (!admin_ok(c)) return;
    if (g_quit_at) {
        ctx_fail(c, "%s", T("Too late: Hemma is restarting to put it back"));
        return;
    }
    store_unstage_restore(data_dir());
    g_restoring[0] = '\0';
    refresh_waiting(now_ms());
    log_info("backups: %s cancelled going back to a backup", c->me->name);
    ctx_ok(c, "%s", T("It won't be put back"));
}

void system_backup_act_dir(Ctx *c, Fields *f) {
    if (!admin_ok(c)) return;
    char dir[520];
    str_copy(dir, sizeof dir, fields_str(f, "dir"));
    str_trim(dir);
    size_t n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) dir[--n] = '\0';
    bool absolute = dir[0] == '/' || dir[0] == '\\' || (dir[0] && dir[1] == ':');
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)dir[i] < 32) absolute = false;
    if (n && !absolute) {
        ctx_fail(c, "%s", T("Give the whole path, e.g. /mnt/media/hemma-backups"));
        return;
    }
    setting_set(SET_BACKUP_DIR, dir);
    log_info("backups: %s set the backup folder to %s", c->me->name, n ? dir : "the default");
    start_job(JOB_LIST, true, c->me->name, "");
    ctx_ok(c, "%s", n ? T("Saved - the next backup goes there") : T("Saved - Hemma chooses the folder"));
}

void system_backup_act_restored_seen(Ctx *c, Fields *f) {
    if (!admin_ok(c)) return;
    setting_set(SETTING_RESTORED, "");
}

/* ------------------------------------------------------------ the download */

bool system_backup_bundle(const char *path, const char *salt, int64_t mtime, Buf *out) {
    Buf db = {0};
    if (!file_read_all(path, &db, (size_t)1 << 30)) return false;
    Buf txt = {0}; /* (in the language of whoever downloads it: the download's thread speaks it) */
    buf_printf(&txt, "%s\n\nHEMMA_AUTH_SALT=%s\n\n%s\n", T("SECRET - keep this file like a password."), salt ? salt : "",
               T("Every password and recovery code in this backup is made with the line above.\n"
                 "To put it back on a new memory card or another computer, run hemma --restore with this .tar file\n"
                 "(docs/raspberry-pi.md, \"Backups\"). It tells you if the settings file there needs this line."));
    tar_add(out, "hemma.db", db.p, db.len, mtime, 0600);
    tar_add(out, "hemma-secret.txt", txt.p, txt.len, mtime, 0600);
    tar_end(out);
    buf_free(&db);
    buf_free(&txt);
    return true;
}

typedef struct {
    sock_t s;
    Buf head; /* bytes the connection still owed (pipelining) */
    char path[760], name[32], salt[160];
    int64_t mtime;
    Lang lang; /* the one who asked for it */
} Download;

static bool send_all(sock_t s, const char *p, size_t n) {
    int waited = 0;
    while (n) {
        long k = sock_send(s, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            waited = 0;
            continue;
        }
        if (k < 0 && sock_again(sock_errno())) {
            pollfd_t pf = {.fd = s, .events = POLLOUT};
            int rc = plat_poll(&pf, 1, 1000);
            if (rc < 0 || (rc > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL)))) return false;
            if (rc == 0 && (waited += 1000) >= 30000) return false;
            continue;
        }
        return false;
    }
    return true;
}

static void download_thread(void *arg) {
    Download *d = arg;
    i18n_use(d->lang); /* (this thread's own language: the secret file's words and the answer below) */
    Buf tar = {0}, out = {0};
    buf_put(&out, d->head.p, d->head.len);
    if (system_backup_bundle(d->path, d->salt, d->mtime, &tar)) {
        buf_printf(&out,
                   "HTTP/1.1 200 OK\r\nServer: hemma\r\nContent-Type: application/x-tar\r\nContent-Length: %lld\r\n"
                   "Content-Disposition: attachment; filename=\"%.*s-with-secret.tar\"\r\nCache-Control: no-store\r\n"
                   "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                   (long long)tar.len, (int)(strlen(d->name) - 3), d->name);
        buf_put(&out, tar.p, tar.len);
    } else {
        const char *gone = T("the backup can't be read (has the disk gone?)");
        buf_printf(&out,
                   "HTTP/1.1 404 Not Found\r\nServer: hemma\r\nContent-Type: text/plain; charset=utf-8\r\n"
                   "Content-Length: %lld\r\nConnection: close\r\n\r\n%s\n",
                   (long long)strlen(gone) + 1, gone);
    }
    sock_nonblock(d->s, true);
    send_all(d->s, out.p, out.len);
    sock_shutdown_write(d->s);
    sock_close(d->s);
    memset(d->salt, 0, sizeof d->salt);
    buf_free(&tar);
    buf_free(&out);
    buf_free_on_loop(&d->head); /* (the event loop's: base.h free_on_loop) */
    free_on_loop(d);
    atomic_fetch_sub(&g_downloads, 1);
    i18n_use(LANG_EN);
}

bool system_backup_http(Ctx *c, Conn *conn, HttpReq *r) {
    static const char pre[] = "/system/backups/";
    if (r->streaming || !str_starts(r->path, pre) || !is_admin(c)) return false;
    char name[40];
    const char *rest = r->path + sizeof pre - 1;
    size_t n = strlen(rest);
    if (n < 5 || n > 30 || !str_ends(rest, ".tar")) return false;
    snprintf(name, sizeof name, "%.*s.db", (int)(n - 4), rest);
    if (!valid_name(name)) return false;
    if (!str_eq(r->method, "GET")) {
        conn_reply(conn, 405, "text/plain", "GET only\n", 9, "Allow: GET\r\n");
        return true;
    }
    if (!g_listed || atomic_load(&g_downloads) >= MAX_DOWNLOADS) {
        char busy[120];
        snprintf(busy, sizeof busy, "%s\n", T("Busy - try again in a moment"));
        conn_reply(conn, 503, "text/plain; charset=utf-8", busy, strlen(busy), "Retry-After: 2\r\n");
        return true;
    }
    Download *d = xcalloc(1, sizeof *d);
    d->lang = i18n_lang();
    snprintf(d->path, sizeof d->path, "%s/%s", g_where, name);
    str_copy(d->name, sizeof d->name, name);
    str_copy(d->salt, sizeof d->salt, env("HEMMA_AUTH_SALT", ""));
    d->mtime = name_time(name);
    log_info("backups: %s downloaded %s (with the password secret)", c->me->name, name);
    atomic_fetch_add(&g_downloads, 1);
    Detached det;
    conn_detach(conn, r, &det);
    d->s = det.fd;
    d->head = det.unsent;
    buf_free(&det.body);
    if (!thread_spawn(download_thread, d)) {
        sock_close(d->s);
        buf_free(&d->head);
        free(d);
        atomic_fetch_sub(&g_downloads, 1);
    }
    return true;
}
