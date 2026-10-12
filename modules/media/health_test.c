/* The library's checks for the System page (health.c): folders, room, the converter, the step. */
#include "../../tests/harness.h"
#include "health.h"
#include "room.h"

static const HealthItem *item(const Health *h, const char *title) {
    for (int i = 0; i < h->n; i++)
        if (str_eq(h->items[i].title, title)) return &h->items[i];
    return NULL;
}

static int admin(void) {
    int id = th_person("Adam");
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

TEST(media_health_checks_the_library_folders) {
    th_boot();
    int adam = admin();
    media_test_free_space(50LL << 30);
    Ctx c;
    app_ctx(&c, adam, "/system");
    Health h = {0};

    /* nothing set up yet: worth knowing, nothing wrong */
    media_health(&c, &h);
    const HealthItem *it = item(&h, "Films and series");
    CHECK(it && it->state == HEALTH_INFO && !it->step);
    CHECK_STR(it->link, "/media/settings");
    it = item(&h, "Add your films and series");
    CHECK(it && it->step && it->state != HEALTH_OK);
    CHECK(item(&h, "Play everywhere") != NULL);
    CHECK(health_worst(&h) < HEALTH_WARN);
    health_free(&h);

    /* a folder that's there, readable and writable */
    const char *dir = test_tmpdir();
    char form[700];
    snprintf(form, sizeof form, "roots=%s", dir);
    CHECK(th_act(adam, "media.roots", form).ok);
    media_health(&c, &h);
    it = item(&h, "Films and series");
    CHECK(it && it->state == HEALTH_OK);
    CHECK_HAS(it->detail, "1 folder, 0 videos; 50 GB free.");
    CHECK(item(&h, "Read-only library folder") == NULL);
    it = item(&h, "Add your films and series");
    CHECK(it && it->state == HEALTH_OK); /* the step ticked itself */
    health_free(&h);

    /* its disk fills up */
    media_test_free_space(1LL << 30);
    media_health(&c, &h);
    it = item(&h, "Library disk nearly full");
    CHECK(it && it->state == HEALTH_WARN);
    CHECK_HAS(it->detail, "1.0 GB free");
    CHECK(health_worst(&h) == HEALTH_WARN);
    health_free(&h);
    media_test_free_space(50LL << 30);

    /* the disk was unplugged */
    char gone[700];
    snprintf(gone, sizeof gone, "%s/unplugged", dir);
    setting_set("media.roots", gone);
    media_health(&c, &h);
    it = item(&h, "Library folder missing");
    CHECK(it && it->state == HEALTH_BAD);
    CHECK_HAS(it->detail, "unplugged");
    CHECK(item(&h, "Films and series") == NULL);
    health_free(&h);

    /* through the System page, when it's there */
    if (module_find("system")) CHECK_HAS(th_page(adam, "/system"), "Library folder missing");
    media_test_free_space(-1);
    app_ctx_free(&c);
    th_halt();
}
