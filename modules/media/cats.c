/* modules/media/cats.c — categories of series and films (1.4; owner, 2026-10-27: "increase the searching and
 * usage of the series and movies"; categories "by hand + .nfo genres", ticked by admins and members).
 *
 * What a work is and where its categories come from: media.h. Here: the starters (a household's common
 * genres and seasons, in all three languages, with the other words they're known by - an .nfo's "Animation",
 * a lookup's "Science-Fiction", a Swede typing "Komedi" all find "animated", "scifi", "comedy"), the table of
 * what people ticked, the line and the dialog on a series' or film's page, and the two actions. The lists,
 * rows and search that use them are media.c's; the TV's row is tiles.c's. */
#include "../../core/ui.h"
#include "media.h"

typedef struct {
    const char *key, *name;
    const char *also; /* other names, folded, '|' between: English, Swedish, Spanish, and the services' genres */
} Starter;

static const Starter STARTERS[] = {
    {"action", N_("Action"), "action|action & adventure|acción|accion"},
    {"adventure", N_("Adventure"), "adventure|äventyr|aventura|aventuras|action & adventure"},
    {"animated", N_("Animated"), "animation|animated|animerat|animerad|animerade|tecknat|tecknad film|animación|animacion|"
                                 "animada|animadas|dibujos animados|cartoon|cartoons"},
    {"anime", N_("Anime"), "anime"},
    {"comedy", N_("Comedy"), "comedy|comedies|komedi|komedier|comedia|comedias|sitcom"},
    {"crime", N_("Crime"), "crime|kriminal|brott|crimen|policíaca|policiaca"},
    {"documentary", N_("Documentary"), "documentary|documentaries|dokumentär|dokumentar|documental|documentales"},
    {"drama", N_("Drama"), "drama|dramas"},
    {"family", N_("Family"), "family|familj|familjefilm|familia|familiar|kids & family"},
    {"fantasy", N_("Fantasy"), "fantasy|fantasi|fantasía|fantasia|sci-fi & fantasy|supernatural"},
    {"horror", N_("Horror"), "horror|skräck|skrack|terror"},
    {"kids", N_("Kids"), "kids|children|barn|barnprogram|barnfilm|niños|ninos|infantil|kids & family"},
    {"musical", N_("Musical"), "musical|music|musik|música|musica"},
    {"mystery", N_("Mystery"), "mystery|mysterium|deckare|misterio"},
    {"romance", N_("Romance"), "romance|romantik|romantisk|romántica|romantica"},
    {"scifi", N_("Science fiction"), "science fiction|science-fiction|sci-fi|scifi|sci-fi & fantasy|ciencia ficción|"
                                     "ciencia ficcion|ciencia-ficción"},
    {"thriller", N_("Thriller"), "thriller|thrillers|spänning|spanning|suspense|suspenso"},
    {"war", N_("War"), "war|krig|krigsfilm|guerra|bélica|belica|war & politics"},
    {"western", N_("Western"), "western|westerns|vilda västern|oeste"},
    {"christmas", N_("Christmas"), "christmas|xmas|jul|julfilm|navidad|navideña|navidena"},
    {"halloween", N_("Halloween"), "halloween"},
};

/* ------------------------------------------------------------------ table */

typedef struct {
    int id;
    char work[20];          /* "s<16 hex>" / "m<16 hex>" (media.h) */
    char cat[MEDIA_CAT_LEN]; /* a starter's key, or "+<name>" */
    bool on;                /* false: one the .nfo or the lookup gave, taken off */
    int64_t at;
} MediaCat;

static const FieldDef cat_fields[] = {
    FIELD(MediaCat, id, F_INT), FIELD(MediaCat, work, F_STR), FIELD(MediaCat, cat, F_STR),
    FIELD(MediaCat, on, F_BOOL), FIELD(MediaCat, at, F_I64),
};
static Table T_media_cat = TABLE("media_cat", MediaCat, cat_fields);

void media_cats_init(void) {
    T_media_cat.what = "categories of series and films";
    store_register(&T_media_cat);
}

/* ------------------------------------------------------------------ works */

void media_work_series(const MediaSeries *s, MediaWork *w) {
    *w = (MediaWork){.nfo = s->nfo, .s = s};
    snprintf(w->key, sizeof w->key, "s%016llx", (unsigned long long)s->id);
}

void media_film_key(const char *title, int year, char out[20]) {
    char t[160], k[200];
    fold_copy(t, sizeof t, title);
    snprintf(k, sizeof k, "%s|%d", t, year);
    snprintf(out, 20, "m%016llx", (unsigned long long)hash64_str(k));
}

void media_work_film(const MediaFile *f, MediaWork *w) {
    *w = (MediaWork){.nfo = f->nfo, .f = f};
    media_film_key(f->name.title, f->name.year, w->key);
}

/* The work a key names, in the whole library (an action's field: only what's there may be ticked). */
bool media_work_find(const char *key, MediaWork *w) {
    const MediaIndex *ix = media_lib();
    uint64_t id = 0;
    if (key[0] == 's' && media_parse_id(key + 1, &id)) {
        const MediaSeries *s = media_series(id);
        if (s) media_work_series(s, w);
        return s != NULL;
    }
    for (int i = 0; key[0] == 'm' && i < ix->nmovies; i++) {
        media_work_film(&ix->files[ix->movies[i]], w);
        if (str_eq(w->key, key)) return true;
    }
    return false;
}

/* -------------------------------------------------------------- categories */

static const Starter *starter(const char *key) {
    for (int i = 0; i < countof(STARTERS); i++)
        if (str_eq(STARTERS[i].key, key)) return &STARTERS[i];
    return NULL;
}

const char *media_cat_label(const char *cat) {
    const Starter *s = starter(cat);
    return s ? T(s->name) : cat[0] == '+' ? cat + 1 : cat;
}

/* The starters a name means (folded: its key, its name in any language, or one of the others it's known by),
   in keys; 0 = none - the household's own. Remembered (GMAP): the same few genres come up for every title of a
   page, and each costs the starters in three languages. */
#define GMAP 128
static struct {
    char name[MEDIA_CAT_LEN * 2];
    const char *keys[4];
    int n;
} g_gmap[GMAP];
static int g_ngmap;

static int name_starters_uncached(const char *n, const char *keys[], int max);
static int name_starters(const char *name, const char *keys[], int max) {
    char n[MEDIA_CAT_LEN * 2];
    fold_copy(n, sizeof n, name);
    str_trim(n);
    for (int i = 0; i < g_ngmap; i++)
        if (str_eq(g_gmap[i].name, n)) {
            int k = MIN(max, g_gmap[i].n);
            memcpy(keys, g_gmap[i].keys, (size_t)k * sizeof *keys);
            return k;
        }
    int k = name_starters_uncached(n, keys, max);
    if (g_ngmap < GMAP && max >= countof(g_gmap[0].keys)) {
        str_copy(g_gmap[g_ngmap].name, sizeof g_gmap[0].name, n);
        memcpy(g_gmap[g_ngmap].keys, keys, (size_t)k * sizeof *keys);
        g_gmap[g_ngmap++].n = k;
    }
    return k;
}

static int name_starters_uncached(const char *n, const char *keys[], int max) {
    int k = 0;
    for (int i = 0; i < countof(STARTERS) && k < max; i++) {
        const Starter *s = &STARTERS[i];
        bool hit = str_eq(n, s->key);
        for (int l = 0; l < LANG_COUNT && !hit; l++) { /* (its name as each language says it) */
            char said[MEDIA_CAT_LEN * 2];
            fold_copy(said, sizeof said, T_in((Lang)l, s->name));
            hit = str_eq(n, said);
        }
        for (const char *p = s->also; *p && !hit;) {
            const char *bar = strchr(p, '|');
            size_t len = bar ? (size_t)(bar - p) : strlen(p);
            hit = strlen(n) == len && strncmp(n, p, len) == 0;
            p += len + (bar ? 1 : 0);
        }
        if (hit) keys[k++] = s->key;
    }
    return k;
}

/* Add a category to a list once (n: how many there are; returns the new count). */
static int add_cat(char out[][MEDIA_CAT_LEN], int n, int max, const char *cat) {
    for (int i = 0; i < n; i++)
        if (str_eq(out[i], cat)) return n;
    if (n >= max) return n;
    str_copy(out[n], MEDIA_CAT_LEN, cat);
    return n + 1;
}

/* A name as a category: the starters it means, else "+<name>" - written as one already in use is (the first
   spelling wins: "pokemon" joins "Pokemon"). */
static int name_cats(const char *name, char out[][MEDIA_CAT_LEN], int n, int max) {
    const char *keys[4];
    int k = name_starters(name, keys, countof(keys));
    for (int i = 0; i < k; i++) n = add_cat(out, n, max, keys[i]);
    if (k) return n;
    char own[MEDIA_CAT_LEN];
    snprintf(own, sizeof own, "+%s", name);
    for (int i = 0; i < tbl_count(&T_media_cat); i++) {
        const MediaCat *r = tbl_at(&T_media_cat, i);
        char a[MEDIA_CAT_LEN * 2], b[MEDIA_CAT_LEN * 2];
        fold_copy(a, sizeof a, r->cat);
        fold_copy(b, sizeof b, own);
        if (r->cat[0] == '+' && str_eq(a, b)) return add_cat(out, n, max, r->cat);
    }
    return add_cat(out, n, max, own);
}

/* "Comedy|Animation" -> categories */
static int genre_cats(const char *genres, char out[][MEDIA_CAT_LEN], int n, int max) {
    char g[200];
    for (const char *p = genres; *p;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        str_ncopy(g, sizeof g, p, len);
        if (*str_trim(g)) n = name_cats(g, out, n, max);
        p += len + (bar ? 1 : 0);
    }
    return n;
}

static int cat_order(const void *x, const void *y) { /* the starters in their order, then the household's A-Z */
    const char *a = x, *b = y;
    const Starter *sa = starter(a), *sb = starter(b);
    if (sa && sb) return (int)(sa - sb);
    if (sa || sb) return sa ? -1 : 1;
    char fa[MEDIA_CAT_LEN * 2], fb[MEDIA_CAT_LEN * 2];
    fold_copy(fa, sizeof fa, a);
    fold_copy(fb, sizeof fb, b);
    return strcmp(fa, fb);
}

int media_cats_of(const MediaWork *w, char out[][MEDIA_CAT_LEN], int max) {
    int n = genre_cats(media_nfo_genres(w->nfo), out, 0, max);
    n = genre_cats(media_lookup_genres(w->key), out, n, max);
    for (int i = 0; i < tbl_count(&T_media_cat); i++) { /* what people ticked, and took off */
        const MediaCat *r = tbl_at(&T_media_cat, i);
        if (!str_eq(r->work, w->key)) continue;
        if (r->on) {
            n = add_cat(out, n, max, r->cat);
            continue;
        }
        for (int k = 0; k < n; k++)
            if (str_eq(out[k], r->cat)) {
                memmove(out[k], out[k + 1], (size_t)(n - k - 1) * MEDIA_CAT_LEN);
                n--;
                break;
            }
    }
    qsort(out, (size_t)n, MEDIA_CAT_LEN, cat_order);
    return n;
}

bool media_cat_in(const MediaWork *w, const char *cat) {
    char have[MEDIA_CAT_MAX][MEDIA_CAT_LEN];
    int n = media_cats_of(w, have, MEDIA_CAT_MAX);
    for (int i = 0; i < n; i++)
        if (str_eq(have[i], cat)) return true;
    return false;
}

bool media_cat_parse(const char *q, char out[MEDIA_CAT_LEN]) {
    if (!q || !*q || strlen(q) >= MEDIA_CAT_LEN || (q[0] != '+' && !starter(q))) return false;
    str_copy(out, MEDIA_CAT_LEN, q);
    return true;
}

static int use_count(const MediaCatUse *u) { return u->series + u->films; }
static int use_order(const void *x, const void *y) {
    const MediaCatUse *a = x, *b = y;
    if (use_count(a) != use_count(b)) return use_count(b) - use_count(a);
    return cat_order(a->cat, b->cat);
}

static int tally(MediaCatUse *out, int n, int max, const MediaWork *w) {
    char have[MEDIA_CAT_MAX][MEDIA_CAT_LEN];
    int k = media_cats_of(w, have, MEDIA_CAT_MAX);
    for (int i = 0; i < k; i++) {
        int at = -1;
        for (int j = 0; j < n && at < 0; j++)
            if (str_eq(out[j].cat, have[i])) at = j;
        if (at < 0 && n < max) {
            at = n++;
            out[at] = (MediaCatUse){0};
            str_copy(out[at].cat, sizeof out[at].cat, have[i]);
        }
        if (at >= 0) ++*(w->s ? &out[at].series : &out[at].films);
    }
    return n;
}

int media_cats_used(const Ctx *c, int kinds, MediaCatUse *out, int max) {
    const MediaIndex *ix = media_lib_for(c);
    int n = 0;
    MediaWork w;
    for (int i = 0; (kinds & MEDIA_CATS_SERIES) && i < ix->nseries; i++) {
        media_work_series(&ix->series[i], &w);
        n = tally(out, n, max, &w);
    }
    for (int i = 0; (kinds & MEDIA_CATS_FILMS) && i < ix->nmovies; i++) {
        media_work_film(&ix->files[ix->movies[i]], &w);
        n = tally(out, n, max, &w);
    }
    qsort(out, (size_t)n, sizeof *out, use_order);
    return n;
}

void media_cats_words(const MediaWork *w, Buf *out) {
    char have[MEDIA_CAT_MAX][MEDIA_CAT_LEN];
    int n = media_cats_of(w, have, MEDIA_CAT_MAX);
    for (int i = 0; i < n; i++) {
        const Starter *s = starter(have[i]);
        buf_printf(out, " %s", media_cat_label(have[i])); /* (the reader's words, and English's) */
        if (s) buf_printf(out, " %s", s->name);
    }
}

void media_cats_move(const char *from, const char *to) {
    for (int i = 0; i < tbl_count(&T_media_cat); i++) {
        MediaCat r = *(const MediaCat *)tbl_at(&T_media_cat, i);
        if (!str_eq(r.work, from)) continue;
        str_copy(r.work, sizeof r.work, to);
        tbl_put(&T_media_cat, &r);
    }
}

/* ------------------------------------------------------------------ pages */

void media_cats_line(Ctx *c, Buf *b, const MediaWork *w, const char *list, const char *here) {
    char have[MEDIA_CAT_MAX][MEDIA_CAT_LEN];
    int n = media_cats_of(w, have, MEDIA_CAT_MAX);
    bool can = ui_can("media.cat");
    if (!n && !can) return;
    H(b, "<p class=\"chips media-cats-line\" aria-label=\"%s\">", T("Categories"));
    for (int i = 0; i < n; i++) H(b, "<a class=\"chip\" href=\"%s?cat=%u\">%s</a>", list, have[i], media_cat_label(have[i]));
    if (can) { /* (a display account, a child, a guest: the names only) */
        H(b, "<a class=\"chip media-cats-edit\" href=\"%s?cats=1\">", here);
        ui_icon(b, n ? "edit" : "plus");
        H(b, "<span>%s</span></a>", n ? T("Categories") : T("Add categories"));
    }
    buf_puts(b, "</p>");
}

/* One category's toggle in the dialog: a form of its own (wire.js re-draws the dialog with it changed). */
static void toggle(Buf *b, const char *work, const char *cat, bool on) {
    ui_form(b, "media.cat", "inline");
    ui_hidden(b, "work", work);
    ui_hidden(b, "cat", cat);
    ui_hidden(b, "on", on ? "0" : "1");
    H(b, "<button class=\"chip media-cat-t%r\" aria-pressed=\"%r\">%s</button>", on ? " on" : "", on ? "true" : "false",
      media_cat_label(cat));
    ui_form_end(b);
}

void media_cats_modal(Ctx *c, Buf *b, const MediaWork *w, const char *title, const char *close) {
    if (!str_eq(ctx_q(c, "cats"), "1") || !ui_can("media.cat")) return;
    char have[MEDIA_CAT_MAX][MEDIA_CAT_LEN];
    int n = media_cats_of(w, have, MEDIA_CAT_MAX);
    ui_modal(b, T("Categories"), close);
    H(b, "<p class=\"muted\">");
    H(b, T("Tap the ones that fit %s. Everyone finds it under them on Movies and Series, and when searching."), title);
    buf_puts(b, "</p><div class=\"chips media-cats-pick\" id=\"media-cats-pick\">");
    for (int i = 0; i < countof(STARTERS); i++) {
        bool on = false;
        for (int k = 0; k < n && !on; k++) on = str_eq(have[k], STARTERS[i].key);
        toggle(b, w->key, STARTERS[i].key, on);
    }
    /* the household's own: ticked here, or anywhere in the library */
    char own[64][MEDIA_CAT_LEN];
    int no = 0;
    for (int k = 0; k < n; k++)
        if (have[k][0] == '+') no = add_cat(own, no, countof(own), have[k]);
    for (int i = 0; i < tbl_count(&T_media_cat); i++) {
        const MediaCat *r = tbl_at(&T_media_cat, i);
        if (r->on && r->cat[0] == '+') no = add_cat(own, no, countof(own), r->cat);
    }
    qsort(own, (size_t)no, MEDIA_CAT_LEN, cat_order);
    for (int i = 0; i < no; i++) {
        bool on = false;
        for (int k = 0; k < n && !on; k++) on = str_eq(have[k], own[i]);
        toggle(b, w->key, own[i], on);
    }
    buf_puts(b, "</div>");
    /* folded: a dialog opens on its first text field, and a phone's keyboard would cover the chips */
    H(b, "<details class=\"media-cats-own\"><summary>%s</summary>", T("A category of your own"));
    ui_form(b, "media.cat_new", "media-cats-new");
    ui_hidden(b, "work", w->key);
    H(b, "<input name=\"name\" maxlength=\"40\" autocomplete=\"off\" placeholder=\"%s\" aria-label=\"%s\">"
         "<button class=\"btn\">%s</button>",
      T("Pokémon, Swedish, Sunday films…"), T("A category of your own"), T("Add"));
    ui_form_end(b);
    buf_puts(b, "</details>");
    if (w->nfo || media_lookup_genres(w->key)[0])
        H(b, "<p class=\"muted media-small\">%s</p>",
          T("Some were filled in from its .nfo file or looked up online. Tap one to take it off."));
    media_lookup_form(c, b, w); /* (admins: what was found online, and looking it up by an id) */
    H(b, "<div class=\"modal-f\"><a class=\"btn primary\" href=\"%s\">%s</a></div>", close, T("Done"));
    ui_modal_end(b);
}

/* ---------------------------------------------------------------- actions */

/* media.cat work cat on: tick one on (1) or off (0). Members and admins (not children, guests or a TV). */
void act_media_cat(Ctx *c, Fields *f) {
    MediaWork w;
    char cat[MEDIA_CAT_LEN];
    if (!media_work_find(fields_str(f, "work"), &w)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    if (!media_cat_parse(fields_str(f, "cat"), cat)) {
        ctx_fail(c, "%s", T("There's no such category"));
        return;
    }
    bool on = fields_int(f, "on", 1) != 0;
    for (int i = tbl_count(&T_media_cat) - 1; i >= 0; i--) { /* its row goes; a new one says it now */
        const MediaCat *r = tbl_at(&T_media_cat, i);
        if (str_eq(r->work, w.key) && str_eq(r->cat, cat)) tbl_del(&T_media_cat, r->id);
    }
    if (media_cat_in(&w, cat) != on) { /* (what the .nfo / lookup say needs no row) */
        MediaCat r = {.on = on, .at = now_sec()};
        str_copy(r.work, sizeof r.work, w.key);
        str_copy(r.cat, sizeof r.cat, cat);
        tbl_put(&T_media_cat, &r);
    }
}

/* media.cat_new work name: a category typed in - a starter's name in any language is that starter. */
void act_media_cat_new(Ctx *c, Fields *f) {
    MediaWork w;
    if (!media_work_find(fields_str(f, "work"), &w)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    char name[MEDIA_CAT_LEN];
    str_copy(name, sizeof name - 1, fields_str(f, "name")); /* (room for the "+") */
    str_trim(name);
    for (char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == '|') *p = ' ';
    if (!*name) {
        ctx_fail_at(c, "name", "%s", T("Type a name for it"));
        return;
    }
    char cats[4][MEDIA_CAT_LEN];
    int n = name_cats(name, cats, 0, countof(cats));
    int had = 0;
    for (int i = 0; i < n; i++) {
        if (media_cat_in(&w, cats[i])) {
            had++;
            continue;
        }
        for (int k = tbl_count(&T_media_cat) - 1; k >= 0; k--) {
            const MediaCat *r = tbl_at(&T_media_cat, k);
            if (str_eq(r->work, w.key) && str_eq(r->cat, cats[i])) tbl_del(&T_media_cat, r->id);
        }
        if (!media_cat_in(&w, cats[i])) { /* (taking the "off" row away may have been enough) */
            MediaCat r = {.on = true, .at = now_sec()};
            str_copy(r.work, sizeof r.work, w.key);
            str_copy(r.cat, sizeof r.cat, cats[i]);
            tbl_put(&T_media_cat, &r);
        }
    }
    if (had == n) ctx_ok(c, T("It's in %s already"), media_cat_label(cats[0]));
    else ctx_ok(c, T("Added to %s"), media_cat_label(cats[0]));
}
