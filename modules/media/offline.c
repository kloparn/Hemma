/* modules/media/offline.c — OFFLINE IN THE APP (2.3; owner, 2026-10-30: "a 'offline' section in the app, where you can
 * download videos/audiobooks and play them offline from the app"; who: "Grown-ups and children").
 *
 * Hemma's half: what the Android app needs to save something, and the buttons that ask it to.
 *  - GET /media/offline/<id>.json: a video's (its file id) or an audiobook's manifest - its title, picture, length, the
 *    viewer's place, its files (a video's ONE address with the viewer's sound: media_file_src), its text subtitles, a
 *    book's chapters. GET /media/offline/season/<series id>-<season>.json: that season's episodes' manifests, in order.
 *  - Who may: members, admins and children (a child only what's open to them - kids.c); guests and display accounts
 *    get 403, what isn't there (or isn't a child's) 404.
 *  - The buttons are plain links the app's WebView takes (MainActivity: hemma-app:save?m=<manifest>, hemma-app:offline),
 *    shown only inside the app (session_in_app) - a browser couldn't follow them.
 * The app downloads the files with the WebView's cookie and plays them with no Hemma in reach; what it watched comes
 * back by POST /a/media.progress, as any player's report. */
#include "../../core/json.h"
#include "../../core/ui.h"
#include "download.h"
#include "media.h"

#define OFFLINE_FORMAT 1 /* the manifests' "v" */

bool media_offline_ok(const Ctx *c) {
    return c->me && (c->me->role == ROLE_MEMBER || c->me->role == ROLE_ADMIN || c->me->role == ROLE_CHILD);
}

/* The screen is the app's (its links reach MainActivity), and not a TV page */
static bool in_app(const Ctx *c) { return media_offline_ok(c) && c->sess && session_in_app(c->sess) && !media_tv_mode(c); }

static void button(Buf *b, const char *manifest, const char *label) {
    H(b, "<a class=\"btn media-offline\" href=\"hemma-app:save?m=%u\">%s</a>", manifest, label);
}

void media_offline_video(Ctx *c, Buf *b, const MediaFile *f) {
    if (!in_app(c) || !f || f->type != MF_VIDEO) return;
    char m[48];
    snprintf(m, sizeof m, "/media/offline/%016llx.json", (unsigned long long)f->id);
    button(b, m, T("Save for offline"));
}

void media_offline_season(Ctx *c, Buf *b, const MediaSeries *s, int season) {
    if (!in_app(c) || !s) return;
    char m[64];
    snprintf(m, sizeof m, "/media/offline/season/%016llx-%d.json", (unsigned long long)s->id, season);
    button(b, m, season == 0 ? T("Save the specials for offline") : T("Save season for offline"));
}

void media_offline_book(Ctx *c, Buf *b, const MediaBook *bk) {
    if (!in_app(c) || !bk) return;
    char m[48];
    snprintf(m, sizeof m, "/media/offline/%016llx.json", (unsigned long long)bk->id);
    button(b, m, T("Save for offline"));
}

void media_offline_head(Ctx *c, Buf *b) {
    if (in_app(c)) H(b, "<a class=\"btn media-offline-go\" href=\"hemma-app:offline\">%s</a>", T("Offline"));
}

/* ---------------------------------------------------------------- manifests */

static void put_str(Buf *j, const char *key, const char *value) {
    buf_printf(j, ",\"%s\":", key);
    json_str(j, value ? value : "");
}

static void video_manifest(Ctx *c, const MediaFile *f, Buf *j) {
    const MediaFile *pf = media_playable(f);
    char title[300], hex[17];
    media_id_hex(f->id, hex);
    if (f->name.kind == MEDIA_EPISODE) str_copy(title, sizeof title, f->name.ep_title[0] ? f->name.ep_title : f->name.title);
    else media_title(f, title, sizeof title);
    if (f->name.kind == MEDIA_MOVIE && f->name.year) { /* "Spring (2019)" */
        size_t n = strlen(title);
        snprintf(title + n, sizeof title - n, " (%d)", f->name.year);
    }
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    Buf code = {0}, poster = {0};
    if (s) media_ep_code(&code, f);
    uint64_t pic = media_picture(f);
    if (pic) media_img_src(&poster, pic);
    const MediaWatch *w = media_watch_of(c, f);
    char src[160];
    media_file_src(c, f->id, src, sizeof src); /* (the viewer's sound when it has several: ?a=<n> - 2.2) */
    buf_printf(j, "{\"v\":%d,\"kind\":\"video\"", OFFLINE_FORMAT);
    put_str(j, "id", hex);
    put_str(j, "title", title);
    put_str(j, "series", s ? s->title : "");
    put_str(j, "code", buf_cstr(&code));
    put_str(j, "poster", buf_cstr(&poster));
    buf_printf(j, ",\"duration_ms\":%lld,\"resume_s\":%d,\"files\":[{\"url\":",
               (long long)(f->probe.duration_ms ? f->probe.duration_ms : pf->probe.duration_ms), w && !w->done ? w->pos : 0);
    json_str(j, src);
    buf_printf(j, ",\"bytes\":%lld}],\"subs\":", (long long)MAX(pf->size, 0));
    media_subs_json(c, f, j);
    buf_puts(j, ",\"chapters\":[]");
    put_str(j, "author", "");
    put_str(j, "narrator", "");
    buf_putc(j, '}');
    buf_free(&code);
    buf_free(&poster);
}

static void book_manifest(Ctx *c, const MediaBook *bk, Buf *j) {
    char hex[17];
    media_id_hex(bk->id, hex);
    Buf poster = {0};
    media_book_art_src(&poster, bk);
    int v = ctx_viewer(c);
    const MediaWatch *w = v ? media_watch_get(v, bk->id) : NULL; /* (a book's place: its first part's row, in book seconds) */
    buf_printf(j, "{\"v\":%d,\"kind\":\"book\"", OFFLINE_FORMAT);
    put_str(j, "id", hex);
    put_str(j, "title", bk->title);
    put_str(j, "series", "");
    put_str(j, "code", "");
    put_str(j, "poster", buf_cstr(&poster));
    buf_printf(j, ",\"duration_ms\":%lld,\"resume_s\":%d,\"files\":[", (long long)bk->duration_ms, w && !w->done ? w->pos : 0);
    for (int i = 0; i < bk->count; i++) {
        const MediaFile *p = media_book_part(bk, i);
        buf_printf(j, "%s{\"url\":\"/media/file/%016llx\",\"bytes\":%lld,\"ms\":%lld}", i ? "," : "", (unsigned long long)p->id,
                   (long long)MAX(p->size, 0), (long long)MAX(p->probe.duration_ms, 0)); /* (ms: the app needn't measure) */
    }
    buf_puts(j, "],\"subs\":[],\"chapters\":");
    media_book_chapters_json(bk, j);
    put_str(j, "author", bk->author);
    put_str(j, "narrator", bk->narrator);
    buf_putc(j, '}');
    buf_free(&poster);
}

/* A season's main episodes (as its page lists them), each by its manifest's address. false = none (or not theirs). */
static bool season_list(Ctx *c, const MediaSeries *s, int season, Buf *j) {
    if (!s || !media_kids_series_ok(c, s)) return false; /* (a child: a series with anything marked goes whole) */
    char title[200];
    if (season == 0) snprintf(title, sizeof title, "%s · %s", s->title, T("Specials"));
    else {
        char words[40];
        snprintf(words, sizeof words, T("Season %d"), season);
        snprintf(title, sizeof title, "%s · %s", s->title, words);
    }
    buf_printf(j, "{\"v\":%d,\"kind\":\"list\"", OFFLINE_FORMAT);
    put_str(j, "title", title);
    buf_puts(j, ",\"items\":[");
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        if (!e || e->name.season != season || !media_kids_ok(c, e)) continue;
        buf_printf(j, "%s\"/media/offline/%016llx.json\"", n++ ? "," : "", (unsigned long long)e->id);
    }
    buf_puts(j, "]}");
    return n > 0;
}

static void reply_json(Conn *conn, const Buf *j) {
    conn_reply(conn, 200, "application/json; charset=utf-8", j->p, j->len, "Cache-Control: no-store\r\n");
}

static void reply_text(Conn *conn, int status, const char *text) {
    Buf b = {0};
    buf_printf(&b, "%s\n", text);
    conn_reply(conn, status, "text/plain; charset=utf-8", b.p, b.len, NULL);
    buf_free(&b);
}

/* "<16 hex>.json" -> its id; false = not that */
static bool hex_json(const char *p, uint64_t *id) {
    char hex[17];
    if (strlen(p) != 21 || !str_eq(p + 16, ".json")) return false;
    memcpy(hex, p, 16);
    hex[16] = '\0';
    return media_parse_id(hex, id);
}

bool media_offline_http(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD")) {
        conn_reply(conn, 405, "text/plain; charset=utf-8", "GET only\n", 9, "Allow: GET, HEAD\r\n");
        return true;
    }
    if (!media_offline_ok(c)) { /* (guests and screens: the owner's "Grown-ups and children") */
        reply_text(conn, 403, T("Saving for offline isn't open to guests or screens."));
        return true;
    }
    const char *rest = r->path + strlen("/media/offline/");
    Buf j = {0};
    bool found = false;
    uint64_t id = 0;
    if (str_starts(rest, "season/")) { /* "season/<16 hex>-<season>.json" */
        const char *p = rest + 7, *dash = strchr(p, '-');
        char hex[17], *end = NULL;
        long season = dash ? strtol(dash + 1, &end, 10) : -1;
        if (dash && dash - p == 16 && end && end > dash + 1 && str_eq(end, ".json") && season >= 0 && season < 10000) {
            memcpy(hex, p, 16);
            hex[16] = '\0';
            found = media_parse_id(hex, &id) && season_list(c, media_series(id), (int)season, &j);
        }
    } else if (hex_json(rest, &id)) {
        const MediaFile *f = media_file(id);
        const MediaBook *bk = f && f->type == MF_VIDEO ? NULL : media_book(id);
        if (f && f->type == MF_VIDEO && f->role != MF_WEBCOPY && media_kids_ok(c, f)) {
            video_manifest(c, f, &j);
            found = true;
        } else if (bk && media_kids_book_ok(c, bk)) {
            book_manifest(c, bk, &j);
            found = true;
        }
    }
    if (found) reply_json(conn, &j);
    else reply_text(conn, 404, "not in the library");
    buf_free(&j);
    return true;
}
