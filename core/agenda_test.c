/* The shared agenda: bookings show up in the planner's calendar without the
 * two modules knowing about each other. */
#include "../tests/harness.h"

TEST(agenda_collects_bookings_into_the_calendar) {
    if (!module_find("book") || !module_find("plan")) return; /* integration test: needs both modules */
    th_boot();
    g_fake_now = dt_make(2026, 10, 7, 10, 0);
    int adam = th_person("Adam");
    CHECK(th_act(adam, "book.preset", "name=Car").ok);
    CHECK(th_act(adam, "book.create", "res=1&date=2026-10-08&from=14:00&to=16:00&note=dentist").ok);

    Ctx c;
    app_ctx(&c, adam, "/");
    Agenda ag = {0};
    agenda_collect(&c, dt_make(2026, 10, 8, 0, 0), dt_make(2026, 10, 9, 0, 0), NULL, &ag);
    CHECK_INT(ag.n, 1);
    CHECK_STR(ag.items[0].module, "book");
    CHECK_STR(ag.items[0].title, "Car · dentist");
    CHECK_STR(ag.items[0].icon, "🚗");
    CHECK_INT(ag.items[0].who, adam);
    CHECK_HAS(ag.items[0].link, "/book/1?w=2026-10-08&b=1");
    agenda_free(&ag);
    agenda_collect(&c, dt_make(2026, 10, 8, 0, 0), dt_make(2026, 10, 9, 0, 0), "book", &ag);
    CHECK_INT(ag.n, 0); /* `except` skips a module */
    agenda_collect(&c, dt_make(2026, 10, 9, 0, 0), dt_make(2026, 10, 10, 0, 0), NULL, &ag);
    CHECK_INT(ag.n, 0); /* outside the window */
    agenda_free(&ag);
    app_ctx_free(&c);

    const char *cal = th_page(adam, "/plan/cal?day=2026-10-07");
    CHECK_HAS(cal, "plan-ext");
    CHECK_HAS(cal, "Car · dentist");
    th_halt();
}
