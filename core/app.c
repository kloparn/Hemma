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
    FIELD(Session, remember, F_BOOL),
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

/* ================================================================ people */

Person *person_get(int id) { return id > 0 ? tbl_get(&T_person, id) : NULL; }
int account_count(void) { return tbl_count(&T_person); }
Person *account_at(int i) { return tbl_at(&T_person, i); }

int person_count(void) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_person); i++) n += ((Person *)tbl_at(&T_person, i))->role != ROLE_DISPLAY;
    return n;
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

static Session *session_by_token(const char *token) {
    if (!token || !token[0]) return NULL;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        if (strcmp(s->token, token) == 0) return s;
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
static void render_app(Ctx *c, Buf *out) {
    const Module *m = module_for_path(c->path);
    Buf page = {0};
    c->title[0] = '\0';
    c->bare = false;
    if (!c->me && !(m && (m->flags & MOD_PUBLIC))) {
        auth_page(c, &page);
        m = NULL;
    } else if (!m && str_eq(c->path, "/more")) {
        ui_more_page(c, &page);
    } else if (m && m->page) {
        if (store_failed() && is_admin(c)) /* e.g. a full disk: say so where someone can act on it */
            buf_puts(&page, "<p class=\"notice bad\" role=\"alert\">Saving to disk failed - the disk may be full. "
                            "Recent changes are only in memory and will be lost on restart; see the server log.</p>");
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
    for (int i = 0; i < nmods; i++) {
        if (!mods[i]->agenda || (except && str_eq(mods[i]->id, except))) continue;
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
static bool run_action(Ctx *c, const char *name, Fields *f, bool *ran) {
    const Module *owner = NULL;
    const Action *a = action_find(name, &owner);
    if (ran) *ran = false;
    if (!a) return false;
    if (!c->me) {
        ctx_fail(c, "Sign in first");
        return true;
    }
    if (c->me->role == ROLE_DISPLAY) {
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
    if (!run_action(&c, name, f, &ran)) ctx_fail(&c, "Unknown action \"%s\"", name);
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
        } else if (!run_action(c, rest, &f, NULL)) {
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
        for (int i = 0; i < nmods; i++)
            for (const Action *a = mods[i]->actions; a && a->name; a++) {
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
            if (row) tbl_to_json(t, row, &out);
            else buf_puts(&out, "{\"error\":\"not found\"}");
            reply_json(conn, row ? 200 : 404, &out);
        } else {
            buf_putc(&out, '[');
            for (int i = 0; i < tbl_count(t); i++) {
                if (i) buf_putc(&out, ',');
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
            if (mods[i]->http && (c.me || (mods[i]->flags & MOD_PUBLIC))) taken = mods[i]->http(&c, conn, r);
        if (!taken) {
            static const char msg[] = "request body too large\n";
            conn->keep_alive = false;
            conn_reply(conn, 413, "text/plain; charset=utf-8", msg, sizeof msg - 1, NULL);
        }
        ctx_free(&c);
        return;
    }

    if (str_eq(r->path, "/ws")) {
        if (!origin_ok(r)) conn_reply(conn, 403, "text/plain", "forbidden\n", 10, NULL);
        else if (conn_ws_upgrade(conn, r)) client_new(conn, live ? token : "");
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
            if (!run_action(&c, r->path + 3, &f, &ran)) ctx_fail(&c, "Unknown action");
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
        if (!mods[i]->http || (!c.me && !(mods[i]->flags & MOD_PUBLIC))) continue;
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

/* Sessions that ended long ago: unremembered ones after a week idle, the rest after a year. */
static void prune_sessions(void) {
    int64_t now = now_sec();
    for (int i = tbl_count(&T_session) - 1; i >= 0; i--) {
        Session *s = tbl_at(&T_session, i);
        int64_t idle = now - MAX(s->seen, s->created);
        if (!s->token[0] || !person_get(s->person) || idle > (s->remember ? 366 : 7) * 86400LL)
            tbl_del(&T_session, s->id);
    }
}

bool app_boot(const char *data_dir) {
    g_nhome = 0;
    g_tracking = false;
    auth_reset_throttle();
    T_session.api_hidden = true;
    T_setting.api_hidden = true;
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
    ctx_init(c, &s, NULL, 0x7F000001, url);
}

void app_ctx_free(Ctx *c) { ctx_free(c); }
void app_render(Ctx *c, Buf *out) { render_app(c, out); }

bool app_action(Ctx *c, const char *name, Fields *f) { return run_action(c, name, f, NULL); }

void app_serve(void) {
    if (!g_asset_ver[0]) compute_asset_version();
    static const ServerHooks hooks = {on_http, on_ws_message, on_ws_closed, on_tick, on_idle, on_next_tick};
    g_tick_due = 0;
    server_run(&hooks);
}

int app_main(const AppConfig *cfg) {
    g_cfg = *cfg;
    if (!app_boot(cfg->data_dir)) return 1;
    compute_asset_version();
    if (!server_listen(cfg->host, cfg->port)) return 1;

    char where[64] = "";
    uint32_t any = 0;
    ip_parse(cfg->host ? cfg->host : "0.0.0.0", &any);
    LanIface ifc;
    if ((!cfg->host || !*cfg->host || any == 0) && lan_iface(&ifc)) {
        char ip[16];
        ip_format(ifc.ip, ip);
        snprintf(where, sizeof where, "http://%s:%d", ip, server_port());
    } else {
        snprintf(where, sizeof where, "http://%s:%d", cfg->host && *cfg->host ? cfg->host : "localhost",
                 server_port());
    }
    log_info("%s is up on %s  (%d modules%s)", app_name(), where, nmods, cfg->web_dir ? ", dev mode" : "");
    if (!auth_ready())
        log_warn("HEMMA_AUTH_SALT is not set, so nobody can sign in or be created (see the line above; "
                 "hemma --gen-salt makes a value for the settings file)");
    else if (auth_setup_open()) {
        log_warn("No admin yet - open %s/ in a browser to create one", where);
        log_warn("The welcome page asks for this setup code: %s", auth_setup_code());
    }

    plat_on_quit(server_stop);
    app_serve();
    log_info("shutting down");
    server_shutdown();
    app_halt();
    return 0;
}
