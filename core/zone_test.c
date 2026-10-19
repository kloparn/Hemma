/* The time zone and the times Hemma keeps (issue #39): days stay their days, typed times can be read again. */
#include "../tests/harness.h"
#include "plat.h"
#include "zone.h"

static int64_t utc(int y, int m, int d, int hh, int mm) { return days_of_date(y, m, d) * 86400 + hh * 3600 + mm * 60; }

TEST(zone_signatures_read_back) {
    int jan = 1, jul = 1;
    CHECK(zone_offsets("+00:00", &jan, &jul));
    CHECK_INT(jan, 0);
    CHECK_INT(jul, 0);
    CHECK(zone_offsets("+01:00/+02:00", &jan, &jul));
    CHECK_INT(jan, 3600);
    CHECK_INT(jul, 7200);
    CHECK(zone_offsets("-03:30", &jan, &jul));
    CHECK_INT(jan, -12600);
    CHECK(!zone_offsets("", &jan, &jul));
    CHECK(!zone_offsets("CET", &jan, &jul));
    char w[80];
    zone_words("+01:00/+02:00", w, sizeof w);
    CHECK_STR(w, "UTC+01:00 in January, UTC+02:00 in July");
    zone_words("+00:00", w, sizeof w);
    CHECK_STR(w, "UTC+00:00 all year");
    int y, m, d;
    date_of_days(days_of_date(2028, 2, 29), &y, &m, &d);
    CHECK(y == 2028 && m == 2 && d == 29);
    date_of_days(-1, &y, &m, &d);
    CHECK(y == 1969 && m == 12 && d == 31);
}

TEST(zone_days_keep_their_day) {
    plat_set_tz("UTC0"); /* the zone now; the old one had summer time (Stockholm) */
    Rezone z = {.days = true, .jan = 3600, .jul = 7200};
    int64_t t = utc(2026, 6, 24, 22, 0); /* midnight of 25 June in Stockholm, in summer */
    CHECK(rezone_day(&z, &t));
    CHECK(t == utc(2026, 6, 25, 0, 0));
    t = utc(2026, 1, 9, 23, 0); /* midnight of 10 January there, in winter */
    CHECK(rezone_day(&z, &t));
    CHECK(t == utc(2026, 1, 10, 0, 0));
    t = utc(2026, 6, 25, 0, 0); /* a midnight here already: as it is (a second start changes nothing) */
    CHECK(!rezone_day(&z, &t));
    t = utc(2026, 6, 25, 11, 37); /* no midnight there either: not a day */
    CHECK(!rezone_day(&z, &t));
    CHECK(t == utc(2026, 6, 25, 11, 37));
    t = 0; /* "someday" */
    CHECK(!rezone_day(&z, &t));
    /* and the other way: a Pi on UTC, set to UTC+2 */
    plat_set_tz("XXX-2");
    Rezone z2 = {.days = true};
    t = utc(2026, 6, 25, 0, 0);
    CHECK(rezone_day(&z2, &t));
    CHECK(t == utc(2026, 6, 24, 22, 0));
    plat_set_tz(NULL);
}

TEST(zone_typed_times_read_the_same_again) {
    plat_set_tz("XXX-2"); /* a Pi left on UTC, then set to UTC+2: 14:00 was typed, 16:00 shows */
    Rezone z = {.jan = 0, .jul = 0};
    int64_t t = utc(2026, 10, 13, 14, 0);
    CHECK(rezone_time(&z, &t));
    CHECK(t == utc(2026, 10, 13, 12, 0)); /* = 14:00 here */
    CHECK_INT(dt_local(t).hour, 14);
    /* an old zone with summer time, a new one without: the EU's dates tell its summer */
    plat_set_tz("UTC0");
    Rezone eu = {.jan = 3600, .jul = 7200};
    t = utc(2026, 7, 1, 12, 0); /* 14:00 in Stockholm's summer */
    CHECK(rezone_time(&eu, &t));
    CHECK(t == utc(2026, 7, 1, 14, 0));
    t = utc(2026, 1, 15, 12, 0); /* 13:00 in its winter */
    CHECK(rezone_time(&eu, &t));
    CHECK(t == utc(2026, 1, 15, 13, 0));
    t = utc(2026, 3, 29, 0, 30); /* summer time starts on the last Sunday of March, 01:00 UTC */
    CHECK(rezone_time(&eu, &t));
    CHECK(t == utc(2026, 3, 29, 1, 30));
    t = utc(2026, 3, 29, 1, 30);
    CHECK(rezone_time(&eu, &t));
    CHECK(t == utc(2026, 3, 29, 3, 30));
    t = utc(2026, 10, 25, 0, 30); /* ... and ends on the last Sunday of October */
    CHECK(rezone_time(&eu, &t));
    CHECK(t == utc(2026, 10, 25, 2, 30));
    t = utc(2026, 10, 25, 1, 30);
    CHECK(rezone_time(&eu, &t));
    CHECK(t == utc(2026, 10, 25, 2, 30));
    /* the same zone: nothing moves */
    Rezone same = {0};
    t = utc(2026, 10, 13, 14, 0);
    CHECK(!rezone_time(&same, &t));
    plat_set_tz(NULL);
}

TEST(zone_span_moves_in_its_own_pass) {
    plat_set_tz("XXX-2");
    Rezone days = {.days = true}, typed = {.before = 1000};
    int64_t a = utc(2026, 6, 25, 0, 0), b = utc(2026, 6, 26, 0, 0);
    CHECK(!rezone_span(&typed, true, 0, &a, &b)); /* a whole-day thing isn't typed */
    CHECK(rezone_span(&days, true, 0, &a, &b));
    CHECK(a == utc(2026, 6, 24, 22, 0) && b == utc(2026, 6, 25, 22, 0));
    a = utc(2026, 6, 25, 14, 0), b = utc(2026, 6, 25, 15, 0);
    CHECK(!rezone_span(&days, false, 0, &a, &b));       /* typed: not in the days' pass */
    CHECK(!rezone_span(&typed, false, 1000, &a, &b));   /* entered after the change: typed in this zone */
    CHECK(rezone_span(&typed, false, 999, &a, &b));
    CHECK(a == utc(2026, 6, 25, 12, 0) && b == utc(2026, 6, 25, 13, 0));
    Rezone notes = {.before = 1000};
    rezone_note(&notes, "Dentist", utc(2026, 10, 13, 14, 0), utc(2026, 10, 13, 12, 0));
    CHECK_STR(notes.note[0], "Dentist · Tue 13 Oct 16:00 → 14:00");
    rezone_note(&notes, "Car", utc(2026, 10, 13, 23, 0), utc(2026, 10, 13, 21, 0));
    CHECK_STR(notes.note[1], "Car · Wed 14 Oct 01:00 → Tue 13 Oct 23:00");
    plat_set_tz(NULL);
}

TEST(zone_change_is_seen_at_the_start) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    zone_test_sig("+00:00");
    th_boot_in(dir);
    ZoneChange zc;
    CHECK(!zone_change(&zc)); /* the first start: nothing kept in another zone */
    th_halt();
    th_boot_in(dir); /* the same zone again */
    CHECK(!zone_change(&zc));
    th_halt();
    zone_test_sig("+01:00/+02:00");
    g_fake_now = utc(2026, 10, 12, 8, 0);
    th_boot_in(dir);
    CHECK(zone_change(&zc));
    CHECK_STR(zc.from, "+00:00");
    CHECK_STR(zc.to, "+01:00/+02:00");
    CHECK(zc.at == g_fake_now);
    CHECK_INT(zc.retimed, -1);
    Rezone z;
    CHECK_INT(zone_retime(true, &z), 0); /* nothing typed: none moved, and it's done */
    CHECK(zone_change(&zc) && zc.retimed == 0);
    zone_seen();
    CHECK(!zone_change(&zc));
    th_halt();
    /* a record from 0.14 ("<from>|<to>|<day>") still reads */
    th_boot_in(dir);
    setting_set("system.tz_changed", "+00:00|+01:00/+02:00|2026-10-12");
    CHECK(zone_change(&zc));
    CHECK_STR(zc.day, "2026-10-12");
    CHECK(zc.at == dt_make(2026, 10, 12, 0, 0));
    CHECK_INT(zc.retimed, -1);
    g_fake_now = 0;
    zone_test_sig("");
    th_halt();
}
