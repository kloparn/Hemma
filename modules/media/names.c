/* modules/media/names.c — movie / series / episode from a library path.
 *
 * Plex/Kodi-style heuristics: SxxEyy, 1x02, "Season 1 Episode 2",
 * "Show/Season 2/05 - Title.mkv", anime "[Group] Show - 05", "Title (2010)",
 * scene names "Title.2010.1080p.BluRay.x264-GRP" and folder fallbacks for
 * junk file names ("Movies/Inception (2010)/movie.mkv"). */
#include "mediainfo.h"

#include <ctype.h>

#define MAX_TOK 96
#define TOK_LEN 64

typedef struct {
    char s[TOK_LEN];  /* original text */
    char lo[TOK_LEN]; /* case-folded */
    int grp;          /* 0 plain, 1 inside (...), 2 inside [...] or {...} */
    int gid;          /* bracket group id, to re-wrap multi-word "(US Version)" */
} Tok;

typedef struct {
    Tok t[MAX_TOK];
    int n;
    bool lead_group; /* starts with a [Group] tag (anime releases) */
} Toks;

typedef struct {
    int start, end; /* token range of the marker */
    int season;     /* -1 = not part of the marker */
    int ep, ep_end;
} EpMatch;

typedef struct {
    char title[128]; /* cut at the year */
    char full[128];  /* cut only at tags: keeps a trailing year ("Christmas 2019") */
    int year;
    bool year_paren; /* the year was written "(2010)" */
    bool tags;       /* quality/source/codec tags followed the title */
} Clean;

/* ------------------------------------------------------------- word lists */

static const char *const TAGS[] = {
    "4k", "8k", "uhd", "fhd", "hdr", "hdr10", "hdr10+", "hdr10plus", "dv", "dovi", "sdr", "hi10p", "hi10", "bluray",
    "blu-ray", "bdrip", "brrip", "bdremux", "remux", "web-dl", "webdl", "webrip", "web-rip", "hdtv", "pdtv", "sdtv",
    "dsr", "dvdrip", "dvdscr", "dvd", "dvd5", "dvd9", "dvdr", "hdrip", "hdcam", "hdts", "telesync", "telecine",
    "vhsrip", "tvrip", "satrip", "uhdrip", "amzn", "nf", "dsnp", "hmax", "atvp", "pcok", "hulu", "itunes", "viaplay",
    "cmore", "svtplay", "tv4play", "dplay", "x264", "x265", "h264", "h265", "hevc", "avc", "xvid", "divx", "av1",
    "vp9", "mpeg2", "vc1", "aac", "ac3", "eac3", "dd", "ddp", "dd+", "dts", "dts-hd", "dtshd", "dts-ma", "dts-x",
    "truehd", "atmos", "flac", "opus", "mp3", "lpcm", "proper", "repack", "rerip", "internal", "limited", "extended",
    "unrated", "uncut", "remastered", "imax", "criterion", "theatrical", "multi", "multisubs", "subbed", "dubbed",
    "swesub", "swesubs", "nordic", "nordicsubs", "swe", "eng", "dual", "dual-audio", "complete", "readnfo", "nfofix",
    "hc", "hardsub", "hardcoded", "sample", NULL};

/* Tags that are also ordinary words: they never cut the very first title word ("Dual (2022)"). */
static const char *const WORDLIKE_TAGS[] = {"dual",   "complete", "limited", "extended",  "internal", "proper",
                                            "unrated", "uncut",   "remastered", "theatrical", "criterion", "multi",
                                            "sample", "imax",     "atmos",   "nordic",    "dubbed",   "subbed",
                                            "hulu",   "dv",       "dd",      "hc",        "swe",      "eng",
                                            "repack", "nf",       NULL};

/* Language words are tags only in scene casing ("SWEDiSH", "NORDiC") or when
   shouting after a normally-cased title — "The Danish Girl" must survive. */
static const char *const LANG_WORDS[] = {"swedish", "danish",  "norwegian", "finnish", "nordic",  "english",
                                         "german",  "french",  "spanish",   "italian", "dutch",   "icelandic",
                                         NULL};

/* codec names that are often glued to a channel count: DDP5.1 -> "ddp5" "1" */
static const char *const CH_PREFIX[] = {"ddp", "dd", "aac", "ac3", "eac3", "dts", "truehd", "atmos", "flac", "opus", NULL};

static const char *const SEASON_WORDS[] = {"season", "säsong",    "series",   "saison", "staffel",
                                           "temporada", "stagione", "sæson", "sesong",   "kausi", NULL};
static const char *const EPISODE_WORDS[] = {"episode", "episod",   "ep",    "avsnitt",
                                            "afsnit",  "episodio", "folge", "jakso", NULL};

static const char *const SMALL_WORDS[] = {"a",  "an",   "and", "as",  "at",  "but", "by",  "for",  "from",
                                          "in", "into", "nor", "of",  "on",  "or",  "the", "to",   "vs",
                                          "via", "with", "och", "på", "av", "med", "för", "till", "om",
                                          "en", "ett",  NULL};
static const char *const UPPER_WORDS[] = {"uk", "usa", "tv",  "fbi", "csi", "nypd", "ncis", "bbc",  "cia", "nasa",
                                          "ufo", "ii",  "iii", "iv",  "vi",  "vii",  "viii", "ix",   "xi",  "xii",
                                          "xiii", "xiv", "xv",  NULL};

enum { GEN_NONE, GEN_SERIES, GEN_MOVIES, GEN_HOME, GEN_NEUTRAL, GEN_EXTRAS };

static const char *const DIR_SERIES[] = {"series", "serier", "tv", "tv shows", "tvshows", "tv series", "tv serier",
                                         "tvserier", "shows", "anime", "tv program", NULL};
static const char *const DIR_MOVIES[] = {"movies", "movie", "films", "film", "filmer", "barnfilmer", "kids movies",
                                         "4k movies", "hd movies", NULL};
static const char *const DIR_HOME[] = {"home videos", "home video", "hemvideor", "hemvideo", "hemfilmer", "family",
                                       "familj",     "familjen",   "camera",    "kamera",   "camera roll", "dcim",
                                       "phone",      "mobil",      "recordings", "inspelningar", "clips", "klipp",
                                       "other",      "övrigt",     "misc",      "personal", "privat", NULL};
static const char *const DIR_NEUTRAL[] = {"media",   "video",    "videos",   "videor",   "downloads", "download",
                                          "nedladdningar", "nedladdat", "new folder", "ny mapp", "torrents",
                                          "complete", "completed", "incoming", "library",  "bibliotek", "public",
                                          "shared",  "delat",    "usb",      "hdd",      "disk",      "volumes",
                                          "mnt",     "share",    "barn",     "kids",     NULL};
static const char *const DIR_EXTRAS[] = {"extras",  "extra",  "featurettes", "featurette", "behind the scenes",
                                         "deleted scenes", "interviews", "trailers", "trailer", "samples",
                                         "sample",  "bonus",  "scenes",      "shorts",     NULL};

/* ---------------------------------------------------------------- helpers */

static bool in_list(const char *s, const char *const *list) {
    for (; *list; list++)
        if (strcmp(s, *list) == 0) return true;
    return false;
}

static bool all_digits(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* Parse 1..maxlen digits at *pp. Fails (-1) when there are none or more than maxlen. */
static int digits_at(const char **pp, int maxlen) {
    const char *p = *pp;
    int v = 0, n = 0;
    while (n < maxlen && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
        n++;
    }
    if (n == 0 || isdigit((unsigned char)*p)) return -1;
    *pp = p;
    return v;
}

static bool is_num(const char *s, int minlen, int maxlen, int *v) {
    size_t n = strlen(s);
    if ((int)n < minlen || (int)n > maxlen || !all_digits(s, n)) return false;
    *v = atoi(s);
    return true;
}

/* "05" or "05v2" (anime re-releases) */
static bool is_num_v(const char *s, int maxlen, int *v) {
    const char *p = s;
    int val = digits_at(&p, maxlen);
    if (val < 0) return false;
    if (*p == 'v' && isdigit((unsigned char)p[1]) && !p[2]) p += 2;
    if (*p) return false;
    *v = val;
    return true;
}

static bool is_dash(const char *s) {
    return !strcmp(s, "-") || !strcmp(s, "~") || !strcmp(s, "\xE2\x80\x93") || !strcmp(s, "\xE2\x80\x94");
}

static bool is_single_letter(const Tok *t) {
    return t->grp == 0 && t->s[0] && !t->s[1] && isalpha((unsigned char)t->s[0]);
}

/* ------------------------------------------------------------- tokenizing */

static void push_tok(Toks *T, const char *s, size_t n, int grp, int gid) {
    if (T->n >= MAX_TOK || !n) return;
    char tmp[256];
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    Tok *t = &T->t[T->n++];
    str_copy(t->s, sizeof t->s, tmp);
    fold_copy(t->lo, sizeof t->lo, t->s);
    t->grp = grp;
    t->gid = gid;
}

static void tokenize(const char *s, Toks *T) {
    T->n = 0;
    int grp = 0, gid = 0;
    const char *start = NULL;
    for (const char *p = s;; p++) {
        char c = *p;
        bool open = c == '(' || c == '[' || c == '{';
        bool close = c == ')' || c == ']' || c == '}';
        bool split = c == ' ' || c == '.' || c == '_' || c == '\t' || c == ',';
        if (c && !split && !open && !close) {
            if (!start) start = p;
            continue;
        }
        if (start) {
            /* keep a trailing comma attached so "Tiger, Hidden" survives */
            size_t n = (size_t)(p - start);
            push_tok(T, start, n, grp, gid);
            if (c == ',' && T->n) {
                Tok *t = &T->t[T->n - 1];
                size_t l = strlen(t->s);
                if (l + 1 < sizeof t->s) {
                    t->s[l] = ',';
                    t->s[l + 1] = 0;
                }
            }
            start = NULL;
        }
        if (!c) break;
        if (open) {
            grp = c == '(' ? 1 : 2;
            gid++;
        } else if (close) {
            grp = 0;
        }
    }
    T->lead_group = T->n > 0 && T->t[0].grp == 2;
}

/* ------------------------------------------------------- tags and markers */

static bool tag_word(const char *w) {
    if (in_list(w, TAGS)) return true;
    size_t n = strlen(w);
    if ((n == 4 || n == 5) && (w[n - 1] == 'p' || w[n - 1] == 'i') && all_digits(w, n - 1)) return true; /* 1080p */
    if (n > 3 && str_ends(w, "bit") && all_digits(w, n - 3)) return true;                                /* 10bit */
    for (int i = 0; CH_PREFIX[i]; i++) {
        size_t k = strlen(CH_PREFIX[i]);
        if (n > k && !strncmp(w, CH_PREFIX[i], k) && isdigit((unsigned char)w[k])) return true;
    }
    return str_starts(w, "hdr");
}

static bool is_tag(const char *lo) {
    if (tag_word(lo)) return true;
    const char *dash = strchr(lo, '-'); /* "x264-grp", "1080p-grp" */
    if (dash && dash != lo) {
        char w[TOK_LEN];
        size_t n = (size_t)(dash - lo);
        if (n >= sizeof w) return false;
        memcpy(w, lo, n);
        w[n] = 0;
        return tag_word(w);
    }
    return false;
}

static bool has_lower(const char *s, bool except_i) {
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z' && !(except_i && *s == 'i')) return true;
    return false;
}

/* token i is a language release tag; `strict` (after an episode marker) means any upper-case form counts */
static bool is_lang_tag(const Toks *T, int a, int i, bool strict) {
    const Tok *t = &T->t[i];
    if (!in_list(t->lo, LANG_WORDS) || has_lower(t->s, true)) return false; /* "Danish" */
    if (strchr(t->s, 'i')) return i > a || strict;                          /* "SWEDiSH" */
    if (strict) return true;                                                /* "SWEDISH" after S01E02 */
    for (int k = a; k < i; k++)
        if (has_lower(T->t[k].s, false)) return true; /* "Solsidan SWEDISH" but not "THE SWEDISH ..." */
    return false;
}

static int tok_year(const Tok *t) {
    if (strlen(t->lo) != 4 || !all_digits(t->lo, 4)) return 0;
    int y = atoi(t->lo);
    return (y >= 1920 && y <= 2039) ? y : 0;
}

/* After an episode number: "e03", "-e03", "-03", "-s01e03", "+e03", "&e03" (double episodes). */
static void parse_more(const char **pp, int *end) {
    const char *p = *pp;
    for (;;) {
        const char *q = p;
        if (*q == '-' || *q == '+' || *q == '&') q++;
        if (*q == 's') {
            q++;
            if (digits_at(&q, 4) < 0 || *q != 'e') break;
        }
        if (*q == 'e') q++;
        else if (q == p) break;
        int v = digits_at(&q, 4);
        if (v < 0 || v <= *end || v > *end + 20) break;
        *end = v;
        p = q;
    }
    *pp = p;
}

static bool tail_ok(const char *p) {
    return !*p || !isalnum((unsigned char)*p) || (*p == 'v' && isdigit((unsigned char)p[1]));
}

/* s01e02, s1e2, s01e02e03, s01e02-e03, s01e02-03, s2024e05 */
static bool match_sxe(const char *lo, int *season, int *ep, int *ep_end) {
    const char *p = lo;
    if (*p++ != 's') return false;
    int s = digits_at(&p, 4);
    if (s < 0 || *p++ != 'e') return false;
    int e = digits_at(&p, 4);
    if (e < 0) return false;
    int end = e;
    parse_more(&p, &end);
    if (!tail_ok(p)) return false;
    *season = s, *ep = e, *ep_end = end;
    return true;
}

static bool match_s_only(const char *lo, int *season) {
    const char *p = lo;
    if (*p++ != 's') return false;
    int s = digits_at(&p, 4);
    if (s < 0 || *p) return false;
    *season = s;
    return true;
}

/* e05, ep05, -e05, e05e06 */
static bool match_e_only(const char *lo, int *ep, int *ep_end) {
    const char *p = lo;
    if (*p == '-') p++;
    if (*p++ != 'e') return false;
    if (*p == 'p') p++;
    int e = digits_at(&p, 4);
    if (e < 0) return false;
    int end = e;
    parse_more(&p, &end);
    if (!tail_ok(p)) return false;
    *ep = e, *ep_end = end;
    return true;
}

/* 1x02, 01x02, 1x02-03, 1x02-1x03 (but never 1920x1080) */
static bool match_nxn(const char *lo, int *season, int *ep, int *ep_end) {
    const char *p = lo;
    int s = digits_at(&p, 2);
    if (s < 0 || *p++ != 'x') return false;
    const char *q = p;
    int e = digits_at(&q, 3);
    if (e < 0 || q - p < 2) return false;
    p = q;
    int end = e;
    if (*p == '-') {
        const char *r = p + 1;
        int v = digits_at(&r, 3);
        if (v >= 0 && *r == 'x') {
            r++;
            v = digits_at(&r, 3);
        }
        if (v > end && v <= end + 20) {
            end = v;
            p = r;
        }
    }
    if (!tail_ok(p)) return false;
    *season = s, *ep = e, *ep_end = end;
    return true;
}

static bool is_season_tok(const Toks *T, int i) {
    int s, e, ee;
    const char *lo = T->t[i].lo;
    if (match_s_only(lo, &s) || match_sxe(lo, &s, &e, &ee)) return true;
    return in_list(lo, SEASON_WORDS) && i + 1 < T->n && is_num(T->t[i + 1].lo, 1, 4, &s);
}

static void set_match(EpMatch *m, int start, int end, int season, int ep, int ep_end) {
    m->start = start, m->end = end, m->season = season, m->ep = ep, m->ep_end = ep_end;
}

static bool joiner(const char *lo) { return is_dash(lo) || !strcmp(lo, "&") || !strcmp(lo, "+") || !strcmp(lo, "and") || !strcmp(lo, "och"); }

/* More episodes in the same file after a marker: "S01E01.E02", "S01E01 & E02", "S01E01 + S01E02",
   "1x01 and 1x02" - the next numbers in a row only (a later "- E05 Title" is something else). */
static void more_episodes(const Toks *T, EpMatch *m) {
    for (;;) {
        int k = m->end, s, e, ee;
        bool joined = k < T->n && joiner(T->t[k].lo);
        if (joined) k++;
        if (k >= T->n) return;
        const char *lo = T->t[k].lo;
        bool ok = match_e_only(lo, &e, &ee) || ((match_sxe(lo, &s, &e, &ee) || match_nxn(lo, &s, &e, &ee)) && s == m->season);
        /* after "&", "-" ...: the very next episode ("S01E04 - E10 Title" is a title); glued on ("S01E01.E02"): close by */
        if (!ok || (joined ? e != m->ep_end + 1 : e <= m->ep_end || e > m->ep_end + 20)) return;
        m->ep_end = ee;
        m->end = k + 1;
    }
}

/* Markers that need no context: SxxEyy, S01 E02, 1x02, "Season 1 Episode 2". */
static bool find_explicit(const Toks *T, EpMatch *m) {
    for (int i = 0; i < T->n; i++) {
        const char *lo = T->t[i].lo;
        int s, e, ee;
        if (match_sxe(lo, &s, &e, &ee)) {
            set_match(m, i, i + 1, s, e, ee);
            more_episodes(T, m);
            return true;
        }
        if (match_s_only(lo, &s) && i + 1 < T->n && match_e_only(T->t[i + 1].lo, &e, &ee)) {
            set_match(m, i, i + 2, s, e, ee);
            more_episodes(T, m);
            return true;
        }
        if (match_nxn(lo, &s, &e, &ee)) {
            set_match(m, i, i + 1, s, e, ee);
            more_episodes(T, m);
            return true;
        }
        if (in_list(lo, SEASON_WORDS) && i + 1 < T->n && is_num(T->t[i + 1].lo, 1, 4, &s)) {
            int k = i + 2;
            if (k < T->n && is_dash(T->t[k].lo)) k++;
            if (k + 1 < T->n && in_list(T->t[k].lo, EPISODE_WORDS) && is_num(T->t[k + 1].lo, 1, 4, &e)) {
                set_match(m, i, k + 2, s, e, e);
                return true;
            }
            if (k < T->n && match_e_only(T->t[k].lo, &e, &ee)) {
                set_match(m, i, k + 1, s, e, ee);
                return true;
            }
        }
    }
    return false;
}

/* "01-02" (two episodes in one file, no letters): the second just after the first */
static bool is_num_range(const char *lo, int *e, int *ee) {
    const char *p = lo;
    int a = digits_at(&p, 3);
    if (a < 0 || (*p != '-' && *p != '+' && *p != '&')) return false;
    p++;
    int b = digits_at(&p, 3);
    if (b <= a || b > a + 20 || *p) return false;
    *e = a, *ee = b;
    return true;
}

/* Markers that only mean "episode" inside a show/season folder or an anime
   release: "E05", "Episode 5", "Avsnitt 5", "05 - Title", "Show - 05", "01-02 - Title". */
static bool find_contextual(const Toks *T, EpMatch *m) {
    int first = 0;
    while (first < T->n && T->t[first].grp == 2) first++;
    int e, ee;
    for (int i = first; i < T->n; i++) {
        const char *lo = T->t[i].lo;
        if (T->t[i].grp == 2) continue;
        if (match_e_only(lo, &e, &ee)) {
            set_match(m, i, i + 1, -1, e, ee);
            return true;
        }
        if ((in_list(lo, EPISODE_WORDS) || !strcmp(lo, "del")) && i + 1 < T->n && is_num_v(T->t[i + 1].lo, 4, &e)) {
            set_match(m, i, i + 2, -1, e, e);
            return true;
        }
    }
    if (first < T->n && (is_num_v(T->t[first].lo, 3, &e) ? (ee = e, true) : is_num_range(T->t[first].lo, &e, &ee))) {
        set_match(m, first, first + 1, -1, e, ee);
        return true;
    }
    for (int i = first + 1; i + 1 < T->n; i++)
        if (is_dash(T->t[i].lo) && T->t[i + 1].grp == 0 &&
            (is_num_v(T->t[i + 1].lo, 4, &e) ? (ee = e, true) : is_num_range(T->t[i + 1].lo, &e, &ee))) {
            set_match(m, i, i + 2, -1, e, ee);
            return true;
        }
    return false;
}

/* ----------------------------------------------------------- capitalising */

static bool is_up(const unsigned char *p) {
    return (*p >= 'A' && *p <= 'Z') || (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97);
}

static bool is_low(const unsigned char *p) {
    return (*p >= 'a' && *p <= 'z') || (p[0] == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7);
}

static void upper1(unsigned char *p) {
    if (*p >= 'a' && *p <= 'z') *p = (unsigned char)(*p - 32);
    else if (p[0] == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) p[1] = (unsigned char)(p[1] - 0x20);
}

static void title_case(char *s) {
    fold_copy(s, strlen(s) + 1, s); /* lower everything first (length preserving) */
    bool first = true;
    for (char *w = s; *w;) {
        while (*w == ' ') w++;
        if (!*w) break;
        char *e = w;
        while (*e && *e != ' ') e++;
        char word[TOK_LEN];
        size_t n = (size_t)(e - w);
        if (n >= sizeof word) n = sizeof word - 1;
        memcpy(word, w, n);
        word[n] = 0;
        char *core = word;
        while (*core == '(') core++;
        size_t cl = strlen(core);
        while (cl && strchr("),:;!?", core[cl - 1])) core[--cl] = 0;
        bool last = !e[strspn(e, " ")];
        if (in_list(core, UPPER_WORDS)) {
            for (char *p = w; p < e; p++)
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        } else if (first || last || !in_list(core, SMALL_WORDS)) {
            unsigned char *p = (unsigned char *)w, *end = (unsigned char *)e;
            while (p < end && !is_low(p) && !is_up(p) && !isdigit(*p)) p++;
            if (p < end) upper1(p);
            for (unsigned char *q = (unsigned char *)w; q + 1 < end; q++)
                if (*q == '-' && is_low(q + 1)) upper1(q + 1); /* Spider-Man */
        }
        first = false;
        w = e;
    }
}

/* Leave sensible capitalisation alone; fix all-lowercase and SHOUTING names. */
static void fix_case(char *s) {
    int up = 0, low = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (is_up(p)) up++;
        else if (is_low(p)) low++;
        if (*p == 0xC3 && p[1]) p++;
    }
    if (up + low == 0) return;
    if (up == 0 || (low == 0 && up >= 6)) title_case(s);
}

/* -------------------------------------------------------------- cleaning */

static void build(const Toks *T, int a, int b, char *out, size_t cap) {
    while (b > a && (is_dash(T->t[b - 1].lo) || T->t[b - 1].grp == 2)) b--;
    Buf o = {0};
    for (int i = a; i < b; i++) {
        const Tok *t = &T->t[i];
        if (t->grp == 2) continue;
        if (is_single_letter(t) && i + 1 < b && is_single_letter(&T->t[i + 1])) { /* S.H.I.E.L.D. */
            if (o.len) buf_putc(&o, ' ');
            int j = i;
            while (j < b && is_single_letter(&T->t[j])) {
                buf_puts(&o, T->t[j].s);
                buf_putc(&o, '.');
                j++;
            }
            i = j - 1;
            continue;
        }
        bool open = t->grp == 1 && (i == a || T->t[i - 1].grp != 1 || T->t[i - 1].gid != t->gid);
        bool close = t->grp == 1 && (i + 1 >= b || T->t[i + 1].grp != 1 || T->t[i + 1].gid != t->gid);
        bool glue = o.len && o.p[o.len - 1] == '.' && (t->s[0] == '\'' || !strncmp(t->s, "\xE2\x80\x99", 3)); /* D.W.'s */
        if (o.len && !glue) buf_putc(&o, ' ');
        if (open) buf_putc(&o, '(');
        buf_puts(&o, t->s);
        if (close) buf_putc(&o, ')');
    }
    str_copy(out, cap, o.len ? buf_cstr(&o) : "");
    buf_free(&o);
    fix_case(out);
}

/* Title from tokens [a, b): skip [group]/spam prefixes, cut at the first
   tag, bracket or season marker, then at the last plausible year (one with
   at least one title word before it: "1917 (2019)" -> "1917", 2019).
   `ep_part`: the text after an episode marker, where a leading tag-like
   word ("PROPER", "WEB", "SWEDISH") is a tag rather than a title word. */
static void clean_title_ex(const Toks *T, int a, int b, Clean *c, bool ep_part) {
    memset(c, 0, sizeof *c);
    if (b > T->n) b = T->n;
    if (a < b && !strcmp(T->t[a].lo, "www")) { /* "www.site.org - Title" */
        int j = a;
        while (j < b && !is_dash(T->t[j].lo)) j++;
        if (j < b) a = j + 1;
    }
    while (a < b && (T->t[a].grp == 2 || is_dash(T->t[a].lo))) a++;
    int cut = b;
    for (int i = a; i < b; i++) {
        const Tok *t = &T->t[i];
        if (!ep_part && i == a && in_list(t->lo, WORDLIKE_TAGS)) continue;
        bool tag = t->grp == 2 || is_tag(t->lo) || is_season_tok(T, i) || is_lang_tag(T, a, i, ep_part) ||
                   (ep_part && !strcmp(t->lo, "web"));
        if (tag) {
            cut = i;
            c->tags = true;
            break;
        }
    }
    int yi = -1;
    for (int i = a + 1; i < cut; i++)
        if (tok_year(&T->t[i])) yi = i;
    build(T, a, cut, c->full, sizeof c->full);
    if (yi > a) {
        c->year = tok_year(&T->t[yi]);
        c->year_paren = T->t[yi].grp != 0;
        build(T, a, yi, c->title, sizeof c->title);
    } else {
        str_copy(c->title, sizeof c->title, c->full);
        for (int i = cut; i < b && !c->year && cut > a; i++) /* "Title.LIMITED.2016.1080p" */
            if (T->t[i].grp != 2 && tok_year(&T->t[i])) {
                c->year = tok_year(&T->t[i]);
                c->year_paren = T->t[i].grp != 0;
            }
    }
}

static void clean_title(const Toks *T, int a, int b, Clean *c) { clean_title_ex(T, a, b, c, false); }

static void clean_text(const char *s, Clean *c) {
    Toks *T = xmalloc(sizeof *T);
    tokenize(s, T);
    clean_title(T, 0, T->n, c);
    free(T);
}

/* ----------------------------------------------------------------- folders */

static const char *const DISC_WORDS[] = {"disc", "disk", "dvd", "cd", "skiva", NULL};

/* "disc 2", "disc - 2", "disc2", "dvd 1" at the start of lo (folded): the number, else -1 */
static int disc_marker(const char *lo) {
    for (int i = 0; DISC_WORDS[i]; i++) {
        size_t k = strlen(DISC_WORDS[i]);
        if (strncmp(lo, DISC_WORDS[i], k) != 0) continue;
        const char *p = lo + k;
        while (*p == ' ' || *p == '-' || *p == '_' || *p == '.') p++;
        int v = digits_at(&p, 2);
        if (v >= 0 && (!*p || !isalnum((unsigned char)*p))) return v;
    }
    return -1;
}

/* "Disc - 1", "Disc 1 of 4", "Disk2", "DVD 2", or just "D1" */
static int disc_dir(const char *name) {
    char lo[128];
    fold_copy(lo, sizeof lo, name);
    str_trim(lo);
    int v = disc_marker(lo);
    if (v >= 0) return v;
    if (lo[0] == 'd' && isdigit((unsigned char)lo[1])) {
        const char *p = lo + 1;
        v = digits_at(&p, 2);
        if (v >= 0 && !*p) return v;
    }
    return -1;
}

/* A season folder: "Season 2", "Season01", "S01", "Säsong 3", "Specials" (0), and names that carry
   the season among other words: "season 1 70's show", "That.70s.Show.S01.DVDRip", "Season 1 Disc 2".
   Returns the season or -1. `rest` (optional) gets the other words cleaned like a title
   ("70's Show", "That 70s Show"; "" when nothing title-like is left); `disc` a disc in the same name. */
static int season_dir_ex(const char *name, char *rest, size_t cap, int *disc) {
    if (rest && cap) rest[0] = '\0';
    if (disc) *disc = 0;
    char lo[128];
    fold_copy(lo, sizeof lo, name);
    str_trim(lo);
    if (!strcmp(lo, "specials") || !strcmp(lo, "special")) return 0;
    Toks *T = xmalloc(sizeof *T);
    tokenize(name, T);
    int season = -1, a = 0, b = 0, d = 0;
    for (int i = 0; i < T->n && season < 0; i++) {
        const char *t = T->t[i].lo;
        int v;
        /* "season 1" (a later "season 2024" in a longer name is more likely a year) */
        if (in_list(t, SEASON_WORDS) && i + 1 < T->n && is_num(T->t[i + 1].lo, 1, i == 0 ? 4 : 2, &v)) {
            season = v, a = i, b = i + 2;
            break;
        }
        for (int w = 0; SEASON_WORDS[w] && season < 0; w++) { /* "season01" */
            size_t k = strlen(SEASON_WORDS[w]);
            if (!strncmp(t, SEASON_WORDS[w], k) && is_num(t + k, 1, 4, &v)) season = v, a = i, b = i + 1;
        }
        if (season >= 0) break;
        const char *p = t;
        if (*p++ == 's' && (v = digits_at(&p, T->n == 1 ? 4 : 2)) >= 0) {
            if (!*p) season = v, a = i, b = i + 1; /* "S01" */
            else if (*p == 'd' && isdigit((unsigned char)p[1])) { /* "S01D02" */
                p++;
                int dv = digits_at(&p, 2);
                if (dv >= 0 && !*p) season = v, d = dv, a = i, b = i + 1;
            }
        }
    }
    if (season < 0) {
        free(T);
        return -1;
    }
    /* what else the name says: a disc ("Season 1 Disc 2"), and maybe the show's name */
    Toks *R = xmalloc(sizeof *R);
    R->n = 0;
    R->lead_group = false;
    for (int i = 0; i < T->n; i++) {
        if (i >= a && i < b) continue;
        const char *t = T->t[i].lo;
        int dv = disc_marker(t); /* "disc2" */
        int j = i + 1;
        if (j < T->n && is_dash(T->t[j].lo)) j++;
        if (dv < 0 && in_list(t, DISC_WORDS) && j < T->n && is_num(T->t[j].lo, 1, 2, &dv)) i = j; /* "disc - 2" */
        else if (dv < 0 && t[0] == 'd') is_num(t + 1, 1, 2, &dv);                                 /* "D2" */
        if (dv < 0) {
            R->t[R->n++] = T->t[i];
            continue;
        }
        if (!d) d = dv;
    }
    if (rest && cap) {
        Clean c;
        clean_title_ex(R, 0, R->n, &c, true); /* "Complete", "DVDRip" ... are tags here */
        bool letters = false;
        for (const unsigned char *p = (const unsigned char *)c.title; *p; p++) letters |= isalpha(*p) || *p >= 0x80;
        if (letters) str_copy(rest, cap, c.title);
    }
    if (disc) *disc = d;
    free(R);
    free(T);
    return season;
}

int media_season_folder(const char *name) { return name ? season_dir_ex(name, NULL, 0, NULL) : -1; }
int media_disc_folder(const char *name) { return name ? disc_dir(name) : -1; }

static int generic_dir(const char *name) {
    char lo[128];
    fold_copy(lo, sizeof lo, name);
    for (char *p = lo; *p; p++)
        if (*p == '_' || *p == '.' || *p == '-') *p = ' ';
    str_trim(lo);
    if (in_list(lo, DIR_SERIES)) return GEN_SERIES;
    if (in_list(lo, DIR_MOVIES)) return GEN_MOVIES;
    if (in_list(lo, DIR_HOME)) return GEN_HOME;
    if (in_list(lo, DIR_NEUTRAL)) return GEN_NEUTRAL;
    if (in_list(lo, DIR_EXTRAS)) return GEN_EXTRAS;
    return GEN_NONE;
}

/* ---------------------------------------------------------------- files */

static void strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    if (!dot || dot == s) return;
    size_t n = strlen(dot + 1);
    if (n < 1 || n > 5 || all_digits(dot + 1, n)) return;
    for (const char *p = dot + 1; *p; p++)
        if (!isalnum((unsigned char)*p)) return;
    *dot = 0;
}

static bool extra_name(const char *base) {
    static const char *const SUF[] = {"trailer", "sample",  "featurette", "behindthescenes", "deleted",
                                      "deletedscene", "interview", "scene", "short", "extra", "teaser", NULL};
    char lo[256];
    fold_copy(lo, sizeof lo, base);
    size_t n = strlen(lo);
    for (int i = 0; SUF[i]; i++) {
        size_t k = strlen(SUF[i]);
        if (n == k && !strcmp(lo, SUF[i])) return true;
        if (n > k && !strcmp(lo + n - k, SUF[i]) && strchr("-._ ", lo[n - k - 1])) return true;
    }
    return str_starts(lo, "sample") && strchr("-._ ", lo[6]) && lo[6];
}

/* MakeMKV names its files "title_t00", "B1_t03", "<VOLUME_LABEL>_t07" or "title00": the title
   number (the disc's own order), else -1. */
static int makemkv_title(const char *base) {
    char lo[256];
    fold_copy(lo, sizeof lo, base);
    str_trim(lo);
    size_t n = strlen(lo);
    for (size_t k = 2; k <= 3; k++)
        if (n >= k + 3 && lo[n - k - 2] == '_' && lo[n - k - 1] == 't' && all_digits(lo + n - k, k))
            return atoi(lo + n - k);
    if (!strncmp(lo, "title", 5)) {
        const char *p = lo + 5;
        while (*p == '_' || *p == ' ' || *p == '-') p++;
        if (*p == 't') p++;
        size_t k = strlen(p);
        if (k >= 1 && k <= 3 && all_digits(p, k)) return atoi(p);
    }
    return -1;
}

static bool junk_name(const char *base, const Clean *c) {
    static const char *const J[] = {"movie", "film",    "video", "main", "feature", "title", "untitled",
                                    "default", "index", "file",  "output", "final", NULL};
    if (!c->full[0]) return true;
    char lo[256];
    fold_copy(lo, sizeof lo, base);
    str_trim(lo);
    if (in_list(lo, J)) return true;
    if (makemkv_title(base) >= 0) return true; /* title_t00, title00, B1_t00 */
    bool numeric = true, hex = true;
    size_t n = 0;
    for (const char *p = lo; *p; p++, n++) {
        if (!isdigit((unsigned char)*p) && !strchr(" _-.", *p)) numeric = false;
        if (!isxdigit((unsigned char)*p)) hex = false;
    }
    return numeric || (hex && n >= 16);
}

/* "The Office (US)" vs "The Office US": same show once reduced to letters and digits. */
static bool same_show(const char *a, const char *b) {
    char x[128], y[128];
    size_t nx = 0, ny = 0;
    char fa[128], fb[128];
    fold_copy(fa, sizeof fa, a);
    fold_copy(fb, sizeof fb, b);
    for (const unsigned char *p = (const unsigned char *)fa; *p && nx < sizeof x - 1; p++)
        if (isalnum(*p) || *p >= 0x80) x[nx++] = (char)*p;
    for (const unsigned char *p = (const unsigned char *)fb; *p && ny < sizeof y - 1; p++)
        if (isalnum(*p) || *p >= 0x80) y[ny++] = (char)*p;
    x[nx] = y[ny] = 0;
    if (nx < 2 || ny < 2) return false;
    return strstr(x, y) || strstr(y, x);
}

static bool good_ep_title(const char *s, const char *series) {
    if (!s[0] || str_ieq(s, series)) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (isalpha(*p) || *p >= 0x80) return true;
    return false;
}

/* A folder named after one release ("The.Office.S02E05.720p-GRP") never names the show of other files. */
static bool looks_like_release(const char *name) {
    Toks *T = xmalloc(sizeof *T);
    tokenize(name, T);
    EpMatch m;
    bool r = find_explicit(T, &m);
    free(T);
    return r;
}

/* ------------------------------------------------------------------- API */

int media_name_part(const char *rel) {
    static const char *const words[] = {"cd", "disc", "disk", "dvd", "part", "pt", "del", NULL};
    static const char *const counts[] = {"one", "two", "three", "four", "five", "six", NULL};
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    char lo[256];
    size_t n = 0;
    for (const char *p = base; *p && n + 1 < sizeof lo; p++) lo[n++] = (char)tolower((unsigned char)*p);
    lo[n] = '\0';
    char *dot = strrchr(lo, '.');
    if (dot && dot != lo) *dot = '\0'; /* ".mkv"; a ".web" left over is just another word */
    int want = 0;                     /* the previous word was "part" (or "cd", ...): a number may follow */
    for (char *p = lo; *p;) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        char *w = p;
        while (isalnum((unsigned char)*p)) p++;
        if (w == p) break;
        char word[24];
        str_ncopy(word, sizeof word, w, (size_t)(p - w));
        char *digits = word;
        while (*digits && !isdigit((unsigned char)*digits)) digits++;
        int num = *digits && strlen(digits) <= 2 && strspn(digits, "0123456789") == strlen(digits) ? atoi(digits) : 0;
        if (want && digits == word && num > 0) return num; /* "part 2" */
        if (want)
            for (int k = 0; counts[k]; k++)
                if (!strcmp(word, counts[k])) return k + 1; /* "part two" */
        *digits = '\0';
        want = in_list(word, words);
        if (want && num > 0) return num; /* "cd2", "pt1" */
    }
    return 0;
}

void media_parse_name(const char *rel, MediaName *out) { media_parse_name_in(NULL, rel, out); }

void media_parse_name_in(const char *root_name, const char *rel, MediaName *out) {
    memset(out, 0, sizeof *out);
    out->track = -1;
    char *path = xstrdup(rel ? rel : "");
    for (char *p = path; *p; p++)
        if (*p == '\\') *p = '/';

    char *parts[48];
    int np = 0;
    for (char *seg = path;;) {
        char *slash = strchr(seg, '/');
        if (slash) *slash = 0;
        if (*seg) {
            if (np == countof(parts)) { /* keep the deepest components */
                memmove(parts, parts + 1, sizeof parts - sizeof parts[0]);
                np--;
            }
            parts[np++] = seg;
        }
        if (!slash) break;
        seg = slash + 1;
    }
    if (np == 0) {
        str_copy(out->title, sizeof out->title, "Untitled");
        free(path);
        return;
    }

    char base[512];
    str_copy(base, sizeof base, parts[np - 1]);
    strip_ext(base);
    int nd = np - 1; /* directories */

    bool series_hint = false, movie_hint = false, home_hint = false, extra = extra_name(base);
    for (int i = 0; i < nd; i++) {
        int g = generic_dir(parts[i]);
        if (g == GEN_SERIES) series_hint = true;
        else if (g == GEN_MOVIES) movie_hint = true;
        else if (g == GEN_HOME) home_hint = true;
        else if (g == GEN_EXTRAS) extra = true;
    }
    int track = makemkv_title(base);

    /* Walk up from the file over extras, disc and season folders ("Movie (2010)/Extras/x.mkv",
       "Show/Season 1/Disc - 2/B3_t03.mkv"): the next folder names the show or movie. Above the
       first folder of `rel` comes the root folder's own name. */
    int sdir = -1, disc = 0;
    char srest[128] = ""; /* the show, as a season folder may name it: "season 1 70's show" */
    const char *show = NULL;
    for (int i = nd - 1; i >= -1; i--) {
        const char *dname = i >= 0 ? parts[i] : root_name;
        if (!dname || !*dname) break;
        if (generic_dir(dname) == GEN_EXTRAS) {
            if (i < 0) break;
            continue;
        }
        int d = disc_dir(dname);
        if (d >= 0) {
            if (!disc) disc = d;
            continue;
        }
        int d2 = 0, s = season_dir_ex(dname, sdir < 0 ? srest : NULL, sizeof srest, &d2);
        if (s >= 0 && (sdir < 0 || s == sdir)) { /* also "Season 1/Season 1 Disc 2/" */
            sdir = s;
            if (!disc) disc = d2;
            continue;
        }
        show = dname;
        break;
    }
    if (show && (generic_dir(show) != GEN_NONE || looks_like_release(show))) show = NULL;

    Toks *T = xmalloc(sizeof *T), *D = NULL;
    tokenize(base, T);
    bool ctx = sdir >= 0 || series_hint || T->lead_group;
    EpMatch m;
    const Toks *src = T;
    /* MakeMKV's "E1_t02" is title 2, not episode 1 */
    bool episode = !extra && (find_explicit(T, &m) || (ctx && track < 0 && find_contextual(T, &m)));
    if (!episode && !extra && nd >= 1) { /* "The.Office.S02E05.720p-GRP/abc123.mkv" */
        D = xmalloc(sizeof *D);
        tokenize(parts[nd - 1], D);
        if (find_explicit(D, &m)) {
            episode = true;
            src = D;
        }
    }
    /* No number of its own, but in a season folder (or a disc of a series): an episode all the
       same — the library numbers these in disc and title order. */
    bool unnumbered = !episode && !extra && (sdir >= 0 || (disc && series_hint));

    Clean cd = {0};
    if (show) clean_text(show, &cd);

    if (episode || unnumbered) {
        out->kind = MEDIA_EPISODE;
        Clean cf = {0}, ce;
        if (episode) {
            out->season = m.season >= 0 ? m.season : (sdir >= 0 ? sdir : 1);
            out->episode = m.ep;
            out->episode_end = m.ep_end >= m.ep ? m.ep_end : m.ep;
            clean_title(src, 0, m.start, &cf);
        } else {
            out->unnumbered = true;
            out->season = sdir >= 0 ? sdir : 1;
        }
        /* Trust the show folder in structured libraries (season folders) or when it agrees with
           the file; a random download folder must not rename the show, and neither may a folder
           above a season folder that names another show ("Stuff/That.70s.Show.S01.DVDRip/"). */
        bool use_dir = cd.title[0] && (sdir >= 0 || !cf.title[0] || same_show(cd.title, cf.title)) &&
                       (!srest[0] || same_show(cd.title, srest));
        const char *t = use_dir ? cd.title : cf.title[0] ? cf.title : srest[0] ? srest : "Unknown series";
        str_copy(out->title, sizeof out->title, t);
        out->year = use_dir && cd.year ? cd.year : cf.year;
        if (episode) {
            clean_title_ex(src, m.end, src->n, &ce, true);
            if (good_ep_title(ce.full, out->title)) str_copy(out->ep_title, sizeof out->ep_title, ce.full);
        } else {
            clean_title(T, 0, T->n, &ce); /* "Season 1/Hospital.mp4": the name is the episode's */
            if (!junk_name(base, &ce) && !same_show(ce.full, out->title) && good_ep_title(ce.full, out->title))
                str_copy(out->ep_title, sizeof out->ep_title, ce.full);
        }
    } else {
        Clean cf;
        clean_title(T, 0, T->n, &cf);
        bool junk = junk_name(base, &cf);
        bool dir_movie = cd.year && cd.title[0] && (cd.year_paren || cd.tags) && !home_hint;
        out->extra = extra;
        if (extra) {
            out->kind = MEDIA_OTHER;
            str_copy(out->title, sizeof out->title, cf.full[0] ? cf.full : base);
            out->year = cf.year ? cf.year : cd.year;
        } else if (cf.year && cf.title[0] && (cf.year_paren || cf.tags || movie_hint || !home_hint)) {
            out->kind = MEDIA_MOVIE;
            str_copy(out->title, sizeof out->title, cf.title);
            out->year = cf.year;
        } else if (dir_movie) {
            out->kind = MEDIA_MOVIE;
            str_copy(out->title, sizeof out->title, cd.title);
            out->year = cd.year;
        } else if (junk && cd.full[0]) {
            bool movie = (movie_hint || cd.tags) && !home_hint;
            out->kind = movie ? MEDIA_MOVIE : MEDIA_OTHER;
            str_copy(out->title, sizeof out->title, movie ? cd.title : cd.full);
            out->year = cd.year;
        } else if (cf.title[0] && !home_hint && (cf.tags || movie_hint)) {
            out->kind = MEDIA_MOVIE;
            str_copy(out->title, sizeof out->title, cf.title);
        } else {
            out->kind = MEDIA_OTHER;
            str_copy(out->title, sizeof out->title, cf.full[0] ? cf.full : base);
            out->year = cf.year;
        }
        if (!out->title[0]) str_copy(out->title, sizeof out->title, "Untitled");
    }
    out->extra = extra;
    out->disc = disc;
    out->track = track;
    free(T);
    free(D);
    free(path);
}
