/* modules/media/manage.c — admins look after what's in the library (owner, 2026-10-21: "easier and
 * better control of the stuff inside").
 *
 * The Manage view of a series (/media/series/<id>?manage=1): season by season every episode with its
 * files, their versions and size, "Fix number" and "Remove"; "Remove season" and "Remove series". A
 * film's (or other video's) "Fix title" on its watch page. Library settings' "What takes room".
 *
 * Removing: remove.c plans it from the index (the dialog lists what goes and how much room it frees)
 * and deletes it - on a thread of its own, one removal at a time: a series of DVD rips is a thousand
 * files, and the household's pages mustn't wait for them. When it's done (the tick): everyone's watch
 * history of those videos goes, a line in Latest, a rescan.
 *
 * Fixing renames files on the disk - the library is the folders, and the next scan reads the new names.
 * Nothing is ever renamed over a file that's there; a name is cleaned for every disk, and checked the
 * way the library will read it (a fix that Hemma would misread too is refused). The watch history moves
 * to the video's new id at once (the scan fills in the rest, library.c watch_follow), and a "Not for
 * children" mark on a renamed folder goes with it (kids.c). Renaming a whole series isn't offered: its
 * id comes from its title, and TV channels and screen rows keep series ids. */
#include "manage.h"

#include <ctype.h>
#include <stdatomic.h>

#include "../../core/ui.h"
#include "convert.h"
#include "media.h"
#include "remove.h"
#include "room.h"
#include "upload.h"

/* ---------------------------------------------------------------- helpers */

static const char *base_of(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

static size_t dir_len(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? (size_t)(s - rel) : 0;
}

/* "Film (2020)" of "Film (2020).mkv" - and of "Film (2020).web.mp4": a copy is named like its original */
static size_t stem_len(const char *base) {
    size_t n = strlen(base);
    if (media_is_web_copy(base) && n > 8) return n - 8;
    const char *dot = strrchr(base, '.');
    return dot && dot != base ? (size_t)(dot - base) : n;
}

static const char *ext_of(const char *base) { return base + stem_len(base); } /* ".mkv", ".web.mp4" */

static bool join(char *out, size_t cap, const char *root, const char *rel) {
    size_t rl = strlen(root);
    if (!rel[0]) return snprintf(out, cap, "%s", root) < (int)cap;
    return snprintf(out, cap, "%s%s%s", root, rl && root[rl - 1] == '/' ? "" : "/", rel) < (int)cap;
}

/* name in folder dir ("" = the library folder itself), below the library folder */
static bool in_dir(char *out, size_t cap, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + (dl ? 1 : 0) + nl >= cap) return false;
    memcpy(out, dir, dl);
    if (dl) out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
    return true;
}

static bool fold_eq(const char *a, const char *b) {
    char x[1100], y[1100];
    fold_copy(x, sizeof x, a);
    fold_copy(y, sizeof y, b);
    return str_eq(x, y);
}

/* The same words, give or take case and punctuation: what Hemma reads back from a name, and what was typed. */
static bool same_words(const char *a, const char *b) {
    char fa[300], fb[300], x[300], y[300];
    size_t nx = 0, ny = 0;
    fold_copy(fa, sizeof fa, a);
    fold_copy(fb, sizeof fb, b);
    for (const unsigned char *p = (const unsigned char *)fa; *p && nx < sizeof x - 1; p++)
        if (isalnum(*p) || *p >= 0x80) x[nx++] = (char)*p;
    for (const unsigned char *p = (const unsigned char *)fb; *p && ny < sizeof y - 1; p++)
        if (isalnum(*p) || *p >= 0x80) y[ny++] = (char)*p;
    x[nx] = y[ny] = '\0';
    return nx && str_eq(x, y);
}

/* A name typed for a file or a folder, as every disk can store it (Windows refuses \ / : * ? " < > |): a
   colon becomes " -", a slash or a bar a dash, a double quote a single one, the rest of those go; one space
   at a time; no dots or spaces at either end (Windows drops them, and the library skips a name that starts
   with a dot). false = control characters or broken UTF-8, or nothing left. */
static bool clean_part(const char *in, char *out, size_t cap) {
    if (!in || !utf8_valid(in, strlen(in))) return false;
    char tmp[600];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char one[2] = {(char)*p, 0};
        const char *add = one;
        if (*p < 0x20 || *p == 0x7F) return false;
        if (*p == ':') add = " - ";
        else if (*p == '/' || *p == '\\' || *p == '|') add = "-";
        else if (*p == '"') add = "'";
        else if (strchr("*?<>", *p)) add = "";
        for (; *add && n + 1 < sizeof tmp; add++) {
            if (*add == ' ' && (!n || tmp[n - 1] == ' ')) continue;
            tmp[n++] = *add;
        }
    }
    tmp[n] = '\0';
    const char *s = tmp;
    while (*s == '.' || *s == ' ') s++;
    str_copy(out, cap, s);
    size_t len = strlen(out);
    while (len && (out[len - 1] == '.' || out[len - 1] == ' ')) out[--len] = '\0';
    return len > 0;
}

/* One part of a path as every disk stores it (upload.c's rules: no reserved names, at most 200 bytes ...). */
static bool name_ok(const char *name) {
    char tmp[400];
    return upload_clean_rel(name, tmp, sizeof tmp) && str_eq(tmp, name) && !strchr(name, '/');
}

static bool int_field(const Fields *fl, const char *k, long long lo, long long hi, long long *out) {
    char s[24];
    str_copy(s, sizeof s, fields_str(fl, k));
    str_trim(s);
    return str_int(s, out) && *out >= lo && *out <= hi;
}

/* Rename without ever replacing a file that's there (file_replace would). A change of case alone goes
   through a name of its own first: on a disk that ignores case, "x.mkv" and "X.mkv" are one file. */
static bool rename_safe(const char *from, const char *to, bool *taken) {
    if (taken) *taken = false;
    if (str_eq(from, to)) return true;
    if (!fold_eq(from, to)) {
        if (path_stat(to, NULL, NULL, NULL)) {
            if (taken) *taken = true;
            return false;
        }
        return file_replace(from, to);
    }
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.hemma-rename", from) >= (int)sizeof tmp || path_stat(tmp, NULL, NULL, NULL) ||
        !file_replace(from, tmp))
        return false;
    if (path_stat(to, NULL, NULL, NULL)) { /* another file after all: a disk that tells case apart */
        file_replace(tmp, from);
        if (taken) *taken = true;
        return false;
    }
    if (file_replace(tmp, to)) return true;
    file_replace(tmp, from);
    return false;
}

#define MV_MAX 64
typedef struct {
    struct {
        char from[1024], to[1024];
        bool done;
    } m[MV_MAX];
    int n;
    bool full;
} Moves;

static void mv_add(Moves *mv, const char *from, const char *to) {
    if (mv->n == MV_MAX) {
        mv->full = true;
        return;
    }
    str_copy(mv->m[mv->n].from, sizeof mv->m[0].from, from);
    str_copy(mv->m[mv->n].to, sizeof mv->m[0].to, to);
    mv->m[mv->n].done = false;
    mv->n++;
}

static bool mv_nothing(const Moves *mv) {
    for (int i = 0; i < mv->n; i++)
        if (!str_eq(mv->m[i].from, mv->m[i].to)) return false;
    return true;
}

/* A name some other file has already (before anything is touched): its name, or NULL. */
static const char *mv_taken(const Moves *mv) {
    for (int i = 0; i < mv->n; i++)
        if (!fold_eq(mv->m[i].from, mv->m[i].to) && path_stat(mv->m[i].to, NULL, NULL, NULL)) return base_of(mv->m[i].to);
    return NULL;
}

/* Every move, or none: the first that fails puts back those before it. taken: the name in the way. */
static bool mv_run(Moves *mv, char *taken, size_t cap) {
    for (int i = 0; i < mv->n; i++) {
        bool t = false;
        if (rename_safe(mv->m[i].from, mv->m[i].to, &t)) {
            mv->m[i].done = true;
            continue;
        }
        if (t) str_copy(taken, cap, base_of(mv->m[i].to));
        for (int k = i - 1; k >= 0; k--)
            if (mv->m[k].done) rename_safe(mv->m[k].to, mv->m[k].from, NULL);
        return false;
    }
    return true;
}

/* Folders left empty above dir go - never the library folder, nor a folder right in it ("Series"). */
static void prune_up(const char *root, const char *dir) {
    char r[400], abs[1024];
    str_copy(r, sizeof r, dir);
    for (char *cut; strchr(r, '/') && join(abs, sizeof abs, root, r) && dir_remove(abs);) {
        cut = strrchr(r, '/');
        *cut = '\0';
    }
}

/* ------------------------------------------------- "Not for children" marks */

static bool kids_covers(const char *root, const char *dir) {
    return media_kids_marked(root, dir) || media_kids_covered(root, dir) != NULL;
}

/* A folder renamed: its marks - its own, and those of folders inside it - go with it. */
static void kids_follow(const char *root, const char *from, const char *to) {
    int n = media_kids_count(), k = 0;
    size_t fl = strlen(from);
    char(*dirs)[400] = xmalloc((size_t)(n ? n : 1) * sizeof *dirs);
    for (int i = 0; i < n; i++) {
        char r[300], d[400];
        if (media_kids_at(i, r, sizeof r, d, sizeof d) && str_eq(r, root) && strncmp(d, from, fl) == 0 &&
            (d[fl] == '\0' || d[fl] == '/'))
            str_copy(dirs[k++], sizeof dirs[0], d);
    }
    for (int i = 0; i < k; i++) {
        char nd[600];
        snprintf(nd, sizeof nd, "%s%s", to, dirs[i] + fl);
        media_kids_set(root, dirs[i], false);
        media_kids_set(root, nd, true);
    }
    free(dirs);
}

/* ------------------------------------------------------- names for things */

/* "Garden Show S01E03", "Garden Show S01E01-E02", a disc's extra "70's Show S01 (C1_t07.mkv)": the same
   words in every language - a value in the household's Latest feed and toasts. */
static void item_label(const MediaFile *f, const MediaSeries *s, char *out, size_t cap) {
    Buf t = {0};
    if (f->name.kind != MEDIA_EPISODE) buf_printf(&t, "%s S%02d (%s)", s->title, f->name.season, base_of(f->rel));
    else if (f->name.episode_end > f->name.episode)
        buf_printf(&t, "%s S%02dE%02d-E%02d", s->title, f->name.season, f->name.episode, f->name.episode_end);
    else buf_printf(&t, "%s S%02dE%02d", s->title, f->name.season, f->name.episode);
    str_copy(out, cap, buf_cstr(&t));
    buf_free(&t);
}

/* "Garden Show S01E03 - Seeds.mkv": a name the library reads right */
static void episode_stem(char *out, size_t cap, const char *show, long long season, long long ep, long long last,
                         const char *title) {
    Buf t = {0};
    buf_printf(&t, "%s S%02lldE%02lld", show, season, ep);
    if (last > ep) buf_printf(&t, "-E%02lld", last);
    if (title && title[0]) buf_printf(&t, " - %s", title);
    str_copy(out, cap, buf_cstr(&t));
    buf_free(&t);
}

static void manage_url(char *out, size_t cap, uint64_t series, int season) {
    if (season >= 0) snprintf(out, cap, "/media/series/%016llx?manage=1&s=%d", (unsigned long long)series, season);
    else snprintf(out, cap, "/media/series/%016llx?manage=1", (unsigned long long)series);
}

/* What "Fix number" takes: an episode, or a ripped disc's bonus clip that should have been one (not a
   play-all: that's the disc's episodes together). */
static bool fixable(const MediaFile *f) {
    return f && f->type == MF_VIDEO && f->series && !media_is_playall_head(f->rel) &&
           ((f->role == MF_MAIN && f->name.kind == MEDIA_EPISODE) || f->role == MF_BONUS);
}

/* "Fix title": films, and other videos (a film without a year in its name shows up as one) - never an
   extra, a copy or an episode. */
static bool can_fix_title(const Ctx *c, const MediaFile *f) {
    return is_admin(c) && f && f->type == MF_VIDEO && f->role == MF_MAIN && !f->name.extra &&
           (f->name.kind == MEDIA_MOVIE || f->name.kind == MEDIA_OTHER);
}

static bool converting(const MediaFile *f) { return convert_queued(f->id) || (f->web && convert_queued(f->web)); }

/* A video's companions that share its name - subtitles ("x.sv.srt") and its own picture ("x.jpg",
   "x-poster.jpg") - moved along to folder dest as new_stem + the rest of their name. */
static void companions(Moves *mv, const char *root, const MediaFile *v, const char *dest, const char *new_stem) {
    const MediaIndex *ix = media_lib();
    const char *vb = base_of(v->rel);
    size_t ol = stem_len(vb);
    char old[300];
    str_ncopy(old, sizeof old, vb, ol);
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *x = &ix->files[i];
        bool sub = x->type == MF_SUB && x->video == v->id, pic = x->type == MF_IMAGE && x->id == v->poster;
        if (!sub && !pic) continue;
        const char *xb = base_of(x->rel);
        char start[300], name[600], rel[400], from[1024], to[1024];
        str_ncopy(start, sizeof start, xb, ol);
        if (strlen(xb) < ol || !fold_eq(start, old)) {
            if (pic) continue; /* (a picture named otherwise isn't the video's alone) */
            /* named for the new stem with what its name said: "x.en.sdh.2.srt"; a VobSub's .idx and
               .sub alike, so the pair stays a pair */
            const char *dot = strrchr(xb, '.');
            MediaSubTags t;
            char as[400];
            if (!media_sub_tags(vb, xb, &t)) str_copy(t.lang, sizeof t.lang, x->lang);
            snprintf(as, sizeof as, "%s.x", new_stem);
            if (!dot || !media_sub_name(as, &t, dot + 1, name, sizeof name)) continue;
        } else {
            snprintf(name, sizeof name, "%s%s", new_stem, xb + ol);
        }
        if (in_dir(rel, sizeof rel, dest, name) && join(from, sizeof from, root, x->rel) && join(to, sizeof to, root, rel))
            mv_add(mv, from, to);
    }
}

/* ------------------------------------------------------------- removing */

static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct {
    PurgeJob *job;
    int person;
    Lang lang;      /* the asker's, for the size in the last toast */
    char what[200]; /* "Garden Show S01E03", "Garden Show S01", "Garden Show": the same in every language */
    char link[64];  /* where its line in Latest leads */
} Removal;

static Removal *g_removing;   /* one at a time; the event loop's - its thread only writes the job */
static atomic_bool g_removed; /* ... and that thread is done */

/* (event loop) The run is over: whose history goes, what Latest and the asker are told, a rescan. */
static void removal_finish(Ctx *c, Removal *r) {
    PurgeJob *j = r->job;
    uint64_t *gone = xmalloc((size_t)(j->nfiles ? j->nfiles : 1) * sizeof *gone);
    int ng = 0;
    for (int i = 0; i < j->nfiles; i++)
        if (j->files[i].video && j->files[i].gone) gone[ng++] = j->files[i].id;
    qsort(gone, (size_t)ng, sizeof *gone, u64_cmp);
    for (int i = tbl_count(&T_media_watch) - 1; i >= 0 && ng; i--) { /* everyone's progress in them */
        const MediaWatch *w = tbl_at(&T_media_watch, i);
        uint64_t id = 0;
        if (media_parse_id(w->item, &id) && bsearch(&id, gone, (size_t)ng, sizeof id, u64_cmp)) tbl_del(&T_media_watch, w->id);
    }
    free(gone);
    char size[24], logged[24];
    Lang was = i18n_use(LANG_EN); /* (the log speaks English) */
    media_size_text(j->freed, logged, sizeof logged);
    i18n_use(r->lang);
    media_size_text(j->freed, size, sizeof size);
    i18n_use(was);
    const Person *who = person_get(r->person);
    log_info("media: %s removed %s: %d file%s deleted (%s), %d wouldn't go", who ? who->name : "someone", r->what,
             j->deleted, j->deleted == 1 ? "" : "s", logged, j->failed);
    if (j->deleted) activity_add(r->person, "🗑️", r->link, N_("removed %s from the library"), r->what);
    if (!j->deleted && j->failed) {
        if (c) ctx_fail(c, "%s", T("Couldn't delete it - is it playing somewhere? Try again in a moment."));
        else app_notify_person(r->person, TOAST_ERR, N_("Couldn't delete it - is it playing somewhere? Try again in a moment."));
    } else if (j->failed) {
        if (c)
            ctx_info(c,
                     TN(j->failed, "Removed %s - %d leftover file couldn't be deleted",
                        "Removed %s - %d leftover files couldn't be deleted"),
                     r->what, j->failed);
        else
            app_notify_person(r->person, TOAST_INFO,
                              TN(j->failed, "Removed %s - %d leftover file couldn't be deleted",
                                 "Removed %s - %d leftover files couldn't be deleted"),
                              r->what, j->failed);
    } else if (c) {
        ctx_ok(c, T("Removed %s - %s freed"), r->what, size);
    } else {
        app_notify_person(r->person, TOAST_OK, N_("Removed %s - %s freed"), r->what, size);
    }
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

static void removal_free(Removal *r) {
    media_purge_job_free(r->job);
    free(r);
}

static void removal_thread(void *arg) {
    Removal *r = arg;
    thread_background();
    media_purge_run(r->job);
    atomic_store(&g_removed, true);
    app_tick_soon();
}

void media_manage_tick(int64_t now) {
    (void)now;
    if (!g_removing || !atomic_load(&g_removed)) return;
    Removal *r = g_removing;
    g_removing = NULL;
    atomic_store(&g_removed, false);
    removal_finish(NULL, r);
    removal_free(r);
    app_changed();
}

void media_manage_act_remove(Ctx *c, Fields *fl) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can remove things from the library"));
        return;
    }
    if (g_removing) {
        ctx_fail(c, T("Hemma is still removing %s - try again in a moment"), g_removing->what);
        return;
    }
    const char *what = fields_str(fl, "what");
    uint64_t id = 0;
    media_parse_id(fields_str(fl, "id"), &id);
    MediaPurge p = {0};
    const MediaSeries *s = NULL;
    char label[200] = "", back[96] = "/media/series";
    bool ok = false;
    if (str_eq(what, "episode")) {
        const MediaFile *f = media_file(id);
        s = f ? media_series(f->series) : NULL;
        ok = s && media_purge_episode(f, &p);
        if (ok) {
            item_label(f, s, label, sizeof label);
            manage_url(back, sizeof back, s->id, f->name.season);
        }
    } else if (str_eq(what, "season")) {
        s = media_series(id);
        long long season = -1;
        ok = s && int_field(fl, "s", 0, 9999, &season) && media_purge_season(s, (int)season, &p);
        if (ok) {
            snprintf(label, sizeof label, "%s S%02lld", s->title, season);
            manage_url(back, sizeof back, s->id, -1);
        }
    } else if (str_eq(what, "series")) {
        s = media_series(id);
        ok = s && media_purge_series(s, &p);
        if (ok) str_copy(label, sizeof label, s->title);
    }
    if (!ok) {
        media_purge_free(&p);
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    for (int i = 0; i < p.n; i++)
        if (convert_queued(p.ids[i])) {
            media_purge_free(&p);
            ctx_fail(c, "%s", T("It's being converted right now - stop that first (Library settings)"));
            return;
        }
    Removal *r = xcalloc(1, sizeof *r);
    r->job = media_purge_job(&p);
    r->person = c->me->id;
    r->lang = c->lang;
    str_copy(r->what, sizeof r->what, label);
    if (str_eq(what, "series")) str_copy(r->link, sizeof r->link, "/media/series");
    else snprintf(r->link, sizeof r->link, "/media/series/%016llx", (unsigned long long)s->id);
    log_info("media: %s is removing %s (%d file%s)", c->me->name, r->what, p.n, p.n == 1 ? "" : "s");
    media_purge_free(&p);
    ctx_redirect(c, "%s", back);
    if (!app_testing()) {
        g_removing = r;
        atomic_store(&g_removed, false);
        if (thread_spawn(removal_thread, r)) {
            ctx_info(c, T("Removing %s…"), r->what);
            return;
        }
        g_removing = NULL; /* (no thread: here and now) */
    }
    media_purge_run(r->job); /* tests: here and now */
    removal_finish(c, r);
    removal_free(r);
}

/* ---------------------------------------------------------------- fixing */

/* The season's folder in a show's folder: the one there ("Season 2", "S02", "season 2 70's show"), else
   "Season 2" ("Specials" for season 0) - made when the files move in. */
static void season_folder(const char *root, const char *show_dir, int season, char *out, size_t cap) {
    char abs[1024];
    if (season) snprintf(out, cap, "Season %d", season);
    else str_copy(out, cap, "Specials");
    DirIter *it = join(abs, sizeof abs, root, show_dir) ? dir_open(abs) : NULL;
    if (!it) return;
    DirEntry e;
    while (dir_next(it, &e))
        if (e.is_dir && !e.is_link && media_season_folder(e.name) == season) {
            str_copy(out, cap, e.name);
            break;
        }
    dir_close(it);
}

/* Is a folder between the show's folder and the file one of that season's ("Show/Season 1/Disc - 2/x.mkv")? */
static bool in_season_folder(const char *rel, size_t show_dl, int season) {
    const char *p = rel + show_dl + (show_dl ? 1 : 0);
    for (;;) {
        size_t n = strcspn(p, "/");
        if (!p[n]) return false; /* (the file's own name) */
        char seg[200];
        str_ncopy(seg, sizeof seg, p, n);
        if (media_season_folder(seg) == season) return true;
        p += n + 1;
    }
}

static void fail_moves(Ctx *c, const char *taken) {
    if (taken && taken[0]) ctx_fail(c, T("There's already a file called %s there"), taken);
    else ctx_fail(c, "%s", T("Couldn't rename it - is it playing somewhere? Try again in a moment."));
}

/* Nothing to do, a name that's taken, too many files: said, and true. */
static bool moves_refused(Ctx *c, const Moves *mv) {
    const char *taken = mv_taken(mv);
    if (taken) {
        bool folder = false;
        for (int i = 0; i < mv->n && !folder; i++)
            if (str_eq(base_of(mv->m[i].to), taken)) folder = path_is_dir(mv->m[i].to);
        if (folder) ctx_fail(c, T("There's already a folder called %s there"), taken);
        else ctx_fail(c, T("There's already a file called %s there"), taken);
    } else if (mv->full) {
        ctx_fail(c, "%s", T("Couldn't rename it - is it playing somewhere? Try again in a moment."));
    } else if (mv_nothing(mv)) {
        ctx_info(c, "%s", T("That's its name already"));
    } else {
        return false;
    }
    return true;
}

void media_manage_act_fix_episode(Ctx *c, Fields *fl) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can fix names in the library"));
        return;
    }
    const MediaIndex *ix = media_lib();
    uint64_t id = 0;
    const MediaFile *e = media_parse_id(fields_str(fl, "id"), &id) ? media_file(id) : NULL;
    const MediaSeries *s = e ? media_series(e->series) : NULL;
    if (!fixable(e) || !s || e->root < 0 || e->root >= ix->nroots) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    if (converting(e)) {
        ctx_fail(c, "%s", T("It's being converted right now - stop that first (Library settings)"));
        return;
    }
    long long season, ep, last;
    if (!int_field(fl, "season", 0, 999, &season)) {
        ctx_fail_at(c, "season", "%s", T("A season number: 0 for specials, else 1 and up"));
        return;
    }
    if (!int_field(fl, "episode", 1, 999, &ep)) {
        ctx_fail_at(c, "episode", "%s", T("An episode number: 1 and up"));
        return;
    }
    last = ep;
    if (!str_blank(fields_str(fl, "last")) && !int_field(fl, "last", ep, ep + 20, &last)) {
        ctx_fail_at(c, "last", "%s", T("The last episode comes after the first - or leave it empty"));
        return;
    }
    char title[128] = "", show[128];
    if (!str_blank(fields_str(fl, "title")) && !clean_part(fields_str(fl, "title"), title, sizeof title)) {
        ctx_fail_at(c, "title", "%s", T("That title has characters a disk can't store"));
        return;
    }
    if (!clean_part(s->title, show, sizeof show)) str_copy(show, sizeof show, "Show");
    const char *root = ix->roots[e->root];
    char stem[300], name[400], cur[400], dest[400], folder[200] = "", rel[400], probe[400];
    episode_stem(stem, sizeof stem, show, season, ep, last, title);
    snprintf(name, sizeof name, "%s%s", stem, ext_of(base_of(e->rel)));
    str_ncopy(cur, sizeof cur, e->rel, dir_len(e->rel));
    str_copy(dest, sizeof dest, cur);
    /* into the show's season folder - when the show has a folder of its own (not a "Series" folder) */
    size_t sd = media_show_dir_len(e->rel);
    if (media_names_show(root, e->rel, sd, s->title) && !in_season_folder(e->rel, sd, (int)season)) {
        char show_dir[400];
        str_ncopy(show_dir, sizeof show_dir, e->rel, sd);
        season_folder(root, show_dir, (int)season, folder, sizeof folder);
        in_dir(dest, sizeof dest, show_dir, folder);
    }
    char probe_name[420];
    snprintf(probe_name, sizeof probe_name, "%s.mkv", stem);
    if (!name_ok(name) || (folder[0] && !name_ok(folder)) || !in_dir(rel, sizeof rel, dest, name) ||
        !in_dir(probe, sizeof probe, dest, probe_name)) {
        ctx_fail_at(c, "title", "%s", T("That name is too long, or one a disk can't store - try a shorter title"));
        return;
    }
    /* what the library will read from it */
    MediaName n;
    media_parse_name_in(media_root_name(root), probe, &n);
    if (n.kind != MEDIA_EPISODE || n.season != season || n.episode != ep || n.episode_end != last || !fold_eq(n.title, s->title)) {
        char lbl[200];
        media_label(&n, lbl, sizeof lbl);
        ctx_fail(c, T("Hemma would read that name as \"%s\" - try other words"), lbl);
        return;
    }
    Moves *mv = xcalloc(1, sizeof *mv);
    char from[1024], to[1024];
    if (join(from, sizeof from, root, e->rel) && join(to, sizeof to, root, rel)) mv_add(mv, from, to);
    const MediaFile *web = e->web ? media_file(e->web) : NULL;
    char copy[420], copy_rel[400];
    snprintf(copy, sizeof copy, "%s.web.mp4", stem);
    if (web && in_dir(copy_rel, sizeof copy_rel, dest, copy) && join(from, sizeof from, root, web->rel) &&
        join(to, sizeof to, root, copy_rel))
        mv_add(mv, from, to);
    companions(mv, root, e, dest, stem);
    if (moves_refused(c, mv)) {
        free(mv);
        return;
    }
    char dest_abs[1024], taken_now[300] = "";
    bool made = join(dest_abs, sizeof dest_abs, root, dest) && !path_is_dir(dest_abs);
    if ((made && !dir_make(dest_abs)) || !mv_run(mv, taken_now, sizeof taken_now)) {
        if (made) dir_remove(dest_abs);
        fail_moves(c, taken_now);
        free(mv);
        return;
    }
    free(mv);
    /* after: the old folder if it's empty now, everyone's progress, a "Not for children" mark */
    char was[200], now_is[200];
    item_label(e, s, was, sizeof was);
    snprintf(now_is, sizeof now_is, "%s S%02lldE%02lld", s->title, season, ep);
    if (last > ep) snprintf(now_is + strlen(now_is), sizeof now_is - strlen(now_is), "-E%02lld", last);
    uint64_t sid = s->id, old_id = e->id, new_id = media_make_id(root, rel);
    if (!str_eq(cur, dest)) {
        prune_up(root, cur);
        if (kids_covers(root, cur) && !kids_covers(root, dest)) media_kids_set(root, dest, true);
    }
    media_watch_move(old_id, new_id);
    log_info("media: %s renamed %s to %s", c->me->name, e->rel, rel);
    char link[64];
    snprintf(link, sizeof link, "/media/series/%016llx", (unsigned long long)sid);
    activity_add(c->me->id, "✏️", link, N_("renamed %s to %s"), was, now_is);
    ctx_ok(c, T("Renamed it %s"), now_is);
    char back[96];
    manage_url(back, sizeof back, sid, (int)season);
    ctx_redirect(c, "%s", back);
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

void media_manage_act_fix_title(Ctx *c, Fields *fl) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can fix names in the library"));
        return;
    }
    const MediaIndex *ix = media_lib();
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(fields_str(fl, "id"), &id) ? media_file(id) : NULL;
    if (!can_fix_title(c, f) || f->root < 0 || f->root >= ix->nroots) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    if (converting(f)) {
        ctx_fail(c, "%s", T("It's being converted right now - stop that first (Library settings)"));
        return;
    }
    char title[128];
    if (!clean_part(fields_str(fl, "title"), title, sizeof title)) {
        ctx_fail_at(c, "title", "%s", T("Type its title - in letters a disk can store"));
        return;
    }
    long long year = 0;
    if (!str_blank(fields_str(fl, "year")) && !int_field(fl, "year", 1880, 2100, &year)) {
        ctx_fail_at(c, "year", "%s", T("A year like 1993 - or leave it empty"));
        return;
    }
    const char *root = ix->roots[f->root];
    char stem[160], name[400], cur[400], dest[400], rel[400], probe[400], probe_name[200];
    if (year) snprintf(stem, sizeof stem, "%s (%lld)", title, year);
    else str_copy(stem, sizeof stem, title);
    snprintf(name, sizeof name, "%s%s", stem, ext_of(base_of(f->rel)));
    str_ncopy(cur, sizeof cur, f->rel, dir_len(f->rel));
    /* its own folder goes by the same name (remove.c: named like the film, nothing else of the library in it) */
    RemovePlan rp;
    char own[400] = "";
    if (media_remove_plan(f, &rp) && rp.folder[0] && str_eq(rp.folder, cur)) str_copy(own, sizeof own, rp.folder);
    media_remove_free(&rp);
    if (own[0]) {
        char parent[400];
        str_ncopy(parent, sizeof parent, own, dir_len(own));
        in_dir(dest, sizeof dest, parent, stem);
    } else {
        str_copy(dest, sizeof dest, cur);
    }
    snprintf(probe_name, sizeof probe_name, "%s.mkv", stem);
    if (!name_ok(name) || (own[0] && !name_ok(stem)) || !in_dir(rel, sizeof rel, dest, name) ||
        !in_dir(probe, sizeof probe, dest, probe_name)) {
        ctx_fail_at(c, "title", "%s", T("That name is too long, or one a disk can't store - try a shorter title"));
        return;
    }
    MediaName n;
    media_parse_name_in(media_root_name(root), probe, &n);
    if (f->name.kind == MEDIA_MOVIE && n.kind != MEDIA_MOVIE && !year) {
        ctx_fail_at(c, "year", "%s", T("Without a year Hemma can't tell it's a film - add the year"));
        return;
    }
    if (n.kind == MEDIA_EPISODE || (f->name.kind == MEDIA_MOVIE && n.kind != MEDIA_MOVIE) || !same_words(n.title, title) ||
        n.year != (int)year) {
        char lbl[200];
        media_label(&n, lbl, sizeof lbl);
        ctx_fail(c, T("Hemma would read that name as \"%s\" - try other words"), lbl);
        return;
    }
    /* the files, in the folder they're in; then that folder when it's the film's own */
    Moves *mv = xcalloc(1, sizeof *mv);
    char from[1024], to[1024], there[400];
    if (join(from, sizeof from, root, f->rel) && in_dir(there, sizeof there, cur, name) && join(to, sizeof to, root, there))
        mv_add(mv, from, to);
    const MediaFile *web = f->web ? media_file(f->web) : NULL;
    char copy[220];
    snprintf(copy, sizeof copy, "%s.web.mp4", stem);
    if (web && join(from, sizeof from, root, web->rel) && in_dir(there, sizeof there, cur, copy) &&
        join(to, sizeof to, root, there))
        mv_add(mv, from, to);
    companions(mv, root, f, cur, stem);
    if (own[0] && join(from, sizeof from, root, own) && join(to, sizeof to, root, dest)) mv_add(mv, from, to);
    if (moves_refused(c, mv)) {
        free(mv);
        return;
    }
    char taken_now[300] = "";
    bool ok = mv_run(mv, taken_now, sizeof taken_now);
    free(mv);
    if (!ok) {
        fail_moves(c, taken_now);
        return;
    }
    char was[200], now_is[200];
    media_label(&f->name, was, sizeof was);
    media_label(&n, now_is, sizeof now_is);
    uint64_t old_id = f->id, new_id = media_make_id(root, rel);
    bool movie = n.kind == MEDIA_MOVIE;
    log_info("media: %s renamed %s to %s", c->me->name, f->rel, rel);
    if (own[0] && !str_eq(own, dest)) kids_follow(root, own, dest);
    media_watch_move(old_id, new_id);
    activity_add(c->me->id, "✏️", movie ? "/media/movies" : "/media/browse", N_("renamed %s to %s"), was, now_is);
    ctx_ok(c, T("Renamed it %s"), now_is);
    ctx_redirect(c, "/media/watch/%016llx", (unsigned long long)new_id);
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

/* ------------------------------------------------------------- the views */

void media_manage_series_link(Ctx *c, Buf *b, const MediaSeries *s) {
    if (!is_admin(c) || !s) return;
    int sq = (int)ctx_qint(c, "s", -1);
    char href[96];
    if (str_eq(ctx_q(c, "manage"), "1")) {
        if (sq >= 0) snprintf(href, sizeof href, "/media/series/%016llx?s=%d", (unsigned long long)s->id, sq);
        else snprintf(href, sizeof href, "/media/series/%016llx", (unsigned long long)s->id);
        H(b, " <a class=\"btn\" href=\"%s\">", href);
        ui_icon(b, "check");
        H(b, "<span>%s</span></a>", T("Done"));
        return;
    }
    manage_url(href, sizeof href, s->id, sq);
    H(b, " <a class=\"btn\" href=\"%s\" title=\"%s\">", href, T("Fix episode numbers, or remove episodes, seasons or the whole series"));
    ui_icon(b, "settings");
    H(b, "<span>%s</span></a>", T("Manage"));
}

static void file_line(Buf *b, const MediaFile *f, const char *cls, const char *label) {
    H(b, "<span><code class=\"media-code\">%s</code> <span class=\"badge%r\">%s</span></span>", base_of(f->rel), cls, label);
}

static const char *const PLAY_CLS[] = {"", " ok", " warn", " bad"};

static void episode_row(Buf *b, const MediaSeries *s, const MediaFile *e, const char *here) {
    const MediaFile *web = e->web ? media_file(e->web) : NULL;
    bool ep = e->name.kind == MEDIA_EPISODE;
    char size[24], label[200];
    media_size_text(e->size + (web ? web->size : 0), size, sizeof size);
    item_label(e, s, label, sizeof label);
    H(b, "<li class=\"media-ep\" id=\"media-m-%X\"><span class=\"media-ep-n%r\">", e->id,
      ep && e->name.episode_end > e->name.episode ? " media-ep-nn" : "");
    if (!ep) ui_icon(b, e->role == MF_PLAYALL ? "play" : "film");
    else if (e->name.episode_end > e->name.episode) H(b, "%d–%d", e->name.episode, e->name.episode_end);
    else H(b, "%d", e->name.episode);
    buf_puts(b, "</span><div class=\"media-ep-t\"><b>");
    if (ep) media_ep_title(b, e);
    else if (e->role == MF_PLAYALL) H(b, "%s", T("All episodes in one"));
    else if (e->name.ep_title[0]) html_esc(b, e->name.ep_title);
    else H(b, "%s", T("Extra"));
    buf_puts(b, "</b><small>");
    if (ep) media_ep_code(b, e);
    else if (e->name.disc) H(b, T("Disc %d"), e->name.disc);
    else H(b, "%s", T("Extra"));
    if (e->probe.duration_ms) {
        char d[32];
        media_fmt_duration(e->probe.duration_ms, d, sizeof d);
        H(b, " · %s", d);
    }
    H(b, " · %s</small>", size);
    if (web) {
        file_line(b, e, "", T("Original"));
        file_line(b, web, PLAY_CLS[CLAMP((int)web->probe.playable, 0, 3)], T("Converted copy"));
    } else {
        file_line(b, e, PLAY_CLS[CLAMP((int)e->probe.playable, 0, 3)], T(media_playable_label(e->probe.playable)));
    }
    buf_puts(b, "<div class=\"ph-a\">");
    if (fixable(e))
        H(b, "<a class=\"btn\" href=\"%s&amp;fix=%X\">%s<span class=\"sr-only\"> %s</span></a>", here, e->id, T("Fix number"),
          label);
    H(b, "<a class=\"btn danger\" href=\"%s&amp;rm=%X\">", here, e->id);
    ui_icon(b, "trash");
    H(b, "<span>%s</span><span class=\"sr-only\"> %s</span></a></div></div></li>", T("Remove"), label);
}

/* What a removal takes, for its dialog: the files (the first dozen), their size, the folders that go. */
static void purge_list(Buf *b, const MediaPurge *p, const char *skip) {
    char size[24];
    media_size_text(p->bytes, size, sizeof size);
    buf_puts(b, "<p class=\"muted\">");
    H(b, TN(p->n, "%d file, %s:", "%d files, %s:"), p->n, size);
    buf_puts(b, "</p><ul class=\"media-rm-list\">");
    size_t sl = strlen(skip);
    for (int i = 0; i < p->n && i < 12; i++) {
        const MediaFile *f = media_file(p->ids[i]);
        if (!f) continue;
        const char *rel = sl && strncmp(f->rel, skip, sl) == 0 && f->rel[sl] == '/' ? f->rel + sl + 1 : f->rel;
        H(b, "<li><code class=\"media-code\">%s</code></li>", rel);
    }
    if (p->n > 12) {
        buf_puts(b, "<li class=\"muted\">");
        H(b, T("and %d more"), p->n - 12);
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ul>");
    if (p->ndirs == 1) {
        buf_puts(b, "<p class=\"muted\">");
        H(b, T("Its folder <code class=\"media-code\">%s</code> goes too, with what's left in it - unless another video is in there."),
          p->dirs[0].rel);
        buf_puts(b, "</p>");
    } else if (p->ndirs > 1) {
        buf_puts(b, "<p class=\"muted\">");
        H(b, T("Its %d folders go too, with what's left in them - unless another video is in there."), p->ndirs);
        buf_puts(b, "</p>");
    }
}

static void purge_form(Buf *b, const char *what, uint64_t id, int season, const char *close) {
    ui_form(b, "media.manage_remove", "inline");
    H(b, "<input type=\"hidden\" name=\"what\" value=\"%s\"><input type=\"hidden\" name=\"id\" value=\"%X\">", what, id);
    if (season >= 0) H(b, "<input type=\"hidden\" name=\"s\" value=\"%d\">", season);
    H(b, "<div class=\"modal-f\"><a class=\"btn\" href=\"%s\">%s</a><button class=\"btn danger\">%s</button></div>", close,
      T("Cancel"), T("Delete from the disk"));
    ui_form_end(b);
}

static void show_prefix(const MediaSeries *s, char *out, size_t cap) {
    const MediaFile *first = media_episode(s, 0);
    out[0] = '\0';
    if (first) str_ncopy(out, cap, first->rel, media_show_dir_len(first->rel));
}

static void remove_dialog(Buf *b, const MediaSeries *s, const char *rm, int season, const char *close) {
    MediaPurge p = {0};
    uint64_t xid = 0;
    const MediaFile *f = NULL;
    char prefix[400], label[200];
    show_prefix(s, prefix, sizeof prefix);
    if (str_eq(rm, "season")) {
        if (!media_purge_season(s, season, &p)) goto out;
        ui_modal(b, season ? T("Remove the season") : T("Remove the specials"), close);
        buf_puts(b, "<p>");
        if (season)
            H(b, T("Season %d of <b>%s</b> is deleted from the disk - for everyone in the household, and it can't be undone."),
              season, s->title);
        else H(b, T("The specials of <b>%s</b> are deleted from the disk - for everyone in the household, and it can't be undone."), s->title);
        buf_puts(b, "</p>");
        purge_list(b, &p, prefix);
        purge_form(b, "season", s->id, season, close);
    } else if (str_eq(rm, "series")) {
        if (!media_purge_series(s, &p)) goto out;
        ui_modal(b, T("Remove the series"), close);
        buf_puts(b, "<p>");
        H(b, T("<b>%s</b> is deleted from the disk - for everyone in the household, and it can't be undone."), s->title);
        buf_puts(b, "</p>");
        purge_list(b, &p, prefix);
        purge_form(b, "series", s->id, -1, close);
    } else if (media_parse_id(rm, &xid) && (f = media_file(xid)) != NULL && f->series == s->id && media_purge_episode(f, &p)) {
        item_label(f, s, label, sizeof label);
        ui_modal(b, f->name.kind == MEDIA_EPISODE ? T("Remove the episode") : T("Remove the extra"), close);
        buf_puts(b, "<p>");
        H(b, T("<b>%s</b> is deleted from the disk - for everyone in the household, and it can't be undone."), label);
        buf_puts(b, "</p>");
        purge_list(b, &p, prefix);
        purge_form(b, "episode", f->id, -1, close);
    } else {
        goto out;
    }
    ui_modal_end(b);
out:
    media_purge_free(&p);
}

static void fix_dialog(Buf *b, const MediaSeries *s, const MediaFile *e, const char *close) {
    bool ep = e->name.kind == MEDIA_EPISODE;
    int first = ep ? e->name.episode : 0, last = ep && e->name.episode_end > e->name.episode ? e->name.episode_end : 0;
    char show[128], stem[300], example[320];
    if (!clean_part(s->title, show, sizeof show)) str_copy(show, sizeof show, "Show");
    episode_stem(stem, sizeof stem, show, e->name.season, first ? first : 1, last, e->name.ep_title);
    snprintf(example, sizeof example, "%s%s", stem, ext_of(base_of(e->rel)));
    ui_modal(b, T("Fix the episode number"), close);
    buf_puts(b, "<p class=\"muted\">");
    H(b, T("Hemma renames its files to a name it reads right - like <code class=\"media-code\">%s</code> - and moves them into "
           "the season's folder."),
      example);
    buf_puts(b, "</p>");
    if (e->name.unnumbered || e->role == MF_BONUS)
        H(b, "<p class=\"muted media-small\">%s</p>",
          T("On a ripped disc, the titles without a number of their own are numbered in disc order, after the season's "
            "highest number."));
    ui_form(b, "media.fix_episode", "stack");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><div class=\"field-row\"><label class=\"field\"><span>%s</span>"
         "<input type=\"number\" name=\"season\" min=\"0\" max=\"999\" value=\"%d\" required></label>",
      e->id, T("Season"), e->name.season);
    H(b, "<label class=\"field\"><span>%s</span><input type=\"number\" name=\"episode\" min=\"1\" max=\"999\"", T("Episode"));
    if (first) H(b, " value=\"%d\"", first);
    buf_puts(b, " required></label></div>");
    H(b, "<label class=\"field\"><span>%s</span><input type=\"number\" name=\"last\" min=\"1\" max=\"999\"",
      T("Last episode, when the file holds more than one"));
    if (last) H(b, " value=\"%d\"", last);
    H(b, "></label><label class=\"field\"><span>%s</span><input name=\"title\" maxlength=\"100\" value=\"%s\"></label>",
      T("Episode title (optional)"), e->name.ep_title);
    H(b, "<div class=\"modal-f\"><a class=\"btn\" href=\"%s\">%s</a><button class=\"btn primary\">%s</button></div>", close,
      T("Cancel"), T("Rename"));
    ui_form_end(b);
    ui_modal_end(b);
}

static void season_name(char *out, size_t cap, int season) {
    if (season) snprintf(out, cap, T("Season %d"), season);
    else str_copy(out, cap, T("Specials"));
}

bool media_manage_series(Ctx *c, Buf *b, const MediaSeries *s) {
    if (!is_admin(c) || !s || !s->count || !str_eq(ctx_q(c, "manage"), "1")) return false;
    const MediaIndex *ix = media_lib();
    int first = media_episode(s, 0)->name.season, season = (int)ctx_qint(c, "s", first);
    bool have = false;
    for (int i = 0; i < s->count && !have; i++) have = media_episode(s, i)->name.season == season;
    if (!have) season = first;
    char here[96], t[64];
    manage_url(here, sizeof here, s->id, season);
    if (g_removing) {
        buf_puts(b, "<p class=\"notice\">");
        H(b, T("Removing %s…"), g_removing->what);
        buf_puts(b, "</p>");
    }
    if (s->nseasons > 1) {
        buf_puts(b, "<nav class=\"seg media-seasons\">");
        int last = -1;
        for (int i = 0; i < s->count; i++) {
            const MediaFile *e = media_episode(s, i);
            if (e->name.season == last) continue;
            last = e->name.season;
            char href[96];
            manage_url(href, sizeof href, s->id, last);
            season_name(t, sizeof t, last);
            H(b, "<a href=\"%s\"%r>%s</a>", href, last == season ? " class=\"on\"" : "", t);
        }
        buf_puts(b, "</nav>");
    }

    /* the season: every episode, its files and size */
    MediaPurge sp, all;
    media_purge_season(s, season, &sp);
    media_purge_series(s, &all);
    char size[24];
    media_size_text(sp.bytes, size, sizeof size);
    season_name(t, sizeof t, season);
    ui_card(b, t, NULL, NULL);
    int eps = 0;
    for (int i = 0; i < s->count; i++) eps += media_episode(s, i)->name.season == season;
    buf_puts(b, "<p class=\"muted\">");
    H(b, TN(eps, "%d episode", "%d episodes"), eps);
    H(b, " · %s</p><div class=\"media-left\"><ol class=\"media-eps media-eps-compact\">", size);
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        if (e->name.season == season) episode_row(b, s, e, here);
    }
    buf_puts(b, "</ol>");
    int nx = 0;
    for (int i = 0; i < ix->nextras; i++) { /* a ripped disc's play-all and bonus clips */
        const MediaFile *x = &ix->files[ix->extras[i]];
        if (x->series != s->id || x->name.season != season) continue;
        if (!nx++) H(b, "<h3 class=\"mt\">%s</h3><ol class=\"media-eps media-eps-compact\">", T("Extras"));
        episode_row(b, s, x, here);
    }
    if (nx) buf_puts(b, "</ol>");
    H(b, "</div><div class=\"ph-a\"><a class=\"btn danger\" href=\"%s&amp;rm=season\">", here);
    ui_icon(b, "trash");
    buf_puts(b, "<span>");
    if (season) H(b, T("Remove season %d (%s)"), season, size);
    else H(b, T("Remove the specials (%s)"), size);
    buf_puts(b, "</span></a></div>");
    ui_card_end(b);

    /* the whole series */
    media_size_text(all.bytes, size, sizeof size);
    ui_card(b, T("The whole series"), NULL, NULL);
    buf_puts(b, "<p class=\"muted\">");
    if (s->nseasons > 1) {
        H(b, T("%d seasons"), s->nseasons);
        buf_puts(b, " · ");
    }
    int n = media_episodes_in(s);
    H(b, TN(n, "%d episode", "%d episodes"), n);
    H(b, " · %s</p><div class=\"ph-a\"><a class=\"btn danger\" href=\"%s&amp;rm=series\">", size, here);
    ui_icon(b, "trash");
    buf_puts(b, "<span>");
    H(b, T("Remove the series (%s)"), size);
    buf_puts(b, "</span></a></div>");
    ui_card_end(b);
    media_purge_free(&sp);
    media_purge_free(&all);

    /* the dialogs */
    const char *rm = ctx_q(c, "rm"), *fx = ctx_q(c, "fix");
    uint64_t xid = 0;
    const MediaFile *x;
    if (rm[0]) remove_dialog(b, s, rm, season, here);
    else if (media_parse_id(fx, &xid) && (x = media_file(xid)) != NULL && x->series == s->id && fixable(x))
        fix_dialog(b, s, x, here);
    return true;
}

void media_manage_watch_button(Ctx *c, Buf *b, const MediaFile *f) {
    if (!can_fix_title(c, f)) return;
    H(b, "<a class=\"btn\" href=\"/media/watch/%X?fix=1\" title=\"%s\">", f->id, T("Fix its title and year"));
    ui_icon(b, "edit");
    H(b, "<span>%s</span></a>", T("Fix title"));
}

void media_manage_watch_dialog(Ctx *c, Buf *b, const MediaFile *f) {
    if (!str_eq(ctx_q(c, "fix"), "1") || !can_fix_title(c, f)) return;
    char close[64], example[300];
    snprintf(close, sizeof close, "/media/watch/%016llx", (unsigned long long)f->id);
    if (f->name.year) snprintf(example, sizeof example, "%s (%d)%s", f->name.title, f->name.year, ext_of(base_of(f->rel)));
    else snprintf(example, sizeof example, "%s%s", f->name.title, ext_of(base_of(f->rel)));
    ui_modal(b, T("Fix the title"), close);
    buf_puts(b, "<p class=\"muted\">");
    H(b, T("Its files - the video, its copy, subtitles and picture - are renamed like <code class=\"media-code\">%s</code>, "
           "and so is a folder of its own."),
      example);
    buf_puts(b, "</p>");
    ui_form(b, "media.fix_title", "stack");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><div class=\"field-row\"><label class=\"field\"><span>%s</span>"
         "<input name=\"title\" maxlength=\"120\" required value=\"%s\"></label>",
      f->id, T("Title"), f->name.title);
    H(b, "<label class=\"field\"><span>%s</span><input type=\"number\" name=\"year\" min=\"1880\" max=\"2100\"", T("Year"));
    if (f->name.year) H(b, " value=\"%d\"", f->name.year);
    H(b, "></label></div><div class=\"modal-f\"><a class=\"btn\" href=\"%s\">%s</a><button class=\"btn primary\">%s</button>"
         "</div>",
      close, T("Cancel"), T("Rename"));
    ui_form_end(b);
    ui_modal_end(b);
}

/* ------------------------------------------------------ what takes room */

#define ROOM_ROWS 10
#define ROOM_MIN (1LL << 20) /* under a megabyte nothing takes room worth a row */

typedef struct {
    uint64_t id;
    int64_t bytes;
    const MediaSeries *s; /* a series, else */
    const MediaFile *f;   /* a film */
} Big;

static int big_id_cmp(const void *x, const void *y) {
    uint64_t a = ((const Big *)x)->id, b = ((const Big *)y)->id;
    return a < b ? -1 : a > b;
}

static int big_cmp(const void *x, const void *y) {
    const Big *a = x, *b = y;
    if (a->bytes != b->bytes) return a->bytes > b->bytes ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}

void media_manage_room_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    const MediaIndex *ix = media_lib();
    int n = ix->nseries + ix->nmovies;
    if (!n) return;
    Big *all = xcalloc((size_t)n, sizeof *all);
    for (int i = 0; i < ix->nseries; i++) all[i] = (Big){ix->series[i].id, 0, &ix->series[i], NULL};
    qsort(all, (size_t)ix->nseries, sizeof *all, big_id_cmp);
    for (int i = 0; i < ix->nfiles; i++) { /* a series: its episodes and extras, originals and copies */
        const MediaFile *f = &ix->files[i];
        if (f->type != MF_VIDEO || f->role == MF_WEBCOPY || !f->series) continue;
        Big key = {f->series, 0, NULL, NULL}, *hit = bsearch(&key, all, (size_t)ix->nseries, sizeof *all, big_id_cmp);
        const MediaFile *web = f->web ? media_file(f->web) : NULL;
        if (hit) hit->bytes += f->size + (web ? web->size : 0);
    }
    for (int i = 0; i < ix->nmovies; i++) {
        const MediaFile *f = &ix->files[ix->movies[i]], *web = f->web ? media_file(f->web) : NULL;
        all[ix->nseries + i] = (Big){f->id, f->size + (web ? web->size : 0), NULL, f};
    }
    qsort(all, (size_t)n, sizeof *all, big_cmp);
    int shown = 0;
    while (shown < n && shown < ROOM_ROWS && all[shown].bytes >= ROOM_MIN) shown++;
    if (shown) {
        ui_card(b, T("What takes room"), NULL, NULL);
        H(b, "<p class=\"muted\">%s</p><ol class=\"media-eps media-eps-compact\">",
          T("The biggest series and films on the disk, originals and their copies together."));
    }
    for (int i = 0; i < shown; i++) {
        char size[24];
        media_size_text(all[i].bytes, size, sizeof size);
        if (all[i].s)
            H(b, "<li class=\"media-ep\" id=\"media-room-%X\"><a class=\"media-ep-main\" href=\"/media/series/%X?manage=1\">",
              all[i].id, all[i].id);
        else H(b, "<li class=\"media-ep\" id=\"media-room-%X\"><a class=\"media-ep-main\" href=\"/media/watch/%X\">", all[i].id, all[i].id);
        H(b, "<span class=\"media-ep-n\">%d</span><span class=\"media-ep-t\"><b>%s</b><small>", i + 1,
          all[i].s ? all[i].s->title : all[i].f->name.title);
        if (all[i].s) {
            int eps = media_episodes_in(all[i].s);
            H(b, "<span>%s</span> · ", T("Series"));
            H(b, TN(eps, "%d episode", "%d episodes"), eps);
        } else {
            H(b, "<span>%s</span>", T("Film"));
            if (all[i].f->name.year) H(b, " · %d", all[i].f->name.year);
        }
        H(b, "</small></span><b>%s</b></a></li>", size);
    }
    if (shown) {
        buf_puts(b, "</ol>");
        ui_card_end(b);
    }
    free(all);
}
