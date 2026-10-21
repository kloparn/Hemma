/* modules/media — the home cinema.
 *
 * Browse the household's movies, series and other videos from a local drive
 * and watch them in any browser on the network. The library is scanned by a
 * background thread (library.c), files stream with Range support from worker
 * threads (stream.c) and every person's progress is remembered, so "continue
 * watching" works across the phone, the laptop and the TV's browser. */
#include "../../core/env.h"
#include "../../core/ui.h"
#include "media.h"
#include "convert.h"
#include "remove.h"
#include "room.h"
#include "upload.h"
#include "health.h"
#include "drives.h"

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
    return code && *code ? code : T("Subtitles");
}

static void keyed_src(const Ctx *c, const char *path, char *out, size_t cap) {
    char k[CTX_KEY_LEN];
    ctx_url_key(c, path, k);
    snprintf(out, cap, "%s%s%s", path, k[0] ? "?k=" : "", k);
}

void media_file_src(const Ctx *c, uint64_t id, char *out, size_t cap) {
    char path[40];
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)id);
    keyed_src(c, path, out, cap);
}

void media_file_src_at(const Ctx *c, uint64_t id, int64_t at, char *out, size_t cap) {
    media_file_src(c, id, out, cap);
    size_t n = strlen(out);
    if (at > 0 && n < cap) snprintf(out + n, cap - n, "#t=%lld", (long long)at);
}

int media_resume_at(const MediaWatch *w) {
    if (!w || w->done || w->pos <= 10 || (w->dur > 0 && w->pos >= w->dur - 5)) return 0;
    return w->pos;
}

void media_sub_src(const Ctx *c, uint64_t id, char *out, size_t cap) {
    char path[40];
    snprintf(path, sizeof path, "/media/sub/%016llx.vtt", (unsigned long long)id);
    keyed_src(c, path, out, cap);
}

/* The video's subtitles as <track>s; the viewer's language (Library settings) is on by default. */
void media_tracks(const Ctx *c, Buf *b, const MediaFile *f) {
    const MediaIndex *ix = media_lib();
    int v = ctx_viewer(c);
    const char *want = v ? pref(v, "media.sublang", "") : "";
    bool defaulted = false;
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *sub = &ix->files[i];
        if (sub->type != MF_SUB || sub->video != f->id) continue;
        bool def = !defaulted && *want && str_ieq(sub->lang, want);
        defaulted |= def;
        char src[100];
        media_sub_src(c, sub->id, src, sizeof src);
        H(b, "<track kind=\"subtitles\" src=\"%s\" srclang=\"%s\" label=\"%s\"%r>", src,
          sub->lang[0] ? sub->lang : "und", lang_name(sub->lang), def ? " default" : "");
    }
}

/* The same tracks as a player's subtitle menu: same order, same one on. */
static int media_subs(const Ctx *c, const MediaFile *f, UiSub *out, int max) {
    const MediaIndex *ix = media_lib();
    int v = ctx_viewer(c);
    const char *want = v ? pref(v, "media.sublang", "") : "";
    bool defaulted = false;
    int n = 0;
    for (int i = 0; i < ix->nfiles && n < max; i++) {
        const MediaFile *sub = &ix->files[i];
        if (sub->type != MF_SUB || sub->video != f->id) continue;
        bool def = !defaulted && *want && str_ieq(sub->lang, want);
        defaulted |= def;
        out[n++] = (UiSub){sub->lang[0] ? sub->lang : "und", lang_name(sub->lang), def};
    }
    return n;
}

void media_controls(const Ctx *c, Buf *b, const MediaFile *f, bool tv) {
    UiSub subs[16];
    char key[17];
    media_id_hex(f->id, key);
    const MediaFile *pf = media_playable(f);
    int64_t ms = f->probe.duration_ms ? f->probe.duration_ms : pf->probe.duration_ms;
    UiPlayer p = {.box = "media-box", .video = "media-video", .key = key, .seconds = (int)(ms / 1000), .subs = subs, .tv = tv};
    if (str_eq(pf->probe.vcodec, "hevc")) /* (an HEVC copy plays where the device decodes HEVC - not every TV's browser) */
        p.trouble = T("This video is HEVC (x265), which this browser can't play. Most phones, computers and iPads can.");
    p.nsubs = media_subs(c, f, subs, countof(subs));
    ui_player(b, &p);
}

static const char *query(const Ctx *c) {
    const char *q = ctx_live(c, "media.q");
    return *q ? q : ctx_q(c, "q");
}

int media_percent(const MediaWatch *w) {
    if (!w || w->dur <= 0 || w->pos <= 0) return 0;
    return CLAMP(w->pos * 100 / w->dur, 1, 100);
}

/* Progress, "continue watching" and watched ticks are the viewer's: the signed-in person, or
   whoever was picked on a display account (the living-room TV) - nobody's until someone is. */
const MediaWatch *media_watch_of(const Ctx *c, const MediaFile *f) {
    int v = ctx_viewer(c);
    return v && f ? media_watch_get(v, f->id) : NULL;
}

/* Episodes in a series, counting both halves of a two-episode file ("S01E01-E02"). */
int media_episodes_in(const MediaSeries *s) {
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

void media_ep_code(Buf *b, const MediaFile *f) {
    H(b, "S%02dE%02d", f->name.season, f->name.episode);
    if (f->name.episode_end > f->name.episode) H(b, "–E%02d", f->name.episode_end);
}

/* Bold line of an episode row: its title, or "Episode 3". */
void media_ep_title(Buf *b, const MediaFile *f) {
    if (f->name.ep_title[0]) html_esc(b, f->name.ep_title);
    else H(b, T("Episode %d"), f->name.episode);
}

static void quality(Buf *b, const MediaFile *f) {
    int h = f->probe.height, w = f->probe.width;
    if (h >= 2000 || w >= 3800) buf_puts(b, "4K");
    else if (h >= 1000 || w >= 1900) buf_puts(b, "1080p");
    else if (h >= 700 || w >= 1270) buf_puts(b, "720p");
    else if (h > 0) buf_puts(b, "SD");
}

/* A codec or container by the name people know it by ("h264" -> "H.264"); others in capitals. */
static void tech_name(Buf *b, const char *id) {
    static const char *const names[][2] = {
        {"h264", "H.264"}, {"hevc", "HEVC (H.265)"}, {"mpeg2", "MPEG-2"}, {"mpeg4", "MPEG-4 (XviD/DivX)"},
        {"ac3", "AC-3 (Dolby Digital)"}, {"eac3", "E-AC-3"}, {"opus", "Opus"}, {"vorbis", "Vorbis"},
        {"truehd", "TrueHD"}, {"pcm", "PCM"}};
    for (int i = 0; i < countof(names); i++)
        if (str_eq(id, names[i][0])) {
            buf_puts(b, names[i][1]);
            return;
        }
    for (const char *p = id; *p; p++) buf_putc(b, (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p));
}

/* "MP4 file · H.264 video · AAC sound (5.1)" - folded under the plain badge (issue #62). Escaped. */
static void tech_fact(Buf *t, const char *fmt, const char *id) { /* fmt: "%s file" in the language in use */
    Buf n = {0};
    tech_name(&n, id);
    buf_puts(t, t->len ? " · " : "");
    buf_printf(t, fmt, buf_cstr(&n));
    buf_free(&n);
}

static void tech_facts(Buf *b, const MediaFile *f) {
    Buf t = {0};
    if (f->probe.container[0]) tech_fact(&t, T("%s file"), f->probe.container);
    if (f->probe.vcodec[0]) tech_fact(&t, T("%s video"), f->probe.vcodec);
    if (f->probe.acodec[0]) {
        tech_fact(&t, T("%s sound"), f->probe.acodec);
        if (f->probe.audio_channels > 2) buf_printf(&t, " (%d.1)", f->probe.audio_channels - 1);
    }
    html_esc(b, buf_cstr(&t));
    buf_free(&t);
}

static void duration(Buf *b, const MediaFile *f) {
    char d[32];
    media_fmt_duration(f->probe.duration_ms, d, sizeof d);
    html_esc(b, d);
}

/* /media/img/<id>?v=<version>: a replaced picture keeps its name (and id), and browsers keep
   images for a day - the version makes them fetch the new one at once. */
void media_img_src(Buf *b, uint64_t img) {
    const MediaFile *f = media_file(img);
    H(b, "/media/img/%X?v=%X", img, f ? (uint64_t)f->mtime * 1000003u + (uint64_t)f->size : (uint64_t)0);
}

uint64_t media_picture(const MediaFile *f) {
    if (!f) return 0;
    if (f->poster) return f->poster;
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    return s ? s->poster : 0;
}

/* The colours of a drawn poster, from its title (each series its own, the same everywhere): the
   hue and two neighbours worked out here, so the CSS needs no calc() inside hsl() - older TV
   browsers drop a whole background over that. */
void media_art_style(Buf *b, const char *title) {
    int h = media_hue(title);
    H(b, " style=\"--h:%d;--h2:%d;--h3:%d\"", h, (h + 40) % 360, (h + 25) % 360);
}

/* The art of a poster: an image when the library has one, otherwise a
 * typographic card in the title's colour (`shown`: the words on it, NULL = the title - a disc
 * extra's in the reader's language keeps its colour from its own title). */
static void poster_art_as(Buf *b, const char *title, const char *shown, int year, const char *label, uint64_t img, int pct,
                          bool done) {
    H(b, "<span class=\"media-art\"");
    media_art_style(b, title);
    buf_putc(b, '>');
    if (img) {
        buf_puts(b, "<img src=\"");
        media_img_src(b, img);
        buf_puts(b, "\" alt=\"\" loading=\"lazy\" decoding=\"async\">");
    } else {
        if (label) H(b, "<span class=\"media-art-k\">%s</span>", label);
        H(b, "<span class=\"media-art-t\">%s</span>", shown ? shown : title);
        if (year) H(b, "<span class=\"media-art-y\">%d</span>", year);
    }
    if (done) {
        H(b, "<span class=\"media-art-done\" title=\"%s\">", T("Watched"));
        ui_icon(b, "check");
        buf_puts(b, "</span>");
    } else if (pct > 0) {
        H(b, "<span class=\"media-art-bar\"><i style=\"width:%d%%\"></i></span>", pct);
    }
    buf_puts(b, "</span>");
}

static void poster_art(Buf *b, const char *title, int year, const char *label, uint64_t img, int pct, bool done) {
    poster_art_as(b, title, NULL, year, label, img, pct, done);
}

/* A video's title as people read it. The library names a ripped disc's "play all" title and bonus clips
   itself, in English ("70's Show – season 1, disc 4: all episodes" - kept so: it's part of what the video
   is, library.c name_extra); here they're said again in the reader's language from their parts. */
const char *media_title(const MediaFile *f, char *out, size_t cap) {
    const MediaSeries *s = f->name.extra && (f->role == MF_PLAYALL || f->role == MF_BONUS) ? media_series(f->series) : NULL;
    if (!s) {
        str_copy(out, cap, f->name.title);
        return out;
    }
    char where[96];
    if (f->name.season && f->name.disc) snprintf(where, sizeof where, T("season %d, disc %d"), f->name.season, f->name.disc);
    else if (f->name.season) snprintf(where, sizeof where, T("season %d"), f->name.season);
    else if (f->name.disc) snprintf(where, sizeof where, T("specials, disc %d"), f->name.disc);
    else str_copy(where, sizeof where, T("specials"));
    if (f->role == MF_PLAYALL) snprintf(out, cap, T("%s – %s: all episodes"), s->title, where);
    else if (f->name.ep_title[0]) snprintf(out, cap, "%s – %s: %s", s->title, where, f->name.ep_title);
    else if (f->name.track >= 0) snprintf(out, cap, T("%s – %s: extra (title %d)"), s->title, where, f->name.track);
    else snprintf(out, cap, T("%s – %s: extra"), s->title, where);
    return out;
}

static void video_card(Ctx *c, Buf *b, const MediaFile *f) {
    const MediaWatch *w = media_watch_of(c, f);
    H(b, "<a class=\"media-poster\" href=\"/media/watch/%X\">", f->id);
    if (f->name.kind == MEDIA_EPISODE) {
        const MediaSeries *s = media_series(f->series);
        poster_art(b, f->name.title, 0, NULL, f->poster ? f->poster : s ? s->poster : 0, media_percent(w), w && w->done);
        H(b, "<span class=\"media-cap\">%s<small>", f->name.title);
        media_ep_code(b, f);
        if (f->name.ep_title[0]) H(b, " · %s", f->name.ep_title);
    } else {
        char title[300];
        media_title(f, title, sizeof title);
        poster_art_as(b, f->name.title, title, f->name.year, f->name.kind == MEDIA_OTHER ? T("Video") : NULL, f->poster,
                      media_percent(w), w && w->done);
        H(b, "<span class=\"media-cap\">%s<small>", title);
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
    ui_modal(b, T("Picture"), close);
    buf_puts(b, "<div class=\"media-pic\"><div class=\"media-pic-art\">");
    poster_art(b, title, year, label, img, 0, false);
    /* the form first, so status lines appearing below never rebuild it (and drop the chosen file) */
    H(b,
      "</div><div class=\"stack\"><form class=\"stack\" id=\"media-pic-f\" data-upload=\"/media/upload\" "
      "data-empty=\"%s\"><input type=\"hidden\" name=\"art\" value=\"%s\">"
      "<label class=\"field\"><span>%s</span><input type=\"file\" name=\"files\" "
      "accept=\"image/jpeg,image/png,image/webp\"></label>"
      "<p class=\"muted media-small\">",
      T("Choose a picture first"), hex, T("A picture from this device"));
    H(b,
      T("JPEG, PNG or WebP - a tall poster (2:3) fits best. It's saved in the library folder as "
        "<code class=\"media-code\">%s.jpg</code> (or .png/.webp), where other players find it too."),
      stem);
    H(b, "</p><div class=\"modal-f\"><button class=\"btn primary\">%s</button></div></form>", T("Use this picture"));
    upload_art_status(c, b, id);
    if (media_art_current(id)) {
        ui_form_confirm(b, "media.art_remove", "inline", T("Remove this picture? Its file is deleted from the library folder."));
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%s\"><button class=\"btn\">%s</button>", hex, T("Remove picture"));
        ui_form_end(b);
    }
    buf_puts(b, "</div></div>");
    ui_modal_end(b);
}

static int watched_count(const Ctx *c, const MediaSeries *s) {
    int n = 0, v = ctx_viewer(c);
    for (int i = 0; v && i < s->count; i++) {
        MediaWatch *w = media_watch_get(v, media_episode(s, i)->id);
        n += w && w->done;
    }
    return n;
}

static void series_card(Ctx *c, Buf *b, const MediaSeries *s) {
    H(b, "<a class=\"media-poster\" href=\"/media/series/%X\">", s->id);
    int seen = watched_count(c, s);
    poster_art(b, s->title, s->year, T("Series"), s->poster, seen ? seen * 100 / s->count : 0, seen == s->count && seen);
    int eps = media_episodes_in(s);
    H(b, "<span class=\"media-cap\">%s<small>", s->title);
    H(b, TN(eps, "%d episode", "%d episodes"), eps);
    if (s->nseasons > 1) {
        buf_puts(b, " · ");
        H(b, T("%d seasons"), s->nseasons);
    }
    buf_puts(b, "</small></span></a>");
}

/* The viewer's unfinished videos, most recently watched first. */
int media_in_progress(const Ctx *c, const MediaFile **out, int max) {
    const MediaWatch *keep[24];
    int n = 0, v = ctx_viewer(c);
    max = MIN(max, countof(keep));
    for (int i = 0; v && max > 0 && i < tbl_count(&T_media_watch); i++) {
        const MediaWatch *w = tbl_at(&T_media_watch, i);
        uint64_t id = 0;
        if (w->person != v || w->done || w->pos < 10 || !media_parse_id(w->item, &id) || !media_file(id) ||
            !media_kids_ok(c, media_file(id))) /* (a child's: not what's marked - issue #44) */
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

/* What to play next in a series for the viewer: the first unfinished episode
 * after the last one they finished (or the one they're in the middle of). */
const MediaFile *media_next_up(const Ctx *c, const MediaSeries *s) {
    int last_done = -1, v = ctx_viewer(c);
    for (int i = 0; v && i < s->count; i++) {
        MediaWatch *w = media_watch_get(v, media_episode(s, i)->id);
        if (w && w->done) last_done = i;
        else if (w && w->pos > 0) return media_episode(s, i);
    }
    return media_episode(s, last_done + 1 < s->count ? last_done + 1 : 0);
}

/* ------------------------------------------------------------------ chrome */

/* One Watch section: the library's tabs, and tv's channels as "Live TV" (tv's pages draw this
   header too, through media.h). */
void media_lib_head(Ctx *c, Buf *b, const char *active, const char *title, const char *sub, const char *buttons) {
    ui_head(b, title, sub);
    if (media_scanning()) {
        int seen = media_scan_seen();
        buf_puts(b, "<span class=\"badge media-scanning\"><span class=\"media-spin\"></span>");
        H(b, TN(seen, "Scanning · %d file", "Scanning · %d files"), seen);
        buf_puts(b, "</span>");
    }
    if (buttons) buf_puts(b, buttons);
    H(b, "<a class=\"icon-btn\" href=\"/media/settings\" title=\"%s\" aria-label=\"%s\">", T("Library settings"),
      T("Library settings"));
    ui_icon(b, "settings");
    buf_puts(b, "</a>");
    ui_head_end(b);
    static const char *const tabs[][3] = {{"home", "/media", N_("Home")},          {"movies", "/media/movies", N_("Movies")},
                                          {"series", "/media/series", N_("Series")}, {"tv", "/tv", N_("Live TV")},
                                          {"browse", "/media/browse", N_("Folders")}};
    buf_puts(b, "<div class=\"toolbar media-tools\"><nav class=\"seg media-tabs\">");
    for (int i = 0; i < countof(tabs); i++) {
        if (str_eq(tabs[i][0], "tv") && !module_find("tv")) continue; /* built without channels */
        bool on = str_eq(active, tabs[i][0]);
        H(b, "<a href=\"%s\"%r>%s</a>", tabs[i][1], on ? " class=\"on\" aria-current=\"page\"" : "", T(tabs[i][2]));
    }
    /* searching is the library's: from Live TV the box simply opens Watch's results */
    H(b,
      "</nav><form class=\"media-search\" method=\"get\" action=\"/media\" role=\"search\">"
      "<input type=\"search\" name=\"q\" value=\"%s\" placeholder=\"%s\" autocomplete=\"off\"%r "
      "data-key=\"/\" aria-label=\"%s\"></form></div>",
      str_eq(active, "tv") ? "" : query(c), T("Search movies and series…"), str_eq(active, "tv") ? "" : " data-live=\"media.search\"",
      T("Search the library"));
}

static void shelf_head(Buf *b, const char *title, const char *more) {
    H(b, "<section class=\"media-row\"><header class=\"media-row-h\"><h2>%s</h2>", title);
    if (more) H(b, "<a href=\"%s\">%s</a>", more, T("See all"));
    buf_puts(b, "</header><div class=\"media-shelf\">");
}

static void shelf_end(Buf *b) { buf_puts(b, "</div></section>"); }

/* "Recently added" with a series' batch of new episodes as one entry (media.h). An episode joins
   its series' latest entry when it came within a week of that entry's oldest episode, so two
   series copied in at once (their files interleaved) still make one entry each; an older batch
   of the same series is an entry of its own. */
#define BATCH_S (7 * 86400)
int media_recent(const Ctx *c, MediaRecent *out, int max) {
    const MediaIndex *ix = media_lib_for(c);
    int n = 0;
    for (int i = 0; i < ix->nrecent && max > 0; i++) {
        const MediaFile *f = &ix->files[ix->recent[i]];
        int k = -1;
        if (f->name.kind == MEDIA_EPISODE && f->series)
            for (int j = n - 1; j >= 0 && k < 0; j--)
                if (out[j].f->name.kind == MEDIA_EPISODE && out[j].f->series == f->series) k = j;
        if (k >= 0 && out[k].oldest - f->mtime <= BATCH_S) {
            out[k].n++;
            out[k].oldest = f->mtime;
            out[k].series = media_series(f->series);
            continue;
        }
        if (n < max) {
            out[n++] = (MediaRecent){f, NULL, 1, f->mtime};
            continue;
        }
        /* full: only open batches can still grow - stop once this is older than all of them */
        bool open = false;
        for (int j = 0; j < n && !open; j++) open = out[j].oldest - f->mtime <= BATCH_S;
        if (!open) break;
    }
    for (int j = 0; j < n; j++)
        if (out[j].n > 1 && !out[j].series) out[j].n = 1; /* (a series that vanished: just the video) */
    return n;
}

/* A shelf card for a batch of new episodes: the series' poster, "8 new episodes". */
static void batch_card(Buf *b, const MediaRecent *r) {
    const MediaSeries *s = r->series;
    H(b, "<a class=\"media-poster\" href=\"/media/series/%X\">", s->id);
    poster_art(b, s->title, s->year, T("Series"), s->poster, 0, false);
    H(b, "<span class=\"media-cap\">%s<small>", s->title);
    H(b, TN(r->n, "%d new episode", "%d new episodes"), r->n);
    buf_puts(b, "</small></span></a>");
}

/* ------------------------------------------------- folders to suggest (issue #27) */

/* What an admin can add with one tap instead of typing a path from memory: the folders the installer was told
   about (HEMMA_MEDIA in the settings file, ";" between them) - until they're in the library - and, while the
   library has no folders yet, the disks this machine has mounted (plat_mounts: under /mnt and /media, a Mac's
   /Volumes, Windows' other drives). Each once, only folders that are there. */
typedef struct {
    char path[300];
    bool installer; /* named by the installer (HEMMA_MEDIA) */
    int64_t free, total;
} Suggest;

static void gb(int64_t n, char *out, size_t cap);

static char g_test_mounts[600]; /* tests: the disks "this machine" has (";" between), instead of plat_mounts() */
void media_test_mounts(const char *list) { str_copy(g_test_mounts, sizeof g_test_mounts, list ? list : ""); }

static void same_slashes(char *p) {
    for (; *p; p++)
        if (*p == '\\') *p = '/';
}

static bool same_folder(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    while (na > 1 && a[na - 1] == '/') na--;
    while (nb > 1 && b[nb - 1] == '/') nb--;
    return na == nb && strncmp(a, b, na) == 0;
}

static int suggestions(Suggest *out, int max) {
    char roots[MEDIA_MAX_ROOTS][300], found[16][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS), n = 0, nfound = 0;
    const char *told = env("HEMMA_MEDIA", "");
    while (*told && nfound < countof(found)) { /* "/mnt/media;/mnt/more" */
        size_t len = strcspn(told, ";");
        str_ncopy(found[nfound], sizeof found[0], told, len);
        str_trim(found[nfound]);
        if (found[nfound][0]) nfound++;
        told += len + (told[len] == ';');
    }
    int ntold = nfound;
    if (!nroots && !app_testing()) nfound += plat_mounts(found + nfound, countof(found) - nfound);
    for (const char *t = g_test_mounts; !nroots && *t && nfound < countof(found);) { /* (tests: this machine's disks) */
        size_t len = strcspn(t, ";");
        str_ncopy(found[nfound++], sizeof found[0], t, len);
        t += len + (t[len] == ';');
    }
    for (int i = 0; i < nfound && n < max; i++) {
        same_slashes(found[i]);
        bool skip = !path_is_dir(found[i]);
        for (int k = 0; k < nroots && !skip; k++) skip = same_folder(roots[k], found[i]);
        for (int k = 0; k < n && !skip; k++) skip = same_folder(out[k].path, found[i]);
        if (skip) continue;
        Suggest *s = &out[n++];
        *s = (Suggest){.installer = i < ntold};
        str_copy(s->path, sizeof s->path, found[i]);
        path_disk_space(s->path, &s->free, &s->total);
    }
    return n;
}

/* One tap adds it (media.roots_add); `big` = the Watch page's first-run buttons, else chips. */
static void suggestion_forms(Buf *b, const Suggest *s, int n, bool big) {
    for (int i = 0; i < n; i++) {
        char size[32] = "";
        if (s[i].total > 0) gb(s[i].total, size, sizeof size);
        ui_form(b, "media.roots_add", "inline");
        ui_hidden(b, "dir", s[i].path);
        if (big) {
            buf_puts(b, "<button class=\"btn primary\">");
            H(b, T("Use <code class=\"media-code\">%s</code>"), s[i].path);
            buf_puts(b, "</button>");
        } else {
            buf_puts(b, "<button class=\"chip\" aria-label=\"");
            H(b, T("Add %s"), s[i].path);
            H(b, "\">+ <code class=\"media-code\">%s</code>%r%s", s[i].path, size[0] ? " <small>" : "", size);
            if (s[i].installer) H(b, " · %s</small>", T("from the installer"));
            else if (size[0]) buf_puts(b, "</small>");
            buf_puts(b, "</button>");
        }
        ui_form_end(b);
    }
}

/* A sentence naming who to ask (ui_admins: "Anna (admin)"), fmt's %r. */
static void ask_admins(Buf *b, const char *fmt) {
    Buf who = {0};
    ui_admins(&who);
    H(b, fmt, buf_cstr(&who));
    buf_free(&who);
}

/* The last row of a cut list: open ("<li class=\"muted\">") + "and 3 more" + "</li>". */
static void more_row(Buf *b, const char *open, int n) {
    buf_puts(b, open);
    H(b, T("and %d more"), n);
    buf_puts(b, "</li>");
}

static bool library_empty_states(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    if (ix->nroots == 0 && !is_admin(c)) {
        ui_empty(b, "🎬", T("No movies yet"), T("Movies and series from the drive at home will show up here."));
        buf_puts(b, "<p class=\"media-center muted\">");
        ask_admins(b, T("Ask %r to add the movie folders."));
        buf_puts(b, "</p>");
        return true;
    }
    if (ix->nroots == 0) {
        ui_empty(b, "🎬", T("Point Hemma at your movies"),
                 T("Tell it which folders hold your movies and series — the drive on the Pi, a mounted share, "
                   "or a folder on this computer."));
        if (media_drives_offer(c, b)) { /* (issue #74: a USB disk just plugged in) */
            H(b, "<p class=\"media-center\"><a href=\"/media/settings\">%s</a></p>", T("Another folder"));
            return true;
        }
        Suggest s[4];
        int ns = suggestions(s, countof(s)); /* (issue #27: one tap, not a path typed from memory) */
        if (ns) {
            buf_puts(b, "<div class=\"media-center media-suggest-big\">");
            suggestion_forms(b, s, ns, true);
            H(b, "</div><p class=\"media-center\"><a href=\"/media/settings\">%s</a></p>", T("Another folder"));
            return true;
        }
        H(b, "<p class=\"media-center\"><a class=\"btn primary\" href=\"/media/settings\">%s</a></p>", T("Choose folders"));
        return true;
    }
    if (ix->nrecent == 0) {
        if (media_scanning())
            ui_empty(b, "🔎", T("Looking through your library…"), T("Movies and series show up here as soon as the scan "
                                                                    "finishes."));
        else
            ui_empty(b, "🗂️", T("No videos found yet"),
                     T("Hemma looked in your folders but found no video files. Check the folders in Library settings."));
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- search */

static void search_results(Ctx *c, Buf *b, const char *q) {
    const MediaIndex *ix = media_lib_for(c);
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
    if (!shown)
        ui_empty(b, "🤷", T("Nothing matches"), T("Try another word — titles, episode names and folders are searched."));
}

/* ----------------------------------------------------------------- pages */

/* "2 movies", "1 series" (the same word for one and many in English: hence "1 series" as its own text) */
static void count_movies(char *out, size_t cap, int n) { snprintf(out, cap, TN(n, "%d movie", "%d movies"), n); }
static void count_series(char *out, size_t cap, int n) { snprintf(out, cap, TN(n, "1 series", "%d series"), n); }

static void page_home(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib_for(c);
    ctx_title(c, "%s", T("Watch"));
    char sub[160] = "";
    if (ix->nrecent) {
        char m[48], s[48], o[48];
        count_movies(m, sizeof m, ix->nmovies);
        count_series(s, sizeof s, ix->nseries);
        snprintf(o, sizeof o, TN(ix->nothers, "%d other video", "%d other videos"), ix->nothers);
        snprintf(sub, sizeof sub, "%s · %s · %s", m, s, o);
    }
    media_lib_head(c, b, "home", T("Watch"), sub, NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;

    const MediaFile *going[12];
    int ngoing = media_in_progress(c, going, countof(going));
    if (ngoing) {
        shelf_head(b, T("Continue watching"), NULL);
        for (int i = 0; i < ngoing; i++) going_card(c, b, going[i], false);
        shelf_end(b);
    }
    /* what the channels air right now: tv's row, found by id (tv links media, never the other way) */
    const Block *onnow = block_find("tv.onnow", NULL);
    Buf row = {0};
    if (onnow && onnow->render(c, &row, 0, "")) { /* kind 0: an ordinary page - plain links */
        H(b, "<section class=\"media-row media-onnow\"><header class=\"media-row-h\"><h2>%s</h2>"
             "<a href=\"/tv\">%s</a></header>",
          T("On TV"), T("Guide"));
        buf_put(b, row.p, row.len);
        buf_puts(b, "</section>");
    }
    buf_free(&row);
    MediaRecent recent[14];
    int nrecent = media_recent(c, recent, countof(recent));
    shelf_head(b, T("Recently added"), NULL);
    for (int i = 0; i < nrecent; i++) {
        if (recent[i].n > 1) batch_card(b, &recent[i]);
        else video_card(c, b, recent[i].f);
    }
    shelf_end(b);
    if (ix->nseries) {
        shelf_head(b, T("Series"), "/media/series");
        for (int i = 0; i < ix->nseries && i < 14; i++) series_card(c, b, &ix->series[i]);
        shelf_end(b);
    }
    if (ix->nmovies) {
        shelf_head(b, T("Movies"), "/media/movies");
        for (int i = 0; i < ix->nmovies && i < 14; i++) video_card(c, b, &ix->files[ix->movies[i]]);
        shelf_end(b);
    }
    if (ix->nothers) {
        shelf_head(b, T("Other videos"), "/media/browse");
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
    const MediaIndex *ix = media_lib_for(c);
    ctx_title(c, "%s", T("Movies"));
    char sub[48];
    count_movies(sub, sizeof sub, ix->nmovies);
    media_lib_head(c, b, "movies", T("Movies"), sub, NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    const char *sort = ctx_q(c, "sort");
    if (!*sort) sort = "az";
    H(b, "<nav class=\"seg media-sort\"><a href=\"/media/movies\"%r>%s</a><a href=\"/media/movies?sort=new\"%r>%s</a>"
         "<a href=\"/media/movies?sort=year\"%r>%s</a></nav>",
      str_eq(sort, "az") ? " class=\"on\"" : "", T("A–Z"), str_eq(sort, "new") ? " class=\"on\"" : "", T("Newest"),
      str_eq(sort, "year") ? " class=\"on\"" : "", T("Year"));
    int *order = xmalloc((size_t)(ix->nmovies ? ix->nmovies : 1) * sizeof(int));
    memcpy(order, ix->movies, (size_t)ix->nmovies * sizeof(int));
    g_sort_ix = ix;
    if (str_eq(sort, "new")) qsort(order, (size_t)ix->nmovies, sizeof(int), by_new);
    else if (str_eq(sort, "year")) qsort(order, (size_t)ix->nmovies, sizeof(int), by_year);
    buf_puts(b, "<div class=\"media-grid\">");
    for (int i = 0; i < ix->nmovies; i++) video_card(c, b, &ix->files[order[i]]);
    buf_puts(b, "</div>");
    if (!ix->nmovies)
        ui_empty(b, "🎞️", T("No movies yet"), T("Files with a year in the name — like \"Inception (2010).mkv\" — "
                                                "become movies."));
    free(order);
}

static void page_series_list(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib_for(c);
    ctx_title(c, "%s", T("Series"));
    char sub[48];
    count_series(sub, sizeof sub, ix->nseries);
    media_lib_head(c, b, "series", T("Series"), sub, NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    buf_puts(b, "<div class=\"media-grid\">");
    for (int i = 0; i < ix->nseries; i++) series_card(c, b, &ix->series[i]);
    buf_puts(b, "</div>");
    if (!ix->nseries)
        ui_empty(b, "📺", T("No series yet"), T("Episodes named like \"Show.S01E02.mkv\" are grouped into series."));
}

static void tick_form(Ctx *c, Buf *b, const MediaFile *f, bool done) {
    ui_form(b, "media.watched", "inline");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"done\" value=\"%d\">", f->id,
      done ? 0 : 1);
    const char *say = done ? T("Mark as not watched") : T("Mark as watched");
    H(b, "<button class=\"tick%r\" title=\"%s\" aria-label=\"%s\">", done ? " on" : "", say, say);
    ui_icon(b, "check");
    buf_puts(b, "</button>");
    ui_form_end(b);
}

static void page_series(Ctx *c, Buf *b, const char *idhex) {
    uint64_t id = 0;
    const MediaSeries *s = media_parse_id(idhex, &id) ? media_series(id) : NULL;
    if (!media_kids_series_ok(c, s)) s = NULL; /* (not for a child: as if it weren't there - issue #44) */
    if (!s) {
        ctx_title(c, "%s", T("Series"));
        if (media_tv_mode(c)) media_tv_open(c, b, NULL, NULL);
        ui_empty(b, "📺", T("This series isn't in the library"), T("It may have been moved or renamed."));
        if (c->bare) buf_puts(b, "</div>");
        return;
    }
    if (media_tv_mode(c)) { /* opened from a TV screen */
        media_series_tv(c, b, s);
        return;
    }
    ctx_title(c, "%s", s->title);
    buf_puts(b, "<a class=\"media-back\" href=\"/media/series\">");
    ui_icon(b, "left");
    H(b, "%s</a><div class=\"media-sdetail\"><div class=\"media-sdetail-art\">", T("Series"));
    poster_art(b, s->title, s->year, T("Series"), s->poster, 0, false);
    if (can_set_art(c, s->id)) {
        int sq = (int)ctx_qint(c, "s", -1);
        H(b, "<a class=\"media-pic-edit\" href=\"/media/series/%X?pic=1", s->id);
        if (sq >= 0) H(b, "&amp;s=%d", sq);
        H(b, "\" title=\"%s\">", T("Change the picture"));
        ui_icon(b, "edit");
        H(b, "<span>%s</span></a>", T("Picture"));
    }
    H(b, "</div><div class=\"media-sdetail-t\"><h1>%s</h1><p class=\"sub\">", s->title);
    if (s->nseasons > 1) {
        H(b, T("%d seasons"), s->nseasons);
        buf_puts(b, " · ");
    }
    int eps = media_episodes_in(s);
    H(b, TN(eps, "%d episode", "%d episodes"), eps);
    if (s->year) H(b, " · %d", s->year);
    int seen = watched_count(c, s);
    if (seen) {
        buf_puts(b, " · ");
        H(b, T("%d watched"), seen);
    }
    buf_puts(b, "</p>");
    const MediaFile *up = media_next_up(c, s);
    if (up) {
        const MediaWatch *w = media_watch_of(c, up);
        H(b, "<a class=\"btn primary big\" href=\"/media/watch/%X\">", up->id);
        ui_icon(b, "play");
        H(b, "<span>%s ", w && w->pos > 0 && !w->done ? T("Resume") : seen ? T("Next up") : T("Start watching"));
        media_ep_code(b, up);
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
            H(b, "<a href=\"/media/series/%X?s=%d\"%r>", s->id, last, last == season ? " class=\"on\"" : "");
            H(b, last == 0 ? T("Specials %d") : T("Season %d"), last);
            buf_puts(b, "</a>");
        }
        buf_puts(b, "</nav>");
    }
    buf_puts(b, "<ol class=\"media-eps\">");
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        if (s->nseasons > 1 && e->name.season != season) continue;
        const MediaWatch *w = media_watch_of(c, e);
        bool done = w && w->done;
        H(b,
          "<li class=\"media-ep%r%r\"><a class=\"media-ep-main\" href=\"/media/watch/%X\">"
          "<span class=\"media-ep-n%r\">%s</span>",
          done ? " media-ep-done" : "", e == up ? " media-ep-up" : "", e->id,
          e->name.episode_end > e->name.episode ? " media-ep-nn" : "", ep_n(e));
        buf_puts(b, "<span class=\"media-ep-t\"><b>");
        media_ep_title(b, e);
        buf_puts(b, "</b><small>");
        media_ep_code(b, e);
        if (e->probe.duration_ms) {
            buf_puts(b, " · ");
            duration(b, e);
        }
        if (e->name.unnumbered && e->name.disc) { /* numbered in disc order */
            buf_puts(b, " · ");
            H(b, T("disc %d"), e->name.disc);
        }
        if (media_playable(e)->probe.playable == PLAY_NO)
            H(b, " · <span class=\"media-warn\">%s</span>", T("needs conversion"));
        buf_puts(b, "</small>");
        int p = media_percent(w);
        if (p && !done) H(b, "<span class=\"media-ep-bar\"><i style=\"width:%d%%\"></i></span>", p);
        buf_puts(b, "</span></a>");
        if (ctx_viewer(c)) tick_form(c, b, e, done); /* (a display account with nobody picked: no ticks) */
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ol>");

    /* the season's bonus material from ripped discs: "play all" titles and clips (library.c) */
    const MediaIndex *ix = media_lib_for(c);
    int nx = 0;
    for (int i = 0; i < ix->nextras; i++) {
        const MediaFile *x = &ix->files[ix->extras[i]];
        if (x->series != s->id || (s->nseasons > 1 && x->name.season != season)) continue;
        if (!nx++) H(b, "<h2 class=\"mt\">%s</h2><ol class=\"media-eps media-eps-compact\">", T("Extras"));
        H(b, "<li class=\"media-ep\"><a class=\"media-ep-main\" href=\"/media/watch/%X\"><span class=\"media-ep-n\">",
          x->id);
        ui_icon(b, x->role == MF_PLAYALL ? "play" : "film");
        buf_puts(b, "</span><span class=\"media-ep-t\"><b>");
        if (x->role == MF_PLAYALL) H(b, "%s", T("All episodes in one"));
        else if (x->name.ep_title[0]) html_esc(b, x->name.ep_title);
        else if (x->name.track >= 0) H(b, T("Extra · title %d"), x->name.track);
        else H(b, "%s", T("Extra"));
        buf_puts(b, "</b><small>");
        if (x->name.disc) H(b, T("Disc %d"), x->name.disc);
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
        art_modal(c, b, s->id, s->title, s->year, T("Series"), s->poster, close);
    }
}

/* Folder browser over the index: /media/browse?p=<root>:<dir> */
typedef struct {
    char name[200];
    int videos;
} Sub;

static void page_browse(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "%s", T("Folders"));
    media_lib_head(c, b, "browse", T("Folders"), NULL, NULL);
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
      ix->nroots > 1 ? ix->roots[root] : T("Library"));
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
    /* "Not for children" (issue #44): this folder and everything in it - an admin's mark */
    const char *above = media_kids_covered(ix->roots[root], dir);
    if (is_admin(c)) {
        bool on = media_kids_marked(ix->roots[root], dir);
        char p_arg[420];
        snprintf(p_arg, sizeof p_arg, "%d:%s", root, dir);
        ui_form(b, "media.kids", "media-kids");
        ui_hidden(b, "p", p_arg);
        if (above) {
            buf_puts(b, "<p class=\"media-kids-on\">");
            H(b, T("Not for children: <a href=\"/media/browse?p=%d:%u\">%s</a> is marked, and this is in it."), root, above,
              above[0] ? above : ix->roots[root]);
            buf_puts(b, "</p>");
        } else {
            H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> %s "
                 "<span class=\"muted media-small\">%s</span></label>",
              on ? " checked" : "", T("Not for children"),
              dir[0] ? T("- this folder and everything in it stays out of a child's Watch")
                     : T("- the whole library folder and everything in it stays out of a child's Watch"));
        }
        ui_form_end(b);
    }

    size_t dl = strlen(dir);
    Sub *subs = NULL;
    int nsubs = 0, capsubs = 0, nvid = 0;
    buf_puts(b, "<div class=\"media-grid\" id=\"media-files\">");
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        /* a play-everywhere copy is shown as its original; a play-all's header isn't a video */
        if (f->root != root || f->type != MF_VIDEO || f->role == MF_WEBCOPY || media_is_playall_head(f->rel)) continue;
        if (!media_kids_ok(c, f)) continue; /* (a child's: not what's marked - issue #44) */
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
            H(b, "<span>%s</span><small>", subs[k].name);
            H(b, TN(subs[k].videos, "%d video", "%d videos"), subs[k].videos);
            if (is_admin(c) && media_kids_marked(ix->roots[root], next)) H(b, " · %s", T("not for children"));
            buf_puts(b, "</small></a></li>");
        }
        buf_puts(b, "</ul>");
    }
    if (!nsubs && !nvid) ui_empty(b, "📂", T("Nothing to play here"), NULL);
    free(subs);
}

/* ----------------------------------------------------------------- removing */

/* Admins take a film (or another video) out of the library for good: remove.c deletes its files
   - and its own folder, when it has one. Episodes belong to their series and aren't removed one by one. */
static bool can_remove(const Ctx *c, const MediaFile *f) {
    return is_admin(c) && f->type == MF_VIDEO && f->role == MF_MAIN && f->name.kind != MEDIA_EPISODE;
}

/* "1.5 GB" -> "1,5 GB" in Swedish and Spanish */
static void decimal_mark(char *s) {
    char d = i18n_decimal();
    for (; d != '.' && *s; s++)
        if (*s == '.') *s = d;
}

static void size_text(int64_t n, char *out, size_t cap) {
    if (n >= 1000LL * 1048576) snprintf(out, cap, "%.1f GB", (double)n / 1073741824.0);
    else if (n >= 1048576) snprintf(out, cap, "%lld MB", (long long)(n / 1048576));
    else snprintf(out, cap, "%lld KB", (long long)((n + 1023) / 1024));
    decimal_mark(out);
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
    ui_modal(b, T("Remove from the library"), close);
    buf_puts(b, "<p>");
    if (f->name.year)
        H(b, T("<b>%s</b> (%d) is deleted from the disk - for everyone in the household, and it can't be undone."),
          f->name.title, f->name.year);
    else H(b, T("<b>%s</b> is deleted from the disk - for everyone in the household, and it can't be undone."), f->name.title);
    buf_puts(b, "</p><p class=\"muted\">");
    if (p.folder[0])
        H(b,
          TN(p.files, "Its folder <code class=\"media-code\">%s</code> goes with everything in it: %d file, %s.",
             "Its folder <code class=\"media-code\">%s</code> goes with everything in it: %d files, %s."),
          p.folder, p.files, size);
    else H(b, TN(p.files, "%d file, %s:", "%d files, %s:"), p.files, size);
    buf_puts(b, "</p>");
    buf_puts(b, "<ul class=\"media-rm-list\">");
    int shown = 0;
    size_t skip = p.folder[0] ? strlen(p.folder) + 1 : 0;
    for (int i = 0; i < p.n && shown < 12; i++) {
        if (p.items[i].dir) continue;
        const char *rel = p.items[i].rel;
        H(b, "<li><code class=\"media-code\">%s</code></li>", strlen(rel) > skip ? rel + skip : rel);
        shown++;
    }
    if (p.files > shown) more_row(b, "<li class=\"muted\">", p.files - shown);
    buf_puts(b, "</ul>");
    ui_form(b, "media.remove", "inline");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><div class=\"modal-f\"><a class=\"btn\" "
         "href=\"%s\">%s</a><button class=\"btn danger\">%s</button></div>",
      f->id, close, T("Keep it"), T("Delete from the disk"));
    ui_form_end(b);
    ui_modal_end(b);
    media_remove_free(&p);
}

void media_upnext(Buf *b, const MediaFile *next, const char *href, bool tv) {
    H(b, "<div class=\"media-upnext\" id=\"media-upnext\" data-upnext=\"%d\" data-static=\"%X\" hidden%r>"
         "<p class=\"media-upnext-t\"><small>",
      MEDIA_UPNEXT_S, next->id, tv ? " data-arrows" : "");
    H(b, T("Next episode in <span data-count>%d</span> s"), MEDIA_UPNEXT_S);
    buf_puts(b, "</small><b>");
    media_ep_code(b, next);
    buf_puts(b, " · ");
    media_ep_title(b, next);
    H(b, "</b></p><div class=\"media-upnext-a\"><a class=\"btn primary\" href=\"%r\"%r>", href, tv ? " autofocus" : "");
    ui_icon(b, "right");
    H(b, "<span>%s</span></a><button type=\"button\" class=\"btn\" data-hide>%s</button></div></div>", T("Play now"),
      T("Hide"));
}

static void page_watch(Ctx *c, Buf *b, const char *idhex) {
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(idhex, &id) ? media_file(id) : NULL;
    if (!media_kids_ok(c, f)) f = NULL; /* (issue #44) */
    if (!f || f->type != MF_VIDEO) {
        ctx_title(c, "%s", T("Watch"));
        if (media_tv_mode(c)) media_tv_open(c, b, NULL, NULL);
        ui_empty(b, "🎬", T("This video isn't in the library"), T("It may have been moved, renamed or deleted."));
        if (c->bare) buf_puts(b, "</div>");
        return;
    }
    if (media_tv_mode(c)) { /* opened from a TV screen: the player, full screen */
        media_watch_tv(c, b, f);
        return;
    }
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    const MediaFile *prev = media_neighbour(f, -1), *next = media_neighbour(f, +1);
    const MediaWatch *w = media_watch_of(c, f);
    char title[300];
    media_title(f, title, sizeof title);
    if (s) {
        Buf t = {0};
        buf_printf(&t, "%s ", f->name.title);
        media_ep_code(&t, f);
        ctx_title(c, "%s", buf_cstr(&t));
        buf_free(&t);
        H(b, "<a class=\"media-back\" href=\"/media/series/%X?s=%d\">", s->id, f->name.season);
        ui_icon(b, "left");
        H(b, "%s</a>", s->title);
    } else {
        ctx_title(c, "%s", title);
        H(b, "<a class=\"media-back\" href=\"%s\">", f->name.kind == MEDIA_MOVIE ? "/media/movies" : "/media/browse");
        ui_icon(b, "left");
        H(b, "%s</a>", f->name.kind == MEDIA_MOVIE ? T("Movies") : T("Folders"));
    }

    /* the player: data-static="<the video>" keeps live re-renders away from it, and the next
       episode morphs into the same element - full screen (the box: the card shows over it) stays.
       Our own controls (media_controls); the browser's only without wire.js */
    bool autoplay = str_eq(ctx_q(c, "auto"), "1");
    Buf go = {0};
    if (next) H(&go, "/media/watch/%X?auto=1", next->id);
    int at = autoplay ? 0 : media_resume_at(w);
    char src[120];
    media_file_src_at(c, f->id, at, src, sizeof src);
    H(b, "<div class=\"media-player-box\" id=\"media-box\"><video id=\"media-video\" class=\"media-player\" "
         "src=\"%s\" controls preload=\"metadata\" playsinline data-static=\"%X\" data-id=\"%X\" "
         "data-report=\"media.progress\"",
      src, f->id, f->id);
    if (at) H(b, " data-resume=\"%d\"", at);
    if (next) H(b, " data-next=\"%r\"", buf_cstr(&go));
    if (autoplay) buf_puts(b, " autoplay");
    uint64_t poster = f->poster ? f->poster : s ? s->poster : 0;
    if (poster) {
        buf_puts(b, " poster=\"");
        media_img_src(b, poster);
        buf_putc(b, '"');
    }
    buf_putc(b, '>');
    media_tracks(c, b, f);
    buf_puts(b, "</video>");
    if (next) media_upnext(b, next, buf_cstr(&go), false);
    media_controls(c, b, f, false);
    buf_puts(b, "</div>");
    buf_free(&go);

    /* about it */
    buf_puts(b, "<div class=\"media-watch-info\"><div class=\"media-watch-t\">");
    if (s) {
        H(b, "<h1>%s</h1><p class=\"sub\">", f->name.ep_title[0] ? f->name.ep_title : f->name.title);
        media_ep_code(b, f);
        H(b, " · %s", s->title);
    } else {
        H(b, "<h1>%s</h1><p class=\"sub\">", title);
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
        H(b, "<a class=\"btn\" href=\"/media/watch/%X\" title=\"%s\">", prev->id, T("Previous episode"));
        ui_icon(b, "left");
        H(b, "<span>%s</span></a>", T("Previous"));
    }
    if (next) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X\" title=\"%s\"><span>%s</span>", next->id, T("Next episode"), T("Next"));
        ui_icon(b, "right");
        buf_puts(b, "</a>");
    }
    if (ctx_viewer(c)) {
        bool done = w && w->done;
        ui_form(b, "media.watched", "inline");
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"done\" value=\"%d\">", f->id,
          done ? 0 : 1);
        H(b, "<button class=\"btn%r\" aria-pressed=\"%r\">", done ? " media-watched" : "", done ? "true" : "false");
        ui_icon(b, "check");
        H(b, "<span>%s</span></button>", done ? T("Watched") : T("Mark watched"));
        ui_form_end(b);
    }
    if (can_set_art(c, f->id)) { /* films and other videos; episodes use their series' picture */
        H(b, "<a class=\"btn\" href=\"/media/watch/%X?pic=1\" title=\"%s\">", f->id, T("Change the picture"));
        ui_icon(b, "edit");
        H(b, "<span>%s</span></a>", T("Picture"));
    }
    if (can_remove(c, f)) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X?remove=1\" title=\"%s\">", f->id, T("Delete it from the library"));
        ui_icon(b, "trash");
        H(b, "<span>%s</span></a>", T("Remove"));
    }
    buf_puts(b, "</div></div>");

    static const char *const cls[] = {"", "ok", "warn", "bad"};
    const MediaFile *pf = media_playable(f); /* the play-everywhere copy, when there is one */
    Playable pl = pf->probe.playable;
    /* an HEVC copy is as far as Hemma goes (no HEVC decoder): say where it plays, not just "may not" */
    bool hevc = pl == PLAY_MAYBE && str_eq(pf->probe.vcodec, "hevc") && !str_eq(pf->probe.container, "mkv");
    H(b, "<div class=\"media-playable\"><span class=\"badge %s\">%s</span>", cls[CLAMP((int)pl, 0, 3)],
      hevc ? T("Plays on newer devices") : T(media_playable_label(pl)));
    if (hevc)
        H(b, "<span class=\"muted\">%s%s%s</span>", pf != f ? T("Converted copy") : "", pf != f ? " · " : "",
          T("HEVC video: Safari on iPhone, iPad and Mac, and Chrome or Edge on most computers and phones from the last "
            "few years"));
    else if (pf != f) H(b, "<span class=\"muted\">%s</span>", T("Converted copy"));
    else if (pl != PLAY_YES && pf->probe.why[0]) {
        char why[200];
        media_probe_why(&pf->probe, why, sizeof why);
        H(b, "<span class=\"muted\">%s</span>", why);
    }
    if (pf->probe.container[0] || pf->probe.vcodec[0]) { /* the codecs folded away: plain words first (issue #62) */
        H(b, "<details class=\"media-tech\"><summary>%s</summary><span>", T("File details"));
        tech_facts(b, pf);
        buf_puts(b, "</span></details>");
    }
    buf_puts(b, "</div>");
    ConvertNeed need = convert_need(f);
    if (need != CONV_NONE) { /* Hemma makes a copy that plays everywhere, once */
        buf_puts(b, "<div class=\"media-convert\">");
        if (convert_queued(f->id))
            H(b, "<p class=\"muted\">%s</p>", T("Being made playable everywhere - see the library settings."));
        else if (is_admin(c)) {
            bool hevc = str_eq(f->probe.vcodec, "hevc"), dolby = str_eq(f->probe.acodec, "ac3") || str_eq(f->probe.acodec, "eac3");
            H(b, "<p class=\"muted\">%s</p>",
              hevc && dolby ? T("Its Dolby sound plays silently in Chrome and Firefox, and Safari can't open MKV files: a copy "
                                "with AAC sound fixes both - quick, the picture isn't re-encoded. HEVC still needs a device "
                                "that can decode it (most from the last few years).")
              : hevc ? T("Safari (iPhone, iPad, Mac) can't open MKV files: a new container fixes that - quick, the picture "
                         "isn't re-encoded. HEVC still needs a device that can decode it (most from the last few years).")
              : need == CONV_REMUX ? T("A new container makes this play on iPhones and every TV too - quick, nothing is "
                                       "re-encoded.")
                                   : T("Hemma can convert this once into a copy that plays on every browser, iPhone and TV."));
            char hex[17];
            media_id_hex(f->id, hex);
            ui_act(b, "media.convert", "btn",
                   hevc && dolby ? T("Fix the sound") : hevc ? T("Make it play in Safari too") : T("Make it play everywhere"),
                   "id", hex, NULL);
        } else {
            buf_puts(b, "<p class=\"muted\">");
            ask_admins(b, T("Hemma can make a copy of this that plays everywhere: ask %r."));
            buf_puts(b, "</p>");
        }
        buf_puts(b, "</div>");
    } else if (pl == PLAY_NO && pf == f) {
        H(b, "<p class=\"muted media-convert\">%s</p>", T("This format can't be converted by Hemma yet."));
    }

    /* the rest of the season, for binge-friendly navigation */
    if (s && s->count > 1) {
        H(b, "<h2 class=\"mt\">%s</h2><ol class=\"media-eps media-eps-compact\">", T("More episodes"));
        for (int i = 0; i < s->count; i++) {
            const MediaFile *e = media_episode(s, i);
            if (e->name.season != f->name.season) continue;
            const MediaWatch *ew = media_watch_of(c, e);
            H(b,
              "<li class=\"media-ep%r%r\"><a class=\"media-ep-main\" href=\"/media/watch/%X\">"
              "<span class=\"media-ep-n%r\">%s</span>",
              ew && ew->done ? " media-ep-done" : "", e == f ? " media-ep-up" : "", e->id,
              e->name.episode_end > e->name.episode ? " media-ep-nn" : "", ep_n(e));
            buf_puts(b, "<span class=\"media-ep-t\"><b>");
            media_ep_title(b, e);
            buf_puts(b, "</b><small>");
            media_ep_code(b, e);
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
        art_modal(c, b, f->id, f->name.title, f->name.year, f->name.kind == MEDIA_OTHER ? T("Video") : NULL, f->poster,
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
    H(b, "<p class=\"muted media-small\">%s</p>", T("Admins can always rescan. Who else may:"));
    for (int i = 0; i < person_count(); i++) {
        const Person *p = person_at(i);
        if (!can_sign_in_member(p)) continue;
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"p\" value=\"%d\" data-auto%r> %s</label>", p->id,
          rescan_granted(p->id) ? " checked" : "", p->name);
    }
    ui_form_end(b);
}

/* Videos the scan left out (issue #28: a film named "Sample Film (2021)" vanished without a word). */
static void left_card(Ctx *c, Buf *b, const MediaIndex *ix) {
    if (!is_admin(c) || !ix->nleft_all) return;
    ui_card(b, T("Left out"), NULL, NULL);
    buf_puts(b, "<p class=\"muted\">");
    H(b,
      TN(ix->nleft_all,
         "%d video is left out of the library: by its name or folder, it looks like trailers, extras or samples (a short "
         "preview clip). A video named like one stays in when it runs long, or when Hemma can't tell how long it runs. "
         "One that belongs in? Rename it, and rescan.",
         "%d videos are left out of the library: by their name or folder, they look like trailers, extras or samples (a "
         "short preview clip). A video named like one stays in when it runs long, or when Hemma can't tell how long it "
         "runs. One that belongs in? Rename it, and rescan."),
      ix->nleft_all);
    H(b, "</p><details class=\"media-left\"><summary>%s</summary><ul class=\"rows\">", T("Which ones"));
    for (int i = 0; i < ix->nleft; i++) {
        const MediaLeft *l = &ix->left[i];
        H(b, "<li class=\"row\"><div class=\"row-main\"><code class=\"media-code\">%s</code><small class=\"muted\">%s"
             "</small></div></li>",
          l->rel,
          l->why == MEDIA_LEFT_SAMPLE  ? T("starts with \"sample\" and runs under 10 minutes: a preview clip")
          : l->why == MEDIA_LEFT_NAMED ? T("named like a trailer or extra (\"-trailer\", \".sample\") and runs under 40 minutes")
                                       : T("in a folder for extras (Extras, Trailers, Featurettes...)"));
    }
    if (ix->nleft_all > ix->nleft) more_row(b, "<li class=\"row muted\">", ix->nleft_all - ix->nleft);
    buf_puts(b, "</ul></details>");
    ui_card_end(b);
}

/* Uploads that never finished (issue #40): their ".part" files stayed for good, as big as a film,
   and nothing in Hemma showed them. Listed once nobody has touched them for MEDIA_PART_DAYS. */
static void parts_card(Ctx *c, Buf *b, const MediaIndex *ix) {
    if (!is_admin(c) || !ix->nparts_all) return;
    char sz[24];
    media_size_text(ix->parts_bytes, sz, sizeof sz);
    ui_card(b, T("Unfinished uploads"), NULL, NULL);
    buf_puts(b, "<p class=\"muted\">");
    H(b,
      TN(ix->nparts_all, "%d upload never finished, and nobody has touched it for over a week: %s on the disk.",
         "%d uploads never finished, and nobody has touched them for over a week: %s on the disk."),
      ix->nparts_all, sz);
    H(b, "</p><details class=\"media-left\"><summary>%s</summary><ul class=\"rows\">", T("Which ones"));
    for (int i = 0; i < ix->nparts; i++) {
        const MediaLeft *l = &ix->parts[i];
        char size[24], day[11];
        media_size_text(l->size, size, sizeof size);
        fmt_ymd(l->mtime, day);
        H(b, "<li class=\"row\"><div class=\"row-main\"><code class=\"media-code\">%s</code><small class=\"muted\">", l->rel);
        H(b, T("%s, last written %s"), size, day);
        buf_puts(b, "</small></div></li>");
    }
    if (ix->nparts_all > ix->nparts) more_row(b, "<li class=\"row muted\">", ix->nparts_all - ix->nparts);
    buf_puts(b, "</ul></details>");
    ui_form_confirm(b, "media.parts_remove", "inline", T("Remove these unfinished uploads? They can't be resumed after."));
    H(b, "<button class=\"btn danger\">%s</button>", T("Remove them"));
    ui_form_end(b);
    ui_card_end(b);
}

static void act_parts_remove(Ctx *c, Fields *f) {
    (void)f;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    const MediaIndex *ix = media_lib();
    int removed = 0;
    int64_t bytes = 0;
    int more = ix->nparts_all - ix->nparts;
    for (int i = 0; i < ix->nparts; i++) { /* each one again: still there, still untouched, no upload's */
        const MediaLeft *l = &ix->parts[i];
        char path[800], video[800];
        int64_t size = 0, mtime = 0;
        snprintf(path, sizeof path, "%s/%s", ix->roots[l->root], l->rel);
        if (!str_iends(path, MEDIA_PART_SUFFIX)) continue; /* (only Hemma's own: issue #89) */
        /* upload_claims knows the video by its own name, not its half-sent file's (issue #89: it never matched) */
        snprintf(video, sizeof video, "%.*s", (int)(strlen(path) - (sizeof MEDIA_PART_SUFFIX - 1)), path);
        if (!path_stat(path, &size, &mtime, NULL) || c->now - mtime <= MEDIA_PART_DAYS * 86400LL || upload_claims(video))
            continue;
        if (file_remove(path)) removed++, bytes += size;
    }
    char sz[24], logged[24];
    Lang was = i18n_use(LANG_EN); /* (the log speaks English: "1.2 GB") */
    media_size_text(bytes, logged, sizeof logged);
    i18n_use(was);
    media_size_text(bytes, sz, sizeof sz);
    if (removed)
        log_info("media: %s removed %d unfinished upload%s (%s)", c->me->name, removed, removed == 1 ? "" : "s", logged);
    media_scan_start(); /* (the list comes again from the disk) */
    if (more > 0)
        ctx_ok(c,
               TN(removed, "Removed %d unfinished upload (%s) - %d more show here after the scan",
                  "Removed %d unfinished uploads (%s) - %d more show here after the scan"),
               removed, sz, more);
    else
        ctx_ok(c, TN(removed, "Removed %d unfinished upload: %s free again", "Removed %d unfinished uploads: %s free again"),
               removed, sz);
}

/* Watch history that found no video after the last scan (issue #93: it used to stay behind without a
   word) - matched by place, content and name already; what's left is a video deleted, or renamed and
   changed at once. */
static void history_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    char labels[8][100];
    bool away = false;
    int n = media_history_lost(labels, countof(labels), &away);
    if (!n) return;
    ui_card(b, T("Watch history without its video"), NULL, NULL);
    buf_puts(b, "<p class=\"muted\">");
    H(b,
      TN(n,
         "The household watched %d video that isn't in the library any more, and it couldn't be found under another "
         "name or folder.",
         "The household watched %d videos that aren't in the library any more, and they couldn't be found under another "
         "name or folder."),
      n);
    if (away) H(b, " %s", T("A library folder is missing or empty right now: they may come back with its disk."));
    buf_puts(b, "</p><ul class=\"rows\">");
    int named = 0;
    for (int i = 0; i < n && i < countof(labels); i++)
        if (labels[i][0]) H(b, "<li class=\"row\"><div class=\"row-main\">%s</div></li>", labels[i]), named++;
    if (n > named) more_row(b, "<li class=\"row muted\">", n - named);
    buf_puts(b, "</ul>");
    if (!away) {
        ui_form_confirm(b, "media.history_forget", "inline",
                        T("Forget this watch history? Continue watching and watched ticks for these videos go."));
        H(b, "<button class=\"btn\">%s</button>", T("Forget it"));
        ui_form_end(b);
    }
    ui_card_end(b);
}

static void act_history_forget(Ctx *c, Fields *f) {
    (void)f;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    int n = media_history_forget();
    if (n) log_info("media: %s forgot the watch history of %d video%s no longer in the library", c->me->name, n,
                    n == 1 ? "" : "s");
    ctx_ok(c, TN(n, "Forgot the history of %d video", "Forgot the history of %d videos"), n);
}

/* Library settings: the folders marked "Not for children" (issue #44; marked on the Folders tab) */
static void kids_card(Buf *b) {
    ui_card(b, T("Not for children"), "/media/browse", T("Folders"));
    int n = media_kids_count();
    if (!n) {
        H(b, "<p class=\"muted\">%r</p>",
          T("Children see the whole library. To keep a folder out of their Watch, open it under "
            "<a href=\"/media/browse\">Folders</a> and tick <i>Not for children</i>."));
        ui_card_end(b);
        return;
    }
    H(b, "<p class=\"muted\">%s</p><ul class=\"rows media-kids-list\">",
      T("Left out of a child's Watch - and of the TV while a child is picked there:"));
    for (int i = 0; i < n; i++) {
        char root[300], dir[400];
        if (!media_kids_at(i, root, sizeof root, dir, sizeof dir)) continue;
        H(b, "<li class=\"row\"><div class=\"row-main\"><code class=\"media-code\">%s%r%s</code></div>", root,
          dir[0] ? "/" : "", dir);
        ui_form(b, "media.kids", "inline");
        ui_hidden(b, "root", root);
        ui_hidden(b, "dir", dir);
        buf_puts(b, "<button class=\"btn sm\" aria-label=\"");
        H(b, T("Open %s to children"), dir[0] ? dir : root);
        H(b, "\">%s</button>", T("Open to children"));
        ui_form_end(b);
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ul>");
    ui_card_end(b);
}

static void page_settings(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "%s", T("Library settings"));
    buf_puts(b, "<a class=\"media-back\" href=\"/media\">");
    ui_icon(b, "left");
    H(b, "%s</a>", T("Watch"));
    ui_head(b, T("Library"), T("Where your movies and series live"));
    ui_head_end(b);
    buf_puts(b, "<div class=\"grid2\">");

    ui_card(b, T("Folders"), NULL, NULL);
    if (is_admin(c)) {
        ui_form(b, "media.roots", "stack");
        H(b, "<label class=\"field\"><span>%s</span><textarea name=\"roots\" rows=\"4\" "
             "spellcheck=\"false\" placeholder=\"%r\">",
          T("One folder per line"), T("/mnt/media&#10;D:\\Movies"));
        for (int r = 0; r < ix->nroots; r++) H(b, "%s%s", r ? "\n" : "", ix->roots[r]);
        H(b, "</textarea></label><p class=\"muted media-small\">%r</p><div class=\"modal-f\"><button class=\"btn primary\">"
             "%s</button></div>",
          T("On the Raspberry Pi that's where the drive is mounted, e.g. <code class=\"media-code\">/mnt/media</code>. "
            "A network share works too once it's mounted."),
          T("Save & scan"));
        ui_form_end(b);
        Suggest s[8];
        int ns = suggestions(s, countof(s)); /* (issue #27) */
        if (ns) {
            H(b, "<div class=\"media-suggest\" id=\"media-suggest\"><p class=\"muted media-small\">%s</p><div class=\"chips\">",
              T("Found on this machine - tap one to add it:"));
            suggestion_forms(b, s, ns, false);
            buf_puts(b, "</div></div>");
        }
    } else {
        buf_puts(b, "<ul class=\"rows\">");
        for (int r = 0; r < ix->nroots; r++) H(b, "<li class=\"row\"><code class=\"media-code\">%s</code></li>", ix->roots[r]);
        if (!ix->nroots) H(b, "<li class=\"row muted\">%s</li>", T("No folders yet."));
        buf_puts(b, "</ul><p class=\"muted media-small\">");
        ask_admins(b, T("An admin chooses which folders are in the library: ask %r."));
        buf_puts(b, "</p>");
    }
    ui_card_end(b);
    media_drives_card(c, b); /* (issue #74: USB disks - new ones, and the library's own) */

    if (is_admin(c) && ix->nroots) {
        ui_card(b, T("Add videos"), "/media/upload", T("Upload"));
        H(b, "<p class=\"muted\">%s</p>",
          T("Send movies and series from a laptop or PC to the library - whole folders at a time, picking up where it "
            "left off if the connection drops."));
        upload_summary(c, b);
        ui_card_end(b);
    }
    convert_card(c, b);

    ui_card(b, T("Status"), NULL, NULL);
    buf_puts(b, "<dl class=\"media-stats\">");
    H(b, "<div><dt>%s</dt><dd>%d</dd></div><div><dt>%s</dt><dd>%d</dd></div>"
         "<div><dt>%s</dt><dd>%d</dd></div><div><dt>%s</dt><dd>%d</dd></div>",
      T("Movies"), ix->nmovies, T("Series"), ix->nseries, T("Episodes"), ix->nepisodes, T("Other videos"), ix->nothers);
    int playable = 0;
    for (int i = 0; i < ix->nrecent; i++) playable += media_playable(&ix->files[ix->recent[i]])->probe.playable == PLAY_YES;
    H(b, "<div><dt>%s</dt><dd>", T("Play everywhere"));
    H(b, T("%d of %d"), playable, ix->nrecent);
    buf_puts(b, "</dd></div></dl><p class=\"muted\">");
    if (media_scanning()) {
        int seen = media_scan_seen();
        H(b, TN(seen, "Scanning now — %d file looked at so far.", "Scanning now — %d files looked at so far."), seen);
    } else if (ix->scanned_at) {
        Buf ago = {0};
        ui_ago(&ago, ix->scanned_at, c->now);
        H(b,
          TN(ix->probed, "Last scan %r, took %d ms (%d new or changed file read). Hemma rescans every 30 minutes.",
             "Last scan %r, took %d ms (%d new or changed files read). Hemma rescans every 30 minutes."),
          buf_cstr(&ago), ix->scan_ms, ix->probed);
        buf_free(&ago);
    } else H(b, "%s", T("Showing the library as it was last time; a fresh scan starts on its own."));
    buf_puts(b, "</p>");
    if (can_rescan(c)) ui_act(b, "media.rescan", "btn", T("Rescan now"), NULL, NULL, NULL);
    if (is_admin(c)) rescanners_form(b);
    ui_card_end(b);
    left_card(c, b, ix);
    parts_card(c, b, ix);
    history_card(c, b);

    if (c->me) {
        ui_card(b, T("Playback"), NULL, NULL);
        const char *cur = pref(c->me->id, "media.sublang", "");
        ui_form(b, "media.prefs", "stack");
        H(b, "<label class=\"field\"><span>%s</span><select name=\"sublang\" data-auto>", T("Subtitles on by default"));
        static const char *const langs[] = {"", "sv", "en", "no", "da", "fi", "de", "fr", "es"};
        for (int i = 0; i < countof(langs); i++)
            H(b, "<option value=\"%s\"%r>%s</option>", langs[i], str_eq(cur, langs[i]) ? " selected" : "",
              i == 0 ? T("Off") : lang_name(langs[i]));
        buf_puts(b, "</select></label><p class=\"muted media-small\">");
        H(b,
          T("Just for %s. Subtitle files next to a video (<code class=\"media-code\">Movie.sv.srt</code>) are picked up "
            "automatically."),
          c->me->name);
        H(b, "</p><noscript><button class=\"btn\">%s</button></noscript>", T("Save"));
        ui_form_end(b);
        ui_card_end(b);
    }

    if (is_admin(c)) kids_card(b);

    ui_card(b, T("What plays where"), NULL, NULL);
    /* plain words first, the formats folded away (issue #62) */
    H(b, "<p class=\"muted media-plays\">%r</p>",
      T("Most videos play straight away. Some - DVD rips especially - need converting first: one button (<i>Play "
        "everywhere</i> above) makes a copy that plays on every phone, computer and TV."));
    H(b, "<ul class=\"media-tips\"><li><span class=\"badge ok\">%s</span> %r</li>", T("Plays in browsers"),
      T("On every screen."));
    H(b, "<li><span class=\"badge warn\">%s</span> %r</li>", T("May not play everywhere"),
      T("On computers and most phones, but a TV's browser or an iPhone may refuse it. <i>Plays on newer devices</i>: "
        "most from the last few years."));
    H(b, "<li><span class=\"badge bad\">%s</span> %r</li></ul>", T("Needs conversion"),
      T("Nowhere until Hemma has made its copy."));
    H(b, "<details class=\"media-formats\"><summary>%s</summary><ul class=\"media-tips\">", T("Which formats are which"));
    H(b, "<li>%r</li>", T("<b>Plays in browsers:</b> MP4 or WebM with H.264/VP9 video and AAC/MP3/Opus sound."));
    H(b, "<li>%r</li>",
      T("<b>May not play everywhere:</b> MKV, HEVC, or Dolby sound (Dolby Digital and Digital Plus: AC-3, E-AC-3) — fine "
        "in Chrome/Edge or Safari, not all of them; Dolby sound plays silently in Chrome and Firefox. An HEVC film "
        "converted here (<i>Plays on newer devices</i>) plays in Safari and on most devices from the last few years."));
    H(b, "<li>%r</li></ul></details>",
      T("<b>Needs conversion:</b> MPEG-2 (DVD rips), AVI/XviD, DTS sound. Hemma converts MKV files with MPEG-2, H.264 or "
        "HEVC video and Dolby or AAC sound itself, and keeps their text subtitles as .srt files next to them."));
    ui_card_end(b);
    buf_puts(b, "</div>");
    media_drives_dialog(c, b); /* (?format=<disk>) */
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
        ctx_title(c, "%s", T("Watch"));
        ui_empty(b, "🧭", T("Nothing here"), T("That page doesn't exist."));
    }
}

/* --------------------------------------------------------------- widgets */

static void mini(Ctx *c, Buf *b, const MediaFile *f) {
    const MediaWatch *w = media_watch_of(c, f);
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    H(b, "<a class=\"media-mini\" href=\"/media/watch/%X\">", f->id);
    /* pictures are for signed-in screens (display accounts too: /media/img only asks for a session);
       a page drawn for nobody (c->me NULL) gets drawn posters */
    uint64_t img = c->me ? media_picture(f) : 0;
    char title[300];
    media_title(f, title, sizeof title);
    poster_art_as(b, f->name.title, title, s ? 0 : f->name.year, NULL, img, media_percent(w), w && w->done);
    H(b, "<span class=\"media-mini-cap\">%s<small>", title);
    if (s) media_ep_code(b, f);
    else if (f->name.year) H(b, "%d", f->name.year);
    buf_puts(b, "</small></span></a>");
}

/* A series' new episodes as one small card ("70's Show · 8 new episodes"), as Watch groups them. */
static void mini_series(Ctx *c, Buf *b, const MediaSeries *s, int new_eps) {
    H(b, "<a class=\"media-mini\" href=\"/media/series/%X\">", s->id);
    poster_art(b, s->title, 0, T("Series"), c->me ? s->poster : 0, 0, false);
    H(b, "<span class=\"media-mini-cap\">%s<small>", s->title);
    H(b, TN(new_eps, "%d new episode", "%d new episodes"), new_eps);
    buf_puts(b, "</small></span></a>");
}

/* A "continue watching" card: the poster, and an x that drops the viewer's progress in it (the
   video then starts from the beginning, and leaves the list). Everyone else's progress stays.
   On a display account the x is there while someone is picked under "Who's watching?". */
static void going_card(Ctx *c, Buf *b, const MediaFile *f, bool small) {
    if (!ctx_viewer(c)) { /* nobody's progress to drop (a wall display only looks) */
        if (small) mini(c, b, f);
        else video_card(c, b, f);
        return;
    }
    H(b, "<div class=\"media-going\" id=\"media-going-%X\">", f->id);
    if (small) mini(c, b, f);
    else video_card(c, b, f);
    ui_form(b, "media.forget", "media-going-x");
    char title[300];
    media_title(f, title, sizeof title);
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><button title=\"%s\" aria-label=\"", f->id,
      T("Remove from Continue watching"));
    H(b, T("Remove %s from Continue watching"), title);
    buf_puts(b, "\">");
    ui_icon(b, "x");
    buf_puts(b, "</button>");
    ui_form_end(b);
    buf_puts(b, "</div>");
}

static void widget(Ctx *c, Buf *b, int where) {
    const MediaIndex *ix = media_lib_for(c);
    (void)where;
    if (ix->nroots == 0) {
        if (!is_admin(c)) { /* Library settings are an admin's: no "Set up" that leads nowhere (issue #58) */
            ui_card(b, T("Watch"), NULL, NULL);
            H(b, "<p class=\"muted\">%s ", T("Movies and series from the drive at home will show up here."));
            ask_admins(b, T("Ask %r to add the movie folders."));
            buf_puts(b, "</p>");
            ui_card_end(b);
            return;
        }
        ui_card(b, T("Watch"), "/media/settings", T("Set up"));
        H(b, "<p class=\"muted\">%s</p>",
          T("Point Hemma at the drive with your movies and series, and everyone can watch them on any screen at home — "
            "offline."));
        ui_card_end(b);
        return;
    }
    if (!ix->nrecent) return;
    const MediaFile *list[4];
    int n = media_in_progress(c, list, 4);
    bool resuming = n > 0;
    ui_card(b, resuming ? T("Continue watching") : T("Recently added"), "/media", T("Watch"));
    buf_puts(b, "<div class=\"media-minis\">");
    for (int i = 0; i < n; i++) going_card(c, b, list[i], true);
    if (!resuming) { /* a series' new episodes as one card, as Watch has them (issue #59: one per episode) */
        MediaRecent r[4];
        int nr = media_recent(c, r, countof(r));
        for (int i = 0; i < nr; i++) {
            if (r[i].n > 1) mini_series(c, b, r[i].series, r[i].n);
            else mini(c, b, r[i].f);
        }
    }
    buf_puts(b, "</div>");
    ui_card_end(b);
}

/* --------------------------------------------------------------- actions */

static void act_search(Ctx *c, Fields *f) { ctx_set_live(c, "media.q", fields_str(f, "q")); }

/* Watching actions (progress, watched, forget) are the viewer's - also on a display account, for
   whoever was picked there (Action.display); with nobody picked they quietly do nothing. */
static void act_progress(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int v = ctx_viewer(c);
    if (!v || !media_parse_id(fields_str(f, "id"), &id) || !media_file(id)) return;
    bool ended = fields_str(f, "ended")[0] == '1';
    if (media_watch_report(v, id, (int)fields_int(f, "pos", 0), (int)fields_int(f, "dur", 0), ended, c->now))
        app_changed(); /* finished: other screens' "continue watching" changes */
}

static void act_watched(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int v = ctx_viewer(c);
    if (!v) return;
    const MediaFile *mf = media_parse_id(fields_str(f, "id"), &id) ? media_file(id) : NULL;
    if (!mf) {
        ctx_fail(c, "%s", T("That video isn't in the library any more"));
        return;
    }
    bool done = fields_int(f, "done", 1) != 0;
    media_watch_set_done(v, id, done, c->now);
    ctx_ok(c, "%s", done ? T("Marked as watched") : T("Marked as not watched"));
}

static void act_rescan(Ctx *c, Fields *f) {
    if (!can_rescan(c)) {
        ctx_fail(c, "%s", T("Rescanning is for admins - ask one to rescan, or to let you do it"));
        return;
    }
    if (app_testing()) {
        media_scan_sync();
        ctx_ok(c, "%s", T("Library scanned"));
        return;
    }
    if (media_scanning()) {
        ctx_info(c, "%s", T("Already scanning — hang on"));
        return;
    }
    media_scan_start();
    ctx_info(c, "%s", T("Scanning your library…"));
}

static void act_art_remove(Ctx *c, Fields *f) {
    uint64_t id = 0;
    char dir[1024], stem[400];
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change pictures"));
        return;
    }
    if (!media_parse_id(fields_str(f, "id"), &id) || !media_art_folder(id, dir, sizeof dir, stem, sizeof stem)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    int n = media_art_prune(dir, stem, NULL);
    if (app_testing()) media_scan_sync();
    else media_scan_start();
    if (n) ctx_ok(c, "%s", T("Picture removed"));
    else ctx_info(c, "%s", T("There was no picture to remove"));
}

static void act_remove(Ctx *c, Fields *fl) {
    uint64_t id = 0;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can remove things from the library"));
        return;
    }
    const MediaFile *f = media_parse_id(fields_str(fl, "id"), &id) ? media_file(id) : NULL;
    if (!f) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    if (convert_queued(f->id)) {
        ctx_fail(c, "%s", T("It's being converted right now - stop that first (Library settings)"));
        return;
    }
    RemovePlan p = {0};
    if (!can_remove(c, f) || !media_remove_plan(f, &p)) {
        media_remove_free(&p);
        ctx_fail(c, "%s", T("Only films and other videos can be removed here"));
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
        ctx_fail(c, "%s", T("Couldn't delete it - is it playing somewhere? Try again in a moment."));
    } else {
        for (int i = tbl_count(&T_media_watch) - 1; i >= 0; i--) { /* everyone's progress in it */
            const MediaWatch *w = tbl_at(&T_media_watch, i);
            if (str_eq(w->item, hex)) tbl_del(&T_media_watch, w->id);
        }
        activity_add(c->me->id, "🗑️", movie ? "/media/movies" : "/media/browse", N_("removed %s from the library"), title);
        if (failed)
            ctx_info(c,
                     TN(failed, "Removed %s - %d leftover file couldn't be deleted",
                        "Removed %s - %d leftover files couldn't be deleted"),
                     title, failed);
        else ctx_ok(c, T("Removed %s - %s freed"), title, size);
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
        ctx_fail(c, "%s", T("Only an admin can delete originals"));
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
    char size[24], logged[24];
    Lang was = i18n_use(LANG_EN); /* (the log speaks English: "1.2 GB") */
    size_text(bytes, logged, sizeof logged);
    i18n_use(was);
    size_text(bytes, size, sizeof size);
    if (done) {
        log_info("media: %s kept only the most compatible versions: %d files deleted (%s)", c->me->name, done, logged);
        if (failed)
            ctx_info(c, TN(done, "Deleted %d file (%s) - %d couldn't be deleted", "Deleted %d files (%s) - %d couldn't be deleted"),
                     done, size, failed);
        else ctx_ok(c, TN(done, "Deleted %d file - %s freed", "Deleted %d files - %s freed"), done, size);
    } else if (failed) {
        ctx_fail(c, "%s", T("Couldn't delete them (in use, or no permission?)"));
    } else {
        ctx_info(c, "%s", T("Nothing to delete: no video has two versions"));
    }
    g_disks_at = 0; /* the sidebar's disk space: look again */
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

/* "Remove from Continue watching": the viewer's progress in the video goes (finished ones aren't listed). */
static void act_forget(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int who = ctx_viewer(c);
    if (!who) return;
    if (!media_parse_id(fields_str(f, "id"), &id)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    const MediaWatch *w = media_watch_get(who, id);
    if (w && !w->done) {
        ctx_undo(c, &T_media_watch, w->id); /* the card's x is instant: Undo brings the progress back (issue #60) */
        tbl_del(&T_media_watch, w->id);
    }
    const MediaFile *v = media_file(id);
    char what[300];
    if (v && v->name.kind == MEDIA_EPISODE)
        snprintf(what, sizeof what, "%s S%02dE%02d", v->name.title, v->name.season, v->name.episode);
    else if (v) media_title(v, what, sizeof what);
    if (v) ctx_ok(c, T("Removed %s from Continue watching"), what);
    else ctx_ok(c, "%s", T("Removed it from Continue watching"));
}

static void act_drop_auto(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    bool on = str_eq(fields_str(f, "on"), "1");
    setting_set_int("media.drop_originals", on);
    ctx_ok(c, "%s",
           on ? T("After each conversion only the most compatible version stays") : T("Both versions stay after converting"));
}

/* media.rescanners: p=<id> for each member who may rescan too (the whole list, every time). */
static void act_rescanners(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can choose who may rescan"));
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
    if (n) ctx_ok(c, TN(n, "%d member may rescan the library too", "%d members may rescan the library too"), n);
    else ctx_ok(c, "%s", T("Only admins may rescan the library"));
}

/* The library's folders, one per line (checked: each must be there): saved, then scanned. */
static void save_roots(Ctx *c, const char *s) {
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
            ctx_fail_at(c, "roots", T("Can't find the folder \"%s\" on the server"), line);
            buf_free(&keep);
            return;
        }
        if (++n > MEDIA_MAX_ROOTS) {
            ctx_fail_at(c, "roots", T("That's more than %d folders — point at a parent folder instead"), MEDIA_MAX_ROOTS);
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
    if (n) ctx_ok(c, TN(n, "Saved — scanning %d folder", "Saved — scanning %d folders"), n);
    else ctx_ok(c, "%s", T("Saved — no folders"));
}

static void act_roots(Ctx *c, Fields *f) {
    if (!is_admin(c)) { /* server-side folders: admins only */
        ctx_fail(c, "%s", T("Only an admin can change the library folders"));
        return;
    }
    save_roots(c, fields_str(f, "roots"));
}

/* A folder more or less from outside an action (a movie disk the disk helper mounted or forgot, drives.c):
   true = the folders changed (and a scan starts). */
bool media_root_add(const char *dir) {
    char roots[MEDIA_MAX_ROOTS][300];
    int n = media_roots(roots, MEDIA_MAX_ROOTS);
    if (n >= MEDIA_MAX_ROOTS || !path_is_dir(dir)) return false;
    Buf all = {0};
    for (int i = 0; i < n; i++) {
        if (same_folder(roots[i], dir)) {
            buf_free(&all);
            return false;
        }
        buf_printf(&all, "%s\n", roots[i]);
    }
    buf_puts(&all, dir);
    setting_set("media.roots", buf_cstr(&all));
    buf_free(&all);
    g_disks_at = 0;
    media_health_reset();
    if (app_testing()) media_scan_sync();
    else media_scan_start();
    return true;
}

bool media_root_drop(const char *dir) {
    char roots[MEDIA_MAX_ROOTS][300];
    int n = media_roots(roots, MEDIA_MAX_ROOTS);
    Buf keep = {0};
    bool dropped = false;
    for (int i = 0; i < n; i++) {
        if (same_folder(roots[i], dir)) {
            dropped = true;
            continue;
        }
        if (keep.len) buf_putc(&keep, '\n');
        buf_puts(&keep, roots[i]);
    }
    if (dropped) {
        setting_set("media.roots", keep.len ? buf_cstr(&keep) : "");
        g_disks_at = 0;
        media_health_reset();
        if (app_testing()) media_scan_sync();
        else media_scan_start();
    }
    buf_free(&keep);
    return dropped;
}

/* One folder more (a suggestion's one tap, issue #27) - the ones there stay. */
static void act_roots_add(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change the library folders"));
        return;
    }
    char dir[300], roots[MEDIA_MAX_ROOTS][300];
    str_copy(dir, sizeof dir, fields_str(f, "dir"));
    str_trim(dir);
    same_slashes(dir);
    int n = media_roots(roots, MEDIA_MAX_ROOTS);
    Buf all = {0};
    bool have = false;
    for (int i = 0; i < n; i++) {
        buf_printf(&all, "%s\n", roots[i]);
        have |= same_folder(roots[i], dir);
    }
    if (!dir[0] || have) {
        if (dir[0]) ctx_info(c, T("%s is in the library already"), dir);
        else ctx_fail(c, "%s", T("Pick a folder"));
        buf_free(&all);
        return;
    }
    buf_puts(&all, dir);
    save_roots(c, buf_cstr(&all));
    buf_free(&all);
}

/* Mark a folder "Not for children", or open it to them again (issue #44): from the Folders tab (p = "<library
   folder number>:<folder below it>", on), or Library settings' list (root + dir: a folder no longer in the
   library can be cleared too). */
static void act_kids(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    const MediaIndex *ix = media_lib();
    char root[300], dir[400];
    bool on = str_eq(fields_str(f, "on"), "1");
    const char *p = fields_str(f, "p");
    if (p[0]) {
        int r = (int)strtol(p, NULL, 10);
        const char *colon = strchr(p, ':');
        if (!colon || r < 0 || r >= ix->nroots) {
            ctx_fail(c, "%s", T("That folder isn't in the library"));
            return;
        }
        str_copy(root, sizeof root, ix->roots[r]);
        str_copy(dir, sizeof dir, colon + 1);
    } else {
        str_copy(root, sizeof root, fields_str(f, "root"));
        str_copy(dir, sizeof dir, fields_str(f, "dir"));
        on = false;
    }
    if (strstr(dir, "..") || dir[0] == '/' || strchr(dir, '\\')) {
        ctx_fail(c, "%s", T("That folder isn't in the library"));
        return;
    }
    media_kids_set(root, dir, on);
    const char *name = dir[0] ? strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir : root;
    ctx_ok(c, on ? T("%s is not for children now") : T("%s is open to children"), name);
}

static void act_prefs(Ctx *c, Fields *f) {
    char lang[16];
    str_copy(lang, sizeof lang, fields_str(f, "sublang"));
    pref_set(c->me->id, "media.sublang", lang);
    if (*lang) ctx_ok(c, T("Subtitles: %s"), lang_name(lang));
    else ctx_ok(c, "%s", T("Subtitles off by default"));
}

/* Watching is open to children and guests too (issue #44): searching, progress, ticks, their own prefs. */
static const Action actions[] = {
    {"media.search", act_search, true, .may = ACT_ALL}, {"media.progress", act_progress, true, true, ACT_ALL},
    {"media.watched", act_watched, false, true, ACT_ALL},
    {"media.rescan", act_rescan}, {"media.roots", act_roots}, {"media.roots_add", act_roots_add},       {"media.prefs", act_prefs, .may = ACT_ALL},
    {"media.upload_stop", upload_act_stop}, {"media.art_remove", act_art_remove},
    {"media.convert", convert_act_start},   {"media.convert_stop", convert_act_stop}, {"media.remove", act_remove},
    {"media.forget", act_forget, false, true, ACT_ALL}, {"media.kids", act_kids},
    {"media.drop_originals", act_drop_originals}, {"media.drop_auto", act_drop_auto},
    {"media.rescanners", act_rescanners}, {"media.parts_remove", act_parts_remove},
    {"media.history_forget", act_history_forget},
    {"media.drive_use", media_drives_act_use}, {"media.drive_format", media_drives_act_format},
    {"media.drive_forget", media_drives_act_forget}, {"media.drive_seen", media_drives_act_seen},
    {0},
};

/* ------------------------------------------------------------- lifecycle */

static void init(void) {
    T_media_watch.api_owner = "person"; /* /api: watch history is for its person (and admins) */
    T_media_watch.what = "watch history";
    store_register(&T_media_watch);
    media_kids_init();
}

/* Someone leaves (issue #45): moved out, what they watched stays with them; erased, it goes. */
static int forget(Forget *f) {
    int n = 0;
    for (int i = tbl_count(&T_media_watch) - 1; i >= 0 && !f->keep; i--) {
        const MediaWatch *w = tbl_at(&T_media_watch, i);
        if (w->person != f->person) continue;
        n++;
        if (f->apply) tbl_del(&T_media_watch, w->id);
    }
    if (n) forget_note(f, TN(n, "What they watched goes (%d video)", "What they watched goes (%d videos)"), n);
    return n;
}

static void start(void) {
    media_test_mounts(NULL); /* (a test's disks end with its boot) */
    media_stream_resume();
    upload_start();
    media_lib_start();
    media_health_reset();
    g_disks_at = 0; /* (a start measures afresh: a test before this one may have measured other folders) */
    media_drives_start(); /* (after the library: a disk the helper just mounted becomes a folder of it) */
}

static void stop(void) {
    media_drives_stop();
    media_stream_stop();
    upload_stop();
    convert_stop();
    media_lib_stop();
    media_kids_stop();
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
    decimal_mark(out);
}

/* (the numbers come from the tick; the words are the reader's, made here) */
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
        H(b, "<a class=\"media-disk%r\" href=\"/media/settings\" title=\"", level);
        H(b, T("Space on the disk with the library (%s)"), g_disks[i].name);
        H(b, "\"><span class=\"media-disk-l\"><span>%s%s%s</span><b>", T("Storage"), g_ndisks > 1 ? " · " : "",
          g_ndisks > 1 ? g_disks[i].name : "");
        H(b, T("%s free"), fr);
        H(b, "</b></span><span class=\"media-disk-bar\"><i style=\"width:%s\"></i></span><small>", width);
        H(b, T("%s used of %s"), us, tot);
        buf_puts(b, "</small></a>");
    }
}

static void tick(int64_t now) {
    media_lib_tick(now);
    upload_tick(now);
    convert_tick(now);
    media_health_tick(now);
    media_drives_tick(now);
    if (!g_disks_at || now - g_disks_at >= 30000) disks_refresh(now);
}

static bool http(Ctx *c, Conn *conn, HttpReq *r) { return upload_http(c, conn, r) || media_http(c, conn, r); }

static const Module media = {
    .id = "media",
    .title = N_("Watch"),
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
    .blocks = media_blocks,
    .forget = forget,
};
MODULE(media)
