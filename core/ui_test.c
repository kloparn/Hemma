/* What the shared components say to a keyboard and a screen reader (issues #63 #68 #69 #70). */
#include "../tests/harness.h"
#include "ui.h"

/* Issue #68: "Skip to content" before the sidebar's links, and the content takes the focus. */
TEST(ui_shell_starts_with_a_skip_link) {
    th_boot();
    int adam = th_person("Adam");
    const char *h = th_page(adam, "/");
    const char *skip = strstr(h, "<a class=\"skip\" href=\"#main\">Skip to content</a>");
    const char *side = strstr(h, "<aside class=\"side\">");
    CHECK(skip != NULL);
    CHECK(skip && side && skip < side);
    CHECK_HAS(h, "<main class=\"main\" id=\"main\" tabindex=\"-1\">");
    th_halt();
}

/* Issue #70: the sign-in pages are a landmark like the app's. */
TEST(ui_sign_in_pages_have_a_main_landmark) {
    th_boot();
    CHECK_HAS(th_page(0, "/"), "<main class=\"center-page\" id=\"main\">");
    th_halt();
}

/* Issue #69: an avatar alone says who; one beside the name is left out of what's read. */
TEST(ui_avatars_are_named_or_hidden) {
    th_boot();
    int eva = th_person("Eva");
    Buf b = {0};
    ui_avatar(&b, person_get(eva), "sm");
    CHECK_HAS(buf_cstr(&b), "title=\"Eva\" role=\"img\" aria-label=\"Eva\"");
    buf_reset(&b);
    ui_avatar_deco(&b, person_get(eva), "sm");
    CHECK_HAS(buf_cstr(&b), "aria-hidden=\"true\"");
    CHECK(!strstr(buf_cstr(&b), "role=\"img\""));
    buf_reset(&b);
    ui_person(&b, person_get(eva)); /* (picture + name: the name is what's read) */
    CHECK_HAS(buf_cstr(&b), "aria-hidden=\"true\"");
    CHECK_HAS(buf_cstr(&b), "<span>Eva</span>");
    buf_reset(&b);
    ui_avatar(&b, NULL, "xs");
    CHECK_HAS(buf_cstr(&b), "role=\"img\" aria-label=\"Someone\"");
    buf_free(&b);
    th_halt();
}

/* Issue #63: a dialog is labelled by its title; the backdrop's close link stays clickable (and
   Escape's) but out of Tab's way - wire.js moves the focus in and keeps it there. */
TEST(ui_dialogs_are_labelled_by_their_title) {
    Buf b = {0};
    ui_modal(&b, "Add someone", "/people");
    ui_modal_end(&b);
    CHECK_HAS(buf_cstr(&b), "role=\"dialog\" aria-modal=\"true\" aria-labelledby=\"modal-t\"");
    CHECK_HAS(buf_cstr(&b), "<h2 id=\"modal-t\">Add someone</h2>");
    CHECK_HAS(buf_cstr(&b), "data-esc tabindex=\"-1\" aria-hidden=\"true\"");
    buf_free(&b);
}
