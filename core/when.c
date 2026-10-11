/* core/when.c — see when.h
 *
 * A small token-scanning parser. Each recognizer looks at token i (and a few
 * tokens ahead), returns how many tokens it matched, and never consumes words
 * it does not understand. The main loop commits matches left to right; filler
 * words ("at", "kl", "på", "for" ...) are only consumed when they sit directly
 * in front of something that was recognised.
 *
 * Calendar arithmetic is done on civil dates (days since 1970-01-01), so it is
 * independent of the machine's time zone and DST; only when_resolve() converts
 * to Unix time via dt_make(). */
#include "when.h"

/* ------------------------------------------------------------- word lists */

static const char *const NEXT_W[] = {"next", "nästa", "nasta", "kommande", NULL};
static const char *const THIS_W[] = {"this", "denna", "coming", NULL};
static const char *const WEEK_W[] = {"week", "vecka", "veckan", NULL};
static const char *const WEEKEND_W[] = {"weekend", "helg", "helgen", NULL};
static const char *const TODAY_W[] = {"today", "idag", "tonight", "ikväll", "ikvall", "inatt", NULL};
static const char *const TOMORROW_W[] = {"tomorrow", "tmrw",    "tmr",   "tomorow", "tommorow", "tommorrow",
                                          "imorgon",  "imorn",   "imorrn", "imorron", NULL};
static const char *const DAT_W[] = {"övermorgon", "overmorgon", NULL};
static const char *const I_TODAY[] = {"dag", "kväll", "kvall", "natt", NULL};
static const char *const I_TOMORROW[] = {"morgon", "morn", "morrn", "morron", NULL};
static const char *const DAY_SEP[] = {"-", "to", "till", "until", "untill", "tills", "through", "thru", NULL};
static const char *const TIME_SEP[] = {"-", "to", "till", "until", "untill", "tills", NULL};
static const char *const UNTIL_W[] = {"until", "untill", "till", "tills", "through", "thru", NULL};
static const char *const AND_W[] = {"and", "och", NULL};
static const char *const BETWEEN_W[] = {"between", "mellan", NULL};
static const char *const A_W[] = {"a", "an", "en", "ett", NULL};
static const char *const HALF_W[] = {"half", "halv", NULL};
static const char *const HALF_HOUR_W[] = {"halvtimme", "halvtimma", "halvtimmen", NULL};
static const char *const EVERY_W[] = {"every", "each", "varje", NULL};
static const char *const OTHER_W[] = {"other", "second", "2nd", NULL};
static const char *const NOW_W[] = {"now", "nu", "asap", "direkt", NULL};
static const char *const IN_W[] = {"in", "om", NULL};
static const char *const DAY_PREFIX[] = {"on", "på", "from", "från", "by", "due", "senast", "between", "mellan", NULL};
static const char *const TIME_PREFIX[] = {"at", "kl",   "klockan", "from", "från", "between", "mellan",
                                           "by", "vid", "around",  "ca",   "cirka", NULL};
static const char *const DUR_PREFIX[] = {"for", "för", "i", "during", "under", NULL};
static const char *const REPEAT_PREFIX[] = {"on", "på", NULL};

typedef struct {
    const char *w;
    int min;
} Unit;

/* Single letters m/d are only accepted attached to a number ("90m", "3d"). */
static const Unit UNITS[] = {
    {"minutes", 1},  {"minute", 1},  {"minuter", 1}, {"minuten", 1}, {"minut", 1},    {"mins", 1},
    {"min", 1},      {"m", 1},       {"hours", 60},  {"hour", 60},   {"hrs", 60},     {"hr", 60},
    {"h", 60},       {"timmar", 60}, {"timmen", 60}, {"timme", 60},  {"timma", 60},   {"tim", 60},
    {"t", 60},       {"days", 1440}, {"day", 1440},  {"dagar", 1440}, {"dag", 1440},  {"dygn", 1440},
    {"d", 1440},     {"nights", 1440}, {"night", 1440}, {"nätter", 1440}, {"natter", 1440}, {"natt", 1440},
    {"weeks", 10080}, {"week", 10080}, {"veckor", 10080}, {"vecka", 10080}, {NULL, 0},
};

/* ---------------------------------------------------------------- helpers */

typedef struct {
    int y, m, d;
} Ymd;

typedef struct {
    Tokens *t;
    int64_t now;
    Ymd today;
    int today_wday; /* 0 = Monday */
} P;

/* Token i if it exists and is still free, else "" (which matches nothing). */
static const char *W(const Tokens *t, int i) {
    if (i < 0 || i >= t->n || t->used[i]) return "";
    return t->low[i];
}

static bool any_of(const char *w, const char *const *set) {
    if (!w || !*w) return false;
    for (; *set; set++)
        if (strcmp(w, *set) == 0) return true;
    return false;
}

static bool eq(const char *a, const char *b) { return *a && strcmp(a, b) == 0; }

static void use(Tokens *t, int i, int k) {
    for (int j = i; j < i + k && j < t->n; j++) t->used[j] = true;
}

/* Consume up to `max` filler words directly in front of token i. */
static void use_prefix(Tokens *t, int i, const char *const *fillers, int max) {
    for (int j = i - 1; j >= 0 && j >= i - max; j--) {
        if (t->used[j] || !any_of(t->low[j], fillers)) break;
        t->used[j] = true;
    }
}

/* Civil date <-> day number (days since 1970-01-01), H. Hinnant's algorithm. */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static Ymd civil_from_days(long z) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    Ymd r = {(int)(y + (m <= 2)), (int)m, (int)d};
    return r;
}

static long ymd_days(Ymd a) { return days_from_civil(a.y, a.m, a.d); }
static Ymd ymd_add(Ymd a, int n) { return civil_from_days(ymd_days(a) + n); }
static int ymd_wday(Ymd a) { return (int)(((ymd_days(a) % 7) + 7 + 3) % 7); } /* 1970-01-01 was a Thursday */
static bool ymd_valid(Ymd a) {
    return a.y >= 1970 && a.y <= 2200 && a.m >= 1 && a.m <= 12 && a.d >= 1 && a.d <= days_in_month(a.y, a.m);
}
static Ymd next_wday(Ymd from, int wd) { return ymd_add(from, (wd - ymd_wday(from) + 7) % 7); }
static Ymd monday_next_week(const P *p) { return ymd_add(p->today, 7 - p->today_wday); }

/* Year for a day/month without one: this year unless that is more than a day ago. */
static Ymd infer_year(const P *p, int m, int d) {
    Ymd r = {p->today.y, m, d};
    if (!ymd_valid(r) || ymd_days(r) < ymd_days(p->today) - 1) r.y++;
    return r;
}

/* "den 12": this month, or the next month that has that day if it already passed. */
static bool month_day(const P *p, int d, Ymd *out) {
    Ymd r = {p->today.y, p->today.m, d};
    for (int tries = 0; tries < 3; tries++) {
        if (ymd_valid(r) && (tries > 0 || d >= p->today.d)) {
            *out = r;
            return true;
        }
        if (++r.m > 12) r.m = 1, r.y++;
    }
    return false;
}

static void weekend(const P *p, Ymd *start, Ymd *end) {
    if (p->today_wday == 6) { /* Sunday: what's left of this weekend is today */
        *start = *end = p->today;
        return;
    }
    *start = next_wday(p->today, 5);
    *end = ymd_add(*start, 1);
}

/* Digits only, 1..maxd of them. */
static bool num(const char *s, int maxd, int *v) {
    int x = 0, n = 0;
    for (; s[n]; n++) {
        if (s[n] < '0' || s[n] > '9' || n >= maxd) return false;
        x = x * 10 + (s[n] - '0');
    }
    if (!n) return false;
    *v = x;
    return true;
}

/* "1st" "2nd" "3rd" "12th" "12:e" "1:a" */
static bool ordinal(const char *s, int *v) {
    int x = 0, n = 0;
    while (s[n] >= '0' && s[n] <= '9' && n < 2) x = x * 10 + (s[n++] - '0');
    if (!n) return false;
    const char *suf = s + n;
    if (strcmp(suf, "st") && strcmp(suf, "nd") && strcmp(suf, "rd") && strcmp(suf, "th") && strcmp(suf, ":a") &&
        strcmp(suf, ":e"))
        return false;
    if (x < 1 || x > 31) return false;
    *v = x;
    return true;
}

static bool day_num(const char *s, int *d) {
    int x;
    if (num(s, 2, &x) && x >= 1 && x <= 31) {
        *d = x;
        return true;
    }
    return ordinal(s, d);
}

static bool year_num(const char *s, int *y) {
    int x;
    if (strlen(s) == 4 && num(s, 4, &x) && x >= 1970 && x <= 2200) {
        *y = x;
        return true;
    }
    return false;
}

static int month_of(const char *w) {
    static const char *const M[12][5] = {
        {"january", "jan", "januari", NULL},   {"february", "feb", "februari", NULL}, {"march", "mar", "mars", NULL},
        {"april", "apr", NULL},                {"may", "maj", NULL},                  {"june", "jun", "juni", NULL},
        {"july", "jul", "juli", NULL},         {"august", "aug", "augusti", NULL},    {"september", "sep", "sept", NULL},
        {"october", "oct", "okt", "oktober"}, {"november", "nov", NULL},             {"december", "dec", NULL},
    };
    if (!*w) return 0;
    for (int i = 0; i < 12; i++)
        for (int j = 0; j < 5 && M[i][j]; j++)
            if (strcmp(w, M[i][j]) == 0) return i + 1;
    return 0;
}

static int weekday_of(const char *w) {
    static const char *const D[7][8] = {
        {"monday", "mon", "måndag", "mån", "mandag", NULL},
        {"tuesday", "tue", "tues", "tisdag", "tis", NULL},
        {"wednesday", "wed", "onsdag", "ons", NULL},
        {"thursday", "thu", "thur", "thurs", "torsdag", "tors", "tor", NULL},
        {"friday", "fri", "fredag", "fre", NULL},
        {"saturday", "sat", "lördag", "lör", "lordag", NULL},
        {"sunday", "sun", "söndag", "sön", "sondag", NULL},
    };
    if (!*w) return -1;
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 8 && D[i][j]; j++)
            if (strcmp(w, D[i][j]) == 0) return i;
    return -1;
}

static int weekday_plural_of(const char *w) {
    static const char *const D[7][4] = {
        {"mondays", "måndagar", "mandagar", NULL}, {"tuesdays", "tisdagar", NULL},
        {"wednesdays", "onsdagar", NULL},          {"thursdays", "torsdagar", NULL},
        {"fridays", "fredagar", NULL},             {"saturdays", "lördagar", "lordagar"},
        {"sundays", "söndagar", "sondagar", NULL},
    };
    if (!*w) return -1;
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 4 && D[i][j]; j++)
            if (strcmp(w, D[i][j]) == 0) return i;
    return -1;
}

/* Split "a-b" at its only dash; both sides non-empty. */
static bool split_dash(const char *w, char *l, char *r) {
    const char *dash = strchr(w, '-');
    if (!dash || dash == w || !dash[1] || strchr(dash + 1, '-')) return false;
    size_t n = (size_t)(dash - w);
    if (n >= TOK_LEN) return false;
    memcpy(l, w, n);
    l[n] = '\0';
    str_copy(r, TOK_LEN, dash + 1);
    return true;
}

/* ------------------------------------------------------------- tokenising */

static size_t space_len(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return 1;
    if (c == 0xC2 && (unsigned char)p[1] == 0xA0) return 2; /* no-break space */
    return 0;
}

static void normalize_dashes(char *s) {
    char *r = s, *w = s;
    while (*r) {
        unsigned char a = (unsigned char)r[0], b = (unsigned char)r[1], c = b ? (unsigned char)r[2] : 0;
        if (a == 0xE2 && b == 0x80 && (c == 0x93 || c == 0x94)) { /* en/em dash */
            *w++ = '-';
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

void tok_split(Tokens *t, const char *text) {
    memset(t, 0, sizeof *t);
    if (!text) return;
    t->src = text;
    const char *p = text;
    while (*p && t->n < TOK_MAX) {
        size_t sl;
        while ((sl = space_len(p))) p += sl;
        if (!*p) break;
        const char *s = p;
        while (*p && !space_len(p)) p++;
        size_t len = (size_t)(p - s);
        while (len && strchr(",.!?;", s[len - 1])) len--;
        if (!len) continue;
        t->at[t->n] = (uint32_t)(s - text);
        t->len[t->n] = (uint32_t)len;
        str_ncopy(t->raw[t->n], TOK_LEN, s, len);
        fold_copy(t->low[t->n], TOK_LEN, t->raw[t->n]);
        normalize_dashes(t->low[t->n]);
        t->n++;
    }
    t->tail = (uint32_t)(p - text);
}

void tok_rest(const Tokens *t, char *out, size_t cap) {
    if (!cap) return;
    Buf b = {0};
    for (int i = 0; i < t->n; i++) {
        if (t->used[i]) continue;
        if (b.len) buf_putc(&b, ' ');
        if (t->src) buf_put(&b, t->src + t->at[i], t->len[i]); /* the whole word, not the cut token */
        else buf_puts(&b, t->raw[i]);
    }
    if (t->src) { /* words past TOK_MAX: kept as typed */
        const char *rest = t->src + t->tail;
        size_t sl;
        while ((sl = space_len(rest))) rest += sl;
        if (*rest) {
            if (b.len) buf_putc(&b, ' ');
            buf_puts(&b, rest);
        }
    }
    size_t n = str_ncopy(out, cap, b.p ? b.p : "", b.len); /* cuts on a UTF-8 boundary */
    while (n && out[n - 1] == ' ') out[--n] = '\0';
    buf_free(&b);
}

/* -------------------------------------------------------------- durations */

static bool set_minutes(double total, int *minutes) {
    if (total < 1 || total > 525600) return false; /* 1 minute .. 1 year */
    *minutes = (int)(total + 0.5);
    return true;
}

/* digits with an optional [.,]digits fraction; advances *pp */
static bool read_num(const char **pp, double *out) {
    const char *p = *pp;
    double v = 0;
    int nd = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        if (++nd > 6) return false;
    }
    if (!nd) return false;
    if ((*p == '.' || *p == ',') && p[1] >= '0' && p[1] <= '9') {
        p++;
        double scale = 0.1;
        for (int fd = 0; *p >= '0' && *p <= '9'; fd++, p++) {
            if (fd < 6) v += (*p - '0') * scale;
            scale /= 10;
        }
    }
    *out = v;
    *pp = p;
    return true;
}

static bool whole_num(const char *s, double *v) {
    const char *p = s;
    return read_num(&p, v) && *p == '\0';
}

static bool number_word(const char *w, double *v) {
    static const struct {
        const char *w;
        int v;
    } N[] = {{"a", 1},     {"an", 1},    {"one", 1},   {"en", 1},   {"ett", 1},  {"two", 2},    {"två", 2},
             {"tva", 2},   {"three", 3}, {"tre", 3},   {"four", 4}, {"fyra", 4}, {"five", 5},   {"fem", 5},
             {"six", 6},   {"sex", 6},   {"seven", 7}, {"sju", 7},  {"eight", 8}, {"åtta", 8},  {"atta", 8},
             {"nine", 9},  {"nio", 9},   {"ten", 10},  {"tio", 10}, {NULL, 0}};
    if (!*w) return false;
    for (int i = 0; N[i].w; i++)
        if (strcmp(w, N[i].w) == 0) {
            *v = N[i].v;
            return true;
        }
    return false;
}

static int unit_word(const char *w) {
    if (!*w) return 0;
    if (w[1] == '\0' && w[0] != 'h' && w[0] != 't') return 0; /* "m", "d" alone are too ambiguous */
    for (const Unit *u = UNITS; u->w; u++)
        if (strcmp(w, u->w) == 0) return u->min;
    return 0;
}

static int unit_prefix(const char **pp) {
    const Unit *best = NULL;
    size_t bl = 0;
    for (const Unit *u = UNITS; u->w; u++) {
        size_t l = strlen(u->w);
        if (l > bl && strncmp(*pp, u->w, l) == 0) best = u, bl = l;
    }
    if (!best) return 0;
    *pp += bl;
    return best->min;
}

/* "2h" "90min" "1.5h" "1,5h" "3d" "1h30" "1h30m" */
static bool compact_duration(const char *w, int *minutes) {
    const char *p = w;
    double v;
    if (!read_num(&p, &v) || !*p) return false;
    int u = unit_prefix(&p);
    if (!u) return false;
    double total = v * u;
    if (*p) {
        double v2;
        if (u != 60 || !read_num(&p, &v2)) return false;
        if (*p && (unit_prefix(&p) != 1 || *p)) return false;
        total += v2;
    }
    return set_minutes(total, minutes);
}

/* "quarter of an hour", "quarter hour" starting at token i */
static int quarter_at(const Tokens *t, int i) {
    if (!eq(W(t, i), "quarter")) return 0;
    if (eq(W(t, i + 1), "of") && any_of(W(t, i + 2), A_W) && unit_word(W(t, i + 3)) == 60) return 4;
    if (unit_word(W(t, i + 1)) == 60) return 2;
    return 0;
}

static int match_duration(const Tokens *t, int i, int *minutes) {
    const char *w = W(t, i);
    double v;
    int u, k;
    if (!*w) return 0;
    if (compact_duration(w, minutes)) return 1;
    if (any_of(w, HALF_HOUR_W)) return set_minutes(30, minutes) ? 1 : 0;
    if (eq(w, "kvart")) return set_minutes(15, minutes) ? 1 : 0;
    if ((k = quarter_at(t, i))) return set_minutes(15, minutes) ? k : 0;
    if (any_of(w, HALF_W)) { /* "half an hour", "half hour" */
        if (any_of(W(t, i + 1), A_W) && unit_word(W(t, i + 2)) == 60) return set_minutes(30, minutes) ? 3 : 0;
        if (unit_word(W(t, i + 1)) == 60) return set_minutes(30, minutes) ? 2 : 0;
        return 0;
    }
    if (any_of(w, A_W)) { /* "en halvtimme", "en kvart", "a half hour", "a quarter of an hour" */
        const char *x = W(t, i + 1);
        if (any_of(x, HALF_HOUR_W)) return set_minutes(30, minutes) ? 2 : 0;
        if (eq(x, "kvart")) return set_minutes(15, minutes) ? 2 : 0;
        if (any_of(x, HALF_W) && unit_word(W(t, i + 2)) == 60) return set_minutes(30, minutes) ? 3 : 0;
        if ((k = quarter_at(t, i + 1))) return set_minutes(15, minutes) ? k + 1 : 0;
    }
    if (whole_num(w, &v) || number_word(w, &v)) {
        if ((u = unit_word(W(t, i + 1)))) { /* "2 hours" [and a half] */
            k = 2;
            if (any_of(W(t, i + 2), AND_W) && any_of(W(t, i + 3), A_W) && any_of(W(t, i + 4), HALF_W)) {
                v += 0.5;
                k = 5;
            }
            return set_minutes(v * u, minutes) ? k : 0;
        }
        /* "one and a half hours", "en och en halv timme" */
        if (any_of(W(t, i + 1), AND_W) && any_of(W(t, i + 2), A_W) && any_of(W(t, i + 3), HALF_W) &&
            (u = unit_word(W(t, i + 4))))
            return set_minutes((v + 0.5) * u, minutes) ? 5 : 0;
    }
    return 0;
}

static int match_relative(const Tokens *t, int i, int *rel) {
    const char *w = W(t, i);
    if (any_of(w, NOW_W)) {
        *rel = 0;
        return 1;
    }
    if (eq(w, "right") && eq(W(t, i + 1), "now")) {
        *rel = 0;
        return 2;
    }
    if (any_of(w, IN_W)) {
        int m, k = match_duration(t, i + 1, &m);
        if (k) {
            *rel = m;
            return 1 + k;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- repeat */

static int match_repeat(const Tokens *t, int i, int *repeat, int *wday) {
    const char *w = W(t, i);
    int wd;
    double v;
    *wday = -1;
    if (!*w) return 0;
    if (any_of(w, EVERY_W)) {
        const char *x = W(t, i + 1);
        if (eq(x, "day") || eq(x, "dag")) return *repeat = REPEAT_DAILY, 2;
        if (eq(x, "week") || eq(x, "vecka")) return *repeat = REPEAT_WEEKLY, 2;
        if (eq(x, "month") || eq(x, "månad") || eq(x, "manad")) return *repeat = REPEAT_MONTHLY, 2;
        if ((wd = weekday_of(x)) >= 0) return *repeat = REPEAT_WEEKLY, *wday = wd, 2;
        if (any_of(x, OTHER_W)) {
            const char *y = W(t, i + 2);
            if (eq(y, "week")) return *repeat = REPEAT_BIWEEKLY, 3;
            if ((wd = weekday_of(y)) >= 0) return *repeat = REPEAT_BIWEEKLY, *wday = wd, 3;
            return 0;
        }
        if ((whole_num(x, &v) || number_word(x, &v)) && v == 2 && (eq(W(t, i + 2), "weeks") || eq(W(t, i + 2), "veckor")))
            return *repeat = REPEAT_BIWEEKLY, 3;
        return 0;
    }
    if (eq(w, "varannan")) {
        const char *x = W(t, i + 1);
        if (eq(x, "vecka")) return *repeat = REPEAT_BIWEEKLY, 2;
        if ((wd = weekday_of(x)) >= 0) return *repeat = REPEAT_BIWEEKLY, *wday = wd, 2;
        return 0;
    }
    if (eq(w, "daily") || eq(w, "dagligen") || eq(w, "everyday")) return *repeat = REPEAT_DAILY, 1;
    if (eq(w, "weekly") || eq(w, "veckovis")) return *repeat = REPEAT_WEEKLY, 1;
    if (eq(w, "biweekly") || eq(w, "fortnightly")) return *repeat = REPEAT_BIWEEKLY, 1;
    if (eq(w, "monthly") || eq(w, "månadsvis") || eq(w, "manadsvis") || eq(w, "månatligen"))
        return *repeat = REPEAT_MONTHLY, 1;
    if ((wd = weekday_plural_of(w)) >= 0) return *repeat = REPEAT_WEEKLY, *wday = wd, 1;
    return 0;
}

/* ------------------------------------------------------------------- days */

static bool parse_slash(const P *p, const char *s, Ymd *out) {
    int d = 0, m = 0, y = -1, n;
    const char *q = s;
    for (n = 0; *q >= '0' && *q <= '9' && n < 3; q++, n++) d = d * 10 + (*q - '0');
    if (n < 1 || n > 2 || *q++ != '/') return false;
    for (n = 0; *q >= '0' && *q <= '9' && n < 3; q++, n++) m = m * 10 + (*q - '0');
    if (n < 1 || n > 2) return false;
    if (*q == '/') {
        q++;
        y = 0;
        for (n = 0; *q >= '0' && *q <= '9' && n < 5; q++, n++) y = y * 10 + (*q - '0');
        if (n == 2) y += 2000;
        else if (n != 4) return false;
    }
    if (*q) return false;
    Ymd r = {y, m, d};
    if (y < 0) {
        if (m < 1 || m > 12 || d < 1 || d > 31) return false;
        r = infer_year(p, m, d);
    }
    if (!ymd_valid(r)) return false;
    *out = r;
    return true;
}

/* An optional 4-digit year at token j: returns 1 and sets *y if present. */
static int opt_year(const Tokens *t, int j, int *y) { return year_num(W(t, j), y) ? 1 : 0; }

static bool make_date(const P *p, int y, int m, int d, Ymd *out) {
    Ymd r = y > 0 ? (Ymd){y, m, d} : infer_year(p, m, d);
    if (!ymd_valid(r)) return false;
    *out = r;
    return true;
}

/* One day expression starting at token i. Weekdays count from `ref`
 * (today, or the start of a range); inline ranges ("fri-sun") set *has_end. */
static int match_day_single(const P *p, int i, Ymd ref, Ymd *out, Ymd *end, bool *has_end) {
    const Tokens *t = p->t;
    const char *w = W(t, i);
    *has_end = false;
    if (!*w) return 0;
    int wd, d, d2, m, y = 0, k;
    char l[TOK_LEN], r[TOK_LEN];
    Ymd a, b;

    if (any_of(w, NEXT_W)) {
        const char *x = W(t, i + 1);
        if ((wd = weekday_of(x)) >= 0) return *out = ymd_add(monday_next_week(p), wd), 2;
        if (any_of(x, WEEK_W)) return *out = monday_next_week(p), 2;
        if (any_of(x, WEEKEND_W)) {
            *out = ymd_add(monday_next_week(p), 5);
            *end = ymd_add(*out, 1);
            *has_end = true;
            return 2;
        }
        return 0;
    }
    if (any_of(w, THIS_W)) {
        const char *x = W(t, i + 1);
        if ((wd = weekday_of(x)) >= 0) return *out = next_wday(p->today, wd), 2;
        if (any_of(x, WEEKEND_W)) return weekend(p, out, end), *has_end = true, 2;
        return 0;
    }
    if (eq(w, "i")) { /* Swedish: i dag, i morgon, i kväll, i övermorgon, i helgen */
        const char *x = W(t, i + 1);
        if (any_of(x, I_TODAY)) return *out = p->today, 2;
        if (any_of(x, I_TOMORROW)) return *out = ymd_add(p->today, 1), 2;
        if (any_of(x, DAT_W)) return *out = ymd_add(p->today, 2), 2;
        if (eq(x, "helgen") || eq(x, "helg")) return weekend(p, out, end), *has_end = true, 2;
        return 0;
    }
    if (eq(w, "the") && eq(W(t, i + 1), "day") && eq(W(t, i + 2), "after") && any_of(W(t, i + 3), TOMORROW_W))
        return *out = ymd_add(p->today, 2), 4;
    if (eq(w, "day") && eq(W(t, i + 1), "after") && any_of(W(t, i + 2), TOMORROW_W))
        return *out = ymd_add(p->today, 2), 3;
    if (any_of(w, TODAY_W)) return *out = p->today, 1;
    if (any_of(w, TOMORROW_W)) return *out = ymd_add(p->today, 1), 1;
    if (any_of(w, DAT_W)) return *out = ymd_add(p->today, 2), 1;
    if (eq(w, "weekend") || eq(w, "helgen")) return weekend(p, out, end), *has_end = true, 1;
    if ((wd = weekday_of(w)) >= 0) return *out = next_wday(ref, wd), 1;

    if (split_dash(w, l, r)) {
        int wa = weekday_of(l), wb = weekday_of(r);
        if (wa >= 0 && wb >= 0) { /* fri-sun */
            *out = next_wday(ref, wa);
            *end = next_wday(*out, wb);
            *has_end = true;
            return 1;
        }
        if (parse_slash(p, l, &a) && parse_slash(p, r, &b)) { /* 12/10-14/10 */
            if (ymd_days(b) < ymd_days(a)) b.y++;
            if (!ymd_valid(b)) return 0;
            *out = a, *end = b, *has_end = true;
            return 1;
        }
        if (day_num(l, &d) && parse_slash(p, r, &b)) { /* 12-14/10 */
            a = (Ymd){b.y, b.m, d};
            if (d > b.d && --a.m < 1) a.m = 12, a.y--;
            if (!ymd_valid(a)) return 0;
            *out = a, *end = b, *has_end = true;
            return 1;
        }
        if (day_num(l, &d) && day_num(r, &d2) && (m = month_of(W(t, i + 1)))) { /* 12-14 okt [2026] */
            k = 2 + opt_year(t, i + 2, &y);
            if (!make_date(p, y, m, d, &a)) return 0;
            b = (Ymd){a.y, m, d2};
            if (!ymd_valid(b) || ymd_days(b) < ymd_days(a)) return 0;
            *out = a, *end = b, *has_end = true;
            return k;
        }
    }
    if (parse_ymd(w, &a.y, &a.m, &a.d)) return *out = a, 1;
    if (parse_slash(p, w, &a)) return *out = a, 1;

    if (day_num(w, &d)) { /* 12 oct [2026], 12th of october */
        int j = i + 1;
        if (eq(W(t, j), "of")) j++;
        if ((m = month_of(W(t, j)))) {
            k = j + 1 - i;
            k += opt_year(t, j + 1, &y);
            return make_date(p, y, m, d, out) ? k : 0;
        }
    }
    if ((m = month_of(w)) && day_num(W(t, i + 1), &d)) { /* oct 12[th] [2026] */
        k = 2 + opt_year(t, i + 2, &y);
        return make_date(p, y, m, d, out) ? k : 0;
    }
    if ((eq(w, "den") || eq(w, "the")) && day_num(W(t, i + 1), &d)) { /* den 12 [oktober], the 12th [of oct] */
        int j = i + 2;
        if (eq(W(t, j), "of") && month_of(W(t, j + 1))) j++;
        if ((m = month_of(W(t, j)))) {
            k = j + 1 - i;
            k += opt_year(t, j + 1, &y);
            return make_date(p, y, m, d, out) ? k : 0;
        }
        return month_day(p, d, out) ? 2 : 0;
    }
    if (ordinal(w, &d)) return month_day(p, d, out) ? 1 : 0; /* bare "12th" / "12:e" */
    return 0;
}

/* A day, optionally followed by a separator and an end day. */
static int match_day(const P *p, int i, Ymd *out, Ymd *end, bool *has_end) {
    int k = match_day_single(p, i, p->today, out, end, has_end);
    if (!k || *has_end) return k;
    const Tokens *t = p->t;
    const char *sep = W(t, i + k);
    bool between = i > 0 && !t->used[i - 1] && any_of(t->low[i - 1], BETWEEN_W);
    if (any_of(sep, DAY_SEP) || (between && any_of(sep, AND_W))) {
        Ymd e, e2;
        bool he2;
        int k2 = match_day_single(p, i + k + 1, *out, &e, &e2, &he2);
        if (k2) {
            if (he2) e = e2;
            if (ymd_days(e) < ymd_days(*out)) e.y++;
            if (ymd_valid(e)) {
                *end = e;
                *has_end = true;
                return k + 1 + k2;
            }
        }
    }
    return k;
}

/* ------------------------------------------------------------------ times */

enum { AMPM_NONE, AMPM_AM, AMPM_PM };

typedef struct {
    int h, m;
    int ampm;
    bool has_min, lead0, word, bare;
} TimeTok;

static int ampm_word(const char *s) {
    if (!strcmp(s, "am") || !strcmp(s, "a.m") || !strcmp(s, "a.m.")) return AMPM_AM;
    if (!strcmp(s, "pm") || !strcmp(s, "p.m") || !strcmp(s, "p.m.")) return AMPM_PM;
    return AMPM_NONE;
}

static bool parse_time_str(const char *s, TimeTok *tt) {
    memset(tt, 0, sizeof *tt);
    if (!*s) return false;
    if (!strcmp(s, "noon") || !strcmp(s, "lunch") || !strcmp(s, "lunchtime") || !strcmp(s, "midday")) {
        tt->h = 12;
        tt->word = true;
        return true;
    }
    if (!strcmp(s, "midnight") || !strcmp(s, "midnatt")) {
        tt->word = true;
        return true;
    }
    const char *p = s;
    int h = 0, nd = 0;
    while (*p >= '0' && *p <= '9') {
        h = h * 10 + (*p++ - '0');
        if (++nd > 2) return false;
    }
    if (!nd) return false;
    tt->lead0 = nd == 2 && s[0] == '0';
    if ((*p == ':' || *p == '.') && p[1] >= '0' && p[1] <= '9') {
        if (!(p[2] >= '0' && p[2] <= '9')) return false;
        tt->m = (p[1] - '0') * 10 + (p[2] - '0');
        if (tt->m > 59) return false;
        tt->has_min = true;
        p += 3;
    }
    if (*p && !(tt->ampm = ampm_word(p))) return false;
    if (tt->ampm ? (h < 1 || h > 12) : (h > 24 || (h == 24 && tt->m))) return false;
    tt->h = h;
    tt->bare = !tt->has_min && !tt->ampm;
    return true;
}

static int to_24(int h, int ampm) {
    if (ampm == AMPM_AM) return h == 12 ? 0 : h;
    if (ampm == AMPM_PM) return h == 12 ? 12 : h + 12;
    return h;
}

/* Single time -> minutes. Bare 1..6 means afternoon: nobody books at 3 am. */
static int tt_minutes(const TimeTok *tt) {
    int h = to_24(tt->h, tt->ampm);
    if (tt->bare && !tt->lead0 && !tt->word && tt->h >= 1 && tt->h <= 6) h += 12;
    return h * 60 + tt->m;
}

static int fwd(int from, int to) { /* forward distance in minutes, 1..1440 */
    int d = (to - from) % 1440;
    return d <= 0 ? d + 1440 : d;
}

/* Pick am/pm for ambiguous ends so the range is as short as possible:
 * "2-5pm" = 14-17, "11-1pm" = 11-13, "6-8" = 18-20, "22-2" = 22-02. */
static void resolve_range(const TimeTok *a, const TimeTok *b, int *s, int *e) {
    if (!a->ampm && b->ampm && !a->word && !a->lead0 && a->h >= 1 && a->h <= 12) {
        int bm = tt_minutes(b);
        int am = to_24(a->h, AMPM_AM) * 60 + a->m, pm = to_24(a->h, AMPM_PM) * 60 + a->m;
        *s = fwd(am, bm) <= fwd(pm, bm) ? am : pm;
        *e = bm;
        return;
    }
    *s = tt_minutes(a);
    if (!b->ampm && !b->word && !b->lead0 && b->h >= 1 && b->h <= 11) {
        int c1 = b->h * 60 + b->m, c2 = c1 + 12 * 60;
        *e = fwd(*s, c1) <= fwd(*s, c2) ? c1 : c2;
    } else {
        *e = tt_minutes(b);
    }
}

static int match_time_single(const Tokens *t, int i, TimeTok *tt) {
    if (!parse_time_str(W(t, i), tt)) return 0;
    int ap = ampm_word(W(t, i + 1));
    if (ap && !tt->ampm && !tt->word && tt->h >= 1 && tt->h <= 12) { /* "2 pm" */
        tt->ampm = ap;
        tt->bare = false;
        return 2;
    }
    return 1;
}

static bool has_time_prefix(const Tokens *t, int i) {
    return i > 0 && !t->used[i - 1] && any_of(t->low[i - 1], TIME_PREFIX);
}

/* A time or a time range at token i. Bare single numbers ("14") are only
 * accepted with allow_bare; the caller decides on those in a second pass. */
static int match_time(const Tokens *t, int i, bool allow_bare, int *s, int *e, bool *has_e) {
    const char *w = W(t, i);
    TimeTok a, b;
    char l[TOK_LEN], r[TOK_LEN];
    *has_e = false;
    if (!*w) return 0;
    if (split_dash(w, l, r) && parse_time_str(l, &a) && parse_time_str(r, &b)) { /* 14-17, 2-5pm */
        int k = 1, ap = ampm_word(W(t, i + 1));
        if (ap && !b.ampm && !b.word && b.h >= 1 && b.h <= 12) {
            b.ampm = ap;
            b.bare = false;
            k = 2;
        }
        resolve_range(&a, &b, s, e);
        *has_e = true;
        return k;
    }
    int k = match_time_single(t, i, &a);
    if (!k) return 0;
    const char *sep = W(t, i + k);
    bool between = i > 0 && !t->used[i - 1] && any_of(t->low[i - 1], BETWEEN_W);
    if (any_of(sep, TIME_SEP) || (between && any_of(sep, AND_W))) {
        int k2 = match_time_single(t, i + k + 1, &b);
        if (k2) {
            resolve_range(&a, &b, s, e);
            *has_e = true;
            return k + 1 + k2;
        }
    }
    if (!allow_bare && a.bare && k == 1) return 0;
    *s = tt_minutes(&a);
    return k;
}

/* "until sunday", "till 17", "tills söndag" */
static int match_until(const P *p, int i, When *w) {
    const Tokens *t = p->t;
    if (!any_of(W(t, i), UNTIL_W)) return 0;
    Ymd ref = w->has_date ? (Ymd){w->y, w->m, w->d} : p->today, e, e2;
    bool he;
    int k = match_day_single(p, i + 1, ref, &e, &e2, &he);
    if (k) {
        if (he) e = e2;
        w->has_end_date = true;
        w->ey = e.y, w->em = e.m, w->ed = e.d;
        return 1 + k;
    }
    TimeTok tt;
    k = match_time_single(t, i + 1, &tt);
    if (k) {
        w->has_end = true;
        if (w->has_start && !tt.ampm && !tt.word && !tt.lead0 && tt.h >= 1 && tt.h <= 11) {
            int c1 = tt.h * 60 + tt.m, c2 = c1 + 12 * 60;
            w->end_min = fwd(w->start_min, c1) <= fwd(w->start_min, c2) ? c1 : c2;
        } else {
            w->end_min = tt_minutes(&tt);
        }
        return 1 + k;
    }
    return 0;
}

/* ------------------------------------------------------------------ parse */

static void set_date(When *w, Ymd d) {
    w->has_date = true;
    w->y = d.y, w->m = d.m, w->d = d.d;
}

bool when_parse(Tokens *t, int64_t now, When *w) {
    memset(w, 0, sizeof *w);
    w->repeat_wday = -1;
    if (!t) return false;
    DateTime dn = dt_local(now);
    P p = {t, now, {dn.year, dn.mon, dn.day}, dn.wday};
    bool found = false;
    int day_end = -1; /* token index just after the last day expression */
    int dur_end = -1; /* token index just after the last duration */

    for (int i = 0; i < t->n; i++) {
        if (t->used[i]) continue;
        int k, a, b, mins;
        bool he;
        Ymd y1, y2;
        if ((k = match_repeat(t, i, &a, &b))) {
            if (!w->repeat) w->repeat = a, w->repeat_wday = b;
            use_prefix(t, i, REPEAT_PREFIX, 1);
            use(t, i, k);
            if (b >= 0) day_end = i + k;
        } else if ((k = match_relative(t, i, &mins))) {
            if (!w->now) w->now = true, w->rel_min = mins;
            use(t, i, k);
        } else if ((k = match_duration(t, i, &mins))) {
            bool joined = i > 0 && dur_end == i - 1 && !t->used[i - 1] && any_of(t->low[i - 1], AND_W);
            if (dur_end == i || joined) {
                w->dur_min += mins; /* "1 hour 30 min", "2 timmar och 30 minuter" */
                if (joined) t->used[i - 1] = true;
            } else if (!w->dur_min) {
                w->dur_min = mins;
            }
            use_prefix(t, i, DUR_PREFIX, 1);
            use(t, i, k);
            dur_end = i + k;
        } else if (!w->has_date && (k = match_day(&p, i, &y1, &y2, &he))) {
            set_date(w, y1);
            if (he) w->has_end_date = true, w->ey = y2.y, w->em = y2.m, w->ed = y2.d;
            use_prefix(t, i, DAY_PREFIX, 1);
            use(t, i, k);
            day_end = i + k;
        } else if (!w->has_start && (k = match_time(t, i, has_time_prefix(t, i), &a, &b, &he))) {
            w->has_start = true;
            w->start_min = a;
            if (he) w->has_end = true, w->end_min = b;
            use_prefix(t, i, TIME_PREFIX, 2);
            use(t, i, k);
        } else if ((k = match_until(&p, i, w))) {
            use(t, i, k);
        } else {
            continue;
        }
        found = true;
        i += k - 1;
    }

    /* Second pass: a bare number becomes the start time — preferably the one
     * right after a day ("room 2 fri 14" -> 14), else the first one. */
    if (!w->has_start) {
        int pick = -1;
        TimeTok tt, best;
        for (int i = 0; i < t->n; i++) {
            if (t->used[i] || !parse_time_str(t->low[i], &tt) || !tt.bare) continue;
            if (pick < 0 || i == day_end) pick = i, best = tt;
            if (i == day_end) break;
        }
        if (pick >= 0) {
            w->has_start = true;
            w->start_min = tt_minutes(&best);
            t->used[pick] = true;
            found = true;
        }
    }
    if (w->repeat_wday >= 0 && !w->has_date) set_date(w, next_wday(p.today, w->repeat_wday));
    return found;
}

/* ---------------------------------------------------------------- resolve */

bool when_resolve(const When *w, int64_t now, int default_min, int64_t *start, int64_t *end) {
    if (!(w->has_date || w->has_end_date || w->has_start || w->has_end || w->dur_min || w->now || w->repeat))
        return false;
    DateTime n = dt_local(now);
    int64_t now_m = now - n.sec;
    int y = w->has_date ? w->y : n.year, m = w->has_date ? w->m : n.mon, d = w->has_date ? w->d : n.day;
    bool is_today = y == n.year && m == n.mon && d == n.day;
    bool allday = false;
    int64_t s, e;

    if (w->now) {
        s = now_m + (int64_t)w->rel_min * 60;
    } else if (w->has_start) {
        s = dt_make(y, m, d, w->start_min / 60, w->start_min % 60);
        if (!w->has_date && s < now - 15 * 60) s = add_days(s, 1);
    } else if ((w->has_end || w->dur_min) && !w->has_end_date && is_today) {
        s = now_m; /* "until 17", "2h": from now */
    } else {
        s = dt_make(y, m, d, 0, 0);
        allday = !w->has_end;
    }

    if (w->has_end_date) {
        e = w->has_end ? dt_make(w->ey, w->em, w->ed, w->end_min / 60, w->end_min % 60)
                       : dt_make(w->ey, w->em, w->ed + 1, 0, 0);
        if (e <= s) e = add_days(day_start(s), 1);
    } else if (w->has_end) {
        DateTime ds = dt_local(s);
        e = dt_make(ds.year, ds.mon, ds.day, w->end_min / 60, w->end_min % 60);
        if (e <= s) e = add_days(e, 1);
    } else if (w->dur_min) {
        if (w->dur_min % 1440 == 0 && s == day_start(s)) e = add_days(s, w->dur_min / 1440);
        else e = s + (int64_t)w->dur_min * 60;
    } else if (allday) {
        e = add_days(s, 1);
    } else {
        e = s + (int64_t)(default_min > 0 ? default_min : 60) * 60;
    }
    *start = s;
    *end = e;
    return true;
}

/* --------------------------------------------------------------- describe */

#define EN_DASH "\xE2\x80\x93"

static long day_index(DateTime d) { return days_from_civil(d.year, d.mon, d.day); }

static void day_label(DateTime d, DateTime n, char *out, size_t cap) {
    long diff = day_index(d) - day_index(n);
    if (diff == 0) snprintf(out, cap, "Today");
    else if (diff == 1) snprintf(out, cap, "Tomorrow");
    else if (diff == -1) snprintf(out, cap, "Yesterday");
    else if (d.year == n.year) snprintf(out, cap, "%s %d %s", weekday_name(d.wday, true), d.day, month_name(d.mon, true));
    else snprintf(out, cap, "%s %d %s %d", weekday_name(d.wday, true), d.day, month_name(d.mon, true), d.year);
}

void when_describe(int64_t start, int64_t end, int64_t now, char *out, size_t cap) {
    if (!cap) return;
    DateTime s = dt_local(start), e = dt_local(end), n = dt_local(now);
    char a[48], b[48], tmp[160];
    if (end > start && s.hour == 0 && s.min == 0 && e.hour == 0 && e.min == 0) { /* all-day */
        DateTime last = dt_local(add_days(end, -1));
        if (day_index(last) <= day_index(s)) {
            day_label(s, n, a, sizeof a);
            snprintf(tmp, sizeof tmp, "%s, all day", a);
        } else {
            if (s.year == last.year && s.mon == last.mon)
                snprintf(a, sizeof a, "%s %d", weekday_name(s.wday, true), s.day);
            else if (s.year == n.year || s.year == last.year)
                snprintf(a, sizeof a, "%s %d %s", weekday_name(s.wday, true), s.day, month_name(s.mon, true));
            else
                snprintf(a, sizeof a, "%s %d %s %d", weekday_name(s.wday, true), s.day, month_name(s.mon, true), s.year);
            if (last.year == n.year)
                snprintf(b, sizeof b, "%s %d %s", weekday_name(last.wday, true), last.day, month_name(last.mon, true));
            else
                snprintf(b, sizeof b, "%s %d %s %d", weekday_name(last.wday, true), last.day,
                         month_name(last.mon, true), last.year);
            snprintf(tmp, sizeof tmp, "%s " EN_DASH " %s", a, b);
        }
    } else {
        day_label(s, n, a, sizeof a);
        bool same = day_index(e) == day_index(s) ||
                    (e.hour == 0 && e.min == 0 && day_index(e) == day_index(s) + 1 && end > start);
        if (end <= start) {
            snprintf(tmp, sizeof tmp, "%s %02d:%02d", a, s.hour, s.min);
        } else if (same) {
            snprintf(tmp, sizeof tmp, "%s %02d:%02d" EN_DASH "%02d:%02d", a, s.hour, s.min, e.hour, e.min);
        } else {
            day_label(e, n, b, sizeof b);
            snprintf(tmp, sizeof tmp, "%s %02d:%02d " EN_DASH " %s %02d:%02d", a, s.hour, s.min, b, e.hour, e.min);
        }
    }
    str_copy(out, cap, tmp);
}
