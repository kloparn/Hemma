/* A person's calendar on their phone (issue #41): the address and its key, and the iCalendar it serves. */
#include "../tests/harness.h"
#include "auth.h"
#include "plat.h"

static int person_who_signs_in(const char *name, const char *user, int role) {
    Person p = {0};
    str_copy(p.name, sizeof p.name, name);
    str_copy(p.username, sizeof p.username, user);
    str_copy(p.pass, sizeof p.pass, "a-hash"); /* (the feed only asks whether they can sign in) */
    p.role = role;
    p.hue = 200;
    tbl_put(&T_person, &p);
    return p.id;
}

static const char *ics_of(int person, Buf *b) {
    Ctx c;
    app_ctx(&c, person, "/");
    str_copy(c.host, sizeof c.host, "hemma.local");
    buf_reset(b);
    cal_feed_ics(&c, person, b);
    app_ctx_free(&c);
    return b->p;
}

/* the calendar's lines with each fold ("\r\n ") put back together */
static void unfold(const char *s, Buf *out) {
    buf_reset(out);
    for (; *s; s++) {
        if (s[0] == '\r' && s[1] == '\n' && s[2] == ' ') s += 2;
        else buf_putc(out, *s);
    }
}

TEST(feed_address_has_its_own_key) {
    th_boot();
    auth_configure("feed-test-salt", 1000);
    int adam = person_who_signs_in("Adam", "adam", ROLE_MEMBER), eva = person_who_signs_in("Eva", "eva", ROLE_MEMBER);
    char path[96], other[96];
    CHECK(!cal_feed_path(adam, path, sizeof path)); /* off until they make one */
    pref_set(adam, PREF_CAL, "1");
    CHECK(cal_feed_path(adam, path, sizeof path));
    CHECK(str_starts(path, "/cal/"));
    CHECK_INT((int)strlen(path), (int)strlen("/cal/-.ics") + (adam > 9 ? 2 : 1) + 32);
    CHECK_INT(cal_feed_person(path), adam);
    /* one character off, someone else's number with that key, no ".ics", more after it */
    str_copy(other, sizeof other, path);
    char *last = other + strlen(other) - 5;
    *last = *last == '0' ? '1' : '0';
    CHECK_INT(cal_feed_person(other), 0);
    snprintf(other, sizeof other, "/cal/%d%s", eva, strchr(path, '-'));
    CHECK_INT(cal_feed_person(other), 0);
    str_copy(other, sizeof other, path);
    other[strlen(other) - 4] = '\0';
    CHECK_INT(cal_feed_person(other), 0);
    snprintf(other, sizeof other, "%s.ics", path);
    CHECK_INT(cal_feed_person(other), 0);
    /* a new link: the old one stops; off: it stops too */
    pref_set(adam, PREF_CAL, "2");
    CHECK_INT(cal_feed_person(path), 0);
    CHECK(cal_feed_path(adam, other, sizeof other) && !str_eq(other, path));
    CHECK_INT(cal_feed_person(other), adam);
    pref_set(adam, PREF_CAL, "-2");
    CHECK(!cal_feed_path(adam, path, sizeof path));
    CHECK_INT(cal_feed_person(other), 0);
    /* not for a display account (the hall tablet), nor without the server secret */
    int hall = person_who_signs_in("Hall", "hall", ROLE_DISPLAY);
    pref_set(hall, PREF_CAL, "1");
    CHECK(!cal_feed_path(hall, path, sizeof path));
    pref_set(eva, PREF_CAL, "1");
    auth_configure("", 1000);
    CHECK(!cal_feed_path(eva, path, sizeof path));
    th_halt();
}

TEST(feed_holds_what_is_on_for_them) {
    plat_set_tz("UTC0");
    th_boot();
    auth_configure("feed-test-salt", 1000);
    int adam = person_who_signs_in("Adam", "adam", ROLE_ADMIN), eva = person_who_signs_in("Eva", "eva", ROLE_MEMBER);
    g_fake_now = dt_make(2026, 10, 3, 10, 0);
    CHECK(th_act(adam, "plan.task_add", "q=fix+bike+%40adam+5%2F10").ok); /* still open after its day */
    g_fake_now = dt_make(2026, 10, 7, 10, 0); /* Wednesday 7 October 2026 */
    CHECK(th_act(adam, "book.preset", "name=Car").ok);
    CHECK(th_act(adam, "book.quick", "q=car+13%2F10+14-17+ikea").ok);
    CHECK(th_act(adam, "book.quick", "q=car+14%2F10+9-11+%40eva").ok);
    CHECK(th_act(adam, "plan.event_add", "q=dinner%2C+drinks%3B+games+24%2F10").ok);
    CHECK(th_act(adam, "plan.event_add", "q=%C3%85sas+f%C3%B6delsedag+med+t%C3%A5rta+och+kaffe+p%C3%A5+verandan+"
                                         "hela+l%C3%A5nga+eftermiddagen+i+solen+25%2F10")
              .ok);
    CHECK(th_act(adam, "plan.task_add", "q=vacuum+%40adam+9%2F10").ok);
    CHECK(th_act(adam, "plan.task_add", "q=water+plants+%40eva+9%2F10").ok);
    CHECK(th_act(adam, "plan.task_add", "q=pay+the+bills+8%2F10").ok);
    Buf b = {0}, u = {0};
    const char *ics = ics_of(adam, &b);
    CHECK(str_starts(ics, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"));
    CHECK_HAS(ics, "\r\nEND:VCALENDAR\r\n");
    CHECK_HAS(ics, "X-WR-CALNAME:Hemma\r\n");
    /* every line CRLF and at most 75 octets */
    for (const char *line = ics; *line;) {
        const char *end = strstr(line, "\r\n");
        CHECK(end != NULL);
        if (!end) break;
        CHECK(end - line <= 75);
        CHECK(!memchr(line, '\n', (size_t)(end - line)));
        line = end + 2;
    }
    /* their booking, in UTC; Eva's isn't theirs */
    CHECK_HAS(ics, "DTSTART:20261013T140000Z\r\nDTEND:20261013T170000Z\r\n");
    CHECK_HAS(ics, "SUMMARY:🚗 Car · ikea\r\n");
    CHECK(!strstr(ics, "20261014T090000Z"));
    /* the household's events, whole days as dates; escaped text; a long Swedish title folded, not broken */
    CHECK_HAS(ics, "DTSTART;VALUE=DATE:20261024\r\nDTEND;VALUE=DATE:20261025\r\n");
    CHECK_HAS(ics, "SUMMARY:🍳 Dinner drinks games\r\n"); /* (quick add leaves the commas out) */
    unfold(ics, &u);
    CHECK_HAS(u.p, "Åsas födelsedag med tårta och kaffe på verandan hela långa eftermiddagen i solen");
    /* to-dos: theirs and anyone's, on their day, not busy; one still open shows today, saying since when */
    CHECK_HAS(u.p, "DTSTART;VALUE=DATE:20261009\r\nDTEND;VALUE=DATE:20261010\r\nSUMMARY:🧹 Vacuum\r\n");
    CHECK_HAS(u.p, "Pay the bills");
    CHECK(!strstr(u.p, "Water plants"));
    CHECK_HAS(u.p, "DTSTART;VALUE=DATE:20261007\r\nDTEND;VALUE=DATE:20261008\r\nSUMMARY:");
    CHECK_HAS(u.p, "Fix bike (due Mon 5 Oct)");
    CHECK_HAS(ics, "TRANSP:TRANSPARENT");
    CHECK_HAS(ics, "URL:http://hemma.local/book/");
    CHECK_HAS(ics, "DESCRIPTION:Booking for you - Hemma");
    /* the same things keep their UIDs from one fetch to the next */
    Buf first = {0};
    buf_puts(&first, ics);
    const char *uid = strstr(first.p, "UID:");
    CHECK(uid != NULL);
    char one[40] = "";
    if (uid) sscanf(uid, "UID:%39[^\r]", one);
    CHECK(one[0]);
    CHECK_HAS(ics_of(adam, &b), one);
    /* every booking, if they want it: Eva's shows, with her name */
    pref_set(adam, PREF_CAL_ALL, "1");
    ics = ics_of(adam, &b);
    CHECK_HAS(ics, "DTSTART:20261014T090000Z");
    CHECK_HAS(ics, "SUMMARY:🚗 Car (Eva)\r\n");
    CHECK(!strstr(ics, "Water plants")); /* (bookings only - not everyone's to-dos) */
    /* text is escaped: the household's name, with a semicolon and a comma */
    setting_set("household.name", "Björk; by the lake, north");
    ics = ics_of(adam, &b);
    CHECK_HAS(ics, "X-WR-CALNAME:Björk\\; by the lake\\, north\r\n");
    CHECK_HAS(ics, "DESCRIPTION:Booking for you - Björk\\; by the lake\\, north\r\n");
    /* in their language (issue #46: cal_feed_http picks it - their own choice, else the app's) */
    pref_set(adam, PREF_LANG, "sv");
    ics = ics_of(adam, &b);
    pref_set(adam, PREF_LANG, "");
    unfold(ics, &u); /* (longer in Swedish: folded at 75 octets) */
    CHECK_HAS(u.p, "DESCRIPTION:Bokning åt dig - Björk\\; by the lake\\, north\r\n");
    /* Eva's own calendar has her booking and her to-do */
    unfold(ics_of(eva, &b), &u);
    CHECK_HAS(u.p, "20261014T090000Z");
    CHECK_HAS(u.p, "Water plants");
    CHECK(!strstr(u.p, "Vacuum"));
    buf_free(&b);
    buf_free(&u);
    buf_free(&first);
    g_fake_now = 0;
    plat_set_tz(NULL);
    th_halt();
}
