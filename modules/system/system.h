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

/* ---- tests ---- */
void system_log_reset(void);               /* empty ring, nothing waiting for the file */
void system_log_set_limit(int64_t bytes);  /* rotate the file past this size (default ~1 MB) */
/* Pretend the LAN addresses are these ("" = none found); NULL = ask the OS again (not under tests). */
void system_test_addresses(const char *a, const char *b);
void system_test_free_space(int64_t bytes); /* the data folder's disk has this much free (-1 = ask the disk) */
void system_test_zone(const char *sig);     /* the time zone, as "+01:00/+02:00" ("" = this computer's) */

#endif
