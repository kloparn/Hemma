/* modules/media/kids.c — folders that aren't for children (issue #44).
 *
 * An admin marks folders of the library "Not for children" on the Folders tab (table media_no_kids: a
 * library folder and a folder below it, "" = all of it). A child - signed in, or picked under "Who's
 * watching?" on a TV - gets the library without what's in them: media_lib_for() hands them a copy of the
 * index's lists with those videos left out, and every series with an episode among them (a show is a
 * show). The copy is made again when the library or the marks change. Whatever is opened by its id - a
 * watch page, a stream, Continue watching, a channel - asks media_kids_ok() too: the lists alone aren't
 * the lock. */
#include "media.h"

typedef struct {
    int id;
    char root[300]; /* the library folder, as Library settings has it */
    char dir[400];  /* the folder below it, '/' separators; "" = the whole library folder */
} NoKids;

static const FieldDef nokids_fields[] = {
    FIELD(NoKids, id, F_INT),
    FIELD(NoKids, root, F_STR),
    FIELD(NoKids, dir, F_STR),
};
static Table T_no_kids = TABLE("media_no_kids", NoKids, nokids_fields);

void media_kids_init(void) {
    T_no_kids.what = "folders not for children";
    store_register(&T_no_kids);
}

bool media_kid(const Ctx *c) {
    if (!c || !c->me) return false;
    if (c->me->role == ROLE_CHILD) return true;
    if (c->me->role != ROLE_DISPLAY) return false;
    const Person *v = person_get(ctx_viewer(c)); /* (the TV: whoever was picked) */
    return v && v->role == ROLE_CHILD;
}

/* ------------------------------------------------------------ the marks */

static const NoKids *mark_find(const char *root, const char *dir) {
    for (int i = 0; i < tbl_count(&T_no_kids); i++) {
        const NoKids *m = tbl_at(&T_no_kids, i);
        if (str_eq(m->root, root) && str_eq(m->dir, dir)) return m;
    }
    return NULL;
}

/* rel lies in dir (dir itself counts for a folder; "" holds everything) */
static bool under(const char *rel, const char *dir) {
    size_t n = strlen(dir);
    return !n || (strncmp(rel, dir, n) == 0 && (rel[n] == '/' || rel[n] == '\0'));
}

bool media_kids_marked(const char *root, const char *dir) { return mark_find(root, dir) != NULL; }

const char *media_kids_covered(const char *root, const char *dir) {
    for (int i = 0; i < tbl_count(&T_no_kids); i++) {
        const NoKids *m = tbl_at(&T_no_kids, i);
        if (str_eq(m->root, root) && !str_eq(m->dir, dir) && under(dir, m->dir)) return m->dir;
    }
    return NULL;
}

int media_kids_count(void) { return tbl_count(&T_no_kids); }

bool media_kids_at(int i, char *root, size_t rcap, char *dir, size_t dcap) {
    if (i < 0 || i >= tbl_count(&T_no_kids)) return false;
    const NoKids *m = tbl_at(&T_no_kids, i);
    str_copy(root, rcap, m->root);
    str_copy(dir, dcap, m->dir);
    return true;
}

void media_kids_set(const char *root, const char *dir, bool on) {
    const NoKids *m = mark_find(root, dir);
    if (on && !m) {
        NoKids rec = {0};
        str_copy(rec.root, sizeof rec.root, root);
        str_copy(rec.dir, sizeof rec.dir, dir);
        tbl_put(&T_no_kids, &rec);
    } else if (!on && m) {
        tbl_del(&T_no_kids, m->id);
    }
}

/* ----------------------------------------------------- the children's lists */

static MediaIndex g_kids;  /* the copy: its own lists; files, episodes and roots are the library's */
static bool *g_hidden;     /* per file of the library: left out */
static uint64_t g_sig;      /* what it was made from */

static uint64_t marks_sig(const MediaIndex *ix) {
    uint64_t h = hash64("kids", 4) ^ ((uint64_t)media_lib_gen() << 32) ^ (uint64_t)(uintptr_t)ix;
    for (int i = 0; i < tbl_count(&T_no_kids); i++) {
        const NoKids *m = tbl_at(&T_no_kids, i);
        h = h * 1099511628211ULL ^ hash64(m->root, strlen(m->root));
        h = h * 1099511628211ULL ^ hash64(m->dir, strlen(m->dir));
    }
    return h;
}

static int *keep_list(const int *in, int n, int *out_n) {
    int *out = xmalloc((size_t)(n ? n : 1) * sizeof *out), k = 0;
    for (int i = 0; i < n; i++)
        if (!g_hidden[in[i]]) out[k++] = in[i];
    *out_n = k;
    return out;
}

static void kids_free(void) {
    free(g_kids.movies);
    free(g_kids.others);
    free(g_kids.recent);
    free(g_kids.extras);
    free(g_kids.series);
    free(g_hidden);
    memset(&g_kids, 0, sizeof g_kids);
    g_hidden = NULL;
}

static void refresh(const MediaIndex *ix) {
    uint64_t sig = marks_sig(ix);
    if (g_hidden && sig == g_sig) return;
    kids_free();
    g_sig = sig;
    g_hidden = xcalloc((size_t)(ix->nfiles ? ix->nfiles : 1), sizeof *g_hidden);
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        if (f->root < 0 || f->root >= ix->nroots) continue;
        for (int k = 0; k < tbl_count(&T_no_kids) && !g_hidden[i]; k++) {
            const NoKids *m = tbl_at(&T_no_kids, k);
            g_hidden[i] = str_eq(m->root, ix->roots[f->root]) && under(f->rel, m->dir);
        }
    }
    /* a series with an episode in a marked folder goes whole, its extras with it */
    g_kids = *ix;
    g_kids.left = g_kids.parts = NULL;
    g_kids.nleft = g_kids.nleft_all = g_kids.nparts = g_kids.nparts_all = 0;
    g_kids.series = xmalloc((size_t)(ix->nseries ? ix->nseries : 1) * sizeof *g_kids.series);
    g_kids.nseries = g_kids.nepisodes = 0;
    uint64_t *gone = xmalloc((size_t)(ix->nseries ? ix->nseries : 1) * sizeof *gone);
    int ngone = 0;
    for (int i = 0; i < ix->nseries; i++) {
        const MediaSeries *s = &ix->series[i];
        bool hide = false;
        for (int e = 0; e < s->count && !hide; e++) hide = g_hidden[ix->episodes[s->first + e]];
        if (!hide) {
            g_kids.series[g_kids.nseries++] = *s;
            g_kids.nepisodes += s->count;
            continue;
        }
        gone[ngone++] = s->id;
        for (int e = 0; e < s->count; e++) g_hidden[ix->episodes[s->first + e]] = true;
    }
    for (int i = 0; i < ix->nextras && ngone; i++) {
        int x = ix->extras[i];
        for (int k = 0; k < ngone && !g_hidden[x]; k++) g_hidden[x] = ix->files[x].series == gone[k];
    }
    free(gone);
    g_kids.movies = keep_list(ix->movies, ix->nmovies, &g_kids.nmovies);
    g_kids.others = keep_list(ix->others, ix->nothers, &g_kids.nothers);
    g_kids.recent = keep_list(ix->recent, ix->nrecent, &g_kids.nrecent);
    g_kids.extras = keep_list(ix->extras, ix->nextras, &g_kids.nextras);
}

const MediaIndex *media_lib_for(const Ctx *c) {
    const MediaIndex *ix = media_lib();
    if (!tbl_count(&T_no_kids) || !media_kid(c)) return ix;
    refresh(ix);
    return &g_kids;
}

bool media_kids_ok(const Ctx *c, const MediaFile *f) {
    if (!f || !tbl_count(&T_no_kids) || !media_kid(c)) return true;
    const MediaIndex *ix = media_lib();
    refresh(ix);
    ptrdiff_t i = f - ix->files;
    return i < 0 || i >= ix->nfiles || !g_hidden[i];
}

bool media_kids_series_ok(const Ctx *c, const MediaSeries *s) {
    return !s || !s->count || media_kids_ok(c, media_episode(s, 0)); /* (all of it, or none) */
}

void media_kids_stop(void) { kids_free(); }
