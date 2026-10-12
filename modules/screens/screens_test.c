/* Screens: the seeded TV screen, the builder (admins only), rows that render or are left out,
 * "Who's watching?" on a display account and when it comes back, "Shows on", walls. Blocks come
 * from two test modules registered here (their output only while these tests run). */
#include "../../tests/harness.h"
#include "screens.h"

static bool g_on; /* the test modules' blocks draw only while these tests run (they stay registered) */

static bool on_render(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)arg;
    if (!g_on) return false;
    H(b, "<div class=\"tiles\"><a class=\"tile\" href=\"/zz\">zz-on kind %d viewer %d</a></div>", kind, ctx_viewer(c));
    return true;
}

static bool off_render(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)c, (void)kind, (void)arg;
    buf_puts(b, "zz-off-drawn"); /* drawn, but "nothing to show": the screen must drop it */
    return false;
}

static bool pick_render(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)c, (void)kind;
    if (!g_on) return false;
    H(b, "<p>zz-picked %s</p>", arg);
    return true;
}

static int pick_choices(Ctx *c, BlockChoice *out, int max) {
    (void)c;
    static const char *const args[] = {"one", "two"}, *const labels[] = {"First one", "Second one"};
    int n = 0;
    for (int i = 0; g_on && i < countof(args) && n < max; i++, n++) {
        str_copy(out[n].arg, sizeof out[n].arg, args[i]);
        str_copy(out[n].label, sizeof out[n].label, labels[i]);
    }
    return n;
}

static bool wall_render(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)c, (void)kind, (void)arg;
    if (!g_on) return false;
    buf_puts(b, "<p>zz-wall</p>");
    return true;
}

static bool secret_render(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)c, (void)kind, (void)arg;
    if (!g_on) return false;
    buf_puts(b, "<p>zz-secret</p>");
    return true;
}

static const Block zz_blocks[] = {
    {"zzscr.on", "Always on", SCREEN_TV | SCREEN_WALL, on_render, NULL},
    {"zzscr.off", "Never anything", SCREEN_TV, off_render, NULL},
    {"zzscr.pick", "Picked", SCREEN_TV, pick_render, pick_choices},
    {"zzscr.wall", "Wall only", SCREEN_WALL, wall_render, NULL},
    {0},
};
static const Module zz = {.id = "zzscr", .title = "Test rows", .icon = "tv", .blocks = zz_blocks};
static const Block adm_blocks[] = {{"zzscradm.secret", "Secret", SCREEN_TV, secret_render, NULL}, {0}};
static const Module zzadm = {.id = "zzscradm", .title = "Admin rows", .icon = "tv", .flags = MOD_ADMIN, .blocks = adm_blocks};

static void boot(void) {
    static bool done;
    if (!done) {
        module_register(&zz);
        module_register(&zzadm);
    }
    done = true;
    g_on = true;
    th_boot();
}

static void halt(void) {
    g_on = false;
    g_fake_now = 0;
    th_halt();
}

static int with_role(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    if (role == ROLE_DISPLAY) th_act(id, "screens.viewer", "who=0"); /* (the harness keeps a pick across tests) */
    return id;
}

/* A new screen by an admin: its id */
static int new_screen(int admin, const char *form) {
    ThResult r = th_act(admin, "screens.create", form);
    return r.ok && str_starts(r.redirect, "/screens/") ? (int)strtol(r.redirect + 9, NULL, 10) : 0;
}

static ThResult add(int admin, int screen, const char *block, const char *arg) {
    char form[160];
    snprintf(form, sizeof form, "id=%d&block=%s%s%s", screen, block, arg ? "&arg=" : "", arg ? arg : "");
    return th_act(admin, "screens.row_add", form);
}

/* The screen's rows as "block:arg:title|..." */
static const char *rows_of(int screen) {
    static char out[600];
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(screen, rows, SCREENS_MAX_ROWS);
    out[0] = '\0';
    for (int i = 0; i < n; i++)
        snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s:%s:%s", i ? "|" : "", rows[i]->block, rows[i]->arg,
                 rows[i]->title);
    return out;
}

static int row_id(int screen, int at) {
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(screen, rows, SCREENS_MAX_ROWS);
    return at < n ? rows[at]->id : 0;
}

static ThResult row_act(int admin, const char *action, int row, const char *more) {
    char form[200];
    snprintf(form, sizeof form, "row=%d%s", row, more ? more : "");
    return th_act(admin, action, form);
}

static int viewer_of(int account) {
    Ctx c;
    app_ctx(&c, account, "/");
    int v = ctx_viewer(&c);
    app_ctx_free(&c);
    return v;
}

TEST(screens_seed_a_tv_screen_once) {
    boot();
    CHECK_INT(tbl_count(&T_screen), 1);
    Screen *s = tbl_at(&T_screen, 0);
    CHECK_STR(s->name, "TV");
    CHECK_INT(s->kind, SCREEN_TV);
    /* the streaming-app rows, in this order - those whose module is here */
    static const char *const want[] = {"media.continue", "tv.onnow", "media.recent", "media.series", "media.movies"};
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(s->id, rows, SCREENS_MAX_ROWS), k = 0;
    for (int i = 0; i < countof(want); i++) {
        const Block *bl = block_find(want[i], NULL);
        if (!bl || !(bl->kinds & SCREEN_TV)) continue;
        CHECK(k < n);
        CHECK_STR(rows[k]->block, want[i]);
        k++;
    }
    CHECK_INT(n, k);
    /* a household that deleted it keeps none: no new TV screen at the next start */
    int adam = with_role("Adam", ROLE_ADMIN);
    char form[32];
    snprintf(form, sizeof form, "id=%d", s->id);
    CHECK(th_act(adam, "screens.delete", form).ok);
    CHECK_INT(tbl_count(&T_screen), 0);
    CHECK_INT(tbl_count(&T_screen_row), 0);
    th_halt();
    CHECK(app_boot(app_config()->data_dir));
    CHECK_INT(tbl_count(&T_screen), 0);
    halt();
}

TEST(screens_are_put_together_by_admins_only) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva"), tv = with_role("Living room TV", ROLE_DISPLAY);
    ThResult r = th_act(eva, "screens.create", "name=Kids");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Only an admin");
    r = th_act(tv, "screens.create", "name=Kids");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "view-only");
    CHECK(!th_act(adam, "screens.create", "name=+++").ok); /* a name it must have */
    int id = new_screen(adam, "name=Kids");
    CHECK(id > 0);
    CHECK_STR(screen_get(id)->name, "Kids");
    CHECK_INT(screen_get(id)->kind, SCREEN_TV);
    CHECK(add(adam, id, "zzscr.on", NULL).ok);
    /* every change is the admin's */
    static const char *const changes[] = {"screens.save", "screens.delete", "screens.row_add", "screens.row_save",
                                          "screens.row_move", "screens.row_remove", "screens.shows"};
    char form[120];
    snprintf(form, sizeof form, "id=%d&row=%d&name=Mine&block=zzscr.on&title=Mine&dir=up", id, row_id(id, 0));
    for (int i = 0; i < countof(changes); i++) {
        r = th_act(eva, changes[i], form);
        CHECK(!r.ok);
        CHECK_HAS(r.toast, "Only an admin");
        r = th_act(tv, changes[i], form);
        CHECK(!r.ok);
        CHECK_HAS(r.toast, "view-only");
    }
    CHECK_STR(screen_get(id)->name, "Kids");
    CHECK_STR(rows_of(id), "zzscr.on::");
    /* the pages: everyone sees the list and may open a screen; only admins edit */
    char url[40];
    snprintf(url, sizeof url, "/screens/%d/edit", id);
    const char *h = th_page(eva, url);
    CHECK_HAS(h, "Only an admin changes screens");
    CHECK(!strstr(h, "screens.row_add"));
    h = th_page(eva, "/screens");
    CHECK_HAS(h, "<b>Kids</b>");
    CHECK(!strstr(h, "New screen"));
    CHECK(!strstr(h, "/edit\""));
    CHECK_HAS(h, "<span>Screens</span>"); /* in the navigation */
    h = th_page(adam, "/screens");
    CHECK_HAS(h, "New screen");
    CHECK_HAS(h, url);
    CHECK_HAS(th_page(adam, "/screens?new=1"), "data-a=\"screens.create\"");
    h = th_page(adam, url);
    CHECK_HAS(h, "Add a row");
    CHECK_HAS(h, "id=\"screens-a-zzscr.on\"");
    CHECK(!strstr(h, "id=\"screens-a-zzscr.wall\"")); /* a wall's block: not for a TV */
    CHECK_HAS(th_page(0, "/screens"), "data-mod=\"auth\""); /* signed out: the sign-in page */
    halt();
}

TEST(screens_rows_are_added_moved_named_and_removed) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN);
    int id = new_screen(adam, "name=Kids");
    CHECK(add(adam, id, "zzscr.on", NULL).ok);
    CHECK(add(adam, id, "zzscr.pick", "two").ok);
    CHECK(add(adam, id, "zzscr.off", NULL).ok);
    CHECK(add(adam, id, "zzscr.pick", NULL).ok); /* no pick sent: the first on offer */
    CHECK_STR(rows_of(id), "zzscr.on::|zzscr.pick:two:|zzscr.off::|zzscr.pick:one:");
    ThResult r = add(adam, id, "zzscr.pick", "three");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "isn't on offer");
    CHECK(!add(adam, id, "zzscr.wall", NULL).ok); /* a wall's block on a TV */
    CHECK(!add(adam, id, "nobody.offers", NULL).ok);
    CHECK_STR(rows_of(id), "zzscr.on::|zzscr.pick:two:|zzscr.off::|zzscr.pick:one:");
    /* up, down - and nothing past either end */
    int on = row_id(id, 0), pick = row_id(id, 1), off = row_id(id, 2), last = row_id(id, 3);
    CHECK(row_act(adam, "screens.row_move", off, "&dir=up").ok);
    CHECK_STR(rows_of(id), "zzscr.on::|zzscr.off::|zzscr.pick:two:|zzscr.pick:one:");
    CHECK(row_act(adam, "screens.row_move", on, "&dir=up").ok);
    CHECK(row_act(adam, "screens.row_move", last, "&dir=down").ok);
    CHECK_STR(rows_of(id), "zzscr.on::|zzscr.off::|zzscr.pick:two:|zzscr.pick:one:");
    CHECK(row_act(adam, "screens.row_move", on, "&dir=down").ok);
    CHECK_STR(rows_of(id), "zzscr.off::|zzscr.on::|zzscr.pick:two:|zzscr.pick:one:");
    /* a heading of its own, another pick; a pick that isn't on offer changes nothing */
    CHECK(row_act(adam, "screens.row_save", on, "&title=+Cartoons+").ok);
    CHECK(row_act(adam, "screens.row_save", pick, "&title=&arg=one").ok);
    r = row_act(adam, "screens.row_save", pick, "&title=Oops&arg=zzz");
    CHECK(!r.ok);
    CHECK_STR(rows_of(id), "zzscr.off::|zzscr.on::Cartoons|zzscr.pick:one:|zzscr.pick:one:");
    CHECK(row_act(adam, "screens.row_remove", off, NULL).ok);
    CHECK(row_act(adam, "screens.row_remove", last, NULL).ok);
    CHECK_STR(rows_of(id), "zzscr.on::Cartoons|zzscr.pick:one:");
    CHECK(!row_act(adam, "screens.row_remove", off, NULL).ok); /* already gone */
    ScreenRow *rows[SCREENS_MAX_ROWS];
    CHECK_INT(screen_rows(id, rows, SCREENS_MAX_ROWS), 2);
    CHECK_INT(rows[0]->pos, 1);
    CHECK_INT(rows[1]->pos, 2);
    /* the screen shows them in that order, under their headings */
    char url[40];
    snprintf(url, sizeof url, "/screens/%d", id);
    const char *h = th_page(adam, url);
    CHECK_HAS(h, "class=\"bare\"");
    CHECK_HAS(h, "data-arrows");
    const char *a = strstr(h, "<h2>Cartoons</h2>"), *b = strstr(h, "<h2>Picked</h2>");
    CHECK(a && b && a < b);
    CHECK_HAS(h, "zz-picked one");
    /* the builder: heading and pick as they are */
    snprintf(url, sizeof url, "/screens/%d/edit", id);
    h = th_page(adam, url);
    CHECK_HAS(h, "value=\"Cartoons\" placeholder=\"Always on\"");
    CHECK_HAS(h, "<option value=\"one\" selected>First one</option>");
    CHECK_HAS(h, "<option value=\"two\">Second one</option>");
    halt();
}

TEST(screens_leave_out_rows_with_nothing_to_show) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva");
    int id = new_screen(adam, "name=Kids");
    CHECK(add(adam, id, "zzscr.off", NULL).ok);
    CHECK(add(adam, id, "zzscr.on", NULL).ok);
    CHECK(add(adam, id, "zzscradm.secret", NULL).ok);
    ScreenRow gone = {.screen = id, .pos = 9}; /* a module that isn't built in any more */
    str_copy(gone.block, sizeof gone.block, "gone.block");
    tbl_put(&T_screen_row, &gone);
    char url[40];
    snprintf(url, sizeof url, "/screens/%d", id);
    const char *h = th_page(eva, url);
    CHECK_HAS(h, "zz-on kind 1");
    CHECK(!strstr(h, "zz-off-drawn"));
    CHECK(!strstr(h, "Never anything"));
    CHECK(!strstr(h, "gone.block"));
    CHECK(!strstr(h, "zz-secret")); /* an admin section's row: admins only */
    CHECK(!strstr(h, "Nothing to show yet"));
    CHECK_HAS(th_page(adam, url), "zz-secret");
    /* the builder keeps the unknown row, marked, removable */
    snprintf(url, sizeof url, "/screens/%d/edit", id);
    h = th_page(adam, url);
    CHECK_HAS(h, "gone.block");
    CHECK_HAS(h, "screens-missing");
    CHECK_HAS(h, ">missing</span>");
    /* nothing at all to show */
    g_on = false;
    snprintf(url, sizeof url, "/screens/%d", id);
    h = th_page(eva, url);
    CHECK_HAS(h, "Nothing to show yet");
    CHECK(!strstr(h, "<section"));
    halt();
}

TEST(screens_ask_whos_watching_on_a_tv) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva"), tv = with_role("Living room TV", ROLE_DISPLAY);
    int id = new_screen(adam, "name=Kids");
    CHECK(add(adam, id, "zzscr.on", NULL).ok);
    char url[40], form[40];
    snprintf(url, sizeof url, "/screens/%d", id);
    const char *h = th_page(tv, url);
    CHECK_HAS(h, "<h1>Who's watching?</h1>");
    CHECK_HAS(h, "data-arrows");
    CHECK_HAS(h, "<b>Adam</b>");
    CHECK_HAS(h, "<b>Eva</b>");
    CHECK(!strstr(h, "<b>Living room TV</b>")); /* display accounts aren't people */
    CHECK(!strstr(h, "zz-on"));
    /* the TV picks; it may do only that here */
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(th_act(tv, "screens.viewer", form).ok);
    CHECK_INT(viewer_of(tv), eva);
    h = th_page(tv, url);
    CHECK(!strstr(h, "Who's watching?"));
    char want[40];
    snprintf(want, sizeof want, "zz-on kind 1 viewer %d", eva);
    CHECK_HAS(h, want);
    CHECK_HAS(h, "<b>Eva</b>");
    CHECK_HAS(h, ">Switch</button>");
    CHECK(!strstr(h, ">Edit</a>"));
    snprintf(form, sizeof form, "who=%d", tv);
    CHECK(!th_act(tv, "screens.viewer", form).ok); /* only a person */
    CHECK(!th_act(tv, "screens.viewer", "who=999").ok);
    CHECK_INT(viewer_of(tv), eva);
    /* Switch: ask again */
    CHECK(th_act(tv, "screens.viewer", "who=0").ok);
    CHECK_INT(viewer_of(tv), 0);
    CHECK_HAS(th_page(tv, url), "Who's watching?");
    /* people on their own devices never get asked: they watch as themselves */
    h = th_page(eva, url);
    CHECK(!strstr(h, "Who's watching?"));
    snprintf(want, sizeof want, "zz-on kind 1 viewer %d", eva);
    CHECK_HAS(h, want);
    CHECK(!strstr(h, "Switch"));
    CHECK_HAS(h, "href=\"/screens\">Exit</a>");
    snprintf(form, sizeof form, "who=%d", adam);
    CHECK(th_act(eva, "screens.viewer", form).ok);
    CHECK_INT(viewer_of(eva), eva);
    h = th_page(adam, url);
    snprintf(want, sizeof want, "zz-on kind 1 viewer %d", adam);
    CHECK_HAS(h, want);
    CHECK_HAS(h, ">Edit</a>");
    halt();
}

TEST(screens_ask_again_once_the_tv_sat_unused) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva"), tv = with_role("Living room TV", ROLE_DISPLAY);
    int id = new_screen(adam, "name=Kids");
    CHECK(add(adam, id, "zzscr.on", NULL).ok);
    int64_t t = dt_make(2026, 10, 12, 19, 0);
    g_fake_now = t;
    char url[40], form[40];
    snprintf(url, sizeof url, "/screens/%d", id);
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(th_act(tv, "screens.viewer", form).ok);
    g_fake_now = t + SCREENS_IDLE_S - 60;
    CHECK(!strstr(th_page(tv, url), "Who's watching?"));
    CHECK_INT(viewer_of(tv), eva);
    g_fake_now = t + SCREENS_IDLE_S + 60; /* four hours without anyone using it */
    CHECK_HAS(th_page(tv, url), "Who's watching?");
    CHECK_INT(viewer_of(tv), 0); /* the pick is over, for every page (the player saves nobody's progress) */
    halt();
}

/* The same rule for a TV's real session, from the module's tick: use keeps the pick, a long break
   ends it - even when the TV sits on another page. */
TEST(screens_tv_use_keeps_the_pick_and_a_break_ends_it) {
    boot();
    int eva = th_person("Eva"), tv = with_role("Living room TV", ROLE_DISPLAY);
    const Module *m = module_find("screens");
    CHECK(m && m->tick);
    int64_t t = dt_make(2026, 10, 12, 19, 0);
    Session s = {0};
    str_copy(s.token, sizeof s.token, "tv-session-1");
    s.person = tv;
    s.created = s.seen = t;
    s.viewer = eva;
    tbl_put(&T_session, &s);
    int sid = s.id;
    g_fake_now = t + 60;
    m->tick(0);
    CHECK_INT(((Session *)tbl_get(&T_session, sid))->viewer, eva);
    ((Session *)tbl_get(&T_session, sid))->seen = t + 3 * 3600; /* watching: progress reports, buttons */
    g_fake_now = t + 3 * 3600 + 10;
    m->tick(0);
    g_fake_now = t + 6 * 3600; /* six hours after the pick, three after the last use */
    m->tick(0);
    CHECK_INT(((Session *)tbl_get(&T_session, sid))->viewer, eva);
    g_fake_now = t + 7 * 3600 + 300;
    m->tick(0);
    CHECK_INT(((Session *)tbl_get(&T_session, sid))->viewer, 0);
    /* turned on again the next morning: that use comes after a long break - ask */
    Session s2 = {0};
    str_copy(s2.token, sizeof s2.token, "tv-session-2");
    s2.person = tv;
    s2.created = s2.seen = t;
    s2.viewer = eva;
    g_fake_now = t + 60;
    tbl_put(&T_session, &s2);
    m->tick(0);
    ((Session *)tbl_get(&T_session, s2.id))->seen = t + 14 * 3600;
    g_fake_now = t + 14 * 3600;
    m->tick(0);
    CHECK_INT(((Session *)tbl_get(&T_session, s2.id))->viewer, 0);
    halt();
}

TEST(screens_shows_on_picks_what_a_display_opens) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva");
    int tv1 = with_role("Living room TV", ROLE_DISPLAY), tv2 = with_role("Kitchen tablet", ROLE_DISPLAY);
    int a = new_screen(adam, "name=Evenings"), b = new_screen(adam, "name=Mornings");
    char form[120], ua[32], ub[32];
    screens_url(a, ua);
    screens_url(b, ub);
    CHECK(add(adam, b, "zzscr.on", NULL).ok);
    snprintf(form, sizeof form, "id=%d&acc=%d&acc=%d&acc=%d", a, tv1, tv2, eva);
    CHECK(th_act(adam, "screens.shows", form).ok);
    CHECK_STR(pref(tv1, PREF_START, ""), ua);
    CHECK_STR(pref(tv2, PREF_START, ""), ua);
    CHECK_STR(pref(eva, PREF_START, ""), ""); /* people keep their home page */
    /* one screen each: ticked on another, it moves */
    snprintf(form, sizeof form, "id=%d&acc=%d", b, tv2);
    CHECK(th_act(adam, "screens.shows", form).ok);
    CHECK_STR(pref(tv1, PREF_START, ""), ua);
    CHECK_STR(pref(tv2, PREF_START, ""), ub);
    char edit[40];
    snprintf(edit, sizeof edit, "/screens/%d/edit", a);
    const char *h = th_page(adam, edit);
    char want[80];
    snprintf(want, sizeof want, "name=\"acc\" value=\"%d\" checked> Living room TV", tv1);
    CHECK_HAS(h, want);
    CHECK_HAS(h, "Kitchen tablet <small class=\"muted\">shows Mornings now</small>");
    CHECK_HAS(th_page(adam, "/screens"), "on Kitchen tablet");
    /* the display's "/" is its screen - asking who's watching, there's no Exit from its own */
    h = th_page(tv2, "/");
    CHECK_HAS(h, "data-mod=\"screens\"");
    CHECK_HAS(h, "Who's watching?");
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(th_act(tv2, "screens.viewer", form).ok);
    h = th_page(tv2, "/");
    CHECK_HAS(h, "zz-on");
    CHECK(!strstr(h, ">Exit</a>"));
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(th_act(tv1, "screens.viewer", form).ok);
    CHECK_HAS(th_page(tv1, ub), ">Exit</a>"); /* someone else's screen: a way back */
    /* unticked: the usual home page again */
    snprintf(form, sizeof form, "id=%d", a);
    CHECK(th_act(adam, "screens.shows", form).ok);
    CHECK_STR(pref(tv1, PREF_START, ""), "");
    CHECK_STR(pref(tv2, PREF_START, ""), ub);
    /* a deleted screen lets go of its displays */
    snprintf(form, sizeof form, "id=%d", b);
    CHECK(th_act(adam, "screens.delete", form).ok);
    CHECK_STR(pref(tv2, PREF_START, ""), "");
    CHECK(!strstr(th_page(tv2, "/"), "data-mod=\"screens\""));
    halt();
}

TEST(screens_a_wall_shows_its_rows_as_cards) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), tablet = with_role("Hall tablet", ROLE_DISPLAY);
    int id = new_screen(adam, "name=Hall&kind=wall");
    CHECK(id > 0);
    CHECK_INT(screen_get(id)->kind, SCREEN_WALL);
    CHECK(add(adam, id, "zzscr.wall", NULL).ok);
    CHECK(add(adam, id, "zzscr.on", NULL).ok);
    CHECK(!add(adam, id, "zzscr.pick", NULL).ok); /* a TV's block */
    char url[40];
    snprintf(url, sizeof url, "/screens/%d", id);
    const char *h = th_page(tablet, url);
    CHECK(!strstr(h, "Who's watching?")); /* nothing personal on a wall */
    CHECK_HAS(h, "screens-wall");
    CHECK_HAS(h, "<section class=\"card screens-card\"");
    CHECK_HAS(h, "zz-wall");
    CHECK_HAS(h, "zz-on kind 2");
    snprintf(url, sizeof url, "/screens/%d/edit", id);
    h = th_page(adam, url);
    CHECK_HAS(h, "id=\"screens-a-zzscr.wall\"");
    CHECK(!strstr(h, "id=\"screens-a-zzscr.pick\""));
    halt();
}

TEST(screens_unknown_addresses_say_so) {
    boot();
    int eva = th_person("Eva");
    CHECK_HAS(th_page(eva, "/screens/999"), "That screen is gone");
    CHECK_HAS(th_page(eva, "/screens/abc"), "That screen is gone");
    char url[40];
    snprintf(url, sizeof url, "/screens/%d/nope", ((Screen *)tbl_at(&T_screen, 0))->id);
    CHECK_HAS(th_page(eva, url), "That screen is gone");
    CHECK_HAS(th_page(eva, "/screens/"), "<h1>Screens</h1>");
    halt();
}
