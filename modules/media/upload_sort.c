/* modules/media/upload_sort.c — where an upload's files go (owner, 2026-10-21: "Hemma sorts uploads by
 * their names"), for the page's preview and for the plan alike (upload.c), so the two always agree.
 *
 *   an episode  -> <series folder>/<Show>/<season folder>/<its name>
 *   a disc rip  -> <series folder>/<Show>/<its path below its own show folder>: the library numbers
 *                  those by their disc and title folders ("Season 1/Disc - 2/B3_t03.mkv"), so they stay
 *   a film      -> <movies folder>/<Title (Year)>/<its name>
 *   an audiobook's file (1.6: an .m4b anywhere; an .m4a or .mp3 anywhere but in a show's or a film's own
 *               folder - an admin who uploads one means an audiobook)
 *               -> <books folder>/<its path below a books folder among its folders>
 *                  ("Ljudböcker/Astrid/Book/01.mp3" -> "Astrid/Book/01.mp3"), else <books folder>/<its path>
 *                  as it was chosen ("Book/CD1/01.mp3", a loose "Book.m4b"); ?series= never takes it
 *   subtitles and pictures go with their video (the same name, in the same folder) - a DVD VobSub's .sub
 *   (only with its .idx among the chosen files) wherever its .idx goes - and pictures with their
 *   audiobook's file ("Book.jpg" with "Book.m4b"); whatever else sits in a show's or a film's own folder
 *   (a poster.jpg, Extras/, Subs/, a theme.mp3, Soundtrack/ - the library leaves audio there alone) goes
 *   along into that folder, and the pictures and subtitles in an audiobook's own folder (above its CD1/,
 *   Disc 2/: a cover.jpg) into its folder - not one inside a show's or a film's, which keeps what's there;
 *   the rest stays as it was chosen.
 *
 * <series folder> is the library folder's own "Series" / "Serier" / "TV Shows" / "TV" / "Anime" (any
 * case), else "Series" - nothing when the library folder itself is one; <movies folder> likewise
 * "Movies" / "Films" / "Filmer"; <books folder> its own "Audiobooks" / "Ljudböcker" / "Audiolibros" ...
 * (media_is_books_folder), else "Audiobooks". A show the library has (by its folded name) goes into ITS
 * folder, and a season into its season folder there; a film it has, into that film's folder. File names
 * never change: a copy (x.web.mp4), its original and their subtitles keep finding each other. */
#include <ctype.h>

#include "remove.h" /* media_names_show */
#include "upload.h"

#define MAPS_MAX 256
#define CACHE_MAX 64

static const char *const SERIES_DIRS[] = {"series", "serier", "tv shows", "tv", "anime", NULL};
static const char *const MOVIE_DIRS[] = {"movies", "films", "filmer", NULL};
/* folders that hold a show's or film's extras: walked over, like season and disc folders */
static const char *const EXTRA_DIRS[] = {"extras",     "extra",   "featurettes", "featurette", "behind the scenes",
                                         "deleted scenes", "interviews", "trailers", "trailer", "samples",
                                         "sample",     "bonus",   "scenes",      "shorts",     NULL};

/* A folder of the chosen files that names a show or a film (or is an audiobook's), and where its contents go. */
typedef struct {
    char src[400], dst[400];
    char title[128];
    bool clash; /* two shows claim it (a "Season 1" folder with two shows' episodes): nothing follows it */
    bool book;  /* an audiobook's own folder: only pictures and subtitles follow it, never a video */
} DirMap;

typedef struct {
    uint64_t key;
    int year;
    char dest[400], title[128];
    bool known;
} Place;

typedef struct {
    char show[400];
    int season;
    char dest[400];
} SeasonPlace;

typedef struct {
    const UpSortOpts *o;
    const MediaIndex *ix;
    int root_ix; /* the library folder in the index (-1: not scanned yet - nothing to find there) */
    char series_dir[256], movies_dir[256], books_dir[256];
    bool series_ok; /* ?series= names a series of this library folder */
    char series_show[400], series_title[128];
    DirMap maps[MAPS_MAX];
    int nmaps;
    Place shows[CACHE_MAX], films[CACHE_MAX];
    int nshows, nfilms;
    SeasonPlace seasons[CACHE_MAX];
    int nseasons;
} Sorter;

/* ------------------------------------------------------------------ names */

static void cat(char *out, size_t cap, const char *a, const char *b) { /* "a/b", either may be "" */
    int k = !*a ? snprintf(out, cap, "%s", b) : !*b ? snprintf(out, cap, "%s", a) : snprintf(out, cap, "%s/%s", a, b);
    if (k < 0 || (size_t)k >= cap) out[utf8_clip(out, cap - 1)] = '\0'; /* (too long: the plan refuses it) */
}

static int rank_in(const char *name, const char *const *list) { /* list position of a folder's name, -1 = none */
    char lo[256];
    fold_copy(lo, sizeof lo, name);
    for (char *p = lo; *p; p++)
        if (*p == '_' || *p == '.' || *p == '-') *p = ' ';
    str_trim(lo);
    for (int i = 0; list[i]; i++)
        if (str_eq(lo, list[i])) return i;
    return -1;
}

uint64_t upload_show_key(const char *name) { /* (the same as the library's series ids: library.c series_id) */
    char k[160];
    fold_copy(k, sizeof k, name);
    uint64_t h = hash64_str(k);
    return h ? h : 1;
}

bool upload_safe_name(const char *in, char *out, size_t cap) {
    char tmp[300];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 3 < sizeof tmp; p++) {
        unsigned char ch = *p;
        if (ch == ':') { /* "Star Wars: A New Hope" -> "Star Wars - A New Hope" */
            tmp[n++] = ' ';
            tmp[n++] = '-';
            ch = ' ';
        } else if (ch < 0x20 || ch == 0x7F || strchr("\\/*?\"<>|", ch)) {
            ch = ' ';
        }
        if (ch == ' ' && (!n || tmp[n - 1] == ' ')) continue; /* one space at a time, none first */
        if (ch == '.' && !n) continue; /* (a dot first hides a folder: the library's scan passes those by) */
        tmp[n++] = (char)ch;
    }
    tmp[n] = '\0';
    while (n && (tmp[n - 1] == ' ' || tmp[n - 1] == '.')) tmp[--n] = '\0'; /* Windows drops these */
    if (n > 120) tmp[utf8_clip(tmp, 120)] = '\0';
    n = strlen(tmp);
    while (n && (tmp[n - 1] == ' ' || tmp[n - 1] == '.')) tmp[--n] = '\0';
    if (!n || n >= cap || !utf8_valid(tmp, n)) return false;
    memcpy(out, tmp, n + 1);
    return true;
}

/* Letters and digits only, folded: "The Office (US)" and "The.Office.US" read alike. */
static void squash(const char *s, char *out, size_t cap) {
    char f[256];
    fold_copy(f, sizeof f, s);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)f; *p && n + 1 < cap; p++)
        if (isalnum(*p) || *p >= 0x80) out[n++] = (char)*p;
    out[n] = '\0';
}

static bool same_title(const char *a, const char *b) {
    char x[256], y[256];
    squash(a, x, sizeof x);
    squash(b, y, sizeof y);
    return strlen(x) >= 2 && strlen(y) >= 2 && (strstr(x, y) || strstr(y, x));
}

static bool fold_eq(const char *a, const char *b) {
    char x[160], y[160];
    fold_copy(x, sizeof x, a);
    fold_copy(y, sizeof y, b);
    return str_eq(x, y);
}

/* Does this folder of the chosen files name that show (or film)? As the library would read it: the
   folder above a season folder names its show, the folder of an anonymous video its film. */
static bool folder_names(const char *folder, const char *title, bool film, int year) {
    char rel[600];
    MediaName n;
    snprintf(rel, sizeof rel, film ? "%s/video.mkv" : "%s/Season 1/video.mkv", folder);
    media_parse_name(rel, &n);
    if (film && year && n.year && n.year != year) return false;
    return same_title(n.title, title);
}

/* The file's name as the library will read it: a copy (x.web.mp4) and a play-all's header
   (x.playall.mkv) are read as x. */
static void parse_of(const char *rel, MediaName *n) {
    char tmp[440];
    snprintf(tmp, sizeof tmp, "%.*s.mkv", (int)media_stem_len(rel), rel);
    media_parse_name(tmp, n);
}

/* Path parts: at most 48, the deepest kept (as names.c does). Returns how many. */
typedef struct {
    const char *at[48];
    size_t len[48];
    int n;
} Parts;

static void split(const char *rel, Parts *p) {
    p->n = 0;
    for (const char *s = rel; *s;) {
        size_t k = strcspn(s, "/");
        if (k) {
            if (p->n == 48) {
                memmove(p->at, p->at + 1, sizeof p->at - sizeof p->at[0]);
                memmove(p->len, p->len + 1, sizeof p->len - sizeof p->len[0]);
                p->n--;
            }
            p->at[p->n] = s;
            p->len[p->n++] = k;
        }
        s += k;
        if (*s) s++;
    }
}

static void part_name(const Parts *p, int i, char *out, size_t cap) { str_ncopy(out, cap, p->at[i], p->len[i]); }

/* Where part i ends in rel: the folder rel[0..that) */
static size_t part_end(const char *rel, const Parts *p, int i) { return (size_t)(p->at[i] + p->len[i] - rel); }

/* The show folder among the chosen file's folders: walking up from its own folder over season, disc
   and extras folders, the first other one (its part index), or -1. */
static int show_part(const Parts *p) {
    for (int i = p->n - 2; i >= 0; i--) {
        char name[256];
        part_name(p, i, name, sizeof name);
        if (media_season_folder(name) >= 0 || media_disc_folder(name) >= 0 || rank_in(name, EXTRA_DIRS) >= 0) continue;
        return i;
    }
    return -1;
}

/* rel's dir without trailing season and disc folders (the library's show folder of an episode) */
static size_t show_dir_len(const char *rel) {
    const char *slash = strrchr(rel, '/');
    size_t dl = slash ? (size_t)(slash - rel) : 0;
    while (dl) {
        const char *seg = rel;
        for (const char *q = rel; q < rel + dl; q++)
            if (*q == '/') seg = q + 1;
        char name[200];
        str_ncopy(name, sizeof name, seg, (size_t)(rel + dl - seg));
        if (media_season_folder(name) < 0 && media_disc_folder(name) < 0) break;
        dl = seg > rel ? (size_t)(seg - rel - 1) : 0;
    }
    return dl;
}

/* --------------------------------------------------------------- folders */

/* The library folder's own folder for series (or films): "Series", "TV Shows" ... as it's spelled
   there, else dflt - "" when the library folder itself is one. */
static void top_dir(const char *root, const char *const *list, const char *dflt, char *out, size_t cap) {
    const char *own = strrchr(root, '/');
    own = own ? own + 1 : root;
    if (rank_in(own, list) >= 0) {
        out[0] = '\0';
        return;
    }
    str_copy(out, cap, dflt);
    int best = 99;
    DirIter *it = dir_open(root);
    DirEntry e;
    while (it && dir_next(it, &e)) {
        int r = e.is_dir ? rank_in(e.name, list) : -1;
        if (r >= 0 && r < best) {
            best = r;
            str_copy(out, cap, e.name);
        }
    }
    if (it) dir_close(it);
}

/* ... and its own folder for audiobooks: "Audiobooks", "Ljudböcker" ... as it's spelled there (the first by
   name, if it has two), else "Audiobooks" - "" when the library folder itself is one. */
static void books_dir(const char *root, char *out, size_t cap) {
    const char *own = strrchr(root, '/');
    out[0] = '\0';
    if (media_is_books_folder(own ? own + 1 : root)) return;
    DirIter *it = dir_open(root);
    DirEntry e;
    while (it && dir_next(it, &e))
        if (e.is_dir && media_is_books_folder(e.name) && (!out[0] || strcmp(e.name, out) < 0))
            str_copy(out, cap, e.name);
    if (it) dir_close(it);
    if (!out[0]) str_copy(out, cap, "Audiobooks");
}

static void add_map(Sorter *s, const char *rel, size_t len, const char *dst, const char *title, bool book) {
    if (!len || len >= sizeof s->maps[0].src) return;
    for (int i = 0; i < s->nmaps; i++) {
        DirMap *m = &s->maps[i];
        if (strlen(m->src) == len && strncmp(m->src, rel, len) == 0) {
            if (!str_eq(m->dst, dst)) m->clash = true;
            return;
        }
    }
    if (s->nmaps == MAPS_MAX) return;
    DirMap *m = &s->maps[s->nmaps++];
    str_ncopy(m->src, sizeof m->src, rel, len);
    str_copy(m->dst, sizeof m->dst, dst);
    str_copy(m->title, sizeof m->title, title);
    m->clash = false;
    m->book = book;
}

static bool mapped(const Sorter *s, const char *rel, size_t len) {
    for (int i = 0; i < s->nmaps; i++)
        if (strlen(s->maps[i].src) == len && strncmp(s->maps[i].src, rel, len) == 0) return true;
    return false;
}

/* the deepest folder of rel's that's mapped (books: an audiobook's folder counts too) */
static const DirMap *map_of(const Sorter *s, const char *rel, bool books) {
    const DirMap *best = NULL;
    for (int i = 0; i < s->nmaps; i++) {
        const DirMap *m = &s->maps[i];
        size_t n = strlen(m->src);
        if (m->clash || (m->book && !books) || strncmp(rel, m->src, n) != 0 || rel[n] != '/') continue;
        if (!best || n > strlen(best->src)) best = m;
    }
    return best;
}

/* The library's folder for a show (in this library folder): of each episode's folders, the shallowest
   that names the show ("Series/Solsidan", not ".../Solsidan COMPLETE S01-S05/2" below it), the one
   most episodes share (a tie: the shorter, then the first); none names it: the first episode's show
   folder (show_dir_len) when that isn't the library folder itself or a "Series" folder of many. False:
   nowhere fit - a new <series folder>/<Show>. (Until 1.2.0 the first episode's folder alone: one copy at
   the library folder's top level sent every new season of that show to the top level too.) */
static bool show_home(const Sorter *s, const MediaSeries *ms, char *out, size_t cap) {
    enum { MAX_HOMES = 16 };
    struct {
        char dir[400];
        int votes;
    } c[MAX_HOMES];
    int nc = 0;
    char fallback[400] = "";
    const char *root = s->ix->roots[s->root_ix];
    for (int i = 0; i < ms->count; i++) {
        const MediaFile *e = media_episode(ms, i);
        if (!e || e->root != s->root_ix) continue;
        size_t dl = show_dir_len(e->rel);
        if (!fallback[0] && dl && dl < sizeof fallback) str_ncopy(fallback, sizeof fallback, e->rel, dl);
        const char *slash = strrchr(e->rel, '/');
        size_t full = slash ? (size_t)(slash - e->rel) : 0;
        for (size_t end = 0; end < full;) {
            while (end < full && e->rel[end] != '/') end++;
            if (end < sizeof c[0].dir && media_names_show(root, e->rel, end, ms->title)) {
                int k = 0;
                while (k < nc && !(strlen(c[k].dir) == end && strncmp(c[k].dir, e->rel, end) == 0)) k++;
                if (k == nc && nc < MAX_HOMES) {
                    str_ncopy(c[nc].dir, sizeof c[nc].dir, e->rel, end);
                    c[nc++].votes = 0;
                }
                if (k < nc) c[k].votes++;
                break;
            }
            end++;
        }
    }
    int best = -1;
    for (int k = 0; k < nc; k++)
        if (best < 0 || c[k].votes > c[best].votes ||
            (c[k].votes == c[best].votes && strlen(c[k].dir) < strlen(c[best].dir)))
            best = k;
    if (best >= 0) {
        str_copy(out, cap, c[best].dir);
        return true;
    }
    const char *last = strrchr(fallback, '/');
    if (!fallback[0] || rank_in(last ? last + 1 : fallback, SERIES_DIRS) >= 0) return false;
    str_copy(out, cap, fallback);
    return true;
}

/* Where a show's episodes go: the library's folder for it (in this library folder, show_home), else
   <series folder>/<its name>. shown: the name to show ("70's Show" as the library spells it). */
static const Place *show_place(Sorter *s, const char *name) {
    static Place spare; /* (a full cache: worked out again each time) */
    uint64_t key = upload_show_key(name);
    for (int i = 0; i < s->nshows; i++)
        if (s->shows[i].key == key) return &s->shows[i];
    Place *p = s->nshows < CACHE_MAX ? &s->shows[s->nshows++] : &spare;
    memset(p, 0, sizeof *p);
    p->key = key;
    str_copy(p->title, sizeof p->title, name);
    const MediaSeries *ms = s->root_ix >= 0 ? media_series(key) : NULL;
    if (ms && show_home(s, ms, p->dest, sizeof p->dest)) {
        str_copy(p->title, sizeof p->title, ms->title);
        p->known = true;
    }
    if (!p->known) {
        char safe[160];
        if (!upload_safe_name(name, safe, sizeof safe)) str_copy(safe, sizeof safe, "Unknown series");
        cat(p->dest, sizeof p->dest, s->series_dir, safe);
    }
    return p;
}

/* The season's folder in a show's folder: one there already ("Season 01", "season 1 70's show" - not
   one that's also a disc's), else "Season N" ("Specials" for season 0). */
static void season_place(Sorter *s, const char *show, int season, char *out, size_t cap) {
    for (int i = 0; i < s->nseasons; i++)
        if (s->seasons[i].season == season && str_eq(s->seasons[i].show, show)) {
            str_copy(out, cap, s->seasons[i].dest);
            return;
        }
    char want[32], best[256] = "", abs[1024], lo[256];
    if (season == 0) str_copy(want, sizeof want, "Specials");
    else snprintf(want, sizeof want, "Season %d", season);
    cat(abs, sizeof abs, s->o->root, show);
    DirIter *it = dir_open(abs);
    DirEntry e;
    while (it && dir_next(it, &e)) {
        if (!e.is_dir || e.is_link || media_season_folder(e.name) != season) continue;
        fold_copy(lo, sizeof lo, e.name);
        if (strstr(lo, "disc") || strstr(lo, "disk") || strstr(lo, "dvd") || strstr(lo, "skiva")) continue;
        if (str_ieq(e.name, want)) {
            str_copy(best, sizeof best, e.name);
            break;
        }
        if (!best[0] || strcmp(e.name, best) < 0) str_copy(best, sizeof best, e.name);
    }
    if (it) dir_close(it);
    cat(out, cap, show, best[0] ? best : want);
    if (s->nseasons < CACHE_MAX) {
        SeasonPlace *sp = &s->seasons[s->nseasons++];
        str_copy(sp->show, sizeof sp->show, show);
        sp->season = season;
        str_copy(sp->dest, sizeof sp->dest, out);
    }
}

/* Where a film goes: the folder the library keeps it in (its own folder: not the library folder or a
   "Movies" folder of many), else <movies folder>/<Title (Year)>. */
static const Place *film_place(Sorter *s, const char *title, int year) {
    static Place spare;
    uint64_t key = upload_show_key(title);
    for (int i = 0; i < s->nfilms; i++)
        if (s->films[i].key == key && s->films[i].year == year) return &s->films[i];
    Place *p = s->nfilms < CACHE_MAX ? &s->films[s->nfilms++] : &spare;
    memset(p, 0, sizeof *p);
    p->key = key;
    p->year = year;
    str_copy(p->title, sizeof p->title, title);
    for (int i = 0; s->root_ix >= 0 && i < s->ix->nmovies && !p->known; i++) {
        const MediaFile *m = &s->ix->files[s->ix->movies[i]];
        if (m->root != s->root_ix || !fold_eq(m->name.title, title)) continue;
        if (year && m->name.year && m->name.year != year) continue;
        const char *slash = strrchr(m->rel, '/');
        if (!slash) continue;
        char dir[400];
        str_ncopy(dir, sizeof dir, m->rel, (size_t)(slash - m->rel));
        const char *last = strrchr(dir, '/');
        if (rank_in(last ? last + 1 : dir, MOVIE_DIRS) >= 0 || str_eq(dir, s->movies_dir)) continue;
        str_copy(p->dest, sizeof p->dest, dir);
        str_copy(p->title, sizeof p->title, m->name.title);
        if (!year) p->year = m->name.year;
        p->known = true;
    }
    if (!p->known) {
        char name[200], safe[160];
        if (year) snprintf(name, sizeof name, "%s (%d)", title, year);
        else str_copy(name, sizeof name, title);
        if (!upload_safe_name(name, safe, sizeof safe)) str_copy(safe, sizeof safe, "Untitled");
        cat(p->dest, sizeof p->dest, s->movies_dir, safe);
    }
    return p;
}

/* ?series=<id>: that series' folder, when it's in this library folder */
static void series_target(Sorter *s) {
    const MediaSeries *ms = media_series(s->o->series);
    if (ms && s->root_ix >= 0 && show_home(s, ms, s->series_show, sizeof s->series_show)) {
        str_copy(s->series_title, sizeof s->series_title, ms->title);
        s->series_ok = true;
    }
}

static const char *rename_of(const UpSortOpts *o, uint64_t key) {
    for (int i = 0; i < o->nnames; i++)
        if (o->names[i].key == key && o->names[i].name[0]) return o->names[i].name;
    return NULL;
}

/* ------------------------------------------------------------------ the sort */

static void place_in(UpSorted *u, const char *dir, const char *rel) { /* dir + the file's own name */
    const char *name = strrchr(rel, '/');
    cat(u->dest, sizeof u->dest, dir, name ? name + 1 : rel);
    u->kept = false;
}

static void below(UpSorted *u, const char *dir, const char *rel, size_t len) { /* dir + rel below rel[0..len) */
    cat(u->dest, sizeof u->dest, dir, len ? rel + len + 1 : rel);
    u->kept = false;
}

/* An episode (numbered or a disc rip) of a show that has a name: where its show goes, and what its
   folders say about the rest of the chosen files. */
static void sort_episode(Sorter *s, UpSorted *u, const char *rel, const MediaName *nm, const char *name) {
    const Place *show;
    Place mine;
    if (s->o->series && s->series_ok) {
        memset(&mine, 0, sizeof mine);
        str_copy(mine.dest, sizeof mine.dest, s->series_show);
        str_copy(mine.title, sizeof mine.title, s->series_title);
        mine.known = true;
        show = &mine;
    } else {
        show = show_place(s, name);
    }
    str_copy(u->title, sizeof u->title, show->title);
    u->known = show->known;
    Parts p;
    split(rel, &p);
    int sp = show_part(&p);
    if (sp >= 0) { /* the show's own folder: what else is in it goes along */
        size_t end = part_end(rel, &p, sp);
        char folder[256];
        part_name(&p, sp, folder, sizeof folder);
        if (!mapped(s, rel, end) && (s->o->series || folder_names(folder, nm->title, false, 0)))
            add_map(s, rel, end, show->dest, show->title, false);
    }
    if (nm->unnumbered) return; /* (a disc rip keeps its folders: placed with the rest, below) */
    char season[400];
    season_place(s, show->dest, nm->season, season, sizeof season);
    place_in(u, season, rel);
    for (int i = sp + 1; i < p.n - 1; i++) { /* its season and disc folders: into the season's */
        char folder[256];
        part_name(&p, i, folder, sizeof folder);
        if (media_season_folder(folder) == nm->season || media_disc_folder(folder) >= 0)
            add_map(s, rel, part_end(rel, &p, i), season, show->title, false);
    }
}

static void sort_film(Sorter *s, UpSorted *u, const char *rel, const MediaName *nm) {
    const Place *film = film_place(s, nm->title, nm->year);
    str_copy(u->title, sizeof u->title, film->title);
    u->year = film->year;
    u->known = film->known;
    place_in(u, film->dest, rel);
    Parts p;
    split(rel, &p);
    for (int i = p.n - 2; i >= 0; i--) { /* its own folder (above an Extras folder it's in, say) */
        char folder[256];
        part_name(&p, i, folder, sizeof folder);
        if (rank_in(folder, EXTRA_DIRS) >= 0) continue;
        if (folder_names(folder, nm->title, true, nm->year))
            add_map(s, rel, part_end(rel, &p, i), film->dest, film->title, false);
        break;
    }
}

/* An audiobook's file (1.6): into the books folder by the path it was chosen with - from below the last books
   folder among its folders ("Ljudböcker/Astrid/Book/01.mp3" -> <books>/Astrid/Book/01.mp3), else all of it.
   Its own folder (above a CD1/ or Disc 2/ it's in) takes its cover along - unless that lies in a show's or a
   film's folder, which keeps what's there. Returns how long its book's folder is in rel (0: none - a loose
   file, or one right in a books folder, is a book of its own); u->title = its own name (upload_sort names an
   .m4a or .mp3 after its book's folder). */
static size_t sort_book(Sorter *s, UpSorted *u, const char *rel) {
    Parts p;
    split(rel, &p);
    char name[256];
    int bk = p.n - 2;
    for (; bk >= 0; bk--) {
        part_name(&p, bk, name, sizeof name);
        if (media_is_books_folder(name)) break;
    }
    size_t from = bk >= 0 ? part_end(rel, &p, bk) + 1 : 0; /* rel + from: the path below the books folder */
    cat(u->dest, sizeof u->dest, s->books_dir, rel + from);
    u->kept = false;
    const char *base = p.n ? p.at[p.n - 1] : rel;
    str_ncopy(u->title, sizeof u->title, base, media_stem_len(base));
    int own = p.n - 2;
    for (; own > bk + 1; own--) {
        part_name(&p, own, name, sizeof name);
        if (media_disc_folder(name) < 0) break;
    }
    if (own < 0) return 0;
    size_t end = part_end(rel, &p, own);
    char below[400], dst[400];
    str_ncopy(below, sizeof below, rel + from, end > from ? end - from : 0);
    cat(dst, sizeof dst, s->books_dir, below);
    if (own > bk) part_name(&p, own, name, sizeof name);
    else name[0] = '\0'; /* (the books folder itself) */
    if (!map_of(s, rel, true)) add_map(s, rel, end, dst, name, true);
    return own > bk ? end : 0;
}

/* "x.en.srt", "x.jpg", "x-poster.jpg", "x.web.mp4" next to the video x.mkv (same folder) */
static bool goes_with(const char *rel, const char *video) {
    size_t sl = media_stem_len(video);
    const char *a = strrchr(rel, '/'), *b = strrchr(video, '/');
    size_t la = a ? (size_t)(a - rel) : 0, lb = b ? (size_t)(b - video) : 0;
    return la == lb && strncmp(rel, video, la) == 0 && strlen(rel) > sl && strncmp(rel, video, sl) == 0 &&
           (rel[sl] == '.' || rel[sl] == '-');
}

/* A DVD VobSub's "x.en.sub" (its pictures) has its "x.en.idx" (the index) among the chosen files: which
   one, or -1. Alone, a .sub is MicroDVD text - not something Hemma reads. */
static int vobsub_idx(const char *const *rels, int n, int i) {
    const char *rel = rels[i];
    if (!rel || !media_is_vobsub_data(rel)) return -1;
    size_t len = strlen(rel);
    for (int k = 0; k < n; k++)
        if (k != i && rels[k] && strlen(rels[k]) == len && strncmp(rels[k], rel, len - 4) == 0 && str_iends(rels[k], ".idx"))
            return k;
    return -1;
}

void upload_sort(const UpSortOpts *o, const char *const *rels, int n, UpSorted *out) {
    Sorter *s = xcalloc(1, sizeof *s);
    s->o = o;
    s->ix = media_lib();
    s->root_ix = -1;
    for (int i = 0; i < s->ix->nroots && s->root_ix < 0; i++)
        if (str_eq(s->ix->roots[i], o->root)) s->root_ix = i;
    top_dir(o->root, SERIES_DIRS, "Series", s->series_dir, sizeof s->series_dir);
    top_dir(o->root, MOVIE_DIRS, "Movies", s->movies_dir, sizeof s->movies_dir);
    books_dir(o->root, s->books_dir, sizeof s->books_dir);
    if (o->series) series_target(s);
    bool to_series = o->series && s->series_ok;
    bool *lead = xcalloc((size_t)n + 1, sizeof *lead); /* placed by its own name: others may go with it */

    /* 1. videos by their own names: episodes and films (and what their folders say) */
    for (int i = 0; i < n; i++) {
        UpSorted *u = &out[i];
        memset(u, 0, sizeof *u);
        u->kept = true;
        const char *rel = rels[i];
        if (!rel) {
            u->kind = UPK_JUNK;
            continue;
        }
        str_copy(u->dest, sizeof u->dest, rel);
        if (media_is_sub_ext(rel) || vobsub_idx(rels, n, i) >= 0) u->kind = UPK_SUB;
        else if (media_is_image_ext(rel)) u->kind = UPK_PIC;
        else if (media_is_audio_ext(rel)) u->kind = UPK_BOOK; /* (placed in 1b) */
        else if (!media_is_video_ext(rel)) u->kind = UPK_JUNK;
        if (u->kind) continue;
        MediaName nm;
        parse_of(rel, &nm);
        u->season = nm.season;
        u->episode = nm.episode;
        u->episode_end = nm.episode_end;
        u->disc = nm.disc;
        str_copy(u->ep_title, sizeof u->ep_title, nm.ep_title);
        str_copy(u->title, sizeof u->title, nm.title);
        u->year = nm.year;
        if (nm.kind == MEDIA_EPISODE && !nm.extra) {
            u->kind = nm.unnumbered ? UPK_DISC : UPK_EPISODE;
            u->show = upload_show_key(nm.title);
            str_copy(u->found, sizeof u->found, nm.title);
            const char *name = rename_of(o, u->show);
            if (!name && !to_series && str_eq(nm.title, "Unknown series")) { /* (names.c's word for none) */
                u->kind = UPK_ASK;
                continue;
            }
            sort_episode(s, u, rel, &nm, name ? name : nm.title);
            lead[i] = !nm.unnumbered;
        } else if (nm.kind == MEDIA_MOVIE && !nm.extra && !to_series) {
            u->kind = UPK_FILM;
            sort_film(s, u, rel, &nm);
            lead[i] = true;
        } else {
            u->kind = nm.extra ? UPK_EXTRA : UPK_OTHER;
        }
    }

    /* 1b. audiobooks' files - after the videos: a show's or film's folder keeps what's in it, an .m4a or .mp3
           there too (a theme song, a soundtrack: placed with that folder in 3). An .m4a or .mp3 in a book's
           folder is named after it, and a part of that book when others are there too (an .m4b is a whole
           book, named by itself). */
    size_t *blen = xcalloc((size_t)n + 1, sizeof *blen);
    uint64_t *bh = xcalloc((size_t)n + 1, sizeof *bh);
    for (int i = 0; i < n; i++) {
        if (out[i].kind != UPK_BOOK) continue;
        if (!str_iends(rels[i], ".m4b") && map_of(s, rels[i], false)) {
            out[i].kind = UPK_AUDIO;
            continue;
        }
        blen[i] = sort_book(s, &out[i], rels[i]);
        if (blen[i] && !str_iends(rels[i], ".m4b")) bh[i] = hash64(rels[i], blen[i]);
        lead[i] = true;
    }
    for (int i = 0; i < n; i++) {
        if (!bh[i]) continue;
        for (int k = 0; k < n && !out[i].part; k++) out[i].part = k != i && bh[k] == bh[i];
        const char *seg = rels[i]; /* (the book's folder: the last part of rels[i][0..blen)) */
        for (const char *q = rels[i]; q < rels[i] + blen[i]; q++)
            if (*q == '/') seg = q + 1;
        str_ncopy(out[i].title, sizeof out[i].title, seg, (size_t)(rels[i] + blen[i] - seg));
    }
    free(bh);
    free(blen);

    /* 2. what goes with a video placed by its name: its subtitles, pictures, copy, a trailer named after it
          (folders compared by a hash first: thousands of files, each against every video) - and with an
          audiobook's file, the pictures named after it */
    uint64_t *dirh = xcalloc((size_t)n + 1, sizeof *dirh);
    for (int i = 0; i < n; i++) {
        const char *slash = rels[i] ? strrchr(rels[i], '/') : NULL;
        if (rels[i]) dirh[i] = hash64(rels[i], slash ? (size_t)(slash - rels[i]) : 0);
    }
    for (int i = 0; i < n; i++) {
        UpSorted *u = &out[i];
        if (!rels[i] || lead[i] || (u->kind != UPK_SUB && u->kind != UPK_PIC && u->kind != UPK_EXTRA && u->kind != UPK_OTHER))
            continue;
        int best = -1;
        size_t best_len = 0;
        for (int k = 0; k < n; k++)
            if (lead[k] && dirh[k] == dirh[i] && (out[k].kind != UPK_BOOK || u->kind == UPK_PIC) &&
                goes_with(rels[i], rels[k]) && media_stem_len(rels[k]) >= best_len) {
                best = k;
                best_len = media_stem_len(rels[k]);
            }
        if (best < 0) continue;
        const UpSorted *v = &out[best];
        char dir[400];
        const char *slash = strrchr(v->dest, '/');
        str_ncopy(dir, sizeof dir, v->dest, slash ? (size_t)(slash - v->dest) : 0);
        place_in(u, dir, rels[i]);
        str_copy(u->title, sizeof u->title, v->title);
        u->year = v->year;
        u->season = v->season;
        u->episode = v->episode;
        u->episode_end = v->episode_end;
        u->known = v->known;
        if (u->kind == UPK_SUB || u->kind == UPK_PIC) u->ep_title[0] = '\0';
        else u->kind = UPK_EXTRA;
    }

    /* 3. the rest: into the folder a show's or film's own folder went to (pictures and subtitles: or an
          audiobook's; audio in a show's or film's folder always has one); else, for a disc rip of a named
          show, below the show's folder; with ?series=, everything into that series' folder */
    for (int i = 0; i < n; i++) {
        UpSorted *u = &out[i];
        const char *rel = rels[i];
        if (!rel || !u->kept || u->kind == UPK_JUNK || u->kind == UPK_ASK) continue;
        const DirMap *m = map_of(s, rel, u->kind == UPK_PIC || u->kind == UPK_SUB);
        Parts p;
        split(rel, &p);
        int sp = show_part(&p);
        size_t end = sp >= 0 ? part_end(rel, &p, sp) : 0;
        if (m) {
            below(u, m->dst, rel, strlen(m->src));
            if (u->kind != UPK_DISC && u->kind != UPK_EPISODE) str_copy(u->title, sizeof u->title, m->title);
        } else if (to_series) {
            below(u, s->series_show, rel, end);
            str_copy(u->title, sizeof u->title, s->series_title);
        } else if (u->kind == UPK_DISC) {
            const char *name = rename_of(o, u->show);
            const Place *show = show_place(s, name ? name : u->found);
            below(u, show->dest, rel, end);
            str_copy(u->title, sizeof u->title, show->title);
            u->known = show->known;
        }
    }

    /* 4. a VobSub's .sub wherever its .idx went: the pair stays together (the library takes the .sub
          only beside its .idx) */
    for (int i = 0; i < n; i++) {
        int k = out[i].kind == UPK_SUB ? vobsub_idx(rels, n, i) : -1;
        if (k < 0) continue;
        UpSorted *u = &out[i];
        const UpSorted *x = &out[k];
        char dir[400];
        const char *slash = strrchr(x->dest, '/');
        str_ncopy(dir, sizeof dir, x->dest, slash ? (size_t)(slash - x->dest) : 0);
        place_in(u, dir, rels[i]);
        u->kept = x->kept;
        str_copy(u->title, sizeof u->title, x->title);
        u->ep_title[0] = '\0';
        u->year = x->year;
        u->season = x->season;
        u->episode = x->episode;
        u->episode_end = x->episode_end;
        u->known = x->known;
    }
    free(dirh);
    free(lead);
    free(s);
}
