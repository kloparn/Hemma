/* core/base.h — basics shared by everything: memory, growable buffers,
 * strings & UTF-8, calendar/time helpers, hashing and logging.
 *
 * Pure C11. Anything that differs between Windows and POSIX lives in plat.h. */
#ifndef HEMMA_BASE_H
#define HEMMA_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define countof(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

#if defined(__GNUC__) || defined(__clang__)
#define FMT(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define FMT(fmt_idx, args_idx)
#endif

/* ---- memory: abort on OOM (a home server has nothing better to do) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* ---- Buf: growable byte buffer. Zero-initialise ({0}) before use. Once
 * anything was written, p is always NUL-terminated (the NUL isn't counted). */
typedef struct {
    char *p;
    size_t len, cap;
} Buf;

void buf_reserve(Buf *b, size_t extra);
void buf_put(Buf *b, const void *data, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) FMT(2, 3);
void buf_vprintf(Buf *b, const char *fmt, va_list ap);
void buf_consume(Buf *b, size_t n); /* drop n bytes from the front */
void buf_free(Buf *b);
const char *buf_cstr(Buf *b); /* NUL-terminated view; the NUL is not counted in len */
static inline void buf_reset(Buf *b) {
    b->len = 0;
    if (b->p) b->p[0] = '\0';
}

/* ---- strings ---- */
/* Copy with truncation that never splits a UTF-8 sequence. Always
 * NUL-terminates (if cap > 0). Returns the number of bytes copied. */
size_t str_copy(char *dst, size_t cap, const char *src);
size_t str_ncopy(char *dst, size_t cap, const char *src, size_t n);
bool str_eq(const char *a, const char *b);  /* NULL-safe */
bool str_ieq(const char *a, const char *b); /* ASCII case-insensitive, NULL-safe */
bool str_starts(const char *s, const char *prefix);
bool str_istarts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);
bool str_iends(const char *s, const char *suffix);
bool str_blank(const char *s);           /* NULL, empty or only whitespace */
char *str_trim(char *s);                 /* in place; returns s */
bool str_int(const char *s, long long *out); /* whole string must be a decimal integer */
long long str_int_or(const char *s, long long dflt);

/* ---- UTF-8 ---- */
size_t utf8_clip(const char *s, size_t max);                /* longest prefix <= max bytes ending on a char boundary */
int utf8_decode(const char *s, size_t n, uint32_t *cp);     /* bytes consumed (>=1 if n>0); invalid -> U+FFFD */
size_t utf8_encode(uint32_t cp, char out[4]);
bool utf8_valid(const char *s, size_t n);
size_t utf8_count(const char *s);                           /* code points */
/* Case-folded copy for matching: ASCII + Latin-1 letters (Å->å, Ä->ä, Ö->ö, É->é ...). */
size_t fold_copy(char *dst, size_t cap, const char *src);
/* Case-insensitive (folded) substring search. Returns pointer into hay or NULL. */
const char *fold_find(const char *hay, const char *needle);

/* ---- time & calendar (local time zone of the server) ---- */
extern int64_t g_fake_now; /* tests: when non-zero, now_sec() returns this */
int64_t now_sec(void);     /* wall clock, Unix seconds */
int64_t now_ms(void);      /* monotonic milliseconds */

typedef struct {
    int year, mon, day; /* mon 1..12 */
    int hour, min, sec;
    int wday; /* ISO: 0 = Monday .. 6 = Sunday */
    int yday; /* 0..365 */
} DateTime;

DateTime dt_local(int64_t t);
int utc_offset(int64_t t); /* seconds east of UTC at t, by this computer's time zone (7200 = UTC+2; 0 = unknown) */
/* Local wall time -> Unix time. Normalises out-of-range fields (day 32,
 * minute 90, negative days ...) and resolves DST like mktime(). */
int64_t dt_make(int year, int mon, int day, int hour, int min);
int64_t day_start(int64_t t);  /* local midnight */
int64_t week_start(int64_t t); /* local Monday 00:00 */
int64_t add_days(int64_t t, int days); /* same wall-clock time `days` later (DST-safe) */
int iso_week(int64_t t, int *iso_year);
int days_in_month(int year, int mon);
const char *weekday_name(int wday, bool abbrev); /* English, wday 0 = Monday */
const char *month_name(int mon, bool abbrev);    /* English, mon 1..12 */
bool parse_ymd(const char *s, int *y, int *m, int *d); /* "2026-10-12" */
bool parse_hm(const char *s, int *minutes);             /* "14:30" "14.30" "14" "1430" -> minutes after midnight */
void fmt_ymd(int64_t t, char out[11]);                  /* "2026-10-12" */
void fmt_hm(int64_t t, char out[6]);                    /* "14:30" */

/* ---- hashing ---- */
uint64_t hash64(const void *p, size_t n); /* FNV-1a */
uint64_t hash64_str(const char *s);
void hex_encode(const void *p, size_t n, char *out); /* writes 2n chars + NUL */

/* ---- logging (stderr, plus an optional sink) ---- */
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_QUIET } LogLevel;
extern LogLevel g_log_level;
extern bool g_log_stderr; /* default true; false = lines only reach the sink (tests) */
void log_at(LogLevel level, const char *fmt, ...) FMT(2, 3);
/* A second home for every line log_at lets through (at or above g_log_level), already cleaned
   (log_clean) and without the time/level prefix - e.g. the System page's log. log_at runs on
   ANY thread, so the sink locks whatever it touches, and it must never log itself. Set it
   before other threads start; NULL = none. Returns the sink it replaces (a test borrowing it
   puts it back). */
typedef void (*LogSink)(LogLevel level, int64_t unix_ts, const char *clean_line);
LogSink log_set_sink(LogSink fn);
/* What log_at does to every line: control characters (\n \r \t, ESC, C1 ...) become visible
   escapes, so text from visitors can't forge log lines. Returns the length written to out. */
size_t log_clean(const char *in, char *out, size_t cap);
#define log_debug(...) log_at(LOG_DEBUG, __VA_ARGS__)
#define log_info(...) log_at(LOG_INFO, __VA_ARGS__)
#define log_warn(...) log_at(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_at(LOG_ERROR, __VA_ARGS__)

#endif
