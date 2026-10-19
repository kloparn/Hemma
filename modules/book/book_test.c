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

static int news_for(int person, const char *has) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_book_news); i++) {
        BookNews *x = tbl_at(&T_book_news, i);
        n += x->person == person && (!has || strstr(x->text, has));
    }
    return n;
}

static int last_booking(void) {
    int n = tbl_count(&T_booking);
    return n ? ((Booking *)tbl_at(&T_booking, n - 1))->id : 0;
}

/* Someone else cancelling or moving your booking tells you, and it waits until you've seen it
   (issue #43); a thing can be kept to whoever it's for, whoever booked it, and admins. */
TEST(book_changes_by_others_are_told_and_can_be_kept_to_theirs) {
    setup();
    int laundry = add_res("Laundry", "🧺", 60, 7 * 60, 22 * 60, 120, 0);
    char form[200];
    CHECK(th_act(eva, "book.quick", "q=laundry+thu+10-12").ok);
    int bk = last_booking();
    /* Adam moves it: Eva hears, with where it went and where it was */
    snprintf(form, sizeof form, "res=%d&id=%d&date=2026-10-08&from=13:00&to=15:00&who=%d", laundry, bk, eva);
    CHECK(th_act(adam, "book.create", form).ok);
    CHECK_INT(news_for(eva, "Adam moved your Laundry booking · now "), 1);
    CHECK_INT(news_for(eva, "(was "), 1);
    CHECK_INT(news_for(adam, NULL), 0);
    const char *html = th_page(eva, "/book");
    CHECK_HAS(html, "id=\"book-news\"");
    CHECK_HAS(html, "Adam moved your Laundry booking");
    CHECK_HAS(th_page(eva, "/"), "Adam moved your Laundry booking"); /* the home card too */
    CHECK(!strstr(th_page(adam, "/book"), "book-news"));
    /* a change of note only isn't a move */
    snprintf(form, sizeof form, "res=%d&id=%d&date=2026-10-08&from=13:00&to=15:00&who=%d&note=towels", laundry, bk, eva);
    CHECK(th_act(adam, "book.create", form).ok);
    CHECK_INT(news_for(eva, NULL), 1);
    CHECK(th_act(eva, "book.news_seen", "").ok);
    CHECK_INT(news_for(eva, NULL), 0);
    CHECK(!strstr(th_page(eva, "/book"), "book-news"));
    /* Adam cancels it: told */
    snprintf(form, sizeof form, "id=%d", bk);
    CHECK(th_act(adam, "book.cancel", form).ok);
    CHECK_INT(news_for(eva, "Adam cancelled your Laundry booking · "), 1);
    /* your own: nothing to tell */
    CHECK(th_act(eva, "book.news_seen", "").ok);
    CHECK(th_act(eva, "book.quick", "q=laundry+thu+10-12").ok);
    snprintf(form, sizeof form, "id=%d", last_booking());
    CHECK(th_act(eva, "book.cancel", form).ok);
    CHECK_INT(news_for(eva, NULL), 0);
    /* booked by Adam for Eva, cancelled by Eva: Adam hears */
    CHECK(th_act(adam, "book.quick", "q=laundry+thu+10-12+@eva").ok);
    snprintf(form, sizeof form, "id=%d", last_booking());
    CHECK(th_act(eva, "book.cancel", form).ok);
    CHECK_INT(news_for(adam, "Eva cancelled the Laundry booking you made · "), 1);
    CHECK_INT(news_for(eva, NULL), 0);

    /* the rule: only theirs (and admins') to change - set by an admin only */
    int anna = th_person("Anna");
    Person a = *person_get(anna);
    a.role = ROLE_ADMIN;
    tbl_put(&T_person, &a);
    snprintf(form, sizeof form, "id=%d&name=Laundry&kind=hourly&slot=60&open=07:00&close=22:00&own_only=1", laundry);
    CHECK(th_act(adam, "book.res_save", form).ok); /* (a member saves the rest; the rule stays) */
    CHECK(!res_get(laundry)->own_only);
    CHECK(!strstr(th_page(adam, "/book/1?res=1"), "name=\"own_only\""));
    CHECK(th_act(anna, "book.res_save", form).ok);
    CHECK(res_get(laundry)->own_only);
    CHECK_HAS(th_page(anna, "/book/1?res=1"), "name=\"own_only\" value=\"1\" checked");
    CHECK_HAS(th_page(adam, "/book/1?res=1"), "(an admin's choice)");
    CHECK(th_act(eva, "book.quick", "q=laundry+thu+10-12").ok);
    bk = last_booking();
    snprintf(form, sizeof form, "id=%d", bk);
    ThResult r = th_act(adam, "book.cancel", form);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Only Eva or an admin can cancel this booking");
    snprintf(form, sizeof form, "res=%d&id=%d&date=2026-10-08&from=13:00&to=15:00&who=%d", laundry, bk, eva);
    r = th_act(adam, "book.create", form);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Only Eva or an admin can move this booking");
    char url[64];
    snprintf(url, sizeof url, "/book/1?b=%d", bk);
    html = th_page(adam, url);
    CHECK_HAS(html, "Only Eva or an admin can cancel or move it.");
    CHECK(!strstr(html, "Cancel booking"));
    snprintf(url, sizeof url, "/book/1?b=%d&edit=1", bk);
    CHECK(!strstr(th_page(adam, url), "name=\"from\"")); /* no way in through the address either */
    CHECK_HAS(th_page(eva, url), "name=\"from\"");
    snprintf(form, sizeof form, "res=%d&id=%d&date=2026-10-08&from=13:00&to=15:00&who=%d", laundry, bk, eva);
    CHECK(th_act(anna, "book.create", form).ok); /* an admin may */
    CHECK_INT(news_for(eva, "Anna moved your Laundry booking"), 1);
    snprintf(form, sizeof form, "id=%d", bk);
    CHECK(th_act(eva, "book.cancel", form).ok); /* and the person it's for */
    th_halt();
}

/* The hint's own "bastu ikväll 19" on a fresh install, whose starter is called "Sauna" (issue #55): the
   starters know their Swedish names, and any thing can be given more. */
TEST(book_quick_parse_knows_other_names) {
    setup();
    CHECK(th_act(adam, "book.preset", "name=Sauna").ok);
    CHECK(th_act(adam, "book.preset", "name=Laundry").ok);
    CHECK(th_act(adam, "book.preset", "name=TV+room").ok);
    int sauna = 1, laundry = 2, tvroom = 3;
    Quick q;
    book_quick_parse("bastu ikväll 19", 0, g_fake_now, &q);
    CHECK_INT(q.res, sauna);
    CHECK_STR(q.problem, "");
    CHECK_INT(at(q.start).day, 7);
    CHECK_INT(at(q.start).hour, 19);
    book_quick_parse("bastun imorgon 7", 0, g_fake_now, &q); /* a form of the word */
    CHECK_INT(q.res, sauna);
    book_quick_parse("tvätt imorgon 19-20", 0, g_fake_now, &q);
    CHECK_INT(q.res, laundry);
    CHECK_STR(q.problem, "");
    CHECK_STR(q.note, "");
    book_quick_parse("tvättstugan lör 10-12", 0, g_fake_now, &q);
    CHECK_INT(q.res, laundry);
    book_quick_parse("tv-rum fre 20-22", 0, g_fake_now, &q); /* one token with a dash */
    CHECK_INT(q.res, tvroom);
    book_quick_parse("tv room fri 20-22", 0, g_fake_now, &q);
    CHECK_INT(q.res, tvroom);

    /* a thing's own other names, from its edit form */
    book_quick_parse("stugan lör 10-12", 0, g_fake_now, &q);
    CHECK_INT(q.res, 0);
    CHECK_STR(q.problem, "I don't know “stugan”. You can book: Sauna, Laundry, TV room.");
    CHECK(th_act(adam, "book.res_save", "id=1&name=Sauna&kind=hourly&always=1&aliases=+stugan%2C+the+cabin+").ok);
    CHECK_STR(res_get(sauna)->aliases, "stugan, the cabin");
    book_quick_parse("stugan lör 10-12", 0, g_fake_now, &q);
    CHECK_INT(q.res, sauna);
    book_quick_parse("the cabin sat 10-12", 0, g_fake_now, &q);
    CHECK_INT(q.res, sauna);
    CHECK_STR(q.note, "");
    const char *html = th_page(adam, "/book/1?res=1");
    CHECK_HAS(html, "name=\"aliases\" maxlength=\"110\" value=\"stugan, the cabin\"");
    CHECK_HAS(html, "It already knows “bastu”.");
    /* a renamed starter is the household's own: only its own other names count */
    CHECK(th_act(adam, "book.res_save", "id=1&name=Bastun&kind=hourly&always=1").ok);
    book_quick_parse("bastu ikväll 19", 0, g_fake_now, &q);
    CHECK_INT(q.res, sauna); /* (by its new name, "Bastun") */
    CHECK(th_act(adam, "book.res_save", "id=1&name=Hot+room&kind=hourly&always=1").ok);
    book_quick_parse("bastu ikväll 19", 0, g_fake_now, &q);
    CHECK_INT(q.res, 0);
    CHECK(!strstr(th_page(adam, "/book/1?res=1"), "It already knows"));
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

    book_quick_parse("boat fri 10-12", 0, g_fake_now, &q); /* a word that names nothing: say so, and what can be */
    CHECK_INT(q.res, 0);
    CHECK_STR(q.problem, "I don't know “boat”. You can book: Car, Tvättstuga, Guest room.");
    book_quick_parse("fri 10-12", 0, g_fake_now, &q); /* no word at all */
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

TEST(book_wall_card_only_shows_and_says_what_it_leaves_out) {
    /* issue #80: the wall's rows were links into the booking page (and a tap on the hallway tablet
       left the wall), the thing's own line dropped the time ("until Tomorrow"); issue #53: bookings
       beyond the five shown vanished without a word */
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    Booking now = {.res = car, .who = eva, .by = eva, .start = dt_make(2026, 10, 7, 9, 0), .end = dt_make(2026, 10, 8, 2, 0)};
    tbl_put(&T_booking, &now);
    for (int i = 0; i < 7; i++) {
        Booking later = {.res = car, .who = adam, .by = adam, .start = dt_make(2026, 10, 9 + i, 8, 0),
                         .end = dt_make(2026, 10, 9 + i, 9, 0)};
        tbl_put(&T_booking, &later);
    }
    Ctx c;
    app_ctx(&c, 0, "/screens/2");
    Buf b = {0};
    CHECK(block_find("book.now", NULL)->render(&c, &b, SCREEN_WALL, ""));
    const char *h = buf_cstr(&b);
    CHECK_HAS(h, "Eva · until Tomorrow 02:00");
    CHECK(!strstr(h, "href="));
    CHECK_HAS(h, "<div class=\"book-up-row");
    CHECK_HAS(h, "data-fit-more=\"3\">+3 more</li>"); /* 8 coming, 5 shown */
    buf_free(&b);
    app_ctx_free(&c);
    /* the home card's rows still open the booking, and its "+N more" leads to Book */
    const char *home = th_page(adam, "/");
    CHECK_HAS(home, "<a class=\"book-up-row");
    CHECK_HAS(home, "<a href=\"/book\">+3 more</a>");
    th_halt();
}

/* Issue #39: the guest room booked for 24-26 October stays those days when the zone changes; the car
   typed for 14:00-17:00 under UTC shows two hours later until an admin moves it back. */
TEST(book_keeps_days_when_the_zone_changes) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    plat_set_tz("UTC0");
    g_fake_now = dt_make(2026, 10, 7, 10, 0);
    th_boot_in(dir);
    adam = th_person("Adam");
    int room = add_res("Guest room", "🛏️", 1440, 0, 1440, 1440, 0), car = add_res("Car", "🚗", 60, 0, 1440, 120, 0);
    Booking stay = {.res = room, .who = adam, .by = adam, .created = g_fake_now};
    stay.start = dt_make(2026, 10, 24, 0, 0);
    stay.end = dt_make(2026, 10, 27, 0, 0);
    tbl_put(&T_booking, &stay);
    Booking drive = {.res = car, .who = adam, .by = adam, .created = g_fake_now};
    drive.start = dt_make(2026, 10, 13, 14, 0);
    drive.end = dt_make(2026, 10, 13, 17, 0);
    tbl_put(&T_booking, &drive);
    /* repeating ones (issue #47): what the bookings still to come are made from moves the same way */
    BookSeries weekend = {.res = room, .who = adam, .by = adam, .repeat = REPEAT_WEEKLY, .made = 1, .created = g_fake_now};
    weekend.start = dt_make(2026, 10, 31, 0, 0);
    weekend.end = dt_make(2026, 11, 1, 0, 0);
    weekend.until = dt_make(2026, 12, 26, 0, 0);
    tbl_put(&T_book_series, &weekend);
    BookSeries commute = {.res = car, .who = adam, .by = adam, .repeat = REPEAT_DAILY, .made = 1, .created = g_fake_now};
    commute.start = dt_make(2026, 10, 13, 8, 0);
    commute.end = dt_make(2026, 10, 13, 9, 0);
    commute.until = dt_make(2026, 10, 15, 0, 0);
    tbl_put(&T_book_series, &commute);
    th_halt();
    plat_set_tz("XXX-2");
    g_fake_now += 86400;
    th_boot_in(dir);
    const BookSeries *ws = tbl_get(&T_book_series, weekend.id);
    CHECK(ws && ws->start == dt_make(2026, 10, 31, 0, 0) && ws->end == dt_make(2026, 11, 1, 0, 0));
    CHECK(ws && ws->until == dt_make(2026, 12, 26, 0, 0));
    const Booking *s = book_get(stay.id);
    CHECK(s && s->start == dt_make(2026, 10, 24, 0, 0) && s->end == dt_make(2026, 10, 27, 0, 0));
    const Booking *dr = book_get(drive.id);
    CHECK(dr && at(dr->start).hour == 16 && at(dr->end).hour == 19);
    /* the commute's 14 and 15 Oct were made at this start - from its times as typed under UTC */
    int made14 = 0, made15 = 0;
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        const Booking *x = tbl_at(&T_booking, i);
        if (x->series == commute.id && at(x->start).day == 14) made14 = x->id;
        if (x->series == commute.id && at(x->start).day == 15) made15 = x->id;
    }
    CHECK(made14 && made15 && at(book_get(made14)->start).hour == 10);
    Rezone z;
    CHECK_INT(zone_retime(false, &z), 3); /* the car on the 13th, and the two the series made */
    CHECK_STR(z.note[0], "Car · Tue 13 Oct 16:00 → 14:00");
    CHECK_STR(z.note[1], "Car · Wed 14 Oct 10:00 → 08:00");
    CHECK_INT(zone_retime(true, &z), 3);
    dr = book_get(drive.id);
    CHECK(dr && at(dr->start).hour == 14 && at(dr->end).hour == 17);
    CHECK(made15 && at(book_get(made15)->start).hour == 8);
    CHECK(at(((const BookSeries *)tbl_get(&T_book_series, commute.id))->start).hour == 8);
    g_fake_now = 0;
    plat_set_tz(NULL);
    th_halt();
}

/* ------------------------------------------ repeating bookings (issue #47) */

static int series_bookings(int series) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_booking); i++) n += ((Booking *)tbl_at(&T_booking, i))->series == series;
    return n;
}

static Booking *booking_at(int res, int64_t start) {
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        Booking *b = tbl_at(&T_booking, i);
        if (b->res == res && b->start == start) return b;
    }
    return NULL;
}

TEST(book_repeats_every_week) {
    setup(); /* Wednesday 7 October */
    int laundry = add_res("Laundry", "🧺", 60, 7 * 60, 22 * 60, 180, 240);
    Booking taken = {.res = laundry, .who = eva, .by = eva, .created = g_fake_now}; /* Eva has Tuesday 3 Nov */
    taken.start = dt_make(2026, 11, 3, 18, 0);
    taken.end = dt_make(2026, 11, 3, 21, 0);
    tbl_put(&T_booking, &taken);
    Quick q;
    book_quick_parse("laundry every tuesday 18-21", 0, g_fake_now, &q);
    CHECK(q.res == laundry && q.repeat == REPEAT_WEEKLY && !q.problem[0]);
    CHECK(q.start == dt_make(2026, 10, 13, 18, 0) && q.end == dt_make(2026, 10, 13, 21, 0));
    Buf lab = {0};
    BookSeries probe = {.start = q.start, .repeat = q.repeat};
    book_series_label(&lab, &probe);
    CHECK_STR(lab.p, "every Tuesday");
    buf_free(&lab);
    const char *h;
    ThResult r = th_act(adam, "book.quick", "q=laundry+every+tuesday+18-21");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "every Tuesday - the next 8 weeks are booked");
    CHECK_INT(tbl_count(&T_book_series), 1);
    int sid = ((BookSeries *)tbl_at(&T_book_series, 0))->id;
    /* 13, 20, 27 Oct, 10, 17, 24 Nov, 1 Dec - 3 Nov was Eva's: left out, and Adam is told */
    CHECK_INT(series_bookings(sid), 7);
    Booking *nov3 = booking_at(laundry, dt_make(2026, 11, 3, 18, 0));
    CHECK(nov3 && nov3->who == eva);
    CHECK_INT(tbl_count(&T_book_news), 1);
    const BookNews *told = tbl_count(&T_book_news) ? tbl_at(&T_book_news, 0) : NULL;
    CHECK(told && told->person == adam);
    CHECK_HAS(told ? told->text : "", "Your Laundry every Tuesday: Tue 3 Nov 18:00–21:00 isn't booked");
    h = th_page(adam, "/book");
    CHECK_HAS(h, "Your Laundry every Tuesday: Tue 3 Nov");
    CHECK_HAS(h, "book-rep");
    /* a week on: one more */
    g_fake_now += 7 * 86400;
    CHECK_INT(book_series_fill(g_fake_now), 1);
    CHECK(booking_at(laundry, dt_make(2026, 12, 8, 18, 0)) != NULL);
    /* one cancelled on its own; the series goes on */
    Booking *oct20 = booking_at(laundry, dt_make(2026, 10, 20, 18, 0));
    CHECK(oct20 != NULL);
    int oct20_id = oct20 ? oct20->id : 0;
    char form[40], url[60];
    snprintf(url, sizeof url, "/book/%d?w=2026-10-20&b=%d", laundry, oct20_id);
    h = th_page(adam, url);
    CHECK_HAS(h, "Repeats every Tuesday");
    CHECK_HAS(h, "Cancel this one");
    CHECK_HAS(h, "Stop repeating");
    snprintf(form, sizeof form, "id=%d", oct20_id);
    CHECK(th_act(adam, "book.cancel", form).ok);
    CHECK(booking_at(laundry, dt_make(2026, 10, 20, 18, 0)) == NULL);
    CHECK(booking_at(laundry, dt_make(2026, 10, 27, 18, 0)) != NULL);
    /* stopped from 17 Nov: that one and the later ones go, the earlier stay */
    Booking *nov17 = booking_at(laundry, dt_make(2026, 11, 17, 18, 0));
    snprintf(form, sizeof form, "id=%d&later=1", nov17 ? nov17->id : 0);
    r = th_act(eva, "book.cancel", form);
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Stopped repeating: 4 Laundry bookings cancelled");
    CHECK(booking_at(laundry, dt_make(2026, 11, 10, 18, 0)) != NULL);
    CHECK(booking_at(laundry, dt_make(2026, 11, 24, 18, 0)) == NULL);
    CHECK(((BookSeries *)tbl_get(&T_book_series, sid))->stopped);
    g_fake_now += 30 * 86400;
    CHECK_INT(book_series_fill(g_fake_now), 0);
    g_fake_now = 0;
    th_halt();
}

TEST(book_repeats_from_the_form_until_a_day) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 60, 0);
    char form[200];
    snprintf(form, sizeof form, "res=%d&date=2026-10-12&from=08%%3A00&to=09%%3A00&rep=1&rep_until=2026-10-16", car);
    CHECK(th_act(adam, "book.create", form).ok);
    int sid = ((BookSeries *)tbl_at(&T_book_series, 0))->id;
    CHECK_INT(series_bookings(sid), 5); /* Monday to Friday, and that's it */
    CHECK(((BookSeries *)tbl_get(&T_book_series, sid))->stopped);
    snprintf(form, sizeof form, "res=%d&date=2026-10-20&from=08%%3A00&to=09%%3A00&rep=1&rep_until=2026-10-19", car);
    ThResult r = th_act(adam, "book.create", form);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Repeat until a day on or after the first one");
    /* every month on the 31st: 30 November - and 31 December again (counted from the first) */
    snprintf(form, sizeof form, "res=%d&date=2026-10-31&from=10%%3A00&to=12%%3A00&rep=4", car);
    CHECK(th_act(adam, "book.create", form).ok);
    CHECK(booking_at(car, dt_make(2026, 11, 30, 10, 0)) != NULL);
    g_fake_now = dt_make(2026, 11, 20, 10, 0);
    book_series_fill(g_fake_now);
    CHECK(booking_at(car, dt_make(2026, 12, 31, 10, 0)) != NULL);
    g_fake_now = 0;
    th_halt();
}

/* ------------------------------------------------- children, guests (issue #44) */

static int with_role(int person, int role) {
    Person p = *person_get(person);
    p.role = role;
    tbl_put(&T_person, &p);
    return person;
}

TEST(book_children_and_guests_book_what_is_opened_to_them) {
    setup();
    int leo = with_role(th_person("Leo"), ROLE_CHILD), gun = with_role(th_person("Gun"), ROLE_GUEST);
    CHECK(th_act(adam, "book.preset", "name=Car").ok);
    CHECK(th_act(adam, "book.preset", "name=TV+room").ok);
    CHECK(th_act(adam, "book.preset", "name=Guest+room").ok);
    int car = 1, tvroom = 2, guestroom = 3;
    CHECK(!res_get(car)->kids && !res_get(car)->guests); /* (the starters' own rules) */
    CHECK(res_get(tvroom)->kids && res_get(tvroom)->guests);
    CHECK(!res_get(guestroom)->kids && res_get(guestroom)->guests);
    char f[200];
    snprintf(f, sizeof f, "res=%d&date=2026-10-08&from=14:00&to=16:00", car);
    ThResult r = th_act(leo, "book.create", f);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Ask a grown-up to book the Car");
    r = th_act(gun, "book.create", f);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "isn't open to guests");
    snprintf(f, sizeof f, "res=%d&date=2026-10-08&from=14:00&to=16:00&who=%d", tvroom, eva);
    r = th_act(leo, "book.create", f); /* for themselves only */
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "for yourself");
    snprintf(f, sizeof f, "res=%d&date=2026-10-08&from=14:00&to=16:00", tvroom);
    CHECK(th_act(leo, "book.create", f).ok);
    CHECK_INT(book_get(1)->who, leo);
    CHECK(!th_act(leo, "book.quick", "q=car+fri+10-12").ok);
    CHECK(th_act(gun, "book.quick", "q=guest+room+fri-sun").ok);
    /* someone else's booking stays theirs to change, even where a member may change anyone's */
    snprintf(f, sizeof f, "res=%d&date=2026-10-09&from=18:00&to=20:00", tvroom);
    CHECK(th_act(eva, "book.create", f).ok);
    int evas = last_booking();
    snprintf(f, sizeof f, "id=%d", evas);
    r = th_act(leo, "book.cancel", f);
    CHECK_HAS(r.toast, "Only");
    CHECK(book_get(evas) != NULL);
    snprintf(f, sizeof f, "id=%d", 1);
    CHECK(th_act(leo, "book.cancel", f).ok); /* their own */
    /* what they see: only what they may book */
    const char *h = th_page(leo, "/book");
    CHECK_HAS(h, "TV room");
    CHECK(!strstr(h, "Guest room"));
    CHECK(!strstr(h, "Add a thing"));
    snprintf(f, sizeof f, "/book/%d", car);
    CHECK_HAS(th_page(leo, f), "Not for children");
    CHECK_HAS(th_page(gun, f), "Not open to guests");
    CHECK(!th_act(leo, "book.res_save", "name=Trampoline").ok); /* (an admin's or a member's) */
    r = th_act(gun, "book.preset", "name=Sauna");
    CHECK_HAS(r.toast, "isn't open to guests");
    /* an admin opens the car to children */
    with_role(adam, ROLE_ADMIN);
    snprintf(f, sizeof f, "id=%d&name=Car&kind=hourly&slot=30&always=1&dflt=120&max=0&kids=1", car);
    CHECK(th_act(adam, "book.res_save", f).ok);
    CHECK(res_get(car)->kids && !res_get(car)->guests);
    snprintf(f, sizeof f, "res=%d&date=2026-10-10&from=10:00&to=11:00", car);
    CHECK(th_act(leo, "book.create", f).ok);
    th_halt();
}

TEST(book_someone_who_leaves_keeps_what_happened) {
    setup();
    int car = add_res("Car", "🚗", 30, 0, 1440, 120, 0);
    char f[200];
    g_fake_now = dt_make(2026, 10, 1, 10, 0); /* a week ago: Eva drove */
    snprintf(f, sizeof f, "res=%d&date=2026-10-02&from=10:00&to=12:00", car);
    CHECK(th_act(eva, "book.create", f).ok);
    g_fake_now = dt_make(2026, 10, 7, 10, 0);
    snprintf(f, sizeof f, "res=%d&date=2026-10-09&from=10:00&to=12:00", car);
    CHECK(th_act(eva, "book.create", f).ok);
    CHECK(th_act(eva, "book.quick", "q=car+every+monday+18-19").ok); /* a series: the next weeks too */
    snprintf(f, sizeof f, "res=%d&date=2026-10-10&from=10:00&to=12:00&who=%d", car, adam);
    CHECK(th_act(eva, "book.create", f).ok); /* she booked it for Adam */
    int n = tbl_count(&T_booking);
    Forget fg = {.person = eva, .keep = true, .now = g_fake_now};
    CHECK(person_forget(&fg) > 0); /* only what would happen */
    CHECK_INT(tbl_count(&T_booking), n);
    bool said = false;
    for (int i = 0; i < fg.nnotes; i++) said |= strstr(fg.notes[i], "still to come") != NULL;
    CHECK(said);
    fg = (Forget){.person = eva, .keep = true, .apply = true, .now = g_fake_now};
    person_forget(&fg);
    CHECK_INT(tbl_count(&T_booking), 2); /* the one that happened, and Adam's */
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        const Booking *b = tbl_at(&T_booking, i);
        CHECK(b->who == adam || b->start < g_fake_now);
    }
    CHECK(((BookSeries *)tbl_at(&T_book_series, 0))->stopped);
    CHECK(person_get(eva) && person_get(eva)->left); /* moved out */
    CHECK(!person_real(eva));
    CHECK_INT(book_series_fill(g_fake_now + 30 * 86400), 0); /* nothing more is made for her */
    /* erasing: the rest of hers goes, and what she made for others keeps no "by" */
    fg = (Forget){.person = eva, .apply = true, .now = g_fake_now};
    person_forget(&fg);
    CHECK_INT(tbl_count(&T_booking), 1);
    CHECK_INT(((Booking *)tbl_at(&T_booking, 0))->who, adam);
    CHECK_INT(((Booking *)tbl_at(&T_booking, 0))->by, 0);
    CHECK_INT(tbl_count(&T_book_series), 0);
    CHECK(person_get(eva) == NULL);
    th_halt();
}

/* A thing open until midnight said "Until 22:00" in its dialog, and saving the dialog (to tick "Children may
   book it", say) shortened its hours. */
TEST(book_open_until_midnight_stays_so_when_saved) {
    setup();
    with_role(adam, ROLE_ADMIN);
    CHECK(th_act(adam, "book.preset", "name=Sauna").ok); /* 06:00-24:00 */
    CHECK_INT(res_get(1)->close_min, 1440);
    const char *h = th_page(adam, "/book/1?res=1");
    CHECK_HAS(h, "name=\"close\" value=\"00:00\"");
    CHECK(th_act(adam, "book.res_save", "id=1&name=Sauna&kind=hourly&slot=30&open=06:00&close=00:00&dflt=90&max=180&kids=1").ok);
    CHECK_INT(res_get(1)->open_min, 360);
    CHECK_INT(res_get(1)->close_min, 1440);
    CHECK(res_get(1)->kids);
    th_halt();
}
