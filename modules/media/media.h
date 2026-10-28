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
   Home | Movies | Series | Audiobooks | Live TV | Folders. active: "home" "movies" "series" "books" "tv" "browse".
   Audiobooks is there while the library has some (1.6).
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
/* Children (issue #44): what's in a folder an admin marked "Not for children" (kids.c) is left out for a
   child - signed in, or picked on a TV (media_kid). media_lib_for(c) = the library as c's viewer may see it
   (media_lib() for everyone else, and for NULL); media_kids_ok / media_kids_series_ok for whatever is opened
   by its id. A series with an episode in a marked folder goes whole. */
bool media_kid(const Ctx *c);
const MediaIndex *media_lib_for(const Ctx *c);
bool media_kids_ok(const Ctx *c, const MediaFile *f);
bool media_kids_series_ok(const Ctx *c, const MediaSeries *s);

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
/* Its title as shown: name.title, but a disc extra's in the reader's language ("70's Show – säsong 1, skiva 4:
   alla avsnitt"); out, returned. */
const char *media_title(const MediaFile *f, char *out, size_t cap);
void media_tracks(const Ctx *c, Buf *b, const MediaFile *f); /* <track>s of its subtitles */
/* A video's / a subtitle's address with its key (ctx_url_key), for src="...": a TV's video player
   fetches without the page's cookies. "/media/file/<id>?k=..." and "/media/sub/<id>.vtt?k=..." */
void media_file_src(const Ctx *c, uint64_t id, char *out, size_t cap);
/* ... plus "#t=<seconds>" when at > 0: the browser starts it there by itself (a media fragment), so a resume
   point and a live channel's "now" hold without wire.js too (issue #49) */
void media_file_src_at(const Ctx *c, uint64_t id, int64_t at, char *out, size_t cap);
/* Where w resumes, in seconds: its position - unless it's done, under 10 s in, or in its last 5 s (0 = from
   the top). wire.js keeps the same last-5-seconds rule for whatever data-resume says. */
int media_resume_at(const MediaWatch *w);
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
int media_recent(const Ctx *c, MediaRecent *out, int max); /* (what c's viewer may see: media_lib_for) */

/* The marks (Library settings, the Folders tab): a library folder and a folder below it ("" = all of it). */
bool media_kids_marked(const char *root, const char *dir);
const char *media_kids_covered(const char *root, const char *dir); /* a marked folder above dir, or NULL */
void media_kids_set(const char *root, const char *dir, bool on);
int media_kids_count(void);
bool media_kids_at(int i, char *root, size_t rcap, char *dir, size_t dcap);
void media_test_mounts(const char *list); /* tests: the disks offered for the library (";" between; NULL = none) */
void media_kids_init(void); /* (media's init and stop) */
/* A library folder more or less from outside an action (drives.c: a movie disk mounted or forgotten by the
   disk helper): true = the folders changed, and a scan starts. */
bool media_root_add(const char *dir);
bool media_root_drop(const char *dir);
void media_kids_stop(void);

/* CATEGORIES (cats.c, 1.4; owner, 2026-10-27: "increase the searching and usage of the series and movies"):
   a series or a film is a WORK - key "s<16 hex>" = the series' id (kept through a rename), "m<16 hex>" = a film's
   folded title + year hashed (the same film moved, or as several parts, keeps them). Its categories = the
   genres of its .nfo (MF_NFO) and of the online lookup (lookup.c), mapped onto the starters ("Animation" ->
   animated), plus what people ticked (table media_cat; a row with on = 0 takes one of those off). A category
   is a starter's key ("comedy": T() its name) or "+" and a name of the household's own ("+Pokémon"). */
#define MEDIA_CAT_MAX 16 /* a work's categories, at most */
#define MEDIA_CAT_LEN 48
typedef struct {
    char key[20];
    uint64_t nfo;
    const MediaSeries *s; /* one of these two */
    const MediaFile *f;
} MediaWork;
void media_work_series(const MediaSeries *s, MediaWork *w);
void media_work_film(const MediaFile *f, MediaWork *w);
void media_film_key(const char *title, int year, char out[20]); /* a film's key by its name (manage.c's Fix title) */
bool media_work_find(const char *key, MediaWork *w); /* a work of the whole library by its key */
int media_cats_of(const MediaWork *w, char out[][MEDIA_CAT_LEN], int max); /* in the order they're listed */
bool media_cat_in(const MediaWork *w, const char *cat);
const char *media_cat_label(const char *cat); /* in the reader's language (a starter's), or the household's name */
/* The categories c's viewer finds titles in (series and/or films, media_lib_for): with how many, most first. */
typedef struct {
    char cat[MEDIA_CAT_LEN];
    int series, films;
} MediaCatUse;
enum { MEDIA_CATS_SERIES = 1, MEDIA_CATS_FILMS = 2 };
int media_cats_used(const Ctx *c, int kinds, MediaCatUse *out, int max);
bool media_cat_parse(const char *q, char out[MEDIA_CAT_LEN]); /* a ?cat= value that names a category */
/* What a series' / film's page shows: its categories as links to list (`/media/series` or `/media/movies`), and -
   for whoever may tick them - a link to edit them (?cats=1 on `here`); the dialog itself when ?cats=1. */
void media_cats_line(Ctx *c, Buf *b, const MediaWork *w, const char *list, const char *here);
void media_cats_modal(Ctx *c, Buf *b, const MediaWork *w, const char *title, const char *close);
void media_cats_words(const MediaWork *w, Buf *out); /* its categories' names for searching, both languages */
void media_cats_move(const char *from, const char *to); /* a film renamed (manage.c): its ticks go along */
void media_cats_init(void);
void act_media_cat(Ctx *c, Fields *f);
void act_media_cat_new(Ctx *c, Fields *f);

/* LOOKING UP GENRES ONLINE (lookup.c, 1.4; owner: "if internet is available ... if the user has checked the box"):
   off by default (setting media.lookup), an admin's box in Library settings - greyed while this Hemma can't
   reach the services (checked when the page opens). Series from TVmaze (no key), films from OMDb with an admin's
   key (setting media.omdb_key) - both plain HTTP, the only thing Hemma itself ever fetches from the internet. */
const char *media_lookup_genres(const char *work); /* what was found, '|' between ("" = nothing yet) */
void media_lookup_card(Ctx *c, Buf *b);           /* Library settings */
/* Admins (owner, 2026-10-27: "press manage, and then maybe 'Update tags'? Where i paste the id"): what was found
   for it and where, and a field for its TVmaze or IMDb link or id - looked up by that, and kept so (pinned). */
void media_lookup_form(Ctx *c, Buf *b, const MediaWork *w);
void media_lookup_renamed(const char *from, const char *to); /* renamed: found by name = looked up again; pinned moves */
void media_lookup_init(void);
void media_lookup_start(void);
void media_lookup_tick(int64_t now);
void media_lookup_stop(void);
void act_media_lookup(Ctx *c, Fields *f);
void act_media_lookup_id(Ctx *c, Fields *f);
/* tests: what an answer gives (1 found, 0 nothing, -1 not understood); the scheduler's next ask (false = none);
   the internet check's result (1 reachable, -1 not, 0 not known); a found row */
int media_lookup_test_tvmaze(const char *json, const char *title, int year, char *genres, size_t cap);
int media_lookup_test_omdb(const char *json, char *genres, size_t cap);
bool media_lookup_test_next(char *host, size_t hcap, char *path, size_t pcap);
void media_lookup_test_online(int state);
void media_lookup_test_put(const char *work, const char *genres, bool pinned);

/* AUDIOBOOKS (books.c, 1.6): Watch's Audiobooks tab (/media/books), a book's page with its player
   (/media/book/<id>), the cards, and the viewer's books under way (Continue listening). */
bool media_kids_book_ok(const Ctx *c, const MediaBook *bk); /* (issue #44: as for a series - all of it, or none) */
void media_books_page(Ctx *c, Buf *b);
void media_book_page(Ctx *c, Buf *b, const char *idhex);
void media_book_card(Ctx *c, Buf *b, const MediaBook *bk);
int media_books_going(const Ctx *c, const MediaBook **out, int max); /* the viewer's unfinished books, latest first */
void media_books_shelf(Ctx *c, Buf *b); /* "Continue listening" (nothing when there's none) */
bool media_search_shown(Ctx *c, Buf *b); /* the library's search results, while a search is typed (media.c) */

/* TV mode's pages (tiles.c) and the rows media offers to screens */
void media_watch_tv(Ctx *c, Buf *b, const MediaFile *f);
void media_series_tv(Ctx *c, Buf *b, const MediaSeries *s);
extern const Block media_blocks[];

#endif
