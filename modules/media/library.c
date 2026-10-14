/* modules/media/library.c — scanning the library roots, the in-memory index,
 * its SQLite cache (instant library at boot, re-probe only changed files) and
 * per-person watch progress. See library.h. */
#include "library.h"
#include "../../core/db.h"
#include "../../core/plat.h"
#include "../../core/app.h"
#include "room.h"

#include <stdatomic.h>

/* ================================================================= state */

static MediaIndex g_empty;
static MediaIndex *g_ix;      /* current index (event-loop thread) */
static Lock g_lock = LOCK_INIT;
static MediaIndex *g_pending; /* produced by the scanner thread, swapped in by tick */
static atomic_bool g_scanning;
static atomic_bool g_abort;
static atomic_int g_seen;
static int64_t g_scan_t0, g_next_scan, g_last_progress;
static bool g_rescan_again;

#define RESCAN_EVERY_MS (30LL * 60 * 1000)

static void index_free(MediaIndex *ix) {
    if (!ix || ix == &g_empty) return;
    free(ix->files);
    free(ix->movies);
    free(ix->others);
    free(ix->recent);
    free(ix->episodes);
    free(ix->extras);
    free(ix->series);
    free(ix);
}

/* ================================================================ lookups */

const MediaIndex *media_lib(void) { return g_ix ? g_ix : &g_empty; }

static const MediaFile *index_find(const MediaIndex *ix, uint64_t id) {
    int lo = 0, hi = ix->nfiles;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ix->files[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < ix->nfiles && ix->files[lo].id == id ? &ix->files[lo] : NULL;
}

const MediaFile *media_file(uint64_t id) { return id ? index_find(media_lib(), id) : NULL; }

const MediaSeries *media_series(uint64_t id) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nseries; i++)
        if (ix->series[i].id == id) return &ix->series[i];
    return NULL;
}

const MediaFile *media_episode(const MediaSeries *s, int i) {
    const MediaIndex *ix = media_lib();
    if (!s || i < 0 || i >= s->count) return NULL;
    return &ix->files[ix->episodes[s->first + i]];
}

const MediaFile *media_neighbour(const MediaFile *ep, int dir) {
    if (!ep || ep->type != MF_VIDEO || ep->name.kind != MEDIA_EPISODE) return NULL;
    const MediaSeries *s = media_series(ep->series);
    for (int i = 0; s && i < s->count; i++)
        if (media_episode(s, i) == ep) return media_episode(s, i + dir);
    return NULL;
}

static void join_path(const char *root, const char *rel, char *out, size_t cap) {
    size_t n = strlen(root);
    if (n && root[n - 1] == '/') snprintf(out, cap, "%s%s", root, rel);
    else snprintf(out, cap, "%s/%s", root, rel);
}

bool media_full_path(const MediaFile *f, char *out, size_t cap) {
    const MediaIndex *ix = media_lib();
    if (!f || f->root < 0 || f->root >= ix->nroots) return false;
    join_path(ix->roots[f->root], f->rel, out, cap);
    return true;
}

int media_hue(const char *title) {
    char k[160];
    fold_copy(k, sizeof k, title ? title : "");
    return (int)(hash64_str(k) % 360);
}

bool media_parse_id(const char *hex, uint64_t *id) {
    if (!hex) return false;
    uint64_t v = 0;
    int n = 0;
    for (; hex[n] && n < 17; n++) {
        char c = hex[n];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v << 4 | (uint64_t)d;
    }
    if (n != 16 || (hex[n] && hex[n] != '.' && hex[n] != '/' && hex[n] != '?')) return false;
    *id = v;
    return v != 0;
}

void media_id_hex(uint64_t id, char out[17]) { snprintf(out, 17, "%016llx", (unsigned long long)id); }

uint64_t media_make_id(const char *root, const char *rel) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)root; *p; p++) h = (h ^ *p) * 1099511628211ULL;
    h = (h ^ '\n') * 1099511628211ULL;
    for (const unsigned char *p = (const unsigned char *)rel; *p; p++) h = (h ^ *p) * 1099511628211ULL;
    return h ? h : 1;
}

/* ================================================================== roots */

static void normalise_root(char *s) {
    for (char *p = s; *p; p++)
        if (*p == '\\') *p = '/';
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/' && !(n == 3 && s[1] == ':')) s[--n] = '\0'; /* keep "/" and "C:/" */
}

/* The root folder's own name ("70's show" for "/mnt/media/70's show"): it names the show
   when the root is a show folder. "" for drives and "/". */
static const char *root_name(const char *root) {
    const char *s = strrchr(root, '/');
    const char *name = s ? s + 1 : root;
    size_t n = strlen(name);
    return n && name[n - 1] != ':' ? name : "";
}

int media_roots(char roots[][300], int max) {
    const char *s = setting("media.roots", "");
    int n = 0;
    while (*s && n < max) {
        size_t len = strcspn(s, "\n");
        char line[300];
        str_ncopy(line, sizeof line, s, len);
        str_trim(line);
        normalise_root(line);
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= str_eq(roots[i], line);
        if (line[0] && !dup) str_copy(roots[n++], 300, line);
        s += len;
        if (*s == '\n') s++;
    }
    return n;
}

/* ============================================================== scanning */

typedef struct {
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots;
    const MediaIndex *old; /* reuse probes of unchanged files */
    MediaFile *files;
    int n, cap, probed;
} Scan;

static bool skip_name(const char *name) {
    static const char *const skip[] = {"System Volume Information", "lost+found", "Thumbs.db", "desktop.ini",
                                       "@eaDir", "#recycle", NULL};
    if (name[0] == '.' || name[0] == '$') return true; /* dot files, $RECYCLE.BIN */
    for (int i = 0; skip[i]; i++)
        if (str_ieq(name, skip[i])) return true;
    return false;
}

bool media_is_web_copy(const char *rel) { return str_iends(rel, ".web.mp4"); }
bool media_is_playall_head(const char *rel) { return str_iends(rel, ".playall.mkv"); }

const MediaFile *media_playable(const MediaFile *f) {
    const MediaFile *w = f && f->web ? media_file(f->web) : NULL;
    return w ? w : f;
}

/* Names come from the path; a ".web.mp4" copy is named like its original ("x.web.mp4" as "x.mp4"),
   so it still makes sense on its own if the original is ever deleted. A play-all's header
   ("x.playall.mkv") is named like the title it stands in for. */
static void parse_name(const char *root_nm, const char *rel, MediaName *out) {
    const char *strip = media_is_web_copy(rel) ? ".web.mp4" : media_is_playall_head(rel) ? ".playall.mkv" : NULL;
    if (!strip) {
        media_parse_name_in(root_nm, rel, out);
        return;
    }
    char plain[400];
    size_t n = strlen(rel) - strlen(strip);
    snprintf(plain, sizeof plain, "%.*s.%s", (int)n, rel, strip + strlen(strip) - 3);
    media_parse_name_in(root_nm, plain, out);
}

static void add_file(Scan *sc, int root, const char *rel, char type, int64_t size, int64_t mtime) {
    if (sc->n == sc->cap) {
        sc->cap = sc->cap ? sc->cap * 2 : 256;
        sc->files = xrealloc(sc->files, (size_t)sc->cap * sizeof *sc->files);
    }
    MediaFile *f = &sc->files[sc->n];
    memset(f, 0, sizeof *f);
    f->root = root;
    f->type = type;
    str_copy(f->rel, sizeof f->rel, rel);
    f->size = size;
    f->mtime = mtime;
    f->id = media_make_id(sc->roots[root], rel);
    if (type == MF_VIDEO) {
        parse_name(root_name(sc->roots[root]), rel, &f->name);
        if (f->name.extra) return; /* trailers, samples, bonus material: hidden */
        const MediaFile *o = sc->old ? index_find(sc->old, f->id) : NULL;
        if (o && o->type == MF_VIDEO && o->size == size && o->mtime == mtime) {
            f->probe = o->probe;
        } else {
            char path[1024];
            join_path(sc->roots[root], rel, path, sizeof path);
            media_probe_file(path, &f->probe);
            sc->probed++;
        }
    }
    sc->n++;
}

static void walk(Scan *sc, int root, const char *abs, const char *rel_dir, int depth) {
    if (depth > 24 || atomic_load(&g_abort)) return;
    DirIter *it = dir_open(abs);
    if (!it) return;
    DirEntry e;
    while (dir_next(it, &e) && !atomic_load(&g_abort)) {
        if (skip_name(e.name)) continue;
        char rel[400], sub[1024];
        int rl = rel_dir[0] ? snprintf(rel, sizeof rel, "%s/%s", rel_dir, e.name) : snprintf(rel, sizeof rel, "%s", e.name);
        if (rl < 0 || rl >= (int)sizeof rel) continue; /* absurdly deep path */
        if (e.is_dir) {
            join_path(abs, e.name, sub, sizeof sub);
            walk(sc, root, sub, rel, depth + 1);
            continue;
        }
        atomic_fetch_add(&g_seen, 1);
        if (str_iends(e.name, ".conv.part")) { /* the converter's half-made copy: a crash's leftover goes */
            join_path(abs, e.name, sub, sizeof sub);
            convert_drop_leftover(sub);
            continue;
        }
        char type = media_is_video_ext(e.name) ? MF_VIDEO
                    : media_is_sub_ext(e.name) ? MF_SUB
                    : media_is_image_ext(e.name) ? MF_IMAGE
                                                 : 0;
        if (type) add_file(sc, root, rel, type, e.size, e.mtime);
    }
    dir_close(it);
}

/* ======================================================== building the index */

static _Thread_local const MediaFile *t_files; /* qsort context */

typedef struct {
    int idx;
    size_t dirlen; /* length of the directory part of rel (0 = root) */
} Ent;

static size_t dir_len(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? (size_t)(s - rel) : 0;
}

static const char *base_of(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

/* A video's name without its extension - and without ".web" for a play-everywhere copy, so
   "x.web.mp4" finds the same pictures as "x.mkv" (and keeps them when only the copy is kept). */
static void video_stem(const char *rel, char *out, size_t cap) {
    str_copy(out, cap, base_of(rel));
    size_t n = strlen(out);
    if (media_is_web_copy(out) && n > 8) out[n - 8] = '\0';
    else {
        char *dot = strrchr(out, '.');
        if (dot) *dot = '\0';
    }
}

static int cmp_dir(const MediaFile *a, size_t al, const MediaFile *b, size_t bl) {
    if (a->root != b->root) return a->root - b->root;
    int c = strncmp(a->rel, b->rel, MIN(al, bl));
    if (c) return c;
    return al < bl ? -1 : al > bl ? 1 : 0;
}

static int ent_cmp(const void *x, const void *y) {
    const Ent *a = x, *b = y;
    int c = cmp_dir(&t_files[a->idx], a->dirlen, &t_files[b->idx], b->dirlen);
    return c ? c : strcmp(t_files[a->idx].rel, t_files[b->idx].rel);
}

static int id_cmp(const void *x, const void *y) {
    const MediaFile *a = x, *b = y;
    return a->id < b->id ? -1 : a->id > b->id;
}

static int title_cmp(const char *a, const char *b) {
    char x[160], y[160];
    fold_copy(x, sizeof x, a);
    fold_copy(y, sizeof y, b);
    /* ignore a leading article when sorting ("The Office" files under O) */
    const char *px = str_starts(x, "the ") ? x + 4 : x, *py = str_starts(y, "the ") ? y + 4 : y;
    return strcmp(px, py);
}

static int movie_cmp(const void *x, const void *y) {
    const MediaFile *a = &t_files[*(const int *)x], *b = &t_files[*(const int *)y];
    int c = title_cmp(a->name.title, b->name.title);
    return c ? c : a->name.year - b->name.year;
}

static int other_cmp(const void *x, const void *y) {
    const MediaFile *a = &t_files[*(const int *)x], *b = &t_files[*(const int *)y];
    if (a->root != b->root) return a->root - b->root;
    return strcmp(a->rel, b->rel);
}

static int recent_cmp(const void *x, const void *y) {
    const MediaFile *a = &t_files[*(const int *)x], *b = &t_files[*(const int *)y];
    if (a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
    return title_cmp(a->name.title, b->name.title);
}

static int episode_cmp(const void *x, const void *y) {
    const MediaFile *a = &t_files[*(const int *)x], *b = &t_files[*(const int *)y];
    int c = title_cmp(a->name.title, b->name.title);
    if (c) return c;
    if (a->series != b->series) return a->series < b->series ? -1 : 1;
    if (a->name.season != b->name.season) return a->name.season - b->name.season;
    if (a->name.episode != b->name.episode) return a->name.episode - b->name.episode;
    return strcmp(a->rel, b->rel);
}

static int series_cmp(const void *x, const void *y) {
    return title_cmp(((const MediaSeries *)x)->title, ((const MediaSeries *)y)->title);
}

static uint64_t series_id(const char *title) {
    char k[160];
    fold_copy(k, sizeof k, title);
    uint64_t h = hash64_str(k);
    return h ? h : 1;
}

/* "Show/x" for "Show/x.mkv", "Show/x.web.mp4" and "Show/x.playall.mkv": a file and its copy */
size_t media_stem_len(const char *rel) {
    size_t n = strlen(rel);
    if (media_is_web_copy(rel)) return n - strlen(".web.mp4");
    if (media_is_playall_head(rel)) return n - strlen(".playall.mkv");
    const char *dot = strrchr(rel, '.'), *slash = strrchr(rel, '/');
    return dot && (!slash || dot > slash) ? (size_t)(dot - rel) : n;
}

static bool same_item(const MediaIndex *ix, const MediaFile *f, const char *root, const char *rel, size_t sl) {
    return str_eq(ix->roots[f->root], root) && media_stem_len(f->rel) == sl && strncmp(f->rel, rel, sl) == 0;
}

bool media_have(const char *root, const char *rel, char *have, size_t cap) {
    const MediaIndex *ix = media_lib();
    if (have && cap) have[0] = '\0';
    if (media_is_playall_head(rel)) return false;
    MediaName n;
    parse_name(root_name(root), rel, &n);
    if (n.extra || n.unnumbered || (n.kind != MEDIA_EPISODE && n.kind != MEDIA_MOVIE)) return false;
    size_t sl = media_stem_len(rel);
    const MediaFile *hit = NULL;
    if (n.kind == MEDIA_EPISODE) {
        int from = n.episode, to = MAX(n.episode, n.episode_end);
        const MediaSeries *s = media_series(series_id(n.title));
        if (!s || from <= 0 || to - from >= 64) return false;
        uint64_t want = to - from == 63 ? ~0ULL : (1ULL << (to - from + 1)) - 1, got = 0;
        for (int i = 0; i < s->count && got != want; i++) {
            const MediaFile *e = media_episode(s, i);
            if (!e || e->name.season != n.season || same_item(ix, e, root, rel, sl)) continue;
            int a = MAX(from, e->name.episode), z = MIN(to, MAX(e->name.episode, e->name.episode_end));
            for (int k = a; k <= z; k++) got |= 1ULL << (k - from);
            if (a <= z && !hit) hit = e;
        }
        if (got != want) return false;
    } else {
        char key[160], other[160];
        fold_copy(key, sizeof key, n.title);
        int part = media_name_part(rel);
        for (int i = 0; i < ix->nmovies && !hit; i++) {
            const MediaFile *m = &ix->files[ix->movies[i]];
            if (n.year && m->name.year && n.year != m->name.year) continue;
            fold_copy(other, sizeof other, m->name.title);
            if (str_eq(key, other) && media_name_part(m->rel) == part && !same_item(ix, m, root, rel, sl)) hit = m;
        }
        if (!hit) return false;
    }
    if (have && cap) str_copy(have, cap, hit->rel);
    return true;
}

/* The show folder of a directory: rel[0..dl) without trailing season and disc folders
   ("Show/Season 1/Disc - 2" -> "Show"). */
static size_t show_dir_len(const char *rel, size_t dl) {
    while (dl) {
        const char *seg = rel;
        for (const char *p = rel; p < rel + dl; p++)
            if (*p == '/') seg = p + 1;
        char name[200];
        str_ncopy(name, sizeof name, seg, (size_t)(rel + dl - seg));
        if (media_season_folder(name) < 0 && media_disc_folder(name) < 0) break;
        dl = seg > rel ? (size_t)(seg - rel - 1) : 0;
    }
    return dl;
}

static bool span_ieq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

/* Image in [group] whose stem (file name without extension) equals `stem`. */
static uint64_t find_image(const MediaFile *files, const Ent *ents, int g0, int g1, const char *stem) {
    size_t sl = strlen(stem);
    for (int k = g0; k < g1; k++) {
        const MediaFile *f = &files[ents[k].idx];
        if (f->type != MF_IMAGE) continue;
        const char *b = base_of(f->rel);
        const char *dot = strrchr(b, '.');
        if (dot && (size_t)(dot - b) == sl && span_ieq(b, stem, sl)) return f->id;
    }
    return 0;
}

static uint64_t generic_poster(const MediaFile *files, const Ent *ents, int g0, int g1) {
    static const char *const names[] = {"poster", "folder", "cover", "show", "movie", NULL};
    for (int i = 0; names[i]; i++) {
        uint64_t id = find_image(files, ents, g0, g1, names[i]);
        if (id) return id;
    }
    return 0;
}

/* The [g0, g1) range of ents whose directory is (root, dir[0..dl)). */
static bool find_group(const MediaFile *files, const Ent *ents, int n, int root, const char *dir, size_t dl, int *g0,
                       int *g1) {
    MediaFile key;
    memset(&key, 0, sizeof key);
    key.root = root;
    str_ncopy(key.rel, sizeof key.rel, dir, dl);
    size_t kl = strlen(key.rel);
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (cmp_dir(&files[ents[mid].idx], ents[mid].dirlen, &key, kl) < 0) lo = mid + 1;
        else hi = mid;
    }
    int e = lo;
    while (e < n && cmp_dir(&files[ents[e].idx], ents[e].dirlen, &key, kl) == 0) e++;
    *g0 = lo;
    *g1 = e;
    return e > lo;
}

/* ============================================================== disc rips */

/* A ripped DVD season ("Show/Season 1/Disc - 2/B3_t03.mkv") names no episodes: the parser marks
 * them `unnumbered`, with their disc and MakeMKV title number. Every disc also carries a "play all"
 * title (its episodes joined) and often bonus clips. Durations sort them out:
 *   1. on one disc, a title >= 2.5x the median — or exactly its episode-like siblings together — is a play-all;
 *   2. a title under 40% of the season's typical episode is a bonus clip;
 *   3. a play-all lasts exactly as long as its disc's episodes, so a title that makes the sum overrun
 *      is a bonus too (an 18-minute featurette among 22-minute episodes), and a clip that fills a
 *      shortfall goes back to being an episode;
 *   4. what remains is numbered in disc, then title order (MakeMKV's title order is the disc's own).
 * Files with their own numbers (S04E01) are never touched. Uploads leave play-alls behind but send
 * their first MB ("B1_t00.playall.mkv"): a play-all from the start, so step 3 still has its length. */

#define PLAYALL_X2 5 /* 2.5x the median, in halves */

static bool rip_title(const MediaFile *f) {
    return f->type == MF_VIDEO && f->name.kind == MEDIA_EPISODE && f->name.unnumbered;
}

static int cmp_i64(const void *x, const void *y) {
    int64_t a = *(const int64_t *)x, b = *(const int64_t *)y;
    return a < b ? -1 : a > b;
}

static int64_t median_of(int64_t *v, int n) { /* sorts v */
    qsort(v, (size_t)n, sizeof *v, cmp_i64);
    return n <= 0 ? 0 : n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static int64_t dur(const MediaFile *f) { return f->probe.duration_ms; }
static int64_t sum_tol(int64_t total) { return MAX((int64_t)30000, total / 200); } /* 30 s or 0.5% */

/* The rip titles of one directory, as file indices; *all_dur = each has a known duration. */
static int disc_titles(const MediaFile *F, const Ent *ents, int g0, int g1, int **out, bool *all_dur) {
    int n = 0;
    *out = xmalloc((size_t)(g1 - g0 + 1) * sizeof **out);
    *all_dur = true;
    for (int k = g0; k < g1; k++)
        if (rip_title(&F[ents[k].idx])) {
            (*out)[n++] = ents[k].idx;
            *all_dur &= dur(&F[ents[k].idx]) > 0;
        }
    return n;
}

/* Step 1: play-alls of one disc (or one season folder without discs). */
static void find_playalls(MediaFile *F, const Ent *ents, int g0, int g1) {
    bool all_dur;
    int *t, n = disc_titles(F, ents, g0, g1, &t, &all_dur);
    int64_t *v = xmalloc((size_t)(n + 1) * sizeof *v);
    if (n >= 3) {
        for (int i = 0; i < n; i++) v[i] = all_dur ? dur(&F[t[i]]) : F[t[i]].size; /* size: same disc, same bitrate */
        int64_t med = median_of(v, n);
        bool any = false;
        for (int i = 0; i < n; i++) {
            int64_t m = all_dur ? dur(&F[t[i]]) : F[t[i]].size;
            if (med > 0 && m * 2 >= med * PLAYALL_X2) F[t[i]].role = MF_PLAYALL, any = true;
        }
        if (!any && all_dur) { /* a small disc, [22, 22, 44]: the longest is its siblings together */
            int L = 0;
            for (int i = 1; i < n; i++)
                if (dur(&F[t[i]]) > dur(&F[t[L]])) L = i;
            int k = 0;
            for (int i = 0; i < n; i++)
                if (i != L) v[k++] = dur(&F[t[i]]);
            int64_t mo = median_of(v, k), sum = 0;
            int parts = 0;
            for (int i = 0; i < k; i++)
                if (v[i] * 10 >= mo * 6 && v[i] * 10 <= mo * 16) sum += v[i], parts++;
            int64_t l = dur(&F[t[L]]);
            if (parts >= 2 && l * 5 >= mo * 9 && llabs(l - sum) <= sum_tol(l)) F[t[L]].role = MF_PLAYALL;
        }
    }
    free(v);
    free(t);
}

/* The k of n titles (k <= 4) whose durations together come closest to `want`; returns the error. */
static int64_t closest_combo(MediaFile *F, const int *t, int n, int k, int64_t want, int best[4]) {
    int c[4];
    for (int i = 0; i < k; i++) c[i] = i;
    int64_t err = INT64_MAX;
    for (;;) {
        int64_t s = 0;
        for (int i = 0; i < k; i++) s += dur(&F[t[c[i]]]);
        if (llabs(s - want) < err) {
            err = llabs(s - want);
            memcpy(best, c, (size_t)k * sizeof *c);
        }
        int i = k - 1;
        while (i >= 0 && c[i] == n - k + i) i--;
        if (i < 0) return err;
        c[i]++;
        for (int j = i + 1; j < k; j++) c[j] = c[j - 1] + 1;
    }
}

/* Step 3: the disc's episodes must add up to its play-all. When they overrun, the fewest titles
   (up to 4) that make up the difference are clips; when they fall short, a clip or two that fill
   the gap are episodes after all. */
static void match_playall(MediaFile *F, const Ent *ents, int g0, int g1) {
    bool all_dur;
    int *t, n = disc_titles(F, ents, g0, g1, &t, &all_dur);
    int *m = xmalloc((size_t)(n + 1) * sizeof *m), *b = xmalloc((size_t)(n + 1) * sizeof *b), nm = 0, nb = 0;
    int64_t P = 0, S = 0;
    for (int i = 0; i < n; i++) {
        const MediaFile *f = &F[t[i]];
        if (f->role == MF_PLAYALL) P = MAX(P, dur(f));
        else if (f->role == MF_MAIN) S += dur(f), m[nm++] = t[i];
        else b[nb++] = t[i];
    }
    int64_t tol = sum_tol(P);
    int pick[4];
    if (all_dur && P && S - P > tol && nm <= 40) {
        for (int k = 1; k <= 4 && k < nm; k++)
            if (closest_combo(F, m, nm, k, S - P, pick) <= tol) {
                for (int i = 0; i < k; i++) F[m[pick[i]]].role = MF_BONUS;
                break;
            }
    } else if (all_dur && P && P - S > tol) {
        for (int k = 1; k <= 2 && k <= nb; k++)
            if (closest_combo(F, b, nb, k, P - S, pick) <= tol) {
                for (int i = 0; i < k; i++) F[b[pick[i]]].role = MF_MAIN;
                break;
            }
    }
    free(b);
    free(m);
    free(t);
}

typedef struct {
    uint64_t series;
    int season, idx;
} SeasonKey;

static int season_key_cmp(const void *x, const void *y) {
    const SeasonKey *a = x, *b = y;
    if (a->series != b->series) return a->series < b->series ? -1 : 1;
    if (a->season != b->season) return a->season - b->season;
    return a->idx - b->idx;
}

/* disc, then title number (none last), then name */
static int rip_order_cmp(const void *x, const void *y) {
    const MediaFile *a = &t_files[*(const int *)x], *b = &t_files[*(const int *)y];
    if (a->name.disc != b->name.disc) return a->name.disc - b->name.disc;
    int ta = a->name.track < 0 ? INT32_MAX : a->name.track, tb = b->name.track < 0 ? INT32_MAX : b->name.track;
    if (ta != tb) return ta < tb ? -1 : 1;
    return strcmp(a->rel, b->rel);
}

/* Steps 2 and 4, one season at a time. */
static void season_pass(MediaFile *F, int nfiles, bool number) {
    SeasonKey *k = xmalloc((size_t)(nfiles + 1) * sizeof *k);
    int n = 0;
    for (int i = 0; i < nfiles; i++)
        if (F[i].type == MF_VIDEO && F[i].name.kind == MEDIA_EPISODE && F[i].role != MF_PLAYALL)
            k[n++] = (SeasonKey){series_id(F[i].name.title), F[i].name.season, i};
    qsort(k, (size_t)n, sizeof *k, season_key_cmp);
    int64_t *v = xmalloc((size_t)(n + 1) * sizeof *v);
    int *u = xmalloc((size_t)(n + 1) * sizeof *u);
    for (int a = 0, b; a < n; a = b) {
        b = a + 1;
        while (b < n && k[b].series == k[a].series && k[b].season == k[a].season) b++;
        if (!number) { /* step 2: clips far shorter than the season's episodes */
            int nv = 0;
            for (int i = a; i < b; i++)
                if (dur(&F[k[i].idx]) > 0) v[nv++] = dur(&F[k[i].idx]);
            int64_t med = nv >= 3 ? median_of(v, nv) : 0;
            for (int i = a; i < b; i++) {
                MediaFile *f = &F[k[i].idx];
                if (med && f->name.unnumbered && dur(f) > 0 && dur(f) * 5 < med * 2) f->role = MF_BONUS;
            }
            continue;
        }
        /* step 4: number what is left after the season's own numbers */
        int top = 0, nu = 0;
        for (int i = a; i < b; i++) {
            const MediaFile *f = &F[k[i].idx];
            if (f->role != MF_MAIN) continue;
            if (f->name.unnumbered) u[nu++] = k[i].idx;
            else top = MAX(top, MAX(f->name.episode, f->name.episode_end));
        }
        qsort(u, (size_t)nu, sizeof *u, rip_order_cmp);
        for (int i = 0; i < nu; i++) F[u[i]].name.episode = F[u[i]].name.episode_end = top + 1 + i;
    }
    free(u);
    free(v);
    free(k);
}

/* Play-alls and bonus clips leave the episode list: they become series extras with a name that
   says what they are ("70's Show – season 1, disc 4: all episodes"). */
static void name_extra(MediaFile *f) {
    char where[64], t[300];
    int w = f->name.season ? snprintf(where, sizeof where, "season %d", f->name.season)
                           : snprintf(where, sizeof where, "specials");
    if (f->name.disc && w > 0 && w < (int)sizeof where) snprintf(where + w, sizeof where - (size_t)w, ", disc %d", f->name.disc);
    if (f->role == MF_PLAYALL) snprintf(t, sizeof t, "%s – %s: all episodes", f->name.title, where);
    else if (f->name.ep_title[0]) snprintf(t, sizeof t, "%s – %s: %s", f->name.title, where, f->name.ep_title);
    else if (f->name.track >= 0) snprintf(t, sizeof t, "%s – %s: extra (title %d)", f->name.title, where, f->name.track);
    else snprintf(t, sizeof t, "%s – %s: extra", f->name.title, where);
    f->series = series_id(f->name.title);
    str_copy(f->name.title, sizeof f->name.title, t);
    f->name.kind = MEDIA_OTHER;
    f->name.extra = true;
    f->name.episode = f->name.episode_end = 0;
}

static int extra_cmp(const void *x, const void *y) {
    const MediaFile *a = &t_files[*(const int *)x], *b = &t_files[*(const int *)y];
    if (a->series != b->series) return a->series < b->series ? -1 : 1;
    if (a->name.season != b->name.season) return a->name.season - b->name.season;
    if (a->role != b->role) return a->role == MF_PLAYALL ? -1 : b->role == MF_PLAYALL ? 1 : 0;
    return rip_order_cmp(x, y);
}

/* Sort, de-duplicate, and derive everything the pages need: subtitle and
 * poster links, series, and the display orders. Runs on whichever thread
 * built the file list. */
static void index_finish(MediaIndex *ix) {
    MediaFile *F = ix->files;
    qsort(F, (size_t)ix->nfiles, sizeof *F, id_cmp);
    int w = 0;
    for (int i = 0; i < ix->nfiles; i++)
        if (!w || F[w - 1].id != F[i].id) F[w++] = F[i];
    ix->nfiles = w;
    t_files = F;
    for (int i = 0; i < w; i++)
        if (F[i].type == MF_VIDEO && media_is_playall_head(F[i].rel)) F[i].role = MF_PLAYALL;

    /* group by directory */
    Ent *ents = xmalloc((size_t)(w ? w : 1) * sizeof *ents);
    for (int i = 0; i < w; i++) ents[i] = (Ent){i, dir_len(F[i].rel)};
    qsort(ents, (size_t)w, sizeof *ents, ent_cmp);
    int *gend = xmalloc((size_t)(w ? w : 1) * sizeof *gend); /* gend[g0]: end of the directory starting at g0 */
    for (int g0 = 0, g1; g0 < w; g0 = g1) {
        g1 = g0 + 1;
        while (g1 < w && cmp_dir(&F[ents[g1].idx], ents[g1].dirlen, &F[ents[g0].idx], ents[g0].dirlen) == 0) g1++;
        gend[g0] = g1;
        /* "x.web.mp4" made by convert.c: its original streams through it, and the copy itself
           stays out of every list (episodes, extras, recent) */
        for (int k = g0; k < g1; k++) {
            MediaFile *c = &F[ents[k].idx];
            if (c->type != MF_VIDEO || !media_is_web_copy(c->rel)) continue;
            const char *cb = base_of(c->rel);
            size_t stem = strlen(cb) - strlen(".web.mp4");
            for (int v = g0; v < g1; v++) {
                MediaFile *o = &F[ents[v].idx];
                if (o == c || o->type != MF_VIDEO || media_is_web_copy(o->rel)) continue;
                const char *ob = base_of(o->rel), *dot = strrchr(ob, '.');
                if (dot && (size_t)(dot - ob) == stem && strncmp(ob, cb, stem) == 0) {
                    o->web = c->id;
                    c->role = MF_WEBCOPY;
                    c->name.kind = MEDIA_OTHER;
                    break;
                }
            }
        }
        int nvideos = 0;
        for (int k = g0; k < g1; k++) nvideos += F[ents[k].idx].type == MF_VIDEO && F[ents[k].idx].role == MF_MAIN;
        for (int k = g0; k < g1; k++) {
            MediaFile *f = &F[ents[k].idx];
            if (f->type == MF_SUB) {
                for (int v = g0; v < g1; v++) {
                    MediaFile *vid = &F[ents[v].idx];
                    if (vid->type == MF_VIDEO && vid->role != MF_WEBCOPY && /* a copy's subtitles are its original's */
                        media_sub_for(base_of(vid->rel), base_of(f->rel), f->lang, sizeof f->lang)) {
                        f->video = vid->id;
                        break;
                    }
                }
            } else if (f->type == MF_VIDEO) {
                char stem[400], alt[420];
                video_stem(f->rel, stem, sizeof stem);
                f->poster = find_image(F, ents, g0, g1, stem);
                snprintf(alt, sizeof alt, "%s-poster", stem);
                if (!f->poster) f->poster = find_image(F, ents, g0, g1, alt);
                snprintf(alt, sizeof alt, "%s-thumb", stem);
                if (!f->poster) f->poster = find_image(F, ents, g0, g1, alt);
                if (!f->poster && nvideos == 1 && f->name.kind != MEDIA_EPISODE) f->poster = generic_poster(F, ents, g0, g1);
            }
        }
    }

    /* ripped discs: play-alls, bonus clips, then episode numbers */
    for (int g0 = 0; g0 < w; g0 = gend[g0]) find_playalls(F, ents, g0, gend[g0]);
    season_pass(F, w, false);
    for (int g0 = 0; g0 < w; g0 = gend[g0]) match_playall(F, ents, g0, gend[g0]);
    season_pass(F, w, true);
    for (int i = 0; i < w; i++)
        if (F[i].role == MF_PLAYALL || F[i].role == MF_BONUS) name_extra(&F[i]);
    free(gend);

    /* series */
    int nep = 0;
    for (int i = 0; i < w; i++)
        if (F[i].type == MF_VIDEO && F[i].name.kind == MEDIA_EPISODE) {
            F[i].series = series_id(F[i].name.title);
            nep++;
        }
    ix->episodes = xmalloc((size_t)(nep ? nep : 1) * sizeof(int));
    ix->nepisodes = 0;
    for (int i = 0; i < w; i++)
        if (F[i].series && F[i].name.kind == MEDIA_EPISODE) ix->episodes[ix->nepisodes++] = i;
    qsort(ix->episodes, (size_t)nep, sizeof(int), episode_cmp);
    ix->series = xcalloc((size_t)(nep ? nep : 1), sizeof *ix->series);
    for (int k = 0; k < nep;) {
        MediaFile *first = &F[ix->episodes[k]];
        MediaSeries *s = &ix->series[ix->nseries++];
        s->id = first->series;
        str_copy(s->title, sizeof s->title, first->name.title);
        s->first = k;
        int last_season = -1;
        while (k < nep && F[ix->episodes[k]].series == s->id) {
            MediaFile *e = &F[ix->episodes[k]];
            if (e->name.season != last_season) s->nseasons++, last_season = e->name.season;
            if (!s->year && e->name.year) s->year = e->name.year;
            if (e->mtime > s->newest) s->newest = e->mtime;
            s->count++;
            k++;
        }
        /* series poster: in the show folder (above "Season N/Disc N"), else the first episode's */
        size_t dl = dir_len(first->rel), show_dl = show_dir_len(first->rel, dl);
        int g0, g1;
        if (find_group(F, ents, w, first->root, first->rel, show_dl, &g0, &g1)) s->poster = generic_poster(F, ents, g0, g1);
        if (!s->poster && find_group(F, ents, w, first->root, first->rel, dl, &g0, &g1))
            s->poster = generic_poster(F, ents, g0, g1);
    }
    qsort(ix->series, (size_t)ix->nseries, sizeof *ix->series, series_cmp);

    /* series extras: disc play-alls and bonus clips (not a play-all's header: it doesn't play) */
    ix->extras = xmalloc((size_t)(w ? w : 1) * sizeof(int));
    ix->nextras = 0;
    for (int i = 0; i < w; i++)
        if (F[i].type == MF_VIDEO && (F[i].role == MF_PLAYALL || F[i].role == MF_BONUS) && !media_is_playall_head(F[i].rel))
            ix->extras[ix->nextras++] = i;
    qsort(ix->extras, (size_t)ix->nextras, sizeof(int), extra_cmp);

    /* display orders */
    ix->movies = xmalloc((size_t)(w ? w : 1) * sizeof(int));
    ix->others = xmalloc((size_t)(w ? w : 1) * sizeof(int));
    ix->recent = xmalloc((size_t)(w ? w : 1) * sizeof(int));
    ix->nmovies = ix->nothers = ix->nrecent = 0;
    for (int i = 0; i < w; i++) {
        if (F[i].type != MF_VIDEO || F[i].role) continue;
        ix->recent[ix->nrecent++] = i;
        if (F[i].name.kind == MEDIA_MOVIE) ix->movies[ix->nmovies++] = i;
        else if (F[i].name.kind == MEDIA_OTHER) ix->others[ix->nothers++] = i;
    }
    qsort(ix->movies, (size_t)ix->nmovies, sizeof(int), movie_cmp);
    qsort(ix->others, (size_t)ix->nothers, sizeof(int), other_cmp);
    qsort(ix->recent, (size_t)ix->nrecent, sizeof(int), recent_cmp);
    free(ents);
    t_files = NULL;
}

static MediaIndex *scan_run(Scan *sc) {
    for (int r = 0; r < sc->nroots && !atomic_load(&g_abort); r++) walk(sc, r, sc->roots[r], "", 0);
    if (atomic_load(&g_abort)) {
        free(sc->files);
        return NULL;
    }
    MediaIndex *ix = xcalloc(1, sizeof *ix);
    memcpy(ix->roots, sc->roots, sizeof ix->roots);
    ix->nroots = sc->nroots;
    ix->files = sc->files ? sc->files : xmalloc(sizeof(MediaFile));
    ix->nfiles = sc->n;
    ix->probed = sc->probed;
    index_finish(ix);
    return ix;
}

/* ============================================================ SQLite cache */

#define CACHE_FORMAT 1 /* raise it with every change to SCHEMA: a cache in an older layout is dropped */
static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS media_file ("
    " id TEXT PRIMARY KEY, root TEXT NOT NULL, type TEXT NOT NULL, rel TEXT NOT NULL,"
    " size INTEGER NOT NULL DEFAULT 0, mtime INTEGER NOT NULL DEFAULT 0,"
    " kind INTEGER NOT NULL DEFAULT 0, title TEXT NOT NULL DEFAULT '', year INTEGER NOT NULL DEFAULT 0,"
    " season INTEGER NOT NULL DEFAULT 0, episode INTEGER NOT NULL DEFAULT 0, episode_end INTEGER NOT NULL DEFAULT 0,"
    " ep_title TEXT NOT NULL DEFAULT '', container TEXT NOT NULL DEFAULT '', duration_ms INTEGER NOT NULL DEFAULT 0,"
    " width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0, vcodec TEXT NOT NULL DEFAULT '',"
    " acodec TEXT NOT NULL DEFAULT '', channels INTEGER NOT NULL DEFAULT 0, playable INTEGER NOT NULL DEFAULT 0,"
    " why TEXT NOT NULL DEFAULT '') WITHOUT ROWID";

static void col_text(sqlite3_stmt *st, int col, char *out, size_t cap) {
    const char *s = (const char *)sqlite3_column_text(st, col);
    str_copy(out, cap, s ? s : "");
}

static void cache_load(void) {
    MediaIndex *ix = xcalloc(1, sizeof *ix);
    ix->nroots = media_roots(ix->roots, MEDIA_MAX_ROOTS);
    int cap = 256;
    ix->files = xmalloc((size_t)cap * sizeof *ix->files);
    /* Names are parsed again from the path (2-3 µs each) rather than read back: the cache then never
       disagrees with a fresh scan, and parser improvements apply at once. Probes are the costly part. */
    sqlite3_stmt *st = db_prep("SELECT id, root, type, rel, size, mtime, container, duration_ms, width, height, "
                               "vcodec, acodec, channels, playable, why FROM media_file");
    while (st && sqlite3_step(st) == SQLITE_ROW) {
        char root[300], type[4];
        col_text(st, 1, root, sizeof root);
        int r = -1;
        for (int i = 0; i < ix->nroots; i++)
            if (str_eq(ix->roots[i], root)) r = i;
        col_text(st, 2, type, sizeof type);
        if (r < 0 || (type[0] != MF_VIDEO && type[0] != MF_SUB && type[0] != MF_IMAGE)) continue;
        if (ix->nfiles == cap) {
            cap *= 2;
            ix->files = xrealloc(ix->files, (size_t)cap * sizeof *ix->files);
        }
        MediaFile *f = &ix->files[ix->nfiles];
        memset(f, 0, sizeof *f);
        char hex[20];
        col_text(st, 0, hex, sizeof hex);
        if (!media_parse_id(hex, &f->id)) continue;
        f->root = r;
        f->type = type[0];
        col_text(st, 3, f->rel, sizeof f->rel);
        f->size = sqlite3_column_int64(st, 4);
        f->mtime = sqlite3_column_int64(st, 5);
        if (f->type == MF_VIDEO) {
            parse_name(root_name(ix->roots[r]), f->rel, &f->name);
            if (f->name.extra) continue; /* hidden since it was stored */
        }
        col_text(st, 6, f->probe.container, sizeof f->probe.container);
        f->probe.duration_ms = sqlite3_column_int64(st, 7);
        f->probe.width = sqlite3_column_int(st, 8);
        f->probe.height = sqlite3_column_int(st, 9);
        col_text(st, 10, f->probe.vcodec, sizeof f->probe.vcodec);
        col_text(st, 11, f->probe.acodec, sizeof f->probe.acodec);
        f->probe.audio_channels = sqlite3_column_int(st, 12);
        f->probe.playable = (Playable)sqlite3_column_int(st, 13);
        col_text(st, 14, f->probe.why, sizeof f->probe.why);
        ix->nfiles++;
    }
    index_finish(ix);
    index_free(g_ix);
    g_ix = ix;
}

static bool same_file(const MediaFile *a, const MediaFile *b) {
    return a->size == b->size && a->mtime == b->mtime && a->root == b->root && a->type == b->type &&
           str_eq(a->rel, b->rel) && a->name.kind == b->name.kind && str_eq(a->name.title, b->name.title) &&
           a->name.year == b->name.year && a->name.season == b->name.season && a->name.episode == b->name.episode &&
           a->name.episode_end == b->name.episode_end && str_eq(a->name.ep_title, b->name.ep_title) &&
           a->probe.duration_ms == b->probe.duration_ms && a->probe.playable == b->probe.playable &&
           str_eq(a->probe.container, b->probe.container) && str_eq(a->probe.vcodec, b->probe.vcodec) &&
           str_eq(a->probe.acodec, b->probe.acodec) && str_eq(a->probe.why, b->probe.why) &&
           a->probe.width == b->probe.width && a->probe.height == b->probe.height &&
           a->probe.audio_channels == b->probe.audio_channels;
}

/* Write only what changed between two indexes: a quiet library costs no disk writes. */
static void cache_store(const MediaIndex *old, const MediaIndex *nw) {
    db_begin();
    sqlite3_stmt *up = NULL;
    int written = 0, removed = 0;
    for (int i = 0; i < nw->nfiles; i++) {
        const MediaFile *f = &nw->files[i];
        const MediaFile *o = index_find(old, f->id);
        if (o && old->nroots > o->root && str_eq(old->roots[o->root], nw->roots[f->root]) && same_file(o, f)) continue;
        up = db_prep("INSERT OR REPLACE INTO media_file (id, root, type, rel, size, mtime, kind, title, year, season, "
                     "episode, episode_end, ep_title, container, duration_ms, width, height, vcodec, acodec, channels, "
                     "playable, why) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        if (!up) break;
        char hex[17], type[2] = {f->type, 0};
        media_id_hex(f->id, hex);
        sqlite3_bind_text(up, 1, hex, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 2, nw->roots[f->root], -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 3, type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 4, f->rel, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(up, 5, f->size);
        sqlite3_bind_int64(up, 6, f->mtime);
        sqlite3_bind_int(up, 7, (int)f->name.kind);
        sqlite3_bind_text(up, 8, f->name.title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(up, 9, f->name.year);
        sqlite3_bind_int(up, 10, f->name.season);
        sqlite3_bind_int(up, 11, f->name.episode);
        sqlite3_bind_int(up, 12, f->name.episode_end);
        sqlite3_bind_text(up, 13, f->name.ep_title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 14, f->probe.container, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(up, 15, f->probe.duration_ms);
        sqlite3_bind_int(up, 16, f->probe.width);
        sqlite3_bind_int(up, 17, f->probe.height);
        sqlite3_bind_text(up, 18, f->probe.vcodec, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(up, 19, f->probe.acodec, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(up, 20, f->probe.audio_channels);
        sqlite3_bind_int(up, 21, (int)f->probe.playable);
        sqlite3_bind_text(up, 22, f->probe.why, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(up) == SQLITE_DONE) written++;
    }
    for (int i = 0; i < old->nfiles; i++) {
        if (index_find(nw, old->files[i].id)) continue;
        sqlite3_stmt *del = db_prep("DELETE FROM media_file WHERE id = ?");
        if (!del) break;
        char hex[17];
        media_id_hex(old->files[i].id, hex);
        sqlite3_bind_text(del, 1, hex, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(del) == SQLITE_DONE) removed++;
    }
    /* rows of roots that are no longer configured */
    Buf q = {0};
    buf_puts(&q, "DELETE FROM media_file WHERE root NOT IN (''");
    for (int r = 0; r < nw->nroots; r++) buf_puts(&q, ",?");
    buf_puts(&q, ")");
    sqlite3_stmt *gone = db_prep(buf_cstr(&q));
    for (int r = 0; gone && r < nw->nroots; r++) sqlite3_bind_text(gone, r + 1, nw->roots[r], -1, SQLITE_TRANSIENT);
    if (gone) sqlite3_step(gone);
    buf_free(&q);
    db_commit();
    if (written || removed) log_info("media: library cache +%d -%d", written, removed);
}

/* ========================================================= scan control */

static void swap_in(MediaIndex *nw) {
    MediaIndex *old = g_ix;
    cache_store(old ? old : &g_empty, nw);
    g_ix = nw;
    index_free(old);
    app_changed();
}

static void publish(MediaIndex *ix) {
    lock_take(&g_lock);
    index_free(g_pending);
    g_pending = ix;
    lock_give(&g_lock);
}

static void scan_thread(void *arg) {
    Scan *sc = arg;
    MediaIndex *ix = scan_run(sc);
    if (ix) publish(ix);
    free(sc);
    atomic_store(&g_scanning, false);
    app_tick_soon(); /* show it now, not at the next idle tick (up to 5 s) */
}

/* Swap in what the scanner thread finished (event loop only). */
static bool adopt_pending(int64_t now) {
    lock_take(&g_lock);
    MediaIndex *nw = g_pending;
    g_pending = NULL;
    lock_give(&g_lock);
    if (!nw) return false;
    nw->scanned_at = now_sec();
    nw->scan_ms = (int)(now - g_scan_t0);
    log_info("media: scanned %d files in %d ms (%d probed)", nw->nfiles, nw->scan_ms, nw->probed);
    swap_in(nw);
    return true;
}

static Scan *scan_new(void) {
    /* A finished scan the tick hasn't swapped in yet goes in first: the new scan compares against
       (and reuses the probes of) the newest index, and the tick must never free an index that a
       running scan still reads. */
    adopt_pending(now_ms());
    Scan *sc = xcalloc(1, sizeof *sc);
    sc->nroots = media_roots(sc->roots, MEDIA_MAX_ROOTS);
    sc->old = media_lib();
    atomic_store(&g_seen, 0);
    return sc;
}

bool media_scanning(void) { return atomic_load(&g_scanning); }
int media_scan_seen(void) { return atomic_load(&g_seen); }

void media_scan_start(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_scanning, &expected, true)) {
        g_rescan_again = true; /* settings changed mid-scan: go again afterwards */
        return;
    }
    if (app_testing()) { /* tests scan on their own thread: media_scan_sync() */
        atomic_store(&g_scanning, false);
        return;
    }
    Scan *sc = scan_new();
    g_scan_t0 = now_ms();
    if (!thread_spawn(scan_thread, sc)) {
        free(sc);
        atomic_store(&g_scanning, false);
        log_error("media: cannot start the scanner thread");
    }
}

void media_scan_sync(void) {
    int64_t t0 = now_ms();
    Scan *sc = scan_new();
    MediaIndex *ix = scan_run(sc);
    free(sc);
    if (!ix) return;
    ix->scanned_at = now_sec();
    ix->scan_ms = (int)(now_ms() - t0);
    swap_in(ix);
}

void media_scan_publish_sync(void) {
    Scan *sc = scan_new();
    MediaIndex *ix = scan_run(sc);
    free(sc);
    if (ix) publish(ix);
}

void media_scan_pretend(bool running) { atomic_store(&g_scanning, running); }
bool media_rescan_queued(void) { return g_rescan_again; }

static void watch_map_reset(void);

void media_lib_start(void) {
    watch_map_reset();
    atomic_store(&g_abort, false);
    g_rescan_again = false;
    /* the cache's own format (issue #38: CREATE TABLE IF NOT EXISTS never changes an old table, so a
       new column would have broken the cache quietly). Older ones go; the next scan fills it again. */
    bool current = setting_int("media.cache_format", CACHE_FORMAT) == CACHE_FORMAT; /* (none yet: today's) */
    if (!current && db_exec("DROP TABLE IF EXISTS media_file")) {
        log_info("media: the library cache's layout changed - the next scan fills it again");
        current = true;
    }
    if (current && setting_int("media.cache_format", 0) != CACHE_FORMAT) setting_set_int("media.cache_format", CACHE_FORMAT);
    db_exec(SCHEMA);
    cache_load();
    g_next_scan = now_ms() + RESCAN_EVERY_MS;
    media_scan_start();
}

void media_lib_tick(int64_t now) {
    adopt_pending(now);
    /* asked for while a scan ran — checked on every tick, as the request can land after the
       finished scan was already swapped in */
    if (g_rescan_again && !media_scanning()) {
        g_rescan_again = false;
        media_scan_start();
    }
    if (media_scanning() && now - g_last_progress >= 1000) {
        g_last_progress = now;
        app_changed_where("/media"); /* scan progress on the library pages */
    }
    if (!app_testing() && now >= g_next_scan) {
        g_next_scan = now + RESCAN_EVERY_MS;
        media_scan_start();
    }
}

void media_lib_stop(void) {
    watch_map_reset();
    atomic_store(&g_abort, true);
    lock_take(&g_lock);
    index_free(g_pending);
    g_pending = NULL;
    lock_give(&g_lock);
    if (!media_scanning()) index_free(g_ix); /* a running scanner still reads it: leak it rather than race */
    g_ix = NULL;                             /* (and never free it on a later start) */
    g_rescan_again = false;
}

/* =============================================================== pictures */

/* A picture set from the browser is an ordinary file the scan already looks for first:
   "poster.<ext>" in a series' show folder, "<video name>.<ext>" next to a film - so it
   survives rescans and other players (Kodi, Jellyfin) find it too. */
bool media_art_target(uint64_t id, int *root, char *dir, size_t dcap, char *stem, size_t scap) {
    const MediaSeries *s = media_series(id);
    if (s && s->count) {
        const MediaFile *first = media_episode(s, 0); /* the episode index_finish looks from */
        *root = first->root;
        str_ncopy(dir, dcap, first->rel, show_dir_len(first->rel, dir_len(first->rel)));
        str_copy(stem, scap, "poster");
        return true;
    }
    const MediaFile *f = media_file(id);
    if (!f || f->type != MF_VIDEO || f->role || f->name.kind == MEDIA_EPISODE) return false; /* episodes: the series' */
    *root = f->root;
    str_ncopy(dir, dcap, f->rel, dir_len(f->rel));
    video_stem(f->rel, stem, scap);
    return true;
}

bool media_art_folder(uint64_t id, char *abs, size_t acap, char *stem, size_t scap) {
    int root;
    char dir[400];
    const MediaIndex *ix = media_lib();
    if (!media_art_target(id, &root, dir, sizeof dir, stem, scap) || root < 0 || root >= ix->nroots) return false;
    if (*dir) join_path(ix->roots[root], dir, abs, acap);
    else str_copy(abs, acap, ix->roots[root]);
    return true;
}

uint64_t media_art_current(uint64_t id) {
    int root;
    char dir[400], stem[400];
    if (!media_art_target(id, &root, dir, sizeof dir, stem, sizeof stem)) return 0;
    const MediaIndex *ix = media_lib();
    size_t dl = strlen(dir), sl = strlen(stem);
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        if (f->type != MF_IMAGE || f->root != root || dir_len(f->rel) != dl || strncmp(f->rel, dir, dl) != 0) continue;
        const char *b = base_of(f->rel), *dot = strrchr(b, '.');
        if (dot && (size_t)(dot - b) == sl && span_ieq(b, stem, sl)) return f->id;
    }
    return 0;
}

int media_art_prune(const char *dir, const char *stem, const char *keep) {
    char found[8][512];
    int n = 0;
    size_t sl = strlen(stem);
    DirIter *it = dir_open(dir);
    if (!it) return 0;
    DirEntry e;
    while (n < (int)countof(found) && dir_next(it, &e)) { /* collect first: no deleting mid-listing */
        const char *dot = strrchr(e.name, '.');
        if (e.is_dir || !dot || (size_t)(dot - e.name) != sl || !span_ieq(e.name, stem, sl) ||
            !media_is_image_ext(e.name) || (keep && str_eq(e.name, keep)))
            continue;
        str_copy(found[n++], sizeof found[0], e.name);
    }
    dir_close(it);
    int removed = 0;
    for (int i = 0; i < n; i++) {
        char path[1100];
        join_path(dir, found[i], path, sizeof path);
        removed += file_remove(path);
    }
    return removed;
}

/* =============================================================== progress */

static const FieldDef watch_fields[] = {
    FIELD(MediaWatch, id, F_INT),  FIELD(MediaWatch, person, F_INT), FIELD(MediaWatch, item, F_STR),
    FIELD(MediaWatch, pos, F_INT), FIELD(MediaWatch, dur, F_INT),    FIELD(MediaWatch, done, F_BOOL),
    FIELD(MediaWatch, updated, F_I64),
};
Table T_media_watch = TABLE("media_watch", MediaWatch, watch_fields);

/* (person, item) -> row index, so series pages can ask about every episode
 * cheaply. Rows are only ever appended (ids grow), so indices stay valid; the
 * map is rebuilt whenever the row count changes. */
static int *g_wmap;
static int g_wcap, g_wcount = -1;

static uint32_t wslot(int person, uint64_t id) {
    uint64_t h = (id ^ ((uint64_t)(uint32_t)person * 0x9E3779B97F4A7C15ULL)) * 0xFF51AFD7ED558CCDULL;
    return (uint32_t)(h >> 32) & (uint32_t)(g_wcap - 1);
}

static void wmap_rebuild(void) {
    int n = tbl_count(&T_media_watch);
    g_wcap = 64;
    while (g_wcap < n * 2) g_wcap *= 2;
    g_wmap = xrealloc(g_wmap, (size_t)g_wcap * sizeof *g_wmap);
    for (int i = 0; i < g_wcap; i++) g_wmap[i] = -1;
    for (int i = 0; i < n; i++) {
        MediaWatch *w = tbl_at(&T_media_watch, i);
        uint64_t id = 0;
        if (!media_parse_id(w->item, &id)) continue;
        uint32_t h = wslot(w->person, id);
        while (g_wmap[h] >= 0) h = (h + 1) & (uint32_t)(g_wcap - 1);
        g_wmap[h] = i;
    }
    g_wcount = n;
}

static void watch_map_reset(void) { g_wcount = -1; } /* the table was reloaded */

MediaWatch *media_watch_get(int person, uint64_t id) {
    if (g_wcount != tbl_count(&T_media_watch)) wmap_rebuild();
    for (uint32_t h = wslot(person, id); g_wmap[h] >= 0; h = (h + 1) & (uint32_t)(g_wcap - 1)) {
        MediaWatch *w = tbl_at(&T_media_watch, g_wmap[h]);
        uint64_t wid = 0;
        if (w->person == person && media_parse_id(w->item, &wid) && wid == id) return w;
    }
    return NULL;
}

bool media_watch_report(int person, uint64_t id, int pos, int dur, bool ended, int64_t now) {
    MediaWatch *w = media_watch_get(person, id);
    MediaWatch rec = w ? *w : (MediaWatch){0};
    bool was_done = rec.done;
    rec.person = person;
    media_id_hex(id, rec.item);
    rec.pos = CLAMP(pos, 0, 7 * 86400);
    if (dur > 0) rec.dur = CLAMP(dur, 0, 7 * 86400);
    if (ended || (rec.dur > 0 && rec.pos >= rec.dur * 92 / 100)) rec.done = true;
    rec.updated = now;
    if (!w || rec.done != was_done || now - rec.saved >= 15) {
        rec.saved = now;
        tbl_put(&T_media_watch, &rec);
    } else {
        tbl_put_quiet(&T_media_watch, &rec);
    }
    return rec.done != was_done;
}

void media_watch_set_done(int person, uint64_t id, bool done, int64_t now) {
    MediaWatch *w = media_watch_get(person, id);
    MediaWatch rec = w ? *w : (MediaWatch){0};
    rec.person = person;
    media_id_hex(id, rec.item);
    rec.done = done;
    if (!done) rec.pos = 0; /* "unwatch" starts over */
    rec.updated = now;
    rec.saved = now;
    tbl_put(&T_media_watch, &rec);
}

void media_watch_move(uint64_t from, uint64_t to) {
    char hf[17], ht[17];
    media_id_hex(from, hf);
    media_id_hex(to, ht);
    for (int i = tbl_count(&T_media_watch) - 1; i >= 0; i--) {
        if (i >= tbl_count(&T_media_watch)) continue; /* (a delete below shortened the table) */
        MediaWatch rec = *(const MediaWatch *)tbl_at(&T_media_watch, i);
        if (!str_eq(rec.item, hf)) continue;
        const MediaWatch *there = media_watch_get(rec.person, to);
        if (there) { /* progress in both: keep what says more - watched, or further along */
            MediaWatch keep = *there;
            if (!keep.done && (rec.done || rec.pos > keep.pos)) {
                keep.done = rec.done, keep.pos = rec.pos, keep.dur = rec.dur, keep.updated = rec.updated;
                tbl_put(&T_media_watch, &keep);
            }
            tbl_del(&T_media_watch, rec.id);
        } else {
            str_copy(rec.item, sizeof rec.item, ht);
            tbl_put(&T_media_watch, &rec);
        }
        g_wcount = -1; /* the (person, id) map: build it again */
    }
}
