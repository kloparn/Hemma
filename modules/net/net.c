/* modules/net — the home network: which devices are around, whose they are,
 * and from that, who's home. Plus Wake-on-LAN for computers that support it.
 *
 * Observation is passive by default: every 30 s the OS neighbour (ARP) cache
 * is read, which sends nothing. Discovery (setting "net.scan", opt-in) also
 * sweeps the subnet every two minutes with 1-byte UDP probes, so phones that
 * sit quietly on the Wi-Fi show up, and asks the router and mDNS for names.
 * Presence ("who's home") is only reported while discovery is on: without
 * probes the ARP cache keeps listing devices that already left. */
#include "net.h"
#include "../../core/db.h"
#include "../../core/ui.h"

static const FieldDef device_fields[] = {
    FIELD(Device, id, F_INT),           FIELD(Device, mac, F_STR),       FIELD(Device, name, F_STR),
    FIELD(Device, owner, F_INT),        FIELD(Device, kind, F_STR),      FIELD(Device, first_seen, F_I64),
    FIELD(Device, last_seen, F_I64),    FIELD(Device, last_ip, F_STR),   FIELD(Device, hostname, F_STR),
    FIELD(Device, random_mac, F_BOOL),  FIELD(Device, wol, F_BOOL),      FIELD(Device, note, F_STR),
    FIELD(Device, used_by, F_INT),
};
Table T_device = TABLE("device", Device, device_fields);

#define OBSERVE_EVERY_MS 30000
#define PROBES_PER_TICK 24
#define SETTLE_MS 3000 /* after a sweep, give ARP replies time to land */
#define FLUSH_EVERY_MS (10 * 60 * 1000)
#define MAX_TRACKED 64
#define PAGE_ROWS 100 /* devices per step on /net: a list flooded by forged ARP stays a light page */
#define NEWS_EVERY 3600 /* "N new devices": one activity line and one toast an hour at most */
#define GONE_RING 512   /* recently forgotten MACs: coming back isn't news */
#define PTR_WINDOW 60   /* s: answers to a name question count this long */

enum { PTR_DNS = 1, PTR_MDNS = 2 }; /* Device.ptr_open */

typedef struct {
    int person;
    bool home;
    int64_t since;
} Pres;

static struct {
    LanIface ifc;
    bool have_ifc;
    int64_t ifc_at;       /* ms */
    int64_t next_observe; /* ms */
    sock_t udp;           /* probes + name lookups; open only while discovery is on */
    int64_t udp_retry;    /* ms */
    bool sweeping;
    uint32_t sweep_next, sweep_last;
    int64_t observe_at; /* ms: read the neighbour cache this long after a sweep */
    int64_t next_sweep; /* ms */
    int64_t last_sweep; /* wall clock: when the last sweep round completed */
    bool baseline;      /* one probed round done since discovery (re)started */
    int router;         /* id of the device the router's address maps to now (0 = none) */
    int evicted;        /* devices forgotten to make room since the last log line */
    int64_t evict_log;  /* wall clock: that log line */
    bool full_logged;   /* "the list is full" was logged; again only after there was room */
    uint64_t gone[GONE_RING]; /* MACs (mac_key) forgotten to make room, oldest overwritten */
    int gone_at;
    int news_id;        /* the activity line counting new devices, while it's under an hour old */
    int news_count;
    int64_t news_at, toast_at; /* wall clock: that line started; the last "new devices" toast */
    int64_t asked_at;   /* wall clock: the last name question (answers count for a minute) */
    int64_t last_flush; /* ms */
    Pres pres[MAX_TRACKED];
    int npres;
} S = {.udp = SOCK_BAD};

/* ------------------------------------------------------------- kinds */

static const char *const KINDS[] = {"📱", "💻", "🖥️", "📺", "🔊", "🎮", "🖨️", "📷", "💡", "🌐", "💾", "🍓", "⌚", "🚗"};
static const char *const KIND_NAMES[] = {"Phone or tablet", "Laptop",     "Computer", "TV",           "Speaker",
                                         "Game console",    "Printer",    "Camera",   "Smart home",   "Router",
                                         "Storage",         "Raspberry Pi", "Watch",  "Car"};

static const struct {
    const char *needle, *kind;
} GUESS[] = {
    {"iphone", "📱"},     {"ipad", "📱"},      {"android", "📱"},     {"pixel", "📱"},     {"galaxy", "📱"},
    {"oneplus", "📱"},    {"xiaomi", "📱"},    {"phone", "📱"},       {"macbook", "💻"},   {"laptop", "💻"},
    {"thinkpad", "💻"},   {"notebook", "💻"},  {"imac", "🖥️"},        {"desktop", "🖥️"},   {"-pc", "🖥️"},
    {"apple-tv", "📺"},   {"appletv", "📺"},   {"chromecast", "📺"},  {"bravia", "📺"},    {"roku", "📺"},
    {"shield", "📺"},     {"firetv", "📺"},    {"webos", "📺"},       {"tizen", "📺"},     {"tv", "📺"},
    {"sonos", "🔊"},      {"homepod", "🔊"},   {"echo", "🔊"},        {"speaker", "🔊"},   {"nest-audio", "🔊"},
    {"playstation", "🎮"}, {"ps5", "🎮"},      {"ps4", "🎮"},         {"xbox", "🎮"},      {"nintendo", "🎮"},
    {"steamdeck", "🎮"},  {"printer", "🖨️"},   {"epson", "🖨️"},       {"brother", "🖨️"},   {"canon", "🖨️"},
    {"officejet", "🖨️"},  {"laserjet", "🖨️"},  {"camera", "📷"},      {"cam", "📷"},       {"philips-hue", "💡"},
    {"hue", "💡"},        {"tradfri", "💡"},   {"shelly", "💡"},      {"tasmota", "💡"},   {"esp32", "💡"},
    {"esp8266", "💡"},    {"plug", "💡"},      {"router", "🌐"},      {"gateway", "🌐"},   {"fritz", "🌐"},
    {"unifi", "🌐"},      {"deco", "🌐"},      {"eero", "🌐"},        {"synology", "💾"},  {"diskstation", "💾"},
    {"qnap", "💾"},       {"nas", "💾"},       {"raspberry", "🍓"},   {"pihole", "🍓"},    {"watch", "⌚"},
    {"tesla", "🚗"},
};

static bool is_letter(char ch) { return ch >= 'a' && ch <= 'z'; }

/* Long needles match anywhere; short ones ("tv", "cam", "nas") only as a whole
 * word, so "camilla" or "jonas" don't become a camera or a NAS. */
static bool needle_in(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) {
        if (nl > 3) return true;
        bool left = !is_letter(needle[0]) || p == hay || !is_letter(p[-1]);
        bool right = !is_letter(needle[nl - 1]) || !is_letter(p[nl]);
        if (left && right) return true;
    }
    return false;
}

static bool known_kind(const char *k) {
    for (int i = 0; i < countof(KINDS); i++)
        if (str_eq(KINDS[i], k)) return true;
    return false;
}

/* The router is the ONE device its address maps to now (find_router), not every device
   that ever claimed that address: anyone can claim it (forged ARP), and the router is
   never forgotten to make room. */
static bool is_gateway(const Device *d) { return S.router && d->id == S.router; }

const char *net_kind(const Device *d) {
    if (d->kind[0]) return d->kind;
    if (is_gateway(d)) return "🌐";
    char hay[128];
    fold_copy(hay, sizeof hay, d->hostname[0] ? d->hostname : d->name);
    for (int i = 0; hay[0] && i < countof(GUESS); i++)
        if (needle_in(hay, GUESS[i].needle)) return GUESS[i].kind;
    if (d->random_mac) return "📱"; /* private addresses are nearly always phones and tablets */
    return NULL;
}

const char *net_label(const Device *d) {
    if (d->name[0]) return d->name;
    if (d->hostname[0]) return d->hostname;
    if (is_gateway(d)) return "Router";
    return "Unknown device";
}

/* ------------------------------------------------------------- lookups */

bool net_online(const Device *d, int64_t now) { return d->last_seen > 0 && now - d->last_seen <= NET_ONLINE_WINDOW; }

bool net_scan_on(void) { return setting_int("net.scan", 0) == 1; }

Device *net_by_mac(const char *mac) {
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        if (str_eq(d->mac, mac)) return d;
    }
    return NULL;
}

/* The device most recently seen at this address. */
static Device *by_ip(const char *ip) {
    Device *best = NULL;
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        if (str_eq(d->last_ip, ip) && (!best || d->last_seen > best->last_seen)) best = d;
    }
    return best;
}

static bool in_scope(uint32_t ip) {
    if (S.have_ifc && S.ifc.ip) return lan_in_subnet(&S.ifc, ip) && ip != S.ifc.ip && ip != S.ifc.broadcast;
    return lan_is_private(ip);
}

/* Only a device on the network right now counts: an address used by Eva's phone
   yesterday may belong to a guest today. */
int net_guess(uint32_t ip) {
    char s[16];
    ip_format(ip, s);
    Device *d = by_ip(s);
    if (!d || !net_online(d, now_sec())) return 0;
    if (d->owner && person_get(d->owner)) return d->owner;
    if (d->used_by && person_get(d->used_by)) return d->used_by;
    return 0;
}

/* After every change to the list or the interface: which device is the router now. */
static void find_router(void) {
    char s[16];
    Device *d = NULL;
    if (S.have_ifc && S.ifc.gateway) {
        ip_format(S.ifc.gateway, s);
        d = by_ip(s);
    }
    S.router = d ? d->id : 0;
}

void net_set_iface(const LanIface *ifc) {
    S.ifc = *ifc;
    S.have_ifc = true;
    find_router();
}

/* ------------------------------------------------------------ presence */

static int window_s(void) { return (int)CLAMP(setting_int("net.window", 10), 1, 240) * 60; }

/* Stop claiming home/away for `person` (0 = everyone) — discovery went off, or they no
   longer own a device — so they show as unknown instead of frozen at the last state. */
static void forget_presence(int person) {
    int w = 0;
    for (int k = 0; k < S.npres; k++)
        if (person && S.pres[k].person != person) S.pres[w++] = S.pres[k];
    S.npres = w;
    presence_forget(person);
}

static void update_presence(int64_t now) {
    int window = window_s();
    for (int i = 0; i < person_count(); i++) {
        int pid = person_at(i)->id;
        bool has = false, home = false;
        for (int k = 0; k < tbl_count(&T_device); k++) {
            Device *d = tbl_at(&T_device, k);
            if (d->owner != pid) continue;
            has = true;
            if (d->last_seen && now - d->last_seen <= window) home = true;
        }
        if (!has) { /* nothing to go on: don't claim "away" — nor keep an old "home" */
            forget_presence(pid);
            continue;
        }
        Pres *p = NULL;
        for (int k = 0; k < S.npres; k++)
            if (S.pres[k].person == pid) p = &S.pres[k];
        if (!p) {
            if (S.npres == MAX_TRACKED) continue;
            p = &S.pres[S.npres++];
            *p = (Pres){pid, home, now};
        } else if (p->home != home) {
            p->home = home;
            p->since = now;
        }
        presence_set_home(pid, home, p->since);
    }
    for (int k = S.npres - 1; k >= 0; k--) /* people who were removed from the household */
        if (!person_get(S.pres[k].person)) forget_presence(S.pres[k].person);
}

/* ------------------------------------------------------------- ingest */

/* Any device on the LAN can make the kernel list made-up neighbours (forged ARP, no sign-in
   needed): ~250 new MACs per round on a /24. So the list is bounded (NET_MAX_DEVICES), new
   devices make one activity line an hour, and a round's lookups don't scan the list per
   neighbour. */

/* Never forgotten to make room: what the household set up (a name, an owner, a kind, a note,
   Wake-on-LAN) and the router. Nothing a device on the LAN can claim by itself counts - not
   an address (anyone can claim the router's) and not used_by (set from an address). */
static bool is_kept(const Device *d) {
    return d->name[0] || d->owner || d->kind[0] || d->note[0] || d->wol || is_gateway(d);
}

typedef struct {
    bool used; /* someone opens Hemma from it: goes last */
    int64_t seen;
    int id;
} Spare;

static int by_seen(const void *a, const void *b) {
    const Spare *x = a, *y = b;
    if (x->used != y->used) return x->used ? 1 : -1;
    if (x->seen != y->seen) return x->seen < y->seen ? -1 : 1;
    return x->id - y->id;
}

static int by_id_desc(const void *a, const void *b) { return ((const Spare *)b)->id - ((const Spare *)a)->id; }

/* The devices that may go when the list is full: not kept, not on the network right now and
   not in this round's list (even as stale) - the longest unseen first, the ones someone uses
   Hemma from last. Returns how many; free(*out). */
static int spares(int64_t now, Spare **out) {
    int n = 0;
    Spare *s = xmalloc(sizeof(Spare) * (size_t)(tbl_count(&T_device) + 1));
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        if (!is_kept(d) && !net_online(d, now) && d->listed != now)
            s[n++] = (Spare){d->used_by != 0, d->last_seen, d->id};
    }
    qsort(s, (size_t)n, sizeof *s, by_seen);
    *out = s;
    return n;
}

/* One round's MAC -> device id lookup: sorted once, kept in step as devices come and go. */
typedef struct {
    uint64_t mac;
    int id;
} MacKey;

typedef struct {
    MacKey *k;
    int n;
} MacIndex;

static uint64_t mac_key(const Mac *m) {
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v = v << 8 | m->b[i];
    return v;
}

static int by_mac(const void *a, const void *b) {
    uint64_t x = ((const MacKey *)a)->mac, y = ((const MacKey *)b)->mac;
    return x < y ? -1 : x > y;
}

static int by_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int key_at(const MacIndex *ix, uint64_t mac) { /* the first slot >= mac */
    int lo = 0, hi = ix->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ix->k[mid].mac < mac) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* Room for `extra` devices added later in the round. */
static MacIndex index_build(int extra) {
    MacIndex ix = {xmalloc(sizeof(MacKey) * (size_t)(tbl_count(&T_device) + extra + 1)), 0};
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        Mac m;
        if (mac_parse(d->mac, &m)) ix.k[ix.n++] = (MacKey){mac_key(&m), d->id};
    }
    qsort(ix.k, (size_t)ix.n, sizeof *ix.k, by_mac);
    return ix;
}

static Device *index_get(const MacIndex *ix, uint64_t mac) {
    int i = key_at(ix, mac);
    return i < ix->n && ix->k[i].mac == mac ? tbl_get(&T_device, ix->k[i].id) : NULL;
}

static void index_add(MacIndex *ix, uint64_t mac, int id) {
    int i = key_at(ix, mac);
    memmove(ix->k + i + 1, ix->k + i, sizeof *ix->k * (size_t)(ix->n - i));
    ix->k[i] = (MacKey){mac, id};
    ix->n++;
}

static void index_drop(MacIndex *ix, uint64_t mac) {
    int i = key_at(ix, mac);
    if (i == ix->n || ix->k[i].mac != mac) return;
    memmove(ix->k + i, ix->k + i + 1, sizeof *ix->k * (size_t)(ix->n - i - 1));
    ix->n--;
}

/* Was this MAC forgotten to make room lately? Then coming back isn't news. (Key 0, the
   ring's empty slots, is the zero MAC: never ingested.) */
static bool gone_lately(uint64_t mac) {
    for (int i = 0; i < GONE_RING; i++)
        if (S.gone[i] == mac) return true;
    return false;
}

/* Forget up to `count` spare devices to make room (ix: the round's index, or NULL). Highest
   ids go first: each delete then moves only the rows that stay. Returns how many went. */
static int evict(MacIndex *ix, int count, int64_t now) {
    Spare *s;
    int n = MIN(spares(now, &s), count);
    qsort(s, (size_t)n, sizeof *s, by_id_desc);
    for (int i = 0; i < n; i++) {
        Device *old = tbl_get(&T_device, s[i].id);
        Mac m;
        if (!old) continue;
        if (mac_parse(old->mac, &m)) {
            S.gone[S.gone_at] = mac_key(&m);
            S.gone_at = (S.gone_at + 1) % GONE_RING;
            if (ix) index_drop(ix, mac_key(&m));
        }
        tbl_del(&T_device, old->id);
    }
    free(s);
    return n;
}

/* A list that grew past the limit before there was one shrinks at start. */
static void trim(int64_t now) {
    int over = tbl_count(&T_device) - NET_MAX_DEVICES;
    if (over <= 0) return;
    db_begin();
    int n = evict(NULL, over, now);
    db_commit();
    if (n) log_info("net: forgot %d unnamed devices not seen for a while (the list keeps %d)", n, NET_MAX_DEVICES);
}

/* A device we know is in this round's list. True when that's worth a write and a re-render. */
static bool refresh(Device *d, const Neighbor *e, int64_t now) {
    char ip[16];
    ip_format(e->ip, ip);
    Device upd = *d;
    bool durable = false;
    upd.listed = now;
    if (!str_eq(upd.last_ip, ip)) {
        str_copy(upd.last_ip, sizeof upd.last_ip, ip);
        durable = true;
    }
    if (e->state == NEIGH_REACHABLE && upd.last_seen != now) { /* stale entries: known, not confirmed now */
        upd.last_seen = now;
        upd.dirty = true;
    }
    if (durable) {
        upd.dirty = false;
        tbl_put(&T_device, &upd);
    } else {
        tbl_put_quiet(&T_device, &upd);
    }
    return durable;
}

static Device *add_device(const Neighbor *e, int64_t now) {
    Device nd = {0};
    mac_format(&e->mac, nd.mac);
    ip_format(e->ip, nd.last_ip);
    nd.first_seen = now;
    nd.last_seen = now;
    nd.listed = now;
    nd.random_mac = mac_is_random(&e->mac);
    nd.online = true;
    return tbl_put(&T_device, &nd);
}

/* New devices make one activity line an hour: later rounds add their count to it while it's
   under an hour old, and screens get a toast at most once an hour. Forged ARP could otherwise
   fill the feed and pester every screen - one fake device a sweep is enough. */
static void announce(int count, const Device *d, int64_t now) {
    char what[160];
    Activity *line = S.news_id && now - S.news_at < NEWS_EVERY ? tbl_get(&T_activity, S.news_id) : NULL;
    if (line) {
        Activity upd = *line;
        S.news_count += count;
        snprintf(upd.text, sizeof upd.text, "%d new devices on the network", S.news_count);
        tbl_put(&T_activity, &upd);
        str_copy(what, sizeof what, upd.text);
    } else {
        if (count > 1 || !d) snprintf(what, sizeof what, "%d new devices on the network", count);
        else if (d->hostname[0])
            snprintf(what, sizeof what, "New device on the network: %s (%s)", d->hostname, d->last_ip);
        else if (d->random_mac)
            snprintf(what, sizeof what, "New device on the network: a phone or tablet with a private address (%s)",
                     d->last_ip);
        else snprintf(what, sizeof what, "New device on the network: an unknown device (%s)", d->last_ip);
        activity_add(0, "🆕", "/net?f=unknown", "%s", what);
        S.news_id = ((Activity *)tbl_at(&T_activity, tbl_count(&T_activity) - 1))->id;
        S.news_count = count;
        S.news_at = now;
    }
    if (now - S.toast_at >= NEWS_EVERY) {
        app_notify(NULL, TOAST_INFO, "%s", what);
        S.toast_at = now;
    }
}

/* Making room is normal on a busy network (phones that change their private address): one
   line an hour at most. A list full of devices that must stay is logged once, and again
   only after there was room. */
static void log_limits(int dropped, int64_t now) {
    if (S.evicted && now - S.evict_log >= 3600) {
        log_info("net: forgot %d unnamed device%s not seen for a while (the list keeps %d)", S.evicted,
                 S.evicted == 1 ? "" : "s", NET_MAX_DEVICES);
        S.evicted = 0;
        S.evict_log = now;
    }
    if (dropped && !S.full_logged)
        log_warn("net: the device list is full (%d devices, each set up by the household or on the network now): "
                 "new devices are left out until some are forgotten on the Network page",
                 NET_MAX_DEVICES);
    S.full_logged = (dropped || S.full_logged) && tbl_count(&T_device) >= NET_MAX_DEVICES;
}

void net_ingest(const Neighbor *nb, int n, int64_t now, bool probed) {
    bool changed = false;
    bool announce_new = probed && S.baseline && net_scan_on();
    db_begin(); /* the round is one write */
    MacIndex ix = index_build(n);
    int *fresh = xmalloc(sizeof(int) * (size_t)(n > 0 ? n : 1)), nfresh = 0;
    /* known devices first: whoever is in this round's list is safe before anyone makes room */
    for (int i = 0; i < n; i++) {
        const Neighbor *e = &nb[i];
        if (e->state == NEIGH_FAILED || mac_is_zero(&e->mac) || mac_is_multicast(&e->mac) || !in_scope(e->ip)) continue;
        Device *d = index_get(&ix, mac_key(&e->mac));
        if (d) changed |= refresh(d, e, now);
        else fresh[nfresh++] = i;
    }
    /* room for the newcomers (each MAC once), all made in one go */
    uint64_t *keys = xmalloc(sizeof(uint64_t) * (size_t)(nfresh + 1));
    for (int k = 0; k < nfresh; k++) keys[k] = mac_key(&nb[fresh[k]].mac);
    qsort(keys, (size_t)nfresh, sizeof *keys, by_u64);
    int want = 0;
    for (int k = 0; k < nfresh; k++) want += !k || keys[k] != keys[k - 1];
    free(keys);
    int room = NET_MAX_DEVICES - tbl_count(&T_device), gone = 0;
    if (want > room) gone = evict(&ix, want - room, now);
    S.evicted += gone;
    changed |= gone > 0;
    int news = 0, first = 0, dropped = 0;
    for (int k = 0; k < nfresh; k++) {
        const Neighbor *e = &nb[fresh[k]];
        uint64_t key = mac_key(&e->mac);
        Device *d = index_get(&ix, key);
        if (d) { /* listed twice in this round */
            changed |= refresh(d, e, now);
            continue;
        }
        if (tbl_count(&T_device) >= NET_MAX_DEVICES) {
            dropped++;
            continue;
        }
        Device *row = add_device(e, now);
        index_add(&ix, key, row->id);
        changed = true;
        if (gone_lately(key)) continue; /* forgotten to make room lately: not news */
        if (!news++) first = row->id;
    }
    free(fresh);
    free(ix.k);
    find_router();
    /* online <-> offline transitions are worth a write and a re-render */
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        bool on = net_online(d, now);
        if (on == d->online) continue;
        Device upd = *d;
        upd.online = on;
        upd.dirty = false;
        tbl_put(&T_device, &upd);
        changed = true;
    }
    if (net_scan_on()) update_presence(now);
    else if (S.npres) forget_presence(0); /* discovery was switched off */
    if (announce_new && news) announce(news, tbl_get(&T_device, first), now);
    log_limits(dropped, now);
    if (probed) S.baseline = true;
    db_commit();
    if (changed) app_changed();
}

void net_name_ip(uint32_t ip, const char *hostname) {
    char s[16];
    ip_format(ip, s);
    Device *d = by_ip(s);
    if (!d || !hostname || !hostname[0] || str_eq(d->hostname, hostname)) return;
    Device upd = *d;
    str_copy(upd.hostname, sizeof upd.hostname, hostname);
    tbl_put(&T_device, &upd);
    app_changed();
}

void net_learn(int person, uint32_t ip, int64_t now) {
    if (!person || !ip) return;
    char s[16];
    ip_format(ip, s);
    Device *d = by_ip(s);
    if (!d || !net_online(d, now) || d->used_by == person) return; /* stale address: maybe someone else's now */
    Device upd = *d;
    upd.used_by = person;
    tbl_put(&T_device, &upd);
    app_changed();
}

static void flush(void) {
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        if (!d->dirty) continue;
        Device upd = *d;
        upd.dirty = false;
        tbl_put(&T_device, &upd);
    }
}

/* ------------------------------------------------------- the event loop */

static void refresh_iface(void) {
    LanIface f;
    if (lan_iface(&f)) net_set_iface(&f);
}

/* Each question gets fresh random ids, one for the router and one for mDNS: the mDNS question
   is multicast, so every device on the LAN sees its id - it must say nothing about the
   router's. Responders answer a question from an ordinary port (not 5353) by unicast, with
   its id (RFC 6762 6.7), so both answers can be matched exactly. */
bool net_ptr_ask(uint32_t ip, int64_t now, uint16_t *dns_id, uint16_t *mdns_id) {
    char s[16];
    ip_format(ip, s);
    Device *d = by_ip(s);
    if (!d) return false;
    uint16_t id[2];
    do {
        rand_bytes(id, sizeof id);
    } while (id[0] == id[1]);
    d->dns_id = id[0];
    d->mdns_id = id[1];
    d->ptr_open = PTR_DNS | PTR_MDNS;
    d->ptr_asked = now;
    S.asked_at = now;
    *dns_id = id[0];
    *mdns_id = id[1];
    return true;
}

static bool from_router(uint32_t from, uint16_t port) {
    return port == 53 && S.have_ifc && S.ifc.gateway && from == S.ifc.gateway;
}

/* A reverse-DNS answer counts only for an address we asked about within the last minute,
   only from someone who may answer for it - the router (DNS, port 53) or the device itself
   (mDNS, port 5353) - only with the id of that very question, and only once. Anything else
   on the LAN could otherwise rename other people's devices (review, issue #10). The first
   answer from such a sender closes the question even with a wrong id: whoever sees the
   multicast question can't spray guesses at the router's id for the rest of the minute. */
bool net_ptr_answer(uint32_t from, uint16_t port, uint16_t id, uint32_t ip, const char *name, int64_t now) {
    bool dns = from_router(from, port);
    bool mdns = port == 5353 && from == ip;
    if (!(dns || mdns) || !in_scope(ip) || now - S.asked_at > PTR_WINDOW) return false;
    char s[16];
    ip_format(ip, s);
    Device *d = by_ip(s);
    if (!d || !d->ptr_asked || now - d->ptr_asked > PTR_WINDOW) return false;
    uint8_t which = dns ? PTR_DNS : PTR_MDNS;
    if (!(d->ptr_open & which)) return false;
    d->ptr_open &= (uint8_t)~which;
    if (id != (dns ? d->dns_id : d->mdns_id)) return false;
    net_name_ip(ip, name);
    return true;
}

/* A response with no name in it - "no such name" (most routers, for most devices), another
   error, or no PTR record - from someone who may answer: the question with that id is
   answered, so nothing that arrives after it counts. */
static void ptr_no_name(uint32_t from, uint16_t port, uint16_t id, int64_t now) {
    bool dns = from_router(from, port);
    if ((!dns && port != 5353) || now - S.asked_at > PTR_WINDOW) return;
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        uint32_t ip;
        if (!d->ptr_open || now - d->ptr_asked > PTR_WINDOW) continue;
        if (dns && (d->ptr_open & PTR_DNS) && d->dns_id == id) d->ptr_open &= (uint8_t)~PTR_DNS;
        else if (!dns && (d->ptr_open & PTR_MDNS) && d->mdns_id == id && ip_parse(d->last_ip, &ip) && ip == from)
            d->ptr_open &= (uint8_t)~PTR_MDNS;
    }
}

void net_dns_packet(uint32_t from, uint16_t port, const uint8_t *pkt, size_t n, int64_t now) {
    if ((port != 53 && port != 5353) || !pkt || n < 12 || !(pkt[2] & 0x80)) return; /* responses only */
    uint16_t id = 0;
    uint32_t ip = 0;
    char name[64];
    if (dns_ptr_reply(pkt, n, &id, &ip, name, sizeof name)) net_ptr_answer(from, port, id, ip, name, now);
    else ptr_no_name(from, port, (uint16_t)(pkt[0] << 8 | pkt[1]), now);
}

static void on_udp(sock_t fd, short revents, void *ctx) {
    uint8_t pkt[1500];
    for (int k = 0; k < 32; k++) {
        uint32_t from = 0;
        uint16_t port = 0;
        long n = udp_recv(fd, pkt, sizeof pkt, &from, &port);
        if (n <= 0) break;
        net_dns_packet(from, port, pkt, (size_t)n, now_sec());
    }
}

static void open_udp(int64_t now) {
    if (now < S.udp_retry) return;
    S.udp = lan_udp_open(false);
    if (S.udp == SOCK_BAD) {
        log_warn("net: cannot open a UDP socket for discovery");
        S.udp_retry = now + 60000;
        return;
    }
    watch_add(S.udp, POLLIN, on_udp, NULL);
    S.baseline = false;
    S.next_sweep = now;
}

static void close_udp(void) {
    if (S.udp == SOCK_BAD) return;
    watch_del(S.udp);
    sock_close(S.udp);
    S.udp = SOCK_BAD;
    S.sweeping = false;
    S.observe_at = 0;
    S.baseline = false;
}

static void observe(bool probed) {
    static Neighbor nb[1024];
    int n = lan_neighbors(nb, countof(nb));
    net_ingest(nb, n, now_sec(), probed);
}

static void ask_names(void) {
    int64_t now = now_sec();
    int sent = 0;
    for (int i = 0; i < tbl_count(&T_device) && sent < 32; i++) {
        Device *d = tbl_at(&T_device, i);
        uint32_t ip;
        uint16_t dns_id, mdns_id;
        if (d->hostname[0] || !net_online(d, now) || now - d->ptr_asked < 1800 || !ip_parse(d->last_ip, &ip)) continue;
        d->ptr_asked = now; /* memory-only fields: no write needed */
        if (!net_ptr_ask(ip, now, &dns_id, &mdns_id)) continue;
        uint8_t q[64];
        size_t n;
        if (S.ifc.gateway && (n = dns_ptr_query(ip, dns_id, false, q, sizeof q)) > 0)
            udp_send(S.udp, S.ifc.gateway, 53, q, n);
        if ((n = dns_ptr_query(ip, mdns_id, true, q, sizeof q)) > 0) udp_send(S.udp, 0xE00000FB, 5353, q, n);
        sent++;
    }
}

static void tick(int64_t now) {
    if (app_testing()) return;
    if (!S.ifc_at || now - S.ifc_at > 60000) {
        S.ifc_at = now;
        refresh_iface();
    }
    bool scan = net_scan_on();
    if (scan && S.udp == SOCK_BAD) open_udp(now);
    else if (!scan && S.udp != SOCK_BAD) close_udp();
    if (scan && S.udp != SOCK_BAD) {
        if (!S.sweeping && !S.observe_at && now >= S.next_sweep) {
            uint32_t first, last;
            if (S.have_ifc && lan_sweep_range(&S.ifc, &first, &last, 254) > 0) {
                S.sweep_next = first;
                S.sweep_last = last;
                S.sweeping = true;
            } else {
                S.next_sweep = now + NET_SCAN_EVERY * 1000;
            }
        }
        if (S.sweeping) {
            for (int k = 0; k < PROBES_PER_TICK && S.sweep_next <= S.sweep_last; k++, S.sweep_next++)
                if (S.sweep_next != S.ifc.ip) lan_probe(S.udp, S.sweep_next);
            if (S.sweep_next > S.sweep_last) {
                S.sweeping = false;
                S.observe_at = now + SETTLE_MS;
            }
        }
        if (S.observe_at && now >= S.observe_at) {
            S.observe_at = 0;
            observe(true);
            S.last_sweep = now_sec();
            S.next_sweep = now + NET_SCAN_EVERY * 1000;
            ask_names();
        }
        /* The core ticks about once a second; a sweep paces PROBES_PER_TICK per tick at 10 Hz,
           and reads the neighbour cache right after it settles. */
        if (S.sweeping) app_wake_in(100);
        else if (S.observe_at) app_wake_in((int)CLAMP(S.observe_at - now, 0, 100));
    }
    if (now >= S.next_observe) {
        S.next_observe = now + OBSERVE_EVERY_MS;
        observe(false);
    }
    if (now - S.last_flush > FLUSH_EVERY_MS) {
        S.last_flush = now;
        flush();
    }
}

/* Every page load tells us which person uses which device (owner suggestions).
   A wall tablet's display account isn't anyone, so it never claims a device. */
static bool http_hook(Ctx *c, Conn *conn, HttpReq *r) {
    if (c->me && c->me->role != ROLE_DISPLAY) net_learn(c->me->id, c->ip, c->now);
    return false;
}

/* --------------------------------------------------------------- pages */

static int64_t g_sort_now;

/* Unnamed and unowned — except the router, which we recognise by its address. */
static bool is_unknown(const Device *d) { return !d->name[0] && !d->owner && !is_gateway(d); }
static bool is_new(const Device *d, int64_t now) { return is_unknown(d) && now - d->first_seen < 86400; }
/* What "Forget unknown devices not seen for a week" takes: whatever the household didn't set
   up and hasn't been around for a week - one click cleans a list that forged ARP flooded. */
static bool is_tidy(const Device *d, int64_t now) { return !is_kept(d) && now - d->last_seen >= NET_TIDY_AFTER; }

/* Online first; within that, owned, then named or the router, then the rest (where a flood
   of strangers lands), each by label. */
static int dev_order(const void *a, const void *b) {
    const Device *x = tbl_at(&T_device, *(const int *)a), *y = tbl_at(&T_device, *(const int *)b);
    bool xo = net_online(x, g_sort_now), yo = net_online(y, g_sort_now);
    if (xo != yo) return xo ? -1 : 1;
    if ((x->owner != 0) != (y->owner != 0)) return x->owner ? -1 : 1;
    if (is_unknown(x) != is_unknown(y)) return is_unknown(x) ? 1 : -1;
    char lx[64], ly[64];
    fold_copy(lx, sizeof lx, net_label(x));
    fold_copy(ly, sizeof ly, net_label(y));
    int cmp = strcmp(lx, ly);
    return cmp ? cmp : x->id - y->id;
}

static void filter_chip(Ctx *c, Buf *b, const char *value, const char *label, int count) {
    const char *cur = ctx_q(c, "f");
    bool on = str_eq(cur, value ? value : "");
    Buf u = {0};
    ctx_url(c, &u, "f", value, "edit", NULL, "n", NULL, NULL);
    H(b, "<a class=\"chip%r\" href=\"%s\">%s <span class=\"net-n\">%d</span></a>", on ? " on" : "", buf_cstr(&u), label,
      count);
    buf_free(&u);
}

static void hub_card(Buf *b) {
    ui_card(b, "This hub", NULL, NULL);
    if (!S.have_ifc) {
        buf_puts(b, "<p class=\"muted\">The network interface couldn't be read.</p>");
    } else {
        char ip[16], gw[16];
        ip_format(S.ifc.ip, ip);
        ip_format(S.ifc.gateway, gw);
        int prefix = 0;
        for (uint32_t m = S.ifc.mask; m; m <<= 1) prefix++;
        H(b, "<dl class=\"net-facts\"><dt>Interface</dt><dd>%s</dd><dt>Address</dt><dd>%s/%d</dd>", S.ifc.name, ip,
          prefix);
        if (S.ifc.gateway) H(b, "<dt>Router</dt><dd>%s</dd>", gw);
        buf_puts(b, "</dl>");
    }
    ui_card_end(b);
}

static void scan_card(Ctx *c, Buf *b) {
    bool on = net_scan_on();
    buf_puts(b, "<section class=\"card net-scan\"><div class=\"net-scan-h\"><div><h2>Discovery</h2>");
    H(b, "<p class=\"muted\">%s</p></div>",
      on ? "On — sweeping the network every two minutes." : "Off — only devices this hub talks to are listed.");
    if (is_admin(c)) {
        ui_form(b, "net.scan", "inline");
        ui_hidden(b, "on", on ? "0" : "1");
        H(b, "<button class=\"net-switch%r\" role=\"switch\" aria-checked=\"%s\" aria-label=\"Discovery\"></button>",
          on ? " on" : "", on ? "true" : "false");
        ui_form_end(b);
    } else {
        H(b, "<span class=\"badge%r\" title=\"An admin can change this\">%s</span>", on ? " ok" : "", on ? "On" : "Off");
    }
    buf_puts(b, "</div>");
    if (on) {
        buf_puts(b, "<p class=\"net-small muted\">");
        if (S.last_sweep) {
            buf_puts(b, "Last sweep ");
            ui_ago(b, S.last_sweep, c->now);
        } else {
            buf_puts(b, "First sweep starting…");
        }
        buf_puts(b, "</p>");
        int cur = window_s() / 60;
        static const int choices[] = {5, 10, 15, 30, 60};
        if (!is_admin(c)) {
            H(b, "<p class=\"net-small muted\">People count as home when their device was seen in the last %d minutes.</p>",
              cur);
        } else {
            ui_form(b, "net.window", "net-window");
            buf_puts(b, "<label>People count as home when their device was seen in the last <select name=\"min\" "
                        "data-auto aria-label=\"Minutes\">");
            for (int i = 0; i < countof(choices); i++)
                H(b, "<option value=\"%d\"%r>%d</option>", choices[i], choices[i] == cur ? " selected" : "",
                  choices[i]);
            buf_puts(b, "</select> minutes.</label><noscript><button class=\"btn sm\">Save</button></noscript>");
            ui_form_end(b);
        }
        /* ARP can't be authenticated: a forged neighbour entry with someone's phone address
           makes them "home" (issue #10). Say so where presence is set up. */
        buf_puts(b, "<p class=\"net-small muted\">It's a good guess, not proof: phones get left at home, and any device "
                    "on the network can pretend to be someone's phone.</p>");
    } else {
        buf_puts(b, "<p class=\"net-explain\">Turn it on at home to see every phone, TV and laptop on the Wi-Fi — and a "
                    "good guess at who's home. Hemma then sends a tiny packet to each address on your own network "
                    "every two minutes. Nothing leaves your network.</p>");
    }
    buf_puts(b, "</section>");
}

static void device_row(Ctx *c, Buf *b, const Device *d) {
    bool on = net_online(d, c->now), admin = is_admin(c); /* changing devices is the admin's */
    char idstr[16];
    snprintf(idstr, sizeof idstr, "%d", d->id);
    H(b, "<li class=\"net-dev%r\" id=\"net-dev-%d\"><span class=\"net-k\">", on ? " is-on" : "", d->id);
    const char *k = net_kind(d);
    if (k) html_esc(b, k);
    else ui_icon(b, "wifi");
    H(b, "<i></i></span><div class=\"net-main\"><div class=\"net-name\"><b>%s</b>", net_label(d));
    if (is_new(d, c->now)) buf_puts(b, "<span class=\"badge net-new\">new</span>");
    buf_puts(b, "</div><small>");
    if (on) buf_puts(b, "<span class=\"net-live\">online</span>");
    else if (d->last_seen) {
        buf_puts(b, "seen ");
        ui_ago(b, d->last_seen, c->now);
    } else buf_puts(b, "never seen");
    if (d->last_ip[0]) H(b, " · %s", d->last_ip);
    if (d->name[0] && d->hostname[0]) H(b, " · %s", d->hostname);
    if (d->random_mac) buf_puts(b, " · private address");
    buf_puts(b, "</small></div><div class=\"net-side\">");
    Person *o = person_get(d->owner);
    Person *u = person_get(d->used_by);
    if (o) {
        H(b, "<span class=\"net-owner\" title=\"%s's\">", o->name);
        ui_avatar(b, o, "sm");
        buf_puts(b, "</span>");
    } else if (u && admin) {
        ui_form(b, "net.assign", "inline");
        ui_hidden(b, "id", idstr);
        ui_hidden_int(b, "person", u->id);
        H(b, "<button class=\"chip net-suggest\" title=\"%s opened Hemma from this device\">", u->name);
        ui_avatar(b, u, "xs");
        H(b, "%s's?</button>", u->name);
        ui_form_end(b);
    }
    if (d->wol && admin) {
        Buf ic = {0};
        ui_icon(&ic, "power");
        ui_act(b, "net.wake", "icon-btn net-wake", buf_cstr(&ic), "id", idstr, "Wake up");
        buf_free(&ic);
    }
    if (admin) {
        Buf href = {0};
        ctx_url(c, &href, "edit", idstr, NULL);
        H(b, "<a class=\"icon-btn\" href=\"%s\" aria-label=\"Edit %s\">", buf_cstr(&href), net_label(d));
        ui_icon(b, "edit");
        buf_puts(b, "</a>");
        buf_free(&href);
    }
    buf_puts(b, "</div></li>");
}

static void edit_modal(Ctx *c, Buf *b, const Device *d) {
    Buf close = {0};
    ctx_url(c, &close, "edit", NULL, NULL);
    ui_modal(b, net_label(d), buf_cstr(&close));
    H(b, "<dl class=\"net-facts\"><dt>Address</dt><dd>%s</dd><dt>Hardware</dt><dd><code>%s</code></dd>",
      d->last_ip[0] ? d->last_ip : "—", d->mac);
    if (d->hostname[0]) H(b, "<dt>Network name</dt><dd>%s</dd>", d->hostname);
    buf_puts(b, "<dt>First seen</dt><dd>");
    ui_day(b, d->first_seen, c->now);
    buf_puts(b, "</dd><dt>Last seen</dt><dd>");
    if (net_online(d, c->now)) buf_puts(b, "online now");
    else if (d->last_seen) ui_ago(b, d->last_seen, c->now);
    else buf_puts(b, "never");
    buf_puts(b, "</dd></dl>");
    if (d->random_mac)
        buf_puts(b, "<p class=\"net-small muted\">This is a private Wi-Fi address — phones pick one per network. It "
                    "stays the same unless the phone is set to rotate it.</p>");

    ui_form(b, "net.save", "stack net-form");
    ui_hidden_int(b, "id", d->id);
    H(b,
      "<label class=\"field\"><span>Name</span><input name=\"name\" maxlength=\"40\" autocomplete=\"off\" "
      "value=\"%s\" placeholder=\"%s\" autofocus></label>",
      d->name, d->hostname[0] ? d->hostname : "e.g. Eva's phone");
    buf_puts(b, "<label class=\"field\"><span>Belongs to</span>");
    ui_person_select(b, "owner", d->owner, "Shared / nobody");
    buf_puts(b, "</label><div class=\"field\"><span>Kind</span><div class=\"emojis\">");
    H(b, "<label class=\"emo\" title=\"Guess from its name\"><input type=\"radio\" name=\"kind\" value=\"\"%r><span "
         "title=\"Auto\">Auto</span></label>",
      d->kind[0] ? "" : " checked");
    for (int i = 0; i < countof(KINDS); i++)
        H(b,
          "<label class=\"emo\" title=\"%s\"><input type=\"radio\" name=\"kind\" value=\"%s\" aria-label=\"%s\"%r><span>%s</span></label>",
          KIND_NAMES[i], KINDS[i], KIND_NAMES[i], str_eq(d->kind, KINDS[i]) ? " checked" : "", KINDS[i]);
    H(b,
      "</div></div><label class=\"net-check\"><input type=\"checkbox\" name=\"wol\" value=\"1\"%r><span><b>Wake-on-LAN"
      "</b><small>Show a button that can switch this computer on over the network.</small></span></label>",
      d->wol ? " checked" : "");
    H(b, "<label class=\"field\"><span>Note</span><textarea name=\"note\" maxlength=\"118\" rows=\"2\">%s</textarea>"
         "</label><div class=\"modal-f\"><button class=\"btn primary\">Save</button></div>",
      d->note);
    ui_form_end(b);
    H(b,
      "<form method=\"post\" action=\"/a/net.forget\" data-a=\"net.forget\" class=\"danger-zone\" "
      "data-confirm=\"Forget %s? It comes back if it's seen again.\">",
      net_label(d));
    ui_hidden_int(b, "id", d->id);
    buf_puts(b, "<button class=\"btn danger\">Forget device</button></form>");
    ui_modal_end(b);
    buf_free(&close);
}

static void page(Ctx *c, Buf *b) {
    ctx_title(c, "Network");
    int n = tbl_count(&T_device), online = 0, people = 0, unknown = 0, tidy = 0;
    for (int i = 0; i < n; i++) {
        Device *d = tbl_at(&T_device, i);
        online += net_online(d, c->now);
        people += d->owner != 0;
        unknown += is_unknown(d);
        tidy += is_tidy(d, c->now);
    }
    char sub[96];
    snprintf(sub, sizeof sub, "%d online · %d known", online, n);
    ui_head(b, "Network", sub);
    ui_head_end(b);

    buf_puts(b, "<div class=\"net-top\">");
    hub_card(b);
    scan_card(c, b);
    buf_puts(b, "</div>");

    const char *f = ctx_q(c, "f");
    buf_puts(b, "<div class=\"toolbar\"><div class=\"chips\">");
    filter_chip(c, b, NULL, "All", n);
    filter_chip(c, b, "online", "Online", online);
    filter_chip(c, b, "people", "People's", people);
    filter_chip(c, b, "unknown", "Unknown", unknown);
    buf_puts(b, "</div></div>");

    int *idx = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
    int shown = 0;
    for (int i = 0; i < n; i++) {
        Device *d = tbl_at(&T_device, i);
        if (str_eq(f, "online") && !net_online(d, c->now)) continue;
        if (str_eq(f, "people") && !d->owner) continue;
        if (str_eq(f, "unknown") && !is_unknown(d)) continue;
        idx[shown++] = i;
    }
    g_sort_now = c->now;
    qsort(idx, (size_t)shown, sizeof *idx, dev_order);
    if (!shown) {
        buf_puts(b, "<section class=\"card\">");
        if (n) ui_empty(b, "🔍", "Nothing matches", "Try another filter.");
        else
            ui_empty(b, "📡", "No devices yet",
                     net_scan_on() ? "The first sweep is on its way."
                                   : "Devices appear as this hub talks to them — or turn on discovery to find them all.");
        buf_puts(b, "</section>");
    } else {
        long long want = ctx_qint(c, "n", PAGE_ROWS);
        int limit = (int)CLAMP(want, PAGE_ROWS, NET_MAX_DEVICES), upto = MIN(shown, limit);
        buf_puts(b, "<section class=\"card net-list\"><ul class=\"net-devs\">");
        for (int i = 0; i < upto; i++) device_row(c, b, tbl_at(&T_device, idx[i]));
        buf_puts(b, "</ul>");
        if (shown > upto) {
            char more[16];
            snprintf(more, sizeof more, "%d", limit + PAGE_ROWS);
            Buf u = {0};
            ctx_url(c, &u, "n", more, "edit", NULL, NULL);
            H(b, "<p class=\"net-more\" id=\"net-more\"><a class=\"btn sm\" href=\"%s\">Show %d more</a><span "
                 "class=\"muted\">%d of %d shown</span></p>",
              buf_cstr(&u), MIN(PAGE_ROWS, shown - upto), upto, shown);
            buf_free(&u);
        }
        buf_puts(b, "</section>");
    }
    free(idx);
    if (tidy && is_admin(c)) {
        const char *s = tidy == 1 ? "" : "s";
        H(b,
          "<form method=\"post\" action=\"/a/net.tidy\" data-a=\"net.tidy\" class=\"net-tidy\" id=\"net-tidy\" "
          "data-confirm=\"Forget %d unknown device%s not seen for a week? Any that come back show up again.\">"
          "<button class=\"btn sm\">Forget %d unknown device%s not seen for a week</button></form>",
          tidy, s, tidy, s);
    }

    Device *ed = tbl_get(&T_device, (int)ctx_qint(c, "edit", 0));
    if (ed && is_admin(c)) edit_modal(c, b, ed);
}

/* ------------------------------------------------------------- widgets */

static bool tracking(void) {
    if (!net_scan_on()) return false;
    for (int i = 0; i < tbl_count(&T_device); i++)
        if (((Device *)tbl_at(&T_device, i))->owner) return true;
    return false;
}

static bool has_device(int person) {
    for (int i = 0; i < tbl_count(&T_device); i++)
        if (((Device *)tbl_at(&T_device, i))->owner == person) return true;
    return false;
}

static void widget(Ctx *c, Buf *b, int where) {
    int online = 0;
    for (int i = 0; i < tbl_count(&T_device); i++) online += net_online(tbl_at(&T_device, i), c->now);
    bool track = tracking();
    if (where == WIDGET_WALL) {
        ui_card(b, track ? "Who's home" : "Network", NULL, NULL);
        if (track) {
            buf_puts(b, "<ul class=\"net-whohome\">");
            for (int i = 0; i < person_count(); i++) {
                Person *p = person_at(i);
                if (!has_device(p->id)) continue;
                int64_t since = 0;
                bool home = presence_is_home(p->id, &since);
                H(b, "<li class=\"%s\">", home ? "home" : "away");
                ui_avatar(b, p, "md");
                H(b, "<span><b>%s</b><small>%s", p->name, home ? "home" : "away");
                if (since) {
                    buf_puts(b, " · since ");
                    if (day_start(since) == day_start(c->now)) ui_hm(b, since);
                    else ui_day(b, since, c->now);
                }
                buf_puts(b, "</small></span></li>");
            }
            buf_puts(b, "</ul>");
        }
        H(b, "<p class=\"net-w-count\"><b>%d</b> device%s online</p>", online, online == 1 ? "" : "s");
        ui_card_end(b);
        return;
    }
    ui_card(b, "Network", "/net", "Devices");
    H(b, "<div class=\"net-w-big\"><b>%d</b><span>device%s online</span></div>", online, online == 1 ? "" : "s");
    if (track) {
        buf_puts(b, "<div class=\"net-w-home\"><span class=\"muted\">Home now</span><div class=\"net-w-av\">");
        int home = 0;
        for (int i = 0; i < person_count(); i++) {
            Person *p = person_at(i);
            if (!presence_is_home(p->id, NULL)) continue;
            ui_avatar(b, p, "sm");
            home++;
        }
        if (!home) buf_puts(b, "<span class=\"muted\">nobody</span>");
        buf_puts(b, "</div></div>");
    } else if (!net_scan_on()) {
        buf_puts(b, "<p class=\"net-small muted\">Turn on discovery to see who's home.</p>");
    } else {
        buf_puts(b, "<p class=\"net-small muted\">Tell Hemma whose devices are whose to see who's home.</p>");
    }
    ui_card_end(b);
}

/* ------------------------------------------------------------- actions */

static Device *device_for(Ctx *c, Fields *f) {
    Device *d = tbl_get(&T_device, (int)fields_int(f, "id", 0));
    if (!d) ctx_fail(c, "That device isn't known any more");
    return d;
}

static void back_without_edit(Ctx *c) {
    Buf u = {0};
    ctx_url(c, &u, "edit", NULL, NULL);
    ctx_redirect(c, "%s", buf_cstr(&u));
    buf_free(&u);
}

static void act_scan(Ctx *c, Fields *f) {
    if (!is_admin(c)) { /* active probing of the whole network is the admin's call */
        ctx_fail(c, "Only an admin can switch network discovery");
        return;
    }
    bool on = str_eq(fields_str(f, "on"), "1");
    setting_set("net.scan", on ? "1" : "0");
    if (on) {
        activity_add(c->me->id, "📡", "/net", "%s turned on network discovery", c->me->name);
        ctx_ok(c, "Discovery is on — the first sweep starts in a moment");
    } else {
        forget_presence(0); /* without sweeps the neighbour cache can't tell who left */
        ctx_info(c, "Discovery is off");
    }
}

/* Devices, Wake-on-LAN and who counts as home are the admin's to change (owner's call). */
static bool admin_only(Ctx *c) {
    if (is_admin(c)) return true;
    ctx_fail(c, "Only an admin can change the network page");
    return false;
}

static void act_window(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    setting_set_int("net.window", CLAMP(fields_int(f, "min", 10), 1, 240));
    if (net_scan_on()) update_presence(now_sec());
    ctx_ok(c, "Saved");
}

static void act_save(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Device *d = device_for(c, f);
    if (!d) return;
    Device upd = *d;
    char name[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    str_copy(upd.name, sizeof upd.name, name);
    int owner = (int)fields_int(f, "owner", 0);
    upd.owner = person_get(owner) ? owner : 0;
    const char *kind = fields_str(f, "kind");
    str_copy(upd.kind, sizeof upd.kind, known_kind(kind) ? kind : "");
    upd.wol = fields_get(f, "wol") != NULL;
    char note[160];
    str_copy(note, sizeof note, fields_str(f, "note"));
    str_trim(note);
    str_copy(upd.note, sizeof upd.note, note);
    upd.dirty = false;
    tbl_put(&T_device, &upd);
    if (net_scan_on()) update_presence(now_sec());
    ctx_ok(c, "Saved %s", net_label(&upd));
    back_without_edit(c);
}

static void act_assign(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Device *d = device_for(c, f);
    Person *p = person_get((int)fields_int(f, "person", 0));
    if (!d) return;
    if (!p) {
        ctx_fail(c, "Unknown person");
        return;
    }
    Device upd = *d;
    upd.owner = p->id;
    tbl_put(&T_device, &upd);
    if (net_scan_on()) update_presence(now_sec());
    ctx_ok(c, "%s is now %s's", net_label(&upd), p->name);
}

static void act_forget(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Device *d = device_for(c, f);
    if (!d) return;
    char label[64];
    str_copy(label, sizeof label, net_label(d));
    tbl_del(&T_device, d->id);
    find_router();
    if (net_scan_on()) update_presence(now_sec()); /* maybe its owner's last device */
    ctx_ok(c, "Forgot %s", label);
    back_without_edit(c);
}

/* All at once, highest ids first (each delete then moves only the rows that stay). None of
   them has an owner: who's home doesn't change. */
static void act_tidy(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    int n = 0;
    Spare *s = xmalloc(sizeof(Spare) * (size_t)(tbl_count(&T_device) + 1));
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        if (is_tidy(d, c->now)) s[n++] = (Spare){false, d->last_seen, d->id};
    }
    qsort(s, (size_t)n, sizeof *s, by_id_desc);
    for (int i = 0; i < n; i++) tbl_del(&T_device, s[i].id);
    free(s);
    find_router();
    if (n) ctx_ok(c, "Forgot %d device%s not seen for a week", n, n == 1 ? "" : "s");
    else ctx_info(c, "Nothing to forget");
}

static void act_wake(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Device *d = device_for(c, f);
    if (!d) return;
    Mac m;
    if (!d->wol || !mac_parse(d->mac, &m)) {
        ctx_fail(c, "Wake-on-LAN isn't enabled for this device");
        return;
    }
    char label[64];
    str_copy(label, sizeof label, net_label(d));
    bool ok = true;
    if (!app_testing()) { /* tests never put packets on the wire */
        sock_t s = lan_udp_open(true);
        ok = s != SOCK_BAD && lan_wake(s, &m, S.have_ifc ? S.ifc.broadcast : 0);
        if (s != SOCK_BAD) sock_close(s);
    }
    if (!ok) {
        ctx_fail(c, "Couldn't send the wake-up packet");
        return;
    }
    activity_add(c->me->id, "⏰", "/net", "%s woke up %s", c->me->name, label);
    ctx_ok(c, "Wake-up sent to %s — give it a minute", label);
}

/* -------------------------------------------------------------- module */

/* The System page's checks: is there a network, and is discovery on and working? (Cached
   facts: the interface is looked up once a minute by the tick.) */
static void health(Ctx *c, Health *h) {
    char d[320], ip[16], gw[16];
    if (S.have_ifc) {
        ip_format(S.ifc.ip, ip);
        if (S.ifc.gateway) {
            ip_format(S.ifc.gateway, gw);
            snprintf(d, sizeof d, "Connected as %s (%s), with the router at %s.", ip, S.ifc.name, gw);
        } else {
            snprintf(d, sizeof d, "Connected as %s (%s), but no router was found: devices may go unnamed.", ip,
                     S.ifc.name);
        }
        health_add(h, HEALTH_OK, "Home network", d, NULL, NULL);
    } else if (!app_testing()) { /* (tests have no interface unless they set one) */
        health_add(h, HEALTH_WARN, "Home network", "Hemma can't find a network connection on this computer.",
                   "Check the network cable or Wi-Fi - Hemma looks again every minute.", NULL);
    }
    if (!net_scan_on()) {
        health_add(h, HEALTH_INFO, "Device discovery",
                   "Off: the Network page only lists devices that talk to Hemma, and nobody shows as home or away.",
                   "Turn it on on the Network page to see who's home.", "/net");
    } else if (S.udp == SOCK_BAD && S.udp_retry) {
        health_add(h, HEALTH_WARN, "Device discovery", "On, but Hemma can't send its probes (no network socket).",
                   "The log says more; Hemma tries again every minute.", "/system/log?f=warn");
    } else {
        int n = snprintf(d, sizeof d, "On: Hemma looks for devices every %d minutes, so it can tell who's home.",
                         NET_SCAN_EVERY / 60);
        if (S.last_sweep && n > 0 && (size_t)n < sizeof d) {
            DateTime t = dt_local(S.last_sweep);
            snprintf(d + n, sizeof d - (size_t)n, " Last look: %02d:%02d.", t.hour % 100, t.min % 100);
        }
        health_add(h, HEALTH_OK, "Device discovery", d, NULL, "/net");
    }
}

static void init(void) { store_register(&T_device); }

static void start(void) {
    memset(&S, 0, sizeof S);
    S.udp = SOCK_BAD;
    presence_set_guesser(net_guess);
    int64_t now = now_sec();
    for (int i = 0; i < tbl_count(&T_device); i++) {
        Device *d = tbl_at(&T_device, i);
        d->online = net_online(d, now);
        d->dirty = false;
        d->ptr_asked = 0;
        d->ptr_open = 0;
        d->listed = 0;
    }
    if (!app_testing()) refresh_iface();
    trim(now); /* after refresh_iface: the router is never trimmed */
}

static void stop(void) {
    close_udp();
    flush();
}

static const Action actions[] = {
    {"net.scan", act_scan},     {"net.window", act_window}, {"net.save", act_save},
    {"net.assign", act_assign}, {"net.forget", act_forget}, {"net.wake", act_wake},
    {"net.tidy", act_tidy},
    {0},
};

static const Module net = {
    .id = "net",
    .title = "Network",
    .icon = "wifi",
    .nav = 6,
    .init = init,
    .start = start,
    .stop = stop,
    .tick = tick,
    .page = page,
    .actions = actions,
    .http = http_hook,
    .widget = widget,
    .widget_order = 30,
    .health = health,
};
MODULE(net)
