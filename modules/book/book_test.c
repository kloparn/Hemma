#include "../../tests/harness.h"
#include "book.h"

/* Wednesday 7 Oct 2026, 10:00 local time */
static int adam, eva;

static void setup(void) {
    th_boot();
    g_fake_now = dt_make(2026, 10, 7, 10, 0);
    adam = th_person("Adam");
    eva = th_person("Eva");
}

static int add_res(const char *name, const char *emoji, int slot, int open, int close, int dflt, int max) {
    Resource r = {0};
    str_copy(r.name, sizeof r.name, name);
    str_copy(r.emoji, sizeof r.emoji, emoji);
    r.slot = slot;
    r.open_min = open;
    r.close_min = close;
    r.default_min = dflt;
    r.max_min = max;
    r.hue = 200;
    tbl_put(&T_resource, &r);
    return r.id;
}

static DateTime at(int64_t t) { return dt_local(t); }

TEST(book_presets_add_resources) {
    setup();
    CHECK(th_act(adam, "book.preset", "name=Car").ok);
    CHECK(th_act(adam, "book.preset", "name=Guest+room").ok);
    CHECK(!th_act(adam, "book.preset", "name=Spaceship").ok);
    CHECK_INT(tbl_count(&T_resource), 2);
    Resource *gr = res_get(2);
    CHECK(gr && res_daily(gr));
    CHECK_STR(res_get(1)->emoji, "🚗");
    th_halt();
}

TEST(book_create_detects_clashes_but_allows_back_to_back) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    char form[200];
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=14:00&to=16:00&note=ikea", car);
    CHECK(th_act(adam, "book.create", form).ok);
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=15:00&to=17:00&who=%d", car, eva);
    ThResult r = th_act(eva, "book.create", form);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Adam has Car");
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=16:00&to=18:00", car);
    CHECK(th_act(eva, "book.create", form).ok);
    CHECK_INT(tbl_count(&T_booking), 2);
    Booking *b = book_get(1);
    CHECK_STR(b->note, "ikea");
    CHECK_INT(at(b->start).hour, 14);
    CHECK_INT(b->by, adam);
    th_halt();
}

TEST(book_rules_hours_max_and_past) {
    setup();
    int laundry = add_res("Laundry", "🧺", 60, 7 * 60, 22 * 60, 180, 240);
    char form[200];
    ThResult r;
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=21:00&to=23:00", laundry);
    r = th_act(adam, "book.create", form);
    CHECK_HAS(r.toast, "07:00–22:00");
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=08:00&to=13:00", laundry);
    r = th_act(adam, "book.create", form);
    CHECK_HAS(r.toast, "at most 4 h");
    snprintf(form, sizeof form, "res=%d&date=2026-10-06&from=08:00&to=10:00", laundry);
    r = th_act(adam, "book.create", form);
    CHECK_HAS(r.toast, "already over");
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=10:00&to=09:00", laundry);
    CHECK(!th_act(adam, "book.create", form).ok); /* to <= from means next day: outside hours */
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=08:00&to=11:00", laundry);
    CHECK(th_act(adam, "book.create", form).ok);
    th_halt();
}

TEST(book_quick_parse_english_and_swedish) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    int tv = add_res("Tvättstuga", "🧺", 60, 7 * 60, 22 * 60, 180, 0);
    int guest = add_res("Guest room", "🛏️", 1440, 0, 1440, 1440, 0);
    Quick q;
    book_quick_parse("car fri 14-17 ikea trip", 0, g_fake_now, &q);
    CHECK_INT(q.res, car);
    CHECK_STR(q.problem, "");
    CHECK_INT(at(q.start).day, 9);
    CHECK_INT(at(q.start).hour, 14);
    CHECK_INT(at(q.end).hour, 17);
    CHECK_STR(q.note, "ikea trip");

    book_quick_parse("tvätt imorgon 18-21 @eva", 0, g_fake_now, &q);
    CHECK_INT(q.res, tv);
    CHECK_INT(q.who, eva);
    CHECK_INT(at(q.start).day, 8);
    CHECK_INT(at(q.start).hour, 18);
    CHECK_INT(at(q.end).hour, 21);

    book_quick_parse("guest room fri-sun for grandma", 0, g_fake_now, &q);
    CHECK_INT(q.res, guest);
    CHECK_INT(at(q.start).day, 9);
    CHECK_INT(at(q.start).hour, 0);
    CHECK_INT(at(q.end).day, 12); /* Fri, Sat and Sun: until Monday 00:00 */
    CHECK_STR(q.note, "grandma");

    book_quick_parse("🚗 sat", 0, g_fake_now, &q); /* around-the-clock thing, no time: the whole day */
    CHECK_INT(q.res, car);
    CHECK_INT(at(q.start).day, 10);
    CHECK_INT(q.end - q.start, 86400);

    book_quick_parse("boat fri 10-12", 0, g_fake_now, &q);
    CHECK_INT(q.res, 0);
    CHECK_HAS(q.problem, "Start with what to book");

    book_quick_parse("tvättstuga sat", 0, g_fake_now, &q);
    CHECK_HAS(q.problem, "What time?");

    book_quick_parse("fri 10-12", car, g_fake_now, &q); /* default resource from the page */
    CHECK_INT(q.res, car);
    CHECK_STR(q.problem, "");

    /* issue #13: days that don't exist and ranges ending before they start are refused with a
       reason - "car 31 sep 14-16" used to book today 14-16 */
    book_quick_parse("car 31 sep 14-16", 0, g_fake_now, &q);
    CHECK_HAS(q.problem, "31 September");
    CHECK(q.start == 0 && q.end == 0);
    book_quick_parse("guest room 12/10/2026-10/10/2026", 0, g_fake_now, &q);
    CHECK_HAS(q.problem, "before the start");
    th_halt();
}

TEST(book_quick_offers_free_alternative) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    CHECK(th_act(adam, "book.quick", "q=car+fri+14-17").ok);
    CHECK(!th_act(eva, "book.quick", "q=car+fri+14-17").ok);
    Quick q;
    book_quick_parse("car fri 15-16", 0, g_fake_now, &q);
    CHECK_HAS(q.problem, "Adam has Car");
    CHECK(q.has_alt);
    CHECK_INT(at(q.alt_start).hour, 17);
    CHECK_INT(q.alt_end - q.alt_start, 3600);
    (void)car;
    th_halt();
}

TEST(book_next_free_skips_full_days_and_closed_hours) {
    setup();
    Resource *r = res_get(add_res("Laundry", "🧺", 60, 7 * 60, 22 * 60, 180, 0));
    int id = r->id;
    Booking b = {.res = id, .who = adam, .start = dt_make(2026, 10, 8, 7, 0), .end = dt_make(2026, 10, 8, 22, 0)};
    tbl_put(&T_booking, &b);
    int64_t s = 0;
    CHECK(book_next_free(res_get(id), dt_make(2026, 10, 8, 6, 0), 3 * 3600, 0, &s));
    CHECK_INT(at(s).day, 9);
    CHECK_INT(at(s).hour, 7);
    CHECK(book_next_free(res_get(id), dt_make(2026, 10, 9, 20, 0), 3 * 3600, 0, &s));
    CHECK_INT(at(s).day, 10); /* 20-23 doesn't fit before closing */
    CHECK_INT(at(s).hour, 7);
    th_halt();
}

TEST(book_move_and_cancel) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    char form[200];
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=14:00&to=16:00", car);
    CHECK(th_act(adam, "book.create", form).ok);
    /* moving onto an overlapping time with itself is fine */
    snprintf(form, sizeof form, "id=1&res=%d&date=2026-10-08&from=15:00&to=17:00", car);
    CHECK(th_act(adam, "book.create", form).ok);
    CHECK_INT(tbl_count(&T_booking), 1);
    CHECK_INT(at(book_get(1)->start).hour, 15);
    CHECK(th_act(adam, "book.cancel", "id=1").ok);
    CHECK_INT(tbl_count(&T_booking), 0);
    CHECK(!th_act(adam, "book.cancel", "id=1").ok);
    CHECK(tbl_count(&T_activity) >= 3); /* booked, moved, cancelled */
    th_halt();
}

TEST(book_needs_someone_signed_in) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    char form[200];
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=14:00&to=16:00", car);
    CHECK(!th_act(0, "book.create", form).ok);
    CHECK_INT(tbl_count(&T_booking), 0);
    th_halt();
}

TEST(book_resource_settings) {
    setup();
    CHECK(th_act(adam, "book.res_save", "name=Sauna&emoji=%F0%9F%A7%96&kind=hourly&slot=30&open=06:00&close=23:00"
                                        "&dflt=90&max=180&rules=Shower+first&hue=15")
              .ok);
    Resource *r = res_get(1);
    CHECK(r && r->open_min == 360 && r->close_min == 1380 && r->max_min == 180 && r->default_min == 90);
    CHECK_STR(r->rules, "Shower first");
    CHECK(!th_act(adam, "book.res_save", "id=1&name=Sauna&kind=hourly&open=22:00&close=06:00").ok);
    CHECK(th_act(adam, "book.res_save", "id=1&name=Sauna&kind=hourly&always=1").ok);
    CHECK(res_get(1)->open_min == 0 && res_get(1)->close_min == 1440);
    CHECK(th_act(adam, "book.res_archive", "id=1").ok);
    CHECK(res_get(1)->archived);
    CHECK_HAS(th_page(adam, "/book"), "What do you share at home?");
    th_halt();
}

TEST(book_pages_render) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    int guest = add_res("Guest room", "🛏️", 1440, 0, 1440, 1440, 0);
    char form[200];
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=14:00&to=16:00&who=%d&note=dentist", car, eva);
    CHECK(th_act(adam, "book.create", form).ok);
    snprintf(form, sizeof form, "res=%d&date=2026-10-09&date_to=2026-10-11", guest);
    CHECK(th_act(adam, "book.create", form).ok);

    const char *o = th_page(adam, "/book");
    CHECK_HAS(o, "Coming up");
    CHECK_HAS(o, "Guest room");
    CHECK_HAS(o, "dentist");

    const char *w = th_page(adam, "/book/1?w=2026-10-05");
    CHECK_HAS(w, "Week 41");
    CHECK_HAS(w, "class=\"book-wk-b hued");
    CHECK_HAS(w, "<b>Eva</b>");
    CHECK_HAS(w, "book-wk-now"); /* "now" (Wed 10:00) is inside the visible week */
    CHECK_HAS(w, ">06:00</span>");           /* hour labels (around-the-clock: from 06:00) */
    CHECK_HAS(w, "--top:16.00;--len:4.00"); /* Thu 14:00–16:00 in 30-min rows from 06:00 */
    CHECK_HAS(w, "--top:8.00\"");            /* the now-line at Wed 10:00 */

    CHECK_HAS(th_page(adam, "/book/1?w=2026-10-05&new=1791626400"), "Book Car");
    CHECK_HAS(th_page(adam, "/book/1?b=1"), "Cancel booking");
    CHECK_HAS(th_page(adam, "/book/1?b=1&edit=1"), "Save changes");
    const char *m = th_page(adam, "/book/2?w=2026-10-05");
    CHECK_HAS(m, "book-mo-grid");
    CHECK_HAS(m, "book-mo-b hued book-first");
    th_halt();
}

TEST(book_quick_preview_and_widget) {
    setup();
    add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    CHECK_HAS(th_page(adam, "/"), "Bookings"); /* home widget */
    CHECK(th_act(adam, "book.quick", "q=car+tomorrow+9-11").ok);
    const char *home = th_page(adam, "/");
    CHECK_HAS(home, "Tomorrow");
    CHECK_HAS(home, "09:00–11:00");
    th_halt();
}

TEST(book_running_booking_can_be_extended_or_ended_early) {
    setup(); /* Wed 7 Oct 10:00 */
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    Booking bk = {.res = car, .who = adam, .by = adam, .start = dt_make(2026, 10, 7, 9, 0), .end = dt_make(2026, 10, 7, 11, 0)};
    tbl_put(&T_booking, &bk);
    char form[200];
    snprintf(form, sizeof form, "id=%d&res=%d&date=2026-10-07&from=09:00&to=12:00", bk.id, car);
    ThResult r = th_act(adam, "book.create", form); /* longer: same start */
    CHECK(r.ok);
    CHECK_INT(at(book_get(bk.id)->end).hour, 12);
    snprintf(form, sizeof form, "id=%d&res=%d&date=2026-10-07&from=09:00&to=10:30", bk.id, car);
    CHECK(th_act(adam, "book.create", form).ok); /* back early */
    CHECK_INT(at(book_get(bk.id)->end).min, 30);
    snprintf(form, sizeof form, "id=%d&res=%d&date=2026-10-07&from=08:00&to=10:30", bk.id, car);
    r = th_act(adam, "book.create", form); /* the start itself can't move into the past */
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "already started");
    snprintf(form, sizeof form, "res=%d&date=2026-10-07&from=08:30&to=09:30", car);
    CHECK(!th_act(eva, "book.create", form).ok); /* nor can a new booking start there */

    /* a guest-room stay already under way: shorten it, then extend it */
    int guest = add_res("Guest room", "🛏️", 1440, 0, 1440, 1440, 0);
    Booking stay = {.res = guest, .who = eva, .by = eva, .start = dt_make(2026, 10, 5, 0, 0), .end = dt_make(2026, 10, 10, 0, 0)};
    tbl_put(&T_booking, &stay);
    snprintf(form, sizeof form, "id=%d&res=%d&date=2026-10-05&date_to=2026-10-07", stay.id, guest);
    CHECK(th_act(eva, "book.create", form).ok);
    CHECK_INT(book_get(stay.id)->end, dt_make(2026, 10, 8, 0, 0));
    snprintf(form, sizeof form, "id=%d&res=%d&date=2026-10-05&date_to=2026-10-11", stay.id, guest);
    CHECK(th_act(eva, "book.create", form).ok);
    CHECK_INT(book_get(stay.id)->end, dt_make(2026, 10, 12, 0, 0));
    CHECK_INT(book_get(stay.id)->start, dt_make(2026, 10, 5, 0, 0));
    th_halt();
}

TEST(book_coming_up_shows_the_soonest_first_even_with_many_ahead) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    for (int i = 0; i < 70; i++) { /* booked months ahead, all created before today's */
        Booking later = {.res = car, .who = eva, .by = eva, .start = dt_make(2026, 12, 1 + i, 8, 0),
                       .end = dt_make(2026, 12, 1 + i, 9, 0)};
        str_copy(later.note, sizeof later.note, "far ahead");
        tbl_put(&T_booking, &later);
    }
    Booking soon = {.res = car, .who = adam, .by = adam, .start = dt_make(2026, 10, 7, 15, 0), .end = dt_make(2026, 10, 7, 16, 0)};
    str_copy(soon.note, sizeof soon.note, "dentist run");
    tbl_put(&T_booking, &soon);
    const char *o = th_page(adam, "/book");
    CHECK_HAS(o, "dentist run");
    CHECK(strstr(o, "dentist run") < strstr(o, "far ahead")); /* and in order */
    CHECK_HAS(th_page(adam, "/"), "dentist run"); /* the home widget lists only five */
    th_halt();
}

TEST(book_grid_hour_labels_when_opening_on_the_half_hour) {
    setup();
    int sauna = add_res("Sauna", "🧖", 30, 7 * 60 + 30, 22 * 60, 90, 0);
    char url[64];
    snprintf(url, sizeof url, "/book/%d?w=2026-10-05", sauna);
    const char *w = th_page(adam, url);
    CHECK_HAS(w, "<span style=\"--top:1\">08:00</span>"); /* row 0 is 07:30, row 1 is 08:00 */
    CHECK_HAS(w, "<span style=\"--top:27\">21:00</span>");
    CHECK(!strstr(w, ">07:00</span>"));
    th_halt();
}

TEST(book_next_free_on_the_days_the_clocks_change) {
    setup();
    Resource *r = res_get(add_res("Laundry", "🧺", 60, 7 * 60, 22 * 60, 180, 0));
    int64_t s = 0;
    CHECK(book_next_free(r, dt_make(2026, 10, 25, 6, 0), 3 * 3600, 0, &s)); /* clocks went back last night */
    CHECK_INT(at(s).day, 25);
    CHECK_INT(at(s).hour, 7);
    CHECK_INT(at(s).min, 0);
    CHECK(book_next_free(r, dt_make(2026, 3, 29, 6, 0), 3 * 3600, 0, &s)); /* clocks went forward */
    CHECK_INT(at(s).day, 29);
    CHECK_INT(at(s).hour, 7);
    CHECK(book_next_free(r, dt_make(2026, 10, 24, 21, 0), 3 * 3600, 0, &s)); /* the evening before: next morning */
    CHECK_INT(at(s).day, 25);
    CHECK_INT(at(s).hour, 7);
    th_halt();
}

TEST(book_next_free_offers_today_for_whole_day_things) {
    setup(); /* Wed 7 Oct 10:00 */
    int guest = add_res("Guest room", "🛏️", 1440, 0, 1440, 1440, 0);
    int64_t s = 0;
    CHECK(book_next_free(res_get(guest), g_fake_now, 86400, 0, &s));
    CHECK_INT(s, dt_make(2026, 10, 7, 0, 0));
    char url[64], want[80];
    snprintf(url, sizeof url, "/book/%d", guest);
    snprintf(want, sizeof want, "href=\"/book/%d?w=2026-10-07&new=%lld\"", guest, (long long)dt_make(2026, 10, 7, 0, 0));
    CHECK_HAS(th_page(adam, url), want); /* the page's "Book" button starts today */
    Booking b = {.res = guest, .who = eva, .start = dt_make(2026, 10, 7, 0, 0), .end = dt_make(2026, 10, 9, 0, 0)};
    tbl_put(&T_booking, &b);
    CHECK(book_next_free(res_get(guest), g_fake_now, 86400, 0, &s));
    CHECK_INT(s, dt_make(2026, 10, 9, 0, 0));
    th_halt();
}

TEST(book_daily_max_is_clamped_to_whole_days) {
    setup();
    CHECK(th_act(adam, "book.res_save", "name=Guest+room&kind=daily&max=3000000000").ok);
    CHECK_INT(res_get(1)->max_min, 30 * 1440); /* no int overflow into "no limit" */
    CHECK(th_act(adam, "book.res_save", "id=1&name=Guest+room&kind=daily&max=4400").ok);
    CHECK_INT(res_get(1)->max_min, 3 * 1440); /* 4400 min: whole days, rounded down */
    CHECK(th_act(adam, "book.res_save", "id=1&name=Guest+room&kind=daily&max=1000").ok);
    CHECK_INT(res_get(1)->max_min, 1440); /* under a day: still a limit */
    CHECK(th_act(adam, "book.res_save", "id=1&name=Guest+room&kind=daily&max=0").ok);
    CHECK_INT(res_get(1)->max_min, 0);
    th_halt();
}

TEST(book_times_snap_to_the_slot_grid) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    char form[200];
    snprintf(form, sizeof form, "res=%d&start=%lld&end=%lld", car, (long long)dt_make(2026, 10, 8, 10, 7),
             (long long)dt_make(2026, 10, 8, 10, 13));
    CHECK(th_act(adam, "book.create", form).ok);
    Booking *b = book_get(1);
    CHECK_INT(b->start, dt_make(2026, 10, 8, 10, 0));
    CHECK_INT(b->end, dt_make(2026, 10, 8, 10, 30));
    snprintf(form, sizeof form, "res=%d&date=2026-10-08&from=14:10&to=15:50", car); /* typed times too */
    CHECK(th_act(adam, "book.create", form).ok);
    b = book_get(2);
    CHECK_INT(b->start, dt_make(2026, 10, 8, 14, 0));
    CHECK_INT(b->end, dt_make(2026, 10, 8, 16, 0));
    th_halt();
}
