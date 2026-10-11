#include "../tests/test.h"
#include "lan.h"

/* ----------------------------------------------------------------- MAC */

TEST(mac_parse_accepts_common_forms) {
    Mac m;
    char s[18];
    CHECK(mac_parse("aa:bb:cc:dd:ee:ff", &m));
    mac_format(&m, s);
    CHECK_STR(s, "aa:bb:cc:dd:ee:ff");
    CHECK(mac_parse("AA-BB-CC-DD-EE-0F", &m));
    mac_format(&m, s);
    CHECK_STR(s, "aa:bb:cc:dd:ee:0f");
    CHECK(mac_parse("3C22FB0A1B2C", &m));
    mac_format(&m, s);
    CHECK_STR(s, "3c:22:fb:0a:1b:2c");
    CHECK(mac_parse("aabb.ccdd.eeff", &m));
    mac_format(&m, s);
    CHECK_STR(s, "aa:bb:cc:dd:ee:ff");
    CHECK(mac_parse("a:b:c:d:e:f", &m)); /* single-digit groups (12 chars) */
    mac_format(&m, s);
    CHECK_STR(s, "0a:0b:0c:0d:0e:0f");
    CHECK(mac_parse("a:b:c:dd:ee:ff", &m)); /* 14 chars but grouped, not Cisco */
    mac_format(&m, s);
    CHECK_STR(s, "0a:0b:0c:dd:ee:ff");
    CHECK(mac_parse("  de:ad:be:ef:00:01\n", &m));
    mac_format(&m, s);
    CHECK_STR(s, "de:ad:be:ef:00:01");
}

TEST(mac_parse_rejects_garbage) {
    Mac m;
    const char *bad[] = {
        "",
        "   ",
        "aa:bb:cc:dd:ee",       /* 5 groups */
        "aa:bb:cc:dd:ee:ff:00", /* 7 groups */
        "aa:bb-cc:dd:ee:ff",    /* mixed separators */
        "gg:bb:cc:dd:ee:ff",    /* not hex */
        "aaa:bb:cc:dd:ee:ff",   /* 3-digit group */
        "aa::cc:dd:ee:ff",      /* empty group */
        "aa:bb:cc:dd:ee:ff:",   /* trailing separator */
        ":aa:bb:cc:dd:ee:ff",   /* leading separator */
        "aabbccddeef",          /* 11 hex digits */
        "aabbccddeeff0",        /* 13 hex digits */
        "aabb.ccdd.eef",        /* short Cisco */
        "aab.bccdd.eeff",       /* misplaced dots */
        "aabbccdd.eeff",
        "aa bb cc dd ee ff",
    };
    for (int i = 0; i < countof(bad); i++) {
        if (mac_parse(bad[i], &m)) {
            test_fail(__FILE__, __LINE__, "accepted bad MAC \"%s\"", bad[i]);
            return;
        }
    }
    CHECK(!mac_parse(NULL, &m));
}

TEST(mac_flags) {
    Mac m;
    CHECK(mac_parse("00:00:00:00:00:00", &m) && mac_is_zero(&m) && !mac_is_multicast(&m) && !mac_is_random(&m));
    CHECK(mac_parse("ff:ff:ff:ff:ff:ff", &m) && mac_is_multicast(&m) && !mac_is_random(&m) && !mac_is_zero(&m));
    CHECK(mac_parse("01:00:5e:00:00:fb", &m) && mac_is_multicast(&m));
    CHECK(mac_parse("da:a1:19:12:34:56", &m) && mac_is_random(&m) && !mac_is_multicast(&m)); /* iPhone private */
    CHECK(mac_parse("02:00:00:00:00:01", &m) && mac_is_random(&m));
    CHECK(mac_parse("3c:22:fb:12:34:56", &m) && !mac_is_random(&m) && !mac_is_multicast(&m));
    Mac a, b;
    CHECK(mac_parse("aa:bb:cc:dd:ee:ff", &a) && mac_parse("AABBCCDDEEFF", &b) && mac_eq(&a, &b));
    b.b[5] ^= 1;
    CHECK(!mac_eq(&a, &b));
}

/* ------------------------------------------------------- /proc/net/arp */

TEST(proc_arp_parsing) {
    const char *text =
        "IP address       HW type     Flags       HW address            Mask     Device\n"
        "192.168.1.1      0x1         0x2         aa:bb:cc:dd:ee:01     *        eth0\n"
        "192.168.1.50     0x1         0x0         00:00:00:00:00:00     *        eth0\n"
        "192.168.1.7      0x1         0x6         AA:BB:CC:DD:EE:07     *        eth0\n"
        "224.0.0.251      0x1         0x2         01:00:5e:00:00:fb     *        eth0\n"
        "192.168.1.9      0x1         0x2         01:00:5e:00:00:09     *        eth0\n"
        "192.168.1.10     0x1         0x2         00:00:00:00:00:00     *        eth0\n"
        "garbage line\n"
        "1.2.3\n"
        "\n"
        "192.168.1.11     0x1         0xZZ        aa:bb:cc:dd:ee:11     *        eth0\n"
        "192.168.1.12     0x1         0x2         not-a-mac             *        eth0\r\n"
        "10.0.0.5\t0x1\t0x2\tde:ad:be:ef:00:05\t*\twlan0"; /* tabs, no final newline */
    Neighbor nb[16];
    int n = lan_parse_proc_arp(text, nb, 16);
    CHECK_INT(n, 4);
    CHECK_INT(nb[0].ip, 0xC0A80101);
    CHECK_INT(nb[0].state, NEIGH_REACHABLE);
    CHECK_INT(nb[0].mac.b[5], 0x01);
    CHECK_INT(nb[1].ip, 0xC0A80132); /* incomplete: reported as failed */
    CHECK_INT(nb[1].state, NEIGH_FAILED);
    CHECK(mac_is_zero(&nb[1].mac));
    CHECK_INT(nb[2].ip, 0xC0A80107); /* permanent */
    CHECK_INT(nb[2].state, NEIGH_REACHABLE);
    CHECK_INT(nb[3].ip, 0x0A000005);
    CHECK_INT(nb[3].mac.b[0], 0xde);
    /* max is honoured */
    CHECK_INT(lan_parse_proc_arp(text, nb, 2), 2);
    CHECK_INT(lan_parse_proc_arp("", nb, 16), 0);
    CHECK_INT(lan_parse_proc_arp(NULL, nb, 16), 0);
}

TEST(proc_route_picks_lowest_metric_default) {
    const char *text = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                       "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t303\t00000000\t0\t0\t0\n"
                       "eth0\t00000000\tFE01A8C0\t0003\t0\t0\t202\t00000000\t0\t0\t0\n"
                       "eth0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"
                       "down0\t00000000\t0101A8C0\t0002\t0\t0\t1\t00000000\t0\t0\t0\n"; /* not RTF_UP */
    char ifname[64] = "";
    uint32_t gw = 0;
    CHECK(lan_parse_proc_route(text, ifname, &gw));
    CHECK_STR(ifname, "eth0");
    CHECK_INT(gw, 0xC0A801FE); /* 192.168.1.254 */

    const char *nogw = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n"
                       "tun0\t00000000\t00000000\t0001\t0\t0\t0\t00000000\n";
    CHECK(lan_parse_proc_route(nogw, ifname, &gw));
    CHECK_STR(ifname, "tun0");
    CHECK_INT(gw, 0);

    const char *none = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\n"
                       "eth0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\n";
    CHECK(!lan_parse_proc_route(none, ifname, &gw));
    CHECK(!lan_parse_proc_route("", ifname, &gw));
}

/* ------------------------------------------- macOS routing-socket records */

TEST(rt_addrs_arp_entry_and_default_route) {
    /* ARP entry: RTA_DST (sockaddr_inarp) + RTA_GATEWAY (sockaddr_dl "en0" + MAC) */
    const uint8_t arp[] = {16, 2, 0, 0, 192, 168, 1, 7, 0, 0, 0, 0, 0, 0, 0, 0,               /* 16 */
                           20, 18, 4, 0, 6, 3, 6, 0, 'e', 'n', '0', 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x07, 0, 0, 0};
    RtAddrs a;
    CHECK(lan_parse_rt_addrs(arp, sizeof arp, 0x3, &a));
    CHECK_INT(a.dst, 0xC0A80107);
    CHECK_INT(a.gateway, 0);
    CHECK(a.has_mac);
    char s[18];
    mac_format(&a.mac, s);
    CHECK_STR(s, "aa:bb:cc:dd:ee:07");

    /* incomplete entry: no link-layer address yet */
    const uint8_t inc[] = {16, 2, 0, 0, 192, 168, 1, 50, 0, 0, 0, 0, 0, 0, 0, 0,
                           12, 18, 4, 0, 6, 0, 0, 0, 0, 0, 0, 0};
    CHECK(lan_parse_rt_addrs(inc, sizeof inc, 0x3, &a));
    CHECK_INT(a.dst, 0xC0A80132);
    CHECK(!a.has_mac);

    /* default route: dst 0.0.0.0, gateway 192.168.1.1, zero-length netmask (4 bytes) */
    const uint8_t def[] = {16, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                           16, 2, 0, 0, 192, 168, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
                           0, 0, 0, 0};
    CHECK(lan_parse_rt_addrs(def, sizeof def, 0x7, &a));
    CHECK_INT(a.dst, 0);
    CHECK_INT(a.gateway, 0xC0A80101);
    CHECK_INT(a.mask, 0);
    CHECK(!a.has_mac);

    /* subnet route with a cut-short netmask (7 bytes = 255.255.255.0, padded to 8) */
    const uint8_t net[] = {16, 2, 0, 0, 192, 168, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                           20, 18, 4, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                           7, 0xff, 0, 0, 255, 255, 255, 0};
    CHECK(lan_parse_rt_addrs(net, sizeof net, 0x7, &a));
    CHECK_INT(a.dst, 0xC0A80100);
    CHECK_INT(a.mask, 0xFFFFFF00);

    /* not IPv4 (AF_INET6 = 30 on macOS): no address */
    const uint8_t v6[] = {28, 30, 0, 0, 0, 0, 0, 0, 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
                          0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
    CHECK(lan_parse_rt_addrs(v6, sizeof v6, 0x1, &a));
    CHECK_INT(a.dst, 0);

    /* cut short: a sockaddr longer than the record, a missing one, NULL */
    CHECK(!lan_parse_rt_addrs(arp, 20, 0x3, &a));
    CHECK(!lan_parse_rt_addrs(arp, 16, 0x3, &a));
    CHECK(!lan_parse_rt_addrs(NULL, 4, 0x1, &a));
    CHECK(lan_parse_rt_addrs(NULL, 0, 0, &a));
    /* a hostile name length can't push the MAC read past the end */
    uint8_t bad[sizeof arp];
    memcpy(bad, arp, sizeof arp);
    bad[16 + 5] = 200;
    CHECK(lan_parse_rt_addrs(bad, sizeof bad, 0x3, &a));
    CHECK(!a.has_mac);
}

/* -------------------------------------------------------- subnet helpers */

TEST(subnet_helpers) {
    LanIface ifc = {.ip = 0xC0A8012A, .mask = 0xFFFFFF00};
    CHECK(lan_in_subnet(&ifc, 0xC0A801FE));
    CHECK(!lan_in_subnet(&ifc, 0xC0A80201));
    CHECK(!lan_in_subnet(&ifc, 0));
    CHECK(lan_is_private(0x0A000001) && lan_is_private(0xAC100001) && lan_is_private(0xAC1FFFFF));
    CHECK(lan_is_private(0xC0A80001));
    CHECK(!lan_is_private(0xAC200001) && !lan_is_private(0x08080808) && !lan_is_private(0x7F000001));
}

TEST(sweep_range_clamps_to_our_block) {
    uint32_t a = 0, b = 0;
    LanIface c24 = {.ip = 0xC0A8012A, .mask = 0xFFFFFF00};
    CHECK_INT(lan_sweep_range(&c24, &a, &b, 254), 254);
    CHECK_INT(a, 0xC0A80101);
    CHECK_INT(b, 0xC0A801FE);

    LanIface c16 = {.ip = 0x0A000507, .mask = 0xFFFF0000}; /* 10.0.5.7/16 */
    CHECK_INT(lan_sweep_range(&c16, &a, &b, 254), 254);
    CHECK_INT(a, 0x0A000501);
    CHECK_INT(b, 0x0A0005FE);
    CHECK_INT(lan_sweep_range(&c16, &a, &b, 1022), 1022); /* -> /22 */
    CHECK_INT(a, 0x0A000401);
    CHECK_INT(b, 0x0A0007FE);

    LanIface c30 = {.ip = 0xC0A80101, .mask = 0xFFFFFFFC};
    CHECK_INT(lan_sweep_range(&c30, &a, &b, 254), 2);
    LanIface c31 = {.ip = 0xC0A80101, .mask = 0xFFFFFFFE};
    CHECK_INT(lan_sweep_range(&c31, &a, &b, 254), 0);
    LanIface c32 = {.ip = 0xC0A80101, .mask = 0xFFFFFFFF};
    CHECK_INT(lan_sweep_range(&c32, &a, &b, 254), 0);
    LanIface none = {0};
    CHECK_INT(lan_sweep_range(&none, &a, &b, 254), 0);
    LanIface nomask = {.ip = 0xC0A8012A}; /* unknown mask: settle on our /24 */
    CHECK_INT(lan_sweep_range(&nomask, &a, &b, 254), 254);
    CHECK_INT(a, 0xC0A80101);
    CHECK_INT(lan_sweep_range(&c24, &a, &b, 0), 0);
}

/* ------------------------------------------------------------------ WoL */

TEST(wol_magic_packet) {
    Mac m;
    CHECK(mac_parse("01:23:45:67:89:ab", &m));
    uint8_t p[102];
    CHECK_INT(lan_wol_packet(&m, p), 102);
    for (int i = 0; i < 6; i++) CHECK_INT(p[i], 0xFF);
    for (int r = 0; r < 16; r++) CHECK(memcmp(p + 6 + 6 * r, m.b, 6) == 0);
}

/* ------------------------------------------------------------------ DNS */

TEST(dns_ptr_query_golden_bytes) {
    static const uint8_t want[] = {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* header, RD */
        2,    '4',  '2',  1,    '1',  3,    '1',  '6',  '8',  3,    '1',  '9',  '2',
        7,    'i',  'n',  '-',  'a',  'd',  'd',  'r',  4,    'a',  'r',  'p',  'a',  0,
        0x00, 0x0c, 0x00, 0x01, /* PTR, IN */
    };
    uint8_t q[64];
    size_t n = dns_ptr_query(0xC0A8012A, 0x1234, false, q, sizeof q);
    CHECK_INT(n, sizeof want);
    CHECK(memcmp(q, want, n) == 0);

    n = dns_ptr_query(0xC0A8012A, 0x1234, true, q, sizeof q);
    CHECK_INT(n, sizeof want);
    CHECK_INT(q[2], 0); /* mDNS: no RD */
    CHECK_INT(q[3], 0);
    CHECK_INT(q[n - 2], 0x80); /* QU bit */
    CHECK_INT(q[n - 1], 0x01);

    CHECK_INT(dns_ptr_query(0xFFFFFFFF, 1, false, q, sizeof q), 46); /* longest name */
    CHECK_INT(dns_ptr_query(0xC0A8012A, 1, false, q, 10), 0);
}

typedef struct {
    uint8_t b[512];
    size_t n;
} Pkt;

static void p8(Pkt *p, unsigned v) { p->b[p->n++] = (uint8_t)v; }
static void p16(Pkt *p, unsigned v) {
    p8(p, v >> 8);
    p8(p, v & 255);
}
static void p32(Pkt *p, unsigned long v) {
    p16(p, (unsigned)(v >> 16));
    p16(p, (unsigned)(v & 0xFFFF));
}
static void pname(Pkt *p, const char *dotted) {
    while (*dotted) {
        const char *dot = strchr(dotted, '.');
        size_t len = dot ? (size_t)(dot - dotted) : strlen(dotted);
        p8(p, (unsigned)len);
        memcpy(p->b + p->n, dotted, len);
        p->n += len;
        dotted += len;
        if (*dotted == '.') dotted++;
    }
    p8(p, 0);
}

/* Unicast answer: question at offset 12, answer owner compressed to it. */
static Pkt ptr_response(uint16_t id, const char *target) {
    Pkt p = {{0}, 0};
    p16(&p, id);
    p16(&p, 0x8180);
    p16(&p, 1), p16(&p, 1), p16(&p, 0), p16(&p, 0);
    pname(&p, "42.1.168.192.in-addr.arpa");
    p16(&p, 12), p16(&p, 1);
    p16(&p, 0xC00C);
    p16(&p, 12), p16(&p, 1), p32(&p, 3600);
    size_t at = p.n;
    p16(&p, 0);
    size_t start = p.n;
    pname(&p, target);
    p.b[at] = (uint8_t)((p.n - start) >> 8);
    p.b[at + 1] = (uint8_t)(p.n - start);
    return p;
}

TEST(dns_ptr_reply_unicast) {
    Pkt p = ptr_response(0xBEEF, "adams-iphone.lan");
    uint16_t id = 0;
    uint32_t ip = 0;
    char name[64];
    CHECK(dns_ptr_reply(p.b, p.n, &id, &ip, name, sizeof name));
    CHECK_INT(id, 0xBEEF);
    CHECK_INT(ip, 0xC0A8012A);
    CHECK_STR(name, "adams-iphone");
    /* small output buffer truncates safely */
    char tiny[6];
    CHECK(dns_ptr_reply(p.b, p.n, &id, &ip, tiny, sizeof tiny));
    CHECK_STR(tiny, "adams");
}

TEST(dns_ptr_reply_strips_home_suffixes) {
    static const struct {
        const char *in, *out;
    } cases[] = {
        {"nas.LAN", "nas"},
        {"box.localdomain", "box"},
        {"tv.home", "tv"},
        {"pi.home.arpa", "pi"},
        {"fritz-repeater.fritz.box", "fritz-repeater"},
        {"Adams-MacBook.local", "Adams-MacBook"},
        {"printer.example.com", "printer.example.com"},
        {"lan", "lan"},
    };
    for (int i = 0; i < countof(cases); i++) {
        Pkt p = ptr_response(1, cases[i].in);
        char name[64];
        CHECK(dns_ptr_reply(p.b, p.n, NULL, NULL, name, sizeof name));
        CHECK_STR(name, cases[i].out);
    }
}

TEST(dns_ptr_reply_follows_compression_in_rdata) {
    Pkt p = {{0}, 0};
    p16(&p, 7);
    p16(&p, 0x8180);
    p16(&p, 1), p16(&p, 1), p16(&p, 0), p16(&p, 0);
    pname(&p, "42.1.168.192.in-addr.arpa"); /* "arpa" label lives at offset 33 */
    p16(&p, 12), p16(&p, 1);
    p16(&p, 0xC00C);
    p16(&p, 12), p16(&p, 1), p32(&p, 60);
    p16(&p, 9); /* rdlength: 1+6 + 2 */
    p8(&p, 6);
    memcpy(p.b + p.n, "laptop", 6);
    p.n += 6;
    p16(&p, 0xC000 | 33);
    char name[64];
    uint32_t ip = 0;
    CHECK(dns_ptr_reply(p.b, p.n, NULL, &ip, name, sizeof name));
    CHECK_STR(name, "laptop.arpa");
    CHECK_INT(ip, 0xC0A8012A);
}

TEST(dns_ptr_reply_mdns_answer_after_other_records) {
    Pkt p = {{0}, 0};
    p16(&p, 0);      /* mDNS id */
    p16(&p, 0x8400); /* QR + AA */
    p16(&p, 0), p16(&p, 2), p16(&p, 0), p16(&p, 0);
    pname(&p, "foo.local"); /* an A record first */
    p16(&p, 1), p16(&p, 0x8001), p32(&p, 120), p16(&p, 4);
    p8(&p, 192), p8(&p, 168), p8(&p, 1), p8(&p, 9);
    pname(&p, "9.1.168.192.in-addr.arpa");
    p16(&p, 12), p16(&p, 0x8001), p32(&p, 120);
    size_t at = p.n;
    p16(&p, 0);
    size_t start = p.n;
    pname(&p, "Living-Room-TV.local");
    p.b[at + 1] = (uint8_t)(p.n - start);
    uint16_t id = 99;
    uint32_t ip = 0;
    char name[64];
    CHECK(dns_ptr_reply(p.b, p.n, &id, &ip, name, sizeof name));
    CHECK_INT(id, 0);
    CHECK_INT(ip, 0xC0A80109);
    CHECK_STR(name, "Living-Room-TV");
}

TEST(dns_ptr_reply_rejects_bad_packets) {
    char name[64];
    Pkt ok = ptr_response(5, "host.lan");

    Pkt nx = ok; /* NXDOMAIN */
    nx.b[3] = 0x83;
    CHECK(!dns_ptr_reply(nx.b, nx.n, NULL, NULL, name, sizeof name));

    Pkt query = ok; /* QR clear */
    query.b[2] = 0x01;
    CHECK(!dns_ptr_reply(query.b, query.n, NULL, NULL, name, sizeof name));

    /* DNS-SD service PTR (owner not in-addr.arpa) carries no host name for us */
    Pkt sd = {{0}, 0};
    p16(&sd, 0), p16(&sd, 0x8400), p16(&sd, 0), p16(&sd, 1), p16(&sd, 0), p16(&sd, 0);
    pname(&sd, "_http._tcp.local");
    p16(&sd, 12), p16(&sd, 1), p32(&sd, 10);
    size_t at = sd.n;
    p16(&sd, 0);
    size_t start = sd.n;
    pname(&sd, "Printer._http._tcp.local");
    sd.b[at + 1] = (uint8_t)(sd.n - start);
    CHECK(!dns_ptr_reply(sd.b, sd.n, NULL, NULL, name, sizeof name));

    /* compression pointer to itself (loop) */
    Pkt loop = {{0}, 0};
    p16(&loop, 1), p16(&loop, 0x8180), p16(&loop, 0), p16(&loop, 1), p16(&loop, 0), p16(&loop, 0);
    p16(&loop, 0xC00C); /* owner name at 12 points at 12 */
    p16(&loop, 12), p16(&loop, 1), p32(&loop, 1), p16(&loop, 2), p16(&loop, 0xC00C);
    CHECK(!dns_ptr_reply(loop.b, loop.n, NULL, NULL, name, sizeof name));

    /* pointer past the end */
    Pkt wild = loop;
    wild.b[12] = 0xC1;
    wild.b[13] = 0xFF;
    CHECK(!dns_ptr_reply(wild.b, wild.n, NULL, NULL, name, sizeof name));

    /* rdlength larger than the packet */
    Pkt longrd = ok;
    longrd.b[longrd.n - (strlen("host.lan") + 2) - 2] = 0x7F;
    CHECK(!dns_ptr_reply(longrd.b, longrd.n, NULL, NULL, name, sizeof name));

    /* every truncation of a valid packet is rejected */
    for (size_t k = 0; k < ok.n; k++) CHECK(!dns_ptr_reply(ok.b, k, NULL, NULL, name, sizeof name));
    CHECK(dns_ptr_reply(ok.b, ok.n, NULL, NULL, name, sizeof name));
    CHECK(!dns_ptr_reply(NULL, 100, NULL, NULL, name, sizeof name));
    CHECK(!dns_ptr_reply(ok.b, ok.n, NULL, NULL, name, 0));
}

TEST(dns_ptr_reply_sanitises_control_chars) {
    Pkt p = ptr_response(1, "evil\x07name\x1b.lan");
    char name[64];
    CHECK(dns_ptr_reply(p.b, p.n, NULL, NULL, name, sizeof name));
    CHECK_STR(name, "evil?name?");
}

TEST(dns_ptr_reply_survives_fuzzing) {
    Pkt base = ptr_response(0x4242, "some-device.lan");
    uint32_t x = 0x9E3779B9u;
    for (int iter = 0; iter < 20000; iter++) {
        Pkt p = base;
        int flips = 1 + iter % 6;
        for (int k = 0; k < flips; k++) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            p.b[x % p.n] = (uint8_t)(x >> 8);
        }
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        size_t len = (iter & 1) ? p.n : x % (p.n + 1);
        char name[32];
        memset(name, 'Z', sizeof name);
        if (dns_ptr_reply(p.b, len, NULL, NULL, name, sizeof name)) CHECK(memchr(name, 0, sizeof name) && name[0]);
    }
}

/* -------------------------------------------- OS smoke (no packets sent) */

TEST(lan_iface_and_neighbors_smoke) {
    LanIface ifc;
    if (lan_iface(&ifc)) {
        CHECK(ifc.ip != 0);
        CHECK(ifc.broadcast == (ifc.ip | ~ifc.mask));
        CHECK(memchr(ifc.name, 0, sizeof ifc.name) != NULL);
        CHECK((ifc.gateway >> 28) != 0xE);
    }
    Neighbor nb[512];
    int n = lan_neighbors(nb, countof(nb));
    CHECK(n >= 0 && n <= countof(nb));
    for (int i = 0; i < n; i++) {
        CHECK(nb[i].ip != 0 && (nb[i].ip >> 28) != 0xE);
        if (nb[i].state != NEIGH_FAILED) CHECK(!mac_is_zero(&nb[i].mac) && !mac_is_multicast(&nb[i].mac));
    }
    CHECK_STR(neigh_state_name(NEIGH_STALE), "stale");
    CHECK_STR(neigh_state_name((NeighState)42), "unknown");
}
