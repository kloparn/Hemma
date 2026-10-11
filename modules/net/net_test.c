/* Tests for the network module. Everything is fed through fake neighbour
 * rounds — no sockets, no packets (app_testing() is on inside the harness). */
#include "../../core/db.h"
#include "../../tests/harness.h"
#include "net.h"

static int64_t T0;

/* Switching discovery on is an admin decision, so the household admin runs these. */
static int admin(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

static void boot(void) {
    th_boot();
    LanIface f = {0};
    str_copy(f.name, sizeof f.name, "eth0");
    ip_parse("192.168.1.2", &f.ip);
    ip_parse("255.255.255.0", &f.mask);
    ip_parse("192.168.1.1", &f.gateway);
    ip_parse("192.168.1.255", &f.broadcast);
    net_set_iface(&f);
    T0 = dt_make(2026, 10, 10, 18, 0);
    g_fake_now = T0;
}

static Neighbor nb(const char *ip, const char *mac, NeighState st) {
    Neighbor n = {0};
    ip_parse(ip, &n.ip);
    mac_parse(mac, &n.mac);
    n.state = st;
    return n;
}

static uint32_t ip(const char *s) {
    uint32_t v = 0;
    ip_parse(s, &v);
    return v;
}

#define ADAMS_PHONE "a4:83:e7:11:22:33"
#define PRIVATE_MAC "02:11:22:33:44:55"

TEST(net_ingest_creates_devices_in_our_subnet_only) {
    boot();
    Neighbor r[] = {
        nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE),
        nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE),
        nb("10.9.9.9", "a4:83:e7:00:00:01", NEIGH_REACHABLE),       /* another subnet */
        nb("192.168.1.2", "a4:83:e7:00:00:02", NEIGH_REACHABLE),    /* ourselves */
        nb("192.168.1.12", "00:00:00:00:00:00", NEIGH_FAILED),      /* probed, nobody home */
        nb("192.168.1.255", "ff:ff:ff:ff:ff:ff", NEIGH_REACHABLE),  /* broadcast */
    };
    net_ingest(r, countof(r), T0, false);
    CHECK_INT(tbl_count(&T_device), 2);
    Device *a = net_by_mac(ADAMS_PHONE);
    CHECK(a != NULL);
    CHECK_STR(a->last_ip, "192.168.1.10");
    CHECK(a->first_seen == T0 && a->last_seen == T0 && !a->random_mac && a->online);
    Device *p = net_by_mac(PRIVATE_MAC);
    CHECK(p && p->random_mac);
    CHECK_STR(net_kind(p), "📱"); /* private addresses are almost always phones */
    CHECK_STR(net_label(p), "Unknown device");
    th_halt();
}

TEST(net_ip_changes_are_written_to_the_database) {
    boot();
    Neighbor r1[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE)};
    net_ingest(r1, 1, T0, false);
    Neighbor r2[] = {nb("192.168.1.20", ADAMS_PHONE, NEIGH_REACHABLE)};
    net_ingest(r2, 1, T0 + 30, false);
    CHECK_STR(net_by_mac(ADAMS_PHONE)->last_ip, "192.168.1.20");
    sqlite3_stmt *st = db_prep("SELECT last_ip FROM device WHERE mac = ?");
    CHECK(st != NULL);
    sqlite3_bind_text(st, 1, ADAMS_PHONE, -1, SQLITE_STATIC);
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    CHECK_STR((const char *)sqlite3_column_text(st, 0), "192.168.1.20");
    th_halt();
}

TEST(net_online_window_and_stale_entries) {
    boot();
    Neighbor seen[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE)};
    net_ingest(seen, 1, T0, false);
    Device *a = net_by_mac(ADAMS_PHONE);
    CHECK(net_online(a, T0));
    CHECK(net_online(a, T0 + NET_ONLINE_WINDOW));
    CHECK(!net_online(a, T0 + NET_ONLINE_WINDOW + 1));
    /* a stale cache entry doesn't count as a fresh sighting */
    Neighbor stale[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_STALE)};
    net_ingest(stale, 1, T0 + 100, false);
    CHECK(net_by_mac(ADAMS_PHONE)->last_seen == T0);
    /* nothing seen for a while: the device goes offline */
    net_ingest(NULL, 0, T0 + 200, false);
    CHECK(!net_by_mac(ADAMS_PHONE)->online);
    /* and comes back */
    net_ingest(seen, 1, T0 + 500, false);
    CHECK(net_by_mac(ADAMS_PHONE)->online);
    CHECK(net_by_mac(ADAMS_PHONE)->last_seen == T0 + 500);
    th_halt();
}

TEST(net_presence_needs_discovery_and_follows_the_window) {
    boot();
    int adam = admin("Adam");
    int eva = th_person("Eva");
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE)};
    net_ingest(r, 1, T0, false);
    CHECK(th_act(adam, "net.save", "id=1&name=Adams+phone&owner=1").ok);
    net_ingest(r, 1, T0 + 5, false);
    CHECK(!presence_tracking()); /* discovery off: no claims about who's home */

    CHECK(th_act(adam, "net.scan", "on=1").ok);
    CHECK(net_scan_on());
    net_ingest(r, 1, T0 + 10, true);
    CHECK(presence_tracking());
    CHECK(presence_is_home(adam, NULL));
    CHECK(!presence_is_home(eva, NULL)); /* no device: never reported */

    int64_t later = T0 + 10 + 11 * 60; /* default window is 10 minutes */
    net_ingest(NULL, 0, later, true);
    int64_t since = 0;
    CHECK(!presence_is_home(adam, &since));
    CHECK(since == later);

    g_fake_now = later;
    CHECK(th_act(adam, "net.window", "min=30").ok); /* a wider window: home again */
    CHECK(presence_is_home(adam, NULL));
    CHECK_INT(setting_int("net.window", 0), 30);
    th_halt();
}

TEST(net_guesser_maps_visitors_to_owners) {
    boot();
    int adam = admin("Adam");
    int eva = th_person("Eva");
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r, 2, T0, false);
    CHECK(th_act(adam, "net.save", "id=1&owner=1").ok);
    CHECK_INT(presence_guess(ip("192.168.1.10")), adam);
    CHECK_INT(presence_guess(ip("192.168.1.99")), 0);
    CHECK_INT(presence_guess(ip("192.168.1.11")), 0);

    /* Eva opens Hemma from the unowned device: the http hook learns it */
    Ctx c;
    app_ctx(&c, eva, "/");
    c.ip = ip("192.168.1.11");
    CHECK(!module_find("net")->http(&c, NULL, NULL)); /* observes, never handles */
    app_ctx_free(&c);
    CHECK_INT(net_by_mac(PRIVATE_MAC)->used_by, eva);
    CHECK_INT(presence_guess(ip("192.168.1.11")), eva);
    CHECK_HAS(th_page(adam, "/net"), "Eva's?"); /* owner suggestion */
    CHECK(th_act(adam, "net.assign", "id=2&person=2").ok);
    CHECK_INT(net_by_mac(PRIVATE_MAC)->owner, eva);
    CHECK(!th_act(adam, "net.assign", "id=2&person=99").ok);

    /* devices not seen for a day don't get attributed */
    net_learn(adam, ip("192.168.1.11"), T0 + 2 * 86400);
    CHECK_INT(net_by_mac(PRIVATE_MAC)->used_by, eva);
    th_halt();
}

TEST(net_new_devices_are_announced_after_the_baseline) {
    boot();
    int adam = admin("Adam");
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    int before = tbl_count(&T_activity);
    Neighbor r1[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE)};
    net_ingest(r1, 1, T0, true); /* baseline round: quiet */
    CHECK_INT(tbl_count(&T_activity), before);
    Neighbor r2[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.30", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r2, 2, T0 + 120, true);
    CHECK_INT(tbl_count(&T_activity), before + 1);
    Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_HAS(a->text, "New device on the network");
    CHECK_HAS(a->text, "192.168.1.30");
    /* passive rounds never announce */
    Neighbor r3[] = {nb("192.168.1.40", "a4:83:e7:44:44:44", NEIGH_REACHABLE)};
    net_ingest(r3, 1, T0 + 130, false);
    CHECK_INT(tbl_count(&T_activity), before + 1);
    CHECK_INT(tbl_count(&T_device), 3);
    th_halt();
}

TEST(net_edit_wake_and_forget) {
    boot();
    int adam = admin("Adam");
    Neighbor r[] = {nb("192.168.1.10", "a4:83:e7:aa:bb:cc", NEIGH_REACHABLE)};
    net_ingest(r, 1, T0, false);
    CHECK(!th_act(0, "net.save", "id=1&name=x").ok); /* anonymous visitors can't */
    CHECK(!th_act(adam, "net.wake", "id=1").ok);     /* WoL not enabled yet */
    CHECK(th_act(adam, "net.save", "id=1&name=++Gaming+PC++&owner=1&kind=%F0%9F%8E%AE&wol=1&note=in+the+attic").ok);
    Device *d = tbl_get(&T_device, 1);
    CHECK_STR(d->name, "Gaming PC");
    CHECK_STR(d->kind, "🎮");
    CHECK(d->wol);
    CHECK_STR(d->note, "in the attic");
    CHECK_INT(d->owner, adam);
    ThResult w = th_act(adam, "net.wake", "id=1"); /* no packet is sent under tests */
    CHECK(w.ok);
    CHECK_HAS(w.toast, "Gaming PC");
    CHECK(th_act(adam, "net.save", "id=1&name=PC&kind=nonsense&owner=42").ok);
    d = tbl_get(&T_device, 1);
    CHECK_STR(d->kind, ""); /* unknown kinds fall back to "guess" */
    CHECK_INT(d->owner, 0); /* unknown people aren't stored */
    CHECK(!d->wol);         /* unticked checkbox */
    CHECK(th_act(adam, "net.forget", "id=1").ok);
    CHECK(tbl_get(&T_device, 1) == NULL);
    CHECK(!th_act(adam, "net.forget", "id=1").ok);
    CHECK(!th_act(adam, "net.save", "id=77").ok);
    th_halt();
}

TEST(net_reverse_dns_names_and_kind_guesses) {
    boot();
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.1", "a4:83:e7:00:00:01", NEIGH_REACHABLE)};
    net_ingest(r, 2, T0, false);
    net_name_ip(ip("192.168.1.10"), "adams-macbook-air");
    Device *a = net_by_mac(ADAMS_PHONE);
    CHECK_STR(a->hostname, "adams-macbook-air");
    CHECK_STR(net_label(a), "adams-macbook-air");
    CHECK_STR(net_kind(a), "💻");
    Device *gw = net_by_mac("a4:83:e7:00:00:01");
    CHECK_STR(net_label(gw), "Router");
    CHECK_STR(net_kind(gw), "🌐");
    net_name_ip(ip("192.168.1.77"), "nobody-here"); /* unknown address: ignored */
    CHECK_INT(tbl_count(&T_device), 2);
    /* the router is recognised, so it's neither "unknown" nor "new" */
    int adam = admin("Adam");
    const char *unknown = th_page(adam, "/net?f=unknown");
    CHECK_HAS(unknown, "id=\"net-dev-1\"");
    CHECK(!strstr(unknown, "id=\"net-dev-2\""));

    static const struct {
        const char *host, *kind;
    } cases[] = {
        {"Adams-iPhone", "📱"},    {"living-room-tv", "📺"}, {"lgwebostv", "📺"},  {"synology-ds220", "💾"},
        {"jonas-pc", "🖥️"},       {"ps5-console", "🎮"},    {"raspberrypi", "🍓"}, {"camilla", NULL},
        {"jonas", NULL},          {"echo-dot", "🔊"},       {"HP-OfficeJet", "🖨️"}, {"", NULL},
    };
    for (int i = 0; i < countof(cases); i++) {
        Device d = {0};
        str_copy(d.hostname, sizeof d.hostname, cases[i].host);
        const char *k = net_kind(&d);
        if (!str_eq(k, cases[i].kind)) {
            test_fail(__FILE__, __LINE__, "kind of \"%s\": got %s, want %s", cases[i].host, k ? k : "NULL",
                      cases[i].kind ? cases[i].kind : "NULL");
            return;
        }
    }
    th_halt();
}

TEST(net_pages_and_widgets_render) {
    boot();
    int adam = admin("Adam");
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r, 2, T0, false);
    const char *html = th_page(adam, "/net");
    CHECK_HAS(html, "This hub");
    CHECK_HAS(html, "192.168.1.2/24");
    CHECK_HAS(html, "Discovery");
    CHECK_HAS(html, "aria-checked=\"false\"");
    CHECK_HAS(html, "Unknown device");
    CHECK_HAS(html, "private address");
    CHECK_HAS(html, ">online<");
    CHECK_HAS(html, "id=\"net-dev-1\"");
    CHECK_HAS(th_page(adam, "/net?edit=1"), "Forget device");
    CHECK_HAS(th_page(adam, "/net?edit=1"), ADAMS_PHONE);
    CHECK_HAS(th_page(adam, "/net?f=online"), "id=\"net-dev-2\"");
    CHECK_HAS(th_page(adam, "/net?f=people"), "Nothing matches");
    CHECK_HAS(th_page(0, "/net"), "data-mod=\"auth\""); /* signed-out visitors get the sign-in page */

    CHECK_HAS(th_page(adam, "/"), "devices online"); /* dashboard card */
    CHECK_HAS(th_page(adam, "/"), "Turn on discovery");

    /* the wall display is public: render the widget with nobody signed in */
    Ctx c;
    app_ctx(&c, 0, "/wall");
    Buf b = {0};
    module_find("net")->widget(&c, &b, WIDGET_WALL);
    CHECK_HAS(buf_cstr(&b), "devices online");
    int eva = th_person("Eva");
    CHECK(!th_act(eva, "net.scan", "on=1").ok); /* members can't switch discovery on */
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    CHECK(th_act(adam, "net.save", "id=1&owner=1").ok);
    net_ingest(r, 2, T0 + 30, true);
    buf_reset(&b);
    c.now = T0 + 30;
    module_find("net")->widget(&c, &b, WIDGET_WALL);
    CHECK_HAS(buf_cstr(&b), "Who&#39;s home"); /* card titles are escaped */
    CHECK_HAS(buf_cstr(&b), "Adam");
    buf_free(&b);
    app_ctx_free(&c);
    CHECK_HAS(th_page(adam, "/net"), "aria-checked=\"true\"");
    th_halt();
}

TEST(net_presence_is_forgotten_when_discovery_stops_or_devices_go) {
    boot();
    int adam = admin("Adam");
    int eva = th_person("Eva");
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r, 2, T0, false);
    CHECK(th_act(adam, "net.save", "id=1&owner=1").ok);
    CHECK(th_act(adam, "net.save", "id=2&owner=2").ok);
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    net_ingest(r, 2, T0 + 10, true);
    CHECK(presence_is_home(adam, NULL) && presence_is_home(eva, NULL));

    /* discovery off: nobody is claimed home (or away) any more, hours later either */
    CHECK(th_act(adam, "net.scan", "on=0").ok);
    CHECK(!presence_tracking());
    CHECK(!presence_known(adam) && !presence_known(eva));
    g_fake_now = T0 + 6 * 3600;
    net_ingest(NULL, 0, g_fake_now, false);
    CHECK(!presence_known(adam) && !presence_is_home(adam, NULL));
    CHECK(!strstr(th_page(adam, "/"), "strip-p home")); /* the home strip */

    /* on again: Eva's phone is given away, then Adam's is forgotten */
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    net_ingest(r, 2, g_fake_now, true);
    CHECK(presence_is_home(adam, NULL) && presence_is_home(eva, NULL));
    CHECK(th_act(adam, "net.save", "id=2&owner=0").ok);
    CHECK(!presence_known(eva));
    CHECK(presence_is_home(adam, NULL));
    CHECK(th_act(adam, "net.forget", "id=1").ok);
    CHECK(!presence_known(adam));
    th_halt();
}

TEST(net_guesses_and_learns_only_from_devices_online_now) {
    boot();
    int adam = admin("Adam");
    int eva = th_person("Eva");
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r, 2, T0, false);
    CHECK(th_act(adam, "net.save", "id=1&owner=1").ok);
    CHECK_INT(presence_guess(ip("192.168.1.10")), adam);
    /* an hour later the address may be a guest's phone: no "Welcome back, Adam" */
    g_fake_now = T0 + 3600;
    CHECK_INT(presence_guess(ip("192.168.1.10")), 0);
    net_learn(eva, ip("192.168.1.11"), g_fake_now); /* nor is a stale device attributed */
    CHECK_INT(net_by_mac(PRIVATE_MAC)->used_by, 0);
    th_halt();
}

TEST(net_display_accounts_never_claim_a_device) {
    boot();
    th_person("Adam");
    int tablet = th_person("Hallway tablet");
    Person p = *person_get(tablet);
    p.role = ROLE_DISPLAY;
    tbl_put(&T_person, &p);
    Neighbor r[] = {nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r, 1, T0, false);
    Ctx c;
    app_ctx(&c, tablet, "/wall");
    c.ip = ip("192.168.1.11");
    CHECK(c.me != NULL);
    CHECK(!module_find("net")->http(&c, NULL, NULL));
    app_ctx_free(&c);
    CHECK_INT(net_by_mac(PRIVATE_MAC)->used_by, 0);
    th_halt();
}

TEST(net_names_only_from_who_may_answer_for_that_address) {
    boot();
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r, 2, T0, false);
    uint32_t phone = ip("192.168.1.10"), laptop = ip("192.168.1.11"), router = ip("192.168.1.1");
    CHECK(!net_ptr_answer(phone, 5353, 0, phone, "adams-iphone", T0)); /* nobody asked yet */
    net_by_mac(ADAMS_PHONE)->ptr_asked = T0;
    net_by_mac(PRIVATE_MAC)->ptr_asked = T0;
    /* review: any packet from port 5353 on the LAN could set how another device is named */
    CHECK(!net_ptr_answer(laptop, 5353, 0, phone, "evil-name", T0 + 1));
    CHECK(!net_ptr_answer(phone, 5354, 0, phone, "adams-iphone", T0 + 1));
    CHECK(!net_ptr_answer(laptop, 53, 0, phone, "evil-name", T0 + 1)); /* DNS answers only from the router */
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "");
    CHECK(net_ptr_answer(phone, 5353, 0, phone, "adams-iphone", T0 + 1)); /* mDNS from the device itself */
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "adams-iphone");
    CHECK(net_ptr_answer(router, 53, 0, laptop, "work-laptop", T0 + 2)); /* the router answers for anyone */
    CHECK_STR(net_by_mac(PRIVATE_MAC)->hostname, "work-laptop");
    CHECK(!net_ptr_answer(phone, 5353, 0, phone, "renamed", T0 + 120)); /* long after the question */
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "adams-iphone");
    th_halt();
}
