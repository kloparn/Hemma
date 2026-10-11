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
