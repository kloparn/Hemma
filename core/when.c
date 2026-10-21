/* core/when.c — see when.h
 *
 * A small token-scanning parser. Each recognizer looks at token i (and a few
 * tokens ahead), returns how many tokens it matched, and never consumes words
 * it does not understand. The main loop commits matches left to right; filler
 * words ("at", "kl", "på", "for", "a las" ...) are only consumed when they sit directly
 * in front of something that was recognised.
 *
 * Calendar arithmetic is done on civil dates (days since 1970-01-01), so it is
 * independent of the machine's time zone and DST; only when_resolve() converts
 * to Unix time, via wall_time() (which settles the clock changes itself rather
 * than leave them to each C library's mktime()).
 *
 * A text that names a day that can't be ("31 sep") or a day range that ends
 * before it starts is refused as a whole: reading the words after it on their
 * own would quietly book something else.
 *
 * Spanish (issue #46) goes through the same recognizers. Its little words are English and
 * Swedish words too - "a", "de", "el", "la", "en" (Swedish "a"/"one"), "dom" (Swedish "they"),
 * "ya", "mayo" - so they are only taken inside a Spanish expression: "a las 14", "5 de noviembre",
 * "el lunes", "en 30 minutos". Where even that can't tell ("el 12": the 12th, or the electricity
 * - Swedish "el" - at 12?), the Spanish reading is for people who use Hemma in Spanish (es_ui()).
 * English and Swedish texts read exactly as before. */
#include "when.h"
#include "i18n.h"

/* ------------------------------------------------------------- word lists */

static const char *const NEXT_W[] = {"next", "nästa", "nasta", "kommande", NULL};
static const char *const THIS_W[] = {"this", "denna", "coming", "este", "esta", NULL};
static const char *const WEEK_W[] = {"week", "vecka", "veckan", NULL};
static const char *const WEEKEND_W[] = {"weekend", "helg", "helgen", "finde", NULL};
static const char *const TODAY_W[] = {"today", "idag", "tonight", "ikväll", "ikvall", "inatt", "hoy", NULL};
/* Evening days: "tonight", "ikväll", "inatt", "i kväll", "i natt", and any day + an evening noun
 * ("tomorrow evening", "imorgon kväll", "fri night", "this evening", "esta noche", "viernes noche"). */
static const char *const EVENING_W[] = {"tonight", "ikväll", "ikvall", "inatt", NULL};
static const char *const EVENING_NOUN[] = {"evening", "night", "kväll", "kvall", "kvällen", "kvallen",
                                           "natt",    "natten", "noche", NULL};
static const char *const TOMORROW_W[] = {"tomorrow", "tmrw",   "tmr",    "tomorow", "tommorow", "tommorrow",
                                          "imorgon",  "imorn",  "imorrn", "imorron", "mañana",   "manana", NULL};
static const char *const DAT_W[] = {"övermorgon", "overmorgon", NULL};
static const char *const I_TODAY[] = {"dag", "kväll", "kvall", "natt", NULL};
static const char *const I_TOMORROW[] = {"morgon", "morn", "morrn", "morron", NULL};
static const char *const DAY_SEP[] = {"-", "to", "till", "until", "untill", "tills", "through", "thru", "hasta", NULL};
static const char *const TIME_SEP[] = {"-", "to", "till", "until", "untill", "tills", NULL};
static const char *const UNTIL_W[] = {"until", "untill", "till", "tills", "through", "thru", "hasta", NULL};
static const char *const AND_W[] = {"and", "och", "y", NULL};
static const char *const BETWEEN_W[] = {"between", "mellan", "entre", NULL};
static const char *const A_W[] = {"a", "an", "en", "ett", "un", "una", NULL};
static const char *const HALF_W[] = {"half", "halv", "media", NULL};
static const char *const HALF_HOUR_W[] = {"halvtimme", "halvtimma", "halvtimmen", NULL};
static const char *const EVERY_W[] = {"every", "each", "varje", "cada", NULL};
static const char *const OTHER_W[] = {"other", "second", "2nd", NULL};
static const char *const NOW_W[] = {"now", "nu", "asap", "direkt", "ahora", NULL};
static const char *const IN_W[] = {"in", "om", NULL};
static const char *const DAY_PREFIX[] = {"on",      "på",     "from",  "från",  "by",   "due", "senast",
                                         "between", "mellan", "desde", "entre", "para", NULL};
static const char *const TIME_PREFIX[] = {"at", "kl",   "klockan", "from", "från", "between", "mellan",
                                           "by", "vid", "around",  "ca",   "cirka", NULL};
static const char *const END_TIME_PREFIX[] = {"at", "kl", "klockan", "vid", NULL}; /* "until sunday at 17" */
static const char *const DUR_PREFIX[] = {"for", "för", "i", "during", "under", "durante", "por", "para", NULL};
static const char *const REPEAT_PREFIX[] = {"on", "på", NULL};

/* Spanish only */
static const char *const ES_NEXT[] = {"próximo", "proximo", "próxima", "proxima", NULL};
static const char *const ES_ART[] = {"las", "la", NULL}; /* "a las 14", "a la 1" */
static const char *const ES_HOUR_PREP[] = {"a", "de", "desde", "entre", "sobre", "hacia", "para", NULL};
static const char *const ES_MORNING[] = {"mañana", "manana", NULL}; /* "por la mañana": also tomorrow */
static const char *const ES_TIME_WORD[] = {"mediodía", "mediodia", "medianoche", NULL};
static const char *const ES_HOUR_UNIT[] = {"horas", "hs", "h", "hrs", NULL}; /* "a las 14 horas" = 14:00 */
static const char *const ES_DAYS[] = {"días", "dias", NULL};
static const char *const ES_ALL[] = {"todos", "todas", NULL};
static const char *const ES_THE_PL[] = {"los", "las", NULL};
/* Words no English or Swedish text has: a day or a duration with one in it is Spanish (es_words). */
static const char *const ES_WORDS[] = {
    "lunes",  "lun",    "martes",  "miércoles", "miercoles", "mié",     "mie",       "jueves",  "jue",    "viernes",
    "vie",    "sábado", "sabado",  "sáb",       "sab",       "domingo", "dom",       "hoy",     "mañana", "manana",
    "pasado", "de",     "día",     "dia",       "días",      "dias",    "próximo",   "proximo", "próxima", "proxima",
    "que",    "viene",  "fin",     "finde",     "semana",    "semanas", "noche",     "noches",  "tarde",  "minutos",
    "minuto", "horas",  "hora",    "un",        "una",       "uno",     "dos",       "tres",    "cuatro", "cinco",
    "seis",   "siete",  "ocho",    "nueve",     "diez",      "media",   "medio",     "cuarto",  NULL};

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
    {"weeks", 10080}, {"week", 10080}, {"veckor", 10080}, {"vecka", 10080},
    /* Spanish */
    {"minutos", 1},  {"minuto", 1},  {"horas", 60},  {"hora", 60},   {"días", 1440},  {"dias", 1440},
    {"día", 1440},   {"dia", 1440},  {"noches", 1440}, {"noche", 1440}, {"semanas", 10080}, {"semana", 10080},
    {NULL, 0},
};

/* ---------------------------------------------------------------- helpers */

typedef struct {
    int y, m, d;
} Ymd;

/* What a day expression says about the times typed on it (P.mark, tt_minutes): DAY_EVENING = its
 * evening and the night after ("tonight", "fri night", "esta noche"), DAY_AFTERNOON = its afternoon
 * ("esta tarde", "mañana por la tarde"), DAY_MORNING = its morning ("por la mañana"). */
enum { DAY_PLAIN = 1, DAY_EVENING = 2, DAY_AFTERNOON = 3, DAY_MORNING = 4 };

typedef struct {
    Tokens *t;
    int64_t now;
    Ymd today;
    int today_wday; /* 0 = Monday */
    int named;      /* part of a day named anywhere (0 = none), for a time typed before it ("8 tonight") */
    int8_t *mark;   /* per token: DAY_PLAIN / DAY_EVENING ... for the words of the day expressions */
    int *date_mark; /* the start date's: 0 (none yet), DAY_PLAIN, DAY_EVENING ... */
    When *w;        /* for refuse() */
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

/* The person reads Hemma in Spanish: words Spanish shares with Swedish ("el", "dom", "en") and
 * English ("ya") are read the Spanish way. */
static bool es_ui(void) { return i18n_lang() == LANG_ES; }

static bool is_manana(const char *w) { return any_of(w, ES_MORNING); }

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
/* Spanish "el próximo viernes", "el viernes que viene": the next one to come - never today. */
static Ymd es_coming(const P *p, int wd) { return ymd_add(p->today, (wd - p->today_wday + 6) % 7 + 1); }

/* Refuse the whole text: when_parse() returns false and w->refused says why (the first reason
 * wins), in the language in use. Recognizers return 0 after calling it. */
static int refuse(const P *p, const char *fmt, ...) FMT(2, 3);
static int refuse(const P *p, const char *fmt, ...) {
    if (p->w->refused[0]) return 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->w->refused, sizeof p->w->refused, fmt, ap);
    va_end(ap);
    return 0;
}

/* r looked like a date but isn't one. */
static int no_such_day(const P *p, Ymd r) {
    if (r.y < 1970 || r.y > 2200) return refuse(p, T("Pick a year from 1970 to 2200"));
    if (r.d > days_in_month(2000, r.m)) return refuse(p, T("There's no %d %s"), r.d, month_name(r.m, false));
    return refuse(p, T("There's no %d %s %d"), r.d, month_name(r.m, false), r.y); /* 29 Feb, not a leap year */
}

static int reversed(const P *p) { return refuse(p, T("The end date is before the start date")); }

/* Year for a day/month without one: this year unless that is more than a day ago. */
static Ymd infer_year(const P *p, int m, int d) {
    Ymd r = {p->today.y, m, d};
    if (!ymd_valid(r) || ymd_days(r) < ymd_days(p->today) - 1) r.y++;
    return r;
}

/* Day d of month m in year y (0 = not typed). A start's year is guessed from today
 * (infer_year); an end's (start set) is the start's year, or the next one when its month comes
 * earlier ("28/12-2/1" runs past New Year; "12/10-10/10" stays reversed for the caller to
 * refuse). A day that can't be refuses the text. */
static bool make_date(const P *p, int y, int m, int d, const Ymd *start, Ymd *out) {
    Ymd r = y > 0 ? (Ymd){y, m, d} : start ? (Ymd){start->y + (m < start->m), m, d} : infer_year(p, m, d);
    if (!ymd_valid(r)) return no_such_day(p, r);
    *out = r;
    return true;
}

/* "den 12": that day in from's month, or the next month that has it if it already passed. */
static bool month_day(Ymd from, int d, Ymd *out) {
    Ymd r = {from.y, from.m, d};
    for (int tries = 0; tries < 3; tries++) {
        if (ymd_valid(r) && (tries > 0 || d >= from.d)) {
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

/* Spanish "el próximo fin de semana", "el finde que viene": the next one that hasn't begun. */
static void es_next_weekend(const P *p, Ymd *start, Ymd *end) {
    *start = p->today_wday >= 5 ? ymd_add(monday_next_week(p), 5) : next_wday(p->today, 5);
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

/* Month names. MON_ANY: English and Swedish (with the short names Spanish shares: "nov", "oct").
 * MON_ES: Spanish only - read only day first ("5 julio", "5 de julio"; never "julio 5": Julio is a
 * name). MON_ES_DE: Spanish words that are English ones too ("mayo", "ago") - only with "de"
 * ("2 de mayo"), or for people who use Hemma in Spanish. */
enum { MON_ANY, MON_ES, MON_ES_DE };

static int month_kind(const char *w, int *m) {
    static const struct {
        const char *w;
        int m, kind;
    } M[] = {
        {"january", 1, MON_ANY},     {"jan", 1, MON_ANY},        {"januari", 1, MON_ANY},   {"february", 2, MON_ANY},
        {"feb", 2, MON_ANY},         {"februari", 2, MON_ANY},   {"march", 3, MON_ANY},     {"mar", 3, MON_ANY},
        {"mars", 3, MON_ANY},        {"april", 4, MON_ANY},      {"apr", 4, MON_ANY},       {"may", 5, MON_ANY},
        {"maj", 5, MON_ANY},         {"june", 6, MON_ANY},       {"jun", 6, MON_ANY},       {"juni", 6, MON_ANY},
        {"july", 7, MON_ANY},        {"jul", 7, MON_ANY},        {"juli", 7, MON_ANY},      {"august", 8, MON_ANY},
        {"aug", 8, MON_ANY},         {"augusti", 8, MON_ANY},    {"september", 9, MON_ANY}, {"sep", 9, MON_ANY},
        {"sept", 9, MON_ANY},        {"october", 10, MON_ANY},   {"oct", 10, MON_ANY},      {"okt", 10, MON_ANY},
        {"oktober", 10, MON_ANY},    {"november", 11, MON_ANY},  {"nov", 11, MON_ANY},      {"december", 12, MON_ANY},
        {"dec", 12, MON_ANY},
        {"enero", 1, MON_ES},        {"ene", 1, MON_ES},         {"febrero", 2, MON_ES},    {"marzo", 3, MON_ES},
        {"abril", 4, MON_ES},        {"abr", 4, MON_ES},         {"mayo", 5, MON_ES_DE},    {"junio", 6, MON_ES},
        {"julio", 7, MON_ES},        {"agosto", 8, MON_ES},      {"ago", 8, MON_ES_DE},     {"septiembre", 9, MON_ES},
        {"setiembre", 9, MON_ES},    {"octubre", 10, MON_ES},    {"noviembre", 11, MON_ES}, {"diciembre", 12, MON_ES},
        {"dic", 12, MON_ES},
    };
    if (!*w) return -1;
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if (strcmp(w, M[i].w) == 0) {
            *m = M[i].m;
            return M[i].kind;
        }
    return -1;
}

/* A month read first ("oct 12") or after "the 12th of": English and Swedish names. */
static int month_of(const char *w) {
    int m;
    return month_kind(w, &m) == MON_ANY ? m : 0;
}

/* A month after its day ("12 okt", "5 noviembre"); de: written after "de" ("5 de mayo"). */
static int month_after_day(const char *w, bool de) {
    int m, kind = month_kind(w, &m);
    return kind == MON_ANY || kind == MON_ES || (kind == MON_ES_DE && (de || es_ui())) ? m : 0;
}

static int weekday_of(const char *w) {
    static const char *const D[7][10] = {
        {"monday", "mon", "måndag", "mån", "mandag", "lunes", "lun", NULL},
        {"tuesday", "tue", "tues", "tisdag", "tis", "martes", NULL}, /* not "mar": that's March */
        {"wednesday", "wed", "onsdag", "ons", "miércoles", "miercoles", "mié", "mie", NULL},
        {"thursday", "thu", "thur", "thurs", "torsdag", "tors", "tor", "jueves", "jue", NULL},
        {"friday", "fri", "fredag", "fre", "viernes", NULL}, /* "vie": es_weekday */
        {"saturday", "sat", "lördag", "lör", "lordag", "sábado", "sabado", "sáb", "sab", NULL},
        {"sunday", "sun", "söndag", "sön", "sondag", "domingo", NULL},
    };
    if (!*w) return -1;
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 10 && D[i][j]; j++)
            if (strcmp(w, D[i][j]) == 0) return i;
    if (es_ui()) { /* Swedish "dom" = they, English "vie": see es_weekday */
        if (strcmp(w, "vie") == 0) return 4;
        if (strcmp(w, "dom") == 0) return 6;
    }
    return -1;
}

/* A weekday in a Spanish expression ("el vie", "lun-vie", "cada dom", "del vie al dom"), where
 * the short names that are other words too - "vie" (English), "dom" (Swedish "they") - are days. */
static int es_weekday(const char *w) {
    int wd = weekday_of(w);
    return wd >= 0 ? wd : eq(w, "vie") ? 4 : eq(w, "dom") ? 6 : -1;
}

static int weekday_plural_of(const char *w) {
    static const char *const D[7][5] = {
        {"mondays", "måndagar", "mandagar", NULL},     {"tuesdays", "tisdagar", NULL},
        {"wednesdays", "onsdagar", NULL},              {"thursdays", "torsdagar", NULL},
        {"fridays", "fredagar", NULL},                 {"saturdays", "lördagar", "lordagar", "sábados", "sabados"},
        {"sundays", "söndagar", "sondagar", "domingos", NULL},
    };
    if (!*w) return -1;
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 5 && D[i][j]; j++)
            if (strcmp(w, D[i][j]) == 0) return i;
    return -1;
}

/* Is any of the k tokens from `from` a word only Spanish has? */
static bool es_words(const Tokens *t, int from, int k) {
    int m;
    for (int j = from; j < from + k && j < t->n; j++)
        if (any_of(t->low[j], ES_WORDS) || month_kind(t->low[j], &m) >= MON_ES) return true;
    return false;
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
    } N[] = {{"a", 1},     {"an", 1},    {"one", 1},   {"en", 1},    {"ett", 1},   {"two", 2},    {"två", 2},
             {"tva", 2},   {"three", 3}, {"tre", 3},   {"four", 4},  {"fyra", 4},  {"five", 5},   {"fem", 5},
             {"six", 6},   {"sex", 6},   {"seven", 7}, {"sju", 7},   {"eight", 8}, {"åtta", 8},   {"atta", 8},
             {"nine", 9},  {"nio", 9},   {"ten", 10},  {"tio", 10},
             /* Spanish */
             {"un", 1},    {"una", 1},   {"uno", 1},   {"dos", 2},   {"tres", 3},  {"cuatro", 4}, {"cinco", 5},
             {"seis", 6},  {"siete", 7}, {"ocho", 8},  {"nueve", 9}, {"diez", 10}, {NULL, 0}};
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

/* "quarter of an hour", "quarter hour", "cuarto de hora" starting at token i */
static int quarter_at(const Tokens *t, int i) {
    if (eq(W(t, i), "cuarto") && eq(W(t, i + 1), "de") && unit_word(W(t, i + 2)) == 60) return 3;
    if (!eq(W(t, i), "quarter")) return 0;
    if (eq(W(t, i + 1), "of") && any_of(W(t, i + 2), A_W) && unit_word(W(t, i + 3)) == 60) return 4;
    if (unit_word(W(t, i + 1)) == 60) return 2;
    return 0;
}

/* Spanish "y media" ("dos horas y media", "una hora y media"): half a unit more. */
static bool y_media(const Tokens *t, int i) {
    return eq(W(t, i), "y") && (eq(W(t, i + 1), "media") || eq(W(t, i + 1), "medio"));
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
    if (any_of(w, HALF_W)) { /* "half an hour", "half hour", "media hora" */
        if (any_of(W(t, i + 1), A_W) && unit_word(W(t, i + 2)) == 60) return set_minutes(30, minutes) ? 3 : 0;
        if (unit_word(W(t, i + 1)) == 60) return set_minutes(30, minutes) ? 2 : 0;
        return 0;
    }
    if (eq(w, "hora") && y_media(t, i + 1)) return set_minutes(90, minutes) ? 3 : 0; /* "hora y media" */
    if (any_of(w, A_W)) { /* "en halvtimme", "en kvart", "a half hour", "a quarter of an hour", "un cuarto de hora" */
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
            } else if (y_media(t, i + 2)) { /* "2 horas y media" */
                v += 0.5;
                k = 4;
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
    if (any_of(w, NOW_W) || (eq(w, "ya") && es_ui())) { /* "ya" is English "you" too ("see ya") */
        *rel = 0;
        return eq(w, "ahora") && eq(W(t, i + 1), "mismo") ? 2 : 1;
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
    /* Spanish "en 30 minutos", "en una hora", "dentro de una hora". "en" is also Swedish for "a":
     * "en 30 min promenad" is a 30-minute walk, so it's only "in" with Spanish words after it (or
     * for people using Hemma in Spanish). */
    int off = eq(w, "en") ? 1 : eq(w, "dentro") && eq(W(t, i + 1), "de") ? 2 : 0;
    if (off) {
        int m, k = match_duration(t, i + off, &m);
        if (k && (off == 2 || es_ui() || es_words(t, i + off, k))) {
            *rel = m;
            return off + k;
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
        bool cada = eq(w, "cada");
        if (eq(x, "day") || eq(x, "dag") || eq(x, "día") || eq(x, "dia")) return *repeat = REPEAT_DAILY, 2;
        if (eq(x, "week") || eq(x, "vecka") || eq(x, "semana")) return *repeat = REPEAT_WEEKLY, 2;
        if (eq(x, "month") || eq(x, "månad") || eq(x, "manad") || eq(x, "mes")) return *repeat = REPEAT_MONTHLY, 2;
        if ((wd = cada ? es_weekday(x) : weekday_of(x)) >= 0) return *repeat = REPEAT_WEEKLY, *wday = wd, 2;
        if (any_of(x, OTHER_W)) {
            const char *y = W(t, i + 2);
            if (eq(y, "week")) return *repeat = REPEAT_BIWEEKLY, 3;
            if ((wd = weekday_of(y)) >= 0) return *repeat = REPEAT_BIWEEKLY, *wday = wd, 3;
            return 0;
        }
        if (whole_num(x, &v) || number_word(x, &v)) {
            const char *y = W(t, i + 2);
            if (v == 2 && (eq(y, "weeks") || eq(y, "veckor") || eq(y, "semanas"))) return *repeat = REPEAT_BIWEEKLY, 3;
            if (cada && v == 2 && (wd = es_weekday(y)) >= 0) /* "cada dos viernes" */
                return *repeat = REPEAT_BIWEEKLY, *wday = wd, 3;
            if (cada && (v == 14 || v == 15) && any_of(y, ES_DAYS)) return *repeat = REPEAT_BIWEEKLY, 3;
        }
        if (cada && eq(x, "quince") && any_of(W(t, i + 2), ES_DAYS)) /* "cada quince días": fortnightly */
            return *repeat = REPEAT_BIWEEKLY, 3;
        return 0;
    }
    if (eq(w, "varannan")) {
        const char *x = W(t, i + 1);
        if (eq(x, "vecka")) return *repeat = REPEAT_BIWEEKLY, 2;
        if ((wd = weekday_of(x)) >= 0) return *repeat = REPEAT_BIWEEKLY, *wday = wd, 2;
        return 0;
    }
    if (any_of(w, ES_ALL) && any_of(W(t, i + 1), ES_THE_PL)) { /* "todos los días", "todos los martes" */
        const char *x = W(t, i + 2);
        if (any_of(x, ES_DAYS)) return *repeat = REPEAT_DAILY, 3;
        if (eq(x, "semanas")) return *repeat = REPEAT_WEEKLY, 3;
        if (eq(x, "meses")) return *repeat = REPEAT_MONTHLY, 3;
        if ((wd = es_weekday(x)) >= 0 || (wd = weekday_plural_of(x)) >= 0)
            return *repeat = REPEAT_WEEKLY, *wday = wd, 3;
        return 0;
    }
    if (eq(w, "los")) { /* "los martes" = every Tuesday ("el martes" is one) */
        const char *x = W(t, i + 1);
        if ((wd = es_weekday(x)) >= 0 || (wd = weekday_plural_of(x)) >= 0)
            return *repeat = REPEAT_WEEKLY, *wday = wd, 2;
        return 0;
    }
    if (eq(w, "a") && eq(W(t, i + 1), "diario")) return *repeat = REPEAT_DAILY, 2;
    if (eq(w, "daily") || eq(w, "dagligen") || eq(w, "everyday") || eq(w, "diariamente"))
        return *repeat = REPEAT_DAILY, 1;
    if (eq(w, "weekly") || eq(w, "veckovis") || eq(w, "semanal") || eq(w, "semanalmente"))
        return *repeat = REPEAT_WEEKLY, 1;
    if (eq(w, "biweekly") || eq(w, "fortnightly") || eq(w, "quincenal") || eq(w, "quincenalmente"))
        return *repeat = REPEAT_BIWEEKLY, 1;
    if (eq(w, "monthly") || eq(w, "månadsvis") || eq(w, "manadsvis") || eq(w, "månatligen") || eq(w, "mensual") ||
        eq(w, "mensualmente"))
        return *repeat = REPEAT_MONTHLY, 1;
    if ((wd = weekday_plural_of(w)) >= 0) return *repeat = REPEAT_WEEKLY, *wday = wd, 1;
    return 0;
}

/* ------------------------------------------------------------------- days */

/* "12/10", "12/10/26", "12/10/2026" (day/month[/year]; start: see make_date). Other numbers
 * with a slash ("12/13", "50/50") are no dates, just words; one that can't be ("31/9") refuses. */
static bool slash_date(const P *p, const char *s, const Ymd *start, Ymd *out) {
    int d = 0, m = 0, y = 0, n;
    const char *q = s;
    for (n = 0; *q >= '0' && *q <= '9' && n < 3; q++, n++) d = d * 10 + (*q - '0');
    if (n < 1 || n > 2 || *q++ != '/') return false;
    for (n = 0; *q >= '0' && *q <= '9' && n < 3; q++, n++) m = m * 10 + (*q - '0');
    if (n < 1 || n > 2) return false;
    bool has_y = *q == '/';
    if (has_y) {
        q++;
        for (n = 0; *q >= '0' && *q <= '9' && n < 5; q++, n++) y = y * 10 + (*q - '0');
        if (n == 2) y += 2000;
        else if (n != 4) return false;
    }
    if (*q || m < 1 || m > 12 || d < 1 || d > 31) return false;
    if (has_y && (y < 1970 || y > 2200)) return no_such_day(p, (Ymd){y, m, d}); /* "1/1/0000" too */
    return make_date(p, y, m, d, start, out);
}

/* "2026-10-12"; one that can't be ("2026-09-31") refuses the text. */
static bool iso_date(const P *p, const char *s, Ymd *out) {
    Ymd r = {0, 0, 0};
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
        int *v = i < 4 ? &r.y : i < 7 ? &r.m : &r.d;
        *v = *v * 10 + (s[i] - '0');
    }
    if (r.m < 1 || r.m > 12 || r.d < 1 || r.d > 31) return false;
    if (!ymd_valid(r)) return no_such_day(p, r);
    *out = r;
    return true;
}

/* An optional 4-digit year at token j ("2026", Spanish "de 2026"): the tokens it takes; sets *y. */
static int opt_year(const Tokens *t, int j, int *y) {
    if (year_num(W(t, j), y)) return 1;
    return eq(W(t, j), "de") && year_num(W(t, j + 1), y) ? 2 : 0;
}

/* "weekend", "helg", "finde", "fin de semana" at token j: the tokens it takes. */
static int weekend_at(const Tokens *t, int j) {
    if (any_of(W(t, j), WEEKEND_W)) return 1;
    return eq(W(t, j), "fin") && eq(W(t, j + 1), "de") && eq(W(t, j + 2), "semana") ? 3 : 0;
}

/* Spanish next week: "próxima semana", "semana que viene", "semana próxima". */
static int es_week_at(const Tokens *t, int j) {
    if (any_of(W(t, j), ES_NEXT) && eq(W(t, j + 1), "semana")) return 2;
    if (!eq(W(t, j), "semana")) return 0;
    if (eq(W(t, j + 1), "que") && eq(W(t, j + 2), "viene")) return 3;
    return any_of(W(t, j + 1), ES_NEXT) ? 2 : 0;
}

/* After a Spanish day: "que viene" / "próximo" (the next one to come). Not "próxima semana". */
static int es_next_after(const Tokens *t, int j) {
    if (eq(W(t, j), "que") && eq(W(t, j + 1), "viene")) return 2;
    return any_of(W(t, j), ES_NEXT) && !eq(W(t, j + 1), "semana") ? 1 : 0;
}

/* Could token j start the end of a Spanish day range ("lunes a viernes", "del 28 de octubre al 3 de
 * noviembre")? Asked before reading "a" that way: English "fri a 3rd time" is no range. */
static bool es_end_ok(const Tokens *t, int j) {
    const char *x = W(t, j);
    int d, m;
    if (!*x) return false;
    if (es_weekday(x) >= 0 || eq(x, "el") || eq(x, "hoy") || is_manana(x) || eq(x, "pasado") || strchr(x, '/') ||
        (strlen(x) == 10 && x[4] == '-'))
        return true;
    if (num(x, 2, &d) && eq(W(t, j - 1), "al")) return true; /* "del 1 de diciembre al 3" */
    return num(x, 2, &d) && (eq(W(t, j + 1), "de") || month_kind(W(t, j + 1), &m) >= 0);
}

static int match_day_single(const P *p, int i, const Ymd *start, Ymd *out, Ymd *end, bool *has_end);

/* A Spanish day range's separator and end at token j: "a viernes", "al miércoles", "hasta el
 * domingo" (never before *start). */
static int es_range_tail(const P *p, int j, const Ymd *start, Ymd *e) {
    const Tokens *t = p->t;
    const char *s = W(t, j);
    if (!(eq(s, "a") || eq(s, "al") || eq(s, "hasta")) || !es_end_ok(t, j + 1)) return 0;
    Ymd e2;
    bool he;
    int k = match_day_single(p, j + 1, start, e, &e2, &he), wd, d;
    if (!k && !p->w->refused[0] && (wd = es_weekday(W(t, j + 1))) >= 0) /* "al dom" */
        *e = next_wday(*start, wd), k = 1;
    if (!k && !p->w->refused[0] && eq(s, "al") && num(W(t, j + 1), 2, &d) && month_day(*start, d, e))
        k = 1; /* "del 1 de diciembre al 3": the start's month, or the next */
    if (!k) return 0;
    if (he) *e = e2;
    if (ymd_days(*e) < ymd_days(*start)) return reversed(p);
    return 1 + k;
}

/* "5 al 10 de noviembre", "5 a 10 nov [de 2026]" at token j: one month's days. */
static int es_num_range(const P *p, int j, const Ymd *start, Ymd *out, Ymd *end, bool *has_end) {
    const Tokens *t = p->t;
    int d, d2, m, y = 0;
    if (!day_num(W(t, j), &d) || !(eq(W(t, j + 1), "al") || eq(W(t, j + 1), "a")) || !day_num(W(t, j + 2), &d2))
        return 0;
    int q = j + 3;
    bool de = eq(W(t, q), "de");
    if (de) q++;
    if (!(m = month_after_day(W(t, q), de))) return 0;
    int k = q + 1 - j + opt_year(t, q + 1, &y);
    Ymd a, b;
    if (!make_date(p, y, m, d, start, &a)) return 0;
    b = (Ymd){a.y, m, d2};
    if (!ymd_valid(b)) return no_such_day(p, b);
    if (d2 < d) return reversed(p);
    *out = a, *end = b, *has_end = true;
    return k;
}

/* After a weekday, Spanish: "lunes 5 de noviembre" (the date says which), "lunes que viene" /
 * "lunes próximo" (the next one to come), "viernes de la semana que viene" (next week's). */
static int es_after_weekday(const P *p, int i, int wd, const Ymd *start, Ymd *out) {
    const Tokens *t = p->t;
    int d, m, y = 0, q = i + 1, k;
    if (day_num(W(t, q), &d)) { /* with "de", or a month only Spanish has: not English "fri 12 oct" */
        int j = q + 1, mm;
        bool de = eq(W(t, j), "de");
        if (de) j++;
        if ((m = month_after_day(W(t, j), de)) && (de || month_kind(W(t, j), &mm) == MON_ES)) {
            k = j + 1 - i + opt_year(t, j + 1, &y);
            return make_date(p, y, m, d, start, out) ? k : 0;
        }
    }
    if ((k = es_next_after(t, q))) return *out = es_coming(p, wd), 1 + k;
    if (eq(W(t, q), "de")) q++;
    if (eq(W(t, q), "la")) q++;
    if ((k = es_week_at(t, q))) return *out = ymd_add(monday_next_week(p), wd), q + k - i;
    return 0;
}

static int ampm_word(const char *s);

/* "el jueves 12": the Thursday that is a 12th, the first to come (a time is "el jueves a las 12";
 * "jueves 12" without "el" stays Thursday at 12, as "thu 12" and "tors 12" do). Not "el lunes 2
 * horas", "el lunes 8 pm". */
static int es_weekday_daynum(const Tokens *t, int j, int wd, Ymd from, Ymd *out) {
    int d;
    if (!num(W(t, j), 2, &d) || d < 1 || d > 31 || unit_word(W(t, j + 1)) || ampm_word(W(t, j + 1))) return 0;
    Ymd r = {from.y, from.m, d};
    for (int n = 0; n < 15; n++) { /* a day of the month meets every weekday within 14 months */
        if (ymd_valid(r) && ymd_days(r) >= ymd_days(from) && ymd_wday(r) == wd) return *out = r, 1;
        if (++r.m > 12) r.m = 1, r.y++;
    }
    return 0;
}

/* Spanish day expressions that begin with a word of their own. */
static int match_day_es(const P *p, int i, const Ymd *start, Ymd *out, Ymd *end, bool *has_end) {
    const Tokens *t = p->t;
    const char *w = W(t, i), *x = W(t, i + 1);
    Ymd ref = start ? *start : p->today;
    int k, wd, d;
    if (eq(w, "pasado") && is_manana(x)) return *out = ymd_add(p->today, 2), 2; /* the day after tomorrow */
    if ((k = es_week_at(t, i))) return *out = monday_next_week(p), k;
    if (eq(w, "la") && (k = es_week_at(t, i + 1))) return *out = monday_next_week(p), 1 + k;
    if (any_of(w, ES_NEXT)) { /* "próximo lunes", "próximo fin de semana": the next one to come */
        if ((wd = es_weekday(x)) >= 0) return *out = es_coming(p, wd), 2;
        if ((k = weekend_at(t, i + 1))) return es_next_weekend(p, out, end), *has_end = true, 1 + k;
        return 0;
    }
    if ((k = eq(w, "finde") ? 1 : eq(w, "fin") ? weekend_at(t, i) : 0)) { /* "fin de semana", "finde" [que viene] */
        int n = es_next_after(t, i + k);
        if (n) es_next_weekend(p, out, end);
        else weekend(p, out, end);
        *has_end = true;
        return k + n;
    }
    if ((eq(w, "día") || eq(w, "dia")) && day_num(x, &d)) { /* "día 12", "día 12 de noviembre" */
        if ((k = match_day_single(p, i + 1, start, out, end, has_end))) return 1 + k;
        if (p->w->refused[0]) return 0;
        return month_day(ref, d, out) ? 2 : 0;
    }
    if (day_num(w, &d) && (k = es_num_range(p, i, start, out, end, has_end))) return k;
    if (p->w->refused[0]) return 0;
    if (eq(w, "el") || eq(w, "del")) { /* "el lunes", "el 5 de noviembre", "el próximo lunes", "del lunes al ..." */
        if (eq(w, "del") && (k = es_num_range(p, i + 1, start, out, end, has_end))) return 1 + k;
        if (p->w->refused[0]) return 0;
        int d2;
        if (eq(w, "del") && day_num(x, &d) && eq(W(t, i + 2), "al") && day_num(W(t, i + 3), &d2) &&
            month_day(ref, d, out) && month_day(*out, d2, end)) { /* "del 3 al 5": the next 3rd to the 5th after it */
            *has_end = true;
            return 4;
        }
        if ((k = match_day_single(p, i + 1, start, out, end, has_end))) {
            if (k == 1 && !*has_end && (wd = es_weekday(x)) >= 0) k += es_weekday_daynum(t, i + 2, wd, ref, out);
            /* "el fredag", "del 2": Swedish "el" (electricity), "del" (part) - unless it's Spanish all
               the same: Spanish words, a Spanish range after it ("del 28/12 al 2/1"), Spanish readers */
            const char *s = W(t, i + 1 + k);
            bool range = !*has_end && (eq(s, "a") || eq(s, "al") || eq(s, "hasta")) && es_end_ok(t, i + 2 + k);
            return es_ui() || es_words(t, i + 1, k) || range ? 1 + k : 0;
        }
        if (p->w->refused[0]) return 0;
        if ((wd = es_weekday(x)) >= 0) { /* "el vie", "el dom" [que viene] */
            if ((k = es_after_weekday(p, i + 1, wd, start, out)) || p->w->refused[0]) return k ? 1 + k : 0;
            *out = next_wday(ref, wd);
            return 2 + es_weekday_daynum(t, i + 2, wd, ref, out);
        }
        if (es_ui() && day_num(x, &d)) return month_day(ref, d, out) ? 2 : 0; /* "el 12": the 12th */
        return 0;
    }
    if (eq(w, "de")) { /* only a range's start ("de lunes a viernes") or "de hoy", "de mañana" (its) */
        if ((k = es_num_range(p, i + 1, start, out, end, has_end))) return 1 + k;
        if (p->w->refused[0]) return 0;
        if (!(k = match_day_single(p, i + 1, start, out, end, has_end)) || *has_end) return 0;
        const char *s = W(t, i + 1 + k);
        bool range = (eq(s, "a") || eq(s, "al") || eq(s, "hasta")) && es_end_ok(t, i + 2 + k);
        bool its = (k == 1 && (eq(x, "hoy") || is_manana(x))) || (k == 2 && eq(x, "pasado"));
        return range || its ? 1 + k : 0;
    }
    return 0;
}

/* One day expression starting at token i: a range's start (start NULL: weekdays and dates
 * count from today) or its end (from *start). Inline ranges ("fri-sun") set *has_end.
 * Returns 0 with p->w->refused set for a day that can't be or a range that runs backwards. */
static int match_day_single(const P *p, int i, const Ymd *start, Ymd *out, Ymd *end, bool *has_end) {
    const Tokens *t = p->t;
    const char *w = W(t, i);
    *has_end = false;
    if (!*w) return 0;
    int wd, d, d2, m, y = 0, k;
    char l[TOK_LEN], r[TOK_LEN];
    Ymd a, b, ref = start ? *start : p->today;

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
        bool es = eq(w, "este") || eq(w, "esta");
        if ((wd = es ? es_weekday(x) : weekday_of(x)) >= 0) return *out = next_wday(p->today, wd), 2;
        if ((k = weekend_at(t, i + 1))) return weekend(p, out, end), *has_end = true, 1 + k;
        if (any_of(x, EVENING_NOUN)) return *out = p->today, 2; /* "this evening", "esta noche" */
        if (es && (eq(x, "tarde") || is_manana(x))) return *out = p->today, 2; /* "esta tarde", "esta mañana" */
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
    if ((k = match_day_es(p, i, start, out, end, has_end)) || p->w->refused[0]) return k;
    if (any_of(w, TODAY_W)) return *out = p->today, 1;
    if (any_of(w, TOMORROW_W)) {
        if (is_manana(w) && i > 0 && strcmp(t->low[i - 1], "la") == 0) return 0; /* "por la mañana": a morning */
        return *out = ymd_add(p->today, 1), 1;
    }
    if (any_of(w, DAT_W)) return *out = ymd_add(p->today, 2), 1;
    if (eq(w, "weekend") || eq(w, "helgen")) return weekend(p, out, end), *has_end = true, 1;
    if ((wd = weekday_of(w)) >= 0) {
        if ((k = es_after_weekday(p, i, wd, start, out)) || p->w->refused[0]) return k;
        return *out = next_wday(ref, wd), 1;
    }

    if (split_dash(w, l, r)) {
        int wa = es_weekday(l), wb = es_weekday(r); /* two days joined: a range, "vie-dom" too */
        if (wa >= 0 && wb >= 0) {                   /* fri-sun */
            *out = next_wday(ref, wa);
            *end = next_wday(*out, wb);
            *has_end = true;
            return 1;
        }
        if (slash_date(p, l, start, &a)) { /* 12/10-14/10 */
            if (!slash_date(p, r, &a, &b)) return 0;
            if (ymd_days(b) < ymd_days(a)) return reversed(p);
            *out = a, *end = b, *has_end = true;
            return 1;
        }
        if (day_num(l, &d) && slash_date(p, r, start, &b)) { /* 12-14/10 */
            a = (Ymd){b.y, b.m, d};
            if (d > b.d) { /* "28-3/10": from the month before - for two weeks at most, else it's reversed */
                if (--a.m < 1) a.m = 12, a.y--;
                if (!ymd_valid(a)) return no_such_day(p, a);
                if (ymd_days(b) - ymd_days(a) > 14) return reversed(p); /* "14-12/10" */
            }
            *out = a, *end = b, *has_end = true;
            return 1;
        }
        if (p->w->refused[0]) return 0;
        int q = i + 1;
        bool de = eq(W(t, q), "de");
        if (de) q++;
        if (day_num(l, &d) && day_num(r, &d2) && (m = month_after_day(W(t, q), de))) { /* 12-14 okt [2026] */
            k = q + 1 - i + opt_year(t, q + 1, &y);
            if (!make_date(p, y, m, d, start, &a)) return 0;
            b = (Ymd){a.y, m, d2};
            if (!ymd_valid(b)) return no_such_day(p, b);
            if (d2 < d) return reversed(p);
            *out = a, *end = b, *has_end = true;
            return k;
        }
    }
    if (iso_date(p, w, &a)) return *out = a, 1;
    if (slash_date(p, w, start, &a)) return *out = a, 1;
    if (p->w->refused[0]) return 0;

    if (day_num(w, &d)) { /* 12 oct [2026], 12th of october, 5 de noviembre [de 2026] */
        int j = i + 1;
        bool de = false;
        if (eq(W(t, j), "of")) j++;
        else if (eq(W(t, j), "de")) j++, de = true;
        if ((m = month_after_day(W(t, j), de))) {
            k = j + 1 - i;
            k += opt_year(t, j + 1, &y);
            return make_date(p, y, m, d, start, out) ? k : 0;
        }
    }
    if ((m = month_of(w)) && day_num(W(t, i + 1), &d)) { /* oct 12[th] [2026] */
        k = 2 + opt_year(t, i + 2, &y);
        return make_date(p, y, m, d, start, out) ? k : 0;
    }
    if ((eq(w, "den") || eq(w, "the")) && day_num(W(t, i + 1), &d)) { /* den 12 [oktober], the 12th [of oct] */
        int j = i + 2;
        if (eq(W(t, j), "of") && month_of(W(t, j + 1))) j++;
        if ((m = month_of(W(t, j)))) {
            k = j + 1 - i;
            k += opt_year(t, j + 1, &y);
            return make_date(p, y, m, d, start, out) ? k : 0;
        }
        return month_day(ref, d, out) ? 2 : 0;
    }
    if (ordinal(w, &d)) return month_day(ref, d, out) ? 1 : 0; /* bare "12th" / "12:e" */
    return 0;
}

/* A day, optionally followed by a separator and an end day (never before it). *k1 = the
 * start day's words. */
static int match_day(const P *p, int i, Ymd *out, Ymd *end, bool *has_end, int *k1) {
    int k = *k1 = match_day_single(p, i, NULL, out, end, has_end);
    if (!k || *has_end) return k;
    const Tokens *t = p->t;
    const char *sep = W(t, i + k);
    bool between = i > 0 && !t->used[i - 1] && any_of(t->low[i - 1], BETWEEN_W);
    if (any_of(sep, DAY_SEP) || (between && any_of(sep, AND_W))) {
        Ymd e, e2;
        bool he2;
        int k2 = match_day_single(p, i + k + 1, out, &e, &e2, &he2);
        if (k2) {
            if (he2) e = e2;
            if (ymd_days(e) < ymd_days(*out)) return reversed(p);
            *end = e;
            *has_end = true;
            return k + 1 + k2;
        }
        if (p->w->refused[0]) return 0;
    }
    Ymd e;
    int k2 = es_range_tail(p, i + k, out, &e); /* "lunes a viernes", "del lunes al miércoles" */
    if (k2) {
        *end = e;
        *has_end = true;
        return k + k2;
    }
    if (p->w->refused[0]) return 0;
    return k;
}

/* ------------------------------------------------------------------ times */

enum { AMPM_NONE, AMPM_AM, AMPM_PM };

typedef struct {
    int h, m; /* m may be negative: "a las 7 menos cuarto" = 7:00 - 15 */
    int ampm;
    bool has_min, lead0, word, bare;
    bool night; /* "de la noche": that evening and the night after it, like "tonight" */
} TimeTok;

static int ampm_word(const char *s) {
    if (!strcmp(s, "am") || !strcmp(s, "a.m") || !strcmp(s, "a.m.")) return AMPM_AM;
    if (!strcmp(s, "pm") || !strcmp(s, "p.m") || !strcmp(s, "p.m.")) return AMPM_PM;
    return AMPM_NONE;
}

static bool parse_time_str(const char *s, TimeTok *tt) {
    memset(tt, 0, sizeof *tt);
    if (!*s) return false;
    if (!strcmp(s, "noon") || !strcmp(s, "lunch") || !strcmp(s, "lunchtime") || !strcmp(s, "midday") ||
        !strcmp(s, "mediodía") || !strcmp(s, "mediodia")) {
        tt->h = 12;
        tt->word = true;
        return true;
    }
    if (!strcmp(s, "midnight") || !strcmp(s, "midnatt") || !strcmp(s, "medianoche")) {
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

/* After "a las": the hours in words ("a la una", "a las dos" - said like a bare number) and the
 * 24-hour clock with an h ("a las 14h", "a las 9h", "14:30h" - as typed). */
static bool es_hour(const char *w, TimeTok *tt) {
    static const char *const H[] = {"una", "dos", "tres",  "cuatro", "cinco", "seis", "siete",
                                    "ocho", "nueve", "diez", "once", "doce", NULL};
    memset(tt, 0, sizeof *tt);
    for (int n = 0; H[n]; n++)
        if (eq(w, H[n])) {
            tt->h = n + 1;
            tt->bare = true;
            return true;
        }
    size_t n = strlen(w);
    char s[TOK_LEN];
    if (n < 2 || n >= sizeof s || w[n - 1] != 'h') return false;
    memcpy(s, w, n - 1);
    s[n - 1] = '\0';
    if (!parse_time_str(s, tt) || tt->ampm || tt->word) return false;
    tt->bare = false;
    return true;
}

static int to_24(int h, int ampm) {
    if (ampm == AMPM_AM) return h == 12 ? 0 : h;
    if (ampm == AMPM_PM) return h == 12 ? 12 : h + 12;
    return h;
}

/* Single time -> minutes. Bare 1..6 means afternoon: nobody books at 3 am.
 * A time of an evening day ("tonight 8", "ikväll 7:30", "fri night 11") belongs to that evening
 * and the night after it: 5..11 are pm, 12 and "midnight" the midnight that ends the day (24:00),
 * and whatever then falls before 05:00 - 1..4, "00:30", "1am" - is that night after midnight
 * (24:00 + time: "tonight 1" = 01:00 tomorrow). A leading zero (24-hour clock: "tonight 08"),
 * am/pm ("tonight 8am") and 13..24 mean what they say otherwise. "de la noche" makes one time
 * an evening one ("a las 10 de la noche", "a la 1 de la noche").
 * An afternoon ("esta tarde 4", "4:30") makes 1..11 pm; a morning ("por la mañana 6") takes the
 * numbers as they are (no afternoon guess). */
static int tt_minutes(const TimeTok *tt, int part) {
    if (part == DAY_EVENING || tt->night) {
        int h = tt->ampm                                 ? to_24(tt->h, tt->ampm)
                : tt->word                               ? (tt->h ? tt->h : 24) /* noon, midnight */
                : !tt->lead0 && tt->h >= 5 && tt->h <= 11 ? tt->h + 12
                : !tt->lead0 && tt->h == 12              ? 24
                                                         : tt->h;
        if (h < 5) h += 24;
        return h * 60 + tt->m;
    }
    int h = to_24(tt->h, tt->ampm);
    if (part == DAY_AFTERNOON) {
        if (!tt->ampm && !tt->word && !tt->lead0 && tt->h >= 1 && tt->h <= 11) h += 12;
    } else if (part != DAY_MORNING && tt->bare && !tt->lead0 && !tt->word && tt->h >= 1 && tt->h <= 6) {
        h += 12;
    }
    return h * 60 + tt->m;
}

static int fwd(int from, int to) { /* forward distance in minutes, 1..1440 */
    int d = (to - from) % 1440;
    return d <= 0 ? d + 1440 : d;
}

/* Pick am/pm for ambiguous ends so the range is as short as possible:
 * "2-5pm" = 14-17, "11-1pm" = 11-13, "6-8" = 18-20, "22-2" = 22-02; tonight "7-9" = 19-21. */
static void resolve_range(const TimeTok *a, const TimeTok *b, int part, int *s, int *e) {
    if (!a->ampm && b->ampm && !a->word && !a->lead0 && a->h >= 1 && a->h <= 12) {
        int bm = tt_minutes(b, part);
        int am = to_24(a->h, AMPM_AM) * 60 + a->m, pm = to_24(a->h, AMPM_PM) * 60 + a->m;
        if (part == DAY_EVENING && am < 5 * 60) am += 1440; /* tonight "1-3am": after midnight */
        *s = fwd(am, bm) <= fwd(pm, bm) ? am : pm;
        *e = bm;
        return;
    }
    *s = tt_minutes(a, part);
    if (!b->ampm && !b->word && !b->lead0 && b->h >= 1 && b->h <= 11) {
        int c1 = b->h * 60 + b->m, c2 = c1 + 12 * 60;
        *e = fwd(*s, c1) <= fwd(*s, c2) ? c1 : c2;
    } else {
        *e = tt_minutes(b, part);
    }
}

/* Spanish after a time: "de la tarde" (pm), "de la mañana" / "de la madrugada" (am), "de la noche"
 * (that evening and the night after it), also with "por la" / "en la", and "del mediodía" (pm).
 * Returns the tokens taken. */
static int es_day_part_suffix(const Tokens *t, int j, TimeTok *tt) {
    const char *a = W(t, j), *x = W(t, j + 2);
    int k = 3, ap = 0;
    bool night = false;
    if (eq(a, "del") && (eq(W(t, j + 1), "mediodía") || eq(W(t, j + 1), "mediodia"))) {
        ap = AMPM_PM, k = 2;
    } else {
        if (!(eq(a, "de") || eq(a, "por") || eq(a, "en")) || !eq(W(t, j + 1), "la")) return 0;
        if (is_manana(x) || eq(x, "madrugada")) ap = AMPM_AM;
        else if (eq(x, "tarde")) ap = AMPM_PM;
        else if (eq(x, "noche")) night = true;
        else return 0;
    }
    if (!tt->ampm && !tt->word && !tt->lead0 && tt->h >= 1 && tt->h <= 12) {
        if (night) tt->night = true;
        else tt->ampm = ap;
        tt->bare = false;
    }
    return k;
}

/* A time at token i. es: after a Spanish lead-in ("a las", "desde las", "hasta las" ...), which
 * also allows "a la una", "a las 14h", "a las 14 horas", "a las 8 y media", "a las 9 menos cuarto"
 * and "en punto". "de la tarde" and the like may follow any time. */
static int match_time_single(const Tokens *t, int i, TimeTok *tt, bool es) {
    if (!parse_time_str(W(t, i), tt) && !(es && es_hour(W(t, i), tt))) return 0;
    int ap = ampm_word(W(t, i + 1));
    if (ap && !tt->ampm && !tt->word && tt->h >= 1 && tt->h <= 12) { /* "2 pm" */
        tt->ampm = ap;
        tt->bare = false;
        return 2;
    }
    int q = i + 1;
    if (!tt->has_min && !tt->ampm && !tt->word && tt->h >= 1) { /* still bare, for the afternoon guess */
        if (eq(W(t, q), "y") && eq(W(t, q + 1), "media")) tt->m = 30, q += 2;
        else if (eq(W(t, q), "y") && eq(W(t, q + 1), "cuarto")) tt->m = 15, q += 2;
        else if (eq(W(t, q), "menos") && eq(W(t, q + 1), "cuarto")) tt->m = -15, q += 2;
    }
    if (eq(W(t, q), "en") && eq(W(t, q + 1), "punto")) q += 2; /* "a las 9 en punto" */
    if (es && q == i + 1 && tt->bare && any_of(W(t, q), ES_HOUR_UNIT)) { /* "a las 14 horas": the clock */
        tt->bare = false;
        q++;
    }
    return q - i + es_day_part_suffix(t, q, tt);
}

static bool has_time_prefix(const Tokens *t, int i) {
    return i > 0 && !t->used[i - 1] && any_of(t->low[i - 1], TIME_PREFIX);
}

/* A Spanish lead-in right before the time at token i: "a las", "a la", "desde las", "de las",
 * "entre las", "sobre las", "hacia las", "para las" (2), "desde", "entre" (1), "a medianoche",
 * "al mediodía" (1). "las" alone isn't one ("comprar las 2 pizzas"), nor "a" ("a 2 liter bottle").
 * Returns the tokens it takes. */
static int es_lead_before(const Tokens *t, int i) {
    if (i < 1 || i >= t->n || t->used[i - 1]) return 0;
    const char *a = t->low[i - 1];
    if (any_of(t->low[i], ES_TIME_WORD) && (eq(a, "a") || eq(a, "al"))) return 1;
    if (eq(a, "desde") || eq(a, "entre")) return 1;
    if (!any_of(a, ES_ART) || i < 2 || t->used[i - 2] || !any_of(t->low[i - 2], ES_HOUR_PREP)) return 0;
    return 2;
}

/* The same lead-in read forwards from token j (after a day: "hasta mañana a las 10"). */
static int es_lead_at(const Tokens *t, int j) { return eq(W(t, j), "a") && any_of(W(t, j + 1), ES_ART) ? 2 : 0; }

/* "between 2 and 5", "entre las 14 y las 16": the word right before the time (or its "las"). */
static bool between_before(const Tokens *t, int i) {
    int j = i - 1;
    if (j >= 0 && !t->used[j] && any_of(t->low[j], ES_ART)) j--;
    return j >= 0 && !t->used[j] && any_of(t->low[j], BETWEEN_W);
}

/* The separator of a time range at token j: "-", "to", "till" ..., "and" after "between", and
 * Spanish "hasta [las]", "y [las]" after "entre", and "a [las]" when the start was Spanish (es_ok:
 * "de 14 a 16", "desde las 14 a las 16") - else "at 2 a 3 hour drive" would be a range, and in
 * "el 15 a las 10" the 15 is a day. *es: a Spanish one. */
static int time_sep(const Tokens *t, int j, bool between, bool es_ok, bool *es) {
    const char *s = W(t, j);
    int art = any_of(W(t, j + 1), ES_ART) ? 1 : 0;
    *es = false;
    if (any_of(s, TIME_SEP)) return 1;
    if (between && any_of(s, AND_W)) return *es = art > 0, 1 + art;
    if (eq(s, "hasta") || (eq(s, "a") && es_ok)) return *es = true, 1 + art;
    return 0;
}

/* ---------------------------------------------------------------- evenings */

/* What a day word says about the times typed on it: "tonight", "kväll", "noche" (evening), "tarde"
 * (afternoon), "madrugada" and the "mañana" of "esta mañana" / "por la mañana" (morning). */
static int word_part(const Tokens *t, int j) {
    const char *w = t->low[j];
    if (any_of(w, EVENING_W) || any_of(w, EVENING_NOUN)) return DAY_EVENING;
    if (strcmp(w, "tarde") == 0) return DAY_AFTERNOON;
    if (strcmp(w, "madrugada") == 0) return DAY_MORNING;
    if (any_of(w, ES_MORNING) && j > 0 && (strcmp(t->low[j - 1], "la") == 0 || strcmp(t->low[j - 1], "esta") == 0))
        return DAY_MORNING;
    return DAY_PLAIN;
}

/* Marks tokens [from, to) as the words of a day expression, each with its word_part; returns the
 * expression's (its last word that names one, else DAY_PLAIN). */
static int mark_day(const P *p, int from, int to) {
    int kind = DAY_PLAIN;
    for (int j = from; j < to && j < p->t->n; j++) {
        int part = word_part(p->t, j);
        p->mark[j] = (int8_t)part;
        if (part != DAY_PLAIN) kind = part;
    }
    return kind;
}

/* After a day: "evening"/"kväll"/"noche" ("fri night", "imorgon kväll", "viernes noche"), "tarde",
 * "por la noche", "por la tarde", "por la mañana", "en la noche" (Latin America), "de madrugada".
 * es_only: the Spanish ones only (after a repeat: "los viernes por la noche"). The tokens taken. */
static int day_part_after(const Tokens *t, int j, bool es_only) {
    const char *a = W(t, j), *x = W(t, j + 2);
    if (any_of(a, EVENING_NOUN) && (!es_only || eq(a, "noche"))) return 1;
    if (eq(a, "tarde")) return 1;
    if ((eq(a, "por") || eq(a, "en")) && eq(W(t, j + 1), "la") &&
        (eq(x, "noche") || eq(x, "tarde") || is_manana(x) || eq(x, "madrugada")))
        return 3;
    return eq(a, "de") && eq(W(t, j + 1), "madrugada") ? 2 : 0;
}

/* Which part of a day the time at token i belongs to (see tt_minutes)? The day written right
 * before it decides ("tonight 8", "imorgon kväll kl 8", "esta noche a las 8"; in "tonight until
 * tomorrow 10" the 10 is tomorrow's, 10:00), else the start day, else one named anywhere
 * ("8 tonight", "a las 4 esta tarde"). */
static int start_part(const P *p) { return *p->date_mark ? *p->date_mark : p->named; }

static int time_part(const P *p, int i) {
    int j = i - 1 - es_lead_before(p->t, i);
    for (int n = 0; n < 2 && j >= 0 && !p->mark[j] && any_of(p->t->low[j], TIME_PREFIX); n++) j--;
    if (j >= 0 && p->mark[j]) return p->mark[j];
    return start_part(p);
}

/* A part of a day named anywhere in the text, for a time typed before its day ("8 tonight",
 * "a las 4 esta tarde"); 0 = none. */
static int part_named(const Tokens *t) {
    for (int i = 0; i < t->n; i++) {
        const char *w = W(t, i);
        if (any_of(w, EVENING_W)) return DAY_EVENING;
        if (!(eq(w, "i") || any_of(w, TODAY_W) || any_of(w, TOMORROW_W) || any_of(w, I_TOMORROW) ||
              any_of(w, DAT_W) || any_of(w, THIS_W) || weekday_of(w) >= 0))
            continue;
        int k = day_part_after(t, i + 1, false);
        if (k) return word_part(t, i + k);
        if ((eq(w, "esta") || eq(w, "este")) && is_manana(W(t, i + 1))) return DAY_MORNING;
    }
    return 0;
}

/* After the last day of a written-out range ("fri till sun 17", "until tomorrow kl 10", "tonight
 * - imorgon kväll 8", "hasta mañana a las 10"), the day words being [from, j): an optional part
 * of the day, then an optional end time on that day. Marks the day's words; returns the tokens
 * taken from j. */
static int end_day_tail(const P *p, int from, int j, When *w) {
    const Tokens *t = p->t;
    int n = j + day_part_after(t, j, false), q; /* "until tomorrow evening", "hasta mañana por la noche" */
    int part = mark_day(p, from, n);
    for (q = n; q < n + 2 && any_of(W(t, q), END_TIME_PREFIX); q++) {}
    int lead = q == n ? es_lead_at(t, q) : 0;
    TimeTok tt;
    int kt = w->has_end ? 0 : match_time_single(t, q + lead, &tt, lead > 0);
    if (kt && !unit_word(W(t, q + lead + kt))) { /* a time, not "2 nights" */
        w->has_end = true;
        w->end_min = tt_minutes(&tt, part);
        n = q + lead + kt;
    }
    return n - j;
}

/* "until sunday", "till 17", "tills söndag", "hasta el domingo", "hasta las 17", and "until sunday
 * 17" / "till imorgon kl 10" / "hasta mañana a las 10" (the end time, on that day). From the day
 * mentioned before, else from today. */
static int match_until(const P *p, int i, When *w) {
    const Tokens *t = p->t;
    if (!any_of(W(t, i), UNTIL_W)) return 0;
    Ymd ref = w->has_date ? (Ymd){w->y, w->m, w->d} : p->today, e, e2;
    bool he;
    int k = match_day_single(p, i + 1, &ref, &e, &e2, &he);
    if (k) {
        if (he) e = e2;
        if (ymd_days(e) < ymd_days(ref)) return reversed(p); /* "until 5/10" on the 7th */
        w->has_end_date = true;
        w->ey = e.y, w->em = e.m, w->ed = e.d;
        return 1 + k + end_day_tail(p, i + 1, i + 1 + k, w);
    }
    if (p->w->refused[0]) return 0;
    bool hasta = eq(W(t, i), "hasta");
    int art = hasta && any_of(W(t, i + 1), ES_ART) ? 1 : 0; /* "hasta las 17" */
    TimeTok tt;
    k = match_time_single(t, i + 1 + art, &tt, hasta);
    if (k) {
        w->has_end = true;
        if (w->has_start && !tt.ampm && !tt.word && !tt.lead0 && !tt.night && tt.h >= 1 && tt.h <= 11) {
            int c1 = tt.h * 60 + tt.m, c2 = c1 + 12 * 60;
            w->end_min = fwd(w->start_min, c1) <= fwd(w->start_min, c2) ? c1 : c2;
        } else { /* "ikväll till 11": the start day's evening */
            w->end_min = tt_minutes(&tt, start_part(p));
        }
        return 1 + art + k;
    }
    return 0;
}

/* A time or a time range at token i. Bare single numbers ("14") are only accepted after a
 * prefix ("kl 14", "a las 14"); the caller decides on those in a second pass. lead = the Spanish
 * lead-in before it (es_lead_before); *es_range: a Spanish separator ("de 14 a 16"). */
static int match_time(const P *p, int i, int lead, int *s, int *e, bool *has_e, bool *es_range) {
    const Tokens *t = p->t;
    const char *w = W(t, i);
    TimeTok a, b;
    char l[TOK_LEN], r[TOK_LEN];
    int part = time_part(p, i);
    *has_e = *es_range = false;
    if (!*w) return 0;
    if (split_dash(w, l, r) && parse_time_str(l, &a) && parse_time_str(r, &b)) { /* 14-17, 2-5pm */
        int k = 1, ap = ampm_word(W(t, i + 1));
        if (ap && !b.ampm && !b.word && b.h >= 1 && b.h <= 12) {
            b.ampm = ap;
            b.bare = false;
            k = 2;
        } else {
            k += es_day_part_suffix(t, i + 1, &b); /* "8-10 de la noche" */
        }
        resolve_range(&a, &b, b.night ? DAY_EVENING : part, s, e);
        *has_e = true;
        return k;
    }
    int k = match_time_single(t, i, &a, lead > 0);
    if (!k) return 0;
    bool es_ok = lead > 0 || (i > 0 && !t->used[i - 1] && strcmp(t->low[i - 1], "de") == 0) || es_ui();
    bool es;
    int sk = time_sep(t, i + k, between_before(t, i), es_ok, &es);
    if (sk) {
        int k2 = match_time_single(t, i + k + sk, &b, es);
        if (k2) {
            resolve_range(&a, &b, a.night || b.night ? DAY_EVENING : part, s, e); /* "de 8 a 10 de la noche" */
            *has_e = true;
            *es_range = es;
            return k + sk + k2;
        }
    }
    if (!(has_time_prefix(t, i) || lead) && a.bare && k == 1) return 0;
    *s = tt_minutes(&a, part);
    return k;
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
    int8_t mark[TOK_MAX] = {0};
    int date_mark = 0;
    P p = {t, now, {dn.year, dn.mon, dn.day}, dn.wday, part_named(t), mark, &date_mark, w};
    bool found = false;
    int day_end = -1; /* token index just after the last day expression */
    int dur_end = -1; /* token index just after the last duration */
    bool claimed[TOK_MAX]; /* the caller's marks, put back if the text is refused */
    memcpy(claimed, t->used, sizeof claimed);

    for (int i = 0; i < t->n && !w->refused[0]; i++) {
        if (t->used[i]) continue;
        int k, k1, a, b, mins, kp, lead = es_lead_before(t, i);
        bool he, esr;
        Ymd y1, y2;
        if ((k = match_repeat(t, i, &a, &b))) {
            if (!w->repeat) w->repeat = a, w->repeat_wday = b;
            use_prefix(t, i, REPEAT_PREFIX, 1);
            if (b >= 0) k += day_part_after(t, i + k, true); /* "los viernes por la noche" */
            use(t, i, k);
            if (b >= 0) day_end = i + k, mark_day(&p, i, i + k);
        } else if ((k = match_relative(t, i, &mins))) {
            if (!w->now) w->now = true, w->rel_min = mins;
            use(t, i, k);
        } else if (lead < 2 && (k = match_duration(t, i, &mins))) { /* "a las 14 horas" is a time */
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
        } else if (!w->has_date && (k = match_day(&p, i, &y1, &y2, &he, &k1))) {
            if (!he && (kp = day_part_after(t, i + k, false))) k += kp, k1 += kp; /* "imorgon kväll", "fri night" */
            set_date(w, y1);
            if (he) w->has_end_date = true, w->ey = y2.y, w->em = y2.m, w->ed = y2.d;
            date_mark = mark_day(&p, i, i + k1);
            if (k > k1) k += end_day_tail(&p, i + k1, i + k, w); /* "fri till sun 17": ends Sun 17:00 */
            use_prefix(t, i, DAY_PREFIX, 1);
            use(t, i, k);
            day_end = i + k;
        } else if (!w->has_start && (k = match_time(&p, i, lead, &a, &b, &he, &esr))) {
            w->has_start = true;
            w->start_min = a;
            if (he) w->has_end = true, w->end_min = b;
            if (lead) use(t, i - lead, lead);
            else use_prefix(t, i, TIME_PREFIX, 2);
            if (esr && !lead && i > 0 && !t->used[i - 1] && strcmp(t->low[i - 1], "de") == 0)
                t->used[i - 1] = true; /* "de 14 a 16" */
            use(t, i, k);
        } else if ((k = match_until(&p, i, w))) {
            use(t, i, k);
        } else {
            continue;
        }
        found = true;
        i += k - 1;
    }
    if (w->refused[0]) { /* nothing but the reason, every word back as the caller had it */
        When why = {.repeat_wday = -1};
        memcpy(why.refused, w->refused, sizeof why.refused);
        *w = why;
        memcpy(t->used, claimed, sizeof claimed);
        return false;
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
            w->start_min = tt_minutes(&best, time_part(&p, pick));
            t->used[pick] = true;
            found = true;
        }
    }
    /* Typed between midnight and 05:00, "tonight" is still the night we're in for a time that
     * hasn't passed (15 minutes' grace): "tonight 12:30" at 00:10 is in 20 minutes, not the next
     * night; "tonight 11" is still this evening, and "tonight 2" at 03:00 the next night. */
    int now_min = dn.hour * 60 + dn.min;
    if (date_mark == DAY_EVENING && dn.hour < 5 && w->y == dn.year && w->m == dn.mon && w->d == dn.day) {
        if (w->has_start && w->start_min >= 1440 && w->start_min - 1440 >= now_min - 15) {
            w->start_min -= 1440;
            if (w->has_end && w->end_min >= 1440) w->end_min -= 1440;
        } else if (!w->has_start && w->has_end && w->end_min >= 1440 && w->end_min - 1440 > now_min) {
            w->end_min -= 1440; /* "ikväll till 2" at 00:10: from now until 02:00 */
        }
    }
    if (w->repeat_wday >= 0 && !w->has_date) set_date(w, next_wday(p.today, w->repeat_wday));
    return found;
}

/* ---------------------------------------------------------------- resolve */

/* Seconds from 1970-01-01 00:00 to what the clock shows: civil time as if it were UTC. */
static int64_t civil_sec(DateTime d) {
    return (int64_t)days_from_civil(d.year, d.mon, d.day) * 86400 + d.hour * 3600 + d.min * 60 + d.sec;
}

/* The instant the clock here shows `min` minutes after the start of `day` (1440 and up: the
 * days after). The clock changes are settled here, the same on every C library: a time in the
 * hour skipped in spring moves forward by the jump (02:30 -> 03:30, what calendars do, RFC 5545);
 * one in the hour that repeats in autumn means its first pass. The offsets a day before and a
 * day after give the two candidates (no zone changes its clocks twice within two days). */
static int64_t wall_time(Ymd day, int min) {
    int64_t c = (int64_t)ymd_days(day) * 86400 + (int64_t)min * 60;
    int64_t a = c - utc_offset(c - 86400), b = c - utc_offset(c + 86400);
    bool va = civil_sec(dt_local(a)) == c, vb = civil_sec(dt_local(b)) == c;
    if (va && vb) return MIN(a, b);
    if (vb && !va) return b;
    return a; /* a, or neither: skipped, and the offset from before the jump moves it on */
}

bool when_resolve(const When *w, int64_t now, int default_min, int64_t *start, int64_t *end) {
    if (w->refused[0] ||
        !(w->has_date || w->has_end_date || w->has_start || w->has_end || w->dur_min || w->now || w->repeat))
        return false;
    DateTime n = dt_local(now);
    int64_t now_m = now - n.sec;
    Ymd today = {n.year, n.mon, n.day}, day = w->has_date ? (Ymd){w->y, w->m, w->d} : today;
    bool allday = false;
    int smin; /* the start as the clock shows it: minutes after the start of `day` */
    int64_t s, e;

    if (w->now) {
        s = now_m + (int64_t)w->rel_min * 60;
        DateTime ds = dt_local(s);
        day = (Ymd){ds.year, ds.mon, ds.day};
        smin = ds.hour * 60 + ds.min;
    } else if (w->has_start) {
        smin = w->start_min;
        s = wall_time(day, smin);
        if (!w->has_date && s < now - 15 * 60) day = ymd_add(day, 1), s = wall_time(day, smin);
    } else if ((w->has_end || (w->dur_min && !w->has_end_date)) && ymd_days(day) == ymd_days(today)) {
        s = now_m; /* "until 17", "2h", "until sunday 17": from now */
        smin = n.hour * 60 + n.min;
    } else {
        smin = 0;
        s = wall_time(day, 0);
        allday = !w->has_end;
    }
    /* the clocks jumped over the start, which moved on: a range keeps the length typed */
    bool skipped = civil_sec(dt_local(s)) != (int64_t)ymd_days(day) * 86400 + (int64_t)smin * 60;

    if (w->has_end_date) {
        e = wall_time((Ymd){w->ey, w->em, w->ed}, w->has_end ? w->end_min : 1440);
        if (e <= s) {
            DateTime ds = dt_local(s);
            e = wall_time((Ymd){ds.year, ds.mon, ds.day}, 1440);
        }
    } else if (w->has_end) {
        /* on the clock, not in elapsed time: 01:00-04:00 is 01:00-04:00 even on the night the
           clocks change, and an end only goes to the next day when it is not after the start */
        int em = w->end_min;
        while (em <= smin) em += 1440; /* "22-02" */
        e = wall_time(day, em);
        if (skipped || e <= s) e = s + (int64_t)(em - smin) * 60;
    } else if (w->dur_min) {
        if (w->dur_min % 1440 == 0 && smin % 1440 == 0) e = wall_time(day, smin + w->dur_min); /* whole days */
        else e = s + (int64_t)w->dur_min * 60;
    } else if (allday) {
        e = wall_time(day, 1440);
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

/* A capital first letter where a line starts: Swedish and Spanish write weekdays in lower case
 * (base.c), and every one starts with a plain letter (as core/ui.c's day_words). */
static void capital(char *s) {
    if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
}

/* first: the line starts with it. English keeps its capitals anywhere ("Today 22:00 – Tomorrow 02:00");
 * Swedish and Spanish write the second day in lower case ("Idag 22:00 – imorgon 02:00"). */
static void day_label(DateTime d, DateTime n, bool first, char *out, size_t cap) {
    long diff = day_index(d) - day_index(n);
    bool big = first || i18n_lang() == LANG_EN;
    if (diff == 0) snprintf(out, cap, "%s", big ? T("Today") : T("today"));
    else if (diff == 1) snprintf(out, cap, "%s", big ? T("Tomorrow") : T("tomorrow"));
    else if (diff == -1) snprintf(out, cap, "%s", big ? T("Yesterday") : T("yesterday"));
    else if (d.year == n.year) snprintf(out, cap, "%s %d %s", weekday_name(d.wday, true), d.day, month_name(d.mon, true));
    else snprintf(out, cap, "%s %d %s %d", weekday_name(d.wday, true), d.day, month_name(d.mon, true), d.year);
    if (first) capital(out);
}

void when_describe(int64_t start, int64_t end, int64_t now, char *out, size_t cap) {
    if (!cap) return;
    DateTime s = dt_local(start), e = dt_local(end), n = dt_local(now);
    char a[48], b[48], tmp[160];
    if (end > start && s.hour == 0 && s.min == 0 && e.hour == 0 && e.min == 0) { /* all-day */
        DateTime last = dt_local(add_days(end, -1));
        if (day_index(last) <= day_index(s)) {
            day_label(s, n, true, a, sizeof a);
            snprintf(tmp, sizeof tmp, T("%s, all day"), a);
        } else {
            if (s.year == last.year && s.mon == last.mon)
                snprintf(a, sizeof a, "%s %d", weekday_name(s.wday, true), s.day);
            else if (s.year == n.year || s.year == last.year)
                snprintf(a, sizeof a, "%s %d %s", weekday_name(s.wday, true), s.day, month_name(s.mon, true));
            else
                snprintf(a, sizeof a, "%s %d %s %d", weekday_name(s.wday, true), s.day, month_name(s.mon, true), s.year);
            capital(a);
            if (last.year == n.year)
                snprintf(b, sizeof b, "%s %d %s", weekday_name(last.wday, true), last.day, month_name(last.mon, true));
            else
                snprintf(b, sizeof b, "%s %d %s %d", weekday_name(last.wday, true), last.day,
                         month_name(last.mon, true), last.year);
            snprintf(tmp, sizeof tmp, "%s " EN_DASH " %s", a, b);
        }
    } else {
        day_label(s, n, true, a, sizeof a);
        bool same = day_index(e) == day_index(s) ||
                    (e.hour == 0 && e.min == 0 && day_index(e) == day_index(s) + 1 && end > start);
        if (end <= start) {
            snprintf(tmp, sizeof tmp, "%s %02d:%02d", a, s.hour, s.min);
        } else if (same) {
            snprintf(tmp, sizeof tmp, "%s %02d:%02d" EN_DASH "%02d:%02d", a, s.hour, s.min, e.hour, e.min);
        } else {
            day_label(e, n, false, b, sizeof b);
            snprintf(tmp, sizeof tmp, "%s %02d:%02d " EN_DASH " %s %02d:%02d", a, s.hour, s.min, b, e.hour, e.min);
        }
    }
    str_copy(out, cap, tmp);
}
