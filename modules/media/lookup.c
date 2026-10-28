/* modules/media/lookup.c — categories looked up online (1.4; owner, 2026-10-27: "look up the tags and categories
 * the movie / series has online ... if the user has checked the box", "It can be grayed if the server does not have
 * online activity available to it", "press manage, and then maybe 'Update tags'? Where i paste the id").
 *
 * OFF until an admin ticks it on Library settings (setting media.lookup) - greyed, with the reason, while this
 * Hemma can't reach the services: that page asks for a check when it's opened (at most every PROBE_EVERY_S), and
 * a lookup that got no answer marks it unreachable. Then, one at a time, ASK_EVERY_MS apart, in the background:
 * each series by its name at TVmaze (no key; its data is CC BY-SA - the card credits it), each film by its name
 * and year at OMDb with an admin's own free key (setting media.omdb_key). Both answer over plain http://
 * (core/fetch.c: Hemma has no TLS). Only a title and a year leave the home. What was found (table media_lookup)
 * stays when it's switched off: off stops the asking. An admin can give a title's TVmaze or IMDb id instead
 * (Manage, and the categories dialog): looked up by that, and never again by its name (pinned). cats.c turns
 * the genres into categories. Under tests nothing is fetched: the parsers and the scheduler are tested alone. */
#include "../../core/codec.h"
#include "../../core/fetch.h"
#include "../../core/json.h"
#include "../../core/ui.h"
#include "media.h"

#define SETTING_ON "media.lookup"
#define SETTING_KEY "media.omdb_key"
#define TVMAZE "api.tvmaze.com"
#define OMDB "www.omdbapi.com"
#define ASK_EVERY_MS 3000     /* one lookup, then a pause: TVmaze allows 20 in 10 s */
#define PROBE_EVERY_S 300     /* the internet check, when Library settings is opened */
#define OFFLINE_WAIT_S 900    /* no answer: lookups wait this long, then check again */
#define NOT_FOUND_DAYS 30     /* nothing by that name: asked again after this (a rename asks at once) */
#define ANSWER_MAX (1 << 20)
#define TOKENS 16384

typedef struct {
    int id;
    char work[20];    /* media.h: "s<series>" / "m<film>" */
    char genres[200]; /* as the service says them, '|' between */
    char source[8];   /* "tvmaze" "omdb" */
    char ref[24];     /* its id there: TVmaze's show number, an IMDb "tt..." */
    char name[100];   /* what it's called there: "That '70s Show (1998)" (so a wrong match shows) */
    bool found, pinned;
    int64_t at;
} MediaLookup;

static const FieldDef lookup_fields[] = {
    FIELD(MediaLookup, id, F_INT),       FIELD(MediaLookup, work, F_STR),  FIELD(MediaLookup, genres, F_STR),
    FIELD(MediaLookup, source, F_STR),   FIELD(MediaLookup, ref, F_STR),   FIELD(MediaLookup, name, F_STR),
    FIELD(MediaLookup, found, F_BOOL),   FIELD(MediaLookup, pinned, F_BOOL), FIELD(MediaLookup, at, F_I64),
};
static Table T_media_lookup = TABLE("media_lookup", MediaLookup, lookup_fields);

/* One ask, made on the event loop, answered on a thread. */
typedef struct {
    bool probe, omdb, pinned;
    char work[20], title[128], path[700];
    int year, asker; /* asker: an admin who gave the id - told how it went */
    int got;         /* 1 found, 0 nothing by that, -1 no answer, -2 OMDb refused the key, -3 an answer not understood */
    char genres[200], name[100], ref[24], err[160];
} Job;

static Lock g_lock = LOCK_INIT;
static Job *g_done;              /* answered (under g_lock) */
static Job *g_ask;               /* an admin's id lookup, next in line */
static bool g_running;           /* a thread is out asking */
static int g_online;             /* 1 reachable, -1 not, 0 not known yet */
static int64_t g_online_at, g_next_ms;
static bool g_probe_wanted;
static char g_why[160];          /* why it isn't reachable */
static char g_omdb_bad[120];     /* OMDb's refusal of the key */
static int64_t g_dirty_ms;       /* something found: the lists re-drawn (at most every 15 s) */

void media_lookup_init(void) {
    T_media_lookup.what = "categories looked up online";
    store_register(&T_media_lookup);
}

void media_lookup_start(void) {
    g_online = 0;
    g_online_at = g_next_ms = g_dirty_ms = 0;
    g_probe_wanted = false;
    g_why[0] = g_omdb_bad[0] = '\0';
}

void media_lookup_stop(void) {
    free(g_ask);
    g_ask = NULL;
    /* (a thread still asking frees its own job when it's done: g_done is taken by the next start's tick) */
}

static bool lookup_on(void) { return setting_int(SETTING_ON, 0) == 1; }

static const MediaLookup *row_of(const char *work) {
    for (int i = 0; i < tbl_count(&T_media_lookup); i++) {
        const MediaLookup *r = tbl_at(&T_media_lookup, i);
        if (str_eq(r->work, work)) return r;
    }
    return NULL;
}

const char *media_lookup_genres(const char *work) {
    const MediaLookup *r = row_of(work);
    return r && r->found ? r->genres : "";
}

static void drop_row(const char *work) {
    for (int i = tbl_count(&T_media_lookup) - 1; i >= 0; i--) {
        const MediaLookup *r = tbl_at(&T_media_lookup, i);
        if (str_eq(r->work, work)) tbl_del(&T_media_lookup, r->id);
    }
}

void media_lookup_renamed(const char *from, const char *to) {
    const MediaLookup *r = row_of(from);
    if (!r) return;
    if (!r->pinned) { /* found by its old name: asked again by the new one */
        drop_row(from);
        return;
    }
    MediaLookup m = *r;
    drop_row(to);
    str_copy(m.work, sizeof m.work, to);
    tbl_put(&T_media_lookup, &m);
}

/* ---------------------------------------------------------------- answers */

/* "Comedy|Family" -> "Comedy, Family" ("-" for none) */
static void genres_said(const char *g, char *out, size_t cap) {
    size_t k = 0;
    for (const char *p = g; *p && k + 3 < cap; p++) {
        if (*p == '|') out[k++] = ',', out[k++] = ' ';
        else out[k++] = *p;
    }
    out[k] = '\0';
    if (!k) str_copy(out, cap, "-");
}

static void add_genre(char *out, size_t cap, const char *g) {
    if (!*g || strlen(g) > 40 || strchr(g, '|')) return;
    size_t n = strlen(out);
    if (n + strlen(g) + 2 <= cap) snprintf(out + n, cap - n, "%s%s", n ? "|" : "", g);
}

/* A TVmaze show object: its genres (+ "Animation" / "Documentary" when that's its type), name, year, id. */
static void tvmaze_show(const Json *j, int show, Job *out) {
    char s[128];
    out->genres[0] = '\0';
    int gs = json_get(j, show, "genres");
    for (int k = json_child(j, gs); k >= 0; k = json_next(j, gs, k))
        if (json_string(j, k, s, sizeof s)) add_genre(out->genres, sizeof out->genres, s);
    if (json_string(j, json_get(j, show, "type"), s, sizeof s) && (str_eq(s, "Animation") || str_eq(s, "Documentary")))
        add_genre(out->genres, sizeof out->genres, s);
    char name[96] = "", when[16] = "";
    json_string(j, json_get(j, show, "name"), name, sizeof name);
    json_string(j, json_get(j, show, "premiered"), when, sizeof when);
    if (strlen(when) >= 4) snprintf(out->name, sizeof out->name, "%s (%.4s)", name, when);
    else str_copy(out->name, sizeof out->name, name);
    long long id = 0;
    if (json_integer(j, json_get(j, show, "id"), &id)) snprintf(out->ref, sizeof out->ref, "%lld", id);
}

/* Folded, and without the accents Latin-1 letters carry ("Pokémon" = "pokemon"): names compared. */
static void bare(const char *in, char *out, size_t cap) {
    static const char *const from[] = {"à", "á", "â", "ã", "ä", "å", "è", "é", "ê", "ë", "ì", "í", "î", "ï",
                                       "ò", "ó", "ô", "õ", "ö", "ù", "ú", "û", "ü", "ñ", "ç", "ý", "ÿ"};
    static const char to[] = "aaaaaaeeeeiiiiooooouuuuncyy";
    char f[300];
    fold_copy(f, sizeof f, in);
    size_t k = 0;
    for (const char *p = f; *p && k + 1 < cap;) {
        int hit = -1;
        for (int i = 0; i < countof(from) && hit < 0; i++)
            if (str_starts(p, from[i])) hit = i;
        out[k++] = hit >= 0 ? to[hit] : *p;
        p += hit >= 0 ? strlen(from[hit]) : 1;
    }
    out[k] = '\0';
}

/* every word of ours (letters and digits) in their name: "70's Show" is in "That '70s Show" */
static bool words_in(const char *ours, const char *theirs) {
    for (const char *p = ours; *p;) {
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) p++;
        size_t len = 0;
        while ((p[len] >= 'a' && p[len] <= 'z') || (p[len] >= '0' && p[len] <= '9')) len++;
        if (!len) break;
        char w[64];
        str_ncopy(w, sizeof w, p, len);
        if (!strstr(theirs, w)) return false;
        p += len;
    }
    return true;
}

/* /search/shows (a list) or /shows/N (one). TVmaze answers ANY name with something ("Garden Show" -> a talk show
   once), so only a show whose name holds every word of ours counts: its very name in its year, else ours in its
   name in its year, else its very name, else ours in its name - else nothing was found. */
static int tvmaze_answer(const char *text, size_t n, const char *title, int year, Job *out) {
    JsonTok *tok = xmalloc(TOKENS * sizeof *tok);
    Json j;
    int got = -1;
    if (json_parse(&j, text, n, tok, TOKENS)) {
        int show = json_type(&j, 0) == JSON_OBJECT ? 0 : -1;
        if (json_type(&j, 0) == JSON_ARRAY) {
            int best = -1, rank = 0; /* rank: 4 same name and year, 3 words and year, 2 same name, 1 words */
            char want[300], name[300], when[16];
            bare(title, want, sizeof want);
            for (int k = json_child(&j, 0); k >= 0; k = json_next(&j, 0, k)) {
                int s = json_get(&j, k, "show");
                if (json_type(&j, s) != JSON_OBJECT) continue;
                char raw[160] = "";
                json_string(&j, json_get(&j, s, "name"), raw, sizeof raw);
                bare(raw, name, sizeof name);
                bool same = str_eq(name, want), words = same || words_in(want, name);
                bool dated = year && json_string(&j, json_get(&j, s, "premiered"), when, sizeof when) && atoi(when) == year;
                int r = !words ? 0 : dated ? (same ? 4 : 3) : same ? 2 : 1;
                if (r > rank) rank = r, best = s;
            }
            show = best;
        }
        got = show >= 0 ? 1 : 0;
        if (show >= 0) tvmaze_show(&j, show, out);
    }
    free(tok);
    return got;
}

/* OMDb: {"Title","Year","Genre":"Comedy, Fantasy","imdbID","Response":"True"} | {"Response":"False","Error"} */
static int omdb_answer(const char *text, size_t n, Job *out) {
    JsonTok tok[256];
    Json j;
    if (!json_parse(&j, text, n, tok, countof(tok)) || json_type(&j, 0) != JSON_OBJECT) return -1;
    if (!json_eq(&j, json_get(&j, 0, "Response"), "True")) {
        json_string(&j, json_get(&j, 0, "Error"), out->err, sizeof out->err);
        return fold_find(out->err, "api key") ? -2 : 0;
    }
    char g[300] = "", t[96] = "", y[16] = "";
    out->genres[0] = '\0';
    json_string(&j, json_get(&j, 0, "Genre"), g, sizeof g);
    for (char *p = g, *comma; p; p = comma ? comma + 1 : NULL) {
        comma = strchr(p, ',');
        if (comma) *comma = '\0';
        if (!str_eq(str_trim(p), "N/A")) add_genre(out->genres, sizeof out->genres, str_trim(p));
    }
    json_string(&j, json_get(&j, 0, "Title"), t, sizeof t);
    json_string(&j, json_get(&j, 0, "Year"), y, sizeof y);
    snprintf(out->name, sizeof out->name, y[0] ? "%s (%.4s)" : "%s", t, y);
    json_string(&j, json_get(&j, 0, "imdbID"), out->ref, sizeof out->ref);
    return 1;
}

/* The thread: one GET (and TVmaze's redirect from an IMDb id to the show's number, asked again over http) */
static void ask_thread(void *arg) {
    Job *j = arg;
    thread_background();
    Fetched f;
    const char *host = j->omdb ? OMDB : TVMAZE;
    if (!fetch_get(host, 80, j->path, ANSWER_MAX, 15000, &f)) {
        j->got = -1;
        str_copy(j->err, sizeof j->err, f.err);
    } else if (j->probe) {
        j->got = 1;
    } else {
        const char *shows = f.status / 100 == 3 ? strstr(f.location, "/shows/") : NULL;
        if (shows) {
            char again[64];
            snprintf(again, sizeof again, "/shows/%d", atoi(shows + 7));
            fetched_free(&f);
            if (!fetch_get(host, 80, again, ANSWER_MAX, 15000, &f)) {
                j->got = -1;
                str_copy(j->err, sizeof j->err, f.err);
            }
        }
        if (f.status == 404) j->got = 0; /* (an id that names nothing) */
        else if (f.status == 200 || (j->omdb && f.status == 401)) { /* (OMDb says a bad key in JSON, with a 401) */
            j->got = j->omdb ? omdb_answer(f.body.p ? f.body.p : "", f.body.len, j)
                             : tvmaze_answer(f.body.p ? f.body.p : "", f.body.len, j->title, j->year, j);
            if (j->got == -1) {
                j->got = -3;
                if (!j->err[0]) snprintf(j->err, sizeof j->err, "an answer from %s Hemma didn't understand", host);
            }
        } else if (f.status) {
            j->got = -3;
            snprintf(j->err, sizeof j->err, "%s answered %d", host, f.status);
        }
    }
    fetched_free(&f);
    lock_take(&g_lock);
    Job *old = g_done;
    g_done = j;
    lock_give(&g_lock);
    free_on_loop(old); /* (never: the loop takes each answer before it asks again) */
    app_tick_soon();
}

/* ---------------------------------------------------------------- asking */

static void path_tvmaze(Job *j) {
    Buf p = {0};
    buf_puts(&p, "/search/shows?q=");
    url_encode(&p, j->title, false);
    str_copy(j->path, sizeof j->path, buf_cstr(&p));
    buf_free(&p);
}

static void path_omdb(Job *j, const char *by_id) {
    Buf p = {0};
    if (by_id) {
        buf_printf(&p, "/?i=%s", by_id);
    } else {
        buf_puts(&p, "/?type=movie&t=");
        url_encode(&p, j->title, false);
        if (j->year) buf_printf(&p, "&y=%d", j->year);
    }
    buf_puts(&p, "&apikey=");
    url_encode(&p, setting(SETTING_KEY, ""), false);
    str_copy(j->path, sizeof j->path, buf_cstr(&p));
    buf_free(&p);
}

static bool due(const char *work, int64_t now) {
    const MediaLookup *r = row_of(work);
    return !r || (!r->pinned && !r->found && now - r->at >= NOT_FOUND_DAYS * 86400LL);
}

/* What to ask next by name: series first, then films (with a key OMDb hasn't refused). NULL = nothing. */
static Job *next_job(void) {
    const MediaIndex *ix = media_lib();
    int64_t now = now_sec();
    MediaWork w;
    for (int i = 0; i < ix->nseries; i++) {
        media_work_series(&ix->series[i], &w);
        if (!due(w.key, now)) continue;
        Job *j = xcalloc(1, sizeof *j);
        str_copy(j->work, sizeof j->work, w.key);
        str_copy(j->title, sizeof j->title, ix->series[i].title);
        j->year = ix->series[i].year;
        path_tvmaze(j);
        return j;
    }
    if (!setting(SETTING_KEY, "")[0] || g_omdb_bad[0]) return NULL;
    for (int i = 0; i < ix->nmovies; i++) {
        const MediaFile *f = &ix->files[ix->movies[i]];
        media_work_film(f, &w);
        if (!due(w.key, now)) continue;
        Job *j = xcalloc(1, sizeof *j);
        j->omdb = true;
        str_copy(j->work, sizeof j->work, w.key);
        str_copy(j->title, sizeof j->title, f->name.title);
        j->year = f->name.year;
        path_omdb(j, NULL);
        return j;
    }
    return NULL;
}

static void launch(Job *j) {
    g_running = true;
    if (!thread_spawn(ask_thread, j)) {
        g_running = false;
        free(j);
    }
}

/* An answer, on the event loop. */
static void settle(Job *j) {
    int64_t now = now_sec();
    g_running = false;
    g_next_ms = now_ms() + ASK_EVERY_MS;
    if (j->probe || j->got == -1) {
        bool was = g_online > 0;
        g_online = j->got == 1 ? 1 : -1;
        g_online_at = now;
        str_copy(g_why, sizeof g_why, j->got == 1 ? "" : j->err);
        if (g_online < 0 && (was || j->probe)) log_info("media: looking up online isn't possible now: %s", j->err);
        app_changed_where("/media/settings");
        if (j->asker && !j->probe) app_notify_person(j->asker, TOAST_ERR, N_("Couldn't look it up: no answer from the internet"));
        return;
    }
    g_online = 1;
    g_online_at = now;
    if (j->got == -2) {
        str_copy(g_omdb_bad, sizeof g_omdb_bad, j->err);
        log_warn("media: OMDb refused the key: %s", j->err);
        app_changed_where("/media/settings");
        if (j->asker) app_notify_person(j->asker, TOAST_ERR, N_("OMDb refused the key: %s"), j->err);
        return;
    }
    if (j->got <= 0 && j->pinned) { /* an id that names nothing (or an answer not understood): what was there stays */
        if (j->asker && j->got) app_notify_person(j->asker, TOAST_ERR, N_("Couldn't look it up: %s"), j->err);
        else if (j->asker) app_notify_person(j->asker, TOAST_ERR, N_("Nothing has that id"));
        return;
    }
    if (j->got == -3) log_warn("media: looking up %s: %s", j->title, j->err);
    drop_row(j->work);
    /* (not understood: kept as "nothing found" for a day, then asked again - never every 3 s) */
    MediaLookup r = {.found = j->got == 1, .pinned = j->pinned, .at = j->got == -3 ? now - (NOT_FOUND_DAYS - 1) * 86400LL : now};
    str_copy(r.work, sizeof r.work, j->work);
    str_copy(r.genres, sizeof r.genres, j->genres);
    str_copy(r.source, sizeof r.source, j->omdb ? "omdb" : "tvmaze");
    str_copy(r.ref, sizeof r.ref, j->ref);
    str_copy(r.name, sizeof r.name, j->name);
    tbl_put(&T_media_lookup, &r);
    if (r.found) log_info("media: looked up %s at %s: %s (%s)", j->title, r.source, r.name, r.genres[0] ? r.genres : "no genres");
    if (j->asker) { /* an admin waits for it: their page now */
        char said[300];
        genres_said(r.genres, said, sizeof said);
        if (r.found) app_notify_person(j->asker, TOAST_INFO, N_("Found %s: %s"), r.name, said);
        else app_notify_person(j->asker, TOAST_INFO, N_("Nothing found by the name %s"), j->title);
        app_changed_where("/media");
    } else if (!g_dirty_ms) {
        g_dirty_ms = now_ms(); /* (the lists and the card, soon - not every 3 s) */
    }
}

void media_lookup_tick(int64_t now) {
    lock_take(&g_lock);
    Job *j = g_done;
    g_done = NULL;
    lock_give(&g_lock);
    if (j) {
        settle(j);
        free(j);
    }
    if (g_dirty_ms && now - g_dirty_ms >= 15000) {
        g_dirty_ms = 0;
        app_changed_where("/media");
    }
    if (app_testing() || g_running) return;
    int64_t t = now_sec();
    if (g_probe_wanted || (lookup_on() && g_online < 0 && t - g_online_at >= OFFLINE_WAIT_S)) {
        g_probe_wanted = false;
        Job *p = xcalloc(1, sizeof *p);
        p->probe = true;
        str_copy(p->path, sizeof p->path, "/");
        launch(p);
        return;
    }
    if (!lookup_on() || g_online < 0 || now < g_next_ms) return;
    Job *next = g_ask ? g_ask : next_job();
    g_ask = NULL;
    if (next) launch(next);
}

/* ------------------------------------------------------------------ pages */

static int counted(bool films, int *all) {
    const MediaIndex *ix = media_lib();
    MediaWork w;
    int n = 0;
    *all = 0;
    for (int i = 0; i < (films ? ix->nmovies : ix->nseries); i++) {
        if (films) media_work_film(&ix->files[ix->movies[i]], &w);
        else media_work_series(&ix->series[i], &w);
        const MediaLookup *r = row_of(w.key);
        (*all)++;
        n += r && r->found;
    }
    return n;
}

void media_lookup_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    if (!g_running && now_sec() - g_online_at >= PROBE_EVERY_S) g_probe_wanted = true; /* (the tick checks: a page never waits) */
    bool on = lookup_on(), off_line = g_online < 0;
    const char *key = setting(SETTING_KEY, "");
    ui_card(b, T("Categories from the internet"), NULL, NULL);
    H(b, "<p class=\"muted media-small\">%s</p><p id=\"media-lookup\" class=\"media-lookup-at\">",
      T("Hemma can look up the genres of each series and film online, and put them under categories. Only titles and "
        "years are sent: series' to TVmaze, films' to OMDb. Nothing is looked up until you tick the box."));
    if (!g_online) {
        H(b, "%s", T("Checking whether this Hemma can reach the internet…"));
    } else if (off_line) {
        H(b, "<span class=\"media-warn\">%s</span>", T("This Hemma can't reach the internet right now, so it can't look anything up."));
        if (g_why[0]) H(b, " <span class=\"muted\">(%s)</span>", g_why);
    } else if (on) {
        int all = 0, n = counted(false, &all);
        H(b, T("Looked up: %d of %d series"), n, all);
        if (key[0]) {
            n = counted(true, &all);
            buf_puts(b, " · ");
            H(b, T("%d of %d films"), n, all);
        }
    } else {
        H(b, "%s", T("This Hemma can reach the internet."));
    }
    buf_puts(b, "</p>");
    if (g_omdb_bad[0]) {
        buf_puts(b, "<p class=\"media-warn\">");
        H(b, T("OMDb refused the key: %s"), g_omdb_bad);
        buf_puts(b, "</p>");
    }
    ui_form(b, "media.lookup", "stack");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r%r> %s</label>"
         "<label class=\"field\"><span>%s</span><input name=\"omdb\" value=\"%s\" maxlength=\"40\" autocomplete=\"off\" "
         "spellcheck=\"false\" placeholder=\"%s\"></label>"
         "<p class=\"muted media-small\">%r</p><div><button class=\"btn sm\">%s</button></div>",
      on ? " checked" : "", off_line && !on ? " disabled" : "", T("Look up categories online"),
      T("OMDb key, for films (optional)"), key, T("For example 1a2b3c4d"),
      T("A free key comes by email from <a href=\"https://www.omdbapi.com/apikey.aspx\" rel=\"noreferrer\">omdbapi.com</a>. "
        "Without one, films get categories by hand and from .nfo files only."),
      T("Save"));
    ui_form_end(b);
    H(b, "<p class=\"muted media-small\">%r</p>",
      T("Series' genres come from <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a> (CC BY-SA), films' from "
        "<a href=\"https://www.omdbapi.com\" rel=\"noreferrer\">OMDb</a>. What was found stays when you switch it off."));
    ui_card_end(b);
}

void media_lookup_form(Ctx *c, Buf *b, const MediaWork *w) {
    if (!is_admin(c)) return;
    const MediaLookup *r = row_of(w->key);
    bool on = lookup_on(), film = w->f != NULL;
    H(b, "<div class=\"media-lookup-one\" id=\"media-lookup-one\"><h3>%s</h3><p class=\"muted\">", T("From the internet"));
    const char *where = r && str_eq(r->source, "omdb") ? "OMDb" : "TVmaze";
    if (r && r->found) {
        char said[300];
        genres_said(r->genres, said, sizeof said);
        H(b, T("Found at %s as %s: %s"), where, r->name, said);
        if (r->pinned) H(b, " · %s", T("by the id you gave"));
    } else if (r) {
        H(b, T("Nothing found by its name at %s."), where);
    } else {
        H(b, "%s", on ? T("Not looked up yet.") : T("Looking up online is switched off."));
    }
    buf_puts(b, "</p>");
    if (!on || (film && !setting(SETTING_KEY, "")[0])) {
        H(b, "<p class=\"muted media-small\">%s <a href=\"/media/settings\">%s</a></p></div>",
          !on ? T("Switch it on to look titles up by their name or by an id you give.")
              : T("Films are looked up at OMDb, which needs a key."),
          T("Library settings"));
        return;
    }
    ui_form(b, "media.lookup_id", "stack");
    ui_hidden(b, "work", w->key);
    H(b, "<label class=\"field\"><span>%s</span><input name=\"id\" maxlength=\"200\" autocomplete=\"off\" "
         "spellcheck=\"false\" placeholder=\"%s\"%r></label><div><button class=\"btn\"%r>%s</button></div>",
      film ? T("Its IMDb link or id, when the name finds the wrong one") : T("Its TVmaze or IMDb link or id, when the name "
                                                                             "finds the wrong one"),
      film ? "tt0094721" : "https://www.tvmaze.com/shows/84", g_online < 0 ? " disabled" : "",
      g_online < 0 ? " disabled" : "", T("Update categories"));
    ui_form_end(b);
    buf_puts(b, "</div>");
}

/* ---------------------------------------------------------------- actions */

void act_media_lookup(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    char key[64];
    str_copy(key, sizeof key, fields_str(f, "omdb"));
    char *k = str_trim(key);
    for (const char *p = k; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) || strlen(k) > 40) {
            ctx_fail_at(c, "omdb", "%s", T("An OMDb key is letters and digits, like 1a2b3c4d"));
            return;
        }
    bool on = str_eq(fields_str(f, "on"), "1"), was = lookup_on();
    if (on && !was && g_online < 0) {
        ctx_fail(c, "%s", T("This Hemma can't reach the internet right now"));
        return;
    }
    if (!str_eq(k, setting(SETTING_KEY, ""))) { /* a new key: what OMDb didn't find, or refused, is asked again */
        setting_set(SETTING_KEY, k);
        g_omdb_bad[0] = '\0';
        for (int i = tbl_count(&T_media_lookup) - 1; i >= 0; i--) {
            const MediaLookup *r = tbl_at(&T_media_lookup, i);
            if (str_eq(r->source, "omdb") && !r->found && !r->pinned) tbl_del(&T_media_lookup, r->id);
        }
    }
    setting_set_int(SETTING_ON, on ? 1 : 0);
    if (on != was) log_info("media: %s switched %s looking up categories online", c->me->name, on ? "on" : "off");
    if (on && !was) g_probe_wanted = true;
    if (on != was) ctx_ok(c, "%s", on ? T("Hemma looks up categories online now") : T("Hemma doesn't look anything up online now"));
    else ctx_ok(c, "%s", T("Saved"));
}

/* "https://www.tvmaze.com/shows/84/family-guy", "84" -> 84; "https://www.imdb.com/title/tt0182576/", "tt0182576" */
static bool id_imdb(const char *s, char out[16]) {
    for (const char *p = s; (p = strstr(p, "tt")); p++) {
        size_t d = 0;
        while (p[2 + d] >= '0' && p[2 + d] <= '9') d++;
        if (d >= 7 && d <= 10 && (p == s || !(p[-1] >= 'a' && p[-1] <= 'z'))) {
            snprintf(out, 16, "tt%.*s", (int)d, p + 2);
            return true;
        }
    }
    return false;
}

static long id_tvmaze(const char *s) {
    const char *p = strstr(s, "/shows/");
    if (p) return atol(p + 7);
    long long n = 0;
    return str_int(s, &n) && n > 0 && n < 100000000 ? (long)n : 0;
}

/* media.lookup_id work id: look it up by that id (TVmaze or IMDb), and keep it so - or, with no id, by its name again */
void act_media_lookup_id(Ctx *c, Fields *f) {
    MediaWork w;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    if (!media_work_find(fields_str(f, "work"), &w)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    if (!lookup_on() || g_online < 0) {
        ctx_fail(c, "%s", !lookup_on() ? T("Switch on looking up categories online first (Library settings)")
                                       : T("This Hemma can't reach the internet right now"));
        return;
    }
    char given[256], imdb[16];
    str_copy(given, sizeof given, fields_str(f, "id"));
    const char *id = str_trim(given);
    long tvm = w.s ? id_tvmaze(id) : 0;
    bool by_imdb = id_imdb(id, imdb);
    if (w.f && !setting(SETTING_KEY, "")[0]) {
        ctx_fail(c, "%s", T("Films are looked up at OMDb, which needs a key (Library settings)"));
        return;
    }
    if (*id && !by_imdb && !tvm) {
        ctx_fail_at(c, "id", "%s", w.f ? T("Paste its IMDb link, or its id like tt0094721")
                                       : T("Paste its TVmaze or IMDb link, or an id like 84 or tt0182576"));
        return;
    }
    Job *j = xcalloc(1, sizeof *j);
    j->omdb = w.f != NULL;
    j->pinned = *id != '\0';
    j->asker = c->me->id;
    str_copy(j->work, sizeof j->work, w.key);
    str_copy(j->title, sizeof j->title, w.s ? w.s->title : w.f->name.title);
    j->year = w.s ? w.s->year : w.f->name.year;
    if (j->omdb) path_omdb(j, by_imdb ? imdb : NULL);
    else if (tvm) snprintf(j->path, sizeof j->path, "/shows/%ld", tvm);
    else if (by_imdb) snprintf(j->path, sizeof j->path, "/lookup/shows?imdb=%s", imdb);
    else path_tvmaze(j);
    free(g_ask);
    g_ask = j; /* (next in line: the tick sends it) */
    ctx_ok(c, "%s", T("Looking it up…"));
}

/* ------------------------------------------------------------------ tests */

int media_lookup_test_tvmaze(const char *json, const char *title, int year, char *genres, size_t cap) {
    Job j = {0};
    int got = tvmaze_answer(json, strlen(json), title, year, &j);
    str_copy(genres, cap, j.genres);
    return got;
}

int media_lookup_test_omdb(const char *json, char *genres, size_t cap) {
    Job j = {0};
    int got = omdb_answer(json, strlen(json), &j);
    str_copy(genres, cap, j.genres);
    return got;
}

bool media_lookup_test_next(char *host, size_t hcap, char *path, size_t pcap) {
    Job *j = g_ask ? g_ask : next_job();
    if (j == g_ask) g_ask = NULL;
    if (!j) return false;
    str_copy(host, hcap, j->omdb ? OMDB : TVMAZE);
    str_copy(path, pcap, j->path);
    free(j);
    return true;
}

void media_lookup_test_online(int state) {
    g_online = state;
    g_online_at = now_sec();
}

void media_lookup_test_put(const char *work, const char *genres, bool pinned) {
    drop_row(work);
    MediaLookup r = {.found = true, .pinned = pinned, .at = now_sec()};
    str_copy(r.work, sizeof r.work, work);
    str_copy(r.genres, sizeof r.genres, genres);
    str_copy(r.source, sizeof r.source, "tvmaze");
    str_copy(r.name, sizeof r.name, "Test");
    tbl_put(&T_media_lookup, &r);
}
