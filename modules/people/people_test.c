#include "../../core/auth.h"
#include "../../tests/harness.h"

static void boot(void) {
    th_boot();
    auth_configure("test-server-salt", 1000);
}

static int admin(const char *name, const char *user) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash("correct horse", p.pass, sizeof p.pass);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

TEST(people_admin_adds_people_with_and_without_sign_in) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Love&hue=10").ok); /* a toddler: no sign-in */
    Person *love = person_get(2);
    CHECK(love && !love->username[0] && !love->pass[0]);
    ThResult r = th_act(adam, "people.save", "name=Eva&username=Eva&password=short&role=0");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "8 characters");
    CHECK(!th_act(adam, "people.save", "name=Eva&username=eva").ok);                       /* needs a password */
    CHECK(!th_act(adam, "people.save", "name=Eva&username=e+v&password=longenough1").ok);  /* bad username */
    CHECK(!th_act(adam, "people.save", "name=Eva&password=longenough1").ok);               /* password, no user */
    CHECK(th_act(adam, "people.save", "name=Eva&username=Eva&password=longenough1&role=0").ok);
    Person *eva = person_by_username("eva");
    CHECK(eva != NULL);
    CHECK_STR(eva->username, "eva");
    CHECK(auth_check("longenough1", eva->pass));
    CHECK(!th_act(adam, "people.save", "name=Eve&username=eva&password=longenough2").ok); /* username taken */
    CHECK(!th_act(adam, "people.save", "name=adam&hue=1").ok);                            /* name taken */
    th_halt();
}

TEST(people_members_can_only_edit_themselves) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Eva&username=eva&password=longenough1").ok);
    int eva = person_by_username("eva")->id;
    char f[160];
    snprintf(f, sizeof f, "id=%d&name=Eva+B&hue=40&role=1&username=evil&password=hackhackhack", eva);
    CHECK(th_act(eva, "people.save", f).ok); /* her own profile ... */
    Person *e = person_get(eva);
    CHECK_STR(e->name, "Eva B");
    CHECK_INT(e->role, ROLE_MEMBER); /* ... but no admin fields */
    CHECK_STR(e->username, "eva");
    CHECK(auth_check("longenough1", e->pass));
    snprintf(f, sizeof f, "id=%d&name=Hacked", adam);
    CHECK(!th_act(eva, "people.save", f).ok);
    CHECK(!th_act(eva, "people.save", "name=Newbie").ok);
    CHECK(!th_act(eva, "people.delete", "id=1").ok);
    CHECK(!th_act(eva, "household.save", "name=Mine").ok);
    CHECK(!th_act(eva, "people.signout", "id=1").ok);
    CHECK_STR(person_get(adam)->name, "Adam");
    th_halt();
}

TEST(people_keep_one_admin_who_can_sign_in) {
    boot();
    int adam = admin("Adam", "adam");
    char f[100];
    snprintf(f, sizeof f, "id=%d&name=Adam&username=", adam); /* the only admin dropping their sign-in */
    CHECK(!th_act(adam, "people.save", f).ok);
    CHECK_STR(person_get(adam)->username, "adam");
    snprintf(f, sizeof f, "id=%d&name=Adam&username=adam&role=0", adam); /* own role can't be changed */
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK_INT(person_get(adam)->role, ROLE_ADMIN);
    th_halt();
}

TEST(people_keep_an_admin_counted_after_the_change) {
    /* issue #14: with two admins, Adam blanked his own username (Bea still counted), then removed
       Bea - and nobody could sign in as an admin any more */
    boot();
    int adam = admin("Adam", "adam");
    int bea = admin("Bea", "bea");
    char f[120];
    snprintf(f, sizeof f, "id=%d&name=Adam&username=", adam);
    CHECK(th_act(adam, "people.save", f).ok); /* fine: Bea can still sign in */
    CHECK_STR(person_get(adam)->username, "");
    snprintf(f, sizeof f, "id=%d", bea);
    ThResult r = th_act(adam, "people.delete", f);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Keep at least one admin who can sign in");
    CHECK(person_get(bea) != NULL);
    snprintf(f, sizeof f, "id=%d&name=Bea&role=0", bea); /* demoting her: the same */
    CHECK(!th_act(adam, "people.save", f).ok);
    CHECK_INT(person_get(bea)->role, ROLE_ADMIN);
    snprintf(f, sizeof f, "id=%d&name=Bea&username=", bea); /* taking her sign-in away: the same */
    CHECK(!th_act(adam, "people.save", f).ok);
    CHECK_STR(person_get(bea)->username, "bea");
    /* once Adam can sign in again, Bea may go */
    snprintf(f, sizeof f, "id=%d&name=Adam&username=adam&password=longenough1", adam);
    CHECK(th_act(bea, "people.save", f).ok);
    snprintf(f, sizeof f, "id=%d&how=erase", bea);
    CHECK(th_act(adam, "people.delete", f).ok);
    CHECK(person_get(bea) == NULL);
    th_halt();
}

TEST(people_change_password_signs_out_other_devices) {
    boot();
    int adam = admin("Adam", "adam");
    char a[33], b[33];
    session_create(adam, true, "Mozilla/5.0 (Windows NT 10.0) Chrome/130", a);
    session_create(adam, false, "Mozilla/5.0 (iPhone) Safari/605", b);
    CHECK_INT(tbl_count(&T_session), 2);
    CHECK(!th_act(adam, "people.password", "current=wrong&new=newpassword1&again=newpassword1").ok);
    CHECK(!th_act(adam, "people.password", "current=correct+horse&new=newpassword1&again=different1").ok);
    CHECK(!th_act(adam, "people.password", "current=correct+horse&new=short&again=short").ok);
    ThResult r = th_act(adam, "people.password", "current=correct+horse&new=newpassword1&again=newpassword1");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "2 other devices");
    CHECK(auth_check("newpassword1", person_get(adam)->pass));
    CHECK_INT(tbl_count(&T_session), 0); /* the test's own session isn't stored */
    th_halt();
}

TEST(people_admin_removes_people_and_their_sessions) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Eva&username=eva&password=longenough1").ok);
    int eva = person_by_username("eva")->id;
    char sid[33], f[32];
    session_create(eva, true, NULL, sid);
    snprintf(f, sizeof f, "id=%d", eva);
    CHECK(th_act(adam, "people.signout", f).ok);
    CHECK_INT(tbl_count(&T_session), 0);
    session_create(eva, true, NULL, sid);
    CHECK(th_act(adam, "people.delete", f).ok); /* moved out (issue #45): the row stays, the sign-in goes */
    CHECK(person_get(eva) && person_get(eva)->left);
    CHECK_STR(person_get(eva)->username, "");
    CHECK_INT(tbl_count(&T_session), 0);
    CHECK(!person_real(eva));
    snprintf(f, sizeof f, "id=%d&how=erase", eva);
    CHECK(th_act(adam, "people.delete", f).ok);
    CHECK(person_get(eva) == NULL);
    snprintf(f, sizeof f, "id=%d", adam);
    CHECK(!th_act(adam, "people.delete", f).ok); /* not yourself */
    th_halt();
}

TEST(people_display_accounts_are_not_household_members) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Hallway&username=hallway&password=tablet-pass-1&role=2").ok);
    CHECK_INT(account_count(), 2);
    CHECK_INT(person_count(), 1); /* not listed among people (who's home, pickers ...) */
    CHECK_STR(person_at(0)->name, "Adam");
    int tablet = person_by_username("hallway")->id;
    CHECK(!th_act(tablet, "people.password", "current=tablet-pass-1&new=whatever123&again=whatever123").ok);
    CHECK_HAS(th_page(adam, "/people"), "@hallway · Display");
    th_halt();
}

TEST(people_household_page) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Eva&hue=10").ok);
    char sid[33];
    session_create(adam, true, "Mozilla/5.0 (Windows NT 10.0) Chrome/130", sid);
    const char *h = th_page(adam, "/people");
    CHECK_HAS(h, "@adam · Admin");
    CHECK_HAS(h, "no sign-in");
    CHECK_HAS(h, "Chrome on Windows");
    CHECK_HAS(h, "action=\"/logout\"");
    CHECK_HAS(th_page(adam, "/people?edit=2"), "Remove Eva");
    CHECK_HAS(th_page(adam, "/people?add=1"), "Signing in");
    CHECK(th_act(adam, "household.save", "name=Villa+Villekulla").ok);
    CHECK_STR(app_name(), "Villa Villekulla");
    th_halt();
}

TEST(people_admin_edit_changes_only_what_was_sent) {
    /* review: {"id":2,"name":"Boss B."} over the API used to clear the username and password,
       demote the admin and sign them out */
    boot();
    int adam = admin("Adam", "adam");
    int boss = admin("Boss", "boss");
    CHECK(th_act(adam, "people.save", "name=Love&hue=10&emoji=%F0%9F%A6%8A").ok);
    char sid[33], f[96];
    session_create(boss, true, NULL, sid);
    snprintf(f, sizeof f, "id=%d&name=Boss+B.", boss);
    CHECK(th_act(adam, "people.save", f).ok);
    Person *b = person_get(boss);
    CHECK_STR(b->name, "Boss B.");
    CHECK_STR(b->username, "boss");
    CHECK_INT(b->role, ROLE_ADMIN);
    CHECK(auth_check("correct horse", b->pass));
    CHECK_INT(tbl_count(&T_session), 1); /* still signed in */
    int love = person_find("Love")->id;
    snprintf(f, sizeof f, "id=%d&name=Love+L", love);
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK_INT(person_get(love)->hue, 10); /* profile fields that weren't sent stay too */
    CHECK_STR(person_get(love)->emoji, "\xF0\x9F\xA6\x8A");
    /* what is sent still applies, including an explicit empty username */
    snprintf(f, sizeof f, "id=%d&name=Boss&role=0", boss);
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK_INT(person_get(boss)->role, ROLE_MEMBER);
    snprintf(f, sizeof f, "id=%d&name=Boss&username=", boss);
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK_STR(person_get(boss)->username, "");
    CHECK(!person_get(boss)->pass[0]);
    CHECK_INT(tbl_count(&T_session), 0);
    th_halt();
}

TEST(people_adding_many_keeps_the_author) {
    /* review: adding the 17th, 33rd ... person grows the table; the action used to read
       c->me through the stale pointer afterwards */
    boot();
    int adam = admin("Adam", "adam");
    char f[48];
    for (int i = 0; i < 70; i++) {
        snprintf(f, sizeof f, "name=Guest+%d&hue=%d", i, i);
        CHECK(th_act(adam, "people.save", f).ok);
    }
    int joined = 0;
    for (int i = 0; i < tbl_count(&T_activity); i++) {
        Activity *a = tbl_at(&T_activity, i);
        if (!strstr(a->text, "joined the household")) continue;
        joined++;
        if (a->who != adam) {
            test_fail(__FILE__, __LINE__, "\"%s\" credited to %d, not %d", a->text, a->who, adam);
            return;
        }
    }
    CHECK_INT(joined, 70);
    th_halt();
}

TEST(people_password_check_is_throttled) {
    boot();
    int adam = admin("Adam", "adam");
    for (int i = 0; i < 5; i++)
        CHECK(!th_act(adam, "people.password", "current=guess&new=newpassword1&again=newpassword1").ok);
    ThResult r = th_act(adam, "people.password", "current=correct+horse&new=newpassword1&again=newpassword1");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Too many tries");
    CHECK(auth_check("correct horse", person_get(adam)->pass)); /* unchanged */
    th_halt();
}

TEST(people_admin_sets_own_password_only_under_your_account) {
    /* review: people.save id=<self>&password=X changed the admin's own password without the
       current one (people.password asks for it) and left their other devices signed in */
    boot();
    int adam = admin("Adam", "adam");
    char a[33], b[33], f[128];
    session_create(adam, true, "Mozilla/5.0 (Windows NT 10.0) Chrome/130", a);
    session_create(adam, false, "Mozilla/5.0 (iPhone) Safari/605", b);
    snprintf(f, sizeof f, "id=%d&name=Adam&password=hijacked-pass-1", adam);
    ThResult r = th_act(adam, "people.save", f);
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Your account");
    CHECK(auth_check("correct horse", person_get(adam)->pass));
    CHECK_INT(tbl_count(&T_session), 2);
    snprintf(f, sizeof f, "id=%d&name=Adam+A&hue=30", adam); /* the rest of the profile still saves */
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK_STR(person_get(adam)->name, "Adam A");
    /* the dialog doesn't offer the field for yourself - but does for others */
    snprintf(f, sizeof f, "/people?edit=%d", adam);
    const char *h = th_page(adam, f);
    CHECK_HAS(h, "Change your own password under <b>Your account</b>");
    CHECK(!strstr(h, "name=\"password\""));
    CHECK(th_act(adam, "people.save", "name=Eva&username=eva&password=longenough1").ok);
    int eva = person_by_username("eva")->id;
    snprintf(f, sizeof f, "/people?edit=%d", eva);
    CHECK_HAS(th_page(adam, f), "name=\"password\"");
    /* an admin setting someone else's password ends that person's sessions (not their own) */
    char e1[33], e2[33];
    session_create(eva, true, NULL, e1);
    session_create(eva, false, NULL, e2);
    snprintf(f, sizeof f, "id=%d&name=Eva&password=fresh-pass-12", eva);
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK(auth_check("fresh-pass-12", person_get(eva)->pass));
    int evas = 0, adams = 0;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        evas += s->person == eva;
        adams += s->person == adam;
    }
    CHECK_INT(evas, 0);
    CHECK_INT(adams, 2);
    th_halt();
}

TEST(people_password_checks_share_the_address_budget) {
    /* wrong passwords from this device for any username count before the current-password
       check hashes anything (each check costs a password hash on the event loop) */
    boot();
    int adam = admin("Adam", "adam");
    int who = 0, wait = 0;
    char user[32];
    for (int i = 0; i < 10; i++) { /* th_act runs as 127.0.0.1 */
        snprintf(user, sizeof user, "nobody-%d", i);
        auth_login(user, "x", 0x7F000001, now_sec(), &who, &wait);
    }
    ThResult r = th_act(adam, "people.password", "current=correct+horse&new=newpassword1&again=newpassword1");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Too many tries");
    CHECK(!th_act(adam, "people.codes", "current=correct+horse").ok);
    CHECK(auth_check("correct horse", person_get(adam)->pass));
    th_halt();
}

TEST(people_recovery_codes_on_the_account_card) {
    boot();
    int adam = admin("Adam", "adam");
    const char *h = th_page(adam, "/people");
    CHECK_HAS(h, "Recovery codes");
    CHECK_HAS(h, "None yet");
    CHECK_HAS(h, "data-a=\"people.codes\"");
    /* making codes asks for the password (throttled like changing it) */
    ThResult r = th_act(adam, "people.codes", "current=wrong");
    CHECK(!r.ok);
    CHECK_INT(auth_codes_left(adam), 0);
    r = th_act(adam, "people.codes", "current=correct+horse");
    CHECK(r.ok);
    CHECK_STR(r.redirect, "/people/codes");
    CHECK_INT(auth_codes_left(adam), AUTH_CODES);
    h = th_page(adam, "/people/codes");
    CHECK_HAS(h, "Save these now");
    CHECK_HAS(h, "download=\"hemma-recovery-codes.txt\"");
    CHECK_HAS(h, "data-a=\"people.codes_saved\"");
    CHECK_HAS(h, "username (<b>adam</b>)");
    int shown = 0;
    for (const char *p = h; (p = strstr(p, "<li><code>")) != NULL; p += 10) shown++;
    CHECK_INT(shown, AUTH_CODES);
    CHECK_HAS(th_page(adam, "/people"), "waiting to be saved");
    CHECK(th_act(adam, "people.codes_saved", "").ok);
    h = th_page(adam, "/people/codes");
    CHECK_HAS(h, "only shown right after");
    CHECK(!strstr(h, "<li><code>"));
    h = th_page(adam, "/people");
    CHECK_HAS(h, "8 of 8 left");
    CHECK(!strstr(h, "waiting to be saved"));
    CHECK_HAS(h, "Your old codes stop working");
    th_halt();
}

TEST(people_members_make_their_own_codes_and_lose_them_with_their_sign_in) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Eva&username=eva&password=longenough1").ok);
    int eva = person_by_username("eva")->id;
    CHECK(th_act(eva, "people.codes", "current=longenough1").ok); /* created by an admin, codes of her own */
    CHECK_INT(auth_codes_left(eva), AUTH_CODES);
    CHECK_HAS(th_page(eva, "/people/codes"), "username (<b>eva</b>)");
    char f[96];
    snprintf(f, sizeof f, "id=%d&name=Eva&username=", eva); /* no sign-in any more: no codes either */
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK_INT(auth_codes_left(eva), 0);
    CHECK(th_act(adam, "people.save", "name=Hallway&username=hallway&password=tablet-pass-1&role=2").ok);
    int tablet = person_by_username("hallway")->id;
    CHECK(!strstr(th_page(tablet, "/people"), "Recovery codes")); /* display accounts have none */
    CHECK(!th_act(tablet, "people.codes", "current=tablet-pass-1").ok);
    CHECK_INT(auth_codes_left(tablet), 0);
    CHECK(th_act(adam, "people.codes", "current=correct+horse").ok);
    int boss = admin("Boss", "boss");
    Session s = {0};
    str_copy(s.token, sizeof s.token, "boss-phone");
    s.person = boss;
    CHECK(auth_codes_new(boss, &s));
    snprintf(f, sizeof f, "id=%d", boss); /* removed from the household: the codes go too */
    CHECK(th_act(adam, "people.delete", f).ok);
    CHECK_INT(auth_codes_left(boss), 0);
    CHECK_INT(auth_codes_left(adam), AUTH_CODES);
    th_halt();
}

TEST(people_page_after_a_recovery) {
    boot();
    int adam = admin("Adam", "adam");
    Session s = {0};
    str_copy(s.token, sizeof s.token, "x");
    s.person = adam;
    auth_codes_new(adam, &s);
    CHECK_HAS(th_page(adam, "/people?recovered=1"), "You have 8 recovery codes left.");
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    auth_codes_fresh(&s, codes);
    int who = 0, wait = 0;
    for (int i = 0; i < 7; i++) auth_recover("adam", codes[i], 0x0A000001, 1800000000 + i, &who, &wait);
    CHECK_HAS(th_page(adam, "/people?recovered=1"), "You have 1 recovery code left - make new ones");
    th_halt();
}

/* Issue #41: "Your calendar on your phone" - off until you make a link; the link is for the address you
   opened Hemma by, as webcal:// (a tap or a QR code on an iPhone) and http:// (ICSx⁵ on Android) */
TEST(people_calendar_link_is_made_renewed_and_turned_off) {
    boot();
    int adam = admin("Adam", "adam");
    int hall = admin("Hall", "hall");
    Person h = *person_get(hall);
    h.role = ROLE_DISPLAY;
    tbl_put(&T_person, &h);
    const char *page = th_page(adam, "/people");
    CHECK_HAS(page, "Your calendar on your phone");
    CHECK_HAS(page, "people.cal_make");
    CHECK(!strstr(page, "webcal://"));
    CHECK(!strstr(th_page(hall, "/people"), "Your calendar on your phone")); /* (a display account has none) */
    CHECK(th_act(adam, "people.cal_make", "").ok);
    char first[96], second[96], third[96];
    CHECK(cal_feed_path(adam, first, sizeof first));
    /* no address known: nothing to make the link of */
    page = th_page(adam, "/people");
    CHECK_HAS(page, "Open this page at Hemma's address to see your link.");
    CHECK(!strstr(page, "webcal://"));
    /* opened on this computer: the link works for its own calendar, a phone can't reach it (no code to scan) */
    app_test_ctx_host("127.0.0.1:8080");
    page = th_page(adam, "/people");
    CHECK_HAS(page, "which only this computer can reach");
    CHECK_HAS(page, "href=\"webcal://127.0.0.1:8080/cal/");
    CHECK(!strstr(page, "<svg class=\"qr\""));
    app_test_ctx_host("hemma.local:8080");
    page = th_page(adam, "/people");
    char want[160];
    snprintf(want, sizeof want, "href=\"webcal://hemma.local:8080%s\" data-native", first);
    CHECK_HAS(page, want);
    snprintf(want, sizeof want, "<code class=\"people-cal-url\">http://hemma.local:8080%s</code>", first);
    CHECK_HAS(page, want);
    CHECK_HAS(page, "<svg class=\"qr\"");
    CHECK_HAS(page, "ICSx⁵");
    /* every booking: a tick */
    CHECK(th_act(adam, "people.cal_all", "all=1").ok);
    CHECK_STR(pref(adam, PREF_CAL_ALL, ""), "1");
    CHECK_HAS(th_page(adam, "/people"), "value=\"1\" data-auto checked");
    CHECK(th_act(adam, "people.cal_all", "").ok);
    CHECK_STR(pref(adam, PREF_CAL_ALL, ""), "");
    /* a new link: the old one stops */
    ThResult r = th_act(adam, "people.cal_make", "");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "the old one stopped working");
    CHECK(cal_feed_path(adam, second, sizeof second) && !str_eq(first, second));
    CHECK_INT(cal_feed_person(first), 0);
    /* off - and made again: neither of the old ones comes back */
    CHECK(th_act(adam, "people.cal_off", "").ok);
    CHECK(!cal_feed_path(adam, third, sizeof third));
    CHECK_INT(cal_feed_person(second), 0);
    CHECK_HAS(th_page(adam, "/people"), "Make my calendar link");
    CHECK(th_act(adam, "people.cal_make", "").ok);
    CHECK(cal_feed_path(adam, third, sizeof third) && !str_eq(third, first) && !str_eq(third, second));
    CHECK(!th_act(hall, "people.cal_make", "").ok); /* view-only */
    app_test_ctx_host(NULL);
    th_halt();
}

/* ------------------------------------------------- children and guests (issue #44) */

TEST(people_children_and_guests_are_roles_an_admin_gives) {
    boot();
    g_fake_now = dt_make(2026, 10, 7, 10, 0); /* a Wednesday */
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Leo&username=leo&password=longenough1&role=3").ok);
    CHECK_INT(person_by_username("leo")->role, ROLE_CHILD);
    ThResult r = th_act(adam, "people.save", "name=Gun&username=gun&password=longenough1&role=4&until=");
    CHECK(!r.ok);
    CHECK_STR(r.field, "until");
    r = th_act(adam, "people.save", "name=Gun&username=gun&password=longenough1&role=4&until=2026-10-06");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "from today on");
    CHECK(th_act(adam, "people.save", "name=Gun&username=gun&password=longenough1&role=4&until=2026-10-11").ok);
    Person *gun = person_by_username("gun");
    CHECK_INT(gun->role, ROLE_GUEST);
    CHECK(gun->until == dt_make(2026, 10, 12, 0, 0)); /* the midnight after the last day */
    int gid = gun->id;
    const char *h = th_page(adam, "/people");
    CHECK_HAS(h, "@leo · Child");
    CHECK_HAS(h, "@gun · Guest until Sun");
    char f[160], url[40];
    snprintf(url, sizeof url, "/people?edit=%d", gid);
    h = th_page(adam, url);
    CHECK_HAS(h, "value=\"2026-10-11\"");
    CHECK_HAS(h, "Child — their own to-dos and the lists");
    snprintf(f, sizeof f, "id=%d&name=Gun&role=0", gid); /* a member now: no end */
    CHECK(th_act(adam, "people.save", f).ok);
    CHECK(person_get(gid)->until == 0);
    /* what they may do on their own account */
    int leo = person_by_username("leo")->id;
    snprintf(f, sizeof f, "id=%d&name=Leo+L&emoji=🐢", leo);
    CHECK(th_act(leo, "people.save", f).ok);
    CHECK(!th_act(leo, "people.save", "name=Newbie").ok);
    r = th_act(leo, "household.save", "name=Mine");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Ask a grown-up");
    CHECK(th_act(leo, "people.privacy", "all=1&tv=1").ok);
    g_fake_now = 0;
    th_halt();
}

TEST(people_a_guests_sign_in_ends_by_itself) {
    boot();
    int64_t now = dt_make(2026, 10, 7, 10, 0);
    g_fake_now = now;
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Gun&username=gun&password=longenough1&role=4&until=2026-10-08").ok);
    int gun = person_by_username("gun")->id, who = 0, wait = 0;
    CHECK_INT(auth_login("gun", "longenough1", 0x0A000001, now, &who, &wait), LOGIN_OK);
    CHECK(!person_signin_over(person_get(gun), now));
    char sid[33];
    CHECK(session_create(gun, true, NULL, sid));
    CHECK_INT(auth_codes_left(gun), 0); /* (no recovery codes: an admin sets a new password) */
    CHECK(!auth_codes_new(gun, NULL));
    int64_t later = dt_make(2026, 10, 9, 0, 0);
    CHECK(person_signin_over(person_get(gun), later));
    CHECK_INT(auth_login("gun", "wrong", 0x0A000002, later, &who, &wait), LOGIN_WRONG); /* (said only to the one who knows it) */
    CHECK_INT(auth_login("gun", "longenough1", 0x0A000003, later, &who, &wait), LOGIN_ENDED);
    CHECK_HAS(th_page(0, "/login?e=5"), "Your sign-in has ended");
    CHECK_HAS(th_page(0, "/login?e=5"), "ask Adam (admin) for more days");
    g_fake_now = later;
    CHECK_HAS(th_page(adam, "/people"), "Guest, ended Yesterday"); /* (ended at the midnight after Thursday) */
    g_fake_now = 0;
    th_halt();
}

TEST(people_a_guest_sees_their_own_account_only) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Gun&username=gun&password=longenough1&role=4&until=2099-01-01").ok);
    int gun = person_by_username("gun")->id;
    const char *h = th_page(gun, "/people");
    CHECK_HAS(h, "Your account");
    CHECK_HAS(h, "a guest until");
    CHECK_HAS(h, "Privacy");
    CHECK(!strstr(h, "Adam")); /* not the household's list */
    CHECK(!strstr(h, "Recovery codes"));
    CHECK(!strstr(h, "Your calendar on your phone"));
    CHECK(!strstr(th_page(gun, "/net"), "net-devs")); /* (not a guest's) */
    CHECK_HAS(th_page(gun, "/net"), "Nothing here");
    th_halt();
}

/* ------------------------------------------------- leaving (issue #45) */

TEST(people_the_remove_dialog_says_what_happens_first) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Eva&username=eva&password=longenough1").ok);
    int eva = person_by_username("eva")->id;
    CHECK(th_act(adam, "plan.task_add", "q=fix+the+fence+%40eva").ok);
    char sid[33], url[40], f[64];
    session_create(eva, true, NULL, sid);
    snprintf(url, sizeof url, "/people?remove=%d", eva);
    const char *h = th_page(adam, url);
    CHECK_HAS(h, "Eva moved out - keep their history");
    CHECK_HAS(h, "Erase everything about Eva");
    CHECK_HAS(h, "1 open to-do of theirs goes to anyone");
    CHECK_HAS(h, "Their sign-in goes (signed in on 1 device now)");
    CHECK(!strstr(th_page(eva, url), "role=\"dialog\"")); /* (an admin's) */
    snprintf(url, sizeof url, "/people?edit=%d", eva);
    CHECK_HAS(th_page(adam, url), "?remove=");
    snprintf(f, sizeof f, "id=%d&how=keep", eva);
    ThResult r = th_act(adam, "people.delete", f);
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Eva moved out - their history stays");
    Person *e = person_get(eva);
    CHECK(e && e->left && !e->username[0] && !e->pass[0]);
    CHECK_INT(tbl_count(&T_session), 0);
    h = th_page(adam, "/people");
    CHECK_HAS(h, "Moved out");
    CHECK_HAS(h, "Back in the household");
    CHECK_INT(person_count(), 1);
    CHECK(person_find("Eva") == NULL); /* (in no picker) */
    char label[80];
    CHECK_STR(person_label(person_get(eva), label, sizeof label), "Eva (moved out)");
    snprintf(url, sizeof url, "/people?remove=%d", eva);
    h = th_page(adam, url);
    CHECK_HAS(h, "Erase Eva");
    CHECK(!strstr(h, "keep their history"));
    /* she's back */
    snprintf(f, sizeof f, "id=%d", eva);
    CHECK(th_act(adam, "people.return", f).ok);
    CHECK(!person_get(eva)->left);
    CHECK_INT(person_count(), 2);
    CHECK(!th_act(adam, "people.return", f).ok);
    th_halt();
}

/* ------------------------------------------------- privacy (issue #42) */

TEST(people_privacy_is_each_persons_own) {
    boot();
    int adam = admin("Adam", "adam");
    CHECK(th_act(adam, "people.save", "name=Eva&username=eva&password=longenough1").ok);
    int eva = person_by_username("eva")->id;
    CHECK_INT(privacy_wall(eva), WALL_SHOW);
    CHECK(privacy_doing(eva) && privacy_home(eva));
    CHECK(privacy_on_tv(person_get(eva)));  /* a member: on the TV unless she unticks it */
    CHECK(!privacy_on_tv(person_get(adam))); /* an admin: once he ticks it */
    const char *h = th_page(eva, "/people");
    CHECK_HAS(h, "On the wall tablet");
    CHECK_HAS(h, "data-a=\"people.privacy\"");
    CHECK(th_act(eva, "people.privacy", "all=1&wall=name").ok); /* the boxes left unticked: no */
    CHECK_INT(privacy_wall(eva), WALL_NAME);
    CHECK(!privacy_doing(eva) && !privacy_home(eva) && !privacy_on_tv(person_get(eva)));
    presence_set_home(eva, true, now_sec()); /* her devices don't tell */
    CHECK(!presence_known(eva));
    CHECK(th_act(eva, "people.privacy", "all=1&wall=no&doing=1&tv=1&home=1").ok);
    CHECK_INT(privacy_wall(eva), WALL_HIDE);
    CHECK(privacy_doing(eva) && privacy_home(eva) && privacy_on_tv(person_get(eva)));
    presence_set_home(eva, true, now_sec());
    CHECK(presence_is_home(eva, NULL));
    CHECK(th_act(eva, "people.privacy", "tv=0").ok); /* one at a time (no "all"): the rest stays */
    CHECK(!privacy_on_tv(person_get(eva)));
    CHECK_INT(privacy_wall(eva), WALL_HIDE);
    CHECK(privacy_doing(eva));
    th_halt();
}
