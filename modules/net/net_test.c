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

/* The hub's connection in words, not "en0" (issue #62); the interface's name when it can't tell. */
TEST(net_hub_says_how_it_is_connected) {
    boot();
    int adam = admin("Adam");
    const char *html = th_page(adam, "/net");
    CHECK_HAS(html, "<dt>Connected</dt><dd>through eth0</dd>");
    CHECK_HAS(th_page(adam, "/system"), "Connected through &quot;eth0&quot; as 192.168.1.2, with the router at "
                                          "192.168.1.1.");
    LanIface f = {0};
    str_copy(f.name, sizeof f.name, "en0");
    ip_parse("192.168.1.2", &f.ip);
    ip_parse("255.255.255.0", &f.mask);
    ip_parse("192.168.1.1", &f.gateway);
    f.link = LAN_LINK_WIFI;
    net_set_iface(&f);
    html = th_page(adam, "/net");
    CHECK_HAS(html, "<dt>Connected</dt><dd title=\"en0\">over Wi-Fi</dd>");
    CHECK(!strstr(html, "Interface"));
    CHECK_HAS(th_page(adam, "/system"), "Connected over Wi-Fi as 192.168.1.2");
    f.link = LAN_LINK_CABLE;
    net_set_iface(&f);
    CHECK_HAS(th_page(adam, "/net"), ">over a network cable</dd>");
    th_halt();
}

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
    /* several more within the hour: counted on the same line (issue #10) */
    Neighbor r4[] = {nb("192.168.1.50", "a4:83:e7:55:55:01", NEIGH_REACHABLE),
                     nb("192.168.1.51", "a4:83:e7:55:55:02", NEIGH_REACHABLE),
                     nb("192.168.1.52", "a4:83:e7:55:55:03", NEIGH_REACHABLE),
                     nb("192.168.1.53", "a4:83:e7:55:55:03", NEIGH_REACHABLE)}; /* one device, two addresses */
    net_ingest(r4, countof(r4), T0 + 240, true);
    CHECK_INT(tbl_count(&T_device), 6);
    CHECK_STR(net_by_mac("a4:83:e7:55:55:03")->last_ip, "192.168.1.53");
    CHECK_INT(tbl_count(&T_activity), before + 1);
    a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "4 new devices on the network");
    /* an hour after the line started: a new one */
    Neighbor r5[] = {nb("192.168.1.60", "a4:83:e7:66:66:01", NEIGH_REACHABLE),
                     nb("192.168.1.61", "a4:83:e7:66:66:02", NEIGH_REACHABLE)};
    net_ingest(r5, countof(r5), T0 + 120 + 3600, true);
    CHECK_INT(tbl_count(&T_activity), before + 2);
    a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "2 new devices on the network");
    th_halt();
}

/* Review of issue #10: one line a round still filled the 300-line feed in ~10 hours with one
   fake device a sweep (and toasted every screen each time). */
TEST(net_new_devices_make_one_line_an_hour) {
    boot();
    int adam = admin("Adam");
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    net_ingest(NULL, 0, T0, true); /* the baseline round */
    int before = tbl_count(&T_activity);
    for (int r = 0; r < 60; r++) { /* two hours of one new MAC a sweep */
        char mac[18];
        snprintf(mac, sizeof mac, "02:55:00:00:00:%02x", r);
        Neighbor one[] = {nb("192.168.1.11", mac, NEIGH_REACHABLE)};
        net_ingest(one, 1, T0 + NET_SCAN_EVERY * (r + 1), true);
    }
    CHECK_INT(tbl_count(&T_device), 60);
    CHECK_INT(tbl_count(&T_activity), before + 2);
    CHECK_STR(((Activity *)tbl_at(&T_activity, before))->text, "30 new devices on the network");
    CHECK_STR(((Activity *)tbl_at(&T_activity, before + 1))->text, "30 new devices on the network");
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

    /* the card renders with nobody signed in too */
    Ctx c;
    app_ctx(&c, 0, "/");
    Buf b = {0};
    module_find("net")->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "devices online");
    int eva = th_person("Eva");
    CHECK(!th_act(eva, "net.scan", "on=1").ok); /* members can't switch discovery on */
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    CHECK(th_act(adam, "net.save", "id=1&owner=1").ok);
    net_ingest(r, 2, T0 + 30, true);
    buf_reset(&b);
    c.now = T0 + 30;
    module_find("net")->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "Home now"); /* a device has an owner now: who's home shows */
    buf_free(&b);
    app_ctx_free(&c);
    CHECK_HAS(th_page(adam, "/net"), "aria-checked=\"true\"");
    /* ARP can't be authenticated: who's home is labelled a guess, for members too (issue #10) */
    CHECK_HAS(th_page(adam, "/net"), "good guess, not proof");
    CHECK_HAS(th_page(eva, "/net"), "good guess, not proof");
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
    uint16_t pd, pm, ld, lm, xd, xm;
    CHECK(net_ptr_ask(phone, T0, &pd, &pm));
    CHECK(net_ptr_ask(laptop, T0, &ld, &lm));
    CHECK(!net_ptr_ask(ip("192.168.1.77"), T0, &xd, &xm)); /* nobody seen there */
    CHECK(pd != pm && ld != lm); /* the multicast mDNS id says nothing about the router's */
    /* review: any packet from port 5353 on the LAN could set how another device is named
       (and such packets don't spend the question either) */
    CHECK(!net_ptr_answer(laptop, 5353, pm, phone, "evil-name", T0 + 1));
    CHECK(!net_ptr_answer(phone, 5354, pm, phone, "evil-name", T0 + 1));
    CHECK(!net_ptr_answer(laptop, 53, pd, phone, "evil-name", T0 + 1)); /* DNS answers only from the router */
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "");
    CHECK(net_ptr_answer(phone, 5353, pm, phone, "adams-iphone", T0 + 1)); /* mDNS from the device itself */
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "adams-iphone");
    CHECK(!net_ptr_answer(phone, 5353, pm, phone, "renamed", T0 + 2)); /* one answer per question */
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "adams-iphone");
    CHECK(net_ptr_answer(router, 53, ld, laptop, "work-laptop", T0 + 2)); /* the router answers for anyone */
    CHECK_STR(net_by_mac(PRIVATE_MAC)->hostname, "work-laptop");
    CHECK(net_ptr_ask(phone, T0 + 10, &pd, &pm));
    CHECK(!net_ptr_answer(phone, 5353, pm, phone, "renamed", T0 + 120)); /* long after the question */
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "adams-iphone");

    /* issue #10: the right sender with a wrong id fails - and spends the question, so the
       right id can't come next as a second guess. Wrong ids: one off, the mDNS id every device
       saw (on the router's DNS answer), the router's id on mDNS, another address's id, and a
       multicast-style 0. */
    for (int k = 0; k < 5; k++) {
        CHECK(net_ptr_ask(phone, T0 + 200, &pd, &pm));
        uint32_t from = k == 1 ? router : phone;
        uint16_t port = k == 1 ? 53 : 5353, right = k == 1 ? pd : pm;
        uint16_t wrong = k == 0 ? (uint16_t)(pm + 1) : k == 1 ? pm : k == 2 ? pd : k == 3 ? lm : 0;
        if (wrong == right) continue; /* random ids: a 1-in-65,536 chance */
        CHECK(!net_ptr_answer(from, port, wrong, phone, "evil-name", T0 + 201));
        CHECK(!net_ptr_answer(from, port, right, phone, "evil-name", T0 + 201));
    }
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "adams-iphone");
    th_halt();
}

/* The router's DNS response to the question dns_ptr_query makes: `rcode` 3 = no such name;
   with a name, one PTR answer. */
static size_t dns_reply(uint8_t *out, uint32_t about, uint16_t id, int rcode, const char *name) {
    size_t n = dns_ptr_query(about, id, false, out, 512);
    out[2] = 0x81; /* QR, RD */
    out[3] = (uint8_t)(0x80 | rcode); /* RA */
    if (!name) return n;
    out[7] = 1; /* ANCOUNT */
    uint8_t *p = out + n;
    size_t len = strlen(name);
    static const uint8_t rr[] = {0xC0, 12, 0, 12, 0, 1, 0, 0, 0, 60}; /* the question's name, PTR IN, TTL */
    memcpy(p, rr, sizeof rr);
    p += sizeof rr;
    *p++ = 0;
    *p++ = (uint8_t)(len + 2);
    *p++ = (uint8_t)len;
    memcpy(p, name, len);
    p += len;
    *p++ = 0;
    return (size_t)(p - out);
}

/* Review of issue #10: a wrong id didn't spend the router's question and the router's "no such
   name" didn't close it, so whoever saw the multicast question could spray guesses at the
   router's id for a minute. Now the router's first response - right, wrong or "no such
   name" - is the only one that counts. */
TEST(net_the_routers_first_response_closes_the_question) {
    boot();
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE), nb("192.168.1.11", PRIVATE_MAC, NEIGH_REACHABLE)};
    net_ingest(r, 2, T0, false);
    uint32_t phone = ip("192.168.1.10"), laptop = ip("192.168.1.11"), router = ip("192.168.1.1");
    uint16_t pd, pm, ld, lm;
    uint8_t pkt[512];
    /* a sprayed guess, then the real answer: the guess spent the question */
    CHECK(net_ptr_ask(phone, T0, &pd, &pm));
    net_dns_packet(router, 53, pkt, dns_reply(pkt, phone, (uint16_t)(pd + 1), 0, "evil-name"), T0 + 1);
    net_dns_packet(router, 53, pkt, dns_reply(pkt, phone, pd, 0, "adams-iphone"), T0 + 1);
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "");
    /* the router's "no such name" closes it: a right guess after it doesn't count */
    CHECK(net_ptr_ask(laptop, T0 + 2, &ld, &lm));
    net_dns_packet(router, 53, pkt, dns_reply(pkt, laptop, ld, 3, NULL), T0 + 3);
    net_dns_packet(router, 53, pkt, dns_reply(pkt, laptop, ld, 0, "evil-name"), T0 + 3);
    CHECK_STR(net_by_mac(PRIVATE_MAC)->hostname, "");
    /* a "no such name" with another id, or from anyone but the router, closes nothing; nor do
       questions (not responses) */
    CHECK(net_ptr_ask(laptop, T0 + 4, &ld, &lm));
    net_dns_packet(router, 53, pkt, dns_reply(pkt, laptop, (uint16_t)(ld + 1), 3, NULL), T0 + 5);
    net_dns_packet(phone, 53, pkt, dns_reply(pkt, laptop, ld, 3, NULL), T0 + 5);
    net_dns_packet(router, 53, pkt, dns_ptr_query(laptop, ld, false, pkt, sizeof pkt), T0 + 5);
    net_dns_packet(router, 54, pkt, dns_reply(pkt, laptop, ld, 0, "evil-name"), T0 + 5);
    net_dns_packet(router, 53, pkt, 5, T0 + 5); /* runt */
    CHECK_STR(net_by_mac(PRIVATE_MAC)->hostname, "");
    net_dns_packet(router, 53, pkt, dns_reply(pkt, laptop, ld, 0, "work-laptop"), T0 + 5);
    CHECK_STR(net_by_mac(PRIVATE_MAC)->hostname, "work-laptop");
    /* the device's own mDNS answer, through the same door */
    CHECK(net_ptr_ask(phone, T0 + 6, &pd, &pm));
    net_dns_packet(phone, 5353, pkt, dns_reply(pkt, phone, pm, 0, "adams-iphone"), T0 + 7);
    CHECK_STR(net_by_mac(ADAMS_PHONE)->hostname, "adams-iphone");
    th_halt();
}

/* Query ids used to count up from 0, so anyone who saw one multicast question knew the next. */
TEST(net_name_questions_carry_fresh_random_ids) {
    boot();
    Neighbor r[] = {nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE)};
    net_ingest(r, 1, T0, false);
    uint32_t phone = ip("192.168.1.10");
    uint16_t id[16];
    for (int i = 0; i < 8; i++) CHECK(net_ptr_ask(phone, T0, &id[2 * i], &id[2 * i + 1]));
    int steps = 0, same = 0;
    for (int i = 1; i < 16; i++) steps += id[i] == (uint16_t)(id[i - 1] + 1);
    for (int i = 0; i < 16; i++)
        for (int k = i + 1; k < 16; k++) same += id[i] == id[k];
    CHECK(steps < 3 && same < 3);
    /* a new question replaces the old one: its id no longer counts (and, wrong now, it spends
       the question) */
    if (id[13] != id[15]) {
        CHECK(!net_ptr_answer(phone, 5353, id[13], phone, "old-question", T0 + 1));
        CHECK(!net_ptr_answer(phone, 5353, id[15], phone, "adams-iphone", T0 + 1));
    }
    CHECK(net_ptr_ask(phone, T0, &id[0], &id[1]));
    CHECK(net_ptr_answer(phone, 5353, id[1], phone, "adams-iphone", T0 + 1));
    th_halt();
}

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

/* A made-up device: MACs no household has, addresses .11-.250. */
static Neighbor forged(int round, int i) {
    char mac[18], addr[16];
    snprintf(mac, sizeof mac, "02:66:%02x:%02x:%02x:01", round & 255, (i >> 8) & 255, i & 255);
    snprintf(addr, sizeof addr, "192.168.1.%d", 11 + i % 240);
    return nb(addr, mac, NEIGH_REACHABLE);
}

/* Issue #10: forged ARP from any device on the LAN grew the list (and the page, and the
   activity feed) without limit - 40 rounds made 10,120 devices and a 4.9 MB /net page. */
TEST(net_forged_arp_cannot_grow_the_list_without_limit) {
    boot();
    int adam = admin("Adam");
    int eva = th_person("Eva");
    Neighbor mine[] = {
        nb("192.168.1.10", ADAMS_PHONE, NEIGH_REACHABLE),
        nb("192.168.1.9", "a4:83:e7:aa:bb:cc", NEIGH_REACHABLE),  /* named: the gaming PC */
        nb("192.168.1.8", PRIVATE_MAC, NEIGH_REACHABLE),          /* Eva opens Hemma from it */
        nb("192.168.1.7", "a4:83:e7:aa:bb:dd", NEIGH_REACHABLE),  /* only set up for Wake-on-LAN */
        nb("192.168.1.6", "a4:83:e7:aa:bb:ee", NEIGH_REACHABLE),  /* nobody's: may go */
    };
    net_ingest(mine, countof(mine), T0, false);
    CHECK(th_act(adam, "net.save", "id=1&name=Adams+phone&owner=1").ok);
    CHECK(th_act(adam, "net.save", "id=2&name=Gaming+PC").ok);
    net_learn(eva, ip("192.168.1.8"), T0);
    CHECK(th_act(adam, "net.save", "id=4&wol=1").ok);
    CHECK(th_act(adam, "net.scan", "on=1").ok);
    net_ingest(mine, 1, T0, true); /* the baseline round; only Adam's phone keeps showing up */
    int news = tbl_count(&T_activity);

    static Neighbor r[241];
    int64_t t = T0;
    for (int round = 0; round < 8; round++) { /* 1,920 made-up devices, a sweep apart */
        t += NET_SCAN_EVERY;
        for (int i = 0; i < 240; i++) r[i] = forged(round, i);
        r[240] = mine[0];
        net_ingest(r, 241, t, true);
        CHECK(tbl_count(&T_device) <= NET_MAX_DEVICES);
    }
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    /* the household's devices stay, however long unseen (the one Eva opens Hemma from goes
       last); the longest-unseen strangers went */
    CHECK(net_by_mac(ADAMS_PHONE) && net_by_mac("a4:83:e7:aa:bb:cc") && net_by_mac(PRIVATE_MAC));
    CHECK(net_by_mac("a4:83:e7:aa:bb:dd") && net_by_mac("a4:83:e7:aa:bb:dd")->wol);
    CHECK(net_by_mac("a4:83:e7:aa:bb:ee") == NULL);
    char mac[18];
    Neighbor first = forged(0, 0), last = forged(7, 239);
    mac_format(&first.mac, mac);
    CHECK(net_by_mac(mac) == NULL);
    mac_format(&last.mac, mac);
    CHECK(net_by_mac(mac) != NULL);
    CHECK(presence_is_home(adam, NULL));

    /* one line in the activity feed for the lot, not one per device or per round */
    CHECK_INT(tbl_count(&T_activity), news + 1);
    Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "1920 new devices on the network");
    /* devices forgotten to make room that come back aren't news again */
    t += NET_SCAN_EVERY;
    Neighbor back[10];
    for (int i = 0; i < 10; i++) back[i] = forged(3, i); /* forgotten in the last round */
    mac_format(&back[0].mac, mac);
    CHECK(net_by_mac(mac) == NULL);
    net_ingest(back, 10, t, true);
    CHECK(net_by_mac(mac) != NULL);
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    CHECK_INT(tbl_count(&T_activity), news + 1);
    CHECK_STR(((Activity *)tbl_at(&T_activity, tbl_count(&T_activity) - 1))->text, "1920 new devices on the network");

    /* the page shows a step at a time, the household's own devices first */
    const char *html = th_page(adam, "/net");
    CHECK_INT(count_of(html, "<li class=\"net-dev"), 100);
    CHECK(strlen(html) < 100000); /* ~58 KB; the whole list would be ~540 KB */
    CHECK_HAS(html, "Adams phone");
    CHECK_HAS(html, "Show 100 more");
    CHECK_HAS(html, "100 of 1024 shown");
    CHECK_HAS(html, "?n=200");
    CHECK_INT(count_of(th_page(adam, "/net?n=200"), "<li class=\"net-dev"), 200);
    html = th_page(adam, "/net?n=99999");
    CHECK_INT(count_of(html, "<li class=\"net-dev"), NET_MAX_DEVICES);
    CHECK(!strstr(html, "Show 100 more"));
    CHECK_INT(count_of(th_page(adam, "/net?n=-5"), "<li class=\"net-dev"), 100);
    CHECK(!strstr(th_page(adam, "/net?f=people&n=300"), "Show 100 more"));
    CHECK(!strstr(th_page(adam, "/net"), "net-tidy")); /* nothing a week old yet */

    /* a week later an admin cleans the list in one go: what the household set up stays */
    g_fake_now = t + 8 * 86400;
    CHECK_HAS(th_page(adam, "/net"), "Forget 1021 unknown devices not seen for a week");
    CHECK(!strstr(th_page(eva, "/net"), "net-tidy"));
    CHECK(!th_act(eva, "net.tidy", "").ok);
    ThResult tidy = th_act(adam, "net.tidy", "");
    CHECK(tidy.ok);
    CHECK_HAS(tidy.toast, "Forgot 1021 devices");
    CHECK_INT(tbl_count(&T_device), 3);
    CHECK(net_by_mac(ADAMS_PHONE) && net_by_mac("a4:83:e7:aa:bb:cc") && net_by_mac("a4:83:e7:aa:bb:dd"));
    CHECK(!strstr(th_page(adam, "/net"), "net-tidy"));
    th_halt();
}

/* Review of issue #10: every device that ever claimed the router's address counted as the
   router - labelled "Router" and never forgotten - so a fake "router" with a new MAC each
   round could fill the list for good. Now only the device at that address right now is. */
TEST(net_only_the_device_at_the_routers_address_is_the_router) {
    boot();
    int adam = admin("Adam");
    const char *router_mac = "a4:83:e7:00:00:01";
    Neighbor real[] = {nb("192.168.1.1", router_mac, NEIGH_REACHABLE)};
    net_ingest(real, 1, T0, false);
    CHECK_STR(net_label(net_by_mac(router_mac)), "Router");
    for (int r = 0; r < 5; r++) { /* someone else answers for the router's address, a new MAC each round */
        char mac[18];
        snprintf(mac, sizeof mac, "02:99:00:00:00:%02x", r);
        Neighbor fake[] = {nb("192.168.1.1", mac, NEIGH_REACHABLE)};
        net_ingest(fake, 1, T0 + 30 * (r + 1), false);
    }
    int routers = 0;
    for (int i = 0; i < tbl_count(&T_device); i++) routers += str_eq(net_label(tbl_at(&T_device, i)), "Router");
    CHECK_INT(routers, 1);
    CHECK_STR(net_label(net_by_mac("02:99:00:00:00:04")), "Router"); /* whoever has the address now */
    CHECK_STR(net_label(net_by_mac(router_mac)), "Unknown device");
    net_ingest(real, 1, T0 + 200, false); /* the real one is back at its address */
    CHECK_STR(net_label(net_by_mac(router_mac)), "Router");
    CHECK_STR(net_label(net_by_mac("02:99:00:00:00:04")), "Unknown device");
    CHECK_HAS(th_page(adam, "/net?f=unknown"), "id=\"net-dev-6\"");

    /* a list already full of such "routers", and of devices someone once opened Hemma from,
       still makes room for a newcomer */
    db_begin();
    for (int i = tbl_count(&T_device); i < NET_MAX_DEVICES; i++) {
        Device d = {0};
        snprintf(d.mac, sizeof d.mac, "02:98:00:00:%02x:%02x", i >> 8, i & 255);
        str_copy(d.last_ip, sizeof d.last_ip, i % 2 ? "192.168.1.1" : "192.168.1.20");
        d.first_seen = d.last_seen = T0 - 5000 + i;
        d.used_by = i % 3 == 0 ? adam : 0;
        tbl_put(&T_device, &d);
    }
    db_commit();
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    Neighbor guest[] = {real[0], nb("192.168.1.30", "a4:83:e7:30:30:30", NEIGH_REACHABLE)};
    net_ingest(guest, 2, T0 + 3600, false);
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    CHECK(net_by_mac("a4:83:e7:30:30:30") != NULL);
    CHECK_STR(net_label(net_by_mac(router_mac)), "Router");
    /* the oldest went - but one someone opens Hemma from goes after everyone else */
    CHECK(net_by_mac("02:98:00:00:00:06") != NULL); /* used, and the oldest */
    CHECK(net_by_mac("02:98:00:00:00:07") == NULL);
    th_halt();
}

TEST(net_a_full_list_keeps_who_is_here_and_leaves_newcomers_out) {
    boot();
    int adam = admin("Adam");
    LanIface f = {0}; /* a /16: one round can bring a whole list */
    str_copy(f.name, sizeof f.name, "eth0");
    ip_parse("192.168.0.2", &f.ip);
    ip_parse("255.255.0.0", &f.mask);
    ip_parse("192.168.0.1", &f.gateway);
    ip_parse("192.168.255.255", &f.broadcast);
    net_set_iface(&f);
    enum { N = NET_MAX_DEVICES + 10 };
    static Neighbor r[N];
    char mac[N][18];
    for (int i = 0; i < N; i++) {
        char addr[16];
        snprintf(addr, sizeof addr, "192.168.%d.%d", 4 + i / 250, 1 + i % 250);
        snprintf(mac[i], sizeof mac[i], "02:77:00:00:%02x:%02x", i >> 8, i & 255);
        r[i] = nb(addr, mac[i], NEIGH_REACHABLE);
    }
    net_ingest(r, NET_MAX_DEVICES, T0, false);
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    /* everyone is on the network right now: nobody makes room, the newcomers wait */
    net_ingest(r + NET_MAX_DEVICES, 10, T0 + 30, false);
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    CHECK(net_by_mac(mac[NET_MAX_DEVICES]) == NULL);
    /* an hour later they've gone quiet: the oldest make room - but not one still listed, if
       only as stale (it would come straight back as "new") */
    static Neighbor later[11];
    memcpy(later, r + NET_MAX_DEVICES, sizeof(Neighbor) * 10);
    later[10] = r[0];
    later[10].state = NEIGH_STALE;
    net_ingest(later, 11, T0 + 3600, false);
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    CHECK(net_by_mac(mac[NET_MAX_DEVICES]) && net_by_mac(mac[N - 1]));
    CHECK(net_by_mac(mac[0]) != NULL && net_by_mac(mac[0])->last_seen == T0);
    CHECK(net_by_mac(mac[1]) == NULL && net_by_mac(mac[10]) == NULL && net_by_mac(mac[11]) != NULL);

    /* a list of devices the household named: nothing may go, so strangers stay out */
    db_begin();
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device d = *(Device *)tbl_at(&T_device, i);
        snprintf(d.name, sizeof d.name, "Lamp %d", i);
        tbl_put(&T_device, &d);
    }
    db_commit();
    Neighbor late[] = {nb("192.168.9.9", "02:77:ff:ff:ff:01", NEIGH_REACHABLE)};
    net_ingest(late, 1, T0 + 7200, false);
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    CHECK(net_by_mac("02:77:ff:ff:ff:01") == NULL);
    char form[32];
    snprintf(form, sizeof form, "id=%d", net_by_mac(mac[11])->id);
    CHECK(th_act(adam, "net.forget", form).ok); /* forgetting one makes room */
    net_ingest(late, 1, T0 + 7230, false);
    CHECK(net_by_mac("02:77:ff:ff:ff:01") != NULL);
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    th_halt();
}

/* A list that grew before there was a limit shrinks at the next start. */
TEST(net_an_overgrown_list_shrinks_at_start) {
    char data[512];
    str_copy(data, sizeof data, test_tmpdir());
    app_set_testing(true);
    g_fake_now = dt_make(2026, 10, 10, 18, 0);
    CHECK(app_boot(data));
    enum { N = NET_MAX_DEVICES + 50 };
    char mac[N][18];
    db_begin();
    for (int i = 0; i < N; i++) {
        Device d = {0};
        snprintf(mac[i], sizeof mac[i], "02:88:00:00:%02x:%02x", i >> 8, i & 255);
        str_copy(d.mac, sizeof d.mac, mac[i]);
        snprintf(d.last_ip, sizeof d.last_ip, "192.168.%d.%d", 4 + i / 250, 1 + i % 250);
        d.first_seen = d.last_seen = g_fake_now - 86400 + i; /* the first rows are the oldest */
        if (i < 30) str_copy(d.name, sizeof d.name, "Kept"); /* ...and named */
        tbl_put(&T_device, &d);
    }
    db_commit();
    app_halt();
    CHECK(app_boot(data));
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    CHECK(net_by_mac(mac[0]) != NULL && net_by_mac(mac[29]) != NULL); /* named: kept though oldest */
    CHECK(net_by_mac(mac[30]) == NULL && net_by_mac(mac[79]) == NULL);
    CHECK(net_by_mac(mac[80]) != NULL && net_by_mac(mac[N - 1]) != NULL);
    app_halt();
    CHECK(app_boot(data)); /* the deletes reached the database */
    CHECK_INT(tbl_count(&T_device), NET_MAX_DEVICES);
    app_halt();
}
