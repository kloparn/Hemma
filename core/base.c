/* core/base.c — see base.h */
#include "base.h"

#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ------------------------------------------------------------------ memory */

static void oom(size_t n) {
    fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom(n);
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) oom(count * size);
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) oom(n);
    return q;
}

char *xstrdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}

/* --------------------------------------------------------------------- Buf */

void buf_reserve(Buf *b, size_t extra) {
    size_t need = b->len + extra + 1; /* always keep room for a NUL */
    if (need <= b->cap) return;
    if (need <= b->len || extra > SIZE_MAX / 2) oom(extra); /* size_t wrapped (huge requests on 32-bit) */
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

/* Invariant: once allocated, p[len] == '\0', so b.p is always a valid C string. */
void buf_put(Buf *b, const void *data, size_t n) {
    /* data may point into this very buffer, which buf_reserve can move */
    uintptr_t d = (uintptr_t)data, lo = (uintptr_t)b->p;
    bool inside = b->p && d >= lo && d < lo + b->cap;
    size_t off = inside ? (size_t)(d - lo) : 0;
    buf_reserve(b, n);
    if (n) memmove(b->p + b->len, inside ? b->p + off : data, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void buf_puts(Buf *b, const char *s) {
    if (s) buf_put(b, s, strlen(s));
}

void buf_putc(Buf *b, char c) {
    buf_reserve(b, 1);
    b->p[b->len++] = c;
    b->p[b->len] = '\0';
}

void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    buf_reserve(b, 64);
    size_t room = b->cap - b->len;
    int n = vsnprintf(b->p + b->len, room, fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    if ((size_t)n >= room) {
        buf_reserve(b, (size_t)n);
        vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap2);
    }
    b->len += (size_t)n;
    va_end(ap2);
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

void buf_consume(Buf *b, size_t n) {
    if (n >= b->len) {
        b->len = 0;
    } else {
        memmove(b->p, b->p + n, b->len - n);
        b->len -= n;
    }
    if (b->p) b->p[b->len] = '\0';
}

void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

const char *buf_cstr(Buf *b) {
    buf_reserve(b, 0);
    b->p[b->len] = '\0';
    return b->p;
}

/* ----------------------------------------------------------------- strings */

size_t str_ncopy(char *dst, size_t cap, const char *src, size_t n) {
    if (!cap) return 0;
    if (!src) src = "";
    size_t len = strnlen(src, n);
    if (len > cap - 1) len = utf8_clip(src, cap - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
    return len;
}

size_t str_copy(char *dst, size_t cap, const char *src) {
    return str_ncopy(dst, cap, src, (size_t)-1);
}

bool str_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

bool str_ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
        a++, b++;
    }
    return *a == *b;
}

bool str_starts(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool str_istarts(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    for (; *prefix; s++, prefix++)
        if (!*s || lower((unsigned char)*s) != lower((unsigned char)*prefix)) return false;
    return true;
}

bool str_ends(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t a = strlen(s), b = strlen(suffix);
    return b <= a && memcmp(s + a - b, suffix, b) == 0;
}

bool str_iends(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t a = strlen(s), b = strlen(suffix);
    return b <= a && str_ieq(s + a - b, suffix);
}

bool str_blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

char *str_trim(char *s) {
    if (!s) return s;
    char *a = s;
    while (*a && isspace((unsigned char)*a)) a++;
    size_t n = strlen(a);
    while (n && isspace((unsigned char)a[n - 1])) n--;
    memmove(s, a, n);
    s[n] = '\0';
    return s;
}

bool str_int(const char *s, long long *out) {
    if (!s || !*s) return false;
    bool neg = false;
    if (*s == '-' || *s == '+') neg = (*s++ == '-');
    if (!*s) return false;
    unsigned long long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        unsigned d = (unsigned)(*s - '0');
        if (v > (9223372036854775807ULL - d) / 10) return false; /* overflow */
        v = v * 10 + d;
    }
    *out = neg ? -(long long)v : (long long)v;
    return true;
}

long long str_int_or(const char *s, long long dflt) {
    long long v;
    return str_int(s, &v) ? v : dflt;
}

/* ------------------------------------------------------------------- UTF-8 */

size_t utf8_clip(const char *s, size_t max) {
    size_t n = strnlen(s, max + 1);
    if (n <= max) return n;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

/* Returns consumed bytes; *ok tells whether the sequence was valid. */
static int decode(const unsigned char *s, size_t n, uint32_t *cp, bool *ok) {
    *ok = false;
    *cp = 0xFFFD;
    if (!n) return 0;
    unsigned c = s[0];
    if (c < 0x80) {
        *cp = c;
        *ok = true;
        return 1;
    }
    int len;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0) len = 2, v = c & 0x1F, min = 0x80;
    else if ((c & 0xF0) == 0xE0) len = 3, v = c & 0x0F, min = 0x800;
    else if ((c & 0xF8) == 0xF0) len = 4, v = c & 0x07, min = 0x10000;
    else return 1;
    if ((size_t)len > n) return 1;
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 1;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 1;
    *cp = v;
    *ok = true;
    return len;
}

int utf8_decode(const char *s, size_t n, uint32_t *cp) {
    bool ok;
    return decode((const unsigned char *)s, n, cp, &ok);
}

size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) return utf8_encode(0xFFFD, out);
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool utf8_valid(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n) {
        uint32_t cp;
        bool ok;
        int k = decode(p, n, &cp, &ok);
        if (!ok) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

size_t utf8_count(const char *s) {
    size_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* Length-preserving fold: ASCII A-Z and U+00C0..U+00DE (except U+00D7 ×)
 * are lowered; everything else is copied byte for byte. */
size_t fold_copy(char *dst, size_t cap, const char *src) {
    if (!cap) return 0;
    size_t n = utf8_clip(src, cap - 1), i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 'A' && c <= 'Z') {
            dst[i] = (char)(c + 32);
            i++;
        } else if (c == 0xC3 && i + 1 < n) {
            unsigned char d = (unsigned char)src[i + 1];
            dst[i] = (char)c;
            dst[i + 1] = (char)((d >= 0x80 && d <= 0x9E && d != 0x97) ? d + 0x20 : d);
            i += 2;
        } else {
            dst[i] = (char)c;
            i++;
        }
    }
    dst[n] = '\0';
    return n;
}

const char *fold_find(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    if (!*needle) return hay;
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn > hn) return NULL;
    char hs[512], ns[256];
    char *h = hn < sizeof hs ? hs : xmalloc(hn + 1);
    char *nd = nn < sizeof ns ? ns : xmalloc(nn + 1);
    fold_copy(h, hn + 1, hay);
    fold_copy(nd, nn + 1, needle);
    const char *hit = strstr(h, nd);
    const char *res = hit ? hay + (hit - h) : NULL; /* fold is length-preserving */
    if (h != hs) free(h);
    if (nd != ns) free(nd);
    return res;
}

/* -------------------------------------------------------------------- time */

int64_t g_fake_now = 0;

int64_t now_sec(void) {
    if (g_fake_now) return g_fake_now;
    return (int64_t)time(NULL);
}

int64_t now_ms(void) {
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

DateTime dt_local(int64_t t) {
    time_t tt = (time_t)t;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    DateTime d = {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec,         (tm.tm_wday + 6) % 7, tm.tm_yday};
    return d;
}

int64_t dt_make(int year, int mon, int day, int hour, int min) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

int64_t day_start(int64_t t) {
    DateTime d = dt_local(t);
    return dt_make(d.year, d.mon, d.day, 0, 0);
}

int64_t week_start(int64_t t) {
    DateTime d = dt_local(t);
    return dt_make(d.year, d.mon, d.day - d.wday, 0, 0);
}

int64_t add_days(int64_t t, int days) {
    DateTime d = dt_local(t);
    return dt_make(d.year, d.mon, d.day + days, d.hour, d.min) + d.sec;
}

int iso_week(int64_t t, int *iso_year) {
    DateTime d = dt_local(t);
    /* The ISO week belongs to the year that contains its Thursday. */
    DateTime th = dt_local(dt_make(d.year, d.mon, d.day - d.wday + 3, 12, 0));
    if (iso_year) *iso_year = th.year;
    return th.yday / 7 + 1;
}

int days_in_month(int year, int mon) {
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon < 1 || mon > 12) return 0;
    if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return dm[mon - 1];
}

const char *weekday_name(int wday, bool abbrev) {
    static const char *full[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const char *ab[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    if (wday < 0 || wday > 6) return "?";
    return abbrev ? ab[wday] : full[wday];
}

const char *month_name(int mon, bool abbrev) {
    static const char *full[12] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
    static const char *ab[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (mon < 1 || mon > 12) return "?";
    return abbrev ? ab[mon - 1] : full[mon - 1];
}

static int digits(const char *s, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 1;
}

bool parse_ymd(const char *s, int *y, int *m, int *d) {
    int yy, mm, dd;
    if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    if (!digits(s, 4, &yy) || !digits(s + 5, 2, &mm) || !digits(s + 8, 2, &dd)) return false;
    if (yy < 1970 || yy > 2200 || mm < 1 || mm > 12 || dd < 1 || dd > days_in_month(yy, mm)) return false;
    *y = yy, *m = mm, *d = dd;
    return true;
}

bool parse_hm(const char *s, int *minutes) {
    if (!s) return false;
    size_t n = strlen(s);
    int h = 0, m = 0;
    const char *sep = strpbrk(s, ":.");
    if (sep) {
        size_t hl = (size_t)(sep - s), ml = n - hl - 1;
        if (hl < 1 || hl > 2 || ml != 2) return false;
        if (!digits(s, (int)hl, &h) || !digits(sep + 1, 2, &m)) return false;
    } else if (n == 1 || n == 2) {
        if (!digits(s, (int)n, &h)) return false;
    } else if (n == 4) {
        if (!digits(s, 2, &h) || !digits(s + 2, 2, &m)) return false;
    } else {
        return false;
    }
    if (h > 24 || m > 59 || (h == 24 && m != 0)) return false;
    *minutes = h * 60 + m;
    return true;
}

void fmt_ymd(int64_t t, char out[11]) {
    DateTime d = dt_local(t);
    snprintf(out, 11, "%04d-%02d-%02d", d.year % 10000, d.mon % 100, d.day % 100);
}

void fmt_hm(int64_t t, char out[6]) {
    DateTime d = dt_local(t);
    snprintf(out, 6, "%02d:%02d", d.hour % 100, d.min % 100);
}

/* ----------------------------------------------------------------- hashing */

uint64_t hash64(const void *p, size_t n) {
    const unsigned char *s = p;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t hash64_str(const char *s) { return hash64(s, s ? strlen(s) : 0); }

void hex_encode(const void *p, size_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    const unsigned char *s = p;
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[s[i] >> 4];
        out[2 * i + 1] = hx[s[i] & 15];
    }
    out[2 * n] = '\0';
}

/* ----------------------------------------------------------------- logging */

LogLevel g_log_level = LOG_INFO;

/* Log lines carry what visitors typed (usernames, host names): a "\n" in there must not
   start a fake line of its own, nor an escape sequence repaint the terminal. Control
   characters become visible escapes (\n \r \t \x1b; C1 controls as \u0085). */
size_t log_clean(const char *in, char *out, size_t cap) {
    static const char hx[] = "0123456789abcdef";
    size_t n = 0, len = strlen(in);
    while (len && (in[len - 1] == '\n' || in[len - 1] == '\r')) len--; /* a trailing newline is just noise */
    if (!cap) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)in[i];
        char esc[8];
        size_t k = 0;
        if (ch == '\n') k = (size_t)snprintf(esc, sizeof esc, "\\n");
        else if (ch == '\r') k = (size_t)snprintf(esc, sizeof esc, "\\r");
        else if (ch == '\t') k = (size_t)snprintf(esc, sizeof esc, "\\t");
        else if (ch < 0x20 || ch == 0x7F) k = (size_t)snprintf(esc, sizeof esc, "\\x%c%c", hx[ch >> 4], hx[ch & 15]);
        else if (ch == 0xC2 && i + 1 < len && (unsigned char)in[i + 1] >= 0x80 && (unsigned char)in[i + 1] <= 0x9F) {
            unsigned char c1 = (unsigned char)in[++i]; /* U+0080..U+009F: 8-bit escapes for some terminals */
            k = (size_t)snprintf(esc, sizeof esc, "\\u00%c%c", hx[c1 >> 4], hx[c1 & 15]);
        } else {
            esc[0] = (char)ch;
            k = 1;
        }
        if (n + k >= cap) break;
        memcpy(out + n, esc, k);
        n += k;
    }
    out[n] = '\0';
    return n;
}

bool g_log_stderr = true;
static LogSink g_log_sink;

void log_set_sink(LogSink fn) { g_log_sink = fn; }

void log_at(LogLevel level, const char *fmt, ...) {
    if (level < g_log_level) return;
    static const char tag[] = "DIWE";
    int64_t ts = (int64_t)time(NULL);
    char line[1024], clean[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_clean(line, clean, sizeof clean);
    if (g_log_stderr) {
        DateTime d = dt_local(ts);
        fprintf(stderr, "%02d:%02d:%02d %c %s\n", d.hour, d.min, d.sec, tag[level < 4 ? level : 3], clean);
    }
    LogSink sink = g_log_sink;
    if (sink) sink(level > LOG_ERROR ? LOG_ERROR : level, ts, clean);
}
