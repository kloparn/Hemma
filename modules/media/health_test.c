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

TEST(media_health_counts_room_left_not_a_share) {
    /* issue #29: a disk 95% used with 20 GB free was "OK" on the System page and "nearly full" here:
       the two checks had different rules. Room for new videos counts, and it says when that disk
       is the one with Hemma's data too. */
    th_boot();
    int adam = admin();
    media_test_free_space(20LL << 30);
    const char *dir = test_tmpdir();
    char form[700];
    snprintf(form, sizeof form, "roots=%s", dir);
    CHECK(th_act(adam, "media.roots", form).ok);
    Ctx c;
    app_ctx(&c, adam, "/system");
    Health h = {0};
    /* issue #95: no row of its own any more - it tells the System page about the disk, which makes
       one row per disk (system_test.c) */
    media_health(&c, &h);
    CHECK_INT(h.ndisks, 1);
    CHECK(h.disks[0].free == (20LL << 30) && h.disks[0].want == (2LL << 30));
    CHECK(h.disks[0].data_disk); /* (the same disk as the data, here) */
    CHECK_STR(h.disks[0].what, "the library");
    CHECK_STR(h.disks[0].why, "for new videos");
    CHECK(item(&h, "Library disk nearly full") == NULL);
    health_free(&h);
    /* a video: room to upload or convert one as big - itself and the reserve, 2 GB at least */
    char film[700];
    snprintf(film, sizeof film, "%s/Film (2020).mkv", dir);
    CHECK(file_write_atomic(film, "a film", 6));
    media_scan_sync();
    media_health(&c, &h);
    CHECK(h.ndisks == 1 && h.disks[0].want == (2LL << 30));
    CHECK_HAS(h.disks[0].why, "to upload or convert a video as big as its biggest (");
    health_free(&h);
    media_test_free_space(-1);
    app_ctx_free(&c);
    th_halt();
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

    /* its disk fills up: said once, by the System page's "Disk space" (system_test.c) */
    media_test_free_space(1LL << 30);
    media_health(&c, &h);
    CHECK(h.ndisks == 1 && h.disks[0].free == (1LL << 30));
    CHECK(item(&h, "Library disk nearly full") == NULL);
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
