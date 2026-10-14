/* The core's side of screens (the TV view, wall tablets): blocks that modules offer, who is
 * watching on a display account, the few actions a display account may run, the page its "/"
 * shows, and a section's pages lighting up their parent in the navigation - with a module of
 * its own, registered once. */
#include "../tests/harness.h"

static bool g_on; /* this module's output only while these tests run (it stays registered) */

static bool rows_render(Ctx *c, Buf *b, int kind, const char *arg) {
    if (!g_on) return false;
    H(b, "<p id=\"zz-row\">kind %d arg %s viewer %d</p>", kind, arg, ctx_viewer(c));
    return true;
}

static int rows_choices(Ctx *c, BlockChoice *out, int max) {
    static const char *const names[] = {"first", "second"};
    int n = 0;
    for (int i = 0; i < countof(names) && n < max; i++, n++) {
        str_copy(out[n].arg, sizeof out[n].arg, names[i]);
        str_copy(out[n].label, sizeof out[n].label, names[i]);
    }
    return n;
}

static void rows_watch(Ctx *c, Fields *f) { ctx_set_viewer(c, (int)fields_int(f, "who", 0)); }
static void rows_change(Ctx *c, Fields *f) { ctx_ok(c, "changed"); }

static void rows_page(Ctx *c, Buf *b) { H(b, "<p id=\"zz-rows-page\">%s</p>", c->url); }

static const Block rows_blocks[] = {
    {"zzrows.one", "One row", SCREEN_TV, rows_render, NULL},
    {"zzrows.pick", "A picked row", SCREEN_TV | SCREEN_WALL, rows_render, rows_choices},
    {0},
};
static const Action rows_actions[] = {
    {"zzrows.watch", rows_watch, false, true},
    {"zzrows.change", rows_change},
    {0},
};
static const Module rows = {
    .id = "zzrows",
    .title = "Rows",
    .icon = "tv",
    .nav = 0,
    .parent = "home", /* its pages light up Home */
    .page = rows_page,
    .actions = rows_actions,
    .blocks = rows_blocks,
};

static void with_rows(void) {
    static bool done;
    if (!done) module_register(&rows);
    done = true;
    g_on = true;
}

static int display_account(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_DISPLAY;
    tbl_put(&T_person, &p);
    return id;
}

TEST(app_blocks_are_found_by_id) {
    with_rows();
    th_boot();
    const Module *owner = NULL;
    const Block *bl = block_find("zzrows.pick", &owner);
    CHECK(bl != NULL && owner == &rows);
    CHECK_STR(bl->title, "A picked row");
    CHECK(bl->kinds & SCREEN_WALL);
    BlockChoice ch[4];
    CHECK_INT(bl->choices(NULL, ch, 4), 2);
    CHECK_STR(ch[1].arg, "second");
    CHECK(block_find("zzrows.nope", NULL) == NULL);
    Ctx c;
    app_ctx(&c, th_person("Adam"), "/");
    Buf b = {0};
    CHECK(bl->render(&c, &b, SCREEN_TV, "first"));
    CHECK_HAS(buf_cstr(&b), "kind 1 arg first");
    buf_free(&b);
    app_ctx_free(&c);
    g_on = false;
    th_halt();
}

TEST(app_a_display_account_watches_for_whoever_it_picked) {
    with_rows();
    th_boot();
    int adam = th_person("Adam"), eva = th_person("Eva");
    int tv = display_account("Living room TV");
    char form[40];
    /* a person is their own viewer; a display account has none until someone is picked */
    Ctx c;
    app_ctx(&c, adam, "/");
    CHECK_INT(ctx_viewer(&c), adam);
    app_ctx_free(&c);
    app_ctx(&c, tv, "/");
    CHECK_INT(ctx_viewer(&c), 0);
    app_ctx_free(&c);
    /* the TV may run what's marked for display accounts, and nothing else */
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(th_act(tv, "zzrows.watch", form).ok);
    app_ctx(&c, tv, "/");
    CHECK_INT(ctx_viewer(&c), eva);
    app_ctx_free(&c);
    ThResult r = th_act(tv, "zzrows.change", "");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "view-only");
    /* only real people can be picked: not another display account, not nobody */
    snprintf(form, sizeof form, "who=%d", tv);
    CHECK(th_act(tv, "zzrows.watch", form).ok);
    app_ctx(&c, tv, "/");
    CHECK_INT(ctx_viewer(&c), 0);
    app_ctx_free(&c);
    /* a person picking for themselves changes nothing: they always watch as themselves */
    snprintf(form, sizeof form, "who=%d", eva);
    CHECK(th_act(adam, "zzrows.watch", form).ok);
    app_ctx(&c, adam, "/");
    CHECK_INT(ctx_viewer(&c), adam);
    app_ctx_free(&c);
    g_on = false;
    th_halt();
}

TEST(app_a_display_account_opens_on_its_own_screen) {
    with_rows();
    th_boot();
    int adam = th_person("Adam"), tv = display_account("Living room TV");
    CHECK(!strstr(th_page(tv, "/"), "zz-rows-page")); /* nothing set: the usual home page */
    pref_set(tv, PREF_START, "/zzrows/3");
    const char *h = th_page(tv, "/");
    CHECK_HAS(h, "<p id=\"zz-rows-page\">/zzrows/3</p>");
    CHECK_HAS(th_page(tv, "/?who=1"), "<p id=\"zz-rows-page\">/zzrows/3?who=1</p>"); /* the query comes along */
    CHECK(!strstr(th_page(adam, "/"), "zz-rows-page")); /* people keep their home page */
    pref_set(tv, PREF_START, "//evil.example");
    CHECK(!strstr(th_page(tv, "/"), "zz-rows-page")); /* only this site's own pages */
    g_on = false;
    th_halt();
}

TEST(app_a_sections_pages_light_up_their_parent) {
    with_rows();
    th_boot();
    int adam = th_person("Adam");
    const char *h = th_page(adam, "/zzrows/1");
    CHECK_HAS(h, "zz-rows-page");
    CHECK_HAS(h, "<a href=\"/\" class=\"on\" aria-current=\"page\">"); /* Home lights up */
    CHECK(!strstr(h, "<span>Rows</span>"));                          /* nav 0: not listed itself */
    g_on = false;
    th_halt();
}

/* Issue #54: a display account (the hallway tablet) saw quick-add boxes, ticks and x buttons on
   every page, and each tap only answered "This screen is view-only". Its pages now carry only the
   forms it may use (Action.display) - here every module's own page, with something on it. */
TEST(app_display_pages_offer_only_what_a_display_may_do) {
    th_boot();
    int adam = th_person("Adam");
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int tablet = th_person("Hall tablet");
    p = *person_get(tablet);
    p.role = ROLE_DISPLAY;
    tbl_put(&T_person, &p);
    /* something to show (actions of modules that aren't built are simply unknown) */
    th_act(adam, "board.post", "text=Plumber+comes+Thursday&hue=45");
    th_act(adam, "plan.task_add", "q=vacuum+today");
    th_act(adam, "plan.shop_add", "q=milk%2C+eggs");
    th_act(adam, "plan.event_add", "q=dentist+tomorrow+14%3A30");
    th_act(adam, "book.res_save", "name=Car&kind=hourly&always=1");
    th_act(adam, "book.quick", "q=car+tomorrow+14-17");
    int pages = 0;
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (!m->page || (m->flags & MOD_ADMIN)) continue;
        char url[64];
        snprintf(url, sizeof url, "/%s", m->id);
        const char *h = th_page(tablet, url);
        pages++;
        for (const char *s = strstr(h, "data-a=\""); s; s = strstr(s + 1, "data-a=\"")) {
            char name[64];
            size_t n = strcspn(s + 8, "\"");
            str_ncopy(name, sizeof name, s + 8, n);
            if (!app_action_display(name)) test_fail(__FILE__, __LINE__, "%s offers %s to a display account", url, name);
        }
    }
    CHECK(pages > 3);
    /* (a person still gets the forms) */
    CHECK_HAS(th_page(adam, "/plan"), "data-a=\"plan.task_add\"");
    th_halt();
}
