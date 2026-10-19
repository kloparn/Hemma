/* core/app.h — everything a feature module gets from the core.
 *
 * A module is a folder under modules/ that defines a `Module` and registers
 * it with MODULE(). It renders pages with H() (html.h) and the components in
 * ui.h, keeps its data in store tables, and mutates data only inside Actions.
 * After an action the core re-renders every open screen and morphs the
 * difference into the browser — modules never write partial DOM updates.
 *
 * Pointer rule: rows returned by the store (Person*, Session*, ...) are only
 * valid until the next insert into that same table. Keep ids, not pointers. */
#ifndef HEMMA_APP_H
#define HEMMA_APP_H

#include "base.h"
#include "html.h"
#include "http.h"
#include "server.h"
#include "store.h"

/* The version: the VERSION file at the top of the repo ("0.14.0"; every PR raises it, merging releases
   it - docs/development.md). HEMMA_VERSION_FULL adds the commit the binary was built from: "0.6.0
   (8242ca8f1a2b)", "-dirty" with uncommitted changes; just the version outside git. core/version.c is
   the one file the build hands -DHEMMA_VER and -DHEMMA_COMMIT, so a new version or commit (or an edit
   that makes it "-dirty") recompiles that file only. */
const char *hemma_version(void);
#define HEMMA_VERSION hemma_version()
const char *hemma_version_full(void);
#define HEMMA_VERSION_FULL hemma_version_full()

/* ------------------------------------------------------------- people */

enum { ROLE_MEMBER = 0, ROLE_ADMIN = 1, ROLE_DISPLAY = 2 }; /* display = read-only (wall tablets) */

typedef struct {
    int id;
    char name[48];
    char emoji[16]; /* avatar emoji; initials are used when empty */
    int hue;        /* 0..359: the person's colour everywhere */
    int64_t created;
    char username[33]; /* lower-case sign-in name; "" = this person can't sign in (e.g. a toddler) */
    char pass[128];    /* password hash (core/auth.c); never leaves the server */
    int role;
    int64_t last_login;
} Person;

#define ADMIN_SESSION_DAYS 30 /* an admin signs in again after this, however much they use the session */
#define DISPLAY_SESSION_DAYS 90 /* ... a display account (the TV, a wall tablet) after this (owner, 2026-10-17: issue #90) */

typedef struct {
    int id;
    char token[65]; /* SHA-256 of the cookie value: the cookie itself is never stored */
    int person;
    int64_t created, seen;
    char agent[64]; /* "Chrome on Windows" — for the signed-in devices list */
    bool remember;
    int viewer; /* a display account (a TV): who is watching on it right now - 0 = nobody picked yet */
} Session;

extern Table T_person, T_session;
Person *person_get(int id);
/* The household's people (display accounts such as a wall tablet are not people). */
int person_count(void);
Person *person_at(int i);
Person *person_real(int id); /* person_get, but only a real person (not a display account): pickers' ids */
Person *person_find(const char *name); /* case-insensitive name, or unique prefix */
/* Every account, display ones included (household admin). */
int account_count(void);
Person *account_at(int i);

/* ------------------------------------------------------------ context */

typedef struct Client Client;
enum { TOAST_OK, TOAST_INFO, TOAST_ERR };

typedef struct Ctx {
    Session *sess;
    Person *me;     /* NULL until the visitor picked who they are */
    Client *client; /* live WebSocket client; NULL for plain HTTP */
    uint32_t ip;    /* visitor's IPv4 (host order) */
    int64_t now;
    char url[600];  /* current page: path + query, e.g. "/book?r=2" */
    char path[300]; /* decoded path, e.g. "/book" */
    Fields q;       /* query parameters */
    /* set by renderers */
    char title[96];
    bool bare; /* render without the app chrome (wall display, fullscreen player) */
    bool display_home; /* a display account's "/", shown as its start page: the browser's address is "/" */
    /* set by actions */
    char redirect[600];
    int toast_kind;
    char toast[240];
    bool failed;
    char fail_field[32]; /* ctx_fail_at(): the form field the error is about */
    uint64_t undo;       /* ctx_undo(): what this action removed can be put back - the toast's Undo */
} Ctx;

const char *ctx_q(const Ctx *c, const char *key); /* "" when absent */
long long ctx_qint(const Ctx *c, const char *key, long long dflt);
void ctx_title(Ctx *c, const char *fmt, ...) FMT(2, 3);
void ctx_ok(Ctx *c, const char *fmt, ...) FMT(2, 3);   /* success toast */
void ctx_info(Ctx *c, const char *fmt, ...) FMT(2, 3); /* neutral toast */
void ctx_fail(Ctx *c, const char *fmt, ...) FMT(2, 3); /* error toast; the form keeps its input */
/* An error about one field of the form ("password"): wire.js shows it next to that field until the field
   changes (issue #60: a toast said "Use at least 8 characters" over that very field, then vanished). A
   plain ctx_fail in a form of class "stack" shows above its buttons; elsewhere, and without wire.js, a toast. */
void ctx_fail_at(Ctx *c, const char *field, const char *fmt, ...) FMT(3, 4);
/* Keep row `id` of t - call it BEFORE tbl_del - so the success toast offers Undo for UNDO_S seconds,
   which puts it back as it was, id and all. More calls in one action add rows to the same Undo (issue #60:
   deleting was instant and final). Only the person who removed them can put them back. */
#define UNDO_S 30
void ctx_undo(Ctx *c, Table *t, int id);
void ctx_redirect(Ctx *c, const char *fmt, ...) FMT(2, 3);
/* Current URL with some query parameters replaced or removed:
 *   ctx_url(c, &b, "w", "2026-10-12", "new", NULL, NULL);   // NULL value removes, NULL key ends */
void ctx_url(const Ctx *c, Buf *out, ...);
/* An address key: some media players fetch a <video>'s file without the browser's cookies (a
   Samsung TV's browser since an update in 2026), so a module endpoint's address can carry its own
   proof - "<path>?k=<key>". The key holds for exactly that path, as this screen's session, from this
   screen's network address, for about two days (and never past the session); it opens module
   endpoints (http hooks) only - never pages, actions, the API or the wire. "" when there's no
   stored session (signed out; the test harness's pages). */
#define CTX_KEY_LEN 64
void ctx_url_key(const Ctx *c, const char *path, char out[CTX_KEY_LEN]);
bool app_action_display(const char *name); /* a display account may run it (Action.display): ui_form asks */

/* Sessions (core/auth.c signs people in; the household module manages them). */
bool session_create(int person, bool remember, const char *user_agent, char sid_out[33]);
void session_end(const char *sid);                 /* by cookie value */
int sessions_drop(int person, const Session *keep); /* sign a person out everywhere (but `keep`) */
static inline bool is_admin(const Ctx *c) { return c->me && c->me->role == ROLE_ADMIN; }
/* Per-screen scratch state for live previews (survives re-renders of that screen). */
const char *ctx_live(const Ctx *c, const char *key);
void ctx_set_live(Ctx *c, const char *key, const char *val);
/* Whose personal media state (progress, "continue watching") applies: the signed-in person -
   or, on a display account such as the living-room TV, the person picked under "Who's
   watching?" on it (0 = nobody picked yet). ctx_set_viewer picks (display accounts only). */
int ctx_viewer(const Ctx *c);
void ctx_set_viewer(Ctx *c, int person);
#define PREF_START "start" /* display accounts: the page their "/" shows, e.g. "/screens/2" */
/* ... and a display account without one: the household's wall (issue #53: a hall tablet landed on
   "Good morning, Hallway tablet"). modules/screens keeps it - its first wall's address, or "". */
#define SETTING_DISPLAY_START "display.start"

/* ------------------------------------------------------------ modules */

typedef struct {
    const char *name; /* global: "<module>.<verb>", e.g. "book.create" */
    void (*run)(Ctx *c, Fields *f);
    bool live; /* may run from a quiet "live" message ([data-live] previews, video[data-report]): no
                  ack, and other screens aren't re-rendered unless the action calls app_changed()
                  itself. A "live" message naming any other action runs nothing (issue #8). */
    bool display; /* a display account (a TV, a wall tablet) may run it too - watching on behalf of
                     its viewer (ctx_viewer): progress, picking who's watching. Everything else
                     stays refused for them ("This screen is view-only"). */
} Action;

/* Screens an admin puts together - the TV view, a wall tablet - are made of blocks that the
   modules offer; modules/screens arranges them. A block draws one row's content. */
enum { SCREEN_TV = 1, SCREEN_WALL = 2 };
typedef struct {
    char arg[64];   /* what the block will show, e.g. a series id */
    char label[96]; /* how the builder lists it, e.g. "70's Show" */
} BlockChoice;
typedef struct {
    const char *id;    /* global "<module>.<name>", e.g. "media.continue" */
    const char *title; /* in the builder, and the row's heading unless the admin names it */
    int kinds;         /* SCREEN_TV | SCREEN_WALL: the screens it suits */
    /* The row's content, without a heading (the screen adds that): big focusable tiles on a TV.
       false = nothing to show right now - the screen leaves the row out. arg: the BlockChoice
       picked in the builder ("" for blocks without choices). Personal blocks use ctx_viewer().
       kind 0 = an ordinary page borrowing the row (Watch's home shows "tv.onnow"): plain links,
       not the TV-mode ones a screen gets. */
    bool (*render)(Ctx *c, Buf *b, int kind, const char *arg);
    /* Optional: what it can show (one series out of the library...); the builder offers a pick. */
    int (*choices)(Ctx *c, BlockChoice *out, int max);
} Block;

/* Shared agenda: anything with a time (bookings, events, due chores) can show
 * up in other modules' calendars and overviews without them knowing each other. */
typedef struct {
    int64_t start, end; /* end == start for a moment in time */
    bool allday;
    char title[120];
    char icon[16]; /* emoji */
    char link[160];
    int who;            /* person, 0 = everyone */
    char kind[24];      /* what it is, shown small ("Booking"); empty = the module's title */
    const char *module; /* filled in by agenda_collect() */
} AgendaItem;

typedef struct {
    AgendaItem *items;
    int n, cap;
} Agenda;

void agenda_add(Agenda *a, const AgendaItem *it);
/* Everything overlapping [from, to) from every module except `except` (NULL = all), sorted by start. */
void agenda_collect(Ctx *c, int64_t from, int64_t to, const char *except, Agenda *out);
void agenda_free(Agenda *a);

/* "Is everything working?" (the System page, admins only): modules report checks in plain
   household words, plus steps for the getting-started list. A health hook runs whenever an
   admin looks (also the home dashboard): keep it quick - cached facts, no disk scans. */
enum {
    HEALTH_OK,   /* working */
    HEALTH_INFO, /* worth knowing, nothing wrong (e.g. "discovery is off") */
    HEALTH_WARN, /* needs a look */
    HEALTH_BAD,  /* a problem: something doesn't work */
};
typedef struct {
    int state;          /* HEALTH_*; a step: HEALTH_OK = done, HEALTH_INFO = still to do */
    bool step;          /* a getting-started step rather than a check */
    const char *module; /* set by health_collect() */
    char title[80];     /* "Library folders" */
    char detail[320];   /* how it is */
    char todo[240];     /* what to do about it, one line ("" = nothing) */
    char link[160];     /* where to do it ("" = nowhere) */
    char tech[240];     /* the flag, variable or command behind it, for whoever runs the server: shown folded
                           under "Technical details" (issue #62: plain words first) - health_tech() */
} HealthItem;
/* A disk a module keeps things on (issue #95: two checks with two rules said "OK" and "nearly full"
   about the same disk). The System page makes ONE "Disk space" row per disk - the data folder's
   included - naming what's on it, held to the biggest wish. Cached numbers, like any health hook. */
typedef struct {
    const char *what;          /* "the library" */
    const char *path;          /* the folder on that disk */
    int64_t free, total;       /* bytes */
    bool data_disk;            /* Hemma's data folder is on it too (the module compared them) */
    int64_t want;              /* free space it wants (0 = none) ... */
    const char *why;           /* ... for this: "to convert or upload a video as big as its biggest (6.0 GB)" */
    const char *todo, *link;   /* what to do when there's less */
} HealthDisk;
typedef struct {
    char what[80], path[300], why[200], todo[240], link[160];
    int64_t free, total, want;
    bool data_disk;
    const char *module; /* set by health_collect() */
} HealthDiskRow;
typedef struct {
    HealthItem *items;
    int n, cap;
    HealthDiskRow *disks; /* health_disk(): the System page turns them into its "Disk space" rows */
    int ndisks, capdisks;
} Health;
void health_add(Health *h, int state, const char *title, const char *detail, const char *todo, const char *link);
void health_tech(Health *h, const char *tech); /* the technical detail of the item added last */
void health_disk(Health *h, const HealthDisk *d);
void health_step(Health *h, bool done, const char *title, const char *detail, const char *link);
void health_collect(Ctx *c, Health *out); /* every module's health hook, in navigation order */
int health_worst(const Health *h);        /* the worst check (steps don't count); HEALTH_OK when none */
void health_free(Health *h);

enum { WIDGET_HOME }; /* a widget's `where`: the home dashboard (walls are screens of blocks now) */
/* MOD_PUBLIC: usable without picking a person.
   MOD_ADMIN: only admins see it in the navigation, open its pages or reach its http hook
   (others get "not found"); its actions still check is_admin themselves. */
enum { MOD_PUBLIC = 1, MOD_ADMIN = 2 };

typedef struct Module {
    const char *id;    /* URL prefix "/<id>"; the module with id "home" owns "/" */
    const char *title; /* navigation label and default page title */
    const char *icon;  /* ui_icon() name */
    int nav;           /* position in the navigation, 0 = not listed */
    int flags;
    void (*init)(void);           /* register tables — runs before the store loads */
    void (*start)(void);          /* after the store loaded */
    void (*stop)(void);
    void (*tick)(int64_t now_ms); /* ~10 Hz on the event loop: keep it quick */
    void (*page)(Ctx *c, Buf *b); /* render /<id> and /<id>/... */
    const Action *actions;        /* terminated by {0} */
    bool (*http)(Ctx *c, Conn *conn, HttpReq *r); /* raw endpoints (files, streams); true = handled */
    void (*widget)(Ctx *c, Buf *b, int where);    /* a card for the home dashboard (WIDGET_HOME) */
    int widget_order;
    void (*agenda)(Ctx *c, int64_t from, int64_t to, Agenda *out); /* timed things for shared agendas */
    /* A small block at the foot of the sidebar, above the profile (phones: on the More page).
       Every page shows it: keep it quick (cached numbers, no disk or network work). */
    void (*side)(Ctx *c, Buf *b);
    /* Checks and getting-started steps for the System page (health_add / health_step above).
       Only called for admins; cached facts only. */
    void (*health)(Ctx *c, Health *h);
    const Block *blocks; /* what it offers to screens (the TV view, walls); terminated by {0} */
    /* Not in the navigation itself (nav 0) but part of another section: its pages light up that
       module's entry - tv's channels live under Watch ("media"). */
    const char *parent;
} Module;

/* MOD_ADMIN modules exist only for admins (see the flags above). */
static inline bool module_visible(const Module *m, const Ctx *c) {
    return m && (!(m->flags & MOD_ADMIN) || is_admin(c));
}

const Block *block_find(const char *id, const Module **owner); /* NULL when no module offers it */

void module_register(const Module *m);
#define MODULE(var)                                                                                                    \
    __attribute__((constructor)) static void module_register_##var(void) { module_register(&var); }
int module_count(void);
const Module *module_at(int i); /* sorted by nav, then id */
const Module *module_find(const char *id);

/* ---------------------------------------------------- live propagation */

void app_changed(void); /* data changed: re-render every open screen (diffed; cheap) */
long app_renders(void); /* live screens rendered so far (tests and measurements) */
void app_changed_where(const char *url_prefix); /* only screens showing e.g. "/media" (progress bars) */
/* Ticks come about once a second while screens are open and every 5 s when
   none are. A module that needs its tick sooner (pacing packets, polling a
   worker) asks from its tick; the request lasts for one tick. */
void app_wake_in(int ms);
/* Any thread: tick (modules' tick hooks) right away - a worker finished something to show. */
void app_tick_soon(void);
/* Toast on every open screen except the one belonging to `except` (may be NULL). */
void app_notify(const Ctx *except, int kind, const char *fmt, ...) FMT(3, 4);
/* ... and not on these people's either (0 = nobody): they get a toast of their own from app_notify_person
   ("Adam cancelled your Laundry booking" instead of "Adam cancelled Laundry" - issue #43). */
void app_notify_but(const Ctx *except, int skip_a, int skip_b, int kind, const char *fmt, ...) FMT(5, 6);
/* Toast only on one person's open screens (e.g. "Adam assigned you: Vacuum"). */
void app_notify_person(int person, int kind, const char *fmt, ...) FMT(3, 4);

/* ------------------------------------------------------------ presence */

int presence_screens(int person);       /* open screens right now */
/* Title of the page they are looking at, or NULL. Navigating re-renders only that screen
   plus screens on "/" and "/people"; elsewhere this is only fresh after the minute tick -
   so show it on those two pages (the home dashboard does), not in shared chrome. */
const char *presence_page(int person);
void presence_set_home(int person, bool home, int64_t since); /* fed by the net module */
bool presence_is_home(int person, int64_t *since);
/* Drop what is known about `person` (0 = everyone), e.g. when discovery is
   switched off or their last device is unassigned: they show as unknown again. */
void presence_forget(int person);
bool presence_tracking(void); /* true once a module reports home/away */
bool presence_known(int person); /* has anyone reported home/away for this person? (e.g. they have a device) */
/* "Whose device is this?" — a module that knows devices (net) can answer. */
void presence_set_guesser(int (*guess)(uint32_t ip));
int presence_guess(uint32_t ip); /* person id or 0 */

/* ------------------------------------------------------------ settings */

const char *setting(const char *key, const char *dflt);
long long setting_int(const char *key, long long dflt);
void setting_set(const char *key, const char *val);
void setting_set_int(const char *key, long long v);
/* store_open rescued a damaged database (store_rescue_report): the report, kept until an admin
   presses "Got it" on the System page (system.rescued_seen) - a notice on admins' pages till then. */
#define SETTING_RESCUED "store.rescued"
/* ... and when it put back a backup an admin chose (store_restore_report), until "Got it" (system.restored_seen) */
#define SETTING_RESTORED "store.restored"
/* per-person preferences (theme, favourite resource, wall layout, ...) */
const char *pref(int person, const char *key, const char *dflt);
void pref_set(int person, const char *key, const char *val);

/* ------------------------------------------------------------ activity */

typedef struct {
    int id;
    int64_t ts;
    int who;
    char icon[16]; /* emoji */
    char text[200];
    char link[160];
} Activity;
extern Table T_activity;
void activity_add(int who, const char *icon, const char *link, const char *fmt, ...) FMT(4, 5);

/* --------------------------------------------------------------- misc */

bool app_testing(void); /* true under tests: don't open sockets, scan disks or start threads */
void app_set_testing(bool on);
const char *app_name(void);        /* household name, default "Hemma" */
const char *app_asset_version(void); /* changes whenever web/ assets change */
bool app_dev_mode(void);

/* Bring the core up without a server (used by app_main and tests/harness.c):
 * registers core tables, runs module init hooks, opens the store, runs start hooks. */
bool app_boot(const char *data_dir);
void app_halt(void);                       /* stop hooks + close the store */
void app_ctx(Ctx *c, int person, const char *url); /* context as a person (0 = anonymous visitor) */
void app_ctx_free(Ctx *c);
void app_render(Ctx *c, Buf *out);         /* the <div id="app"> for c->url */
bool app_action(Ctx *c, const char *name, Fields *f); /* false if no such action */
void app_serve(void); /* after app_boot + server_listen: run the event loop until server_stop() */

/* Entry points used by main.c */
typedef struct {
    const char *host;
    int port;
    const char *data_dir;
    const char *web_dir; /* serve web/ from disk (live CSS/JS editing + auto-reload); NULL = embedded */
    const char *env_file; /* the settings file (--env, default ".env"): a new install's secret goes there */
    const char *demo_password; /* hemma --demo: anna's (and jonas's) password; NULL = no demo */
} AppConfig;
int app_main(const AppConfig *cfg);
/* hemma --demo (issue #31; main.c makes the throwaway folder and its settings file): every page says
   it's a demo, the sign-in page gives the login, and the first boot fills a made-up household
   (core/demo.c: app_demo_seed, through the modules' own actions). */
bool app_demo(void);
const char *app_demo_password(void);
void app_demo_seed(const char *password); /* once per data folder (setting demo.seeded); needs "anna" */
void app_test_demo(const char *password); /* tests: as if started with --demo (NULL: not) */
/* app_main's exit code for a start refused over the data (store_refused: a newer Hemma's format, a
   rescue that got next to nothing back) - starting again won't change it, so deploy/hemma.service
   doesn't restart on it (RestartPreventExitStatus=78; 78 = EX_CONFIG in sysexits.h). */
#define HEMMA_EXIT_DATA 78
/* ... and for "start me again" (a backup to put back waits: store_stage_restore): systemd
   (Restart=on-failure) and launchd (KeepAlive) do, within seconds. 75 = EX_TEMPFAIL. */
#define HEMMA_EXIT_RESTART 75
/* Ends the event loop: app_main then stops the modules and returns `code` (thread safe). */
void app_quit(int code);
/* How this instance was started (any field may be NULL/0 under tests); data_dir is always the
   folder app_boot opened. */
const AppConfig *app_config(void);
/* The addresses other devices at home open, e.g. "http://192.168.1.50:8080" and
   "http://hemma.local:8080" (no port when it's 80) - or the one address Hemma listens on.
   Asks the OS (the network interface, the machine's name): call it at start or from a tick,
   not on every render. Returns how many were written. */
int app_addresses(char out[][96], int max);
/* Hemma listens on this computer only (--host 127.x or localhost): no other device can reach it. */
bool app_local_only(void);
void app_test_host(const char *host); /* tests: as if started with --host (NULL: none) */

#endif
