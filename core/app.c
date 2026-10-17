/* core/app.c — the live application core.
 *
 * Every open browser tab is a Client with a URL. Whenever data changes, each
 * client's page is rendered again (cheap: it's C) and, if the HTML differs
 * from what that tab last got, it is sent as a "morph" op over the
 * WebSocket. The wire diffs it into the DOM, so user input, focus and
 * scroll survive. Plain HTTP (no JS) renders the same pages. */
#include "app.h"
#include "assets.h"
#include "auth.h"
#include "codec.h"
#include "db.h"
#include "env.h"
#include "json.h"
#include "lan.h"
#include "ui.h"

#include <limits.h>
#include <stdatomic.h>

/* ================================================================ tables */

static const FieldDef person_fields[] = {
    FIELD(Person, id, F_INT),        FIELD(Person, name, F_STR),          FIELD(Person, emoji, F_STR),
    FIELD(Person, hue, F_INT),       FIELD(Person, created, F_I64),       FIELD(Person, username, F_STR),
    FIELD_SECRET(Person, pass, F_STR), FIELD(Person, role, F_INT),        FIELD(Person, last_login, F_I64),
};
Table T_person = TABLE("person", Person, person_fields);

static const FieldDef session_fields[] = {
    FIELD(Session, id, F_INT),      FIELD(Session, token, F_STR), FIELD(Session, person, F_INT),
    FIELD(Session, created, F_I64), FIELD(Session, seen, F_I64),  FIELD(Session, agent, F_STR),
    FIELD(Session, remember, F_BOOL), FIELD(Session, viewer, F_INT),
};
Table T_session = TABLE("session", Session, session_fields);

typedef struct {
    int id;
    char key[48];
    char val[512];
} Setting;
static const FieldDef setting_fields[] = {FIELD(Setting, id, F_INT), FIELD(Setting, key, F_STR),
                                          FIELD(Setting, val, F_STR)};
static Table T_setting = TABLE("setting", Setting, setting_fields);

typedef struct {
    int id;
    int person;
    char key[48];
    char val[256];
} Pref;
static const FieldDef pref_fields[] = {FIELD(Pref, id, F_INT), FIELD(Pref, person, F_INT), FIELD(Pref, key, F_STR),
                                       FIELD(Pref, val, F_STR)};
static Table T_pref = TABLE("pref", Pref, pref_fields);

static const FieldDef activity_fields[] = {
    FIELD(Activity, id, F_INT),   FIELD(Activity, ts, F_I64),   FIELD(Activity, who, F_INT),
    FIELD(Activity, icon, F_STR), FIELD(Activity, text, F_STR), FIELD(Activity, link, F_STR),
};
Table T_activity = TABLE("activity", Activity, activity_fields);

/* ================================================================ config */

static AppConfig g_cfg;
static char g_asset_ver[17];
static bool g_testing;
static char g_extra_hosts[512]; /* HEMMA_HOSTS: more names this server answers to */

bool app_testing(void) { return g_testing; }
void app_set_testing(bool on) { g_testing = on; }

const char *app_name(void) { return setting("household.name", "Hemma"); }
const char *app_asset_version(void) { return g_asset_ver; }
bool app_dev_mode(void) { return g_cfg.web_dir != NULL; }
const AppConfig *app_config(void) { return &g_cfg; }

int app_addresses(char out[][96], int max) {
    int n = 0, port = server_port() > 0 ? server_port() : g_cfg.port;
    char ps[8] = "";
    if (port > 0 && port != 80) snprintf(ps, sizeof ps, ":%d", port);
    uint32_t bound = 0;
    char ip[16];
    /* the listen setting as the listener reads it (issue #99: "localhost" and "*" were shown as such) */
    if (max <= 0 || !listen_addr(g_cfg.host, &bound)) return 0;
    if (bound != 0) { /* listening on one address: that's the one (127.0.0.1: see app_local_only) */
        ip_format(bound, ip);
        snprintf(out[n++], 96, "http://%s%s", ip, ps);
        return n;
    }
    LanIface ifc;
    if (!lan_iface(&ifc)) return 0;
    ip_format(ifc.ip, ip);
    snprintf(out[n++], 96, "http://%s%s", ip, ps);
    /* the machine's own name, announced on the LAN as <name>.local - only when something announces it
       (mDNS: Avahi, Bonjour, Windows) */
    char name[64] = "";
    if (n < max && lan_mdns_announced() && gethostname(name, (int)sizeof name - 1) == 0 && name[0]) {
        name[sizeof name - 1] = '\0';
        bool ok = true;
        for (char *p = name; *p && ok; p++) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
            ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.';
        }
        if (str_iends(name, ".local")) name[strlen(name) - 6] = '\0';
        if (ok && name[0] && !strchr(name, '.') && !str_eq(name, "localhost"))
            snprintf(out[n++], 96, "http://%s.local%s", name, ps);
    }
    return n;
}

bool app_local_only(void) {
    uint32_t bound = 0;
    return listen_addr(g_cfg.host, &bound) && (bound >> 24) == 127;
}

void app_test_host(const char *host) { g_cfg.host = host; }

/* ================================================================ health */

static HealthItem *health_new(Health *h) {
    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 16;
        h->items = xrealloc(h->items, (size_t)h->cap * sizeof *h->items);
    }
    HealthItem *it = &h->items[h->n++];
    memset(it, 0, sizeof *it);
    return it;
}

void health_add(Health *h, int state, const char *title, const char *detail, const char *todo, const char *link) {
    HealthItem *it = health_new(h);
    it->state = CLAMP(state, HEALTH_OK, HEALTH_BAD);
    str_copy(it->title, sizeof it->title, title);
    str_copy(it->detail, sizeof it->detail, detail);
    str_copy(it->todo, sizeof it->todo, todo);
    str_copy(it->link, sizeof it->link, link);
}

void health_step(Health *h, bool done, const char *title, const char *detail, const char *link) {
    health_add(h, done ? HEALTH_OK : HEALTH_INFO, title, detail, NULL, link);
    h->items[h->n - 1].step = true;
}

int health_worst(const Health *h) {
    int worst = HEALTH_OK;
    for (int i = 0; i < h->n; i++)
        if (!h->items[i].step && h->items[i].state > worst) worst = h->items[i].state;
    return worst;
}

void health_free(Health *h) {
    free(h->items);
    h->items = NULL;
    h->n = h->cap = 0;
}

/* =============================================================== modules */

#define MAX_MODULES 64
static const Module *mods[MAX_MODULES];
static int nmods;
static bool mods_sorted;

void module_register(const Module *m) {
    if (nmods < MAX_MODULES) mods[nmods++] = m;
    mods_sorted = false;
}

static int mod_cmp(const void *a, const void *b) {
    const Module *x = *(const Module *const *)a, *y = *(const Module *const *)b;
    int xn = x->nav ? x->nav : 1000, yn = y->nav ? y->nav : 1000;
    return xn != yn ? xn - yn : strcmp(x->id, y->id);
}

static void sort_mods(void) {
    if (mods_sorted) return;
    qsort(mods, (size_t)nmods, sizeof *mods, mod_cmp);
    mods_sorted = true;
}

int module_count(void) { return nmods; }

const Module *module_at(int i) {
    sort_mods();
    return i >= 0 && i < nmods ? mods[i] : NULL;
}

const Module *module_find(const char *id) {
    for (int i = 0; i < nmods; i++)
        if (str_eq(mods[i]->id, id)) return mods[i];
    return NULL;
}

static const Module *module_for_path(const char *path) {
    if (!path[0] || str_eq(path, "/")) return module_find("home");
    const char *p = path + 1;
    size_t n = strcspn(p, "/");
    char seg[64];
    if (n == 0 || n >= sizeof seg) return NULL;
    memcpy(seg, p, n);
    seg[n] = '\0';
    return module_find(seg);
}

const Block *block_find(const char *id, const Module **owner) {
    for (int i = 0; i < nmods; i++)
        for (const Block *bl = mods[i]->blocks; bl && bl->id; bl++)
            if (str_eq(bl->id, id)) {
                if (owner) *owner = mods[i];
                return bl;
            }
    return NULL;
}

static const Action *action_find(const char *name, const Module **owner);

bool app_action_display(const char *name) {
    const Module *owner = NULL;
    const Action *a = action_find(name, &owner);
    return a && a->display;
}

static const Action *action_find(const char *name, const Module **owner) {
    for (int i = 0; i < nmods; i++) {
        for (const Action *a = mods[i]->actions; a && a->name; a++) {
            if (str_eq(a->name, name)) {
                if (owner) *owner = mods[i];
                return a;
            }
        }
    }
    return NULL;
}

void health_collect(Ctx *c, Health *out) {
    sort_mods();
    for (int i = 0; i < nmods; i++) {
        if (!mods[i]->health) continue;
        int before = out->n;
        mods[i]->health(c, out);
        for (int k = before; k < out->n; k++) out->items[k].module = mods[i]->id;
    }
}

/* ================================================================ people */

Person *person_get(int id) { return id > 0 ? tbl_get(&T_person, id) : NULL; }
int account_count(void) { return tbl_count(&T_person); }
Person *account_at(int i) { return tbl_at(&T_person, i); }

int person_count(void) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_person); i++) n += ((Person *)tbl_at(&T_person, i))->role != ROLE_DISPLAY;
    return n;
}

Person *person_real(int id) {
    Person *p = person_get(id);
    return p && p->role != ROLE_DISPLAY ? p : NULL;
}

Person *person_at(int i) {
    for (int k = 0; k < tbl_count(&T_person); k++) {
        Person *p = tbl_at(&T_person, k);
        if (p->role != ROLE_DISPLAY && i-- == 0) return p;
    }
    return NULL;
}

Person *person_find(const char *name) {
    if (str_blank(name)) return NULL;
    Person *prefix = NULL;
    int nprefix = 0;
    char want[64], have[64];
    fold_copy(want, sizeof want, name);
    for (int i = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        fold_copy(have, sizeof have, p->name);
        if (str_eq(have, want)) return p;
        if (str_starts(have, want)) prefix = p, nprefix++;
    }
    return nprefix == 1 ? prefix : NULL;
}

/* ============================================================== sessions */

static bool valid_sid(const char *s) {
    if (strlen(s) != 32) return false;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return false;
    return true;
}

/* cookie value -> stored token (we only ever keep the hash) */
static void sid_token(const char *sid, char token[65]) {
    if (sid && valid_sid(sid)) sha256_hex(sid, 32, token);
    else token[0] = '\0';
}

/* A session that has ended: its person is gone, it sat unused too long (a week; a year with
   "keep me signed in"), or it's an admin's and older than ADMIN_SESSION_DAYS however much it
   was used - a captured admin cookie (plain http) stops working within a month (issue #5). */
static bool session_over(const Session *s, int64_t now) {
    const Person *p = s->token[0] ? person_get(s->person) : NULL;
    if (!p) return true;
    if (now - MAX(s->seen, s->created) > (s->remember ? 366 : 7) * 86400LL) return true;
    return p->role == ROLE_ADMIN && now - s->created > ADMIN_SESSION_DAYS * 86400LL;
}

static Session *session_by_token(const char *token) {
    if (!token || !token[0]) return NULL;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        if (strcmp(s->token, token) == 0) return session_over(s, now_sec()) ? NULL : s; /* pruned hourly */
    }
    return NULL;
}

static void agent_summary(const char *ua, char *out, size_t cap) {
    const char *browser = !ua                       ? "A browser"
                          : strstr(ua, "Edg/")      ? "Edge"
                          : strstr(ua, "Firefox/")  ? "Firefox"
                          : strstr(ua, "Chrome/")   ? "Chrome"
                          : strstr(ua, "Safari/")   ? "Safari"
                                                    : "A browser";
    const char *os = !ua                          ? ""
                     : strstr(ua, "iPhone")       ? "iPhone"
                     : strstr(ua, "iPad")         ? "iPad"
                     : strstr(ua, "Android")      ? "Android"
                     : strstr(ua, "Windows")      ? "Windows"
                     : strstr(ua, "Mac OS")       ? "Mac"
                     : strstr(ua, "Linux")        ? "Linux"
                                                  : "";
    snprintf(out, cap, "%s%s%s", browser, os[0] ? " on " : "", os);
}

bool session_create(int person, bool remember, const char *user_agent, char sid_out[33]) {
    rand_hex(sid_out, 16);
    Session s = {0};
    sid_token(sid_out, s.token);
    s.person = person;
    s.created = s.seen = now_sec();
    s.remember = remember;
    agent_summary(user_agent, s.agent, sizeof s.agent);
    tbl_put(&T_session, &s);
    return true;
}

void session_end(const char *sid) {
    char token[65];
    sid_token(sid, token);
    Session *s = session_by_token(token);
    if (s) tbl_del(&T_session, s->id);
}

/* Address keys (ctx_url_key): "<session id>-<expires>-<HMAC(server secret, session token + path
   + expires + the screen's network address), 128 bits>". The token (the cookie's hash) never leaves
   the server, so a key can't be made without the secret; it fits only that path, only from the
   address of the screen it was made for (a TV's browser and its video player share one), and
   only until it expires or its session ends (signing out, a long break). Found 2026-10-13: a
   Samsung TV's video player asked for the file without the page's cookie and got the sign-in page,
   which it reported as a decode error - every video, every Hemma version. Issue #78: 0.6.0's keys
   had no expiry or address, so a copied link played from anywhere while the session lasted. */
#define KEY_HOURS 48 /* a key lasts this long, to the next whole hour: a film left paused overnight still plays */

static bool url_key(const Session *s, const char *path, uint32_t ip, int64_t expires, char out[CTX_KEY_LEN]) {
    out[0] = '\0';
    if (!s || s->id <= 0 || !s->token[0]) return false;
    Buf msg = {0};
    buf_printf(&msg, "hemma address key\n%s\n%s\n%lld\n%u", s->token, path, (long long)expires, (unsigned)ip);
    uint8_t mac[32];
    bool ok = auth_mac(msg.p, msg.len, mac);
    buf_free(&msg);
    if (!ok) return false;
    char hex[33];
    hex_encode(mac, 16, hex);
    snprintf(out, CTX_KEY_LEN, "%d-%lld-%s", s->id, (long long)expires, hex);
    return true;
}

/* The session a request's ?k= stands for: only a live one, only with the key for its path and the
   address it comes from, and only before the key expires. */
static Session *session_by_url_key(const HttpReq *r, uint32_t ip) {
    if (!r->query[0]) return NULL;
    Fields q = {0};
    fields_parse(&q, r->query, strlen(r->query));
    const char *k = fields_str(&q, "k");
    char *end = NULL, *end2 = NULL;
    long id = strtol(k, &end, 10);
    long long expires = end && *end == '-' ? strtoll(end + 1, &end2, 10) : 0;
    int64_t now = now_sec();
    Session *s = id > 0 && id < INT_MAX && expires > now && end2 && *end2 == '-' ? tbl_get(&T_session, (int)id) : NULL;
    char want[CTX_KEY_LEN];
    bool ok = s && !session_over(s, now) && url_key(s, r->path, ip, expires, want) && strlen(k) == strlen(want) &&
              ct_equal(k, want, strlen(want));
    fields_free(&q);
    return ok ? s : NULL;
}

int sessions_drop(int person, const Session *keep) {
    char keep_token[65] = ""; /* copy: deleting rows moves the table under `keep` */
    if (keep) str_copy(keep_token, sizeof keep_token, keep->token);
    int dropped = 0;
    for (int i = tbl_count(&T_session) - 1; i >= 0; i--) {
        Session *s = tbl_at(&T_session, i);
        if (s->person == person && (!keep_token[0] || strcmp(s->token, keep_token) != 0)) {
            tbl_del(&T_session, s->id);
            dropped++;
        }
    }
    return dropped;
}

/* =============================================================== clients */

#define LIVE_SLOTS 8
/* Navigation by one screen re-renders only that screen, at most a burst of 15 and then
   5 a second; past that the screen catches up on the next tick (a script flooding "nav"
   costs one render a second). */
#define NAV_COST_MS 200
#define NAV_BURST_MS 3000
struct Client {
    Conn *conn;
    char token[65]; /* session token (hash of the cookie), "" = signed out */
    char url[600];
    char title[160]; /* the document title last sent */
    uint64_t html_hash, head_hash;
    bool ready, dirty, push;
    bool late;       /* navigated past its budget: rendered on the next tick */
    int64_t nav_tat; /* monotonic ms: when its navigation budget is whole again (plus the burst) */
    struct {
        char k[32];
        char v[256];
    } live[LIVE_SLOTS];
};

static Client **clients;
static int nclients, capclients;
static bool all_dirty;
static long g_renders;

long app_renders(void) { return g_renders; }

static Client *client_new(Conn *conn, const char *token) {
    Client *cl = xcalloc(1, sizeof *cl);
    cl->conn = conn;
    str_copy(cl->token, sizeof cl->token, token);
    str_copy(cl->url, sizeof cl->url, "/");
    conn->user = cl;
    if (nclients == capclients) {
        capclients = capclients ? capclients * 2 : 16;
        clients = xrealloc(clients, (size_t)capclients * sizeof *clients);
    }
    clients[nclients++] = cl;
    return cl;
}

/* Signed-out screens (sign-in pages) stay open as long as they answer pings, so they get a
   small share of the server's connections: one host with a few addresses could otherwise
   hold every slot and lock the household out (issue #5). */
#define STRANGER_SCREENS 48
#define STRANGER_SCREENS_PER_ADDR 4

static bool stranger_room(uint32_t ip) {
    int all = 0, here = 0;
    for (int i = 0; i < nclients; i++) {
        if (clients[i]->token[0]) continue;
        all++;
        if (clients[i]->conn && clients[i]->conn->ip == ip) here++;
    }
    return all < STRANGER_SCREENS && here < STRANGER_SCREENS_PER_ADDR;
}

static void client_remove(Client *cl) {
    for (int i = 0; i < nclients; i++) {
        if (clients[i] == cl) {
            clients[i] = clients[--nclients];
            break;
        }
    }
    free(cl);
}

void app_changed(void) { all_dirty = true; }

void app_changed_where(const char *prefix) {
    for (int i = 0; i < nclients; i++)
        if (str_starts(clients[i]->url, prefix)) clients[i]->dirty = true;
}

/* ================================================================== ctx */

static Session g_tmp_sessions[4]; /* signed-out visitors */
static int g_tmp_next;

static Session *session_for(const char *token) {
    Session *s = session_by_token(token);
    if (s) return s;
    Session *t = &g_tmp_sessions[g_tmp_next++ % countof(g_tmp_sessions)];
    memset(t, 0, sizeof *t);
    return t;
}

static void ctx_set_url(Ctx *c, const char *url) {
    if (!url || url[0] != '/' || url[1] == '/') url = "/"; /* no scheme-relative escapes */
    str_copy(c->url, sizeof c->url, url);
    const char *q = strchr(c->url, '?');
    size_t plen = q ? (size_t)(q - c->url) : strlen(c->url);
    char raw[300];
    str_ncopy(raw, sizeof raw, c->url, plen);
    url_decode(raw, strlen(raw), c->path, false);
    fields_free(&c->q);
    if (q) fields_parse(&c->q, q + 1, strlen(q + 1));
}

static void ctx_init(Ctx *c, Session *s, Client *cl, uint32_t ip, const char *url) {
    memset(c, 0, sizeof *c);
    c->sess = s;
    c->me = s ? person_get(s->person) : NULL;
    c->client = cl;
    c->ip = ip;
    c->now = now_sec();
    ctx_set_url(c, url);
}

static void ctx_free(Ctx *c) { fields_free(&c->q); }

static void client_ctx(Client *cl, Ctx *c) {
    ctx_init(c, session_for(cl->token), cl, cl->conn ? cl->conn->ip : 0, cl->url);
}

const char *ctx_q(const Ctx *c, const char *key) { return fields_str(&c->q, key); }
long long ctx_qint(const Ctx *c, const char *key, long long dflt) { return fields_int(&c->q, key, dflt); }
void ctx_url_key(const Ctx *c, const char *path, char out[CTX_KEY_LEN]) {
    /* (to the hour: a page's html - and its hash - changes once an hour, not with every render) */
    url_key(c->sess, path, c->ip, (now_sec() / 3600 + KEY_HOURS + 1) * 3600, out);
}

void ctx_title(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->title, sizeof c->title, fmt, ap);
    va_end(ap);
}

static void ctx_toast_v(Ctx *c, int kind, const char *fmt, va_list ap) {
    c->toast_kind = kind;
    vsnprintf(c->toast, sizeof c->toast, fmt, ap);
}

void ctx_ok(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ctx_toast_v(c, TOAST_OK, fmt, ap);
    va_end(ap);
}

void ctx_info(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ctx_toast_v(c, TOAST_INFO, fmt, ap);
    va_end(ap);
}

void ctx_fail(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ctx_toast_v(c, TOAST_ERR, fmt, ap);
    va_end(ap);
    c->failed = true;
}

void ctx_redirect(Ctx *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->redirect, sizeof c->redirect, fmt, ap);
    va_end(ap);
}

void ctx_url(const Ctx *c, Buf *out, ...) {
    /* collect overrides */
    const char *keys[16], *vals[16];
    int n = 0;
    va_list ap;
    va_start(ap, out);
    for (const char *k; n < 16 && (k = va_arg(ap, const char *)) != NULL; n++) {
        keys[n] = k;
        vals[n] = va_arg(ap, const char *);
    }
    va_end(ap);
    Buf u = {0};
    url_encode(&u, c->path, true);
    char sep = '?';
    for (int i = 0; i < c->q.n; i++) {
        bool overridden = false;
        for (int k = 0; k < n; k++) overridden |= str_eq(keys[k], c->q.k[i]);
        if (overridden) continue;
        buf_putc(&u, sep);
        sep = '&';
        url_encode(&u, c->q.k[i], false);
        buf_putc(&u, '=');
        url_encode(&u, c->q.v[i], false);
    }
    for (int k = 0; k < n; k++) {
        if (!vals[k]) continue;
        buf_putc(&u, sep);
        sep = '&';
        url_encode(&u, keys[k], false);
        buf_putc(&u, '=');
        url_encode(&u, vals[k], false);
    }
    buf_put(out, u.p, u.len);
    buf_free(&u);
}

const char *ctx_live(const Ctx *c, const char *key) {
    if (!c->client) return "";
    for (int i = 0; i < LIVE_SLOTS; i++)
        if (str_eq(c->client->live[i].k, key)) return c->client->live[i].v;
    return "";
}

void ctx_set_live(Ctx *c, const char *key, const char *val) {
    if (!c->client) return;
    int free_slot = -1;
    for (int i = 0; i < LIVE_SLOTS; i++) {
        if (str_eq(c->client->live[i].k, key)) {
            str_copy(c->client->live[i].v, sizeof c->client->live[i].v, val);
            return;
        }
        if (!c->client->live[i].k[0] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    str_copy(c->client->live[free_slot].k, sizeof c->client->live[free_slot].k, key);
    str_copy(c->client->live[free_slot].v, sizeof c->client->live[free_slot].v, val);
}

/* ------------------------------------------------------------ the viewer */

static int g_test_viewer[256]; /* app_ctx's stand-in sessions (tests): what each display account picked */

int ctx_viewer(const Ctx *c) {
    if (!c->me) return 0;
    if (c->me->role != ROLE_DISPLAY) return c->me->id;
    int v = c->sess ? c->sess->viewer : 0;
    return person_real(v) ? v : 0;
}

void ctx_set_viewer(Ctx *c, int person) {
    if (!c->me || c->me->role != ROLE_DISPLAY || !c->sess) return;
    int v = person_real(person) ? person : 0;
    Session *s = session_by_token(c->sess->token);
    if (!s) { /* not a stored session: the harness's */
        c->sess->viewer = v;
        if (c->me->id > 0 && c->me->id < countof(g_test_viewer)) g_test_viewer[c->me->id] = v;
        return;
    }
    Session rec = *s;
    rec.viewer = v;
    tbl_put(&T_session, &rec);
    c->sess = session_by_token(rec.token); /* rows move when the table grows */
}

/* ============================================================== presence */

#define MAX_HOME 64
typedef struct {
    int person;
    bool home;
    int64_t since;
} HomeState;
static HomeState g_home[MAX_HOME];
static int g_nhome;
static bool g_tracking;

int presence_screens(int person) {
    int n = 0;
    for (int i = 0; i < nclients; i++) {
        Session *s = session_by_token(clients[i]->token);
        if (s && s->person == person && clients[i]->ready) n++;
    }
    return n;
}

const char *presence_page(int person) {
    for (int i = nclients - 1; i >= 0; i--) {
        Session *s = session_by_token(clients[i]->token);
        if (!s || s->person != person || !clients[i]->ready) continue;
        Ctx tmp;
        ctx_init(&tmp, NULL, NULL, 0, clients[i]->url);
        const Module *m = module_for_path(tmp.path);
        ctx_free(&tmp);
        return m ? m->title : NULL;
    }
    return NULL;
}

void presence_set_home(int person, bool home, int64_t since) {
    g_tracking = true;
    for (int i = 0; i < g_nhome; i++) {
        if (g_home[i].person == person) {
            if (g_home[i].home != home) {
                g_home[i].home = home;
                g_home[i].since = since;
                app_changed();
            }
            return;
        }
    }
    if (g_nhome < MAX_HOME) {
        g_home[g_nhome++] = (HomeState){person, home, since};
        app_changed();
    }
}

bool presence_is_home(int person, int64_t *since) {
    for (int i = 0; i < g_nhome; i++) {
        if (g_home[i].person == person) {
            if (since) *since = g_home[i].since;
            return g_home[i].home;
        }
    }
    return false;
}

void presence_forget(int person) {
    int w = 0;
    for (int i = 0; i < g_nhome; i++)
        if (person && g_home[i].person != person) g_home[w++] = g_home[i];
    if (w == g_nhome) return;
    g_nhome = w;
    if (!w) g_tracking = false;
    app_changed();
}

bool presence_tracking(void) { return g_tracking; }

bool presence_known(int person) {
    for (int i = 0; i < g_nhome; i++)
        if (g_home[i].person == person) return true;
    return false;
}

static int (*g_guesser)(uint32_t ip);
void presence_set_guesser(int (*guess)(uint32_t ip)) { g_guesser = guess; }
int presence_guess(uint32_t ip) { return g_guesser && ip ? g_guesser(ip) : 0; }

/* ============================================================== settings */

static Setting *setting_row(const char *key) {
    for (int i = 0; i < tbl_count(&T_setting); i++) {
        Setting *s = tbl_at(&T_setting, i);
        if (str_eq(s->key, key)) return s;
    }
    return NULL;
}

const char *setting(const char *key, const char *dflt) {
    Setting *s = setting_row(key);
    return s ? s->val : dflt;
}

long long setting_int(const char *key, long long dflt) { return str_int_or(setting(key, NULL), dflt); }

void setting_set(const char *key, const char *val) {
    Setting *s = setting_row(key);
    if (s && str_eq(s->val, val)) return;
    Setting rec = s ? *s : (Setting){0};
    str_copy(rec.key, sizeof rec.key, key);
    str_copy(rec.val, sizeof rec.val, val);
    tbl_put(&T_setting, &rec);
}

void setting_set_int(const char *key, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    setting_set(key, tmp);
}

static Pref *pref_row(int person, const char *key) {
    for (int i = 0; i < tbl_count(&T_pref); i++) {
        Pref *p = tbl_at(&T_pref, i);
        if (p->person == person && str_eq(p->key, key)) return p;
    }
    return NULL;
}

const char *pref(int person, const char *key, const char *dflt) {
    Pref *p = pref_row(person, key);
    return p ? p->val : dflt;
}

void pref_set(int person, const char *key, const char *val) {
    Pref *p = pref_row(person, key);
    if (p && str_eq(p->val, val)) return;
    Pref rec = p ? *p : (Pref){0};
    rec.person = person;
    str_copy(rec.key, sizeof rec.key, key);
    str_copy(rec.val, sizeof rec.val, val);
    tbl_put(&T_pref, &rec);
}

/* ============================================================== activity */

void activity_add(int who, const char *icon, const char *link, const char *fmt, ...) {
    Activity a = {0};
    a.ts = now_sec();
    a.who = who;
    str_copy(a.icon, sizeof a.icon, icon);
    str_copy(a.link, sizeof a.link, link);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a.text, sizeof a.text, fmt, ap);
    va_end(ap);
    tbl_put(&T_activity, &a);
    while (tbl_count(&T_activity) > 300) tbl_del(&T_activity, ((Activity *)tbl_at(&T_activity, 0))->id);
}

/* ============================================================== rendering */

/* Renders the <div id="app"> element for the context's URL. Signed-out
 * visitors get the sign-in page wherever they are (except public modules). */
/* A display account (the living-room TV, a wall tablet) shows its own page at "/": the screen
   an admin gave it (pref PREF_START). The address stays "/", any query goes along. */
static void display_start(Ctx *c) {
    if (!c->me || c->me->role != ROLE_DISPLAY || !str_eq(c->path, "/")) return;
    const char *start = pref(c->me->id, PREF_START, "");
    if (!start[0]) start = setting(SETTING_DISPLAY_START, "");
    if (start[0] != '/' || start[1] == '/' || str_eq(start, "/") || strchr(start, '?')) return;
    const char *q = strchr(c->url, '?');
    char url[sizeof c->url];
    snprintf(url, sizeof url, "%s%s", start, q ? q : "");
    ctx_set_url(c, url);
    c->display_home = true;
}

static void render_app(Ctx *c, Buf *out) {
    display_start(c);
    ui_view_only(c->me && c->me->role == ROLE_DISPLAY); /* (its forms that only answer "view-only" stay out) */
    const Module *m = module_for_path(c->path);
    Buf page = {0};
    c->title[0] = '\0';
    c->bare = false;
    if (!c->me && !(m && (m->flags & MOD_PUBLIC))) {
        auth_page(c, &page);
        m = NULL;
    } else if (m && !module_visible(m, c)) { /* an admin's section: not there for anyone else */
        m = NULL;
        ctx_title(c, "Not found");
        ui_empty(&page, "🧭", "Nothing here", "That page doesn't exist (anymore).");
    } else if (!m && str_eq(c->path, "/more")) {
        ui_more_page(c, &page);
    } else if (m && m->page) {
        if (store_damaged() && is_admin(c)) /* say so where someone can act on it */
            buf_puts(&page, "<p class=\"notice bad\" role=\"alert\">Saving failed: the database file is damaged. Recent "
                            "changes are only in memory. Restart Hemma - it rescues everything it can still read.</p>");
        else if (store_failed() && is_admin(c)) /* e.g. a full disk */
            buf_puts(&page, "<p class=\"notice bad\" role=\"alert\">Saving to disk failed - the disk may be full. "
                            "Recent changes are only in memory and will be lost on restart; see the server log.</p>");
        else if (setting(SETTING_RESCUED, "")[0] && is_admin(c) && !str_starts(c->path, "/system"))
            buf_puts(&page, "<p class=\"notice bad\" role=\"alert\">The database was damaged, and Hemma rescued what it "
                            "could still read. <a href=\"/system\">See what happened</a></p>");
        m->page(c, &page);
    } else {
        ctx_title(c, "Not found");
        ui_empty(&page, "🧭", "Nothing here", "That page doesn't exist (anymore).");
    }
    if (!c->title[0] && m) str_copy(c->title, sizeof c->title, m->title);
    H(out, "<div id=\"app\" class=\"%s\" data-mod=\"%s\">", c->bare ? "bare" : "shell", m ? m->id : "auth");
    if (c->bare) buf_put(out, page.p, page.len);
    else ui_shell(c, out, m, page.p, page.len);
    buf_puts(out, "</div>");
    buf_free(&page);
    ui_view_only(false);
}

static void send_ops(Client *cl, const Buf *ops) {
    if (cl->conn && !cl->conn->dead) conn_ws_send(cl->conn, ops->p, ops->len);
}

static void full_title(const Ctx *c, Buf *b) {
    if (c->title[0] && !str_eq(c->title, app_name())) buf_printf(b, "%s · %s", c->title, app_name());
    else buf_puts(b, app_name());
}

static void client_render(Client *cl) {
    cl->dirty = false;
    g_renders++;
    Ctx c;
    client_ctx(cl, &c);
    Buf html = {0}, ops = {0}, title = {0};
    render_app(&c, &html);
    full_title(&c, &title);
    str_copy(cl->title, sizeof cl->title, buf_cstr(&title));
    uint64_t hh = hash64(html.p, html.len);
    uint64_t th = hash64(title.p, title.len) ^ hash64_str(cl->url);
    buf_putc(&ops, '[');
    bool any = false;
    if (th != cl->head_hash) {
        buf_puts(&ops, "[\"url\",");
        json_str(&ops, cl->url);
        buf_putc(&ops, ',');
        json_strn(&ops, title.p, title.len);
        buf_printf(&ops, ",%d]", cl->push ? 1 : 0);
        cl->head_hash = th;
        cl->push = false;
        any = true;
    }
    if (hh != cl->html_hash) {
        if (any) buf_putc(&ops, ',');
        buf_puts(&ops, "[\"morph\",");
        json_strn(&ops, html.p, html.len);
        buf_putc(&ops, ']');
        cl->html_hash = hh;
        any = true;
    }
    buf_putc(&ops, ']');
    if (any) send_ops(cl, &ops);
    buf_free(&html);
    buf_free(&ops);
    buf_free(&title);
    ctx_free(&c);
}

static void flush_dirty(void) {
    for (int i = 0; i < nclients; i++) {
        Client *cl = clients[i];
        if (all_dirty) cl->dirty = true;
        if (cl->ready && cl->dirty && cl->conn && !cl->conn->dead) client_render(cl);
    }
    all_dirty = false;
}

void app_notify(const Ctx *except, int kind, const char *fmt, ...) {
    char text[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    Buf ops = {0};
    buf_printf(&ops, "[[\"toast\",%d,", kind);
    json_str(&ops, text);
    buf_puts(&ops, "]]");
    int skip = except && except->me ? except->me->id : -1;
    for (int i = 0; i < nclients; i++) {
        Session *s = session_by_token(clients[i]->token);
        /* household news is for signed-in screens only (not the sign-in page, not revoked sessions) */
        if (!clients[i]->ready || !s || !person_get(s->person) || s->person == skip) continue;
        send_ops(clients[i], &ops);
    }
    buf_free(&ops);
}

void app_notify_person(int person, int kind, const char *fmt, ...) {
    char text[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    Buf ops = {0};
    buf_printf(&ops, "[[\"toast\",%d,", kind);
    json_str(&ops, text);
    buf_puts(&ops, "]]");
    for (int i = 0; i < nclients; i++) {
        Session *s = session_by_token(clients[i]->token);
        if (clients[i]->ready && s && s->person == person) send_ops(clients[i], &ops);
    }
    buf_free(&ops);
}

/* ================================================================= agenda */

void agenda_add(Agenda *a, const AgendaItem *it) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->items = xrealloc(a->items, (size_t)a->cap * sizeof *a->items);
    }
    a->items[a->n++] = *it;
}

static int agenda_order(const void *x, const void *y) {
    const AgendaItem *a = x, *b = y;
    if (a->allday != b->allday && day_start(a->start) == day_start(b->start)) return a->allday ? -1 : 1;
    return a->start < b->start ? -1 : a->start > b->start ? 1 : strcmp(a->title, b->title);
}

void agenda_collect(Ctx *c, int64_t from, int64_t to, const char *except, Agenda *out) {
    for (int i = 0; i < nmods; i++) { /* (an admin section's times are for admins only) */
        if (!mods[i]->agenda || (except && str_eq(mods[i]->id, except)) || !module_visible(mods[i], c)) continue;
        int before = out->n;
        mods[i]->agenda(c, from, to, out);
        for (int k = before; k < out->n; k++) out->items[k].module = mods[i]->id;
    }
    if (out->n > 1) qsort(out->items, (size_t)out->n, sizeof *out->items, agenda_order);
}

void agenda_free(Agenda *a) {
    free(a->items);
    a->items = NULL;
    a->n = a->cap = 0;
}

/* ================================================================ actions */

/* Runs an action; returns false if it doesn't exist. Signed-in people may
 * act, except read-only "display" accounts (wall tablets). *ran (may be NULL)
 * says whether it actually ran: only then can other screens need a re-render. */
static bool run_action(Ctx *c, const char *name, Fields *f, bool *ran, bool live) {
    const Module *owner = NULL;
    const Action *a = action_find(name, &owner);
    if (ran) *ran = false;
    if (!a || (live && !a->live)) return false; /* a "live" message can't run a change: issue #8 */
    if (!c->me) {
        ctx_fail(c, "Sign in first");
        return true;
    }
    if (c->me->role == ROLE_DISPLAY && !a->display) { /* a TV may only watch (Action.display) */
        ctx_fail(c, "This screen is view-only");
        return true;
    }
    int me = c->me->id;
    db_begin(); /* all writes of one action land atomically */
    a->run(c, f);
    db_commit();
    c->me = person_get(me); /* rows move when a table grows (adding a person): look ourselves up again */
    if (ran) *ran = true;
    return true;
}

static void ws_action(Client *cl, const char *name, Fields *f, long long id, bool live) {
    Ctx c;
    client_ctx(cl, &c);
    bool ran = false;
    if (!run_action(&c, name, f, &ran, live)) {
        if (live) { /* not a preview: nothing ran, nothing to say (wire.js only sends previews) */
            ctx_free(&c);
            return;
        }
        ctx_fail(&c, "Unknown action \"%s\"", name);
    }
    Buf ops = {0};
    buf_putc(&ops, '[');
    if (!live) buf_printf(&ops, "[\"ack\",%lld,%s]", id, c.failed ? "false" : "true");
    if (c.toast[0]) {
        if (!live) buf_putc(&ops, ',');
        buf_printf(&ops, "[\"toast\",%d,", c.toast_kind);
        json_str(&ops, c.toast);
        buf_putc(&ops, ']');
    }
    buf_putc(&ops, ']');
    if (ops.len > 2) send_ops(cl, &ops);
    buf_free(&ops);
    if (c.redirect[0]) {
        str_copy(cl->url, sizeof cl->url, c.redirect);
        cl->push = false;
    }
    cl->dirty = true;
    if (!live && ran) app_changed(); /* refused (signed out, view-only) or unknown: nothing changed */
    ctx_free(&c);
}

/* ============================================================= websocket */

static const Module *module_for_url(const char *url) {
    char raw[300], path[300];
    str_ncopy(raw, sizeof raw, url, MIN(strcspn(url, "?"), sizeof raw - 1));
    url_decode(raw, strlen(raw), path, false);
    return module_for_path(path);
}

/* A signed-in person's screen counts for presence (ready, session still valid). */
static bool client_present(const Client *cl) {
    Session *s = cl->ready ? session_by_token(cl->token) : NULL;
    return s && person_get(s->person);
}

/* Someone moved to another section: only the pages about who's around (the home dashboard
   says where people are with presence_page; the household page) need a fresh look - not
   every screen. Other pages don't show where people are (the shell's "Here now" doesn't). */
static void presence_moved(void) {
    for (int i = 0; i < nclients; i++) {
        const char *u = clients[i]->url;
        if (str_eq(u, "/") || str_starts(u, "/?") || str_eq(u, "/people") || str_starts(u, "/people?"))
            clients[i]->dirty = true;
    }
}

/* This screen asked for a render of its own (nav, a repeated hello): now, or - past its
   budget - on the next tick. */
static void client_wants_render(Client *cl) {
    int64_t now = now_ms(), tat = MAX(cl->nav_tat, now);
    if (tat - now >= NAV_BURST_MS) {
        cl->late = true;
        return;
    }
    cl->nav_tat = tat + NAV_COST_MS;
    cl->dirty = true;
}

/* How much of a browser's own trouble goes in the log (issue #79: the cap was per connection,
   so a reload or a reconnect started it again - a TV that can't play a channel, or any signed-in
   account in a loop, could push the lines an admin needs out of the log). Per person, an hour
   at a time: BROWSER_LOGS lines; a line the same as that person's last one is only counted;
   what was left out is said with the next line after the hour. */
#define BROWSER_LOGS 20
#define BROWSER_HOUR 3600
typedef struct {
    int person;
    int64_t since; /* the hour started then */
    int lines, left_out;
    uint64_t last; /* hash of the last line logged */
} BrowserLog;
static BrowserLog g_blog[32];

static bool browser_log_ok(int person, const char *who, const char *m, char *note, size_t cap) {
    int64_t now = now_sec();
    BrowserLog *b = NULL, *oldest = &g_blog[0];
    for (int i = 0; i < countof(g_blog) && !b; i++) {
        if (g_blog[i].person == person) b = &g_blog[i];
        else if (g_blog[i].since < oldest->since) oldest = &g_blog[i];
    }
    if (!b) {
        b = oldest; /* (a person whose hour is long over, or nobody yet) */
        *b = (BrowserLog){.person = person, .since = now};
    }
    if (now - b->since >= BROWSER_HOUR) {
        if (b->left_out) {
            char hm[6];
            fmt_hm(b->since, hm);
            log_warn("browser of %s: %d more line%s since %s left out (repeats, or over %d an hour)", who, b->left_out,
                     b->left_out == 1 ? "" : "s", hm, BROWSER_LOGS);
        }
        *b = (BrowserLog){.person = person, .since = now};
    }
    uint64_t h = hash64_str(m);
    if (h == b->last || b->lines >= BROWSER_LOGS) {
        b->left_out++;
        return false;
    }
    b->last = h;
    if (++b->lines == BROWSER_LOGS) {
        char hm[6];
        fmt_hm(b->since + BROWSER_HOUR, hm);
        snprintf(note, cap, " (no more from %s's browsers until %s)", who, hm);
    }
    return true;
}

/* A browser's user agent goes with its first line only (per session; the last 64 remembered). */
static int g_ua_said[64], g_ua_next;
static bool ua_first(int session) {
    for (int i = 0; i < countof(g_ua_said); i++)
        if (g_ua_said[i] == session) return false;
    g_ua_said[g_ua_next++ % countof(g_ua_said)] = session;
    return true;
}

static void browser_log_reset(void) {
    memset(g_blog, 0, sizeof g_blog);
    memset(g_ua_said, 0, sizeof g_ua_said);
    g_ua_next = 0;
}

static void on_ws_message(Conn *conn, const char *msg, size_t n) {
    Client *cl = conn->user;
    if (!cl) return;
    Session *seen = session_by_token(cl->token);
    if (seen) seen->seen = now_sec(); /* a live tab counts as activity */
    static JsonTok tok[600];
    Json j;
    if (!json_parse(&j, msg, n, tok, countof(tok)) || json_type(&j, 0) != JSON_OBJECT) return;
    char t[16], url[600];
    json_string(&j, json_get(&j, 0, "t"), t, sizeof t);
    if (!json_string(&j, json_get(&j, 0, "url"), url, sizeof url) || url[0] != '/') str_copy(url, sizeof url, "/");

    /* Only this screen re-renders for hello and nav (review: a signed-out socket used to
       re-render every screen in the house); everyone else only when presence changes. */
    if (str_eq(t, "hello")) {
        char v[40] = "", h[24] = "";
        json_string(&j, json_get(&j, 0, "v"), v, sizeof v);
        if (v[0] && !str_eq(v, g_asset_ver)) {
            Buf r = {0};
            buf_puts(&r, "[[\"reload\"]]");
            send_ops(cl, &r);
            buf_free(&r);
            return;
        }
        bool first = !cl->ready;
        if (!str_eq(cl->url, url)) memset(cl->live, 0, sizeof cl->live);
        str_copy(cl->url, sizeof cl->url, url);
        cl->ready = true;
        /* The tab may hold a stale page: send everything - unless it says which page it holds
           (the data-h stamp of the HTML it just loaded, see ui_document): then only what differs. */
        cl->html_hash = cl->head_hash = 0;
        json_string(&j, json_get(&j, 0, "h"), h, sizeof h);
        if (first && strlen(h) == 16 && strspn(h, "0123456789abcdef") == 16) cl->html_hash = strtoull(h, NULL, 16);
        if (first && client_present(cl)) app_changed(); /* a screen opened: presence for everyone */
        else client_wants_render(cl);
    } else if (str_eq(t, "nav")) {
        bool pop = json_type(&j, json_get(&j, 0, "pop")) == JSON_TRUE ||
                   json_type(&j, json_get(&j, 0, "pop")) == JSON_NUMBER;
        if (str_eq(cl->url, url)) {
            /* already there (a tap on the current tab, or the wire catching up after sleep):
               nothing to render - a push just scrolls back to the top */
            if (!pop && cl->ready && cl->title[0]) {
                Buf r = {0};
                buf_puts(&r, "[[\"url\",");
                json_str(&r, cl->url);
                buf_putc(&r, ',');
                json_str(&r, cl->title);
                buf_puts(&r, ",1]]");
                send_ops(cl, &r);
                buf_free(&r);
            }
            return;
        }
        bool moved = client_present(cl) && module_for_url(cl->url) != module_for_url(url);
        memset(cl->live, 0, sizeof cl->live); /* leaving a page clears its live previews */
        str_copy(cl->url, sizeof cl->url, url);
        cl->push = !pop;
        cl->head_hash = 0;
        client_wants_render(cl);
        if (moved && !cl->late) presence_moved();
    } else if (str_eq(t, "log")) {
        /* The browser's own trouble (wire.js: a script error, a video that won't play): a TV's
           browser has no console anyone can read, so it goes in the log - the System page.
           Signed-in screens only, within browser_log_ok()'s limits. */
        char m[420], ua[200] = "";
        const Session *s = session_by_token(cl->token);
        const Person *p = s ? person_get(s->person) : NULL;
        if (!p || !json_string(&j, json_get(&j, 0, "m"), m, sizeof m)) return;
        json_string(&j, json_get(&j, 0, "ua"), ua, sizeof ua);
        const char *who = p->username[0] ? p->username : p->name;
        char note[80] = "";
        if (!browser_log_ok(p->id, who, m, note, sizeof note)) return;
        bool first = ua[0] && ua_first(s->id);
        log_warn("browser of %s (%s) on %s: %s%s%s%s", who, conn->peer, url, utf8_scrub(m), first ? " - " : "",
                 first ? utf8_scrub(ua) : "", note);
    } else if (str_eq(t, "act") || str_eq(t, "live")) {
        char a[64];
        json_string(&j, json_get(&j, 0, "a"), a, sizeof a);
        long long id = 0;
        json_integer(&j, json_get(&j, 0, "id"), &id);
        Fields f = {0};
        int arr = json_get(&j, 0, "f");
        for (int k = json_child(&j, arr); k >= 0; k = json_next(&j, arr, k)) {
            char key[64], val[4096];
            if (json_string(&j, json_index(&j, k, 0), key, sizeof key) &&
                json_string(&j, json_index(&j, k, 1), val, sizeof val))
                fields_add(&f, utf8_scrub(key), utf8_scrub(val));
        }
        str_copy(cl->url, sizeof cl->url, url);
        ws_action(cl, a, &f, id, str_eq(t, "live"));
        fields_free(&f);
    }
}

static void on_ws_closed(Conn *conn) {
    Client *cl = conn->user;
    if (!cl) return;
    conn->user = NULL;
    bool present = client_present(cl);
    client_remove(cl);
    if (present) { /* a screen closed: presence for everyone (strangers' screens don't count) */
        app_changed();
        app_wake_in(0); /* this runs before the loop's poll: don't let the update wait for the next tick */
    }
}

/* ================================================================== http */

static bool origin_ok(const HttpReq *r) { return http_same_origin(r); }

/* ---- dev mode: web/ and each modules/<name>/<file>.css straight from disk ---- */

typedef struct {
    char path[400];
    int64_t mtime, size;
} DevFile;

static int devfile_cmp(const void *a, const void *b) {
    return strcmp(((const DevFile *)a)->path, ((const DevFile *)b)->path);
}

static void dev_scan(const char *dir, const char *ext, DevFile *out, int *n, int max) {
    DirIter *it = dir_open(dir);
    DirEntry e;
    while (it && dir_next(it, &e) && *n < max) {
        if (e.is_dir || (ext && !str_iends(e.name, ext))) continue;
        snprintf(out[*n].path, sizeof out[*n].path, "%s/%s", dir, e.name);
        out[*n].mtime = e.mtime;
        out[*n].size = e.size;
        (*n)++;
    }
    dir_close(it);
}

/* The CSS of the modules compiled into this binary, in a stable order (the
 * same order build.ps1 bundles them). Other folders — e.g. a module someone
 * else is still building — are ignored. */
static int dev_module_css(DevFile *out, int max) {
    int n = 0;
    DirIter *it = dir_open("modules");
    DirEntry e;
    while (it && dir_next(it, &e)) {
        if (!e.is_dir || !module_find(e.name)) continue;
        char dir[300];
        snprintf(dir, sizeof dir, "modules/%s", e.name);
        dev_scan(dir, ".css", out, &n, max);
    }
    dir_close(it);
    qsort(out, (size_t)n, sizeof *out, devfile_cmp);
    return n;
}

static void dev_css(Buf *out) {
    Buf path = {0};
    buf_printf(&path, "%s/app.css", g_cfg.web_dir);
    file_read_all(buf_cstr(&path), out, 4u << 20);
    buf_free(&path);
    static DevFile files[128];
    int n = dev_module_css(files, countof(files));
    for (int i = 0; i < n; i++) {
        buf_printf(out, "\n/* ---- %s ---- */\n", files[i].path);
        file_read_all(files[i].path, out, 8u << 20);
    }
}

static const Asset *asset_find(const char *path) {
    for (int i = 0; i < g_nassets; i++)
        if (str_eq(g_assets[i].path, path)) return &g_assets[i];
    return NULL;
}

static void serve_static(Conn *conn, HttpReq *r) {
    const char *name = str_starts(r->path, "/static/") ? r->path + 7 : "/icon.svg"; /* "/app.css" */
    if (g_cfg.web_dir && str_eq(name, "/app.css")) {
        Buf css = {0};
        dev_css(&css);
        conn_reply(conn, 200, "text/css; charset=utf-8", css.p, css.len, "Cache-Control: no-store\r\n");
        buf_free(&css);
        return;
    }
    if (g_cfg.web_dir) {
        for (const char *p = name; *p; p++) {
            bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                      strchr("/._-", *p);
            if (!ok || (p[0] == '.' && p[1] == '.')) {
                conn_reply(conn, 404, "text/plain", "not found\n", 10, NULL);
                return;
            }
        }
        Buf path = {0}, data = {0};
        buf_printf(&path, "%s%s", g_cfg.web_dir, name);
        bool found = file_read_all(buf_cstr(&path), &data, 8u << 20);
        if (found) conn_reply(conn, 200, http_mime(name), data.p, data.len, "Cache-Control: no-store\r\n");
        buf_free(&path);
        buf_free(&data);
        if (found || !asset_find(name)) { /* built files (av.wasm) aren't in web/: they come from the binary */
            if (!found) conn_reply(conn, 404, "text/plain", "not found\n", 10, NULL);
            return;
        }
    }
    const Asset *a = str_iends(name, ".gz") ? NULL : asset_find(name); /* (twins aren't asked for by name) */
    if (!a) {
        conn_reply(conn, 404, "text/plain", "not found\n", 10, NULL);
        return;
    }
    /* the build gzips text files and av.wasm ("/app.css.gz"): ~120 KB -> ~30 KB for a first visit */
    const Asset *gz = NULL;
    const char *ae = http_header(r, "Accept-Encoding");
    if (ae && strstr(ae, "gzip")) {
        char twin[300];
        snprintf(twin, sizeof twin, "%s.gz", name);
        gz = asset_find(twin);
    }
    const Asset *body = gz ? gz : a;
    char etag[24];
    snprintf(etag, sizeof etag, "\"%s\"", body->etag);
    const char *inm = http_header(r, "If-None-Match");
    Buf extra = {0};
    buf_printf(&extra, "ETag: %s\r\nCache-Control: %s\r\nVary: Accept-Encoding\r\n%s", etag,
               strstr(r->query, "v=") ? "public, max-age=31536000, immutable" : "no-cache",
               gz ? "Content-Encoding: gzip\r\n" : "");
    if (inm && str_eq(inm, etag)) conn_reply(conn, 304, NULL, "", 0, extra.p);
    else conn_reply(conn, 200, a->type, body->data, body->len, buf_cstr(&extra));
    buf_free(&extra);
}

/* flash messages for the no-JS path (POST -> 303 -> GET) */
static struct {
    char token[65];
    int kind;
    char text[240];
} g_flash[16];

static void flash_set(const char *token, int kind, const char *text) {
    static int next;
    int slot = next++ % countof(g_flash);
    for (int i = 0; i < countof(g_flash); i++)
        if (str_eq(g_flash[i].token, token)) slot = i;
    str_copy(g_flash[slot].token, sizeof g_flash[slot].token, token);
    g_flash[slot].kind = kind;
    str_copy(g_flash[slot].text, sizeof g_flash[slot].text, text);
}

static bool flash_take(const char *token, int *kind, char *out, size_t cap) {
    for (int i = 0; i < countof(g_flash); i++) {
        if (g_flash[i].token[0] && str_eq(g_flash[i].token, token)) {
            *kind = g_flash[i].kind;
            str_copy(out, cap, g_flash[i].text);
            g_flash[i].token[0] = '\0';
            return true;
        }
    }
    return false;
}

static void same_origin_path(const char *referer, char *out, size_t cap) {
    /* "http://host:8080/book?x" -> "/book?x" */
    const char *p = referer ? strstr(referer, "://") : NULL;
    p = p ? strchr(p + 3, '/') : NULL;
    str_copy(out, cap, p && p[1] != '/' ? p : "/");
}

static void reply_json(Conn *conn, int status, Buf *body) {
    conn_reply(conn, status, "application/json; charset=utf-8", body->p, body->len, "Cache-Control: no-store\r\n");
}

/* Rows of a table with an owner (preferences, watch history): members see their own, admins all. */
static bool api_may_see(const Ctx *c, const Table *t, const void *row) {
    if (!t->api_owner || is_admin(c)) return true;
    for (int i = 0; i < t->nfields; i++) {
        const FieldDef *fd = &t->fields[i];
        if (fd->type == F_INT && str_eq(fd->name, t->api_owner))
            return *(const int *)((const char *)row + fd->offset) == c->me->id;
    }
    return false; /* a misnamed owner field: show nothing rather than everything */
}

static void api(Ctx *c, Conn *conn, HttpReq *r) {
    Buf out = {0};
    const char *rest = r->path + 4; /* after "/api" */
    if (*rest == '/') rest++;
    if (!c->me) {
        buf_puts(&out, "{\"error\":\"sign in first\"}");
        reply_json(conn, 401, &out);
    } else if (c->me->role == ROLE_DISPLAY) { /* a wall tablet only looks at its own screens (review) */
        buf_puts(&out, "{\"error\":\"display accounts can't use the API\"}");
        reply_json(conn, 403, &out);
    } else if (str_eq(r->method, "POST")) {
        Fields f = {0};
        if (r->body_len && r->body[0] == '{') {
            JsonTok tok[256];
            Json j;
            if (json_parse(&j, r->body, r->body_len, tok, countof(tok)) && json_type(&j, 0) == JSON_OBJECT) {
                for (int k = json_child(&j, 0); k >= 0; k = json_next(&j, 0, k)) {
                    char key[64], val[4096];
                    json_string(&j, k, key, sizeof key);
                    if (json_type(&j, k + 1) == JSON_STRING) json_string(&j, k + 1, val, sizeof val);
                    else str_ncopy(val, sizeof val, j.src + j.tok[k + 1].start, (size_t)j.tok[k + 1].len);
                    fields_add(&f, utf8_scrub(key), utf8_scrub(val));
                }
            }
        } else {
            fields_parse(&f, r->body, r->body_len);
        }
        if (!origin_ok(r)) {
            buf_puts(&out, "{\"error\":\"cross-site request refused\"}");
            reply_json(conn, 403, &out);
        } else if (!run_action(c, rest, &f, NULL, false)) {
            buf_puts(&out, "{\"error\":\"no such action\"}");
            reply_json(conn, 404, &out);
        } else {
            buf_printf(&out, "{\"ok\":%s,\"message\":", c->failed ? "false" : "true");
            json_str(&out, c->toast);
            buf_puts(&out, ",\"redirect\":");
            json_str(&out, c->redirect);
            buf_putc(&out, '}');
            reply_json(conn, c->failed ? 422 : 200, &out);
            if (!c->failed) app_changed();
        }
        fields_free(&f);
    } else if (!*rest) {
        Table **ts;
        int nt = store_tables(&ts);
        buf_puts(&out, "{\"tables\":[");
        for (int i = 0, k = 0; i < nt; i++) {
            if (ts[i]->api_hidden) continue;
            if (k++) buf_putc(&out, ',');
            json_str(&out, ts[i]->name);
        }
        buf_puts(&out, "],\"actions\":[");
        int k = 0;
        for (int i = 0; i < nmods; i++) /* an admin section's actions are listed for admins only */
            for (const Action *a = module_visible(mods[i], c) ? mods[i]->actions : NULL; a && a->name; a++) {
                if (k++) buf_putc(&out, ',');
                json_str(&out, a->name);
            }
        buf_puts(&out, "]}");
        reply_json(conn, 200, &out);
    } else {
        char name[64];
        size_t nl = strcspn(rest, "/");
        str_ncopy(name, sizeof name, rest, nl);
        Table *t = store_table(name);
        long long id = rest[nl] == '/' ? str_int_or(rest + nl + 1, -1) : 0;
        if (!t || t->api_hidden || id < 0) {
            buf_puts(&out, "{\"error\":\"not found\"}");
            reply_json(conn, 404, &out);
        } else if (id) {
            void *row = tbl_get(t, (int)id);
            if (row && !api_may_see(c, t, row)) row = NULL;
            if (row) tbl_to_json(t, row, &out);
            else buf_puts(&out, "{\"error\":\"not found\"}");
            reply_json(conn, row ? 200 : 404, &out);
        } else {
            buf_putc(&out, '[');
            for (int i = 0, k = 0; i < tbl_count(t); i++) {
                if (!api_may_see(c, t, tbl_at(t, i))) continue;
                if (k++) buf_putc(&out, ',');
                tbl_to_json(t, tbl_at(t, i), &out);
            }
            buf_putc(&out, ']');
            reply_json(conn, 200, &out);
        }
    }
    buf_free(&out);
}

static void on_http(Conn *conn, HttpReq *r) {
    if (!http_host_ok(r, g_extra_hosts)) { /* DNS rebinding: someone else's name pointing at us */
        static const char msg[] = "Hemma doesn't answer to this host name.\nOpen it by its address "
                                  "(like http://192.168.0.10:8080), or add the name to HEMMA_HOSTS.\n";
        conn->keep_alive = false;
        conn_reply(conn, 403, "text/plain; charset=utf-8", msg, sizeof msg - 1, NULL);
        return;
    }
    if (!r->streaming && str_eq(r->path, "/healthz")) { /* scripts and the installer: "is it up?" - nothing more */
        if (str_eq(r->method, "GET") || str_eq(r->method, "HEAD"))
            conn_reply(conn, 200, "text/plain; charset=utf-8", "ok\n", 3, "Cache-Control: no-store\r\n");
        else conn_reply(conn, 405, "text/plain", "GET only\n", 9, "Allow: GET, HEAD\r\n");
        return;
    }
    if (!r->streaming && (str_starts(r->path, "/static/") || str_eq(r->path, "/favicon.ico"))) {
        serve_static(conn, r);
        return;
    }

    /* the cookie only ever exists after signing in; we look it up by its hash */
    char sid[40] = "", token[65] = "";
    if (http_cookie(r, "hemma", sid, sizeof sid)) sid_token(sid, token);
    Session *live = session_by_token(token);
    if (live) live->seen = now_sec(); /* memory only; persisted hourly */

    if (r->streaming) {
        /* A body too big to buffer (an upload): only a module endpoint that reads
           it straight from the socket may take it. The server closes otherwise. */
        Ctx c;
        ctx_init(&c, session_for(token), NULL, conn->ip, r->target);
        bool taken = false;
        for (int i = 0; i < nmods && !taken; i++)
            if (mods[i]->http && (c.me || (mods[i]->flags & MOD_PUBLIC)) && module_visible(mods[i], &c))
                taken = mods[i]->http(&c, conn, r);
        if (!taken) {
            static const char msg[] = "request body too large\n";
            conn->keep_alive = false;
            conn_reply(conn, 413, "text/plain; charset=utf-8", msg, sizeof msg - 1, NULL);
        }
        ctx_free(&c);
        return;
    }

    /* No session cookie, but an address key (?k=, ctx_url_key) for exactly this path: a module
       endpoint answers as that session - a TV's video player that fetches without cookies. Nothing
       else does: an address no endpoint takes goes on as a signed-out request. */
    if (!live && (str_eq(r->method, "GET") || str_eq(r->method, "HEAD"))) {
        Session *keyed = session_by_url_key(r, conn->ip);
        if (keyed) {
            Ctx c;
            ctx_init(&c, keyed, NULL, conn->ip, r->target);
            bool taken = false;
            for (int i = 0; i < nmods && !taken; i++)
                if (mods[i]->http && c.me && module_visible(mods[i], &c)) taken = mods[i]->http(&c, conn, r);
            ctx_free(&c);
            if (taken) return;
        }
    }

    if (str_eq(r->path, "/ws")) {
        if (!origin_ok(r)) {
            conn_reply(conn, 403, "text/plain", "forbidden\n", 10, NULL);
        } else if (!live && !stranger_room(conn->ip)) {
            static const char msg[] = "too many signed-out screens\n";
            conn->keep_alive = false; /* the page still works: signing in is a plain POST */
            conn_reply(conn, 503, "text/plain", msg, sizeof msg - 1, "Retry-After: 60\r\n");
        } else if (conn_ws_upgrade(conn, r)) {
            client_new(conn, live ? token : "");
        }
        return;
    }

    Ctx c;
    ctx_init(&c, session_for(token), NULL, conn->ip, r->target);

    if (auth_http(&c, conn, r)) {
        ctx_free(&c);
        return;
    }

    if (str_starts(r->path, "/a/")) {
        char back[600];
        same_origin_path(http_header(r, "Referer"), back, sizeof back);
        if (!str_eq(r->method, "POST")) {
            conn_reply(conn, 405, "text/plain", "POST only\n", 10, "Allow: POST\r\n");
        } else if (!origin_ok(r)) {
            conn_reply(conn, 403, "text/plain", "cross-site request refused\n", 27, NULL);
        } else {
            Fields f = {0};
            fields_parse(&f, r->body, r->body_len);
            ctx_set_url(&c, back);
            bool ran = false;
            if (!run_action(&c, r->path + 3, &f, &ran, false)) ctx_fail(&c, "Unknown action");
            if (c.toast[0] && token[0]) flash_set(token, c.toast_kind, c.toast);
            Buf extra = {0};
            buf_printf(&extra, "Location: %s\r\n", c.redirect[0] ? c.redirect : back);
            conn_reply(conn, 303, "text/plain", "", 0, extra.p);
            buf_free(&extra);
            fields_free(&f);
            if (ran) app_changed(); /* refused or unknown: no other screen needs a look */
        }
        ctx_free(&c);
        return;
    }

    if (str_eq(r->path, "/api") || str_starts(r->path, "/api/")) {
        api(&c, conn, r);
        ctx_free(&c);
        return;
    }

    /* raw module endpoints (streams, images, ...) are for signed-in people only,
       unless the module is public */
    for (int i = 0; i < nmods; i++) {
        if (!mods[i]->http || (!c.me && !(mods[i]->flags & MOD_PUBLIC)) || !module_visible(mods[i], &c)) continue;
        if (mods[i]->http(&c, conn, r)) {
            ctx_free(&c);
            return;
        }
    }

    if (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD")) {
        conn_reply(conn, 405, "text/plain", "method not allowed\n", 19, "Allow: GET\r\n");
        ctx_free(&c);
        return;
    }
    Buf app = {0}, doc = {0}, extra = {0};
    render_app(&c, &app);
    char flash[240];
    int fkind = 0;
    bool has_flash = token[0] && flash_take(token, &fkind, flash, sizeof flash);
    ui_document(&c, &doc, app.p, app.len, has_flash ? flash : NULL, fkind);
    buf_puts(&extra, "Cache-Control: no-store\r\n");
    conn_reply(conn, 200, "text/html; charset=utf-8", doc.p, doc.len, extra.p); /* HEAD: the server drops the body */
    buf_free(&app);
    buf_free(&doc);
    buf_free(&extra);
    ctx_free(&c);
}

/* ============================================================== lifecycle */

static void prune_sessions(void);

static void compute_asset_version(void) {
    uint64_t h = 14695981039346656037ULL;
    if (g_cfg.web_dir) {
        static DevFile files[192];
        int n = dev_module_css(files, countof(files));
        dev_scan(g_cfg.web_dir, NULL, files, &n, countof(files));
        for (int i = 0; i < n; i++) {
            h = (h ^ hash64_str(files[i].path)) * 1099511628211ULL;
            h = (h ^ (uint64_t)files[i].mtime ^ ((uint64_t)files[i].size << 20)) * 1099511628211ULL;
        }
    } else {
        for (int i = 0; i < g_nassets; i++) h = (h ^ hash64_str(g_assets[i].etag)) * 1099511628211ULL;
    }
    char v[17];
    hex_encode(&h, 6, v);
    if (g_asset_ver[0] && !str_eq(v, g_asset_ver)) {
        log_info("web assets changed - reloading open screens");
        Buf r = {0};
        buf_puts(&r, "[[\"reload\"]]");
        for (int i = 0; i < nclients; i++) send_ops(clients[i], &r);
        buf_free(&r);
    }
    str_copy(g_asset_ver, sizeof g_asset_ver, v);
}

/* The loop sleeps until there is work: about once a second while someone has
   a screen open (clocks, live state), every 5 s when nobody does, and sooner
   only when a module asks for it (app_wake_in) - e.g. a discovery sweep pacing
   its packets. An idle Pi then wakes 12 times a minute instead of 600. */
static int64_t g_tick_due, g_wake_at;

void app_wake_in(int ms) {
    int64_t at = now_ms() + MAX(ms, 0);
    if (!g_wake_at || at < g_wake_at) g_wake_at = at;
}

/* Any thread: tick right away - a worker finished something the screens should show
   (an upload, a scan) instead of it waiting for the next idle tick (up to 5 s). */
static atomic_bool g_tick_now;
void app_tick_soon(void) {
    atomic_store(&g_tick_now, true);
    server_wake();
}

static int64_t on_next_tick(int64_t now) {
    if (atomic_load(&g_tick_now)) return now;
    return g_wake_at && g_wake_at < g_tick_due ? g_wake_at : g_tick_due;
}

static void on_tick(int64_t now) {
    static int64_t last_minute, last_assets, last_compact;
    atomic_store(&g_tick_now, false);
    g_wake_at = 0;
    g_tick_due = now + (nclients ? 1000 : 5000);
    auth_tick();
    for (int i = 0; i < nmods; i++)
        if (mods[i]->tick) mods[i]->tick(now);
    bool moved = false;
    for (int i = 0; i < nclients; i++) { /* screens that navigated past their budget catch up now */
        Client *cl = clients[i];
        if (!cl->late) continue;
        cl->late = false;
        cl->dirty = true;
        moved |= client_present(cl);
    }
    if (moved) presence_moved();
    int64_t minute = now_sec() / 60;
    if (minute != last_minute) {
        last_minute = minute;
        app_changed(); /* clocks, "5 min ago", the booking grid's now-line */
    }
    if (g_cfg.web_dir && now - last_assets > 1000) {
        last_assets = now;
        compute_asset_version();
    }
    if (now - last_compact > 60 * 60 * 1000) {
        last_compact = now;
        for (int i = 0; i < tbl_count(&T_session); i++) { /* persist "last seen" once an hour */
            Session s = *(Session *)tbl_at(&T_session, i);
            tbl_put(&T_session, &s);
        }
        prune_sessions();
        store_maintain();
    }
}

static void on_idle(void) {
    flush_dirty();
    db_idle();
}

/* Sessions that ended (session_over): their rows go. */
static void prune_sessions(void) {
    int64_t now = now_sec();
    for (int i = tbl_count(&T_session) - 1; i >= 0; i--) {
        Session *s = tbl_at(&T_session, i);
        if (session_over(s, now)) tbl_del(&T_session, s->id);
    }
}

bool app_boot(const char *data_dir) {
    static char dir[600];
    if (data_dir != dir) str_copy(dir, sizeof dir, data_dir ? data_dir : "data");
    g_cfg.data_dir = dir; /* app_config(): the folder in use (tests boot without app_main) */
    memset(g_test_viewer, 0, sizeof g_test_viewer); /* a fresh boot: no stand-in session picked anyone */
    g_nhome = 0;
    g_tracking = false;
    browser_log_reset();
    auth_reset_throttle();
    T_session.api_hidden = true;
    T_setting.api_hidden = true;
    T_pref.api_owner = "person";
    store_register(&T_person);
    store_register(&T_session);
    store_register(&T_setting);
    store_register(&T_pref);
    store_register(&T_activity);
    auth_init();
    sort_mods();
    for (int i = 0; i < nmods; i++)
        if (mods[i]->init) mods[i]->init();
    if (!store_open(data_dir)) return false;
    if (store_rescue_report()[0]) setting_set(SETTING_RESCUED, store_rescue_report()); /* (System: until seen) */
    if (!g_testing) {
        char why[300];
        int made = auth_ensure_secret(g_cfg.env_file, why, sizeof why);
        if (made > 0) log_info("%s", why);
        else if (made < 0) log_warn("%s", why);
        auth_configure(env("HEMMA_AUTH_SALT", NULL), (int)env_int("HEMMA_KDF_ITERATIONS", 120000));
        auth_bootstrap_admin();
    }
    auth_start(); /* password resets left by `hemma --reset-password` */
    str_copy(g_extra_hosts, sizeof g_extra_hosts, env("HEMMA_HOSTS", ""));
    prune_sessions();
    for (int i = 0; i < nmods; i++)
        if (mods[i]->start) mods[i]->start();
    return true;
}

void app_halt(void) {
    for (int i = 0; i < nmods; i++)
        if (mods[i]->stop) mods[i]->stop();
    store_close();
    g_nhome = 0;
    g_tracking = false;
}

void app_ctx(Ctx *c, int person, const char *url) {
    static Session s;
    memset(&s, 0, sizeof s);
    str_copy(s.token, sizeof s.token, "test");
    s.person = person;
    if (person > 0 && person < countof(g_test_viewer)) s.viewer = g_test_viewer[person];
    ctx_init(c, &s, NULL, 0x7F000001, url);
}

void app_ctx_free(Ctx *c) { ctx_free(c); }
void app_render(Ctx *c, Buf *out) { render_app(c, out); }

bool app_action(Ctx *c, const char *name, Fields *f) { return run_action(c, name, f, NULL, false); }

void app_serve(void) {
    if (!g_asset_ver[0]) compute_asset_version();
    static const ServerHooks hooks = {on_http, on_ws_message, on_ws_closed, on_tick, on_idle, on_next_tick};
    g_tick_due = 0;
    server_run(&hooks);
}

/* One block after the start that says it's working, where to open it, where the log is and how
   to stop it - for whoever is looking at the window, the journal or the installer's output. */
static void startup_banner(const char *where) {
    char addr[3][96];
    int na = app_addresses(addr, 3);
    bool local_only = app_local_only();
    const char *svc = plat_getenv("INVOCATION_ID"), *agent = plat_getenv("XPC_SERVICE_NAME");
    bool systemd = svc && *svc, launchd = agent && str_starts(agent, "local.hemma");
    bool log_file = module_find("system") != NULL; /* the System page keeps <data>/hemma.log */
    static const char rule[] = "------------------------------------------------------------";
    log_info("%s", rule);
    log_info("Hemma %s is running.", HEMMA_VERSION_FULL);
    if (local_only) log_info("  Open it on this computer: %s  (other devices can't: it listens on %s only)",
                             na ? addr[0] : where, g_cfg.host);
    else if (na) {
        log_info("  Open it on a phone or computer at home:  %s", addr[0]);
        for (int i = 1; i < na; i++) log_info("                                      or:  %s", addr[i]);
    } else log_info("  Open it at %s  (no home network found yet: check the cable or Wi-Fi)", where);
    if (auth_ready() && auth_setup_open())
        log_info("  First time? Open that address and type the setup code shown above.");
    char logs[400];
    snprintf(logs, sizeof logs, "%s%s%s%s", systemd ? "journalctl -u hemma -f" : launchd ? "its log file" : "this window",
             log_file ? ", " : "", log_file ? g_cfg.data_dir : "", log_file ? "/hemma.log and Hemma's System page" : "");
    log_info("  The log: %s", logs);
    log_info("  Stop it: %s", systemd   ? "sudo systemctl stop hemma  (restart: sudo systemctl restart hemma)"
                          : launchd ? "launchctl bootout gui/$(id -u)/local.hemma"
                                    : "Ctrl+C");
    log_info("%s", rule);
}

int app_main(const AppConfig *cfg) {
    g_cfg = *cfg;
    if (!app_boot(cfg->data_dir)) return 1;
    compute_asset_version();
    if (!server_listen(cfg->host, cfg->port)) {
        app_halt(); /* modules stop: the log file gets why (a port in use) - a service has no window */
        return 1;
    }

    char where[64] = "";
    uint32_t any = 0;
    char ip[16];
    listen_addr(cfg->host, &any); /* (the listener took it: it's valid) */
    LanIface ifc;
    if (any == 0 && lan_iface(&ifc)) ip_format(ifc.ip, ip);
    else ip_format(any ? any : 0x7F000001, ip);
    snprintf(where, sizeof where, "http://%s:%d", ip, server_port());
    log_info("%s is up on %s  (%d modules%s)", app_name(), where, nmods, cfg->web_dir ? ", dev mode" : "");
    if (!auth_ready())
        log_warn("HEMMA_AUTH_SALT is not set, so nobody can sign in or be created (see the line above; "
                 "hemma --gen-salt makes a value for the settings file)");
    else if (auth_setup_open()) {
        log_warn("No admin yet - open %s/ in a browser to create one", where);
        log_warn("The welcome page asks for this setup code: %s", auth_setup_code());
    }
    startup_banner(where);

    plat_on_quit(server_stop);
    app_serve();
    log_info("shutting down");
    server_shutdown();
    app_halt();
    return 0;
}
