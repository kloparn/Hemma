/* modules/media — the home cinema.
 *
 * Browse the household's movies, series and other videos from a local drive
 * and watch them in any browser on the network. The library is scanned by a
 * background thread (library.c), files stream with Range support from worker
 * threads (stream.c) and every person's progress is remembered, so "continue
 * watching" works across the phone, the laptop and the TV's browser. */
#include "../../core/ui.h"
#include "library.h"
#include "convert.h"
#include "remove.h"
#include "upload.h"
#include "health.h"

static int64_t g_disks_at; /* when the disks under the library were last measured (see "disk space") */

/* ---------------------------------------------------------------- helpers */

static const char *lang_name(const char *code) {
    static const struct {
        const char *code, *name;
    } L[] = {{"sv", "Svenska"}, {"en", "English"}, {"no", "Norsk"},    {"nb", "Norsk"},   {"da", "Dansk"},
             {"fi", "Suomi"},   {"de", "Deutsch"}, {"fr", "Français"}, {"es", "Español"}, {"it", "Italiano"},
             {"nl", "Nederlands"}, {"pl", "Polski"}, {"pt", "Português"}, {"ja", "日本語"}};
    for (int i = 0; i < countof(L); i++)
        if (str_ieq(code, L[i].code)) return L[i].name;
    return code && *code ? code : "Subtitles";
}

static const char *query(const Ctx *c) {
    const char *q = ctx_live(c, "media.q");
    return *q ? q : ctx_q(c, "q");
}

static int percent(const MediaWatch *w) {
    if (!w || w->dur <= 0 || w->pos <= 0) return 0;
    return CLAMP(w->pos * 100 / w->dur, 1, 100);
}

static MediaWatch *watch_of(const Ctx *c, const MediaFile *f) {
    return c->me && f ? media_watch_get(c->me->id, f->id) : NULL;
}

/* Episodes in a series, counting both halves of a two-episode file ("S01E01-E02"). */
static int episodes_in(const MediaSeries *s) {
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        n += e && e->name.episode_end > e->name.episode ? e->name.episode_end - e->name.episode + 1 : 1;
    }
    return n;
}

/* The number in an episode row: "5", or "1–2" for two episodes in one file. */
static const char *ep_n(const MediaFile *f) {
    static char n[24];
    if (f->name.episode_end > f->name.episode) snprintf(n, sizeof n, "%d–%d", f->name.episode, f->name.episode_end);
    else snprintf(n, sizeof n, "%d", f->name.episode);
    return n;
}

static void ep_code(Buf *b, const MediaFile *f) {
    H(b, "S%02dE%02d", f->name.season, f->name.episode);
    if (f->name.episode_end > f->name.episode) H(b, "–E%02d", f->name.episode_end);
}

/* Bold line of an episode row: its title, or "Episode 3". */
static void ep_title(Buf *b, const MediaFile *f) {
    if (f->name.ep_title[0]) html_esc(b, f->name.ep_title);
    else H(b, "Episode %d", f->name.episode);
}

static void quality(Buf *b, const MediaFile *f) {
    int h = f->probe.height, w = f->probe.width;
    if (h >= 2000 || w >= 3800) buf_puts(b, "4K");
    else if (h >= 1000 || w >= 1900) buf_puts(b, "1080p");
    else if (h >= 700 || w >= 1270) buf_puts(b, "720p");
    else if (h > 0) buf_puts(b, "SD");
}

static void duration(Buf *b, const MediaFile *f) {
    char d[32];
    media_fmt_duration(f->probe.duration_ms, d, sizeof d);
    html_esc(b, d);
}

/* /media/img/<id>?v=<version>: a replaced picture keeps its name (and id), and browsers keep
   images for a day - the version makes them fetch the new one at once. */
static void img_src(Buf *b, uint64_t img) {
    const MediaFile *f = media_file(img);
    H(b, "/media/img/%X?v=%X", img, f ? (uint64_t)f->mtime * 1000003u + (uint64_t)f->size : (uint64_t)0);
}

/* The art of a poster: an image when the library has one, otherwise a
 * typographic card in the title's colour. */
static void poster_art(Buf *b, const char *title, int year, const char *label, uint64_t img, int pct, bool done) {
    H(b, "<span class=\"media-art\"");
    ui_hue_style(b, media_hue(title));
    buf_putc(b, '>');
    if (img) {
        buf_puts(b, "<img src=\"");
        img_src(b, img);
        buf_puts(b, "\" alt=\"\" loading=\"lazy\" decoding=\"async\">");
    } else {
        if (label) H(b, "<span class=\"media-art-k\">%s</span>", label);
        H(b, "<span class=\"media-art-t\">%s</span>", title);
        if (year) H(b, "<span class=\"media-art-y\">%d</span>", year);
    }
    if (done) {
        buf_puts(b, "<span class=\"media-art-done\" title=\"Watched\">");
        ui_icon(b, "check");
        buf_puts(b, "</span>");
    } else if (pct > 0) {
        H(b, "<span class=\"media-art-bar\"><i style=\"width:%d%%\"></i></span>", pct);
    }
    buf_puts(b, "</span>");
}

static void video_card(Ctx *c, Buf *b, const MediaFile *f) {
    MediaWatch *w = watch_of(c, f);
    H(b, "<a class=\"media-poster\" href=\"/media/watch/%X\">", f->id);
    if (f->name.kind == MEDIA_EPISODE) {
        const MediaSeries *s = media_series(f->series);
        poster_art(b, f->name.title, 0, NULL, f->poster ? f->poster : s ? s->poster : 0, percent(w), w && w->done);
        H(b, "<span class=\"media-cap\">%s<small>", f->name.title);
        ep_code(b, f);
        if (f->name.ep_title[0]) H(b, " · %s", f->name.ep_title);
    } else {
        poster_art(b, f->name.title, f->name.year, f->name.kind == MEDIA_OTHER ? "Video" : NULL, f->poster, percent(w),
                   w && w->done);
        H(b, "<span class=\"media-cap\">%s<small>", f->name.title);
        if (f->name.year) H(b, "%d", f->name.year);
        if (f->name.year && f->probe.duration_ms) buf_puts(b, " · ");
        duration(b, f);
    }
    buf_puts(b, "</small></span></a>");
}

static void going_card(Ctx *c, Buf *b, const MediaFile *f, bool small); /* (below, next to mini) */

/* --------------------------------------------------------------- pictures */

/* Admins pick a series' or film's picture: one image from this device, sent like an upload
   (upload.c, form[data-upload]) and saved where the scan looks first (media_art_target). */
static bool can_set_art(const Ctx *c, uint64_t id) {
    int root;
    char dir[400], stem[400];
    return is_admin(c) && media_art_target(id, &root, dir, sizeof dir, stem, sizeof stem);
}

static void art_modal(Ctx *c, Buf *b, uint64_t id, const char *title, int year, const char *label, uint64_t img,
                      const char *close) {
    char hex[17], abs[1024], stem[400];
    media_id_hex(id, hex);
    if (!media_art_folder(id, abs, sizeof abs, stem, sizeof stem)) return;
    ui_modal(b, "Picture", close);
    buf_puts(b, "<div class=\"media-pic\"><div class=\"media-pic-art\">");
    poster_art(b, title, year, label, img, 0, false);
    /* the form first, so status lines appearing below never rebuild it (and drop the chosen file) */
    H(b,
      "</div><div class=\"stack\"><form class=\"stack\" id=\"media-pic-f\" data-upload=\"/media/upload\" "
      "data-empty=\"Choose a picture first\"><input type=\"hidden\" name=\"art\" value=\"%s\">"
      "<label class=\"field\"><span>A picture from this device</span><input type=\"file\" name=\"files\" "
      "accept=\"image/jpeg,image/png,image/webp\"></label>"
      "<p class=\"muted media-small\">JPEG, PNG or WebP - a tall poster (2:3) fits best. It's saved in the library "
      "folder as <code class=\"media-code\">%s.jpg</code> (or .png/.webp), where other players find it too.</p>"
      "<div class=\"modal-f\"><button class=\"btn primary\">Use this picture</button></div></form>",
      hex, stem);
    upload_art_status(c, b, id);
    if (media_art_current(id)) {
        ui_form_confirm(b, "media.art_remove", "inline", "Remove this picture? Its file is deleted from the library folder.");
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%s\"><button class=\"btn\">Remove picture</button>", hex);
        ui_form_end(b);
    }
    buf_puts(b, "</div></div>");
    ui_modal_end(b);
}

static int watched_count(const Ctx *c, const MediaSeries *s) {
    int n = 0;
    for (int i = 0; c->me && i < s->count; i++) {
        MediaWatch *w = media_watch_get(c->me->id, media_episode(s, i)->id);
        n += w && w->done;
    }
    return n;
}

static void series_card(Ctx *c, Buf *b, const MediaSeries *s) {
    H(b, "<a class=\"media-poster\" href=\"/media/series/%X\">", s->id);
    int seen = watched_count(c, s);
    poster_art(b, s->title, s->year, "Series", s->poster, seen ? seen * 100 / s->count : 0, seen == s->count && seen);
    int eps = episodes_in(s);
    H(b, "<span class=\"media-cap\">%s<small>%d episode%s", s->title, eps, eps == 1 ? "" : "s");
    if (s->nseasons > 1) H(b, " · %d seasons", s->nseasons);
    buf_puts(b, "</small></span></a>");
}

/* This person's unfinished videos, most recently watched first. */
static int in_progress(const Ctx *c, const MediaFile **out, int max) {
    const MediaWatch *keep[24];
    int n = 0;
    max = MIN(max, countof(keep));
    for (int i = 0; c->me && i < tbl_count(&T_media_watch); i++) {
        const MediaWatch *w = tbl_at(&T_media_watch, i);
        uint64_t id = 0;
        if (w->person != c->me->id || w->done || w->pos < 10 || !media_parse_id(w->item, &id) || !media_file(id))
            continue;
        if (n == max && keep[n - 1]->updated >= w->updated) continue;
        int k = n < max ? n++ : n - 1; /* full: the oldest falls off the end */
        while (k > 0 && keep[k - 1]->updated < w->updated) keep[k] = keep[k - 1], k--;
        keep[k] = w;
    }
    for (int i = 0; i < n; i++) {
        uint64_t id = 0;
        media_parse_id(keep[i]->item, &id);
        out[i] = media_file(id);
    }
    return n;
}

/* What to play next in a series for this person: the first unfinished episode
 * after the last one they finished (or the one they're in the middle of). */
static const MediaFile *next_up(const Ctx *c, const MediaSeries *s) {
    int last_done = -1;
    for (int i = 0; c->me && i < s->count; i++) {
        MediaWatch *w = media_watch_get(c->me->id, media_episode(s, i)->id);
        if (w && w->done) last_done = i;
        else if (w && w->pos > 0) return media_episode(s, i);
    }
    return media_episode(s, last_done + 1 < s->count ? last_done + 1 : 0);
}

/* ------------------------------------------------------------------ chrome */

static void lib_head(Ctx *c, Buf *b, const char *active, const char *title, const char *sub) {
    ui_head(b, title, sub);
    if (media_scanning())
        H(b, "<span class=\"badge media-scanning\"><span class=\"media-spin\"></span>Scanning · %d files</span>",
          media_scan_seen());
    buf_puts(b, "<a class=\"icon-btn\" href=\"/media/settings\" title=\"Library settings\" aria-label=\"Library settings\">");
    ui_icon(b, "settings");
    buf_puts(b, "</a>");
    ui_head_end(b);
    static const char *const tabs[][2] = {{"home", "/media"}, {"movies", "/media/movies"}, {"series", "/media/series"},
                                          {"browse", "/media/browse"}};
    static const char *const labels[] = {"Home", "Movies", "Series", "Folders"};
    buf_puts(b, "<div class=\"toolbar media-tools\"><nav class=\"seg\">");
    for (int i = 0; i < 4; i++)
        H(b, "<a href=\"%s\"%r>%s</a>", tabs[i][1], str_eq(active, tabs[i][0]) ? " class=\"on\"" : "", labels[i]);
    H(b,
      "</nav><form class=\"media-search\" method=\"get\" action=\"/media\" role=\"search\">"
      "<input type=\"search\" name=\"q\" value=\"%s\" placeholder=\"Search movies and series…\" autocomplete=\"off\" "
      "data-live=\"media.search\" data-key=\"/\" aria-label=\"Search the library\"></form></div>",
      query(c));
}

static void shelf_head(Buf *b, const char *title, const char *more) {
    H(b, "<section class=\"media-row\"><header class=\"media-row-h\"><h2>%s</h2>", title);
    if (more) H(b, "<a href=\"%s\">See all</a>", more);
    buf_puts(b, "</header><div class=\"media-shelf\">");
}

static void shelf_end(Buf *b) { buf_puts(b, "</div></section>"); }

static bool library_empty_states(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    if (ix->nroots == 0) {
        ui_empty(b, "🎬", "Point Hemma at your movies",
                 "Tell it which folders hold your movies and series — the drive on the Pi, a mounted share, "
                 "or a folder on this computer.");
        buf_puts(b, "<p class=\"media-center\"><a class=\"btn primary\" href=\"/media/settings\">Choose folders</a></p>");
        return true;
    }
    if (ix->nrecent == 0) {
        if (media_scanning())
            ui_empty(b, "🔎", "Looking through your library…", "Movies and series show up here as soon as the scan "
                                                               "finishes.");
        else
            ui_empty(b, "🗂️", "No videos found yet",
                     "Hemma looked in your folders but found no video files. Check the folders in Library settings.");
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- search */

static void search_results(Ctx *c, Buf *b, const char *q) {
    const MediaIndex *ix = media_lib();
    int shown = 0;
    buf_puts(b, "<div class=\"media-grid\">");
    for (int i = 0; i < ix->nseries && shown < 60; i++) {
        if (!fold_find(ix->series[i].title, q)) continue;
        series_card(c, b, &ix->series[i]);
        shown++;
    }
    for (int i = 0; i < ix->nrecent && shown < 60; i++) {
        const MediaFile *f = &ix->files[ix->recent[i]];
        bool hit = f->name.kind == MEDIA_EPISODE ? fold_find(f->name.ep_title, q) != NULL
                                                 : fold_find(f->name.title, q) || fold_find(f->rel, q);
        if (!hit) continue;
        video_card(c, b, f);
        shown++;
    }
    buf_puts(b, "</div>");
    if (!shown) ui_empty(b, "🤷", "Nothing matches", "Try another word — titles, episode names and folders are searched.");
}

/* ----------------------------------------------------------------- pages */

static void page_home(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "Watch");
    char sub[120] = "";
    if (ix->nrecent)
        snprintf(sub, sizeof sub, "%d movie%s · %d series · %d other video%s", ix->nmovies, ix->nmovies == 1 ? "" : "s",
                 ix->nseries, ix->nothers, ix->nothers == 1 ? "" : "s");
    lib_head(c, b, "home", "Watch", sub);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;

    const MediaFile *going[12];
    int ngoing = in_progress(c, going, countof(going));
    if (ngoing) {
        shelf_head(b, "Continue watching", NULL);
        for (int i = 0; i < ngoing; i++) going_card(c, b, going[i], false);
        shelf_end(b);
    }
    shelf_head(b, "Recently added", NULL);
    for (int i = 0; i < ix->nrecent && i < 14; i++) video_card(c, b, &ix->files[ix->recent[i]]);
    shelf_end(b);
    if (ix->nseries) {
        shelf_head(b, "Series", "/media/series");
        for (int i = 0; i < ix->nseries && i < 14; i++) series_card(c, b, &ix->series[i]);
        shelf_end(b);
    }
    if (ix->nmovies) {
        shelf_head(b, "Movies", "/media/movies");
        for (int i = 0; i < ix->nmovies && i < 14; i++) video_card(c, b, &ix->files[ix->movies[i]]);
        shelf_end(b);
    }
    if (ix->nothers) {
        shelf_head(b, "Other videos", "/media/browse");
        for (int i = 0; i < ix->nothers && i < 14; i++) video_card(c, b, &ix->files[ix->others[i]]);
        shelf_end(b);
    }
}

static const MediaIndex *g_sort_ix;
static int by_new(const void *x, const void *y) {
    const MediaFile *a = &g_sort_ix->files[*(const int *)x], *b = &g_sort_ix->files[*(const int *)y];
    return a->mtime != b->mtime ? (a->mtime > b->mtime ? -1 : 1) : 0;
}
static int by_year(const void *x, const void *y) {
    const MediaFile *a = &g_sort_ix->files[*(const int *)x], *b = &g_sort_ix->files[*(const int *)y];
    return b->name.year - a->name.year;
}

static void page_movies(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "Movies");
    char sub[40];
    snprintf(sub, sizeof sub, "%d movie%s", ix->nmovies, ix->nmovies == 1 ? "" : "s");
    lib_head(c, b, "movies", "Movies", sub);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    const char *sort = ctx_q(c, "sort");
    if (!*sort) sort = "az";
    H(b, "<nav class=\"seg media-sort\"><a href=\"/media/movies\"%r>A–Z</a><a href=\"/media/movies?sort=new\"%r>Newest</a>"
         "<a href=\"/media/movies?sort=year\"%r>Year</a></nav>",
      str_eq(sort, "az") ? " class=\"on\"" : "", str_eq(sort, "new") ? " class=\"on\"" : "",
      str_eq(sort, "year") ? " class=\"on\"" : "");
    int *order = xmalloc((size_t)(ix->nmovies ? ix->nmovies : 1) * sizeof(int));
    memcpy(order, ix->movies, (size_t)ix->nmovies * sizeof(int));
    g_sort_ix = ix;
    if (str_eq(sort, "new")) qsort(order, (size_t)ix->nmovies, sizeof(int), by_new);
    else if (str_eq(sort, "year")) qsort(order, (size_t)ix->nmovies, sizeof(int), by_year);
    buf_puts(b, "<div class=\"media-grid\">");
    for (int i = 0; i < ix->nmovies; i++) video_card(c, b, &ix->files[order[i]]);
    buf_puts(b, "</div>");
    if (!ix->nmovies) ui_empty(b, "🎞️", "No movies yet", "Files with a year in the name — like \"Inception (2010).mkv\" — "
                                                         "become movies.");
    free(order);
}

static void page_series_list(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "Series");
    char sub[40];
    snprintf(sub, sizeof sub, "%d series", ix->nseries);
    lib_head(c, b, "series", "Series", sub);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    buf_puts(b, "<div class=\"media-grid\">");
    for (int i = 0; i < ix->nseries; i++) series_card(c, b, &ix->series[i]);
    buf_puts(b, "</div>");
    if (!ix->nseries) ui_empty(b, "📺", "No series yet", "Episodes named like \"Show.S01E02.mkv\" are grouped into series.");
}

static void tick_form(Ctx *c, Buf *b, const MediaFile *f, bool done) {
    ui_form(b, "media.watched", "inline");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"done\" value=\"%d\">", f->id,
      done ? 0 : 1);
    H(b, "<button class=\"tick%r\" title=\"%s\" aria-label=\"%s\">", done ? " on" : "",
      done ? "Mark as not watched" : "Mark as watched", done ? "Mark as not watched" : "Mark as watched");
    ui_icon(b, "check");
    buf_puts(b, "</button>");
    ui_form_end(b);
}

static void page_series(Ctx *c, Buf *b, const char *idhex) {
    uint64_t id = 0;
    const MediaSeries *s = media_parse_id(idhex, &id) ? media_series(id) : NULL;
    if (!s) {
        ctx_title(c, "Series");
        ui_empty(b, "📺", "This series isn't in the library", "It may have been moved or renamed.");
        return;
    }
    ctx_title(c, "%s", s->title);
    buf_puts(b, "<a class=\"media-back\" href=\"/media/series\">");
    ui_icon(b, "left");
    buf_puts(b, "Series</a><div class=\"media-sdetail\"><div class=\"media-sdetail-art\">");
    poster_art(b, s->title, s->year, "Series", s->poster, 0, false);
    if (can_set_art(c, s->id)) {
        int sq = (int)ctx_qint(c, "s", -1);
        H(b, "<a class=\"media-pic-edit\" href=\"/media/series/%X?pic=1", s->id);
        if (sq >= 0) H(b, "&amp;s=%d", sq);
        buf_puts(b, "\" title=\"Change the picture\">");
        ui_icon(b, "edit");
        buf_puts(b, "<span>Picture</span></a>");
    }
    H(b, "</div><div class=\"media-sdetail-t\"><h1>%s</h1><p class=\"sub\">", s->title);
    if (s->nseasons > 1) H(b, "%d seasons · ", s->nseasons);
    int eps = episodes_in(s);
    H(b, "%d episode%s", eps, eps == 1 ? "" : "s");
    if (s->year) H(b, " · %d", s->year);
    int seen = watched_count(c, s);
    if (seen) H(b, " · %d watched", seen);
    buf_puts(b, "</p>");
    const MediaFile *up = next_up(c, s);
    if (up) {
        MediaWatch *w = watch_of(c, up);
        H(b, "<a class=\"btn primary big\" href=\"/media/watch/%X\">", up->id);
        ui_icon(b, "play");
        H(b, "<span>%s ", w && w->pos > 0 && !w->done ? "Resume" : seen ? "Next up" : "Start watching");
        ep_code(b, up);
        buf_puts(b, "</span></a>");
    }
    buf_puts(b, "</div></div>");

    /* season tabs */
    int season = (int)ctx_qint(c, "s", up ? up->name.season : media_episode(s, 0)->name.season);
    if (s->nseasons > 1) {
        buf_puts(b, "<nav class=\"seg media-seasons\">");
        int last = -1;
        for (int i = 0; i < s->count; i++) {
            const MediaFile *e = media_episode(s, i);
            if (e->name.season == last) continue;
            last = e->name.season;
            H(b, "<a href=\"/media/series/%X?s=%d\"%r>%r %d</a>", s->id, last, last == season ? " class=\"on\"" : "",
              last == 0 ? "Specials" : "Season", last);
        }
        buf_puts(b, "</nav>");
    }
    buf_puts(b, "<ol class=\"media-eps\">");
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        if (s->nseasons > 1 && e->name.season != season) continue;
        MediaWatch *w = watch_of(c, e);
        bool done = w && w->done;
        H(b,
          "<li class=\"media-ep%r%r\"><a class=\"media-ep-main\" href=\"/media/watch/%X\">"
          "<span class=\"media-ep-n%r\">%s</span>",
          done ? " media-ep-done" : "", e == up ? " media-ep-up" : "", e->id,
          e->name.episode_end > e->name.episode ? " media-ep-nn" : "", ep_n(e));
        buf_puts(b, "<span class=\"media-ep-t\"><b>");
        ep_title(b, e);
        buf_puts(b, "</b><small>");
        ep_code(b, e);
        if (e->probe.duration_ms) {
            buf_puts(b, " · ");
            duration(b, e);
        }
        if (e->name.unnumbered && e->name.disc) H(b, " · disc %d", e->name.disc); /* numbered in disc order */
        if (media_playable(e)->probe.playable == PLAY_NO) buf_puts(b, " · <span class=\"media-warn\">needs conversion</span>");
        buf_puts(b, "</small>");
        int p = percent(w);
        if (p && !done) H(b, "<span class=\"media-ep-bar\"><i style=\"width:%d%%\"></i></span>", p);
        buf_puts(b, "</span></a>");
        tick_form(c, b, e, done);
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ol>");

    /* the season's bonus material from ripped discs: "play all" titles and clips (library.c) */
    const MediaIndex *ix = media_lib();
    int nx = 0;
    for (int i = 0; i < ix->nextras; i++) {
        const MediaFile *x = &ix->files[ix->extras[i]];
        if (x->series != s->id || (s->nseasons > 1 && x->name.season != season)) continue;
        if (!nx++) buf_puts(b, "<h2 class=\"mt\">Extras</h2><ol class=\"media-eps media-eps-compact\">");
        H(b, "<li class=\"media-ep\"><a class=\"media-ep-main\" href=\"/media/watch/%X\"><span class=\"media-ep-n\">",
          x->id);
        ui_icon(b, x->role == MF_PLAYALL ? "play" : "film");
        buf_puts(b, "</span><span class=\"media-ep-t\"><b>");
        if (x->role == MF_PLAYALL) buf_puts(b, "All episodes in one");
        else if (x->name.ep_title[0]) html_esc(b, x->name.ep_title);
        else if (x->name.track >= 0) H(b, "Extra · title %d", x->name.track);
        else buf_puts(b, "Extra");
        buf_puts(b, "</b><small>");
        if (x->name.disc) H(b, "Disc %d", x->name.disc);
        if (x->name.disc && x->probe.duration_ms) buf_puts(b, " · ");
        duration(b, x);
        buf_puts(b, "</small></span></a></li>");
    }
    if (nx) buf_puts(b, "</ol>");

    if (str_eq(ctx_q(c, "pic"), "1") && can_set_art(c, s->id)) {
        char close[64];
        int sq = (int)ctx_qint(c, "s", -1);
        if (sq >= 0) snprintf(close, sizeof close, "/media/series/%016llx?s=%d", (unsigned long long)s->id, sq);
        else snprintf(close, sizeof close, "/media/series/%016llx", (unsigned long long)s->id);
        art_modal(c, b, s->id, s->title, s->year, "Series", s->poster, close);
    }
}

/* Folder browser over the index: /media/browse?p=<root>:<dir> */
typedef struct {
    char name[200];
    int videos;
} Sub;

static void page_browse(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "Folders");
    lib_head(c, b, "browse", "Folders", NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    const char *p = ctx_q(c, "p");
    int root = -1;
    char dir[400] = "";
    if (*p) {
        root = (int)strtol(p, NULL, 10);
        const char *colon = strchr(p, ':');
        str_copy(dir, sizeof dir, colon ? colon + 1 : "");
        if (root < 0 || root >= ix->nroots) root = -1;
    }
    if (root < 0 && ix->nroots == 1) root = 0;
    if (root < 0) {
        buf_puts(b, "<ul class=\"media-folders\">");
        for (int r = 0; r < ix->nroots; r++) {
            H(b, "<li><a href=\"/media/browse?p=%d:\">", r);
            ui_icon(b, "folder");
            H(b, "<span>%s</span></a></li>", ix->roots[r]);
        }
        buf_puts(b, "</ul>");
        return;
    }
    /* breadcrumbs */
    H(b, "<nav class=\"media-crumbs\"><a href=\"/media/browse?p=%d:\">%s</a>", root,
      ix->nroots > 1 ? ix->roots[root] : "Library");
    char acc[400] = "";
    for (const char *s = dir; *s;) {
        size_t n = strcspn(s, "/");
        char seg[200];
        str_ncopy(seg, sizeof seg, s, n);
        size_t al = strlen(acc);
        snprintf(acc + al, sizeof acc - al, "%s%s", al ? "/" : "", seg);
        H(b, "<span>/</span><a href=\"/media/browse?p=%d:%u\">%s</a>", root, acc, seg);
        s += n;
        if (*s == '/') s++;
    }
    buf_puts(b, "</nav>");

    size_t dl = strlen(dir);
    Sub *subs = NULL;
    int nsubs = 0, capsubs = 0, nvid = 0;
    buf_puts(b, "<div class=\"media-grid\" id=\"media-files\">");
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        /* a play-everywhere copy is shown as its original; a play-all's header isn't a video */
        if (f->root != root || f->type != MF_VIDEO || f->role == MF_WEBCOPY || media_is_playall_head(f->rel)) continue;
        const char *rest = f->rel;
        if (dl) {
            if (strncmp(f->rel, dir, dl) != 0 || f->rel[dl] != '/') continue;
            rest = f->rel + dl + 1;
        }
        const char *slash = strchr(rest, '/');
        if (!slash) {
            video_card(c, b, f);
            nvid++;
            continue;
        }
        char name[200];
        str_ncopy(name, sizeof name, rest, (size_t)(slash - rest));
        int k = 0;
        while (k < nsubs && !str_eq(subs[k].name, name)) k++;
        if (k == nsubs) {
            if (nsubs == capsubs) subs = xrealloc(subs, (size_t)(capsubs = capsubs ? capsubs * 2 : 16) * sizeof *subs);
            str_copy(subs[nsubs].name, sizeof subs[nsubs].name, name);
            subs[nsubs++].videos = 0;
        }
        subs[k].videos++;
    }
    buf_puts(b, "</div>");
    if (nsubs) {
        buf_puts(b, "<ul class=\"media-folders\">");
        for (int k = 0; k < nsubs; k++) {
            char next[620];
            snprintf(next, sizeof next, "%s%s%s", dir, dl ? "/" : "", subs[k].name);
            H(b, "<li><a href=\"/media/browse?p=%d:%u\">", root, next);
            ui_icon(b, "folder");
            H(b, "<span>%s</span><small>%d video%s</small></a></li>", subs[k].name, subs[k].videos,
              subs[k].videos == 1 ? "" : "s");
        }
        buf_puts(b, "</ul>");
    }
    if (!nsubs && !nvid) ui_empty(b, "📂", "Nothing to play here", NULL);
    free(subs);
}

/* ----------------------------------------------------------------- removing */

/* Admins take a film (or another video) out of the library for good: remove.c deletes its files
   - and its own folder, when it has one. Episodes belong to their series and aren't removed one by one. */
static bool can_remove(const Ctx *c, const MediaFile *f) {
    return is_admin(c) && f->type == MF_VIDEO && f->role == MF_MAIN && f->name.kind != MEDIA_EPISODE;
}

static void size_text(int64_t n, char *out, size_t cap) {
    if (n >= 1000LL * 1048576) snprintf(out, cap, "%.1f GB", (double)n / 1073741824.0);
    else if (n >= 1048576) snprintf(out, cap, "%lld MB", (long long)(n / 1048576));
    else snprintf(out, cap, "%lld KB", (long long)((n + 1023) / 1024));
}

static void remove_modal(Ctx *c, Buf *b, const MediaFile *f, const char *close) {
    (void)c;
    RemovePlan p;
    if (!media_remove_plan(f, &p)) {
        media_remove_free(&p);
        return;
    }
    char size[24];
    size_text(p.bytes, size, sizeof size);
    ui_modal(b, "Remove from the library", close);
    H(b, "<p><b>%s</b>", f->name.title);
    if (f->name.year) H(b, " (%d)", f->name.year);
    buf_puts(b, " is deleted from the disk - for everyone in the household, and it can't be undone.</p>");
    if (p.folder[0])
        H(b, "<p class=\"muted\">Its folder <code class=\"media-code\">%s</code> goes with everything in it: %d file%s, "
             "%s.</p>",
          p.folder, p.files, p.files == 1 ? "" : "s", size);
    else H(b, "<p class=\"muted\">%d file%s, %s:</p>", p.files, p.files == 1 ? "" : "s", size);
    buf_puts(b, "<ul class=\"media-rm-list\">");
    int shown = 0;
    size_t skip = p.folder[0] ? strlen(p.folder) + 1 : 0;
    for (int i = 0; i < p.n && shown < 12; i++) {
        if (p.items[i].dir) continue;
        const char *rel = p.items[i].rel;
        H(b, "<li><code class=\"media-code\">%s</code></li>", strlen(rel) > skip ? rel + skip : rel);
        shown++;
    }
    if (p.files > shown) H(b, "<li class=\"muted\">and %d more</li>", p.files - shown);
    buf_puts(b, "</ul>");
    ui_form(b, "media.remove", "inline");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><div class=\"modal-f\"><a class=\"btn\" "
         "href=\"%s\">Keep it</a><button class=\"btn danger\">Delete from the disk</button></div>",
      f->id, close);
    ui_form_end(b);
    ui_modal_end(b);
    media_remove_free(&p);
}

static void page_watch(Ctx *c, Buf *b, const char *idhex) {
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(idhex, &id) ? media_file(id) : NULL;
    if (!f || f->type != MF_VIDEO) {
        ctx_title(c, "Watch");
        ui_empty(b, "🎬", "This video isn't in the library", "It may have been moved, renamed or deleted.");
        return;
    }
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    const MediaFile *prev = media_neighbour(f, -1), *next = media_neighbour(f, +1);
    MediaWatch *w = watch_of(c, f);
    if (s) {
        Buf t = {0};
        buf_printf(&t, "%s ", f->name.title);
        ep_code(&t, f);
        ctx_title(c, "%s", buf_cstr(&t));
        buf_free(&t);
        H(b, "<a class=\"media-back\" href=\"/media/series/%X?s=%d\">", s->id, f->name.season);
        ui_icon(b, "left");
        H(b, "%s</a>", s->title);
    } else {
        ctx_title(c, "%s", f->name.title);
        H(b, "<a class=\"media-back\" href=\"%s\">", f->name.kind == MEDIA_MOVIE ? "/media/movies" : "/media/browse");
        ui_icon(b, "left");
        H(b, "%s</a>", f->name.kind == MEDIA_MOVIE ? "Movies" : "Folders");
    }

    /* the player: data-static keeps live re-renders away from it */
    bool autoplay = str_eq(ctx_q(c, "auto"), "1");
    H(b, "<div class=\"media-player-box\"><video id=\"media-v-%X\" class=\"media-player\" src=\"/media/file/%X\" controls "
         "preload=\"metadata\" playsinline data-static data-id=\"%X\" data-report=\"media.progress\"",
      f->id, f->id, f->id);
    if (w && !w->done && w->pos > 10 && !autoplay) H(b, " data-resume=\"%d\"", w->pos);
    if (next) H(b, " data-next=\"/media/watch/%X?auto=1\"", next->id);
    if (autoplay) buf_puts(b, " autoplay");
    uint64_t poster = f->poster ? f->poster : s ? s->poster : 0;
    if (poster) {
        buf_puts(b, " poster=\"");
        img_src(b, poster);
        buf_putc(b, '"');
    }
    buf_putc(b, '>');
    const MediaIndex *ix = media_lib();
    const char *want = c->me ? pref(c->me->id, "media.sublang", "") : "";
    bool defaulted = false;
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *sub = &ix->files[i];
        if (sub->type != MF_SUB || sub->video != f->id) continue;
        bool def = !defaulted && *want && str_ieq(sub->lang, want);
        defaulted |= def;
        H(b, "<track kind=\"subtitles\" src=\"/media/sub/%X.vtt\" srclang=\"%s\" label=\"%s\"%r>", sub->id,
          sub->lang[0] ? sub->lang : "und", lang_name(sub->lang), def ? " default" : "");
    }
    buf_puts(b, "</video></div>");

    /* about it */
    buf_puts(b, "<div class=\"media-watch-info\"><div class=\"media-watch-t\">");
    if (s) {
        H(b, "<h1>%s</h1><p class=\"sub\">", f->name.ep_title[0] ? f->name.ep_title : f->name.title);
        ep_code(b, f);
        H(b, " · %s", s->title);
    } else {
        H(b, "<h1>%s</h1><p class=\"sub\">", f->name.title);
        if (f->name.year) H(b, "%d", f->name.year);
    }
    if (f->probe.duration_ms) {
        buf_puts(b, " · ");
        duration(b, f);
    }
    Buf q = {0};
    quality(&q, f);
    if (q.len) H(b, " · %s", buf_cstr(&q));
    buf_free(&q);
    buf_puts(b, "</p></div><div class=\"ph-a\">");
    if (prev) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X\" title=\"Previous episode\">", prev->id);
        ui_icon(b, "left");
        buf_puts(b, "<span>Previous</span></a>");
    }
    if (next) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X\" title=\"Next episode\"><span>Next</span>", next->id);
        ui_icon(b, "right");
        buf_puts(b, "</a>");
    }
    if (c->me) {
        bool done = w && w->done;
        ui_form(b, "media.watched", "inline");
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"done\" value=\"%d\">", f->id,
          done ? 0 : 1);
        H(b, "<button class=\"btn%r\">", done ? " media-watched" : "");
        ui_icon(b, "check");
        H(b, "<span>%s</span></button>", done ? "Watched" : "Mark watched");
        ui_form_end(b);
    }
    if (can_set_art(c, f->id)) { /* films and other videos; episodes use their series' picture */
        H(b, "<a class=\"btn\" href=\"/media/watch/%X?pic=1\" title=\"Change the picture\">", f->id);
        ui_icon(b, "edit");
        buf_puts(b, "<span>Picture</span></a>");
    }
    if (can_remove(c, f)) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X?remove=1\" title=\"Delete it from the library\">", f->id);
        ui_icon(b, "trash");
        buf_puts(b, "<span>Remove</span></a>");
    }
    buf_puts(b, "</div></div>");

    static const char *const cls[] = {"", "ok", "warn", "bad"};
    const MediaFile *pf = media_playable(f); /* the play-everywhere copy, when there is one */
    Playable pl = pf->probe.playable;
    /* an HEVC copy is as far as Hemma goes (no HEVC decoder): say where it plays, not just "may not" */
    bool hevc = pl == PLAY_MAYBE && str_eq(pf->probe.vcodec, "hevc") && !str_eq(pf->probe.container, "mkv");
    H(b, "<div class=\"media-playable\"><span class=\"badge %s\">%s</span>", cls[CLAMP((int)pl, 0, 3)],
      hevc ? "Plays on newer devices" : media_playable_label(pl));
    if (hevc)
        H(b, "<span class=\"muted\">%sHEVC video: Safari on iPhone, iPad and Mac, and Chrome or Edge on most computers "
             "and phones from the last few years</span>",
          pf != f ? "Converted copy · " : "");
    else if (pf != f) buf_puts(b, "<span class=\"muted\">Converted copy</span>");
    else if (pl != PLAY_YES && pf->probe.why[0]) H(b, "<span class=\"muted\">%s</span>", pf->probe.why);
    if (pf->probe.container[0] || pf->probe.vcodec[0]) {
        H(b, "<span class=\"media-tech\">%s", pf->probe.container);
        if (pf->probe.vcodec[0]) H(b, " · %s", pf->probe.vcodec);
        if (pf->probe.acodec[0]) H(b, " · %s", pf->probe.acodec);
        if (pf->probe.audio_channels > 2) H(b, " %d.1", pf->probe.audio_channels - 1);
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</div>");
    ConvertNeed need = convert_need(f);
    if (need != CONV_NONE) { /* Hemma makes a copy that plays everywhere, once */
        buf_puts(b, "<div class=\"media-convert\">");
        if (convert_queued(f->id)) buf_puts(b, "<p class=\"muted\">Being made playable everywhere - see the library settings.</p>");
        else if (is_admin(c)) {
            bool hevc = str_eq(f->probe.vcodec, "hevc");
            H(b, "<p class=\"muted\">%s</p>",
              hevc ? "Safari (iPhone, iPad, Mac) can't open MKV files: a new container fixes that - quick, the picture isn't "
                     "re-encoded. HEVC still needs a device that can decode it (most from the last few years)."
              : need == CONV_REMUX ? "A new container makes this play on iPhones and every TV too - quick, nothing is re-encoded."
                                   : "Hemma can convert this once into a copy that plays on every browser, iPhone and TV.");
            char hex[17];
            media_id_hex(f->id, hex);
            ui_act(b, "media.convert", "btn", hevc ? "Make it play in Safari too" : "Make it play everywhere", "id", hex, NULL);
        } else buf_puts(b, "<p class=\"muted\">An admin can make this play everywhere (Watch → settings).</p>");
        buf_puts(b, "</div>");
    } else if (pl == PLAY_NO && pf == f) {
        buf_puts(b, "<p class=\"muted media-convert\">This format can't be converted by Hemma yet.</p>");
    }

    /* the rest of the season, for binge-friendly navigation */
    if (s && s->count > 1) {
        buf_puts(b, "<h2 class=\"mt\">More episodes</h2><ol class=\"media-eps media-eps-compact\">");
        for (int i = 0; i < s->count; i++) {
            const MediaFile *e = media_episode(s, i);
            if (e->name.season != f->name.season) continue;
            MediaWatch *ew = watch_of(c, e);
            H(b,
              "<li class=\"media-ep%r%r\"><a class=\"media-ep-main\" href=\"/media/watch/%X\">"
              "<span class=\"media-ep-n%r\">%s</span>",
              ew && ew->done ? " media-ep-done" : "", e == f ? " media-ep-up" : "", e->id,
              e->name.episode_end > e->name.episode ? " media-ep-nn" : "", ep_n(e));
            buf_puts(b, "<span class=\"media-ep-t\"><b>");
            ep_title(b, e);
            buf_puts(b, "</b><small>");
            ep_code(b, e);
            if (e->probe.duration_ms) {
                buf_puts(b, " · ");
                duration(b, e);
            }
            buf_puts(b, "</small></span></a></li>");
        }
        buf_puts(b, "</ol>");
    }

    if (str_eq(ctx_q(c, "pic"), "1") && can_set_art(c, f->id)) {
        char close[64];
        snprintf(close, sizeof close, "/media/watch/%016llx", (unsigned long long)f->id);
        art_modal(c, b, f->id, f->name.title, f->name.year, f->name.kind == MEDIA_OTHER ? "Video" : NULL, f->poster,
                  close);
    }
    if (str_eq(ctx_q(c, "remove"), "1") && can_remove(c, f)) {
        char close[64];
        snprintf(close, sizeof close, "/media/watch/%016llx", (unsigned long long)f->id);
        remove_modal(c, b, f, close);
    }
}

/* Who may rescan the library: admins, and the members an admin lets (setting "media.rescanners",
   their ids comma-separated - a setting only the admin-only media.rescanners writes, not a pref
   people set for themselves). Display accounts never. An id counts only while it is a member's
   who can sign in: one who left (ids are never handed out again) or changed role drops out. */
#define RESCANNERS "media.rescanners"

static bool can_sign_in_member(const Person *p) { return p && p->role == ROLE_MEMBER && p->username[0]; }

static bool rescan_granted(int person) {
    if (!can_sign_in_member(person_get(person))) return false;
    for (const char *s = setting(RESCANNERS, ""); *s;) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s) end++; /* a separator */
        else if (v == person) return true;
        s = end;
    }
    return false;
}

static bool can_rescan(const Ctx *c) { return is_admin(c) || (c->me && rescan_granted(c->me->id)); }

static void rescanners_form(Buf *b) { /* admins: who else may rescan */
    int members = 0;
    for (int i = 0; i < person_count(); i++) members += can_sign_in_member(person_at(i));
    if (!members) return; /* nobody but admins to choose from */
    ui_form(b, "media.rescanners", "stack");
    buf_puts(b, "<p class=\"muted media-small\">Admins can always rescan. Who else may:</p>");
    for (int i = 0; i < person_count(); i++) {
        const Person *p = person_at(i);
        if (!can_sign_in_member(p)) continue;
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"p\" value=\"%d\" data-auto%r> %s</label>", p->id,
          rescan_granted(p->id) ? " checked" : "", p->name);
    }
    ui_form_end(b);
}

static void page_settings(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "Library settings");
    buf_puts(b, "<a class=\"media-back\" href=\"/media\">");
    ui_icon(b, "left");
    buf_puts(b, "Watch</a>");
    ui_head(b, "Library", "Where your movies and series live");
    ui_head_end(b);
    buf_puts(b, "<div class=\"grid2\">");

    ui_card(b, "Folders", NULL, NULL);
    if (is_admin(c)) {
        ui_form(b, "media.roots", "stack");
        buf_puts(b, "<label class=\"field\"><span>One folder per line</span><textarea name=\"roots\" rows=\"4\" "
                    "spellcheck=\"false\" placeholder=\"/mnt/media&#10;D:\\Movies\">");
        for (int r = 0; r < ix->nroots; r++) H(b, "%s%s", r ? "\n" : "", ix->roots[r]);
        buf_puts(b, "</textarea></label><p class=\"muted media-small\">On the Raspberry Pi that's where the drive is "
                    "mounted, e.g. <code class=\"media-code\">/mnt/media</code>. A network share works too once it's "
                    "mounted.</p><div class=\"modal-f\"><button class=\"btn primary\">Save &amp; scan</button></div>");
        ui_form_end(b);
    } else {
        buf_puts(b, "<ul class=\"rows\">");
        for (int r = 0; r < ix->nroots; r++) H(b, "<li class=\"row\"><code class=\"media-code\">%s</code></li>", ix->roots[r]);
        if (!ix->nroots) buf_puts(b, "<li class=\"row muted\">No folders yet.</li>");
        buf_puts(b, "</ul><p class=\"muted media-small\">An admin chooses which folders are in the library.</p>");
    }
    ui_card_end(b);

    if (is_admin(c) && ix->nroots) {
        ui_card(b, "Add videos", "/media/upload", "Upload");
        buf_puts(b, "<p class=\"muted\">Send movies and series from a laptop or PC to the library - whole folders "
                    "at a time, picking up where it left off if the connection drops.</p>");
        upload_summary(c, b);
        ui_card_end(b);
    }
    convert_card(c, b);

    ui_card(b, "Status", NULL, NULL);
    buf_puts(b, "<dl class=\"media-stats\">");
    H(b, "<div><dt>Movies</dt><dd>%d</dd></div><div><dt>Series</dt><dd>%d</dd></div>"
         "<div><dt>Episodes</dt><dd>%d</dd></div><div><dt>Other videos</dt><dd>%d</dd></div>",
      ix->nmovies, ix->nseries, ix->nepisodes, ix->nothers);
    int playable = 0;
    for (int i = 0; i < ix->nrecent; i++) playable += media_playable(&ix->files[ix->recent[i]])->probe.playable == PLAY_YES;
    H(b, "<div><dt>Play everywhere</dt><dd>%d of %d</dd></div>", playable, ix->nrecent);
    buf_puts(b, "</dl><p class=\"muted\">");
    if (media_scanning()) H(b, "Scanning now — %d files looked at so far.", media_scan_seen());
    else if (ix->scanned_at) {
        buf_puts(b, "Last scan ");
        ui_ago(b, ix->scanned_at, c->now);
        H(b, ", took %d ms (%d new or changed files read). Hemma rescans every 30 minutes.", ix->scan_ms, ix->probed);
    } else buf_puts(b, "Showing the library as it was last time; a fresh scan starts on its own.");
    buf_puts(b, "</p>");
    if (can_rescan(c)) ui_act(b, "media.rescan", "btn", "Rescan now", NULL, NULL, NULL);
    if (is_admin(c)) rescanners_form(b);
    ui_card_end(b);

    if (c->me) {
        ui_card(b, "Playback", NULL, NULL);
        const char *cur = pref(c->me->id, "media.sublang", "");
        ui_form(b, "media.prefs", "stack");
        buf_puts(b, "<label class=\"field\"><span>Subtitles on by default</span><select name=\"sublang\" data-auto>");
        static const char *const langs[] = {"", "sv", "en", "no", "da", "fi", "de", "fr", "es"};
        for (int i = 0; i < countof(langs); i++)
            H(b, "<option value=\"%s\"%r>%s</option>", langs[i], str_eq(cur, langs[i]) ? " selected" : "",
              i == 0 ? "Off" : lang_name(langs[i]));
        H(b, "</select></label><p class=\"muted media-small\">Just for %s. Subtitle files next to a video "
             "(<code class=\"media-code\">Movie.sv.srt</code>) are picked up automatically.</p>",
          c->me->name);
        buf_puts(b, "<noscript><button class=\"btn\">Save</button></noscript>");
        ui_form_end(b);
        ui_card_end(b);
    }

    ui_card(b, "What plays where", NULL, NULL);
    buf_puts(b, "<ul class=\"media-tips\"><li><span class=\"badge ok\">Plays in browsers</span> MP4 or WebM with H.264/VP9 "
                "video and AAC/MP3/Opus sound.</li><li><span class=\"badge warn\">May not play everywhere</span> MKV, "
                "HEVC, or Dolby (AC-3) sound — fine in Chrome/Edge or Safari, not all of them. An HEVC film converted here "
                "(<i>Plays on newer devices</i>) plays in Safari and on most devices from the last few years.</li>"
                "<li><span class=\"badge bad\">Needs conversion</span> MPEG-2 (DVD rips), AVI/XviD, DTS sound. Hemma "
                "converts MKV files with MPEG-2 or H.264 video and Dolby or AAC sound itself (<i>Play everywhere</i> "
                "above); the copies play on every browser, iPhone and TV.</li></ul>");
    ui_card_end(b);
    buf_puts(b, "</div>");
}

static void page(Ctx *c, Buf *b) {
    const char *p = c->path + 6; /* after "/media" */
    if (!*p || str_eq(p, "/")) page_home(c, b);
    else if (str_eq(p, "/movies")) page_movies(c, b);
    else if (str_eq(p, "/series")) page_series_list(c, b);
    else if (str_starts(p, "/series/")) page_series(c, b, p + 8);
    else if (str_eq(p, "/browse")) page_browse(c, b);
    else if (str_starts(p, "/watch/")) page_watch(c, b, p + 7);
    else if (str_eq(p, "/settings")) page_settings(c, b);
    else if (str_eq(p, "/upload")) upload_page(c, b);
    else {
        ctx_title(c, "Watch");
        ui_empty(b, "🧭", "Nothing here", "That page doesn't exist.");
    }
}

/* --------------------------------------------------------------- widgets */

static void mini(Ctx *c, Buf *b, const MediaFile *f) {
    MediaWatch *w = watch_of(c, f);
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    H(b, "<a class=\"media-mini\" href=\"/media/watch/%X\">", f->id);
    /* images need a signed-in viewer; the public wall display gets drawn posters */
    uint64_t img = c->me ? (f->poster ? f->poster : s ? s->poster : 0) : 0;
    poster_art(b, f->name.title, s ? 0 : f->name.year, NULL, img, percent(w), w && w->done);
    H(b, "<span class=\"media-mini-cap\">%s<small>", f->name.title);
    if (s) ep_code(b, f);
    else if (f->name.year) H(b, "%d", f->name.year);
    buf_puts(b, "</small></span></a>");
}

/* A "continue watching" card: the poster, and an x that drops this person's progress in it (the
   video then starts from the beginning, and leaves the list). Everyone else's progress stays. */
static void going_card(Ctx *c, Buf *b, const MediaFile *f, bool small) {
    if (!c->me || c->me->role == ROLE_DISPLAY) { /* a wall display only looks */
        if (small) mini(c, b, f);
        else video_card(c, b, f);
        return;
    }
    H(b, "<div class=\"media-going\" id=\"media-going-%X\">", f->id);
    if (small) mini(c, b, f);
    else video_card(c, b, f);
    ui_form(b, "media.forget", "media-going-x");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><button title=\"Remove from Continue watching\" "
         "aria-label=\"Remove %s from Continue watching\">",
      f->id, f->name.title);
    ui_icon(b, "x");
    buf_puts(b, "</button>");
    ui_form_end(b);
    buf_puts(b, "</div>");
}

static void widget(Ctx *c, Buf *b, int where) {
    const MediaIndex *ix = media_lib();
    if (where == WIDGET_HOME && ix->nroots == 0) {
        ui_card(b, "Watch", "/media/settings", "Set up");
        buf_puts(b, "<p class=\"muted\">Point Hemma at the drive with your movies and series, and everyone can watch "
                    "them on any screen at home — offline.</p>");
        ui_card_end(b);
        return;
    }
    if (!ix->nrecent) return;
    const MediaFile *list[4];
    int n = where == WIDGET_HOME ? in_progress(c, list, 4) : 0;
    bool resuming = n > 0;
    for (int i = 0; !resuming && i < ix->nrecent && n < 4; i++) list[n++] = &ix->files[ix->recent[i]];
    ui_card(b, resuming ? "Continue watching" : "Recently added", "/media", "Watch");
    buf_puts(b, "<div class=\"media-minis\">");
    for (int i = 0; i < n; i++) {
        if (resuming) going_card(c, b, list[i], true);
        else mini(c, b, list[i]);
    }
    buf_puts(b, "</div>");
    ui_card_end(b);
}

/* --------------------------------------------------------------- actions */

static void act_search(Ctx *c, Fields *f) { ctx_set_live(c, "media.q", fields_str(f, "q")); }

static void act_progress(Ctx *c, Fields *f) {
    uint64_t id = 0;
    if (!c->me || !media_parse_id(fields_str(f, "id"), &id) || !media_file(id)) return;
    bool ended = fields_str(f, "ended")[0] == '1';
    if (media_watch_report(c->me->id, id, (int)fields_int(f, "pos", 0), (int)fields_int(f, "dur", 0), ended, c->now))
        app_changed(); /* finished: other screens' "continue watching" changes */
}

static void act_watched(Ctx *c, Fields *f) {
    uint64_t id = 0;
    const MediaFile *mf = media_parse_id(fields_str(f, "id"), &id) ? media_file(id) : NULL;
    if (!mf) {
        ctx_fail(c, "That video isn't in the library any more");
        return;
    }
    bool done = fields_int(f, "done", 1) != 0;
    media_watch_set_done(c->me->id, id, done, c->now);
    ctx_ok(c, done ? "Marked as watched" : "Marked as not watched");
}

static void act_rescan(Ctx *c, Fields *f) {
    if (!can_rescan(c)) {
        ctx_fail(c, "Rescanning is for admins - ask one to rescan, or to let you do it");
        return;
    }
    if (app_testing()) {
        media_scan_sync();
        ctx_ok(c, "Library scanned");
        return;
    }
    if (media_scanning()) {
        ctx_info(c, "Already scanning — hang on");
        return;
    }
    media_scan_start();
    ctx_info(c, "Scanning your library…");
}

static void act_art_remove(Ctx *c, Fields *f) {
    uint64_t id = 0;
    char dir[1024], stem[400];
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can change pictures");
        return;
    }
    if (!media_parse_id(fields_str(f, "id"), &id) || !media_art_folder(id, dir, sizeof dir, stem, sizeof stem)) {
        ctx_fail(c, "That isn't in the library any more");
        return;
    }
    int n = media_art_prune(dir, stem, NULL);
    if (app_testing()) media_scan_sync();
    else media_scan_start();
    if (n) ctx_ok(c, "Picture removed");
    else ctx_info(c, "There was no picture to remove");
}

static void act_remove(Ctx *c, Fields *fl) {
    uint64_t id = 0;
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can remove things from the library");
        return;
    }
    const MediaFile *f = media_parse_id(fields_str(fl, "id"), &id) ? media_file(id) : NULL;
    if (!f) {
        ctx_fail(c, "That isn't in the library any more");
        return;
    }
    if (convert_queued(f->id)) {
        ctx_fail(c, "It's being converted right now - stop that first (Library settings)");
        return;
    }
    RemovePlan p = {0};
    if (!can_remove(c, f) || !media_remove_plan(f, &p)) {
        media_remove_free(&p);
        ctx_fail(c, "Only films and other videos can be removed here");
        return;
    }
    char title[160], hex[17], size[24], video[1024] = "";
    snprintf(title, sizeof title, f->name.year ? "%s (%d)" : "%s", f->name.title, f->name.year);
    media_id_hex(f->id, hex);
    media_full_path(f, video, sizeof video);
    bool movie = f->name.kind == MEDIA_MOVIE;
    size_text(p.bytes, size, sizeof size);
    int failed = 0;
    media_remove_run(&p, &failed);
    media_remove_free(&p);
    if (failed && path_stat(video, NULL, NULL, NULL)) { /* the video itself is still there (playing somewhere?) */
        ctx_fail(c, "Couldn't delete it - is it playing somewhere? Try again in a moment.");
    } else {
        for (int i = tbl_count(&T_media_watch) - 1; i >= 0; i--) { /* everyone's progress in it */
            const MediaWatch *w = tbl_at(&T_media_watch, i);
            if (str_eq(w->item, hex)) tbl_del(&T_media_watch, w->id);
        }
        activity_add(c->me->id, "🗑️", movie ? "/media/movies" : "/media/browse", "removed %s from the library", title);
        if (failed) ctx_info(c, "Removed %s - %d leftover file%s couldn't be deleted", title, failed, failed == 1 ? "" : "s");
        else ctx_ok(c, "Removed %s - %s freed", title, size);
        ctx_redirect(c, "%s", movie ? "/media/movies" : "/media/browse");
    }
    g_disks_at = 0; /* the sidebar's disk space: look again */
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

/* Videos with two versions (an original and its converted copy): the one that plays in fewer
   places is deleted (remove.c media_lesser_version) - usually the original, whose progress moves
   to the copy. */
static void act_drop_originals(Ctx *c, Fields *fl) {
    (void)fl;
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can delete originals");
        return;
    }
    const MediaIndex *ix = media_lib();
    int done = 0, failed = 0;
    int64_t bytes = 0;
    for (int i = 0; i < ix->nfiles; i++) { /* (deleting doesn't change the index until the rescan) */
        const MediaFile *better = NULL, *lesser = media_lesser_version(&ix->files[i], &better);
        if (!lesser || convert_queued(ix->files[i].id)) continue;
        if (media_drop_version(lesser, better)) done++, bytes += lesser->size;
        else failed++;
    }
    char size[24];
    size_text(bytes, size, sizeof size);
    if (done) {
        log_info("media: %s kept only the most compatible versions: %d files deleted (%s)", c->me->name, done, size);
        if (failed) ctx_info(c, "Deleted %d files (%s) - %d couldn't be deleted", done, size, failed);
        else ctx_ok(c, "Deleted %d file%s - %s freed", done, done == 1 ? "" : "s", size);
    } else if (failed) {
        ctx_fail(c, "Couldn't delete them (in use, or no permission?)");
    } else {
        ctx_info(c, "Nothing to delete: no video has two versions");
    }
    g_disks_at = 0; /* the sidebar's disk space: look again */
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

/* "Remove from Continue watching": this person's progress in the video goes (finished ones aren't listed). */
static void act_forget(Ctx *c, Fields *f) {
    uint64_t id = 0;
    if (!media_parse_id(fields_str(f, "id"), &id)) {
        ctx_fail(c, "That isn't in the library any more");
        return;
    }
    const MediaWatch *w = media_watch_get(c->me->id, id);
    if (w && !w->done) tbl_del(&T_media_watch, w->id);
    const MediaFile *v = media_file(id);
    char what[200] = "it";
    if (v && v->name.kind == MEDIA_EPISODE)
        snprintf(what, sizeof what, "%s S%02dE%02d", v->name.title, v->name.season, v->name.episode);
    else if (v) snprintf(what, sizeof what, "%s", v->name.title);
    ctx_ok(c, "Removed %s from Continue watching", what);
}

static void act_drop_auto(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can change that");
        return;
    }
    bool on = str_eq(fields_str(f, "on"), "1");
    setting_set_int("media.drop_originals", on);
    ctx_ok(c, on ? "After each conversion only the most compatible version stays" : "Both versions stay after converting");
}

/* media.rescanners: p=<id> for each member who may rescan too (the whole list, every time). */
static void act_rescanners(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can choose who may rescan");
        return;
    }
    int ids[FIELDS_MAX], n = 0;
    for (int i = 0; i < f->n; i++) {
        long long id = str_eq(f->k[i], "p") ? str_int_or(f->v[i], 0) : 0;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= ids[k] == id;
        if (!dup && id > 0 && id < INT32_MAX && can_sign_in_member(person_get((int)id))) ids[n++] = (int)id;
    }
    Buf list = {0};
    for (int k = 0; k < n; k++) buf_printf(&list, "%s%d", k ? "," : "", ids[k]);
    setting_set(RESCANNERS, buf_cstr(&list));
    buf_free(&list);
    if (n) ctx_ok(c, "%d member%s may rescan the library too", n, n == 1 ? "" : "s");
    else ctx_ok(c, "Only admins may rescan the library");
}

static void act_roots(Ctx *c, Fields *f) {
    if (!is_admin(c)) { /* server-side folders: admins only */
        ctx_fail(c, "Only an admin can change the library folders");
        return;
    }
    const char *s = fields_str(f, "roots");
    Buf keep = {0};
    int n = 0;
    while (*s) {
        size_t len = strcspn(s, "\n");
        char line[300];
        str_ncopy(line, sizeof line, s, len);
        str_trim(line);
        s += len;
        if (*s == '\n') s++;
        if (!line[0]) continue;
        for (char *p = line; *p; p++)
            if (*p == '\\') *p = '/';
        if (!path_is_dir(line)) {
            ctx_fail(c, "Can't find the folder \"%s\" on the server", line);
            buf_free(&keep);
            return;
        }
        if (++n > MEDIA_MAX_ROOTS) {
            ctx_fail(c, "That's more than %d folders — point at a parent folder instead", MEDIA_MAX_ROOTS);
            buf_free(&keep);
            return;
        }
        if (keep.len) buf_putc(&keep, '\n');
        buf_puts(&keep, line);
    }
    setting_set("media.roots", keep.len ? buf_cstr(&keep) : "");
    g_disks_at = 0; /* the sidebar's disk space: look again */
    media_health_reset(); /* and the System page's checks */
    buf_free(&keep);
    if (app_testing()) media_scan_sync();
    else media_scan_start();
    if (n) ctx_ok(c, "Saved — scanning %d folder%s", n, n == 1 ? "" : "s");
    else ctx_ok(c, "Saved — no folders");
}

static void act_prefs(Ctx *c, Fields *f) {
    char lang[16];
    str_copy(lang, sizeof lang, fields_str(f, "sublang"));
    pref_set(c->me->id, "media.sublang", lang);
    ctx_ok(c, *lang ? "Subtitles: %s" : "Subtitles off by default", lang_name(lang));
}

static const Action actions[] = {
    {"media.search", act_search, true}, {"media.progress", act_progress, true}, {"media.watched", act_watched},
    {"media.rescan", act_rescan}, {"media.roots", act_roots},       {"media.prefs", act_prefs},
    {"media.upload_stop", upload_act_stop}, {"media.art_remove", act_art_remove},
    {"media.convert", convert_act_start},   {"media.convert_stop", convert_act_stop}, {"media.remove", act_remove}, {"media.forget", act_forget},
    {"media.drop_originals", act_drop_originals}, {"media.drop_auto", act_drop_auto},
    {"media.rescanners", act_rescanners},
    {0},
};

/* ------------------------------------------------------------- lifecycle */

static void init(void) {
    T_media_watch.api_owner = "person"; /* /api: watch history is for its person (and admins) */
    store_register(&T_media_watch);
}

static void start(void) {
    media_stream_resume();
    upload_start();
    media_lib_start();
    media_health_reset();
}

static void stop(void) {
    media_stream_stop();
    upload_stop();
    convert_stop();
    media_lib_stop();
}

/* ------------------------------------------------- disk space (sidebar) */

/* Used and free space of each disk that holds a library folder, for the admins' sidebar:
   checked every 30 s on the tick (and when the folders change), so drawing the sidebar on
   every page never touches a disk. Two folders on one disk give one bar. */
#define DISKS_MAX 4
static struct {
    char name[64];
    int64_t free, total;
} g_disks[DISKS_MAX];
static int g_ndisks;

static void disks_refresh(int64_t now) {
    g_disks_at = now ? now : 1;
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS), n = 0;
    for (int i = 0; i < nroots && n < DISKS_MAX; i++) {
        int64_t fr = 0, tot = 0;
        if (!path_disk_space(roots[i], &fr, &tot) || tot <= 0) continue;
        bool same = false;
        for (int k = 0; k < n && !same; k++) same = g_disks[k].total == tot && llabs(g_disks[k].free - fr) < (64LL << 20);
        if (same) continue;
        const char *slash = strrchr(roots[i], '/');
        str_copy(g_disks[n].name, sizeof g_disks[n].name, slash && slash[1] ? slash + 1 : roots[i]);
        g_disks[n].free = fr;
        g_disks[n].total = tot;
        n++;
    }
    g_ndisks = n;
}

static void gb(int64_t n, char *out, size_t cap) {
    double g = (double)n / 1073741824.0;
    if (g >= 1024) snprintf(out, cap, "%.1f TB", g / 1024);
    else snprintf(out, cap, g >= 10 ? "%.0f GB" : "%.1f GB", g);
}

static void side(Ctx *c, Buf *b) {
    if (!is_admin(c)) return; /* the people who add videos (and would fill the disk) */
    if (!g_disks_at) disks_refresh(now_ms());
    for (int i = 0; i < g_ndisks; i++) {
        int64_t used = g_disks[i].total - g_disks[i].free;
        int pct = (int)CLAMP(used * 100 / g_disks[i].total, 0, 100);
        const char *level = pct >= 95 || g_disks[i].free < (2LL << 30) ? " bad" : pct >= 85 ? " warn" : "";
        char fr[24], us[24], tot[24], width[8];
        gb(g_disks[i].free, fr, sizeof fr);
        gb(used, us, sizeof us);
        gb(g_disks[i].total, tot, sizeof tot);
        snprintf(width, sizeof width, "%d%%", pct);
        H(b, "<a class=\"media-disk%r\" href=\"/media/settings\" title=\"Space on the disk with the library (%s)\">"
             "<span class=\"media-disk-l\"><span>Storage%s%s</span><b>%s free</b></span>"
             "<span class=\"media-disk-bar\"><i style=\"width:%s\"></i></span><small>%s used of %s</small></a>",
          level, g_disks[i].name, g_ndisks > 1 ? " · " : "", g_ndisks > 1 ? g_disks[i].name : "", fr, width, us, tot);
    }
}

static void tick(int64_t now) {
    media_lib_tick(now);
    upload_tick(now);
    convert_tick(now);
    media_health_tick(now);
    if (!g_disks_at || now - g_disks_at >= 30000) disks_refresh(now);
}

static bool http(Ctx *c, Conn *conn, HttpReq *r) { return upload_http(c, conn, r) || media_http(c, conn, r); }

static const Module media = {
    .id = "media",
    .title = "Watch",
    .icon = "play",
    .nav = 4,
    .init = init,
    .start = start,
    .stop = stop,
    .tick = tick,
    .side = side,
    .page = page,
    .actions = actions,
    .http = http,
    .widget = widget,
    .widget_order = 50,
    .health = media_health,
};
MODULE(media)
