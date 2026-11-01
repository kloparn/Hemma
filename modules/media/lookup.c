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
 * the genres into categories. Under tests nothing is fetched: the parsers and the scheduler are tested alone.
 *
 * PICTURES (2.5; owner, 2026-10-30: "we don't have to give an image to a show / movie. It should automatically fetch
 * if an image is not given", "I do not want it to be setup with another key"): a box of its own (setting
 * media.lookup_art), off by default and greyed the same way. A series or film WITHOUT a picture gets one: a series'
 * from TVmaze (the show's poster - in the same ask as its genres when both boxes are on; the picture itself over
 * plain http from static.tvmaze.com), a film's from Wikipedia by its title and year (the film's article's picture -
 * Wikipedia answers only over https: core/fetch.c's fetch_https, the computer's own curl). Saved where "Set
 * picture" puts one (media_art_target: <show folder>/poster.jpg, <film>.jpg beside it), so the scan finds it and
 * other players do too - never over a picture that's there. Table media_art remembers what was looked for. An
 * admin may give the right show's link (TVmaze, IMDb) or the film's Wikipedia page: it replaces only a picture
 * Hemma fetched (one of the same size as it saved), never one of the household's own. */
#include "../../core/codec.h"
#include "../../core/fetch.h"
#include "../../core/json.h"
#include "../../core/ui.h"
#include "media.h"

#define SETTING_ON "media.lookup"
#define SETTING_KEY "media.omdb_key"
#define SETTING_ART "media.lookup_art"
#define TVMAZE "api.tvmaze.com"
#define TVMAZE_PICS "static.tvmaze.com"
#define OMDB "www.omdbapi.com"
#define WIKI "en.wikipedia.org"
#define PICTURE_MAX (8 << 20) /* a picture, at most */
#define ART_QUIET_MS 10000    /* pictures saved: the library looks again once none came for this long ... */
#define ART_RESCAN_MS 60000   /* ... or this long after the first */
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

/* A picture looked for (2.5). */
typedef struct {
    int id;
    char work[20];
    int got;         /* 1 saved, 0 none found by that, -1 couldn't be fetched or saved (asked again in a day) */
    bool pinned;     /* by a link an admin gave: never asked again by name */
    char source[12]; /* "tvmaze" "wikipedia" */
    char ref[200];   /* its show number there, its page's title */
    char name[160];  /* what it is there: "DuckTales (1987)", "Tarzan (1999 film)" - a wrong match shows */
    int64_t size;    /* of the picture saved: the work's picture still that size = still Hemma's own */
    int64_t at;
} MediaArt;

static const FieldDef art_fields[] = {
    FIELD(MediaArt, id, F_INT),       FIELD(MediaArt, work, F_STR), FIELD(MediaArt, got, F_INT),
    FIELD(MediaArt, pinned, F_BOOL),  FIELD(MediaArt, source, F_STR), FIELD(MediaArt, ref, F_STR),
    FIELD(MediaArt, name, F_STR),     FIELD(MediaArt, size, F_I64), FIELD(MediaArt, at, F_I64),
};
static Table T_media_art = TABLE("media_art", MediaArt, art_fields);

/* One ask, made on the event loop, answered on a thread. */
typedef struct {
    bool probe, omdb, pinned;
    bool art;  /* a picture's ask (its genres not kept) */
    bool both; /* a series' genres, and its picture with them */
    bool wiki; /* Wikipedia, over https: path is the whole address */
    char work[20], title[128], path[700];
    int year, asker; /* asker: an admin who gave the id - told how it went */
    int got;         /* 1 found, 0 nothing by that, -1 no answer, -2 OMDb refused the key, -3 an answer not understood */
    char genres[200], name[160], ref[200], err[160];
    /* the picture: where it goes (folder + stem), the one of Hemma's own it replaces, and how it went */
    char dest[700], replace[700];
    int64_t replace_size;
    char img[600], img2[600]; /* its addresses in the answer (TVmaze: original, then medium) */
    int art_got;              /* 1 saved, 0 none there, -1 couldn't, -4 a picture came meanwhile */
    int64_t art_size;
    char art_err[160];
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
static int64_t g_art_first_ms, g_art_last_ms; /* pictures saved since the library last looked */
/* A title renamed: nothing asked by name until the library has read it under the new one (the ask in the same
   second as Manage's Rename went with the old name and year once - and found the wrong DuckTales). */
static bool g_hold;
static unsigned g_hold_gen;

void media_lookup_init(void) {
    T_media_lookup.what = "categories looked up online";
    store_register(&T_media_lookup);
    T_media_art.what = "pictures looked up online";
    store_register(&T_media_art);
}

void media_lookup_start(void) {
    g_online = 0;
    g_online_at = g_next_ms = g_dirty_ms = g_art_first_ms = g_art_last_ms = 0;
    g_probe_wanted = g_hold = false;
    g_why[0] = g_omdb_bad[0] = '\0';
}

void media_lookup_stop(void) {
    free(g_ask);
    g_ask = NULL;
    /* (a thread still asking frees its own job when it's done: g_done is taken by the next start's tick) */
}

static bool lookup_on(void) { return setting_int(SETTING_ON, 0) == 1; }
static bool art_on(void) { return setting_int(SETTING_ART, 0) == 1; }

static const MediaArt *art_of(const char *work) {
    for (int i = 0; i < tbl_count(&T_media_art); i++) {
        const MediaArt *r = tbl_at(&T_media_art, i);
        if (str_eq(r->work, work)) return r;
    }
    return NULL;
}

static void drop_art(const char *work) {
    for (int i = tbl_count(&T_media_art) - 1; i >= 0; i--) {
        const MediaArt *r = tbl_at(&T_media_art, i);
        if (str_eq(r->work, work)) tbl_del(&T_media_art, r->id);
    }
}

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
    g_hold = true;
    g_hold_gen = media_lib_gen();
    const MediaArt *a = art_of(from);
    if (a && (a->pinned || a->got == 1)) { /* (a picture saved went along with its files) */
        MediaArt m = *a;
        drop_art(to);
        str_copy(m.work, sizeof m.work, to);
        tbl_put(&T_media_art, &m);
    } else if (a) {
        drop_art(from);
    }
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
    int im = json_get(j, show, "image"); /* (its poster: "original", and the smaller "medium") */
    out->img[0] = out->img2[0] = '\0';
    json_string(j, json_get(j, im, "original"), out->img, sizeof out->img);
    json_string(j, json_get(j, im, "medium"), out->img2, sizeof out->img2);
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

/* an address whose file is a JPEG, PNG or WebP (by its name: a logo's .svg or .gif isn't a poster) */
static bool picture_address(const char *url) {
    size_t n = strcspn(url, "?#");
    static const char *const exts[] = {".jpg", ".jpeg", ".png", ".webp"};
    char tail[8];
    for (int i = 0; i < countof(exts); i++) {
        size_t e = strlen(exts[i]);
        if (n <= e) continue;
        str_ncopy(tail, sizeof tail, url + n - e, e);
        if (str_ieq(tail, exts[i])) return true;
    }
    return false;
}

/* Wikipedia's answer: query.pages, each with its place in the search ("index"), title, description ("1961 Disney
   animated film") and picture (original.source). A film's: the first in the search's order with a picture that is a
   film - "film" in its description or its title's brackets - of its year (in either), or, with no year to go by,
   whose title is ours ("Hocus Pocus (1993 film)" for "Hocus Pocus"). A page an admin gave (pinned): its picture. */
static int wiki_answer(const char *text, size_t n, const char *title, int year, bool pinned, Job *out) {
    JsonTok *tok = xmalloc(TOKENS * sizeof *tok);
    Json j;
    int got = -1;
    if (json_parse(&j, text, n, tok, TOKENS) && json_type(&j, 0) == JSON_OBJECT) {
        int pages = json_get(&j, json_get(&j, 0, "query"), "pages");
        got = json_get(&j, 0, "error") >= 0 ? -1 : 0; /* (no "query": nothing found) */
        long long best_at = INT64_MAX;
        char want[300], yr[12];
        bare(title, want, sizeof want);
        snprintf(yr, sizeof yr, "%d", year);
        for (int k = json_type(&j, pages) == JSON_ARRAY ? json_child(&j, pages) : -1; k >= 0; k = json_next(&j, pages, k)) {
            char t[200] = "", d[200] = "", src[600] = "", head[200], b[300];
            long long at = 0;
            json_string(&j, json_get(&j, k, "title"), t, sizeof t);
            json_string(&j, json_get(&j, k, "description"), d, sizeof d);
            json_integer(&j, json_get(&j, k, "index"), &at);
            if (!t[0] || !json_string(&j, json_get(&j, json_get(&j, k, "original"), "source"), src, sizeof src) ||
                !picture_address(src))
                continue;
            const char *br = t[strlen(t) - 1] == ')' ? strrchr(t, '(') : NULL;
            str_copy(head, sizeof head, t);
            size_t hl = br ? (size_t)(br - t) : strlen(t);
            while (hl && head[hl - 1] == ' ') hl--;
            head[hl] = '\0';
            bare(head, b, sizeof b);
            bool film = fold_find(d, "film") || (br && fold_find(br, "film"));
            bool dated = year ? strstr(d, yr) || (br && strstr(br, yr)) : str_eq(b, want);
            if (!(pinned || (film && dated)) || at >= best_at) continue;
            best_at = at;
            got = 1;
            src[strcspn(src, "?#")] = '\0'; /* (Wikipedia's "?utm_source=..." left off) */
            str_copy(out->img, sizeof out->img, src);
            out->img2[0] = '\0';
            str_copy(out->ref, sizeof out->ref, t);
            if (br || !d[0]) str_copy(out->name, sizeof out->name, t);
            else snprintf(out->name, sizeof out->name, "%s (%s)", t, d);
        }
    }
    free(tok);
    return got;
}

/* "https://static.tvmaze.com/uploads/x.jpg" -> its host and path */
static bool split_url(const char *url, char *host, size_t hcap, const char **path) {
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    size_t hn = strcspn(p, "/?#");
    if (!hn || hn >= hcap) return false;
    str_ncopy(host, hcap, p, hn);
    *path = p[hn] == '/' ? p + hn : "/";
    return true;
}

/* A picture's bytes: TVmaze's over plain http (it answers there too, no curl needed), any other over https. */
static bool fetch_picture(const char *url, Fetched *f) {
    char host[128];
    const char *path;
    if (!split_url(url, host, sizeof host, &path)) {
        memset(f, 0, sizeof *f);
        str_copy(f->err, sizeof f->err, "not a picture's address");
        return false;
    }
    bool ok = str_eq(host, TVMAZE_PICS) ? fetch_get(host, 80, path, PICTURE_MAX, 30000, f) : fetch_https(url, PICTURE_MAX, 30000, f);
    if (ok && f->status != 200) {
        snprintf(f->err, sizeof f->err, "%s answered %d", host, f->status);
        ok = false;
    }
    return ok;
}

static const char *picture_ext(const char *p, size_t n) {
    const unsigned char *u = (const unsigned char *)p;
    if (n > 3 && u[0] == 0xFF && u[1] == 0xD8 && u[2] == 0xFF) return ".jpg";
    if (n > 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return ".png";
    if (n > 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) return ".webp";
    return NULL;
}

/* The picture to <dest><its extension> (dest = its folder + stem): 1 saved, -4 a picture is there (never over it),
   -1 not a picture or not written. Hemma's own one it replaces goes first, if it's still the size Hemma saved. Disk
   only: the thread. */
static int save_picture(const char *dest, const char *replace, int64_t replace_size, const char *p, size_t n, int64_t *size,
                        char *err, size_t ecap) {
    const char *ext = picture_ext(p, n), *slash = strrchr(dest, '/');
    if (!ext || !slash) {
        str_copy(err, ecap, "what came wasn't a JPEG, PNG or WebP picture");
        return -1;
    }
    char dir[700], path[800];
    str_ncopy(dir, sizeof dir, dest, (size_t)(slash - dest));
    int64_t was = -1;
    if (replace && *replace && path_stat(replace, &was, NULL, NULL) && was == replace_size) file_remove(replace);
    if (media_art_on_disk(dir, slash + 1)) return -4;
    snprintf(path, sizeof path, "%s%s", dest, ext);
    if (!file_write_atomic(path, p, n)) {
        snprintf(err, ecap, "couldn't write %.120s", path);
        return -1;
    }
    *size = (int64_t)n;
    return 1;
}

static void ask_wiki(Job *j) {
    Fetched f;
    if (!fetch_https(j->path, ANSWER_MAX, 20000, &f)) {
        j->got = f.exit == 6 || f.exit == 7 || f.exit == 28 ? -1 : -3; /* (no network; else curl's own trouble) */
        str_copy(j->err, sizeof j->err, f.err);
    } else if (f.status != 200) {
        j->got = -3;
        snprintf(j->err, sizeof j->err, "%s answered %d", WIKI, f.status);
    } else {
        j->got = wiki_answer(f.body.p ? f.body.p : "", f.body.len, j->title, j->year, j->pinned, j);
        if (j->got == -1) {
            j->got = -3;
            str_copy(j->err, sizeof j->err, "an answer from Wikipedia Hemma didn't understand");
        }
    }
    fetched_free(&f);
}

static void ask_picture(Job *j) {
    j->art_got = 0;
    if (!j->img[0]) return; /* (found, but there's no picture there) */
    Fetched f;
    bool ok = fetch_picture(j->img, &f);
    if (!ok && j->img2[0]) { /* (the big one too big, or gone: the smaller one) */
        fetched_free(&f);
        ok = fetch_picture(j->img2, &f);
    }
    if (ok) j->art_got = save_picture(j->dest, j->replace, j->replace_size, f.body.p ? f.body.p : "", f.body.len, &j->art_size,
                                      j->art_err, sizeof j->art_err);
    else {
        j->art_got = -1;
        str_copy(j->art_err, sizeof j->art_err, f.err);
    }
    fetched_free(&f);
}

/* TVmaze or OMDb: one GET (and TVmaze's redirect from an IMDb id to the show's number, asked again over http) */
static void ask_service(Job *j) {
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
}

/* The thread: the ask, then the picture it found (when one is wanted) */
static void ask_thread(void *arg) {
    Job *j = arg;
    thread_background();
    if (j->wiki) ask_wiki(j);
    else ask_service(j);
    if ((j->art || j->both) && j->got == 1 && j->dest[0]) ask_picture(j);
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

/* Wikipedia's search for a film by its title and year, with each page's description and picture - or (an admin's
   link) that one page, in its own language's Wikipedia. Non-free pictures too (pilicense=any): film posters are. */
static void path_wiki(Job *j, const char *lang, const char *page) {
    Buf p = {0};
    buf_printf(&p, "https://%s.wikipedia.org/w/api.php?action=query&format=json&formatversion=2", page ? lang : "en");
    if (page) {
        buf_puts(&p, "&redirects=1&titles=");
        url_encode(&p, page, false);
    } else {
        char q[200];
        if (j->year) snprintf(q, sizeof q, "%s %d film", j->title, j->year);
        else snprintf(q, sizeof q, "%s film", j->title);
        buf_puts(&p, "&generator=search&gsrnamespace=0&gsrlimit=6&gsrsearch=");
        url_encode(&p, q, false);
    }
    buf_puts(&p, "&prop=pageimages%7Cdescription&piprop=original&pilicense=any");
    str_copy(j->path, sizeof j->path, buf_cstr(&p));
    buf_free(&p);
}

static bool due(const char *work, int64_t now) {
    const MediaLookup *r = row_of(work);
    return !r || (!r->pinned && !r->found && now - r->at >= NOT_FOUND_DAYS * 86400LL);
}

/* A picture to look for: none found yet, or nothing by its name a while ago (couldn't: a day ago) - never one an
   admin's link gave, or one Hemma saved (gone since: someone took it away). */
static bool art_due(const char *work, int64_t now) {
    const MediaArt *r = art_of(work);
    return !r || (!r->pinned && r->got <= 0 && now - r->at >= (r->got == 0 ? NOT_FOUND_DAYS : 1) * 86400LL);
}

/* where a work's picture goes: "<its folder>/<stem>" (media_art_target's place) */
static bool art_dest(uint64_t id, char *dest, size_t cap) {
    char abs[600], stem[300];
    if (!media_art_folder(id, abs, sizeof abs, stem, sizeof stem)) return false;
    return (size_t)snprintf(dest, cap, "%s/%s", abs, stem) < cap;
}

static Job *job_new(const char *work, const char *title, int year) {
    Job *j = xcalloc(1, sizeof *j);
    str_copy(j->work, sizeof j->work, work);
    str_copy(j->title, sizeof j->title, title);
    j->year = year;
    return j;
}

/* What to ask next by name: series' genres (with their pictures, when both are wanted), series' pictures, films'
   genres (with a key OMDb hasn't refused), films' pictures (with curl). NULL = nothing. */
static Job *next_job(void) {
    if (g_hold) { /* (a rename: wait for the library to read it) */
        if (media_lib_gen() == g_hold_gen || media_scanning() || media_rescan_queued()) return NULL;
        g_hold = false;
    }
    const MediaIndex *ix = media_lib();
    int64_t now = now_sec();
    MediaWork w;
    bool cats = lookup_on(), pics = art_on();
    for (int i = 0; cats && i < ix->nseries; i++) {
        const MediaSeries *s = &ix->series[i];
        media_work_series(s, &w);
        if (!due(w.key, now)) continue;
        Job *j = job_new(w.key, s->title, s->year);
        path_tvmaze(j);
        j->both = pics && !s->poster && art_due(w.key, now) && art_dest(s->id, j->dest, sizeof j->dest);
        return j;
    }
    for (int i = 0; pics && i < ix->nseries; i++) {
        const MediaSeries *s = &ix->series[i];
        media_work_series(s, &w);
        if (s->poster || !art_due(w.key, now)) continue;
        Job *j = job_new(w.key, s->title, s->year);
        j->art = true;
        if (!art_dest(s->id, j->dest, sizeof j->dest)) {
            free(j);
            continue;
        }
        const MediaLookup *r = row_of(w.key); /* (the show an admin's id named, when there's one) */
        if (r && r->pinned && r->found && str_eq(r->source, "tvmaze") && r->ref[0]) snprintf(j->path, sizeof j->path, "/shows/%s", r->ref);
        else path_tvmaze(j);
        return j;
    }
    for (int i = 0; cats && setting(SETTING_KEY, "")[0] && !g_omdb_bad[0] && i < ix->nmovies; i++) {
        const MediaFile *f = &ix->files[ix->movies[i]];
        media_work_film(f, &w);
        if (!due(w.key, now)) continue;
        Job *j = job_new(w.key, f->name.title, f->name.year);
        j->omdb = true;
        path_omdb(j, NULL);
        return j;
    }
    for (int i = 0; pics && i < ix->nmovies && fetch_curl(); i++) {
        const MediaFile *f = &ix->files[ix->movies[i]];
        media_work_film(f, &w);
        if (f->poster || !art_due(w.key, now)) continue;
        Job *j = job_new(w.key, f->name.title, f->name.year);
        j->art = j->wiki = true;
        if (!art_dest(f->id, j->dest, sizeof j->dest)) {
            free(j);
            continue;
        }
        path_wiki(j, NULL, NULL);
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

/* A picture's answer, on the event loop (j->got: what the ask found; j->art_got: the picture, when it found one) */
static void settle_art(Job *j, int64_t now) {
    if (j->got <= 0 && j->pinned) { /* a link that names nothing (or an answer not understood): what was there stays */
        if (j->asker && j->got) app_notify_person(j->asker, TOAST_ERR, N_("Couldn't look it up: %s"), j->err);
        else if (j->asker) app_notify_person(j->asker, TOAST_ERR, N_("Nothing has that id"));
        return;
    }
    if (j->got == 1 && j->art_got == -4) { /* a picture came meanwhile: the household's own */
        drop_art(j->work);
        if (j->asker) app_notify_person(j->asker, TOAST_INFO, N_("It has a picture of its own now: that one stays"));
        return;
    }
    if (j->got == 1 && j->art_got < 0 && j->pinned) { /* (the picture of the link given didn't come: what was there stays) */
        if (j->asker) app_notify_person(j->asker, TOAST_ERR, N_("Couldn't fetch the picture: %s"), j->art_err);
        return;
    }
    int got = j->got == 1 ? (j->art_got == 1 ? 1 : j->art_got == 0 ? 0 : -1) : j->got == 0 ? 0 : -1;
    drop_art(j->work);
    MediaArt r = {.got = got, .pinned = j->pinned, .size = got == 1 ? j->art_size : 0, .at = now};
    str_copy(r.work, sizeof r.work, j->work);
    str_copy(r.source, sizeof r.source, j->wiki ? "wikipedia" : "tvmaze");
    if (j->got == 1) {
        str_copy(r.ref, sizeof r.ref, j->ref);
        str_copy(r.name, sizeof r.name, j->name);
    }
    tbl_put(&T_media_art, &r);
    const char *why = j->art_err[0] ? j->art_err : j->err;
    if (got == 1) log_info("media: a picture for %s from %s: %s", j->title, r.source, r.name);
    else if (got < 0) log_info("media: no picture for %s from %s now: %s", j->title, r.source, why);
    if (got == 1) {
        g_art_last_ms = now_ms(); /* (the library looks again soon: media_lookup_tick) */
        if (!g_art_first_ms) g_art_first_ms = g_art_last_ms;
    }
    if (j->asker) {
        if (got == 1) app_notify_person(j->asker, TOAST_INFO, N_("Found a picture: %s"), r.name);
        else if (got == 0 && j->got == 1) app_notify_person(j->asker, TOAST_INFO, N_("%s has no picture there"), j->name);
        else if (got == 0) app_notify_person(j->asker, TOAST_INFO, N_("Nothing found by the name %s"), j->title);
        else app_notify_person(j->asker, TOAST_ERR, N_("Couldn't fetch the picture: %s"), why);
        app_changed_where("/media");
    } else if (!g_dirty_ms) {
        g_dirty_ms = now_ms();
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
    if (j->art) {
        settle_art(j, now);
        return;
    }
    if (j->both) settle_art(j, now); /* (its genres below, as for any series) */
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
    if (g_art_last_ms && (now - g_art_last_ms >= ART_QUIET_MS || now - g_art_first_ms >= ART_RESCAN_MS)) {
        g_art_first_ms = g_art_last_ms = 0; /* pictures saved: the library reads them in */
        media_scan_start();
    }
    if (app_testing() || g_running) return;
    int64_t t = now_sec();
    bool any = lookup_on() || art_on();
    if (g_probe_wanted || (any && g_online < 0 && t - g_online_at >= OFFLINE_WAIT_S)) {
        g_probe_wanted = false;
        Job *p = xcalloc(1, sizeof *p);
        p->probe = true;
        str_copy(p->path, sizeof p->path, "/");
        launch(p);
        return;
    }
    if (!any || g_online < 0 || now < g_next_ms) return;
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

static int with_picture(bool films, int *all) {
    const MediaIndex *ix = media_lib();
    int n = 0;
    *all = films ? ix->nmovies : ix->nseries;
    for (int i = 0; i < *all; i++) n += (films ? ix->files[ix->movies[i]].poster : ix->series[i].poster) != 0;
    return n;
}

static void art_card(Buf *b) {
    bool on = art_on(), off_line = g_online < 0;
    ui_card(b, T("Pictures from the internet"), NULL, NULL);
    H(b, "<p class=\"muted media-small\">%s</p><p id=\"media-pics\" class=\"media-lookup-at\">",
      T("Hemma can find a picture for each series and film that has none: a series' at TVmaze, a film's at Wikipedia by its "
        "title and year. Only titles and years are sent, and a picture you set yourself always stays. Nothing is looked up "
        "until you tick the box."));
    if (!g_online) {
        H(b, "%s", T("Checking whether this Hemma can reach the internet…"));
    } else if (off_line) {
        H(b, "<span class=\"media-warn\">%s</span>", T("This Hemma can't reach the internet right now, so it can't look anything up."));
    } else if (on) {
        int all = 0, n = with_picture(false, &all);
        H(b, T("With a picture: %d of %d series"), n, all);
        n = with_picture(true, &all);
        buf_puts(b, " · ");
        H(b, T("%d of %d films"), n, all);
    } else {
        H(b, "%s", T("This Hemma can reach the internet."));
    }
    buf_puts(b, "</p>");
    if (!fetch_curl())
        H(b, "<p class=\"media-warn\">%s</p>",
          T("Films' pictures come over https, which needs the program curl - and this computer doesn't have it. Series' "
            "pictures still come."));
    ui_form(b, "media.lookup_art", "stack");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r%r> %s</label>"
         "<div><button class=\"btn sm\">%s</button></div>",
      on ? " checked" : "", off_line && !on ? " disabled" : "", T("Find pictures online"), T("Save"));
    ui_form_end(b);
    H(b, "<p class=\"muted media-small\">%r</p>",
      T("Series' pictures come from <a href=\"https://www.tvmaze.com\" rel=\"noreferrer\">TVmaze</a>, films' from "
        "<a href=\"https://en.wikipedia.org\" rel=\"noreferrer\">Wikipedia</a> - the studios' own posters, for your "
        "household. Those found stay when you switch it off. A wrong one? Give the right one's link under the title's "
        "Categories, or set a picture of your own."));
    ui_card_end(b);
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
    art_card(b);
}

/* the work's picture now (an MF_IMAGE file's id, 0 = none) - and whether it's the one Hemma saved */
static uint64_t poster_of(const MediaWork *w) { return w->s ? w->s->poster : w->f ? w->f->poster : 0; }

static bool hemmas_own(const MediaWork *w, const MediaArt *r) {
    uint64_t p = poster_of(w);
    const MediaFile *f = p ? media_file(p) : NULL;
    return f && r && r->got == 1 && f->size == r->size;
}

/* what one title's picture is, and an admin's link to the right one */
static void art_one(Buf *b, const MediaWork *w) {
    if (!art_on()) return;
    const MediaArt *r = art_of(w->key);
    bool film = w->f != NULL, own = poster_of(w) && !hemmas_own(w, r);
    const char *where = r && str_eq(r->source, "wikipedia") ? "Wikipedia" : "TVmaze";
    buf_puts(b, "<p class=\"muted\" id=\"media-pic-one\">");
    if (own) {
        H(b, "%s", T("Picture: its own - Hemma never changes it."));
    } else if (r && r->got == 1) {
        H(b, T("Picture: from %s, %s"), where, r->name);
        if (r->pinned) H(b, " · %s", T("by the link you gave"));
    } else if (r && r->got == 0) {
        H(b, T("Picture: nothing found by its name at %s."), where);
    } else if (r) {
        H(b, "%s", T("Picture: it couldn't be fetched - Hemma tries again tomorrow."));
    } else {
        H(b, "%s", T("Picture: not looked for yet."));
    }
    buf_puts(b, "</p>");
    if (own) return;
    if (film && !fetch_curl()) {
        H(b, "<p class=\"muted media-small\">%s</p>", T("Films' pictures need the program curl, which this computer doesn't have."));
        return;
    }
    ui_form(b, "media.art_id", "stack");
    ui_hidden(b, "work", w->key);
    H(b, "<label class=\"field\"><span>%s</span><input name=\"id\" maxlength=\"300\" autocomplete=\"off\" "
         "spellcheck=\"false\" placeholder=\"%s\"%r></label><div><button class=\"btn\"%r>%s</button></div>",
      film ? T("Its Wikipedia link, when the name finds the wrong one")
           : T("Its TVmaze or IMDb link or id, when the name finds the wrong one"),
      film ? "https://en.wikipedia.org/wiki/Hocus_Pocus_(1993_film)" : "https://www.tvmaze.com/shows/84",
      g_online < 0 ? " disabled" : "", g_online < 0 ? " disabled" : "", T("Update picture"));
    ui_form_end(b);
}

static void cats_one(Buf *b, const MediaWork *w);

void media_lookup_form(Ctx *c, Buf *b, const MediaWork *w) {
    if (!is_admin(c)) return;
    H(b, "<div class=\"media-lookup-one\" id=\"media-lookup-one\"><h3>%s</h3>", T("From the internet"));
    if (lookup_on() || !art_on()) cats_one(b, w); /* (only pictures on: nothing about categories) */
    art_one(b, w);
    buf_puts(b, "</div>");
}

static void cats_one(Buf *b, const MediaWork *w) {
    const MediaLookup *r = row_of(w->key);
    bool on = lookup_on(), film = w->f != NULL;
    buf_puts(b, "<p class=\"muted\">");
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
        H(b, "<p class=\"muted media-small\">%s <a href=\"" MEDIA_SETTINGS_ONLINE "\">%s</a></p>",
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

/* media.lookup_art on: finding pictures online (2.5) */
void act_media_lookup_art(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    bool on = str_eq(fields_str(f, "on"), "1"), was = art_on();
    if (on && !was && g_online < 0) {
        ctx_fail(c, "%s", T("This Hemma can't reach the internet right now"));
        return;
    }
    setting_set_int(SETTING_ART, on ? 1 : 0);
    if (on != was) log_info("media: %s switched %s looking up pictures online", c->me->name, on ? "on" : "off");
    if (on && !was) g_probe_wanted = true;
    if (on != was) ctx_ok(c, "%s", on ? T("Hemma looks for pictures online now") : T("Hemma doesn't look for pictures online now"));
    else ctx_ok(c, "%s", T("Saved"));
}

/* "https://en.wikipedia.org/wiki/Hocus_Pocus_(1993_film)" (or en.m.wikipedia.org) -> "en", "Hocus Pocus (1993 film)" */
static bool id_wiki(const char *s, char *lang, size_t lcap, char *page, size_t pcap) {
    const char *w = strstr(s, ".wikipedia.org/wiki/");
    if (!w) return false;
    const char *start = w;
    while (start > s && start[-1] != '/') start--;
    size_t ln = strcspn(start, ".");
    if (!ln || ln >= lcap) return false;
    for (size_t i = 0; i < ln; i++)
        if (!((start[i] >= 'a' && start[i] <= 'z') || start[i] == '-')) return false;
    str_ncopy(lang, lcap, start, ln);
    const char *t = w + strlen(".wikipedia.org/wiki/");
    size_t tn = strcspn(t, "?#");
    if (!tn || tn >= pcap) return false;
    url_decode(t, tn, page, false);
    for (char *p = page; *p; p++)
        if (*p == '_') *p = ' ';
    return page[0] != '\0';
}

/* media.art_id work id: its picture by the link given (series: TVmaze or IMDb; films: Wikipedia) - or, with none, by
   its name again. Only where it has no picture, or the one Hemma saved (replaced). */
void act_media_art_id(Ctx *c, Fields *f) {
    MediaWork w;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    if (!media_work_find(fields_str(f, "work"), &w)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    if (!art_on() || g_online < 0) {
        ctx_fail(c, "%s", !art_on() ? T("Switch on finding pictures online first (Library settings)")
                                    : T("This Hemma can't reach the internet right now"));
        return;
    }
    const MediaArt *r = art_of(w.key);
    uint64_t poster = poster_of(&w);
    bool ours = hemmas_own(&w, r);
    if (poster && !ours) {
        ctx_fail(c, "%s", T("It has a picture of its own - Hemma never changes that one"));
        return;
    }
    if (w.f && !fetch_curl()) {
        ctx_fail(c, "%s", T("Films' pictures need the program curl, which this computer doesn't have"));
        return;
    }
    char given[400], imdb[16], lang[16] = "en", page[300];
    str_copy(given, sizeof given, fields_str(f, "id"));
    const char *id = str_trim(given);
    long tvm = w.s ? id_tvmaze(id) : 0;
    bool by_imdb = w.s && id_imdb(id, imdb), by_wiki = w.f && id_wiki(id, lang, sizeof lang, page, sizeof page);
    if (*id && !tvm && !by_imdb && !by_wiki) {
        ctx_fail_at(c, "id", "%s", w.f ? T("Paste its Wikipedia link, like https://en.wikipedia.org/wiki/Hocus_Pocus_(1993_film)")
                                       : T("Paste its TVmaze or IMDb link, or an id like 84 or tt0182576"));
        return;
    }
    Job *j = job_new(w.key, w.s ? w.s->title : w.f->name.title, w.s ? w.s->year : w.f->name.year);
    j->art = true;
    j->wiki = w.f != NULL;
    j->pinned = *id != '\0';
    j->asker = c->me->id;
    if (!art_dest(w.s ? w.s->id : w.f->id, j->dest, sizeof j->dest)) {
        free(j);
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    const MediaFile *pf = ours ? media_file(poster) : NULL;
    if (pf && media_full_path(pf, j->replace, sizeof j->replace)) j->replace_size = pf->size;
    if (w.f) path_wiki(j, lang, by_wiki ? page : NULL);
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

/* pictures (2.5) */
int media_lookup_test_tvmaze_pic(const char *json, const char *title, int year, char *img, size_t cap) {
    Job j = {0};
    int got = tvmaze_answer(json, strlen(json), title, year, &j);
    str_copy(img, cap, j.img);
    return got;
}

int media_lookup_test_wiki(const char *json, const char *title, int year, bool pinned, char *img, size_t icap, char *name,
                           size_t ncap) {
    Job j = {0};
    int got = wiki_answer(json, strlen(json), title, year, pinned, &j);
    str_copy(img, icap, j.img);
    str_copy(name, ncap, j.name);
    return got;
}

char media_lookup_test_job(char *host, size_t hcap, char *path, size_t pcap, char *dest, size_t dcap) {
    Job *j = g_ask ? g_ask : next_job();
    if (j == g_ask) g_ask = NULL;
    if (!j) return 0;
    char kind = j->art ? 'p' : j->both ? 'b' : 'c';
    str_copy(host, hcap, j->wiki ? WIKI : j->omdb ? OMDB : TVMAZE);
    str_copy(path, pcap, j->path);
    str_copy(dest, dcap, j->replace[0] ? j->replace : j->dest);
    free(j);
    return kind;
}

int media_lookup_test_save(const char *dest, const void *bytes, size_t n, const char *replace, int64_t replace_size) {
    char err[160];
    int64_t size = 0;
    return save_picture(dest, replace, replace_size, bytes, n, &size, err, sizeof err);
}

void media_lookup_test_art_put(const char *work, int got, int64_t size, bool pinned) {
    drop_art(work);
    MediaArt r = {.got = got, .size = size, .pinned = pinned, .at = now_sec()};
    str_copy(r.work, sizeof r.work, work);
    str_copy(r.source, sizeof r.source, "tvmaze");
    str_copy(r.name, sizeof r.name, "Test (2001)");
    tbl_put(&T_media_art, &r);
}
