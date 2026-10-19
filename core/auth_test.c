#include "../tests/harness.h"
#include "auth.h"
#include "env.h"

static int with_login(const char *name, const char *user, const char *pw, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash(pw, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

TEST(auth_hash_and_check) {
    auth_configure("pepper-1", 1000);
    char h[128], h2[128];
    CHECK(auth_hash("correct horse battery", h, sizeof h));
    CHECK(str_starts(h, "pbkdf2-sha256$1000$"));
    CHECK_INT(strlen(h), strlen("pbkdf2-sha256$1000$") + 32 + 1 + 64);
    CHECK(auth_check("correct horse battery", h));
    CHECK(!auth_check("correct horse batterY", h));
    CHECK(!auth_check("", h));
    CHECK(auth_hash("correct horse battery", h2, sizeof h2));
    CHECK(strcmp(h, h2) != 0); /* random per-user salt */
    auth_configure("pepper-2", 1000);
    CHECK(!auth_check("correct horse battery", h)); /* the server salt is part of the secret */
    auth_configure("pepper-1", 2000);
    CHECK(auth_check("correct horse battery", h)); /* stored iteration count is honoured */
    CHECK(auth_needs_rehash(h));
    auth_configure("", 1000);
    CHECK(!auth_ready());
    CHECK(!auth_hash("x", h2, sizeof h2));
    CHECK(!auth_check("correct horse battery", h));
    char tiny[20];
    auth_configure("pepper-1", 1000);
    CHECK(!auth_hash("x", tiny, sizeof tiny)); /* doesn't fit: refused, not truncated */
}

TEST(auth_rejects_malformed_hashes) {
    auth_configure("pepper", 1000);
    const char *bad[] = {
        "",
        "plain-text-password",
        "pbkdf2-sha256$",
        "pbkdf2-sha256$0$00000000000000000000000000000000$0000000000000000000000000000000000000000000000000000000000000000",
        "pbkdf2-sha256$x$00000000000000000000000000000000$0000000000000000000000000000000000000000000000000000000000000000",
        "pbkdf2-sha256$1000$0000$0000000000000000000000000000000000000000000000000000000000000000",
        "pbkdf2-sha256$1000$zz000000000000000000000000000000$0000000000000000000000000000000000000000000000000000000000000000",
        "pbkdf2-sha256$1000$00000000000000000000000000000000$00",
        "md5$1000$00000000000000000000000000000000$0000000000000000000000000000000000000000000000000000000000000000",
    };
    for (int i = 0; i < countof(bad); i++) {
        if (auth_check("pw", bad[i])) {
            test_fail(__FILE__, __LINE__, "accepted malformed hash #%d", i);
            return;
        }
    }
    CHECK(!auth_check("pw", NULL));
    CHECK(!auth_check(NULL, "pbkdf2-sha256$1$00$00"));
}

TEST(auth_usernames_and_password_rules) {
    CHECK(auth_username_ok("adam"));
    CHECK(auth_username_ok("a.b_c-1"));
    CHECK(!auth_username_ok("a"));
    CHECK(!auth_username_ok("Adam")); /* stored lower-case */
    CHECK(!auth_username_ok("ad am"));
    CHECK(!auth_username_ok("åsa"));
    CHECK(!auth_username_ok("abcdefghijklmnopqrstuvwxyz0123456")); /* 33 */
    CHECK(auth_password_problem("short") != NULL);
    CHECK(auth_password_problem(NULL) != NULL);
    CHECK(auth_password_problem("längre lösenord") == NULL);
}

TEST(auth_login_is_case_insensitive_and_records_last_login) {
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int who = 0, wait = 0;
    CHECK_INT(auth_login("  ADAM ", "correct horse", 0x0A000001, 1800000000, &who, &wait), LOGIN_OK);
    CHECK_INT(who, adam);
    CHECK(person_get(adam)->last_login == 1800000000);
    CHECK_INT(auth_login("adam", "Correct horse", 0x0A000001, 1800000001, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("nobody", "correct horse", 0x0A000001, 1800000002, &who, &wait), LOGIN_WRONG);
    /* people without a password can't sign in even with an empty one */
    int love = th_person("Love");
    Person l = *person_get(love);
    str_copy(l.username, sizeof l.username, "love");
    tbl_put(&T_person, &l);
    CHECK_INT(auth_login("love", "", 0x0A000005, 1800000003, &who, &wait), LOGIN_WRONG);
    auth_configure("", 1000);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000001, 1800000004, &who, &wait), LOGIN_UNAVAILABLE);
    th_halt();
}

TEST(auth_throttles_guessing_per_address) {
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int64_t now = 1800000000;
    int who = 0, wait = 0;
    for (int i = 0; i < 5; i++) CHECK_INT(auth_login("adam", "guess", 0x0A000002, now + i, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000002, now + 6, &who, &wait), LOGIN_THROTTLED);
    CHECK(wait > 0 && wait <= 60);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000003, now + 6, &who, &wait), LOGIN_OK); /* others unaffected */
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000002, now + 70, &who, &wait), LOGIN_OK); /* lock expired */
    /* a success clears the slate */
    CHECK_INT(auth_login("adam", "guess", 0x0A000002, now + 71, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000002, now + 72, &who, &wait), LOGIN_OK);
    th_halt();
}

TEST(auth_throttle_ignores_other_accounts_signing_in) {
    /* review: 4 guesses at adam, 1 good sign-in as eve (any account), repeat - the success
       used to wipe the address's failures, so adam could be guessed at full speed */
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    with_login("Eve", "eve", "eves password", ROLE_MEMBER);
    int64_t now = 1800000000;
    int who = 0, wait = 0, wrong = 0, throttled = 0;
    for (int round = 0; round < 6; round++) {
        for (int k = 0; k < 4; k++) {
            LoginResult r = auth_login("adam", "guess", 0x0A000009, now++, &who, &wait);
            wrong += r == LOGIN_WRONG;
            throttled += r == LOGIN_THROTTLED;
        }
        CHECK_INT(auth_login("eve", "eves password", 0x0A000009, now++, &who, &wait), LOGIN_OK);
    }
    CHECK_INT(wrong, 5); /* then locked, eve's successes notwithstanding */
    CHECK_INT(throttled, 19);
    th_halt();
}

TEST(auth_throttles_one_account_across_many_addresses) {
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int64_t now = 1800000000;
    int who = 0, wait = 0;
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000063, now, &who, &wait), LOGIN_OK); /* adam's phone */
    for (int i = 0; i < 20; i++) CHECK_INT(auth_login("adam", "guess", 0x0A000100 + i, now + 1 + i, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000200, now + 30, &who, &wait), LOGIN_THROTTLED);
    CHECK(wait > 0 && wait <= 60);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000063, now + 31, &who, &wait), LOGIN_OK); /* still gets in */
    /* unknown usernames never lock anything account-wide */
    for (int i = 0; i < 30; i++) CHECK_INT(auth_login("nobody", "x", 0x0A000300 + i, now + 40 + i, &who, &wait), LOGIN_WRONG);
    th_halt();
}

TEST(auth_lockouts_grow_to_fifteen_minutes_and_are_forgiven) {
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int64_t t = 1800000000;
    int who = 0, wait = 0;
    for (int i = 0; i < 4; i++) CHECK_INT(auth_login("adam", "guess", 0x0A000007, t++, &who, &wait), LOGIN_WRONG);
    static const int steps[] = {60, 120, 240, 480, 900, 900};
    for (int k = 0; k < countof(steps); k++) {
        CHECK_INT(auth_login("adam", "guess", 0x0A000007, t, &who, &wait), LOGIN_WRONG);
        CHECK_INT(auth_login("adam", "correct horse", 0x0A000007, t, &who, &wait), LOGIN_THROTTLED);
        CHECK_INT(wait, steps[k]);
        t += steps[k];
    }
    /* a quiet half hour forgets earlier failures (another address, so the account limit isn't hit) */
    for (int i = 0; i < 4; i++) CHECK_INT(auth_login("adam", "guess", 0x0A000008, t + i, &who, &wait), LOGIN_WRONG);
    t += 31 * 60;
    for (int i = 0; i < 4; i++) CHECK_INT(auth_login("adam", "guess", 0x0A000008, t + i, &who, &wait), LOGIN_WRONG);
    /* and an admin's new password lifts a lock at once */
    CHECK_INT(auth_login("adam", "guess", 0x0A000008, t + 5, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000008, t + 6, &who, &wait), LOGIN_THROTTLED);
    auth_forget_failures(" ADAM ");
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000008, t + 7, &who, &wait), LOGIN_OK);
    th_halt();
}

TEST(auth_address_budget_stops_a_flood_of_made_up_usernames) {
    /* review: every POST /login with a fresh username ran PBKDF2 on the event loop and
       fresh usernames were never throttled - a flood froze the server */
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int64_t now = 1800000000;
    int who = 0, wait = 0;
    const uint32_t flood = 0x0A000050, other = 0x0A000051;
    char user[32];
    for (int i = 0; i < 10; i++) {
        snprintf(user, sizeof user, "made-up-%d", i);
        CHECK_INT(auth_login(user, "x", flood, now + i, &who, &wait), LOGIN_WRONG);
    }
    /* the right password doesn't help: the address waits before anything is hashed */
    CHECK_INT(auth_login("adam", "correct horse", flood, now + 11, &who, &wait), LOGIN_THROTTLED);
    CHECK(wait > 0 && wait <= 60);
    CHECK_INT(auth_login("made-up-99", "x", flood, now + 12, &who, &wait), LOGIN_THROTTLED);
    /* the same budget covers recovery codes and the current-password check */
    CHECK_INT(auth_recover("adam", "7KQ4-M2XP-9RTA", flood, now + 13, &who, &wait), LOGIN_THROTTLED);
    CHECK_INT(auth_recheck(person_get(adam), "correct horse", flood, now + 14, &wait), LOGIN_THROTTLED);
    /* other devices aren't affected */
    CHECK_INT(auth_login("adam", "correct horse", other, now + 15, &who, &wait), LOGIN_OK);
    CHECK_INT(auth_recheck(person_get(adam), "correct horse", other, now + 16, &wait), LOGIN_OK);
    /* after the lock: one more try, then a longer lock (1, 2, 4 ... minutes) */
    CHECK_INT(auth_login("made-up-11", "x", flood, now + 75, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("adam", "correct horse", flood, now + 76, &who, &wait), LOGIN_THROTTLED);
    CHECK(wait > 60 && wait <= 120);
    /* a signed-in person's wrong current passwords count too */
    for (int i = 0; i < 4; i++)
        CHECK_INT(auth_recheck(person_get(adam), "guess", other, now + 100 + i, &wait), LOGIN_WRONG);
    for (int i = 0; i < 6; i++) {
        snprintf(user, sizeof user, "other-%d", i);
        CHECK_INT(auth_login(user, "x", other, now + 110 + i, &who, &wait), LOGIN_WRONG);
    }
    CHECK_INT(auth_login("adam", "correct horse", other, now + 120, &who, &wait), LOGIN_THROTTLED);
    /* forgiven after a quiet half hour */
    CHECK_INT(auth_login("adam", "correct horse", other, now + 120 + 31 * 60, &who, &wait), LOGIN_OK);
    th_halt();
}

TEST(auth_junk_usernames_cant_flush_failures) {
    /* review: the (address, username) table evicted the oldest unlocked entry, so 4 guesses,
       a spray of junk usernames, 4 more guesses ... never hit the lock */
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int64_t now = 1800000000;
    int who = 0, wait = 0;
    const uint32_t guesser = 0x0A000060;
    for (int i = 0; i < 4; i++) CHECK_INT(auth_login("adam", "guess", guesser, now + i, &who, &wait), LOGIN_WRONG);
    char user[32];
    int wrong = 0;
    for (int i = 0; i < 600; i++) { /* more than the table holds, from 120 helper addresses (5 each: no lock) */
        auth_set_hash_budget(4000); /* about the tables, not the hashing budget: a slow machine runs it dry */
        snprintf(user, sizeof user, "junk-%d", i);
        wrong += auth_login(user, "x", 0x0A010000 + (uint32_t)(i / 5), now + 10, &who, &wait) == LOGIN_WRONG;
    }
    CHECK_INT(wrong, 600);
    CHECK_INT(auth_login("adam", "guess", guesser, now + 20, &who, &wait), LOGIN_WRONG); /* the 5th */
    CHECK_INT(auth_login("adam", "correct horse", guesser, now + 21, &who, &wait), LOGIN_THROTTLED);
    CHECK(wait > 0 && wait <= 60);
    th_halt();
}

TEST(auth_full_address_table_makes_new_addresses_wait) {
    /* every slot holds fresh failures (a flood from many addresses): a new address can't be
       counted, so it waits - except one that signed in as that user before */
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int64_t now = 1800000000;
    int who = 0, wait = 0;
    const uint32_t home = 0x0A000070, fresh = 0x0A000071;
    CHECK_INT(auth_login("adam", "correct horse", home, now, &who, &wait), LOGIN_OK);
    for (int i = 0; i < 256; i++) {
        auth_set_hash_budget(4000); /* about the table, not the hashing budget: a slow machine runs it dry */
        CHECK_INT(auth_login("nobody", "x", 0x0A020000 + (uint32_t)i, now + 1, &who, &wait), LOGIN_WRONG);
    }
    CHECK_INT(auth_login("adam", "correct horse", fresh, now + 2, &who, &wait), LOGIN_THROTTLED);
    CHECK_INT(wait, 60);
    CHECK_INT(auth_login("adam", "correct horse", home, now + 3, &who, &wait), LOGIN_OK);
    CHECK_INT(auth_login("adam", "correct horse", fresh, now + 31 * 60 + 2, &who, &wait), LOGIN_OK); /* forgiven */
    th_halt();
}

TEST(auth_recheck_is_throttled_per_person) {
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int eva = with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    int64_t t = 1800000000;
    int wait = 0;
    CHECK_INT(auth_recheck(person_get(adam), "correct horse", 0x0A000001, t, &wait), LOGIN_OK);
    for (int i = 0; i < 5; i++) CHECK_INT(auth_recheck(person_get(adam), "guess", 0x0A000001, t + i, &wait), LOGIN_WRONG);
    CHECK_INT(auth_recheck(person_get(adam), "correct horse", 0x0A000001, t + 6, &wait), LOGIN_THROTTLED);
    CHECK(wait > 0 && wait <= 60);
    CHECK_INT(auth_recheck(person_get(eva), "eva password", 0x0A000002, t + 6, &wait), LOGIN_OK); /* per person */
    CHECK_INT(auth_recheck(person_get(adam), "correct horse", 0x0A000001, t + 70, &wait), LOGIN_OK);
    th_halt();
}

TEST(auth_safe_next_stays_on_this_site) {
    static const struct {
        const char *in, *want;
    } cases[] = {
        {"/book?w=2026-10-12", "/book?w=2026-10-12"},
        {"/media/series/0123456789abcdef", "/media/series/0123456789abcdef"},
        {"/plan?q=a%20b&x=%C3%A5#top", "/plan?q=a%20b&x=%C3%A5#top"},
        {"/", "/"},
        {"//evil.example", "/"},
        {"/\\evil.example", "/"},
        {"\\\\evil.example", "/"},
        {"/\t/evil.example", "/"}, /* review: browsers drop the tab -> //evil.example */
        {"/\n/evil.example", "/"},
        {"/\r\n/evil.example", "/"},
        {"/ /evil.example", "/"},
        {"/x\x7f", "/"},
        {"/caf\xc3\xa9", "/"}, /* real URLs arrive percent-encoded */
        {"https://evil.example", "/"},
        {"evil.example", "/"},
        {"", "/"},
        {"/login?next=/x", "/"},
        {"/logout", "/"},
    };
    char out[600];
    for (int i = 0; i < countof(cases); i++) {
        auth_safe_next(cases[i].in, out, sizeof out);
        if (strcmp(out, cases[i].want) != 0) {
            test_fail(__FILE__, __LINE__, "case %d: got \"%s\", want \"%s\"", i, out, cases[i].want);
            return;
        }
    }
    auth_safe_next(NULL, out, sizeof out);
    CHECK_STR(out, "/");
    char small[8];
    auth_safe_next("/book?w=2026-10-12", small, sizeof small); /* doesn't fit: home, not a cut-off path */
    CHECK_STR(small, "/");
}

TEST(auth_upgrades_old_hashes_on_sign_in) {
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    auth_configure("pepper", 1500);
    int who = 0, wait = 0;
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000001, 1800000000, &who, &wait), LOGIN_OK);
    CHECK(str_starts(person_get(adam)->pass, "pbkdf2-sha256$1500$"));
    CHECK(auth_check("correct horse", person_get(adam)->pass));
    th_halt();
}

static void clear_admin_env(void) {
    plat_setenv("HEMMA_ADMIN_USER", "", true);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "", true);
    plat_setenv("HEMMA_ADMIN_NAME", "", true);
    plat_setenv("HEMMA_ADMIN_RESET", "", true);
}

TEST(auth_bootstrap_attaches_to_an_existing_person) {
    th_boot();
    auth_configure("pepper", 1000);
    int existing = th_person("Adam");
    plat_setenv("HEMMA_ADMIN_USER", "Adam", true);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "dev-password-123", true);
    auth_bootstrap_admin();
    Person *p = person_by_username("adam");
    CHECK(p != NULL);
    CHECK_INT(p->id, existing);
    CHECK_INT(p->role, ROLE_ADMIN);
    CHECK(auth_check("dev-password-123", p->pass));
    CHECK_INT(account_count(), 1);
    /* later starts leave it alone ... */
    plat_setenv("HEMMA_ADMIN_PASSWORD", "another-password", true);
    auth_bootstrap_admin();
    CHECK(auth_check("dev-password-123", person_by_username("adam")->pass));
    /* ... unless asked to reset */
    plat_setenv("HEMMA_ADMIN_RESET", "1", true);
    auth_bootstrap_admin();
    CHECK(auth_check("another-password", person_by_username("adam")->pass));
    clear_admin_env();
    th_halt();
}

TEST(auth_bootstrap_creates_the_admin) {
    th_boot();
    auth_configure("pepper", 1000);
    plat_setenv("HEMMA_ADMIN_USER", "boss", true);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "dev-password-123", true);
    plat_setenv("HEMMA_ADMIN_NAME", "Mamma", true);
    auth_bootstrap_admin();
    Person *p = person_by_username("boss");
    CHECK(p != NULL);
    CHECK_STR(p->name, "Mamma");
    CHECK_INT(p->role, ROLE_ADMIN);
    clear_admin_env();
    plat_setenv("HEMMA_ADMIN_USER", "Not Valid!", true);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "dev-password-123", true);
    auth_bootstrap_admin(); /* invalid username: ignored with a warning */
    CHECK_INT(account_count(), 1);
    clear_admin_env();
    th_halt();
}

TEST(auth_bootstrap_happens_once) {
    th_boot();
    auth_configure("pepper", 1000);
    plat_setenv("HEMMA_ADMIN_USER", "boss", true);
    auth_bootstrap_admin(); /* no password yet: nobody is created, and that isn't the bootstrap */
    CHECK_INT(account_count(), 0);
    CHECK_INT(setting_int("auth.bootstrapped", 0), 0);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "dev-password-123", true);
    auth_bootstrap_admin();
    CHECK(person_by_username("boss") != NULL);
    CHECK_INT(setting_int("auth.bootstrapped", 0), 1);
    /* review: the household renames the admin ... the old name doesn't come back */
    Person p = *person_by_username("boss");
    str_copy(p.username, sizeof p.username, "mamma");
    tbl_put(&T_person, &p);
    auth_bootstrap_admin();
    CHECK(person_by_username("boss") == NULL);
    CHECK_INT(account_count(), 1);
    /* ... or deletes it (even the last admin): it stays deleted */
    tbl_del(&T_person, p.id);
    auth_bootstrap_admin();
    CHECK_INT(account_count(), 0);
    clear_admin_env();
    th_halt();
}

TEST(auth_bootstrap_leaves_existing_admins_alone) {
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Mamma", "mamma", "correct horse", ROLE_ADMIN); /* a database from before the setting existed */
    plat_setenv("HEMMA_ADMIN_USER", "boss", true);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "dev-password-123", true);
    auth_bootstrap_admin();
    CHECK(person_by_username("boss") == NULL);
    CHECK_INT(setting_int("auth.bootstrapped", 0), 1);
    clear_admin_env();
    th_halt();
}

TEST(auth_reset_is_a_one_shot_recovery) {
    th_boot();
    auth_configure("pepper", 1000);
    int boss = with_login("Boss", "boss", "old-password-1", ROLE_ADMIN);
    char sid[33];
    session_create(boss, true, NULL, sid);
    session_create(boss, false, NULL, sid);
    plat_setenv("HEMMA_ADMIN_USER", "boss", true);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "recovery-pass-1", true);
    plat_setenv("HEMMA_ADMIN_RESET", "1", true);
    auth_bootstrap_admin();
    CHECK(auth_check("recovery-pass-1", person_by_username("boss")->pass));
    CHECK_INT(tbl_count(&T_session), 0); /* signed out everywhere */
    /* the admin picks a new password; the forgotten RESET line doesn't undo it at the next start */
    Person p = *person_by_username("boss");
    auth_hash("newer-password-2", p.pass, sizeof p.pass);
    tbl_put(&T_person, &p);
    session_create(boss, true, NULL, sid);
    auth_bootstrap_admin();
    CHECK(auth_check("newer-password-2", person_by_username("boss")->pass));
    CHECK_INT(tbl_count(&T_session), 1);
    /* deleted (or demoted) admin + a new reset password: back as an admin */
    tbl_del(&T_person, boss);
    plat_setenv("HEMMA_ADMIN_PASSWORD", "recovery-pass-2", true);
    auth_bootstrap_admin();
    Person *b = person_by_username("boss");
    CHECK(b != NULL);
    CHECK_INT(b ? b->role : -1, ROLE_ADMIN);
    CHECK(b && auth_check("recovery-pass-2", b->pass));
    clear_admin_env();
    th_halt();
}

TEST(auth_sessions_create_find_and_drop) {
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    char a[33], b[33];
    CHECK(session_create(adam, true, "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0) Safari/605.1", a));
    CHECK(session_create(adam, false, "Mozilla/5.0 (Windows NT 10.0) Chrome/130.0", b));
    CHECK_INT(strlen(a), 32);
    CHECK(strcmp(a, b) != 0);
    CHECK_INT(tbl_count(&T_session), 2);
    Session *s = tbl_at(&T_session, 0);
    CHECK_STR(s->agent, "Safari on iPhone");
    CHECK(!strstr(s->token, a)); /* only the hash is stored */
    CHECK_INT(strlen(s->token), 64);
    session_end(a);
    CHECK_INT(tbl_count(&T_session), 1);
    CHECK_STR(((Session *)tbl_at(&T_session, 0))->agent, "Chrome on Windows");
    CHECK_INT(sessions_drop(adam, NULL), 1);
    CHECK_INT(tbl_count(&T_session), 0);
    th_halt();
}

TEST(auth_sign_in_page) {
    th_boot();
    auth_configure("pepper", 1000);
    CHECK_HAS(th_page(0, "/"), "Welcome to Hemma"); /* nobody can sign in yet: create the admin */
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    const char *h = th_page(0, "/book?w=2026-10-12");
    CHECK_HAS(h, "action=\"/login\"");
    CHECK_HAS(h, "value=\"/book?w=2026-10-12\""); /* comes back here afterwards */
    CHECK_HAS(h, "autocomplete=\"current-password\"");
    CHECK_HAS(h, "href=\"/forgot\">Forgot your password?</a>");
    CHECK_HAS(th_page(0, "/login?e=1&u=adam"), "don't match");
    /* issue #100: the address carries no name - neither out of the page nor into the box */
    CHECK(!strstr(th_page(0, "/login?e=1&u=adam"), "adam"));
    CHECK_HAS(th_page(0, "/login?e=1&u=adam"), "href=\"/forgot\">Forgot your password?</a>");
    CHECK_HAS(th_page(0, "/forgot"), "No codes left? Ask an admin of your household to set a new password");
    CHECK(!strstr(th_page(0, "/forgot"), "Adam")); /* review: no names for signed-out visitors */
    CHECK_HAS(th_page(0, "/login?e=2&w=120"), "Wait 2 minutes");
    CHECK_HAS(th_page(0, "/login?e=3"), "signed out");
    th_halt();
}

static int g_guess;
static int guess_who(uint32_t ip) { return ip ? g_guess : 0; }

TEST(auth_forgot_page_names_nobody) {
    /* review: /forgot listed the admins' names to anyone on the network; the sign-in page's
       "Welcome back" guess (by address) stays there and doesn't travel to /forgot */
    th_boot();
    auth_configure("pepper", 1000);
    g_guess = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    with_login("Boss", "boss", "correct horse", ROLE_ADMIN);
    presence_set_guesser(guess_who);
    const char *h = th_page(0, "/login");
    CHECK_HAS(h, "Welcome back, Adam");
    CHECK_HAS(h, "href=\"/forgot\">Forgot your password?</a>");
    h = th_page(0, "/forgot");
    CHECK_HAS(h, "Ask an admin of your household");
    CHECK(!strstr(h, "Adam") && !strstr(h, "Boss") && !strstr(h, "adam"));
    CHECK(!strstr(th_page(0, "/login?e=1&u=boss"), "boss")); /* (issue #100: not from the address) */
    presence_set_guesser(NULL);
    th_halt();
}

/* ---- end to end over a loopback socket: signing in from a second tab ---- */

static volatile int g_served;
static void serve_app(void *arg) {
    (void)arg;
    app_serve();
    g_served = 1;
}

/* One request on a fresh connection ("Connection: close"); the whole response in out. */
static void http_once(int port, const char *raw, Buf *out) {
    buf_reset(out);
    sock_t s = tcp_connect_nb(0x7F000001, port);
    pollfd_t pw = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pw, 1, 2000) != 1 || !tcp_connect_done(s)) {
        if (s != SOCK_BAD) sock_close(s);
        return;
    }
    for (size_t n = strlen(raw); n;) {
        long k = sock_send(s, raw, n);
        if (k > 0) raw += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
    for (int64_t end = now_ms() + 4000; now_ms() < end;) {
        pollfd_t pr = {.fd = s, .events = POLLIN};
        if (plat_poll(&pr, 1, 50) != 1) continue;
        char tmp[8192];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(out, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
}

static void post_login(int port, const char *cookie, const char *form, Buf *out) {
    char req[1024];
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\n%s%s%sContent-Type: application/x-www-form-urlencoded\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
             cookie[0] ? "Cookie: hemma=" : "", cookie, cookie[0] ? "\r\n" : "", strlen(form), form);
    http_once(port, req, out);
}

static void set_cookie(const Buf *r, char *out, size_t cap) {
    out[0] = '\0';
    const char *c = r->p ? strstr(r->p, "Set-Cookie: hemma=") : NULL;
    if (c) str_ncopy(out, cap, c + 18, strcspn(c + 18, ";\r\n"));
}

static bool signed_in(int port, const char *cookie) {
    char req[256];
    snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\nConnection: close\r\n\r\n", cookie);
    Buf r = {0};
    http_once(port, req, &r);
    bool in = strstr(buf_cstr(&r), " 200 ") && !strstr(buf_cstr(&r), "action=\"/login\"");
    buf_free(&r);
    return in;
}

TEST(auth_signing_in_again_keeps_the_other_tab) {
    th_boot();
    auth_configure("e2e-salt", 1000);
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_served = 0;
    CHECK(thread_spawn(serve_app, NULL));
    Buf r = {0};
    char first[64], again[64], other[64];
    post_login(port, "", "username=adam&password=correct+horse&next=%2F", &r);
    set_cookie(&r, first, sizeof first);
    CHECK_INT(strlen(first), 32);
    /* review: a second tab still on the sign-in page signs in too - with the first tab's cookie */
    post_login(port, first, "username=adam&password=correct+horse&next=%2Fbook&remember=1", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /book");
    CHECK_HAS(buf_cstr(&r), "Max-Age=2592000"); /* "keep me signed in" still honoured - a month for an admin */
    set_cookie(&r, again, sizeof again);
    CHECK_STR(again, first); /* the same session: the first tab stays signed in */
    CHECK(signed_in(port, first));
    /* someone else signing in on this browser does replace the session */
    post_login(port, first, "username=eva&password=eva+password&next=%2F", &r);
    set_cookie(&r, other, sizeof other);
    CHECK(strlen(other) == 32 && strcmp(other, first) != 0);
    CHECK(!signed_in(port, first));
    CHECK(signed_in(port, other));
    /* a cookie that isn't a live session is replaced, never adopted */
    post_login(port, "0123456789abcdef0123456789abcdef", "username=adam&password=correct+horse", &r);
    set_cookie(&r, again, sizeof again);
    CHECK(strlen(again) == 32 && strcmp(again, "0123456789abcdef0123456789abcdef") != 0);
    /* review: next= with a tab in it can't send anyone off the site */
    post_login(port, "", "username=adam&password=correct+horse&next=%2F%09%2Fevil.example", &r);
    CHECK_HAS(buf_cstr(&r), "Location: /\r\n");
    server_stop();
    sock_t poke = tcp_connect_nb(0x7F000001, port); /* wake a loop that sleeps until its next tick */
    for (int i = 0; i < 300 && !g_served; i++) plat_sleep_ms(10);
    if (poke != SOCK_BAD) sock_close(poke);
    CHECK(g_served);
    server_shutdown();
    buf_free(&r);
    th_halt();
}

/* A new install makes its secret on the first start - into the settings file, never twice. */
TEST(auth_makes_its_secret_on_first_start) {
    th_boot();
    auth_configure("pepper-1", 1000);
    char dir[512], fresh[600], used[600], why[300];
    str_copy(dir, sizeof dir, test_tmpdir());
    snprintf(fresh, sizeof fresh, "%s/new.env", dir);
    snprintf(used, sizeof used, "%s/hemma.env", dir);

    /* no settings file yet: created, with a 256-bit secret, and the process has it */
    plat_setenv("HEMMA_AUTH_SALT", "", true);
    CHECK_INT(auth_ensure_secret(fresh, why, sizeof why), 1);
    CHECK_HAS(why, "new.env");
    Buf text = {0};
    CHECK(file_read_all(fresh, &text, 4096));
    const char *line = strstr(buf_cstr(&text), "\nHEMMA_AUTH_SALT=");
    CHECK(line != NULL);
    char made[80] = "";
    if (line) str_ncopy(made, sizeof made, line + 17, strcspn(line + 17, "\n"));
    CHECK_INT(strlen(made), 64);
    CHECK_STR(env("HEMMA_AUTH_SALT", ""), made);
    CHECK_INT(auth_ensure_secret(fresh, why, sizeof why), 0); /* there is one: nothing happens */

    /* an existing settings file keeps everything; its empty HEMMA_AUTH_SALT= line gets the value */
    static const char mine[] = "HEMMA_PORT=8081\r\n# my notes\nHEMMA_AUTH_SALT=\nHEMMA_HOSTS=nas.home";
    CHECK(file_write_atomic(used, mine, sizeof mine - 1));
    plat_setenv("HEMMA_AUTH_SALT", "", true);
    CHECK_INT(auth_ensure_secret(used, why, sizeof why), 1);
    buf_reset(&text);
    CHECK(file_read_all(used, &text, 4096));
    const char *t = buf_cstr(&text);
    CHECK(str_starts(t, "HEMMA_PORT=8081\r\n# my notes\n"));
    CHECK_HAS(t, "\nHEMMA_HOSTS=nas.home\n");
    CHECK(strstr(t, "HEMMA_AUTH_SALT=\n") == NULL);
    line = strstr(t, "HEMMA_AUTH_SALT=");
    CHECK(line && strstr(line + 1, "HEMMA_AUTH_SALT=") == NULL); /* one line, not two */
    CHECK(line && strlen(env("HEMMA_AUTH_SALT", "")) == 64 && !strncmp(line + 16, env("HEMMA_AUTH_SALT", ""), 64));

    /* passwords exist: a new secret would lock everyone out - refused, file untouched */
    with_login("Adam", "adam", "pw-adam-1234", ROLE_ADMIN);
    plat_setenv("HEMMA_AUTH_SALT", "", true);
    char lost[600];
    snprintf(lost, sizeof lost, "%s/lost.env", dir);
    CHECK_INT(auth_ensure_secret(lost, why, sizeof why), -1);
    CHECK_HAS(why, "already has passwords");
    CHECK(!path_stat(lost, NULL, NULL, NULL));
    CHECK_STR(env("HEMMA_AUTH_SALT", ""), "");
    buf_free(&text);
    th_halt();
}

TEST(auth_fills_in_a_secret_line_behind_a_bom) {
    /* issue #12: "BOM + HEMMA_AUTH_SALT=" (as some Windows editors save it) wasn't recognised:
       a second line was added at the end, and the next start read the empty first one */
    th_boot();
    char path[600], why[300];
    snprintf(path, sizeof path, "%s/bom.env", test_tmpdir());
    static const char mine[] = "\xEF\xBB\xBFHEMMA_AUTH_SALT= # filled in by Hemma\nHEMMA_PORT=8081\n";
    CHECK(file_write_atomic(path, mine, sizeof mine - 1));
    plat_setenv("HEMMA_AUTH_SALT", "", true);
    CHECK_INT(auth_ensure_secret(path, why, sizeof why), 1);
    Buf text = {0};
    CHECK(file_read_all(path, &text, 4096));
    const char *t = buf_cstr(&text), *line = strstr(t, "HEMMA_AUTH_SALT=");
    CHECK(str_starts(t, "\xEF\xBB\xBF#"));
    CHECK(line && strstr(line + 1, "HEMMA_AUTH_SALT=") == NULL); /* one line, not two */
    CHECK(line && !strncmp(line + 16, env("HEMMA_AUTH_SALT", ""), 64));
    CHECK_HAS(t, "\nHEMMA_PORT=8081\n");
    /* and the next start reads that value back */
    char made[80];
    str_copy(made, sizeof made, env("HEMMA_AUTH_SALT", ""));
    plat_setenv("HEMMA_AUTH_SALT", "", true);
    CHECK(env_load(path) >= 2);
    CHECK_STR(env("HEMMA_AUTH_SALT", ""), made);
    plat_setenv("HEMMA_AUTH_SALT", "", true);
    buf_free(&text);
    th_halt();
}

TEST(auth_signs_in_with_the_name_too) {
    /* issue #3: the admin "admin" is Adam to everyone - and typed "Adam" on another computer */
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "admin", "correct horse", ROLE_ADMIN);
    int who = 0, wait = 0;
    CHECK_INT(auth_login("Adam", "correct horse", 0x0A000001, 1800000000, &who, &wait), LOGIN_OK);
    CHECK_INT(who, adam);
    CHECK_INT(auth_login(" ADAM ", "correct horse", 0x0A000001, 1800000001, &who, &wait), LOGIN_OK);
    CHECK_INT(auth_login("admin", "correct horse", 0x0A000001, 1800000002, &who, &wait), LOGIN_OK);
    CHECK_INT(auth_login("Adam", "wrong horse", 0x0A000001, 1800000003, &who, &wait), LOGIN_WRONG);
    /* Swedish letters fold */
    int asa = with_login("\xC3\x85sa", "asa1", "asa password", ROLE_MEMBER);
    CHECK_INT(auth_login("\xC3\xA5sa", "asa password", 0x0A000002, 1800000004, &who, &wait), LOGIN_OK);
    CHECK_INT(who, asa);
    /* a username wins over somebody else's name */
    with_login("admin", "other", "other password", ROLE_MEMBER);
    CHECK(person_for_sign_in("admin") == person_get(adam));
    /* two who go by one name: only their usernames tell them apart */
    with_login("Eva", "eva1", "eva one pass", ROLE_MEMBER);
    with_login("eva", "eva2", "eva two pass", ROLE_MEMBER);
    CHECK(person_for_sign_in("Eva") == NULL);
    CHECK_INT(auth_login("Eva", "eva one pass", 0x0A000003, 1800000005, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("eva1", "eva one pass", 0x0A000003, 1800000006, &who, &wait), LOGIN_OK);
    /* someone without a sign-in doesn't own their name here */
    th_person("Love");
    CHECK(person_for_sign_in("Love") == NULL);
    /* guesses under the name and the username share one budget */
    for (int i = 0; i < 3; i++)
        CHECK_INT(auth_login("Adam", "nope", 0x0A000009, 1800000100 + i, &who, &wait), LOGIN_WRONG);
    for (int i = 0; i < 2; i++)
        CHECK_INT(auth_login("admin", "nope", 0x0A000009, 1800000110 + i, &who, &wait), LOGIN_WRONG);
    CHECK_INT(auth_login("admin", "correct horse", 0x0A000009, 1800000120, &who, &wait), LOGIN_THROTTLED);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000009, 1800000120, &who, &wait), LOGIN_THROTTLED);
    /* the pages say so */
    CHECK_HAS(th_page(0, "/login"), "<span>Username or name</span>");
    CHECK_HAS(th_page(0, "/forgot"), "<span>Username or name</span>");
    th_halt();
}

TEST(auth_welcome_back_has_a_way_to_someone_else) {
    /* issue #3: the "Welcome back" guess filled in another person's username on a shared
       computer, and the admin's password kept failing against it */
    th_boot();
    auth_configure("pepper", 1000);
    g_guess = with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    with_login("Adam", "admin", "correct horse", ROLE_ADMIN);
    presence_set_guesser(guess_who);
    const char *h = th_page(0, "/login?next=%2Fbook");
    CHECK_HAS(h, "Welcome back, Eva");
    CHECK_HAS(h, "value=\"eva\"");
    CHECK_HAS(h, "Not Eva? <a href=\"/login?other=1&amp;next=");
    CHECK_HAS(h, "\">Sign in as someone else</a>");
    h = th_page(0, "/login?other=1&next=%2Fbook");
    CHECK(!strstr(h, "Welcome back") && !strstr(h, "value=\"eva\""));
    CHECK_HAS(h, "value=\"/book\""); /* still goes back to where it was */
    presence_set_guesser(NULL);
    th_halt();
}

TEST(auth_password_checks_share_a_budget_of_loop_time) {
    /* issue #5: every check is a PBKDF2 hash on the event loop (~1 s on a Pi 3), and one host
       with hundreds of addresses could keep it busy for most of an hour */
    th_boot();
    auth_configure("pepper", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int who = 0, wait = 0;
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000001, 1800000000, &who, &wait), LOGIN_OK);
    auth_set_hash_budget(-2000); /* a flood just spent it */
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000002, 1800000001, &who, &wait), LOGIN_BUSY);
    CHECK(wait >= 1 && wait <= 9);
    CHECK_INT(auth_login("nobody", "x", 0x0A000003, 1800000001, &who, &wait), LOGIN_BUSY);
    CHECK_HAS(th_page(0, "/login?e=2&w=9&busy=1"), "Lots of sign-in tries right now. Wait 9 seconds");
    CHECK_HAS(th_page(0, "/login?e=2&w=120"), "Wait 2 minutes"); /* a lock still says minutes */
    CHECK_HAS(th_page(0, "/login?e=2&w=30"), "Wait 1 minute"); /* ... in its last minute too (review) */
    /* asking again and again while it's busy locks that address, like wrong passwords do */
    for (int i = 0; i < 9; i++)
        CHECK_INT(auth_login("nobody", "x", 0x0A000003, 1800000001, &who, &wait), LOGIN_BUSY);
    CHECK_INT(auth_login("nobody", "x", 0x0A000003, 1800000001, &who, &wait), LOGIN_THROTTLED);
    CHECK(wait >= 60);
    /* where this account signed in before, it still can */
    who = 0;
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000001, 1800000002, &who, &wait), LOGIN_OK);
    CHECK_INT(who, adam);
    /* waiting isn't failing: no lock piled up for the address that was turned away */
    auth_set_hash_budget(4000);
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000002, 1800000003, &who, &wait), LOGIN_OK);
    th_halt();
}

TEST(auth_notices_a_different_secret) {
    /* issue #3: started with another machine's HEMMA_AUTH_SALT, every sign-in failed and the
       log only said "failed sign-in" */
    th_boot();
    auth_configure("pepper", 1000);
    CHECK(!auth_salt_mismatch()); /* nothing proven yet */
    with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int who = 0, wait = 0;
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000001, 1800000000, &who, &wait), LOGIN_OK);
    CHECK_INT(strlen(setting("auth.salt_check", "")), 16);
    CHECK(!auth_salt_mismatch());
    auth_configure("another pepper", 1000);
    CHECK(auth_salt_mismatch());
    CHECK_INT(auth_login("adam", "correct horse", 0x0A000001, 1800000001, &who, &wait), LOGIN_WRONG);
    auth_configure("pepper", 1000);
    CHECK(!auth_salt_mismatch());
    th_halt();
}

TEST(auth_admin_sessions_end_after_a_month) {
    /* issue #5: over plain http a captured cookie was good for a year; an admin's session now
       ends ADMIN_SESSION_DAYS after signing in, however much it's used */
    th_boot();
    auth_configure("e2e-salt", 1000);
    int adam = with_login("Adam", "adam", "correct horse", ROLE_ADMIN);
    int eva = with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    char a[33], e[33], fresh[33];
    int tv = with_login("Living room TV", "tv", "tv password", ROLE_DISPLAY);
    char t_old[33], t_new[33];
    CHECK(session_create(adam, true, "test", a));
    CHECK(session_create(eva, true, "test", e));
    CHECK(session_create(tv, true, "test", t_old));
    CHECK(session_create(tv, true, "test", t_new));
    for (int i = 0; i < tbl_count(&T_session); i++) { /* signed in 31 days ago (the TV: 91 and 89), used today */
        Session s = *(Session *)tbl_at(&T_session, i);
        s.created = now_sec() - 31 * 86400LL;
        if (s.person == tv) s.created = now_sec() - (i == tbl_count(&T_session) - 1 ? 89 : 91) * 86400LL;
        s.seen = now_sec();
        tbl_put(&T_session, &s);
    }
    CHECK(session_create(adam, true, "test", fresh));
    CHECK(server_listen("127.0.0.1", 0));
    int port = server_port();
    g_served = 0;
    CHECK(thread_spawn(serve_app, NULL));
    CHECK(!signed_in(port, a));
    CHECK(signed_in(port, fresh));
    CHECK(signed_in(port, e)); /* a member's "keep me signed in" lasts a year while it's used */
    CHECK(!signed_in(port, t_old)); /* a display account's: DISPLAY_SESSION_DAYS (issue #90) */
    CHECK(signed_in(port, t_new));
    server_stop();
    sock_t poke = tcp_connect_nb(0x7F000001, port);
    for (int i = 0; i < 300 && !g_served; i++) plat_sleep_ms(10);
    if (poke != SOCK_BAD) sock_close(poke);
    CHECK(g_served);
    server_shutdown();
    th_halt();
}
