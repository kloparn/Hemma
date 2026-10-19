/* hemma --demo (issue #31): a made-up household through the modules' own actions, a band on every
 * page, and the login on the sign-in page. */
#include "../tests/harness.h"
#include "auth.h"

#define PW "demo-1234abcd"

/* Anna, as the demo's settings file makes her at a real start (auth_bootstrap_admin) */
static int make_anna(void) {
    auth_configure("demo-test-salt", 1000);
    Person p = {0};
    str_copy(p.name, sizeof p.name, "Anna");
    str_copy(p.username, sizeof p.username, "anna");
    p.role = ROLE_ADMIN;
    p.hue = 200;
    p.created = now_sec();
    auth_hash(PW, p.pass, sizeof p.pass);
    tbl_put(&T_person, &p);
    return p.id;
}

TEST(demo_fills_a_made_up_household) {
    th_boot();
    int anna = make_anna();
    CHECK(person_get(anna)->pass[0]);
    app_demo_seed(PW);
    const Person *jonas = person_by_username("jonas");
    CHECK(jonas != NULL);
    CHECK(jonas && jonas->pass[0]); /* (he can sign in, with the same password) */
    CHECK(person_find("Maja") != NULL);
    CHECK(person_find("Leo") != NULL);
    CHECK_STR(person_get(anna)->emoji, "🦊");
    CHECK_STR(app_name(), "Björkbacken");
    if (module_find("book")) {
        const char *h = th_page(anna, "/book");
        CHECK_HAS(h, ">Car<");
        CHECK_HAS(h, ">Laundry<");
        CHECK_HAS(h, "ikea trip");
    }
    if (module_find("plan")) {
        CHECK_HAS(th_page(anna, "/plan"), "Vacuum");
        CHECK_HAS(th_page(anna, "/plan/shop"), "Coffee");
        CHECK_HAS(th_page(anna, "/plan/cal"), "Dentist");
    }
    if (module_find("board")) CHECK_HAS(th_page(anna, "/board"), "plumber");
    CHECK_HAS(th_page(anna, "/"), "Anna booked"); /* the news on Home, as if it happened */
    int people = tbl_count(&T_person);
    app_demo_seed(PW); /* once per folder */
    CHECK_INT(tbl_count(&T_person), people);
    th_halt();
}

TEST(demo_says_so_on_every_page) {
    th_boot();
    int anna = make_anna();
    CHECK(!strstr(th_page(anna, "/"), "demo-band"));
    app_test_demo(PW);
    CHECK_HAS(th_page(anna, "/"), "class=\"demo-band\"");
    CHECK_HAS(th_page(0, "/"), "password <b>" PW "</b>");
    app_test_demo(NULL);
    CHECK(!strstr(th_page(0, "/"), PW));
    th_halt();
}
