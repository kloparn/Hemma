/* The network's checks for the System page (net.c's health hook). */
#include "../../tests/harness.h"
#include "net.h"

static const HealthItem *item(const Health *h, const char *title) {
    for (int i = 0; i < h->n; i++)
        if (str_eq(h->items[i].title, title)) return &h->items[i];
    return NULL;
}

TEST(net_health_says_whether_discovery_is_on) {
    th_boot();
    int adam = th_person("Adam");
    Person p = *person_get(adam);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    const Module *m = module_find("net");
    CHECK(m && m->health);
    Ctx c;
    app_ctx(&c, adam, "/system");
    Health h = {0};
    m->health(&c, &h);
    CHECK(item(&h, "Home network") == NULL); /* tests have no interface until they set one */
    const HealthItem *it = item(&h, "Device discovery");
    CHECK(it && it->state == HEALTH_INFO);
    CHECK_STR(it->link, "/net");
    health_free(&h);

    LanIface f = {0};
    str_copy(f.name, sizeof f.name, "eth0");
    f.ip = 0xC0A80132;
    f.mask = 0xFFFFFF00;
    f.gateway = 0xC0A80101;
    f.link = LAN_LINK_CABLE;
    net_set_iface(&f);
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    m->health(&c, &h);
    it = item(&h, "Home network");
    CHECK(it && it->state == HEALTH_OK);
    CHECK_STR(it->detail, "Connected over a network cable as 192.168.1.50, with the router at 192.168.1.1.");
    it = item(&h, "Device discovery");
    CHECK(it && it->state == HEALTH_OK);
    CHECK_HAS(it->detail, "every 2 minutes");
    CHECK(health_worst(&h) == HEALTH_OK);
    health_free(&h);
    app_ctx_free(&c);
    th_halt();
}
