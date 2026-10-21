/* modules/system/log.c — the server's own log, kept for the System page.
 *
 * core/base.c hands every line log_at lets through to sink(), on whatever thread logged it.
 * The line goes into a ring of the last SYSTEM_LOG_RING lines in memory, and into a buffer
 * for the log file <data>/hemma.log that the module's tick appends to the file - at most
 * every two seconds, never fsynced (a Pi runs from an SD card). Past ~1 MB the file becomes
 * hemma.log.1 (replacing the one before). At start the tail of the previous file is read
 * back into the ring behind a "Hemma restarted" divider, so what happened before a crash or
 * a restart is still there to see. The file marks each start and each clean stop
 * ("- Hemma 0.2.0 started", "- Hemma stopped"): a start without a stop is a crash or a power
 * cut, which the checks mention.
 *
 * File lines: "2026-10-12 14:30:05 W text" (D I W E = detail, info, warning, problem; '-' =
 * those marks). Texts are log_clean()ed already: one line each, no control characters. */
#include "system.h"
#include "../../core/plat.h"

#define LINE_MAX_BYTES 1000      /* kept per line */
#define PENDING_MAX (256 << 10)  /* waiting for the file; past this, lines are counted instead */
#define TAIL_BYTES (96 << 10)    /* read back from the previous file at start */
#define TAIL_LINES 400
#define WRITE_EVERY_MS 2000
#define DEFAULT_LIMIT (1 << 20)

static Lock g_lock = LOCK_INIT; /* the ring and g_pending: any thread logs */
static LogLine g_ring[SYSTEM_LOG_RING];
static int g_first, g_n; /* oldest line at g_first */
static uint32_t g_seq;
static Buf g_pending; /* file lines the tick hasn't written yet */
static int g_dropped;

/* event loop only */
static char g_path[700];
static int64_t g_size, g_written_at, g_limit = DEFAULT_LIMIT;
static bool g_failed, g_unclean;
static char g_failed_why[120];
static int64_t g_unclean_ts;

static const char LEVELS[] = "DIWE";

static void push(char level, int64_t ts, bool old, const char *text, size_t n) { /* g_lock held */
    LogLine *l;
    if (g_n < SYSTEM_LOG_RING) {
        l = &g_ring[(g_first + g_n) % SYSTEM_LOG_RING];
        g_n++;
    } else {
        l = &g_ring[g_first];
        g_first = (g_first + 1) % SYSTEM_LOG_RING;
        free(l->text);
    }
    if (n > LINE_MAX_BYTES) { /* (never splitting a UTF-8 sequence) */
        n = LINE_MAX_BYTES;
        while (n && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    }
    l->text = xmalloc(n + 1);
    memcpy(l->text, text, n);
    l->text[n] = '\0';
    l->seq = ++g_seq;
    l->ts = ts;
    l->level = level;
    l->old = old;
}

static void file_line(Buf *b, int64_t ts, char level, const char *text) {
    DateTime d = dt_local(ts);
    buf_printf(b, "%04d-%02d-%02d %02d:%02d:%02d %c %s\n", d.year % 10000, d.mon % 100, d.day % 100, d.hour % 100,
               d.min % 100, d.sec % 100, level, text);
}

/* core/base.c calls this for every line, on any thread (never logs itself). */
static void sink(LogLevel level, int64_t ts, const char *line) {
    char lv = LEVELS[level < LOG_ERROR ? level : LOG_ERROR];
    lock_take(&g_lock);
    push(lv, ts, false, line, strlen(line));
    if (g_pending.len < PENDING_MAX) file_line(&g_pending, ts, lv, line);
    else g_dropped++;
    lock_give(&g_lock);
}

/* From the very first line (main.c's "settings: ..."), before any module starts: lines logged
   before start wait in the ring and the file buffer. */
__attribute__((constructor)) static void system_log_hook(void) { log_set_sink(sink); }

/* ------------------------------------------------------------- reading back */

typedef struct {
    int64_t ts;
    char level;
    size_t off, len; /* the text, in the tail buffer */
} Parsed;

/* "2026-10-12 14:30:05 W text" -> its parts. */
static bool parse_line(const char *s, size_t n, int64_t *ts, char *level, size_t *text_off) {
    static const int at[6] = {0, 5, 8, 11, 14, 17}, len[6] = {4, 2, 2, 2, 2, 2};
    if (n < 21 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':' || s[19] != ' ')
        return false;
    int v[6];
    for (int k = 0; k < 6; k++) {
        int x = 0;
        for (int j = 0; j < len[k]; j++) {
            char ch = s[at[k] + j];
            if (ch < '0' || ch > '9') return false;
            x = x * 10 + (ch - '0');
        }
        v[k] = x;
    }
    if (!s[20] || !strchr("DIWE-", s[20]) || (n > 21 && s[21] != ' ')) return false;
    *ts = dt_make(v[0], v[1], v[2], v[3], v[4]) + v[5];
    *level = s[20];
    *text_off = n > 22 ? 22 : n;
    return true;
}

void system_log_start(const char *data_dir) {
    snprintf(g_path, sizeof g_path, "%s/hemma.log", data_dir && *data_dir ? data_dir : ".");
    g_size = g_written_at = 0;
    g_failed = g_unclean = false;
    g_unclean_ts = 0;

    /* the end of the previous file */
    Buf tail = {0};
    int64_t size = 0;
    bool dir = false;
    if (path_stat(g_path, &size, NULL, &dir) && !dir && size > 0) {
        g_size = size;
        int64_t from = size > TAIL_BYTES ? size - TAIL_BYTES : 0;
        FILE *f = file_open(g_path, "rb");
        if (f) {
            size_t want = (size_t)(size - from);
            buf_reserve(&tail, want);
            if (file_pread(f, from, tail.p, want)) {
                tail.len = want;
                tail.p[want] = '\0';
            }
            fclose(f);
        }
        if (from > 0 && tail.len) { /* started mid-line: from the next one */
            const char *nl = memchr(tail.p, '\n', tail.len);
            buf_consume(&tail, nl ? (size_t)(nl - tail.p) + 1 : tail.len);
        }
    }
    Parsed *pl = NULL;
    int np = 0, cap = 0;
    for (size_t at = 0; at < tail.len;) {
        const char *s = tail.p + at, *nl = memchr(s, '\n', tail.len - at);
        size_t n = nl ? (size_t)(nl - s) : tail.len - at;
        size_t next = at + n + (nl ? 1 : 0);
        while (n && s[n - 1] == '\r') n--;
        Parsed p;
        size_t toff;
        if (parse_line(s, n, &p.ts, &p.level, &toff)) {
            p.off = at + toff;
            p.len = n - toff;
            if (np == cap) {
                cap = cap ? cap * 2 : 256;
                pl = xrealloc(pl, (size_t)cap * sizeof *pl);
            }
            pl[np++] = p;
        }
        at = next;
    }
    /* the last mark says how the previous run ended: "started" with no "stopped" after it = it never said goodbye */
    for (int i = np - 1; i >= 0; i--) {
        if (pl[i].level != '-') continue;
        const char *t = tail.p + pl[i].off;
        if (pl[i].len >= 7 && memcmp(t + pl[i].len - 7, "started", 7) == 0) {
            g_unclean = true;
            g_unclean_ts = pl[np - 1].ts;
        }
        break;
    }

    lock_take(&g_lock);
    /* what came before goes first, then the divider, then whatever this run logged so far */
    int keep_n = g_n;
    LogLine *keep = xcalloc((size_t)(keep_n ? keep_n : 1), sizeof *keep);
    for (int i = 0; i < keep_n; i++) keep[i] = g_ring[(g_first + i) % SYSTEM_LOG_RING];
    g_first = g_n = 0;
    for (int i = np > TAIL_LINES ? np - TAIL_LINES : 0; i < np; i++)
        push(pl[i].level, pl[i].ts, true, tail.p + pl[i].off, pl[i].len);
    if (np) {
        /* (English in the ring, like every line; the page says a divider in its reader's language) */
        const char *div = g_unclean ? N_("Hemma restarted - it had stopped without warning (a power cut or a crash?)")
                                    : N_("Hemma restarted");
        push('-', keep_n ? MIN(keep[0].ts, now_sec()) : now_sec(), false, div, strlen(div));
    }
    for (int i = 0; i < keep_n; i++) {
        push(keep[i].level, keep[i].ts, keep[i].old, keep[i].text, strlen(keep[i].text));
        free(keep[i].text);
    }
    /* the file: this start's mark, ahead of the lines already waiting */
    Buf pend = {0};
    char mark[64];
    snprintf(mark, sizeof mark, N_("Hemma %s started"), HEMMA_VERSION_FULL);
    file_line(&pend, now_sec(), '-', mark);
    if (g_pending.len) buf_put(&pend, g_pending.p, g_pending.len);
    buf_free(&g_pending);
    g_pending = pend;
    lock_give(&g_lock);
    free(keep);
    free(pl);
    buf_free(&tail);
}

/* -------------------------------------------------------------- writing */

void system_log_tick(int64_t now, bool force) {
    if (!g_path[0]) return;
    if (!force && g_written_at && now - g_written_at < WRITE_EVERY_MS) return;
    lock_take(&g_lock);
    Buf out = g_pending;
    g_pending = (Buf){0};
    int dropped = g_dropped;
    g_dropped = 0;
    lock_give(&g_lock);
    if (dropped) {
        char msg[120];
        snprintf(msg, sizeof msg, "(%d log line%s left out of this file: too many at once)", dropped,
                 dropped == 1 ? "" : "s");
        file_line(&out, now_sec(), 'W', msg);
    }
    if (!out.len) {
        buf_free(&out);
        return;
    }
    g_written_at = now ? now : 1;
    if (g_size > 0 && g_size + (int64_t)out.len > g_limit) { /* the file is full: it becomes hemma.log.1 */
        char old[720];
        snprintf(old, sizeof old, "%s.1", g_path);
        if (file_replace(g_path, old)) g_size = 0;
    }
    FILE *f = file_open(g_path, "ab");
    bool ok = f && fwrite(out.p, 1, out.len, f) == out.len;
    if (f && fclose(f) != 0) ok = false;
    if (ok) {
        g_size += (int64_t)out.len;
        g_failed = false;
    } else if (!g_failed) { /* once: the warning itself would be the next thing that can't be written */
        g_failed = true;
        str_copy(g_failed_why, sizeof g_failed_why,
                 f ? N_("writing to it failed (is the disk full?)") : N_("it can't be opened for writing"));
        log_warn("system: can't keep the log file %s - %s", g_path, g_failed_why);
    }
    buf_free(&out);
}

void system_log_stop(void) {
    if (!g_path[0]) return;
    lock_take(&g_lock);
    file_line(&g_pending, now_sec(), '-', N_("Hemma stopped"));
    lock_give(&g_lock);
    system_log_tick(now_ms(), true);
    g_path[0] = '\0';
}

/* -------------------------------------------------------------- reading */

uint32_t system_log_seq(void) {
    lock_take(&g_lock);
    uint32_t s = g_seq;
    lock_give(&g_lock);
    return s;
}

int system_log_count(void) {
    lock_take(&g_lock);
    int n = g_n;
    lock_give(&g_lock);
    return n;
}

int system_log_find(LogLine *out, int max, int *nout, bool problems, const char *q) {
    bool search = q && *q;
    int matched = 0, k = 0;
    lock_take(&g_lock);
    for (int i = g_n - 1; i >= 0; i--) {
        const LogLine *l = &g_ring[(g_first + i) % SYSTEM_LOG_RING];
        if (l->level == '-') {
            if (search) continue; /* a divider: shown in context, never a match */
        } else {
            if (problems && l->level != 'W' && l->level != 'E') continue;
            if (search && !fold_find(l->text, q)) continue;
            matched++;
        }
        if (k < max) {
            out[k] = *l;
            out[k].text = xstrdup(l->text);
            k++;
        }
    }
    lock_give(&g_lock);
    *nout = k;
    return matched;
}

void system_log_free(LogLine *lines, int n) {
    for (int i = 0; i < n; i++) {
        free(lines[i].text);
        lines[i].text = NULL;
    }
}

int system_log_errors_since(int64_t ts) {
    int n = 0;
    lock_take(&g_lock);
    for (int i = 0; i < g_n; i++) {
        const LogLine *l = &g_ring[(g_first + i) % SYSTEM_LOG_RING];
        n += l->level == 'E' && !l->old && l->ts >= ts;
    }
    lock_give(&g_lock);
    return n;
}

bool system_log_unclean(int64_t *last_ts) {
    if (last_ts) *last_ts = g_unclean_ts;
    return g_unclean;
}

bool system_log_write_failed(char *why, size_t cap) {
    if (g_failed && why) str_copy(why, cap, g_failed_why);
    return g_failed;
}

const char *system_log_path(void) { return g_path; }

void system_log_text(Buf *out) {
    if (g_path[0]) {
        system_log_tick(now_ms(), true); /* what's still waiting goes in too */
        char old[720];
        snprintf(old, sizeof old, "%s.1", g_path);
        size_t max = (size_t)g_limit * 4 + (4u << 20);
        file_read_all(old, out, max);
        file_read_all(g_path, out, max);
        if (out->len) return;
    }
    lock_take(&g_lock); /* no file (it couldn't be written): what's in memory */
    for (int i = 0; i < g_n; i++) {
        const LogLine *l = &g_ring[(g_first + i) % SYSTEM_LOG_RING];
        file_line(out, l->ts, l->level, l->text);
    }
    lock_give(&g_lock);
}

/* ---------------------------------------------------------------- tests */

void system_log_reset(void) {
    lock_take(&g_lock);
    for (int i = 0; i < g_n; i++) {
        LogLine *l = &g_ring[(g_first + i) % SYSTEM_LOG_RING];
        free(l->text);
        l->text = NULL;
    }
    g_first = g_n = 0;
    buf_free(&g_pending);
    g_dropped = 0;
    lock_give(&g_lock);
    g_unclean = g_failed = false;
}

void system_log_set_limit(int64_t bytes) { g_limit = bytes > 0 ? bytes : DEFAULT_LIMIT; }
