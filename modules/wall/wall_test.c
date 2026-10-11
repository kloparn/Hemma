#include "../../tests/harness.h"

/* A wall tablet signs in with a read-only "Display" account. */
static int display_account(void) {
    int id = th_person("Hallway tablet");
    Person p = *person_get(id);
    p.role = ROLE_DISPLAY;
    tbl_put(&T_person, &p);
    return id;
}

TEST(wall_renders_for_a_display_account) {
    th_boot();
    th_person("Adam");
    int tablet = display_account();
    g_fake_now = dt_make(2026, 10, 10, 18, 5);
    const char *html = th_page(tablet, "/wall");
    CHECK_HAS(html, "class=\"bare\"");
    CHECK_HAS(html, "data-mod=\"wall\"");
    CHECK(!strstr(html, "class=\"side\"")); /* no app chrome */
    CHECK(!strstr(html, "class=\"tabbar\""));
    CHECK_HAS(html, ">18:05</time>");
    CHECK_HAS(html, "Saturday 10 October · week 41");
    CHECK_HAS(html, "<b>Adam</b>");
    CHECK_HAS(html, "href=\"/\">Exit</a>");
    th_halt();
}

TEST(wall_needs_a_sign_in) {
    th_boot();
    th_person("Adam");
    const char *html = th_page(0, "/wall");
    CHECK_HAS(html, "data-mod=\"auth\"");
    CHECK(!strstr(html, "class=\"wall\""));
    th_halt();
}

TEST(wall_display_accounts_cannot_act) {
    th_boot();
    int tablet = display_account();
    ThResult r = th_act(tablet, "board.post", "text=hello");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "view-only");
    th_halt();
}

TEST(wall_shows_other_modules_widgets_and_latest) {
    th_boot();
    int adam = th_person("Adam");
    int tablet = display_account();
    CHECK(th_act(adam, "board.post", "text=Pick+up+Love+at+16&pin=1").ok);
    const char *html = th_page(tablet, "/wall");
    CHECK_HAS(html, "wall-grid");
    CHECK_HAS(html, "Pick up Love at 16");
    CHECK_HAS(html, "<h2>Latest</h2>");
    CHECK_HAS(html, "Adam pinned a note");
    th_halt();
}

TEST(wall_shows_who_is_home) {
    th_boot();
    int adam = th_person("Adam");
    th_person("Eva");
    int tablet = display_account();
    CHECK_HAS(th_page(tablet, "/wall"), "<div class=\"wall-person \">"); /* no tracking yet: neutral */
    presence_set_home(adam, true, now_sec());
    const char *html = th_page(tablet, "/wall");
    CHECK_HAS(html, "wall-person home");
    CHECK_HAS(html, "wall-person away");
    th_halt();
}

TEST(wall_is_reachable_by_people_too) {
    th_boot();
    int adam = th_person("Adam");
    const char *html = th_page(adam, "/wall");
    CHECK_HAS(html, "class=\"wall\"");
    th_halt();
}
