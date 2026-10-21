/* core/ui.h — shared building blocks for pages. Everything here writes
 * escaped HTML into a Buf; the classes are styled by web/app.css.
 *
 * Interaction model (no JS needed, the wire just makes it live):
 *   - navigation and modals are plain links: a modal is "open" when the URL
 *     says so (e.g. /book?new=...), closing it links back;
 *   - every mutation is a <form> posting to /a/<action> (ui_form). */
#ifndef HEMMA_UI_H
#define HEMMA_UI_H

#include "app.h"

/* icons: home calendar list play note wifi users plus x left right trash clock monitor power search
 *        settings edit bell check arrow logo pin film tv sparkle folder pulse expand shrink
 *        go pause back10 fwd10 sound muted cc (the player's) */
void ui_icon(Buf *b, const char *name);

/* Avatars: emoji or initials on the person's colour. size: "xs" "sm" "md" "lg" "xl". */
void ui_avatar(Buf *b, const Person *p, const char *size);
/* The same picture for a spot where the name is printed right beside it: hidden from screen readers. */
void ui_avatar_deco(Buf *b, const Person *p, const char *size);
void ui_person(Buf *b, const Person *p); /* avatar + name chip */
void ui_hue_style(Buf *b, int hue);      /* style="--h:<hue>" */

/* Page header: <header class="ph"> with title/subtitle, then whatever you
 * append before ui_head_end() lands on the right (buttons, links). */
void ui_head(Buf *b, const char *title, const char *sub);
void ui_head_end(Buf *b);

void ui_card(Buf *b, const char *title, const char *more_href, const char *more_label);
void ui_card_end(Buf *b);
void ui_empty(Buf *b, const char *emoji, const char *title, const char *text);
/* Who a member asks for what only an admin can do (issue #58: "Set up" links led members nowhere):
   "Anna (admin)", "Anna or Jonas (admins)", "Anna, Jonas or Maja (admins)"; "an admin" with none that
   can sign in, or more than three. Escaped. */
void ui_admins(Buf *b);
/* A video player's own controls (wire.js [data-player], web/app.css .pl): drawn over the <video> inside
   `box` (position: relative; the element that goes full screen, so whatever else is drawn over the
   video shows there too). The same in every browser and for a TV's remote. It arrives hidden: without
   wire.js the video keeps the browser's controls (render it with `controls`; wire.js takes them off). */
typedef struct {
    const char *lang, *label; /* the <track>'s srclang, the words in the menu */
    bool on;                  /* shown now (the <track> is default) */
} UiSub;
typedef struct {
    const char *box, *video; /* ids */
    const char *key;         /* data-static: the video's own - a new one (the next episode) starts over */
    bool live;               /* a broadcast (video[data-onair]): a Live badge, sound, full screen only */
    int seconds;             /* its length, shown until the video knows (0 = unknown) */
    const UiSub *subs;       /* subtitle choices besides Off (none: no subtitles button) */
    int nsubs;
    bool focus_sound;        /* autofocus the sound button (a live channel on a TV: OK = sound) */
    bool tv;                 /* a remote: arrows move between the bar's buttons (else they're the video's) */
    const char *trouble;     /* shown when the browser can't play it (NULL: "This video won't play in this browser.") */
} UiPlayer;
void ui_player(Buf *b, const UiPlayer *p);

/* Forms */
void ui_form(Buf *b, const char *action, const char *cls); /* <form method=post action="/a/x" data-a="x"> */
/* Same, but the browser asks `question` first (e.g. "Remove this note?"). */
void ui_form_confirm(Buf *b, const char *action, const char *cls, const char *question);
void ui_form_end(Buf *b);
/* The role a page is drawn for (the core sets it around each render): a ui_form / ui_form_confirm / ui_act
   whose action that role may not run (app_action_open: a display account's view-only pages - issue #54 - a
   child's, a guest's) is left out whole, so a hallway tablet shows no quick-add boxes, ticks or x buttons
   that only answer "view-only", nor a child's page a form that says "Ask a grown-up". Forms written by hand
   aren't covered: use these helpers. A dialog (ui_modal) in which a form was cut goes whole too (issue #96). */
void ui_role(int role);
/* May this screen use that action? (app_action_open for the role drawn.) Ask it before drawing a link that
   opens a dialog for it ("?edit=", "?new=", a booking row), so a display account gets plain text instead of
   a link to nothing. */
bool ui_can(const char *action);
void ui_hidden(Buf *b, const char *name, const char *value);
void ui_hidden_int(Buf *b, const char *name, long long v);
/* One-button form: label is raw HTML (icons allowed); name/value may be NULL. */
void ui_act(Buf *b, const char *action, const char *cls, const char *label_html, const char *name, const char *value,
            const char *title);
void ui_person_select(Buf *b, const char *name, int selected, const char *none_label);
void ui_hue_picker(Buf *b, const char *name, int selected);

/* Modal driven by the URL. close_href is where "close" (and Escape) leads. */
void ui_modal(Buf *b, const char *title, const char *close_href);
void ui_modal_end(Buf *b);

/* Friendly time */
void ui_ago(Buf *b, int64_t t, int64_t now);           /* "just now" "5 min ago" "yesterday" "3 Oct" */
void ui_day(Buf *b, int64_t day, int64_t now);         /* "Today" "Tomorrow" "Fri 16 Oct" */
void ui_day_inline(Buf *b, int64_t day, int64_t now);  /* mid-sentence: "today" "tomorrow" "Friday" */
const char *const *ui_wire_texts(int *n); /* wire.js's own words (WIRE_TEXTS), for core/i18n_test.c */
void ui_hm(Buf *b, int64_t t);                         /* "14:30" */
void ui_span(Buf *b, int64_t start, int64_t end);      /* "14:00–17:00" (or with days when it spans) */
void ui_duration(Buf *b, int64_t seconds);             /* "1 h 25 min" */

/* Used by the core */
void ui_shell(Ctx *c, Buf *b, const Module *active, const char *page, size_t page_len);
void ui_more_page(Ctx *c, Buf *b); /* /more: every section as a big tile (phones: behind the tab bar's "More") */
void ui_document(Ctx *c, Buf *b, const char *app_html, size_t n, const char *flash, int flash_kind);

#endif
