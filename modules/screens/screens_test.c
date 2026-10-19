/* Screens: the seeded TV screen, the builder (admins only), rows that render or are left out,
 * "Who's watching?" on a display account and when it comes back, "Shows on", walls. Blocks come
 * from two test modules registered here (their output only while these tests run). */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "screens.h"
#include "../../core/auth.h"

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

/* Does the screen hold `want` - those of them offered for its kind - in that order, and nothing else? */
static bool holds(int screen, int kind, const char *const *want, int nwant) {
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(screen, rows, SCREENS_MAX_ROWS), k = 0;
    for (int i = 0; i < nwant; i++) {
        const Block *bl = block_find(want[i], NULL);
        if (!bl || !(bl->kinds & kind)) continue;
        if (k >= n || !str_eq(rows[k]->block, want[i])) return false;
        k++;
    }
    return n == k;
}

static void delete_screen(int admin, int id) {
    char form[32];
    snprintf(form, sizeof form, "id=%d", id);
    CHECK(th_act(admin, "screens.delete", form).ok);
}

TEST(screens_seed_a_tv_and_a_wall_once) {
    boot();
    CHECK_INT(tbl_count(&T_screen), 2);
    int tv = screens_first(SCREEN_TV), wall = screens_first(SCREEN_WALL);
    CHECK(tv && wall);
    CHECK_STR(screen_get(tv)->name, "TV");
    CHECK_STR(screen_get(wall)->name, "Wall");
    /* the streaming-app rows; the day first on the wall - those whose module is here */
    static const char *const tv_rows[] = {"media.continue", "tv.onnow", "media.recent", "media.series", "media.movies"};
    static const char *const wall_cards[] = {"plan.today", "book.now", "plan.days", "plan.shop",
                                             "board.notes", "tv.onnow", "media.recent", "home.latest"};
    CHECK(holds(tv, SCREEN_TV, tv_rows, countof(tv_rows)));
    CHECK(holds(wall, SCREEN_WALL, wall_cards, countof(wall_cards)));
    CHECK(block_find("plan.today", NULL) && block_find("home.latest", NULL)); /* (all here in this build) */
    /* a household that deleted them keeps none: nothing new at the next start */
    int adam = with_role("Adam", ROLE_ADMIN);
    delete_screen(adam, tv);
    delete_screen(adam, wall);
    CHECK_INT(tbl_count(&T_screen), 0);
    CHECK_INT(tbl_count(&T_screen_row), 0);
    th_halt();
    CHECK(app_boot(app_config()->data_dir));
    CHECK_INT(tbl_count(&T_screen), 0);
    /* a household from before walls (its TV seeded already) gets a wall at its next start, once */
    setting_set_int("screens.wall_seeded", 0);
    th_halt();
    CHECK(app_boot(app_config()->data_dir));
    CHECK_INT(tbl_count(&T_screen), 1);
    CHECK(screens_first(SCREEN_WALL) && !screens_first(SCREEN_TV));
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

/* Issue #61: "type the address" - which one? The admin's steps show it, big, and as a QR code. */
TEST(screens_show_the_address_to_type_on_the_tv) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva");
    screens_test_addresses("http://192.168.1.50:8080", "http://hemma.local:8080");
    const char *h = th_page(adam, "/screens");
    CHECK_HAS(h, "<p class=\"screens-addr-big\">http://192.168.1.50:8080</p>");
    CHECK_HAS(h, "Phones and computers can use <b>http://hemma.local:8080</b> too; many TVs can't.");
    CHECK_HAS(h, "<svg class=\"qr\" viewBox=\"-4 -4 33 33\" role=\"img\" aria-label=\"http://192.168.1.50:8080\"");
    CHECK(strstr(h, "Under <a href=\"/people\">Household</a>") < strstr(h, "screens-addr-big"));
    CHECK(!strstr(th_page(eva, "/screens"), "screens-addr")); /* the steps are for admins */
    screens_test_addresses("http://192.168.1.50", NULL); /* port 80, and no name to offer */
    h = th_page(adam, "/screens");
    CHECK_HAS(h, ">http://192.168.1.50</p></div><figure class=\"screens-qr\">");
    CHECK(!strstr(h, "Phones and computers"));
    screens_test_addresses("", NULL); /* no network found: what the browser shows */
    h = th_page(adam, "/screens");
    CHECK(!strstr(h, "screens-addr"));
    CHECK_HAS(h, "type the address this browser shows");
    screens_test_addresses(NULL, NULL);
    /* --host localhost is this computer only, like 127.0.0.1 (issue #99: it showed http://localhost and a
       QR code for it); --host * is every address */
    app_test_host("localhost");
    h = th_page(adam, "/screens");
    CHECK_HAS(h, "Hemma only listens on this computer (localhost) now");
    CHECK(!strstr(h, "class=\"qr\""));
    char addr[2][96];
    CHECK_INT(app_addresses(addr, 2), 1);
    CHECK(str_starts(addr[0], "http://127.0.0.1") && !strstr(addr[0], "localhost"));
    app_test_host("*");
    CHECK(!app_local_only());
    for (int i = 0, n = app_addresses(addr, 2); i < n; i++) CHECK(!strchr(addr[i], '*'));
    app_test_host("192.168.1.50");
    CHECK_INT(app_addresses(addr, 2), 1);
    CHECK(str_starts(addr[0], "http://192.168.1.50"));
    app_test_host(NULL);
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
    snprintf(form, sizeof form, "id=%d&acc=%d", id, tv); /* the TV shows it (only a TV picks) */
    CHECK(th_act(adam, "screens.shows", form).ok);
    snprintf(url, sizeof url, "/screens/%d", id);
    const char *h = th_page(tv, url);
    CHECK_HAS(h, "<h1>Who's watching?</h1>");
    CHECK_HAS(h, "data-arrows");
    CHECK(!strstr(h, "<b>Adam</b>")); /* admins only once they say so (issue #90) */
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

TEST(screens_only_a_tv_picks_whos_watching) {
    /* issue #33: any display account - the hallway tablet too - could pick anyone, then read,
       rewrite or clear their Continue watching for as long as its year-long sign-in lasted */
    boot();
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva");
    int tv = with_role("Living room TV", ROLE_DISPLAY), tablet = with_role("Hall tablet", ROLE_DISPLAY);
    int screen = new_screen(adam, "name=Evenings"), wall = new_screen(adam, "name=Hall&kind=wall");
    CHECK(add(adam, screen, "zzscr.on", NULL).ok);
    char form[80], pick[40], url[40];
    snprintf(pick, sizeof pick, "who=%d", eva);
    CHECK(!th_act(tv, "screens.viewer", pick).ok); /* not ticked for any screen yet */
    snprintf(form, sizeof form, "id=%d&acc=%d", wall, tablet);
    CHECK(th_act(adam, "screens.shows", form).ok);
    CHECK(!th_act(tablet, "screens.viewer", pick).ok); /* a wall tablet never */
    CHECK_INT(viewer_of(tablet), 0);
    snprintf(url, sizeof url, "/screens/%d", screen); /* a TV screen it doesn't drive: watched by nobody */
    const char *h = th_page(tablet, url);
    CHECK(!strstr(h, "Who's watching?"));
    CHECK(!strstr(h, ">Switch</button>"));
    CHECK_HAS(h, "zz-on kind 1 viewer 0");
    /* ticked for the TV screen: it picks */
    snprintf(form, sizeof form, "id=%d&acc=%d", screen, tv);
    CHECK(th_act(adam, "screens.shows", form).ok);
    CHECK(th_act(tv, "screens.viewer", pick).ok);
    CHECK_INT(viewer_of(tv), eva);
    CHECK(th_act(tv, "screens.viewer", "who=0").ok);
    /* unticked: its sessions forget who was watching */
    Session s = {0};
    str_copy(s.token, sizeof s.token, "tv-session");
    s.person = tv;
    s.created = s.seen = now_sec();
    s.viewer = eva;
    tbl_put(&T_session, &s);
    snprintf(form, sizeof form, "id=%d", screen);
    CHECK(th_act(adam, "screens.shows", form).ok);
    CHECK_INT(((Session *)tbl_get(&T_session, s.id))->viewer, 0);
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
    snprintf(form, sizeof form, "id=%d&acc=%d", id, tv);
    CHECK(th_act(adam, "screens.shows", form).ok);
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
    /* unticked: the household's wall (issue #53: not "Good morning, Kitchen tablet") */
    snprintf(form, sizeof form, "id=%d", a);
    CHECK(th_act(adam, "screens.shows", form).ok);
    CHECK_STR(pref(tv1, PREF_START, ""), "");
    CHECK_STR(pref(tv2, PREF_START, ""), ub);
    CHECK_HAS(th_page(tv1, "/"), "screens-wall");
    /* a deleted screen lets go of its displays: they too go to the wall */
    snprintf(form, sizeof form, "id=%d", b);
    CHECK(th_act(adam, "screens.delete", form).ok);
    CHECK_STR(pref(tv2, PREF_START, ""), "");
    CHECK_HAS(th_page(tv2, "/"), "screens-wall");
    /* a household without a wall: the home page */
    snprintf(form, sizeof form, "id=%d", screens_first(SCREEN_WALL));
    CHECK(th_act(adam, "screens.delete", form).ok);
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
    CHECK_HAS(h, "<section class=\"screens-card\" id=\"screens-r");
    CHECK_HAS(h, "<h2>Wall only</h2><p>zz-wall</p>");
    CHECK_HAS(h, "zz-on kind 2");
    snprintf(url, sizeof url, "/screens/%d/edit", id);
    h = th_page(adam, url);
    CHECK_HAS(h, "<h2>Cards</h2>"); /* a wall's words */
    CHECK_HAS(h, "<h2>Add a card</h2>");
    CHECK_HAS(h, "id=\"screens-a-zzscr.wall\"");
    CHECK(!strstr(h, "id=\"screens-a-zzscr.pick\""));
    CHECK_HAS(th_page(adam, "/screens?new=1"), "value=\"wall\""); /* "New screen" offers walls */
    halt();
}

TEST(screens_a_wall_has_the_time_now_and_next_and_whos_home) {
    boot();
    g_fake_now = dt_make(2026, 10, 10, 13, 5); /* a Saturday */
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva"), tablet = with_role("Hall tablet", ROLE_DISPLAY);
    char url[40];
    snprintf(url, sizeof url, "/screens/%d", screens_first(SCREEN_WALL));
    const char *h = th_page(tablet, url);
    CHECK_HAS(h, "class=\"bare\"");
    CHECK(!strstr(h, "class=\"side\"")); /* no app chrome */
    CHECK_HAS(h, ">13:05</time>");
    CHECK_HAS(h, "Saturday 10 October · week 41");
    CHECK_HAS(h, "<div class=\"screens-wp \">"); /* nobody's whereabouts known yet: neutral */
    CHECK_HAS(h, "<b>Eva</b>");
    CHECK(!strstr(h, "screens-wnext")); /* nothing on, nothing next */
    CHECK_HAS(h, "All quiet");          /* every card empty: left out */
    /* no start page of its own: "/" brings this tablet to the household's wall - this one - so there's
       no Exit that only reloads it (issue #97) */
    CHECK(!strstr(h, ">Exit</a>"));
    CHECK(!strstr(h, "/edit\">Edit</a>"));
    CHECK(!strstr(h, "id=\"screens-fitsay\"")); /* "Didn't fit" is for an admin, not the hallway */
    int other = new_screen(adam, "name=Upstairs&kind=wall");
    char url2[40];
    snprintf(url2, sizeof url2, "/screens/%d", other);
    CHECK_HAS(th_page(tablet, url2), "href=\"/\">Exit</a>"); /* another wall: a way home */

    /* the car's out now, the dentist later, a note; Adam's home, Eva isn't */
    CHECK(th_act(adam, "book.preset", "name=Car").ok);
    g_fake_now = dt_make(2026, 10, 10, 11, 0); /* (booked this morning) */
    CHECK(th_act(adam, "book.create", "res=1&date=2026-10-10&from=12:00&to=15:00").ok);
    g_fake_now = dt_make(2026, 10, 10, 13, 5);
    CHECK(th_act(adam, "plan.event_add", "q=dentist+today+16%3A30").ok);
    CHECK(th_act(eva, "board.post", "text=Buy+milk").ok);
    presence_set_home(adam, true, g_fake_now);
    presence_set_home(eva, false, g_fake_now);
    h = th_page(tablet, url);
    CHECK_HAS(h, "<li class=\"screens-wn-now\"><span class=\"screens-wn-when\">Now</span>");
    CHECK_HAS(h, "Car <small>· Adam</small> <small>until 15:00</small>");
    CHECK_HAS(h, "<span class=\"screens-wn-when\">16:30</span>");
    CHECK_HAS(h, "<div class=\"screens-wp home\">");
    CHECK_HAS(h, "<div class=\"screens-wp away\">");
    CHECK_HAS(h, "<h2>Today</h2>");
    CHECK_HAS(h, "<h2>Bookings</h2>");
    CHECK_HAS(h, "<h2>Notes</h2>");
    CHECK_HAS(h, "Buy milk");
    CHECK_HAS(h, "<h2>Latest</h2>");
    CHECK(!strstr(h, "<h2>Shopping list</h2>")); /* empty: left out */
    CHECK(!strstr(h, "All quiet"));
    CHECK(!strstr(h, "<form")); /* a wall shows; it doesn't tick things off */
    CHECK(strstr(h, "<h2>Today</h2>") < strstr(h, "<h2>Bookings</h2>")); /* the wall's order */

    CHECK_HAS(h, "data-fit=\"screens-fitsay\" data-fit-cols=\"4\""); /* (wire.js lays the cards out, issue #97) */

    /* admins can edit it from there, and see what didn't fit; a tablet whose own start page it is has no Exit */
    CHECK_HAS(th_page(adam, url), "/edit\">Edit</a>");
    CHECK_HAS(th_page(adam, url), "id=\"screens-fitsay\"");
    CHECK_HAS(th_page(adam, url), "href=\"/\">Exit</a>");
    pref_set(tablet, PREF_START, url);
    CHECK(!strstr(th_page(tablet, url), ">Exit</a>"));
    pref_set(tablet, PREF_START, url2); /* its own start page elsewhere: Exit goes there */
    CHECK_HAS(th_page(tablet, url), "href=\"/\">Exit</a>");
    halt();
}

/* One request through the screens module's HTTP hook, with a socket-less Conn (the reply stays in conn.out). */
static const char *serve(int person, const char *method, const char *path, Buf *keep) {
    char raw[400];
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: t\r\n\r\n", method, path);
    HttpReq r;
    if (http_parse(raw, strlen(raw), 1 << 20, &r) != 1) return "unparsable";
    Conn conn;
    memset(&conn, 0, sizeof conn);
    conn.fd = SOCK_BAD;
    conn.keep_alive = true;
    Ctx c;
    app_ctx(&c, person, r.target);
    bool handled = module_find("screens")->http(&c, &conn, &r);
    app_ctx_free(&c);
    http_req_free(&r);
    buf_reset(keep);
    buf_put(keep, conn.out.p, conn.out.len);
    buf_free(&conn.out);
    buf_free(&conn.in);
    return handled ? buf_cstr(keep) : "not handled";
}

TEST(screens_the_old_wall_address_opens_the_first_wall) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN);
    Buf out = {0};
    char want[64];
    snprintf(want, sizeof want, "Location: /screens/%d\r\n", screens_first(SCREEN_WALL));
    CHECK(str_starts(serve(adam, "GET", "/wall", &out), "HTTP/1.1 302"));
    CHECK_HAS(buf_cstr(&out), want);
    CHECK(str_starts(serve(adam, "HEAD", "/wall", &out), "HTTP/1.1 302"));
    CHECK_STR(serve(adam, "POST", "/wall", &out), "not handled");
    CHECK_STR(serve(adam, "GET", "/walls", &out), "not handled");
    delete_screen(adam, screens_first(SCREEN_WALL)); /* no wall: the list of screens */
    CHECK_HAS(serve(adam, "GET", "/wall", &out), "Location: /screens\r\n");
    buf_free(&out);
    halt();
}

TEST(screens_wall_latest_is_what_people_did) {
    boot();
    int adam = th_person("Adam");
    activity_add(0, "🆕", "/net", "New device on the network: a phone");
    activity_add(adam, "📌", "/board", "Adam pinned a note");
    const Block *bl = block_find("home.latest", NULL);
    CHECK(bl && (bl->kinds & SCREEN_WALL));
    Ctx c;
    app_ctx(&c, 0, "/screens/2");
    Buf b = {0};
    CHECK(bl->render(&c, &b, SCREEN_WALL, ""));
    CHECK_HAS(buf_cstr(&b), "Adam pinned a note");
    CHECK(!strstr(buf_cstr(&b), "New device"));
    buf_free(&b);
    app_ctx_free(&c);
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

/* ---------------------------------------------------- the picker's guard (issue #90) */

static void tv_showing(int adam, char url[40], int *tv_out) {
    int tv = *tv_out = with_role("Living room TV", ROLE_DISPLAY);
    int id = new_screen(adam, "name=Kids");
    char form[40];
    snprintf(form, sizeof form, "id=%d&acc=%d", id, tv);
    CHECK(th_act(adam, "screens.shows", form).ok);
    snprintf(url, 40, "/screens/%d", id);
}

/* Anyone holding the TV's sign-in could pick an admin, then read and clear their watch history. */
TEST(screens_admins_join_the_picker_when_they_say_so) {
    boot();
    int adam = with_role("Adam", ROLE_ADMIN);
    th_person("Eva");
    char url[40], form[40];
    int tv = 0;
    tv_showing(adam, url, &tv);
    snprintf(form, sizeof form, "who=%d", adam);
    CHECK(!strstr(th_page(tv, url), "<b>Adam</b>"));
    CHECK(!th_act(tv, "screens.viewer", form).ok); /* (not by the address either) */
    CHECK_INT(viewer_of(tv), 0);
    CHECK_HAS(th_page(adam, "/screens"), "Show me on the TV");
    CHECK(th_act(adam, "screens.on_tv", "on=1").ok);
    CHECK_HAS(th_page(tv, url), "<b>Adam</b>");
    CHECK(th_act(tv, "screens.viewer", form).ok);
    CHECK_INT(viewer_of(tv), adam);
    CHECK(th_act(adam, "screens.on_tv", "").ok); /* off again: the TV asks again */
    CHECK_HAS(th_page(tv, url), "Who's watching?");
    CHECK_INT(viewer_of(tv), 0);
    halt();
}

TEST(screens_a_pin_guards_a_spot_on_the_picker) {
    boot();
    auth_configure("screens-salt", 1000);
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva");
    char url[40], form[60], pinurl[60];
    int tv = 0;
    tv_showing(adam, url, &tv);
    CHECK(!th_act(eva, "screens.pin", "pin=12x4").ok);
    CHECK(!th_act(eva, "screens.pin", "pin=123").ok);
    CHECK(th_act(eva, "screens.pin", "pin=1234").ok);
    CHECK(!strstr(pref(eva, PREF_PIN, ""), "1234")); /* (kept as an HMAC, never as it was typed) */
    CHECK_HAS(th_page(eva, "/screens"), "Your PIN is set");
    /* the picker asks for it first */
    snprintf(pinurl, sizeof pinurl, "%s?pin=%d", url, eva);
    char want[80];
    snprintf(want, sizeof want, "href=\"%s\"", pinurl);
    CHECK_HAS(th_page(tv, url), want);
    const char *h = th_page(tv, pinurl);
    CHECK_HAS(h, "<h1>Eva's PIN</h1>");
    CHECK_HAS(h, "inputmode=\"numeric\"");
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(!th_act(tv, "screens.viewer", form).ok); /* no PIN: no */
    CHECK_INT(viewer_of(tv), 0);
    snprintf(form, sizeof form, "who=%d&pin=1234", eva);
    CHECK(th_act(tv, "screens.viewer", form).ok);
    CHECK_INT(viewer_of(tv), eva);
    CHECK(th_act(tv, "screens.viewer", "who=0").ok);
    /* wrong guesses: five, then a wait - even for the right one */
    snprintf(form, sizeof form, "who=%d&pin=0000", eva);
    for (int i = 0; i < 5; i++) CHECK(!th_act(tv, "screens.viewer", form).ok);
    snprintf(form, sizeof form, "who=%d&pin=1234", eva);
    ThResult r = th_act(tv, "screens.viewer", form);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Too many wrong PINs");
    CHECK_HAS(th_page(tv, pinurl), "Too many wrong PINs");
    g_fake_now = now_sec() + 61;
    CHECK(th_act(tv, "screens.viewer", form).ok);
    CHECK_INT(viewer_of(tv), eva);
    g_fake_now = 0;
    /* gone again: picked straight away */
    CHECK(th_act(eva, "screens.pin", "clear=1").ok);
    CHECK(th_act(tv, "screens.viewer", "who=0").ok);
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(th_act(tv, "screens.viewer", form).ok);
    halt();
}

/* Issue #42: each person chooses how the wall shows them and whether a TV lists them; issue #44: guests
   don't get the household's screens, children pick their own place on the TV. */
TEST(screens_people_choose_how_they_are_shown) {
    boot();
    g_fake_now = dt_make(2026, 10, 10, 13, 5);
    int adam = with_role("Adam", ROLE_ADMIN), eva = th_person("Eva"), bo = th_person("Bo");
    int leo = with_role("Leo", ROLE_CHILD), gun = with_role("Gun", ROLE_GUEST);
    int tablet = with_role("Hall tablet", ROLE_DISPLAY);
    char url[40], tvurl[40];
    snprintf(url, sizeof url, "/screens/%d", screens_first(SCREEN_WALL));
    presence_set_home(eva, true, g_fake_now);
    presence_set_home(bo, true, g_fake_now);
    CHECK(th_act(eva, "people.privacy", "all=1&wall=name&doing=1&tv=1&home=1").ok);
    CHECK(th_act(bo, "people.privacy", "all=1&wall=no&doing=1&tv=1&home=1").ok);
    const char *h = th_page(tablet, url);
    CHECK_HAS(h, "<div class=\"screens-wp \">"); /* Eva: her name, not where she is */
    CHECK_HAS(h, "<b>Eva</b>");
    CHECK(!strstr(h, "screens-wp home"));
    CHECK(!strstr(h, "<b>Bo</b>")); /* Bo: not at all */
    CHECK_HAS(h, "<b>Leo</b>");
    /* the TV's picker: members and children unless they untick it, admins and guests once they tick it */
    int tv = 0;
    tv_showing(adam, tvurl, &tv);
    h = th_page(tv, tvurl);
    CHECK_HAS(h, "<b>Leo</b>");
    CHECK_HAS(h, "<b>Eva</b>");
    CHECK(!strstr(h, "<b>Gun</b>"));
    CHECK(th_act(leo, "screens.on_tv", "on=0").ok); /* (a child's own choice) */
    CHECK(!strstr(th_page(tv, tvurl), "<b>Leo</b>"));
    CHECK_HAS(th_page(leo, "/screens"), "Show me on the TV");
    /* a guest: not the household's screens */
    CHECK_HAS(th_page(gun, "/screens"), "Nothing here");
    CHECK(!th_act(gun, "screens.on_tv", "on=1").ok);
    CHECK(!strstr(th_page(gun, "/"), "href=\"/wall\""));
    halt();
}
