/* modules/media/remove.c — admins delete a film (or another video) from the disk.
 *
 * What goes: the video, its play-everywhere copy, its subtitles and its picture. When the
 * film has a folder of its own ("Movies/Hocus Pocus (1993)/…": the folder's name is the
 * film's, and nothing else of the library lives in it but the film's extras), the whole
 * folder goes - extras, .nfo files and all. A folder holding anything else (a flat "Movies"
 * folder, another film, a video the library doesn't know), a symbolic link anywhere inside,
 * or more than RM_MAX entries keeps the folder: then only the film's own files go. Folders
 * left empty are removed up to the library folder, never the library folder itself.
 *
 * An episode, a season or a whole series (the Manage view, manage.c) goes the same way: its files, and
 * its own folders when nothing else is in them - planned from the index for the dialog, deleted on a
 * thread (media_purge_*: a series of disc rips is a thousand files).
 *
 * Also the other kind of removing: a video with a play-everywhere copy next to it (x.mkv +
 * x.web.mp4) is two versions of the same thing. Of those, the one that plays in fewer places
 * goes (media_lesser_version) - by hand from the library settings, or right after converting
 * when the setting "media.drop_originals" is on (convert.c). */
#include "remove.h"

#define RM_MAX 400
#define RM_DEPTH 4

static void add(RemovePlan *p, const char *path, const char *rel, bool dir, int64_t size) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 16;
        p->items = xrealloc(p->items, (size_t)p->cap * sizeof *p->items);
    }
    RemoveItem *it = &p->items[p->n++];
    str_copy(it->path, sizeof it->path, path);
    str_copy(it->rel, sizeof it->rel, rel);
    it->dir = dir;
    it->size = size;
    if (!dir) p->files++, p->bytes += size;
}

static void add_file(RemovePlan *p, uint64_t id) {
    const MediaFile *f = id ? media_file(id) : NULL;
    char full[1024];
    if (!f || !media_full_path(f, full, sizeof full)) return;
    for (int i = 0; i < p->n; i++)
        if (str_eq(p->items[i].path, full)) return;
    add(p, full, f->rel, false, f->size);
}

static bool join(char *out, size_t cap, const char *root, const char *rel) {
    size_t rl = strlen(root);
    bool slash = rl && root[rl - 1] == '/';
    return snprintf(out, cap, "%s%s%s", root, slash ? "" : "/", rel) < (int)cap;
}

/* "Movies/Hocus Pocus (1993) (1080p BluRay x265) REPACK" for "Hocus Pocus" (1993): the folder is
   the film's own when its name says the same title. */
static bool own_folder(const MediaFile *f, char *dir, size_t cap) {
    const char *slash = strrchr(f->rel, '/');
    if (!slash || (size_t)(slash - f->rel) >= cap) return false; /* right in the library folder */
    str_ncopy(dir, cap, f->rel, (size_t)(slash - f->rel));
    const char *name = strrchr(dir, '/');
    name = name ? name + 1 : dir;
    char probe[300], a[160], b[160];
    snprintf(probe, sizeof probe, "%s.mkv", name);
    MediaName n;
    media_parse_name(probe, &n);
    fold_copy(a, sizeof a, n.title);
    fold_copy(b, sizeof b, f->name.title);
    return a[0] && str_eq(a, b) && (!n.year || !f->name.year || n.year == f->name.year);
}

/* A video found in the film's folder: the film itself, its copy, or one of its extras. */
static bool belongs(const MediaFile *film, const char *root, const char *rel) {
    const MediaFile *v = media_file(media_make_id(root, rel));
    if (!v) { /* trailers, samples and bonus material never get into the index (library.c hides them) */
        MediaName n;
        media_parse_name(rel, &n);
        return n.extra; /* anything else the library doesn't know (yet): don't guess */
    }
    if (v->id == film->id || v->id == film->web) return true;
    if (v->role == MF_WEBCOPY) { /* an extra's copy: its original decides */
        const MediaIndex *ix = media_lib();
        for (int i = 0; i < ix->nfiles; i++)
            if (ix->files[i].web == v->id) return ix->files[i].name.extra || ix->files[i].role != MF_MAIN;
        return false;
    }
    return v->name.extra || v->role != MF_MAIN;
}

typedef struct {
    RemovePlan *p;
    const MediaFile *film;
    bool ok;
} Walk;

static void walk(Walk *w, const char *abs, const char *rel, int depth) {
    DirIter *it = depth <= RM_DEPTH ? dir_open(abs) : NULL;
    if (!it) {
        w->ok = false;
        return;
    }
    DirEntry e;
    while (w->ok && dir_next(it, &e)) {
        char a[1024], r[400];
        if (e.is_link || w->p->n >= RM_MAX || snprintf(a, sizeof a, "%s/%s", abs, e.name) >= (int)sizeof a ||
            snprintf(r, sizeof r, "%s/%s", rel, e.name) >= (int)sizeof r) {
            w->ok = false;
        } else if (e.is_dir) {
            walk(w, a, r, depth + 1);
            if (w->ok) add(w->p, a, r, true, 0); /* after what's inside it */
        } else if (media_is_video_ext(e.name) && !belongs(w->film, w->p->root, r)) {
            w->ok = false;
        } else {
            add(w->p, a, r, false, e.size);
        }
    }
    dir_close(it);
}

void media_remove_free(RemovePlan *p) {
    free(p->items);
    p->items = NULL;
    p->n = p->cap = p->files = 0;
    p->bytes = 0;
    p->folder[0] = '\0';
}

bool media_remove_plan(const MediaFile *f, RemovePlan *p) {
    memset(p, 0, sizeof *p);
    const MediaIndex *ix = media_lib();
    if (!f || f->type != MF_VIDEO || f->role != MF_MAIN || f->name.kind == MEDIA_EPISODE) return false;
    if (f->root < 0 || f->root >= ix->nroots) return false;
    str_copy(p->root, sizeof p->root, ix->roots[f->root]);
    char dir[400], abs[1024];
    if (own_folder(f, dir, sizeof dir) && join(abs, sizeof abs, p->root, dir)) {
        Walk w = {p, f, true};
        walk(&w, abs, dir, 0);
        if (w.ok) {
            add(p, abs, dir, true, 0);
            str_copy(p->folder, sizeof p->folder, dir);
            return p->files > 0;
        }
        media_remove_free(p); /* not the film's alone: just its own files */
    }
    add_file(p, f->id);
    add_file(p, f->web);
    for (int i = 0; i < ix->nfiles; i++)
        if (ix->files[i].type == MF_SUB && ix->files[i].video == f->id) add_file(p, ix->files[i].id);
    add_file(p, f->poster);
    return p->files > 0;
}

int media_remove_run(const RemovePlan *p, int *failed) {
    int done = 0, bad = 0;
    for (int i = 0; i < p->n; i++) {
        const RemoveItem *it = &p->items[i];
        if (it->dir) bad += !dir_remove(it->path);
        else if (file_remove(it->path)) done++;
        else bad++;
    }
    /* folders left empty above (a dir_remove only takes an empty one) */
    char rel[400], abs[1024];
    str_copy(rel, sizeof rel, p->folder[0] ? p->folder : p->n ? p->items[0].rel : "");
    for (char *cut; (cut = strrchr(rel, '/')) != NULL;) {
        *cut = '\0'; /* the parent: "Movies/Film (2020)/x.mkv" -> "Movies/Film (2020)", not "Movies" */
        if (!strchr(rel, '/')) break; /* folders right in the library folder ("Movies", "Series") stay */
        if (!join(abs, sizeof abs, p->root, rel) || !dir_remove(abs)) break;
    }
    if (failed) *failed = bad;
    return done;
}

/* ------------------------------------------- an episode, a season, a series */

#define PURGE_MAX 5000 /* entries an own folder may hold to go whole (a long show of disc rips: ~1,000) */
#define PURGE_DEPTH 6

static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* "70's show" for "/mnt/media/70's show" (as library.c names a show from its library folder); "" for a drive */
const char *media_root_name(const char *root) {
    const char *s = strrchr(root, '/');
    const char *name = s ? s + 1 : root;
    size_t n = strlen(name);
    return n && name[n - 1] != ':' ? name : "";
}

static size_t dir_of(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? (size_t)(s - rel) : 0;
}

size_t media_show_dir_len(const char *rel) {
    size_t dl = dir_of(rel);
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

bool media_names_show(const char *root, const char *dir, size_t dl, const char *title) {
    char probe[600], a[160], b[160];
    if (dl) snprintf(probe, sizeof probe, "%.*s/Season 1/S01E01.mkv", (int)dl, dir);
    else str_copy(probe, sizeof probe, "Season 1/S01E01.mkv");
    MediaName n;
    media_parse_name_in(media_root_name(root), probe, &n); /* (a name of no title: the folder's is the show's, or none) */
    fold_copy(a, sizeof a, n.title);
    fold_copy(b, sizeof b, title);
    return n.kind == MEDIA_EPISODE && a[0] && str_eq(a, b);
}

static void purge_push(MediaPurge *p, uint64_t id) {
    if (!id) return;
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->ids = xrealloc(p->ids, (size_t)p->cap * sizeof *p->ids);
    }
    p->ids[p->n++] = id;
}

static void purge_dir(MediaPurge *p, int root, const char *rel, size_t len) {
    if (!len || len >= sizeof p->dirs[0].rel) return; /* never the library folder itself */
    for (int i = 0; i < p->ndirs; i++)
        if (p->dirs[i].root == root && strlen(p->dirs[i].rel) == len && strncmp(p->dirs[i].rel, rel, len) == 0) return;
    if (p->ndirs == PURGE_DIRS) return;
    p->dirs[p->ndirs].root = root;
    str_ncopy(p->dirs[p->ndirs].rel, sizeof p->dirs[0].rel, rel, len);
    p->ndirs++;
}

/* The video's season folder right below its show folder ("Show/Season 2" for "Show/Season 2/Disc - 1/x.mkv")
   when that folder's number is the video's season: its length, else 0. */
static size_t season_dir_len(const MediaFile *f) {
    size_t sd = media_show_dir_len(f->rel);
    const char *rest = f->rel + sd + (sd ? 1 : 0);
    size_t seg = strcspn(rest, "/");
    if (rest[seg] != '/' || seg >= 200) return 0; /* (the file itself) */
    char name[200];
    str_ncopy(name, sizeof name, rest, seg);
    return media_season_folder(name) == f->name.season ? (size_t)(rest - f->rel) + seg : 0;
}

/* The first `videos` pushed are videos: with them goes what goes along - their copies, own pictures and
   subtitles; then each file once, and their size. */
static bool purge_settle(MediaPurge *p, int videos) {
    const MediaIndex *ix = media_lib();
    uint64_t *v = xmalloc((size_t)(videos ? videos : 1) * sizeof *v);
    memcpy(v, p->ids, (size_t)videos * sizeof *v);
    qsort(v, (size_t)videos, sizeof *v, u64_cmp);
    for (int i = 0; i < videos; i++) {
        const MediaFile *f = media_file(v[i]);
        if (!f) continue;
        purge_push(p, f->web);
        if (f->poster) purge_push(p, f->poster);
    }
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *s = &ix->files[i];
        if (s->type == MF_SUB && s->video && bsearch(&s->video, v, (size_t)videos, sizeof *v, u64_cmp)) purge_push(p, s->id);
    }
    free(v);
    qsort(p->ids, (size_t)p->n, sizeof *p->ids, u64_cmp);
    int w = 0;
    for (int i = 0; i < p->n; i++)
        if (!w || p->ids[w - 1] != p->ids[i]) p->ids[w++] = p->ids[i];
    p->n = w;
    p->bytes = 0;
    p->videos = 0;
    for (int i = 0; i < p->n; i++) {
        const MediaFile *f = media_file(p->ids[i]);
        if (!f) continue;
        p->bytes += f->size;
        p->videos += f->type == MF_VIDEO && f->role != MF_WEBCOPY;
    }
    return p->n > 0;
}

static bool series_video(const MediaFile *f, uint64_t series) {
    return f->type == MF_VIDEO && f->role != MF_WEBCOPY && f->series == series;
}

bool media_purge_episode(const MediaFile *ep, MediaPurge *p) {
    memset(p, 0, sizeof *p);
    if (!ep || ep->type != MF_VIDEO || ep->role == MF_WEBCOPY || !ep->series) return false;
    purge_push(p, ep->id);
    return purge_settle(p, 1);
}

bool media_purge_season(const MediaSeries *s, int season, MediaPurge *p) {
    memset(p, 0, sizeof *p);
    const MediaIndex *ix = media_lib();
    if (!s) return false;
    for (int i = 0; i < ix->nfiles; i++) { /* episodes, and the season's extras (a disc's play-all, bonus clips) */
        const MediaFile *f = &ix->files[i];
        if (!series_video(f, s->id) || f->name.season != season) continue;
        purge_push(p, f->id);
        purge_dir(p, f->root, f->rel, season_dir_len(f));
    }
    return purge_settle(p, p->n);
}

bool media_purge_series(const MediaSeries *s, MediaPurge *p) {
    memset(p, 0, sizeof *p);
    const MediaIndex *ix = media_lib();
    if (!s) return false;
    struct {
        int root;
        size_t len;
        uint64_t h;
        bool names;
    } seen[16];
    int nseen = 0;
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        if (!series_video(f, s->id)) continue;
        purge_push(p, f->id);
        size_t sd = media_show_dir_len(f->rel);
        uint64_t h = hash64(f->rel, sd);
        int k = 0;
        while (k < nseen && !(seen[k].root == f->root && seen[k].len == sd && seen[k].h == h)) k++;
        bool names;
        if (k < nseen) names = seen[k].names;
        else {
            names = media_names_show(ix->roots[f->root], f->rel, sd, s->title); /* (asked once per folder) */
            if (nseen < countof(seen)) {
                seen[nseen].root = f->root;
                seen[nseen].len = sd;
                seen[nseen].h = h;
                seen[nseen].names = names;
                nseen++;
            }
        }
        if (names && sd) purge_dir(p, f->root, f->rel, sd);
        else purge_dir(p, f->root, f->rel, season_dir_len(f)); /* (no show folder of its own: its season's, if any) */
    }
    int videos = p->n;
    /* the series' picture, when it lies in a show folder of its own (not a "Series" folder's poster) */
    const MediaFile *pic = media_file(s->poster);
    if (pic && pic->root >= 0 && pic->root < ix->nroots) {
        size_t dl = dir_of(pic->rel);
        bool own = media_names_show(ix->roots[pic->root], pic->rel, dl, s->title);
        for (int i = 0; i < p->ndirs && !own; i++)
            own = p->dirs[i].root == pic->root && strlen(p->dirs[i].rel) == dl && strncmp(p->dirs[i].rel, pic->rel, dl) == 0;
        if (own) purge_push(p, pic->id); /* (after the videos: settle takes the first `videos` as theirs) */
    }
    return purge_settle(p, videos);
}

void media_purge_free(MediaPurge *p) {
    free(p->ids);
    memset(p, 0, sizeof *p);
}

PurgeJob *media_purge_job(const MediaPurge *p) {
    const MediaIndex *ix = media_lib();
    PurgeJob *j = xcalloc(1, sizeof *j);
    memcpy(j->roots, ix->roots, sizeof j->roots);
    j->nroots = ix->nroots;
    j->files = xcalloc((size_t)(p->n ? p->n : 1), sizeof *j->files);
    j->belong = xmalloc((size_t)(p->n ? p->n : 1) * sizeof *j->belong);
    for (int i = 0; i < p->n; i++) {
        const MediaFile *f = media_file(p->ids[i]);
        if (!f || f->root < 0 || f->root >= ix->nroots) continue;
        PurgeFile *pf = &j->files[j->nfiles++];
        pf->root = f->root;
        str_copy(pf->rel, sizeof pf->rel, f->rel);
        pf->size = f->size;
        pf->id = f->id;
        pf->video = f->type == MF_VIDEO;
        j->belong[j->nbelong++] = f->id;
    }
    qsort(j->belong, (size_t)j->nbelong, sizeof *j->belong, u64_cmp);
    memcpy(j->dirs, p->dirs, sizeof j->dirs);
    j->ndirs = p->ndirs;
    return j;
}

void media_purge_job_free(PurgeJob *j) {
    if (!j) return;
    free(j->files);
    free(j->belong);
    free(j);
}

typedef struct {
    char rel[400];
    bool dir;
    int64_t size;
} PurgeItem;

typedef struct {
    const PurgeJob *j;
    int root;
    PurgeItem *items;
    int n, cap;
    bool ok;
} PurgeWalk;

static void pw_add(PurgeWalk *w, const char *rel, bool dir, int64_t size) {
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->items = xrealloc(w->items, (size_t)w->cap * sizeof *w->items);
    }
    PurgeItem *it = &w->items[w->n++];
    str_copy(it->rel, sizeof it->rel, rel);
    it->dir = dir;
    it->size = size;
}

/* A video in an own folder: the item's (or its copy), or an extra by its name (trailers and samples
   never get into the index) - anything else is someone else's: the folder stays. Any thread. */
static bool pw_belongs(const PurgeWalk *w, const char *rel) {
    uint64_t id = media_make_id(w->j->roots[w->root], rel);
    if (bsearch(&id, w->j->belong, (size_t)w->j->nbelong, sizeof id, u64_cmp)) return true;
    MediaName n;
    media_parse_name(rel, &n);
    return n.extra;
}

static void pw_walk(PurgeWalk *w, const char *abs, const char *rel, int depth) {
    DirIter *it = depth <= PURGE_DEPTH ? dir_open(abs) : NULL;
    if (!it) {
        w->ok = false;
        return;
    }
    DirEntry e;
    while (w->ok && dir_next(it, &e)) {
        char a[1024], r[400];
        if (e.is_link || w->n >= PURGE_MAX || snprintf(a, sizeof a, "%s/%s", abs, e.name) >= (int)sizeof a ||
            snprintf(r, sizeof r, "%s/%s", rel, e.name) >= (int)sizeof r) {
            w->ok = false;
        } else if (e.is_dir) {
            pw_walk(w, a, r, depth + 1);
            if (w->ok) pw_add(w, r, true, 0); /* after what's inside it */
        } else if (str_iends(e.name, MEDIA_PART_SUFFIX) || str_iends(e.name, ".conv.part")) {
            w->ok = false; /* an upload or a conversion at work in there */
        } else if (media_is_video_ext(e.name) && !pw_belongs(w, r)) {
            w->ok = false;
        } else {
            pw_add(w, r, false, e.size);
        }
    }
    dir_close(it);
}

/* Another library folder inside this one (folders set up nested): it never goes with it. */
static bool holds_a_root(const PurgeJob *j, const char *abs) {
    size_t n = strlen(abs);
    for (int r = 0; r < j->nroots; r++)
        if (strncmp(j->roots[r], abs, n) == 0 && (j->roots[r][n] == '/' || j->roots[r][n] == '\0')) return true;
    return false;
}

typedef struct {
    int root;
    char rel[400];
    int depth;
} PurgeUp;

static int up_cmp(const void *x, const void *y) {
    const PurgeUp *a = x, *b = y;
    if (a->depth != b->depth) return b->depth - a->depth; /* the deepest first: a parent after its children */
    if (a->root != b->root) return a->root - b->root;
    return strcmp(a->rel, b->rel);
}

/* Every folder above rel (not the library folder, nor one right in it) as a candidate once. */
static void up_add(PurgeUp **list, int *n, int *cap, int root, const char *rel) {
    char r[400];
    str_copy(r, sizeof r, rel);
    for (char *cut; (cut = strrchr(r, '/')) != NULL;) {
        *cut = '\0';
        if (!strchr(r, '/')) break; /* right in the library folder ("Series"): stays */
        bool have = false;
        for (int i = *n - 1; i >= 0 && !have; i--) have = (*list)[i].root == root && str_eq((*list)[i].rel, r);
        if (have) break; /* (and its parents with it) */
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 32;
            *list = xrealloc(*list, (size_t)*cap * sizeof **list);
        }
        PurgeUp *u = &(*list)[(*n)++];
        u->root = root;
        str_copy(u->rel, sizeof u->rel, r);
        u->depth = 0;
        for (const char *p = r; *p; p++) u->depth += *p == '/';
    }
}

void media_purge_run(PurgeJob *j) {
    char path[1024];
    /* 1. the item's own files */
    for (int i = 0; i < j->nfiles; i++) {
        PurgeFile *f = &j->files[i];
        if (f->root < 0 || f->root >= j->nroots || !join(path, sizeof path, j->roots[f->root], f->rel)) {
            j->failed++;
        } else if (file_remove(path)) {
            f->gone = true;
            j->deleted++;
            j->freed += f->size;
        } else if (!path_stat(path, NULL, NULL, NULL)) {
            f->gone = true; /* (gone already) */
        } else {
            j->failed++;
        }
    }
    /* 2. its own folders, whole - when nothing but its files, extras and notes are left in them */
    for (int d = 0; d < j->ndirs; d++) {
        const PurgeDir *pd = &j->dirs[d];
        if (pd->root < 0 || pd->root >= j->nroots || !pd->rel[0] || !join(path, sizeof path, j->roots[pd->root], pd->rel) ||
            holds_a_root(j, path))
            continue;
        PurgeWalk w = {j, pd->root, NULL, 0, 0, true};
        pw_walk(&w, path, pd->rel, 0);
        for (int i = 0; w.ok && i < w.n; i++) {
            char p2[1024];
            if (!join(p2, sizeof p2, j->roots[pd->root], w.items[i].rel)) continue;
            if (w.items[i].dir) dir_remove(p2);
            else if (file_remove(p2)) j->deleted++, j->freed += w.items[i].size;
            else j->failed++;
        }
        if (w.ok) dir_remove(path);
        free(w.items);
    }
    /* 3. folders left empty above them */
    PurgeUp *up = NULL;
    int nup = 0, cap = 0;
    for (int i = 0; i < j->nfiles; i++) up_add(&up, &nup, &cap, j->files[i].root, j->files[i].rel);
    for (int d = 0; d < j->ndirs; d++) up_add(&up, &nup, &cap, j->dirs[d].root, j->dirs[d].rel);
    if (nup) qsort(up, (size_t)nup, sizeof *up, up_cmp);
    for (int i = 0; i < nup; i++)
        if (up[i].root >= 0 && up[i].root < j->nroots && join(path, sizeof path, j->roots[up[i].root], up[i].rel))
            dir_remove(path); /* (only an empty one goes) */
    free(up);
}

/* ------------------------------------------------- two versions of one video */

int media_compat_cost(const MediaProbe *p) {
    static const char *const easy_audio[] = {"", "aac", "mp3", "opus", "flac", NULL};
    int sev = p->playable == PLAY_YES ? 0 : p->playable == PLAY_MAYBE ? 1 : p->playable == PLAY_NO ? 2 : 3;
    bool box = str_eq(p->container, "mp4") || str_eq(p->container, "mov") || str_eq(p->container, "webm");
    bool audio = false;
    for (int i = 0; easy_audio[i]; i++) audio |= str_eq(p->acodec, easy_audio[i]);
    return sev * 4 + !box + str_eq(p->vcodec, "hevc") + !audio;
}

bool media_copy_lasts(int64_t original_ms, int64_t copy_ms) {
    if (original_ms <= 0 || copy_ms <= 0) return false;
    int64_t d = original_ms > copy_ms ? original_ms - copy_ms : copy_ms - original_ms;
    return d <= MAX((int64_t)2000, original_ms / 100);
}

const MediaFile *media_lesser_version(const MediaFile *o, const MediaFile **better) {
    if (better) *better = NULL;
    if (!o || o->type != MF_VIDEO || o->role == MF_WEBCOPY || !o->web) return NULL;
    const MediaFile *c = media_file(o->web);
    if (!c || !media_copy_lasts(o->probe.duration_ms, c->probe.duration_ms)) return NULL;
    bool copy_wins = media_compat_cost(&c->probe) <= media_compat_cost(&o->probe); /* a tie: the copy stays */
    if (better) *better = copy_wins ? c : o;
    return copy_wins ? o : c;
}

bool media_drop_original(const char *path, uint64_t original, uint64_t copy) {
    if (!file_remove(path)) return false;
    media_watch_move(original, copy);
    return true;
}

bool media_drop_version(const MediaFile *lesser, const MediaFile *better) {
    char path[1024];
    if (!lesser || !better || !media_full_path(lesser, path, sizeof path)) return false;
    if (lesser->role == MF_WEBCOPY) return file_remove(path); /* progress was the original's all along */
    return media_drop_original(path, lesser->id, better->id);
}
