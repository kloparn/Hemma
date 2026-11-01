/* modules/media/upload.h — admins add videos (and, 1.6, audiobooks) to the library from a browser.
 * Module-internal (upload.c); media.c wires the page, endpoint, action and tick. */
#ifndef HEMMA_MEDIA_UPLOAD_H
#define HEMMA_MEDIA_UPLOAD_H

#include "library.h"

bool upload_http(Ctx *c, Conn *conn, HttpReq *r); /* /media/upload/... endpoints */
void upload_page(Ctx *c, Buf *b);                 /* the /media/upload page */
void upload_summary(Ctx *c, Buf *b);              /* one-line status for the settings page */
/* Pictures for a series or film go the same way: the plan carries "art":"<id>" and one image,
   saved where media_art_target() says (replacing the old one). This renders how this person's
   latest picture for `id` is doing (nothing when there is none). */
void upload_art_status(Ctx *c, Buf *b, uint64_t id);
void upload_act_stop(Ctx *c, Fields *f);          /* action media.upload_stop */
/* Live action media.upload_preview (flagged live in media.c's table): the files chosen on the page
   ("files" = [[path, size], ...] as JSON, "all" = how many were chosen in all) and the form's options ->
   what the plan would do with them, kept for this screen (ctx_set_live "media.up") and drawn by
   upload_page(). No files: the screen's preview goes. */
void upload_act_preview(Ctx *c, Fields *f);
void upload_start(void);
void upload_tick(int64_t now_ms);
void upload_stop(void); /* module stop: running transfers give up (their .part files stay for resuming) */

/* "Show/Season 1/x.mkv" -> the same, every part checked (no "..", no names
   Windows can't store, valid UTF-8). False when anything is off. */
bool upload_clean_rel(const char *in, char *out, size_t cap);
void upload_test_free_space(int64_t bytes); /* tests: media_test_free_space (room.h) */
/* Tests (the harness has no screens, so no live state): upload_act_preview's preview from `form`
   (url-encoded, as the page's fields) becomes what th_page shows `person` on /media/upload; NULL
   forgets it. */
void upload_test_preview(int person, const char *form);
void upload_test_pace_window(int ms); /* tests: how long the processing speed is measured over (-1: as shipped) */
void upload_test_part_min(int64_t ms); /* (2.7) tests: a video made in parts on a device, parts this short (0: as shipped) */
/* (2.7) a part of a video a device is making in parts (convert_piece_path's name): a scan leaves it be */
bool upload_has_part(const char *path);

/* ------------------------------------------------------------------ sorting (upload_sort.c)
   Hemma sorts an upload by the files' names (owner, 2026-10-21): an episode goes into
   <series folder>/<Show>/Season N/, a film into <movies folder>/<Title (Year)>/ - into the folders
   the library already has for that show or film, next to what's there - and an audiobook's files into
   <books folder>/<their path>. The preview and the plan both ask upload_sort(), so they always agree. */
typedef enum {
    UPK_JUNK = 'j',    /* not a video, subtitle, picture or audio file (or a name a disk can't store) */
    UPK_EPISODE = 'e', /* a numbered episode: <show>/<season folder>/<its name> */
    UPK_DISC = 'd',    /* an episode known only by its folders (a disc rip): its path below the show folder */
    UPK_ASK = 'q',     /* an episode of a series nobody named: it goes as it was chosen */
    UPK_FILM = 'f',    /* <movies folder>/<Title (Year)>/<its name> */
    UPK_EXTRA = 'x',   /* a trailer, sample or bonus clip */
    UPK_OTHER = 'o',   /* a video that's neither an episode nor a film */
    UPK_SUB = 's',     /* subtitles */
    UPK_PIC = 'p',     /* a picture (a poster) */
    UPK_BOOK = 'b',    /* an audiobook's file (1.6: .m4b .m4a .mp3): <books folder>/<its path> */
    UPK_AUDIO = 'a',   /* an .m4a/.mp3 in a show's or film's own folder (a theme song, a soundtrack): goes along */
} UpKind;

typedef struct {
    char dest[400];   /* where it goes, below the library folder ('/' separators) */
    char kind;        /* UpKind */
    char title[128];  /* the show, film or audiobook (subtitles and pictures: their video's or book's, "" = none) */
    char ep_title[96];
    int year, season, episode, episode_end, disc;
    uint64_t show;    /* episodes: the name Hemma found, as a key (upload_show_key) - renames go by it */
    char found[128];  /* ... and that name as found in the files */
    bool known;       /* the library has this show or film already: it goes into that folder */
    bool kept;        /* goes as it was chosen (nothing told Hemma where else) */
    bool part;        /* an audiobook's .m4a/.mp3 among others in its book's folder (an .m4b is a whole book) */
    /* THE YEAR OF A FILM (2.4; owner, 2026-10-30: "almost mandatory ... Otherwise it's hard to identify stuff") */
    uint64_t film;    /* a film: its title as found, as a key (upload_show_key) - its "y.<hex>" year box goes by it */
    bool no_year;     /* a film whose name has no year: the preview asks for it (year = the one typed, else 0) */
    uint64_t group;   /* a video that isn't an episode or an extra: its top folder as chosen, as a key - "These are
                         films" ("films.<hex>") reads every video in there as a film; 0 = none */
    bool as_film;     /* a film by "These are films" (by its name it was another video) */
} UpSorted;

typedef struct {
    uint64_t key;    /* upload_show_key of the name found */
    char name[128];  /* what to call that series instead */
} UpName;

typedef struct {
    uint64_t key; /* UpSorted.film */
    int year;     /* typed on the preview */
} UpYear;

typedef struct {
    const char *root;     /* the library folder it goes into */
    uint64_t series;      /* /media/upload?series=: everything into this series' folder; 0 = by the names */
    const UpName *names;  /* series renamed on the preview */
    int nnames;
    const UpYear *years;  /* films' years typed on the preview (2.4) */
    int nyears;
    const uint64_t *films; /* top folders whose videos are films ("These are films", UpSorted.group) */
    int nfilms;
} UpSortOpts;

/* rels[i]: file i as chosen, cleaned (upload_clean_rel), or NULL (left out: a bad name). out[i] gets
   where it goes and what Hemma thinks it is. Reads the library's index and lists a few folders. */
void upload_sort(const UpSortOpts *o, const char *const *rels, int n, UpSorted *out);
uint64_t upload_show_key(const char *name); /* a series name, folded: "70's Show" = "70's show" */
/* A folder name made safe to store anywhere ("Star Wars: A New Hope" -> "Star Wars - A New Hope"):
   false when nothing is left of it. */
bool upload_safe_name(const char *in, char *out, size_t cap);
/* (2.6) the browser's converter is built in (av.wasm, conv.js), and a form's attributes that tell wire.js where - and
   (2.7) data-workers: how many videos this device makes at once, by its person's power tier (none while unknown) */
bool upload_can_convert(void);
void upload_converter_attrs(Ctx *c, Buf *b);
/* (2.6) the person ticked the upload form's "keep copies in memory" ("ram"): "On this device" does as they chose */
bool upload_ram_chosen(int person);
/* (2.7) the room a person's devices may take for copies under way (GB: the upload form's slider, pref media.upload_room;
   *lo / *hi = its bounds, may be NULL), and setting it (clamped) */
int upload_room_gb(int person, int *lo, int *hi);
void upload_set_room(int person, int gb);
/* (2.6) the person's own "On this device" batch as a card (while it runs, and a while after); false = none */
bool upload_here_card(Ctx *c, Buf *b, uint64_t video);

#endif
