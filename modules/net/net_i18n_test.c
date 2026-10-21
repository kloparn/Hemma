/* The Network page in Swedish and Spanish (issue #46): the hub (Wi-Fi, a cable, an interface by name),
 * discovery off and on, devices of every kind, the filters, a device's dialog, "Your devices", the home
 * card, the System page's checks, the Latest feed and toasts - with nothing left in English but names. */
#include "../../tests/harness.h"
#include "net.h"

/* What other parts of Hemma show around these pages - the shell, a dialog's Close, the colour picker, the
   player, Watch's tabs and search, other modules' names and rows - is theirs to translate: allowed here
   only while this build has no translation for that part (the gate: a word of its own), so this test
   reads this part's words now, and every word once all parts are translated. */
static bool untranslated(const char *en) { return !strcmp(T_in(LANG_SV, en), en); }

static int allow_others(const char **out, int n, int max) {
    static const struct {
        const char *gate, *text;
    } fixed[] = {
        {"Skip to content", "Skip to content"}, {"Skip to content", "More"}, {"Skip to content", "Here now"},
        {"Skip to content", "Close"},
        {"Amber", "Colour"}, {"Amber", "Red"}, {"Amber", "Orange"}, {"Amber", "Amber"}, {"Amber", "Lime"},
        {"Amber", "Green"}, {"Amber", "Teal"}, {"Amber", "Sky"}, {"Amber", "Blue"}, {"Amber", "Indigo"},
        {"Amber", "Purple"}, {"Amber", "Magenta"}, {"Amber", "Pink"},
        {"Volume", "Volume"}, {"Volume", "Live"}, {"Volume", "Sound off"}, {"Volume", "Sound on"},
        {"Volume", "Full screen"}, {"Volume", "Full screen (F)"}, {"Volume", "Mute (M)"}, {"Volume", "Sound (M)"},
        {"Volume", "Play"}, {"Volume", "Play (Space)"}, {"Volume", "Pause (Space)"},
        {"Volume", "Back 10 seconds (Left)"}, {"Volume", "Forward 10 seconds (Right)"}, {"Volume", "Subtitles"},
        {"Library settings", "Library settings"}, {"Library settings", "Search the library"},
        {"Library settings", "Search movies and series…"}, {"Library settings", "Movies"},
        {"Library settings", "Series"}, {"Library settings", "Folders"}, {"Library settings", "Live TV"},
        {"Library settings", "Back"}, /* (TV mode's Back: media_tv_open) */
    };
    for (int i = 0; i < countof(fixed) && n < max; i++)
        if (untranslated(fixed[i].gate)) out[n++] = fixed[i].text;
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (str_eq(m->id, "screens") || str_eq(m->id, "tv") || str_eq(m->id, "net")) continue;
        if (n < max && untranslated(m->title)) out[n++] = m->title;
        for (const Block *bl = m->blocks; bl && bl->id && n < max; bl++)
            if (untranslated(bl->title)) out[n++] = bl->title;
    }
    return n;
}

static int with_role(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* An action's toast (in the actor's language) */
static const char *toast(int person, const char *action, const char *form) {
    static ThResult r;
    r = th_act(person, action, form);
    return r.toast;
}

static void iface(const char *name, int link) {
    LanIface f = {0};
    str_copy(f.name, sizeof f.name, name);
    ip_parse("192.168.1.2", &f.ip);
    ip_parse("255.255.255.0", &f.mask);
    ip_parse("192.168.1.1", &f.gateway);
    ip_parse("192.168.1.255", &f.broadcast);
    f.link = link;
    net_set_iface(&f);
}

static Neighbor nb(const char *ip, const char *mac) {
    Neighbor n = {0};
    ip_parse(ip, &n.ip);
    mac_parse(mac, &n.mac);
    n.state = NEIGH_REACHABLE;
    return n;
}

static int dev(const char *mac) { return net_by_mac(mac) ? net_by_mac(mac)->id : 0; }

/* The System page's checks from net, in a language: how many read the same as in English */
static int health_same(int person, const char *lang, Buf *out) {
    const Module *m = module_find("net");
    Health en = {0}, other = {0};
    Ctx c;
    pref_set(person, PREF_LANG, "en");
    app_ctx(&c, person, "/system");
    m->health(&c, &en);
    app_ctx_free(&c);
    pref_set(person, PREF_LANG, lang);
    app_ctx(&c, person, "/system");
    m->health(&c, &other);
    app_ctx_free(&c);
    pref_set(person, PREF_LANG, "");
    int same = en.n != other.n;
    for (int i = 0; i < en.n && i < other.n; i++) {
        const HealthItem *a = &en.items[i], *b = &other.items[i];
        const char *pairs[][2] = {{a->title, b->title}, {a->detail, b->detail}, {a->todo, b->todo}};
        for (int k = 0; k < 3; k++)
            if (pairs[k][0][0] && !strcmp(pairs[k][0], pairs[k][1])) {
                buf_printf(out, "health (%s): \"%s\"\n", lang, pairs[k][1]);
                same++;
            }
    }
    health_free(&en);
    health_free(&other);
    return same;
}

TEST(net_speaks_swedish_and_spanish) {
    th_boot();
    iface("wlan0", LAN_LINK_WIFI);
    int64_t t0 = dt_make(2026, 10, 10, 18, 0);
    int anna = with_role("Anna", ROLE_ADMIN), jonas = th_person("Jonas"), maja = th_person("Maja");
    int hall = with_role("Hall tablet", ROLE_DISPLAY);
    const char *allow[80] = {"Anna", "Jonas", "Maja", "Hall tablet", "Jonas phone", "Desktop", "in the attic",
                             "work-laptop", "wlan0", "eth0", "A", "J", "M", "HT",
                             /* the same in Swedish (the glossary): "online", the router, a product's name */
                             "online", "Online", "Router", "Raspberry Pi"};
    int na = 0;
    while (allow[na]) na++;
    allow_others(allow, na, countof(allow) - 1);
    Buf r = {0};
    int bad = 0;

    /* nothing found yet, discovery off and on */
    g_fake_now = t0 - 8 * 86400;
    bad += th_untranslated(anna, "/net", allow, &r);
    CHECK(th_act(anna, "net.scan", "on=1").ok);
    bad += th_untranslated(anna, "/net", allow, &r);
    CHECK(th_act(anna, "net.scan", "on=0").ok);

    /* a device gone for a week (Forget unknown devices ...), then today's: Jonas's phone, a laptop with a
       network name, a phone with a private address, the router, a computer that wakes over the network */
    Neighbor old[] = {nb("192.168.1.60", "a4:83:e7:00:00:60")};
    net_ingest(old, 1, g_fake_now, false);
    g_fake_now = t0;
    Neighbor now[] = {nb("192.168.1.10", "a4:83:e7:11:22:33"), nb("192.168.1.11", "a4:83:e7:11:22:44"),
                      nb("192.168.1.12", "02:11:22:33:44:55"), nb("192.168.1.1", "a4:83:e7:00:00:01"),
                      nb("192.168.1.13", "a4:83:e7:11:22:55")};
    net_ingest(now, countof(now), t0, false);
    char form[160];
    snprintf(form, sizeof form, "id=%d&name=Jonas+phone&owner=%d&kind=%%F0%%9F%%93%%B1", dev("a4:83:e7:11:22:33"), jonas);
    CHECK(th_act(anna, "net.save", form).ok);
    net_name_ip(0xC0A8010B, "work-laptop");
    snprintf(form, sizeof form, "id=%d&name=Desktop&wol=1&note=in+the+attic", dev("a4:83:e7:11:22:55"));
    CHECK(th_act(anna, "net.save", form).ok);
    net_learn(maja, 0xC0A8010C, t0); /* Maja opened Hemma from the private one: "Maja's?" */

    char u[8][48];
    const char *macs[] = {"a4:83:e7:11:22:33", "a4:83:e7:11:22:44", "02:11:22:33:44:55", "a4:83:e7:00:00:01",
                          "a4:83:e7:11:22:55", "a4:83:e7:00:00:60"};
    for (int i = 0; i < countof(macs); i++) snprintf(u[i], sizeof u[i], "/net?edit=%d", dev(macs[i]));
    const char *off_urls[] = {"/net", "/net?f=online", "/net?f=people", "/net?f=unknown", u[0], u[1], u[2], u[3], u[4], u[5]};
    for (int i = 0; i < countof(off_urls); i++) bad += th_untranslated(anna, off_urls[i], allow, &r);
    bad += th_untranslated(jonas, "/net", allow, &r);
    bad += th_untranslated(hall, "/net", allow, &r);

    /* discovery on: who's home; a cable, then an interface Hemma names by its name */
    CHECK(th_act(anna, "net.scan", "on=1").ok);
    net_ingest(now, countof(now), t0 + 10, true);
    bad += th_untranslated(anna, "/net", allow, &r);
    bad += th_untranslated(jonas, "/net", allow, &r);
    CHECK(th_act(jonas, "people.privacy", "home=0").ok);
    bad += th_untranslated(jonas, "/net", allow, &r);
    iface("eth0", LAN_LINK_CABLE);
    bad += th_untranslated(anna, "/net", allow, &r);
    iface("eth0", 0);
    bad += th_untranslated(anna, "/net", allow, &r);
    bad += health_same(anna, "sv", &r) + health_same(anna, "es", &r);
    iface("wlan0", LAN_LINK_WIFI);
    bad += health_same(anna, "sv", &r) + health_same(anna, "es", &r);
    CHECK(th_act(anna, "net.scan", "on=0").ok);
    bad += health_same(anna, "sv", &r) + health_same(anna, "es", &r);
    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);

    /* the home card */
    CHECK(th_act(anna, "net.scan", "on=1").ok);
    CHECK(th_act(jonas, "people.privacy", "home=1").ok);
    net_ingest(now, countof(now), t0 + 20, true);
    pref_set(anna, PREF_LANG, "sv");
    Ctx c;
    Buf b = {0};
    app_ctx(&c, anna, "/");
    module_find("net")->widget(&c, &b, WIDGET_HOME);
    app_ctx_free(&c);
    CHECK_HAS(buf_cstr(&b), "<h2>Nätverk</h2>");
    CHECK_HAS(buf_cstr(&b), "<span>enheter online</span>");
    CHECK_HAS(buf_cstr(&b), "Hemma nu");

    /* someone leaving: what the remove dialog says, in the admin's language */
    Forget fg = {.person = jonas, .keep = true, .apply = false, .now = t0};
    app_ctx(&c, anna, "/people");
    module_find("net")->forget(&fg);
    app_ctx_free(&c);
    CHECK_INT(fg.nnotes, 1);
    CHECK_STR(fg.notes[0], "1 av deras enheter på nätverket blir ingens");

    /* toasts in the actor's language */
    snprintf(form, sizeof form, "id=%d", dev("a4:83:e7:11:22:55"));
    CHECK_STR(toast(anna, "net.wake", form), "Väckning skickad till Desktop — ge den en minut");
    snprintf(form, sizeof form, "id=%d", dev("a4:83:e7:00:00:01"));
    pref_set(anna, PREF_LANG, "es");
    CHECK_STR(toast(anna, "net.forget", form), "Se olvidó Router");
    pref_set(anna, PREF_LANG, "");
    buf_free(&b);

    /* new devices in the Latest feed: one line, counted on in place - in each reader's language */
    Neighbor two[] = {nb("192.168.1.70", "a4:83:e7:00:00:70"), nb("192.168.1.71", "a4:83:e7:00:00:71")};
    net_ingest(two, 1, t0 + 30, true);
    Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "New device on the network: an unknown device (192.168.1.70)");
    net_ingest(two, 2, t0 + 40, true);
    a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "2 new devices on the network");
    Buf line = {0};
    Lang was = i18n_use(LANG_SV);
    activity_text(a, &line);
    i18n_use(was);
    CHECK_STR(buf_cstr(&line), "2 nya enheter på nätverket");
    buf_free(&line);
    g_fake_now = 0;
    th_halt();
}
