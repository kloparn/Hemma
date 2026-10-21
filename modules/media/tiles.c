/* modules/media/tiles.c — Watch on the TV.
 *
 * The rows media offers to screens (modules/screens arranges them into the TV view: Continue
 * watching, Recently added, Series, Movies, one series), drawn as big focusable tiles in the
 * shared vocabulary of web/app.css (.tiles .tile .tile-art .tile-t .tile-s .tile-bar), and the
 * TV-mode pages a tile opens (?tv=1): the player full screen, a series with its next episode
 * first - bare, always dark, big type, with a Back link to the screen they came from.
 *
 * A tile on a screen links to the TV-mode page with ?back=<the screen's path>; the same row on
 * an ordinary page (kind 0: Watch's own home) links to the ordinary pages. Progress is the
 * viewer's (ctx_viewer): on the living-room TV, whoever was picked under "Who's watching?". */
#include "../../core/ui.h"
#include "media.h"

/* ---------------------------------------------------------------- TV mode */

bool media_tv_mode(const Ctx *c) { return str_eq(ctx_q(c, "tv"), "1"); }

/* The same rules as signing in's ?next= (core auth_safe_next): only this site's own pages. */
void media_safe_back(const char *in, char *out, size_t cap) {
    size_t n = in ? strlen(in) : 0;
    bool ok = n > 0 && n < 400 && n < cap && in[0] == '/' && !strstr(in, "//");
    for (size_t i = 0; ok && i < n; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch <= 0x20 || ch == 0x7F || ch == '\\') ok = false;
    }
    str_copy(out, cap, ok ? in : "/");
}

void media_tv_back(const Ctx *c, char *out, size_t cap) { media_safe_back(ctx_q(c, "back"), out, cap); }

/* (a TV's "/" is its screen: Back goes to "/", the address its browser has - so wire.js goes back in
   the history instead of adding to it, issue #98) */
void media_row_back(const Ctx *c, char *out, size_t cap) { media_safe_back(c->display_home ? "/" : c->path, out, cap); }

void media_tv_open(Ctx *c, Buf *b, const char *back, const char *cls) {
    char safe[400];
    if (!back) {
        media_tv_back(c, safe, sizeof safe);
        back = safe;
    }
    c->bare = true;
    /* rows of tiles (a series) move with the remote's arrows; a player keeps them for the video */
    bool rows = cls && str_eq(cls, "media-tv-series");
    H(b, "<div class=\"media-tv%r%r\"%r>", cls ? " " : "", cls ? cls : "", rows ? " data-arrows" : "");
    /* first in the tab order; Escape (a computer's keyboard) goes back too, and so does the remote's Back key,
       and the arrows reach it (wire.js [data-back], issue #57) */
    H(b, "<a class=\"media-tv-back\" href=\"%s\" data-esc data-back>", back);
    ui_icon(b, "left");
    H(b, "<span>%s</span></a>", T("Back"));
}

void media_tv_href(Buf *b, const char *path, int kind, const char *back) {
    if (kind) H(b, "%s?tv=1&amp;back=%u", path, back);
    else H(b, "%s", path);
}

static void watch_href(Buf *b, const MediaFile *f, int kind, const char *back) {
    char path[48];
    snprintf(path, sizeof path, "/media/watch/%016llx", (unsigned long long)f->id);
    media_tv_href(b, path, kind, back);
}

static void series_href(Buf *b, const MediaSeries *s, int kind, const char *back) {
    char path[48];
    snprintf(path, sizeof path, "/media/series/%016llx", (unsigned long long)s->id);
    media_tv_href(b, path, kind, back);
}

/* ------------------------------------------------------------------ tiles */

static void tile_bar(Buf *b, int pct) {
    if (pct > 0) H(b, "<span class=\"tile-bar\"><i style=\"width:%d%%\"></i></span>", pct);
}

void media_tile_art(Buf *b, const char *title, const char *label, const char *text, uint64_t img, int pct) {
    H(b, "<span class=\"tile-art%r\"", img ? "" : " media-drawn");
    media_art_style(b, title); /* each title its own colour, the same on every screen */
    buf_putc(b, '>');
    if (img) {
        buf_puts(b, "<img src=\"");
        media_img_src(b, img);
        buf_puts(b, "\" alt=\"\" loading=\"lazy\" decoding=\"async\">");
    } else {
        if (label) H(b, "<span class=\"media-drawn-k\">%s</span>", label);
        H(b, "<span class=\"media-drawn-t\">%s</span>", text ? text : title);
    }
    tile_bar(b, pct);
    buf_puts(b, "</span>");
}

static void duration(Buf *b, int64_t ms) {
    char d[32];
    media_fmt_duration(ms, d, sizeof d);
    html_esc(b, d);
}

/* A film or other video (or one episode) as a poster tile. */
static void video_tile(Ctx *c, Buf *b, const MediaFile *f, int kind, const char *back) {
    const MediaWatch *w = media_watch_of(c, f);
    bool ep = f->name.kind == MEDIA_EPISODE, wall = kind == SCREEN_WALL;
    if (wall) { /* a wall only shows (issue #80: a tap on the hallway tablet started the TV player) */
        buf_puts(b, "<div class=\"tile\">");
    } else {
        buf_puts(b, "<a class=\"tile\" href=\"");
        watch_href(b, f, kind, back);
        buf_puts(b, "\">");
    }
    char title[300];
    media_title(f, title, sizeof title);
    media_tile_art(b, f->name.title, f->name.kind == MEDIA_OTHER ? T("Video") : NULL, title, media_picture(f),
                   w && w->done ? 0 : media_percent(w));
    H(b, "<b class=\"tile-t\">%s</b><span class=\"tile-s\">", title);
    if (ep) {
        media_ep_code(b, f);
        if (f->name.ep_title[0]) H(b, " · %s", f->name.ep_title);
    } else {
        if (f->name.year) H(b, "%d", f->name.year);
        if (f->name.year && f->probe.duration_ms) buf_puts(b, " · ");
        if (f->probe.duration_ms) duration(b, f->probe.duration_ms);
    }
    if (w && w->done) H(b, " · %s", T("Watched"));
    buf_puts(b, wall ? "</span></div>" : "</span></a>");
}

static void series_tile(Buf *b, const MediaSeries *s, int kind, const char *back, int new_eps) {
    bool wall = kind == SCREEN_WALL;
    if (wall) {
        buf_puts(b, "<div class=\"tile\">");
    } else {
        buf_puts(b, "<a class=\"tile\" href=\"");
        series_href(b, s, kind, back);
        buf_puts(b, "\">");
    }
    media_tile_art(b, s->title, T("Series"), NULL, s->poster, 0);
    H(b, "<b class=\"tile-t\">%s</b><span class=\"tile-s\">", s->title);
    if (new_eps) H(b, TN(new_eps, "%d new episode", "%d new episodes"), new_eps);
    else {
        int eps = media_episodes_in(s);
        H(b, TN(eps, "%d episode", "%d episodes"), eps);
        if (s->nseasons > 1) {
            buf_puts(b, " · ");
            H(b, T("%d seasons"), s->nseasons);
        }
    }
    buf_puts(b, wall ? "</span></div>" : "</span></a>");
}

/* An episode as a wide tile: its title, its number and length, the viewer's progress. `up`: the
   one to play next ("Resume" / "Next up"). ided: with an id (a page that lists each episode once). */
static void episode_tile(Ctx *c, Buf *b, const MediaFile *e, const MediaSeries *s, int kind, const char *back, bool up,
                         bool ided) {
    const MediaWatch *w = media_watch_of(c, e);
    bool done = w && w->done, going = w && !done && w->pos > 0;
    H(b, "<a class=\"tile wide%r\"", up ? " media-tile-up" : "");
    if (ided) H(b, " id=\"media-t-%X\"", e->id);
    buf_puts(b, " href=\"");
    watch_href(b, e, kind, back);
    buf_puts(b, "\">");
    Buf code = {0}; /* drawn: the episode's number over the series' colour, not its name sixteen times */
    media_ep_code(&code, e);
    media_tile_art(b, s->title, s->title, buf_cstr(&code), media_picture(e), done ? 0 : media_percent(w));
    buf_free(&code);
    buf_puts(b, "<b class=\"tile-t\">");
    media_ep_title(b, e);
    buf_puts(b, "</b><span class=\"tile-s\">");
    if (up) H(b, "%s · ", going ? T("Resume") : T("Next up"));
    media_ep_code(b, e);
    if (e->probe.duration_ms) {
        buf_puts(b, " · ");
        duration(b, e->probe.duration_ms);
    }
    if (done) H(b, " · %s", T("Watched"));
    buf_puts(b, "</span></a>");
}

/* ------------------------------------------------------------------- rows */

/* Continue watching: the viewer's unfinished videos (nobody picked on the TV yet: no row). */
static bool row_continue(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)arg;
    const MediaFile *going[12];
    int n = media_in_progress(c, going, countof(going));
    if (!n) return false;
    char back[400];
    media_row_back(c, back, sizeof back);
    buf_puts(b, "<div class=\"tiles\">");
    for (int i = 0; i < n; i++) {
        const MediaFile *f = going[i];
        const MediaWatch *w = media_watch_of(c, f);
        buf_puts(b, "<a class=\"tile wide\" href=\"");
        watch_href(b, f, kind, back);
        buf_puts(b, "\">");
        char title[300];
        media_title(f, title, sizeof title);
        media_tile_art(b, f->name.title, NULL, title, media_picture(f), media_percent(w));
        H(b, "<b class=\"tile-t\">%s</b><span class=\"tile-s\">", title);
        if (f->name.kind == MEDIA_EPISODE) {
            media_ep_code(b, f);
            if (f->name.ep_title[0]) H(b, " · %s", f->name.ep_title);
        } else if (w && w->dur > w->pos) {
            Buf left = {0};
            ui_duration(&left, w->dur - w->pos);
            H(b, T("%s left"), buf_cstr(&left));
            buf_free(&left);
        }
        buf_puts(b, "</span></a>");
    }
    buf_puts(b, "</div>");
    return true;
}

/* Recently added: a series' new episodes as one tile ("70's Show · 8 new episodes"). */
static bool row_recent(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)arg;
    MediaRecent r[20];
    int n = media_recent(c, r, countof(r));
    if (!n) return false;
    char back[400];
    media_row_back(c, back, sizeof back);
    buf_puts(b, "<div class=\"tiles\">");
    for (int i = 0; i < n; i++) {
        if (r[i].n > 1) series_tile(b, r[i].series, kind, back, r[i].n);
        else video_tile(c, b, r[i].f, kind, back);
    }
    buf_puts(b, "</div>");
    return true;
}

static bool row_series(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)arg;
    const MediaIndex *ix = media_lib_for(c);
    if (!ix->nseries) return false;
    char back[400];
    media_row_back(c, back, sizeof back);
    buf_puts(b, "<div class=\"tiles\">");
    for (int i = 0; i < ix->nseries && i < 100; i++) series_tile(b, &ix->series[i], kind, back, 0);
    buf_puts(b, "</div>");
    return true;
}

static bool row_movies(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)arg;
    const MediaIndex *ix = media_lib_for(c);
    if (!ix->nmovies) return false;
    char back[400];
    media_row_back(c, back, sizeof back);
    buf_puts(b, "<div class=\"tiles\">");
    for (int i = 0; i < ix->nmovies && i < 100; i++) video_tile(c, b, &ix->files[ix->movies[i]], kind, back);
    buf_puts(b, "</div>");
    return true;
}

#define ROW_EPISODES 30

/* One series (the builder's pick): the viewer's next episode first, the ones after it, then a
   tile for the whole series. */
static bool row_series_one(Ctx *c, Buf *b, int kind, const char *arg) {
    uint64_t id = 0;
    const MediaSeries *s = media_parse_id(arg, &id) ? media_series(id) : NULL;
    if (!s || s->count <= 0 || !media_kids_series_ok(c, s)) return false; /* (a child picked: issue #44) */
    const MediaFile *up = media_next_up(c, s);
    int at = 0;
    for (int i = 0; i < s->count; i++)
        if (media_episode(s, i) == up) at = i;
    char back[400];
    media_row_back(c, back, sizeof back);
    buf_puts(b, "<div class=\"tiles\">");
    for (int i = at; i < s->count && i < at + ROW_EPISODES; i++)
        episode_tile(c, b, media_episode(s, i), s, kind, back, i == at, false);
    buf_puts(b, "<a class=\"tile wide media-tile-all\" href=\"");
    series_href(b, s, kind, back);
    buf_puts(b, "\">");
    media_tile_art(b, s->title, T("Series"), NULL, s->poster, 0);
    int eps = media_episodes_in(s);
    H(b, "<b class=\"tile-t\">%s</b><span class=\"tile-s\">%s · ", T("All episodes"), s->title);
    H(b, TN(eps, "%d episode", "%d episodes"), eps);
    buf_puts(b, "</span></a></div>");
    return true;
}

static int series_choices(Ctx *c, BlockChoice *out, int max) {
    (void)c;
    const MediaIndex *ix = media_lib();
    int n = 0;
    for (int i = 0; i < ix->nseries && n < max; i++, n++) {
        media_id_hex(ix->series[i].id, out[n].arg);
        str_copy(out[n].label, sizeof out[n].label, ix->series[i].title);
    }
    return n;
}

const Block media_blocks[] = {
    {"media.continue", N_("Continue watching"), SCREEN_TV, row_continue, NULL},
    {"media.recent", N_("Recently added"), SCREEN_TV | SCREEN_WALL, row_recent, NULL},
    {"media.series", N_("Series"), SCREEN_TV, row_series, NULL},
    {"media.movies", N_("Movies"), SCREEN_TV, row_movies, NULL},
    {"media.series_one", N_("A series"), SCREEN_TV, row_series_one, series_choices},
    {0},
};

/* --------------------------------------------------------- TV-mode pages */

/* "Watching as Eva" on a display account: whose progress the TV keeps. */
static void watching_as(const Ctx *c, Buf *b) {
    if (!c->me || c->me->role != ROLE_DISPLAY) return;
    const Person *p = person_get(ctx_viewer(c));
    if (!p) return;
    buf_puts(b, "<p class=\"media-tv-who\">");
    ui_avatar_deco(b, p, "sm");
    buf_puts(b, "<span>");
    H(b, T("Watching as %s"), p->name);
    buf_puts(b, "</span></p>");
}

/* The player, full screen: it starts by itself where the browser allows (else OK on the
   player's own big Play button: it has the focus), from where the viewer left off, and goes
   on with the next episode. */
void media_watch_tv(Ctx *c, Buf *b, const MediaFile *f) {
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    const MediaFile *next = s ? media_neighbour(f, +1) : NULL;
    const MediaWatch *w = media_watch_of(c, f);
    if (s) {
        Buf t = {0};
        buf_printf(&t, "%s ", f->name.title);
        media_ep_code(&t, f);
        ctx_title(c, "%s", buf_cstr(&t));
        buf_free(&t);
    }
    char title[300];
    media_title(f, title, sizeof title);
    if (!s) ctx_title(c, "%s", title);
    char back[400];
    media_tv_back(c, back, sizeof back);
    media_tv_open(c, b, back, "media-tv-watch");

    /* data-static="<the video>": live re-renders leave the playing video alone, and the next
       episode (data-next) morphs into the same element - so a full screen stays full screen */
    bool fresh = str_eq(ctx_q(c, "auto"), "1"); /* came on after the previous episode: from the top */
    Buf go = {0};
    if (next) H(&go, "/media/watch/%X?tv=1&amp;auto=1&amp;back=%u", next->id, back);
    char src[100];
    media_file_src(c, f->id, src, sizeof src);
    H(b, "<div class=\"media-tv-player\" id=\"media-box\"><video id=\"media-video\" class=\"media-tv-video\" "
         "src=\"%s\" controls autoplay playsinline preload=\"auto\" tabindex=\"0\" autofocus data-static=\"%X\" "
         "data-id=\"%X\" data-report=\"media.progress\"",
      src, f->id, f->id);
    if (w && !w->done && w->pos > 10 && !fresh) H(b, " data-resume=\"%d\"", w->pos);
    if (next) H(b, " data-next=\"%r\"", buf_cstr(&go));
    uint64_t poster = media_picture(f);
    if (poster) {
        buf_puts(b, " poster=\"");
        media_img_src(b, poster);
        buf_putc(b, '"');
    }
    buf_putc(b, '>');
    media_tracks(c, b, f);
    buf_puts(b, "</video>");
    if (next) media_upnext(b, next, buf_cstr(&go), true);
    media_controls(c, b, f, true);
    buf_puts(b, "</div>");

    buf_puts(b, "<div class=\"media-tv-info\">");
    if (s) {
        H(b, "<h1>%s</h1><p class=\"media-tv-sub\">", f->name.ep_title[0] ? f->name.ep_title : f->name.title);
        media_ep_code(b, f);
        H(b, " · %s", s->title);
    } else {
        H(b, "<h1>%s</h1><p class=\"media-tv-sub\">", title);
        if (f->name.year) H(b, "%d", f->name.year);
        if (f->name.year && f->probe.duration_ms) buf_puts(b, " · ");
    }
    if (f->probe.duration_ms) {
        if (s) buf_puts(b, " · ");
        duration(b, f->probe.duration_ms);
    }
    buf_puts(b, "</p>");
    watching_as(c, b);
    buf_puts(b, "<div class=\"media-tv-acts\">");
    if (next) {
        H(b, "<a class=\"btn big\" href=\"%r\">", buf_cstr(&go));
        H(b, "<span>%s · ", T("Next episode"));
        media_ep_code(b, next);
        buf_puts(b, "</span>");
        ui_icon(b, "right");
        buf_puts(b, "</a>");
    }
    if (s) {
        /* the series' page; opened from it already, back there is the same thing */
        char path[48];
        snprintf(path, sizeof path, "/media/series/%016llx", (unsigned long long)s->id);
        if (str_starts(back, path)) H(b, "<a class=\"btn big\" href=\"%s\">", back);
        else {
            buf_puts(b, "<a class=\"btn big\" href=\"");
            media_tv_href(b, path, SCREEN_TV, back);
            buf_puts(b, "\">");
        }
        ui_icon(b, "list");
        H(b, "<span>%s</span></a>", T("All episodes"));
    }
    buf_puts(b, "</div></div></div>");
    buf_free(&go);
}

/* A series on the TV: the next episode as one big button (it has the focus: OK plays), then
   every season's episodes as tiles. An episode's Back comes here; this page's goes on to the
   screen. */
void media_series_tv(Ctx *c, Buf *b, const MediaSeries *s) {
    ctx_title(c, "%s", s->title);
    char back[400], self[600];
    media_tv_back(c, back, sizeof back);
    Buf me = {0};
    H(&me, "/media/series/%X?tv=1&back=%u", s->id, back); /* (a URL: no HTML escaping in there) */
    str_copy(self, sizeof self, buf_cstr(&me));
    buf_free(&me);
    media_tv_open(c, b, back, "media-tv-series");

    const MediaFile *up = media_next_up(c, s);
    buf_puts(b, "<header class=\"media-tv-head\"><div class=\"media-tv-poster\">");
    media_tile_art(b, s->title, T("Series"), NULL, s->poster, 0);
    H(b, "</div><div class=\"media-tv-head-t\"><h1>%s</h1><p class=\"media-tv-sub\">", s->title);
    if (s->nseasons > 1) {
        H(b, T("%d seasons"), s->nseasons);
        buf_puts(b, " · ");
    }
    int eps = media_episodes_in(s);
    H(b, TN(eps, "%d episode", "%d episodes"), eps);
    if (s->year) H(b, " · %d", s->year);
    buf_puts(b, "</p>");
    watching_as(c, b);
    if (up) {
        const MediaWatch *w = media_watch_of(c, up);
        bool going = w && !w->done && w->pos > 0;
        int seen = 0;
        for (int i = 0; i < s->count; i++) {
            const MediaWatch *ew = media_watch_of(c, media_episode(s, i));
            seen += ew && ew->done;
        }
        buf_puts(b, "<a class=\"btn primary big media-tv-play\" autofocus href=\"");
        watch_href(b, up, SCREEN_TV, self);
        buf_puts(b, "\">");
        ui_icon(b, "play");
        H(b, "<span>%s ", going ? T("Resume") : seen ? T("Next up") : T("Start watching"));
        media_ep_code(b, up);
        buf_puts(b, "</span></a>");
    }
    buf_puts(b, "</div></header>");

    for (int i = 0; i < s->count;) { /* a row per season */
        int season = media_episode(s, i)->name.season;
        if (s->nseasons > 1) {
            buf_puts(b, "<h2 class=\"media-tv-h\">");
            H(b, season == 0 ? T("Specials %d") : T("Season %d"), season);
            buf_puts(b, "</h2>");
        }
        if (up && up->name.season == season) /* opens at the next episode (wire.js data-start) */
            H(b, "<div class=\"tiles\" data-start=\"media-t-%X\">", up->id);
        else buf_puts(b, "<div class=\"tiles\">");
        for (; i < s->count && media_episode(s, i)->name.season == season; i++)
            episode_tile(c, b, media_episode(s, i), s, SCREEN_TV, self, media_episode(s, i) == up, true);
        buf_puts(b, "</div>");
    }
    buf_puts(b, "</div>");
}
