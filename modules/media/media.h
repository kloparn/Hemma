/* modules/media/media.h — what the Watch section shares.
 *
 * The first half is for neighbours: tv's channel pages live under Watch (its "Live TV" tab), so
 * they draw Watch's header and tabs, open the same TV-mode pages from a screen and draw tiles
 * with the library's pictures. tv includes this; media never includes tv (it shows tv's "On now"
 * row through block_find("tv.onnow")).
 *
 * The second half is module-internal: media.c (the pages) and tiles.c (the rows media offers to
 * screens, and the TV-mode pages) share it. */
#ifndef HEMMA_MEDIA_H
#define HEMMA_MEDIA_H

#include "../../core/app.h"
#include "library.h"

/* ------------------------------------------------------------ neighbours */

/* Watch's page header (title, subtitle, the scanning badge, Library settings) and its tabs:
   Home | Movies | Series | Live TV | Folders. active: "home" "movies" "series" "tv" "browse".
   buttons: trusted HTML for the header's right-hand side (NULL = none), e.g. "New channel".
   Live TV is there only while the tv module is. */
void media_lib_head(Ctx *c, Buf *b, const char *active, const char *title, const char *sub, const char *buttons);

/* TV mode (?tv=1): a page opened from a TV screen (modules/screens) - bare, always dark, big type,
   a Back link to ?back= first. Without tv=1 the same address is the ordinary page. */
bool media_tv_mode(const Ctx *c);
/* A path on this site to go back to: a leading single "/", no "//" anywhere, no backslashes,
   spaces or control characters, not too long - else "/". */
void media_safe_back(const char *in, char *out, size_t cap);
void media_tv_back(const Ctx *c, char *out, size_t cap); /* ?back=, made safe */
/* The opening of a TV-mode page (sets c->bare): the dark full-screen box, then the Back link -
   the first thing to focus. cls: an extra class for the box (NULL = none). Close with </div>. */
void media_tv_open(Ctx *c, Buf *b, const char *back, const char *cls);
/* A link (href value) from a screen's row or a TV-mode page: `path` as it is on an ordinary page
   (kind 0, e.g. Watch's own home), or its TV-mode page with the way back (any screen kind).
   path must have no query of its own. */
void media_tv_href(Buf *b, const char *path, int kind, const char *back);
/* Where a row's tiles lead back to: the screen showing them (c->path), made safe. */
void media_row_back(const Ctx *c, char *out, size_t cap);

/* The library's picture of a video: its own, else its series' (0 = none: draw one). */
uint64_t media_picture(const MediaFile *f);
void media_img_src(Buf *b, uint64_t img); /* "/media/img/<id>?v=<version>" */
/* A tile's art (web/app.css .tile-art): the picture, or a drawn poster in the title's colour
   showing `text` (NULL = the title; label: a small word over it, e.g. "Series"; NULL = none),
   with the progress bar along its foot when pct > 0. */
void media_tile_art(Buf *b, const char *title, const char *label, const char *text, uint64_t img, int pct);
/* style="--h:..;--h2:..;--h3:.." - a drawn poster's colours, from its title */
void media_art_style(Buf *b, const char *title);

/* ------------------------------------------------------- inside the module */

const MediaWatch *media_watch_of(const Ctx *c, const MediaFile *f); /* the viewer's progress (ctx_viewer) */
int media_percent(const MediaWatch *w);                            /* 0, or 1..100 watched */
int media_in_progress(const Ctx *c, const MediaFile **out, int max); /* the viewer's unfinished videos */
const MediaFile *media_next_up(const Ctx *c, const MediaSeries *s); /* what the viewer plays next in s */
int media_episodes_in(const MediaSeries *s); /* both halves of a "S01E01-E02" count */
void media_ep_code(Buf *b, const MediaFile *f); /* "S01E02", "S01E01–E02" */
void media_ep_title(Buf *b, const MediaFile *f); /* its title, or "Episode 3" */
void media_tracks(const Ctx *c, Buf *b, const MediaFile *f); /* <track>s of its subtitles */
/* A video's / a subtitle's address with its key (ctx_url_key), for src="...": a TV's video player
   fetches without the page's cookies. "/media/file/<id>?k=..." and "/media/sub/<id>.vtt?k=..." */
void media_file_src(const Ctx *c, uint64_t id, char *out, size_t cap);
void media_sub_src(const Ctx *c, uint64_t id, char *out, size_t cap);
/* The player's own controls (ui_player) for f inside #media-box over #media-video: its length, its
   subtitles in a menu (as media_tracks has them). tv: a remote's arrows move along the bar. */
void media_controls(const Ctx *c, Buf *b, const MediaFile *f, bool tv);
/* The "Next episode in N s" card over a player (wire.js [data-upnext]): shown in the video's last
   MEDIA_UPNEXT_S seconds, Play now (href: escaped already) and Hide; at the end the video goes on
   by itself. tv: the remote's OK plays now (Play now takes the focus, arrows move in the card). */
#define MEDIA_UPNEXT_S 30
void media_upnext(Buf *b, const MediaFile *next, const char *href, bool tv);

/* "Recently added", newest first, with a series' new episodes as one entry: episodes of one series
   that came within a week of each other ("70's Show · 8 new episodes"); films and other videos as
   themselves. n == 1 = that one video. */
typedef struct {
    const MediaFile *f;       /* the newest */
    const MediaSeries *series; /* a series' episodes (n > 1); NULL otherwise */
    int n;                     /* how many videos (episodes) the entry stands for */
    int64_t oldest;            /* mtime of the oldest of them */
} MediaRecent;
int media_recent(MediaRecent *out, int max);

/* TV mode's pages (tiles.c) and the rows media offers to screens */
void media_watch_tv(Ctx *c, Buf *b, const MediaFile *f);
void media_series_tv(Ctx *c, Buf *b, const MediaSeries *s);
extern const Block media_blocks[];

#endif
