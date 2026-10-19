/* modules/system/system.h — internals of the System page, shared with its tests.
 * Nothing outside modules/system includes this: other modules report through the core's
 * health hook (core/app.h), never by linking here. */
#ifndef HEMMA_SYSTEM_H
#define HEMMA_SYSTEM_H

#include "../../core/app.h"

/* ---- the server's own log (log.c) ----
   Every line log_at lets through lands in a ring of the last SYSTEM_LOG_RING lines and, from
   the tick, in <data>/hemma.log (hemma.log.1 after ~1 MB). The tail of the previous file is
   read back at start behind a "Hemma restarted" divider. */
#define SYSTEM_LOG_RING 2000

typedef struct {
    uint32_t seq; /* 1, 2, ... in order of arrival: ids for the page */
    int64_t ts;   /* unix seconds */
    char level;   /* 'D' 'I' 'W' 'E', or '-' = a divider ("Hemma restarted") */
    bool old;     /* from before this start (read back from the file) */
    char *text;
} LogLine;

void system_log_start(const char *data_dir); /* read back the previous file's tail; this run appends */
void system_log_tick(int64_t now_ms, bool force); /* write waiting lines (every 2 s at most unless forced) */
void system_log_stop(void);                       /* write the rest and a "stopped" mark */
uint32_t system_log_seq(void);                    /* number of the newest line (0 = none yet) */
int system_log_count(void);                       /* lines in the ring */
/* Newest first: the lines that pass the filter (problems = warnings and errors only; q = folded
   substring), up to max copied into out (free them with system_log_free). Dividers come along
   when there's no search. Returns how many lines match in all. */
int system_log_find(LogLine *out, int max, int *nout, bool problems, const char *q);
void system_log_free(LogLine *lines, int n);
int system_log_errors_since(int64_t ts); /* error lines of this run logged at or after ts */
/* The previous run ended without its "stopped" mark (a crash, a power cut): when its last line was. */
bool system_log_unclean(int64_t *last_ts);
bool system_log_write_failed(char *why, size_t cap); /* the last write to the file failed */
const char *system_log_path(void);                   /* "<data>/hemma.log" ("" before start) */
/* For the download: the rotated file and the current one (or the ring, when there's no file). */
void system_log_text(Buf *out);

/* ---- backups (backup.c, issue #35) ----
   Every night (and with "Back up now") a checked copy of the database goes into the backup folder:
   SET_BACKUP_DIR, else a hidden .hemma-backups folder in the first library folder on another disk
   than the data (setting media.roots, read as text - no link to the media module), else
   <data>/backups. Named hemma-YYYY-MM-DD-HHMM.db; the newest of each of the last BACKUP_DAYS days
   stay. The work happens on a thread (a USB disk may have to spin up); the page shows what the
   last job found. Restore = store_stage_restore + a restart (a service manager starts Hemma again;
   from a terminal, the admin does). The download is a .tar: the backup + hemma-secret.txt. */
#define BACKUP_DAYS 7
#define SET_BACKUP_DIR "system.backup_dir"   /* the folder an admin chose ("" = the default) */
#define SET_BACKUP_LAST "system.backup_last" /* the newest good one: "<unix>|<name>|<rows>|<bytes>" */
#define SET_BACKUP_FAIL "system.backup_fail" /* the last attempt, when it failed: "<unix>|<why>" */
void system_backup_start(void);           /* from start(): list the folder (in the background) */
void system_backup_stop(void);
void system_backup_tick(int64_t now_ms);  /* the nightly backup; a finished job's results */
void system_backup_checks(Health *h);     /* the Backups check (and a restore waiting/done) */
void system_backup_page(Ctx *c, Buf *b);  /* /system/backups */
bool system_backup_http(Ctx *c, Conn *conn, HttpReq *r); /* GET /system/backups/<name>.tar */
void system_backup_act_now(Ctx *c, Fields *f);
void system_backup_act_restore(Ctx *c, Fields *f);
void system_backup_act_cancel(Ctx *c, Fields *f);
void system_backup_act_dir(Ctx *c, Fields *f);
void system_backup_act_restored_seen(Ctx *c, Fields *f);
/* When the nightly backup is due: none yet (a few minutes after the start), over 26 h old, or over
   20 h old at 03:xx local time. */
bool system_backup_due(int64_t now, int64_t last, int64_t started);
/* The download: the backup at `path` as hemma.db + the password secret line, in a tar. */
bool system_backup_bundle(const char *path, const char *salt, int64_t mtime, Buf *out);

/* ---- shared with backup.c (system.c) ---- */
void system_when(int64_t t, int64_t now, char *out, size_t cap); /* "14:30" today, else "Mon 12 Oct 14:30" */
bool system_restarts_itself(void); /* a service manager starts Hemma again when it stops (systemd, launchd) */
const char *system_data_path(void);

/* ---- tests ---- */
void system_log_reset(void);               /* empty ring, nothing waiting for the file */
void system_log_set_limit(int64_t bytes);  /* rotate the file past this size (default ~1 MB) */
/* Pretend the LAN addresses are these ("" = none found); NULL = ask the OS again (not under tests). */
void system_test_addresses(const char *a, const char *b);
void system_test_free_space(int64_t bytes); /* the data folder's disk has this much free (-1 = ask the disk) */

#endif
