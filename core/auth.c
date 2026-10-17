/* core/auth.c — see auth.h */
#include "auth.h"
#include "codec.h"
#include "db.h"
#include "env.h"
#include "plat.h"
#include "ui.h"

static char g_salt[256];
static int g_iter = 120000;

bool auth_ready(void) { return g_salt[0] != '\0'; }

bool auth_mac(const void *msg, size_t n, uint8_t out[32]) {
    if (!g_salt[0]) return false;
    hmac_sha256(g_salt, strlen(g_salt), msg, n, out);
    return true;
}

void auth_configure(const char *server_salt, int iterations) {
    str_copy(g_salt, sizeof g_salt, server_salt ? server_salt : "");
    if (iterations > 0) g_iter = iterations;
}

/* The secret ("pepper") is mixed into every password hash and recovery code, and lives
   outside the database on purpose: a copied hemma.db alone can't be used to guess
   passwords. So it goes into the settings file - never the data folder. */
static bool salt_line(const char *line) { /* read the way env.c reads it: a BOM, "export ", spaces (issue #12) */
    char key[32];
    return env_line_key(line, key, sizeof key) && str_eq(key, "HEMMA_AUTH_SALT");
}

int auth_ensure_secret(const char *env_file, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    if (*env("HEMMA_AUTH_SALT", "")) return 0;
    if (!env_file || !*env_file) env_file = ".env";
    for (int i = 0; i < account_count(); i++)
        if (account_at(i)->pass[0]) {
            if (why)
                snprintf(why, cap,
                         "HEMMA_AUTH_SALT is missing, but this household already has passwords - they only work with "
                         "the secret they were made with. Put the HEMMA_AUTH_SALT line back into %s (from a backup)",
                         env_file);
            return -1;
        }
    char hex[65];
    rand_hex(hex, 32); /* 256 bits */
    Buf old = {0}, out = {0};
    bool existed = path_stat(env_file, NULL, NULL, NULL);
    if (existed && !file_read_all(env_file, &old, 1u << 20)) {
        if (why) snprintf(why, cap, "can't read %s to add a secret to it", env_file);
        buf_free(&old);
        return -1;
    }
    static const char note[] = "# The secret mixed into every password (made by Hemma on its first start). Keep it\n"
                               "# private and in your backups, apart from the data folder: without it nobody can\n"
                               "# sign in, and a new one doesn't bring the old passwords back.\n";
    if (!existed) buf_puts(&out, "# Hemma settings - see .env.example for what else can go here.\n");
    bool placed = false;
    for (const char *p = buf_cstr(&old); *p;) { /* an empty "HEMMA_AUTH_SALT=" line gets the value */
        size_t n = strcspn(p, "\n");
        char line[512];
        str_ncopy(line, sizeof line, p, n < sizeof line ? n : sizeof line - 1);
        if (!placed && salt_line(line)) {
            if (str_starts(line, "\xEF\xBB\xBF")) buf_puts(&out, "\xEF\xBB\xBF"); /* keep the file's BOM */
            buf_printf(&out, "%sHEMMA_AUTH_SALT=%s\n", note, hex);
            placed = true;
        } else {
            buf_put(&out, p, n);
            buf_putc(&out, '\n');
        }
        p += n + (p[n] == '\n');
    }
    if (!placed) buf_printf(&out, "%s%sHEMMA_AUTH_SALT=%s\n", out.len ? "\n" : "", note, hex);
    bool ok = file_write_private(env_file, out.p, out.len);
    buf_free(&old);
    buf_free(&out);
    if (!ok) {
        if (why) snprintf(why, cap, "couldn't save a secret in %s", env_file);
        return -1;
    }
    plat_setenv("HEMMA_AUTH_SALT", hex, true);
    if (why) snprintf(why, cap, "made a secret for passwords and saved it in %s", env_file);
    return 1;
}

/* ---------------------------------------------------------------- hashing */

static void peppered(const char *password, uint8_t key[32]) {
    hmac_sha256(g_salt, strlen(g_salt), password, strlen(password), key);
}

bool auth_hash(const char *password, char *out, size_t cap) {
    if (!auth_ready() || !password) return false;
    uint8_t key[32], salt[16], dk[32];
    peppered(password, key);
    rand_bytes(salt, sizeof salt);
    pbkdf2_sha256(key, sizeof key, salt, sizeof salt, (uint32_t)g_iter, dk, sizeof dk);
    char sh[33], dh[65];
    hex_encode(salt, sizeof salt, sh);
    hex_encode(dk, sizeof dk, dh);
    int n = snprintf(out, cap, "pbkdf2-sha256$%d$%s$%s", g_iter, sh, dh);
    memset(key, 0, sizeof key);
    return n > 0 && (size_t)n < cap;
}

static bool parse_hash(const char *stored, int *iter, uint8_t salt[16], uint8_t dk[32]) {
    if (!stored || !str_starts(stored, "pbkdf2-sha256$")) return false;
    const char *p = stored + 14;
    char *end;
    long it = strtol(p, &end, 10);
    if (end == p || *end != '$' || it < 1 || it > 10000000) return false;
    const char *s = end + 1, *d = strchr(s, '$');
    if (!d || d - s != 32) return false;
    char sh[33];
    memcpy(sh, s, 32);
    sh[32] = '\0';
    if (!hex_decode(sh, salt, 16) || !hex_decode(d + 1, dk, 32)) return false;
    *iter = (int)it;
    return true;
}

bool auth_check(const char *password, const char *stored) {
    int iter;
    uint8_t salt[16], want[32], got[32], key[32];
    if (!auth_ready() || !password || !parse_hash(stored, &iter, salt, want)) return false;
    peppered(password, key);
    pbkdf2_sha256(key, sizeof key, salt, sizeof salt, (uint32_t)iter, got, sizeof got);
    memset(key, 0, sizeof key);
    return ct_equal(got, want, sizeof got);
}

bool auth_needs_rehash(const char *stored) {
    int iter;
    uint8_t s[16], d[32];
    return !parse_hash(stored, &iter, s, d) || iter < g_iter;
}

const char *auth_password_problem(const char *pw) {
    size_t n = pw ? utf8_count(pw) : 0;
    if (n < 8) return "Use at least 8 characters";
    if (n > 200) return "That's a bit long — 200 characters at most";
    return NULL;
}

bool auth_username_ok(const char *u) {
    size_t n = u ? strlen(u) : 0;
    if (n < 2 || n > 32) return false;
    for (; *u; u++)
        if (!((*u >= 'a' && *u <= 'z') || (*u >= '0' && *u <= '9') || *u == '.' || *u == '_' || *u == '-')) return false;
    return true;
}

static void normalize_username(const char *in, char *out, size_t cap) {
    str_copy(out, cap, in ? in : "");
    str_trim(out);
    for (char *p = out; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
}

Person *person_by_username(const char *username) {
    char u[40];
    normalize_username(username, u, sizeof u);
    if (!u[0]) return NULL;
    for (int i = 0; i < account_count(); i++)
        if (str_eq(account_at(i)->username, u)) return account_at(i);
    return NULL;
}

Person *person_for_sign_in(const char *typed) {
    Person *p = person_by_username(typed);
    if (p) return p;
    char t[64], want[64], got[64];
    str_copy(t, sizeof t, typed ? typed : "");
    str_trim(t);
    if (!t[0]) return NULL;
    fold_copy(want, sizeof want, t);
    Person *hit = NULL;
    for (int i = 0; i < account_count(); i++) {
        Person *q = account_at(i);
        if (!q->username[0] || !q->pass[0]) continue;
        fold_copy(got, sizeof got, q->name);
        if (!str_eq(got, want)) continue;
        if (hit) return NULL; /* two accounts go by that name: only their usernames tell them apart */
        hit = q;
    }
    return hit;
}

/* ------------------------------------------------------------- bootstrap */

#define SET_BOOTSTRAPPED "auth.bootstrapped" /* 1 once the household has had an admin */
#define SET_RESET_DONE "auth.reset_done"     /* hash of the last HEMMA_ADMIN_RESET user + password */
#define PREF_TEMP "auth.temp"                /* per person: "1" = signed in with a server-side temporary password */
#define SET_SALT_CHECK "auth.salt_check"     /* a fingerprint of the secret the passwords were made with */

/* A start with a different HEMMA_AUTH_SALT (another machine's settings file, a lost line) used
   to fail every sign-in with nothing in the log but "failed sign-in" (issue #3). The
   fingerprint can't be turned back into the secret. */
static void salt_fingerprint(char out[17]) {
    static const char what[] = "hemma: which secret";
    uint8_t mac[32];
    hmac_sha256(g_salt, strlen(g_salt), what, sizeof what - 1, mac);
    hex_encode(mac, 8, out);
}

/* The secret just made or matched a password: it's the household's. */
static void salt_proven(void) {
    char fp[17];
    salt_fingerprint(fp);
    if (!str_eq(setting(SET_SALT_CHECK, ""), fp)) setting_set(SET_SALT_CHECK, fp);
}

bool auth_salt_mismatch(void) {
    const char *was = setting(SET_SALT_CHECK, "");
    char fp[17];
    if (!auth_ready() || !was[0]) return false;
    salt_fingerprint(fp);
    return !str_eq(was, fp);
}

static const char *env_nonempty(const char *key) {
    const char *v = env(key, NULL);
    return v && *v ? v : NULL;
}

static bool admin_can_sign_in(void) {
    for (int i = 0; i < account_count(); i++) {
        const Person *p = account_at(i);
        if (p->role == ROLE_ADMIN && p->username[0] && p->pass[0]) return true;
    }
    return false;
}

bool auth_admin_can_sign_in(void) { return admin_can_sign_in(); }

static bool rescued(void) { return setting(SETTING_RESCUED, "")[0] != 0; }

bool auth_setup_open(void) {
    return auth_ready() && !setting_int(SET_BOOTSTRAPPED, 0) && !admin_can_sign_in() && !rescued();
}

typedef enum { MADE_NEW, MADE_ATTACHED, MADE_UPDATED } Made;

/* Makes `uname` an admin with password `pw`: the account with that username, else a
   person called `name` (default: the username) with no sign-in yet (no duplicates), else
   a new person. Returns the person id (0 on failure). */
static int make_admin(const char *uname, const char *name, const char *pw, Made *made) {
    Person rec;
    const Person *p = person_by_username(uname);
    if (p) {
        rec = *p;
        *made = MADE_UPDATED;
    } else {
        const Person *same = NULL;
        for (int i = 0; i < person_count() && !same; i++) {
            const Person *q = person_at(i);
            if (!q->username[0] && str_ieq(q->name, name ? name : uname)) same = q;
        }
        if (same) {
            rec = *same;
            *made = MADE_ATTACHED;
        } else {
            memset(&rec, 0, sizeof rec);
            str_copy(rec.name, sizeof rec.name, name ? name : uname);
            if (!name && rec.name[0] >= 'a' && rec.name[0] <= 'z') rec.name[0] = (char)(rec.name[0] - 32);
            rec.hue = 200;
            rec.created = now_sec();
            *made = MADE_NEW;
        }
    }
    str_copy(rec.username, sizeof rec.username, uname);
    if (!auth_hash(pw, rec.pass, sizeof rec.pass)) return 0;
    rec.role = ROLE_ADMIN;
    tbl_put(&T_person, &rec); /* p/same may dangle from here on: only rec is used */
    salt_proven();
    return rec.id;
}

/* HEMMA_ADMIN_RESET=1: the way back in when every admin is locked out. Applied once per
   user + password value, so a forgotten RESET line doesn't sign the admin out at every start. */
static void reset_admin(const char *uname, const char *pw) {
    if (!pw) {
        log_warn("HEMMA_ADMIN_RESET=1 needs HEMMA_ADMIN_PASSWORD too - nothing was reset");
        return;
    }
    Buf fp = {0};
    buf_printf(&fp, "%s\n%s", uname, pw);
    if (auth_check(buf_cstr(&fp), setting(SET_RESET_DONE, ""))) {
        log_warn("HEMMA_ADMIN_RESET=1 is still set, but this reset was already done - remove HEMMA_ADMIN_RESET "
                 "and HEMMA_ADMIN_PASSWORD from the settings file");
        buf_free(&fp);
        return;
    }
    Made made;
    int id = make_admin(uname, env_nonempty("HEMMA_ADMIN_NAME"), pw, &made);
    if (id) {
        char h[128];
        if (auth_hash(buf_cstr(&fp), h, sizeof h)) setting_set(SET_RESET_DONE, h);
        setting_set_int(SET_BOOTSTRAPPED, 1);
        int n = sessions_drop(id, NULL);
        auth_forget_failures(uname);
        if (made == MADE_UPDATED)
            log_warn("HEMMA_ADMIN_RESET: \"%s\" is an admin again with the password from the environment; signed "
                     "out of %d device%s",
                     uname, n, n == 1 ? "" : "s");
        else log_warn("HEMMA_ADMIN_RESET: admin \"%s\" created again from the environment", uname);
        log_warn("now remove HEMMA_ADMIN_RESET and HEMMA_ADMIN_PASSWORD from the settings file");
    }
    buf_free(&fp);
}

void auth_bootstrap_admin(void) {
    bool done = setting_int(SET_BOOTSTRAPPED, 0) != 0;
    if (!done && admin_can_sign_in()) { /* databases from before this setting existed */
        setting_set_int(SET_BOOTSTRAPPED, 1);
        done = true;
    }
    const char *user = env_nonempty("HEMMA_ADMIN_USER");
    const char *pw = env_nonempty("HEMMA_ADMIN_PASSWORD");
    bool reset = str_eq(env("HEMMA_ADMIN_RESET", ""), "1");
    if (!user) {
        if (reset) log_warn("HEMMA_ADMIN_RESET=1 needs HEMMA_ADMIN_USER too - nothing was reset");
        return;
    }
    char uname[40];
    normalize_username(user, uname, sizeof uname);
    if (!auth_username_ok(uname)) {
        log_warn("HEMMA_ADMIN_USER \"%s\" isn't a valid username (2-32 of a-z 0-9 . _ -)", user);
        return;
    }
    if (!auth_ready()) {
        log_warn("HEMMA_AUTH_SALT is not set, so nobody can sign in. Generate one with: hemma --gen-salt");
        return;
    }
    if (pw && auth_password_problem(pw))
        log_warn("HEMMA_ADMIN_PASSWORD is weak (%s) - change it after signing in", auth_password_problem(pw));
    if (reset) {
        reset_admin(uname, pw);
        return;
    }
    if (done) { /* the household manages its people from here; a deleted or renamed admin stays gone */
        if (pw) log_info("HEMMA_ADMIN_PASSWORD is only used on the very first start - you can remove it");
        return;
    }
    if (!pw) {
        log_info("HEMMA_ADMIN_USER is set without HEMMA_ADMIN_PASSWORD: the admin is created in the browser instead");
        return;
    }
    Made made;
    if (!make_admin(uname, env_nonempty("HEMMA_ADMIN_NAME"), pw, &made)) return;
    setting_set_int(SET_BOOTSTRAPPED, 1);
    if (made == MADE_UPDATED) log_info("admin \"%s\": existing account made the admin, password from the environment", uname);
    else if (made == MADE_ATTACHED) log_info("admin \"%s\" attached to %s", uname, person_by_username(uname)->name);
    else log_info("admin \"%s\" created from the environment", uname);
}

/* ------------------------------------------------------------- throttling */

/* Failures are counted per key: (address, username) pairs, whole accounts, whole
   addresses (any username), and signed-in people re-entering their password. Keys are
   hashes; entries are forgotten after a quiet half hour - and never earlier: a full
   table doesn't make room by dropping someone's recent failures. */
#define PAIR_SLOTS 512
#define ACCOUNT_SLOTS 64
#define ADDR_SLOTS 256
#define KNOWN_SLOTS 64
#define PAIR_LIMIT 5     /* tries per (address, username) before the first lock */
#define ACCOUNT_LIMIT 20 /* tries per account, from all addresses together */
#define ADDR_LIMIT 10    /* tries per address, whatever the usernames: each costs a password hash */
#define SETUP_LIMIT 10   /* refused welcome-page submissions per address */
#define FORGIVE_S (30 * 60)
#define FULL_WAIT_S 60 /* a new address while the address table is full of fresh failures */

typedef struct {
    uint64_t key; /* 0 = free slot */
    uint64_t user; /* hash of the username (to forget a user's failures) */
    int fails;
    int64_t last, until;
} Thr;

/* Every password check is a PBKDF2 hash on the event loop (~1 s on a Pi 3), and one host can
   bring hundreds of addresses (issue #5). Checks for (address, user) pairs that never signed in
   share a budget of loop time - a quarter of it, in bursts of up to HASH_BURST_MS - so a flood
   of guesses slows signing in from new places, never the pages of people already in. */
#define HASH_SHARE 4
#define HASH_BURST_MS 4000
static int64_t g_hash_budget = HASH_BURST_MS, g_hash_at; /* ms left; monotonic ms it was last topped up */
static int64_t g_hash_said;                               /* when the log last said it ran out */

static int64_t hash_budget(void) {
    int64_t now = now_ms();
    if (!g_hash_at) g_hash_at = now;
    int64_t gained = (now - g_hash_at) / HASH_SHARE;
    g_hash_budget += gained;
    g_hash_at += gained * HASH_SHARE; /* the remainder keeps counting: frequent calls still refill */
    if (g_hash_budget >= HASH_BURST_MS) {
        g_hash_budget = HASH_BURST_MS;
        g_hash_at = now;
    }
    return g_hash_budget;
}

void auth_set_hash_budget(int ms) {
    g_hash_budget = ms;
    g_hash_at = now_ms();
}

static Thr g_pair[PAIR_SLOTS], g_account[ACCOUNT_SLOTS], g_recheck[ACCOUNT_SLOTS], g_addr[ADDR_SLOTS];
static struct {
    uint64_t key;
    int64_t at;
} g_known[KNOWN_SLOTS]; /* (address, username) pairs that signed in successfully */

static void fresh_reset(void);

void auth_reset_throttle(void) {
    memset(g_pair, 0, sizeof g_pair);
    memset(g_account, 0, sizeof g_account);
    memset(g_recheck, 0, sizeof g_recheck);
    memset(g_addr, 0, sizeof g_addr);
    memset(g_known, 0, sizeof g_known);
    auth_set_hash_budget(HASH_BURST_MS);
    g_hash_said = 0;
    fresh_reset();
}

static uint64_t thr_key(char kind, uint32_t a, const char *user) {
    char s[64];
    snprintf(s, sizeof s, "%c|%08x|%s", kind, (unsigned)a, user);
    return hash64_str(s) | 1; /* never 0 (= free) */
}

static Thr *thr_find(Thr *t, int n, uint64_t key) {
    for (int i = 0; i < n; i++)
        if (t[i].key == key) return &t[i];
    return NULL;
}

/* An entry still matters while it is locked or its last failure is younger than the
   forgiveness window (thr_fail would still count it). */
static bool thr_live(const Thr *x, int64_t now) { return x->key && (x->until > now || now - x->last <= FORGIVE_S); }

/* A slot for `key`: its own, else a free or forgiven one. Never someone's live entry -
   otherwise spraying junk usernames would wipe the sprayer's own failures (review).
   NULL when every slot is live: the failure then only counts in the other tables. */
static Thr *thr_take(Thr *t, int n, uint64_t key, uint64_t user, int64_t now) {
    Thr *e = thr_find(t, n, key);
    if (e) return e;
    for (int i = 0; i < n; i++) {
        Thr *v = &t[i];
        if (thr_live(v, now)) continue;
        memset(v, 0, sizeof *v);
        v->key = key;
        v->user = user;
        return v;
    }
    return NULL;
}

static bool thr_full(const Thr *t, int n, int64_t now) {
    for (int i = 0; i < n; i++)
        if (!thr_live(&t[i], now)) return false;
    return true;
}

static int thr_wait(const Thr *e, int64_t now) { return e && e->until > now ? (int)(e->until - now) : 0; }

static void thr_fail(Thr *e, int limit, int64_t now) {
    if (!e) return; /* table full of live entries (see thr_take) */
    if (e->until <= now && now - e->last > FORGIVE_S) e->fails = 0;
    e->fails++;
    e->last = now;
    if (e->fails >= limit) { /* 1, 2, 4, 8, then 15 minutes */
        int lock = 60 << MIN(e->fails - limit, 4);
        e->until = now + MIN(lock, 15 * 60);
    }
}

static bool known_has(uint64_t key) {
    for (int i = 0; i < KNOWN_SLOTS; i++)
        if (g_known[i].key == key) return true;
    return false;
}

static void known_add(uint64_t key, int64_t now) {
    int v = 0;
    for (int i = 0; i < KNOWN_SLOTS; i++) {
        if (g_known[i].key == key) {
            v = i;
            break;
        }
        if (g_known[i].at < g_known[v].at) v = i;
    }
    g_known[v].key = key;
    g_known[v].at = now;
}

void auth_forget_failures(const char *username) {
    char u[40];
    normalize_username(username, u, sizeof u);
    uint64_t uk = hash64_str(u);
    for (int i = 0; i < PAIR_SLOTS; i++)
        if (g_pair[i].key && g_pair[i].user == uk) memset(&g_pair[i], 0, sizeof g_pair[i]);
    for (int i = 0; i < ACCOUNT_SLOTS; i++)
        if (g_account[i].key && g_account[i].user == uk) memset(&g_account[i], 0, sizeof g_account[i]);
}

/* The address budget: every wrong password or code from one address counts, whatever
   the username (each one costs a password hash on the event loop). */
static void addr_failed(uint32_t ip, uint64_t key, int64_t now) {
    Thr *a = thr_take(g_addr, ADDR_SLOTS, key, 0, now); /* user 0: a new password for someone doesn't clear it */
    bool was_locked = a && a->until > now;
    thr_fail(a, ADDR_LIMIT, now);
    if (a && !was_locked && a->until > now) {
        char peer[16];
        ip_format(ip, peer);
        log_warn("%d wrong passwords or codes from %s (any username): it waits %d min before the next try", a->fails,
                 peer, (int)((a->until - now + 59) / 60));
    }
}

/* One guess at an account (a password or a recovery code), counted against the same budget. */
typedef struct {
    char user[40];
    Person *p; /* NULL = no such account */
    uint32_t ip;
    uint64_t uk, kpair, kacct, kknown, kaddr;
    bool known; /* this address signed in as this user before */
    Thr *pair;
} Try;

/* Seconds to wait before this guess may be checked (0 = go ahead). Cheap: nothing is
   hashed before the answer is "go ahead". */
static int try_begin(Try *t, const char *typed, uint32_t ip, int64_t now) {
    t->p = person_for_sign_in(typed);
    if (t->p) str_copy(t->user, sizeof t->user, t->p->username); /* "Adam" and "admin": one budget */
    else normalize_username(typed, t->user, sizeof t->user);
    t->ip = ip;
    t->uk = hash64_str(t->user);
    t->kpair = thr_key('p', ip, t->user);
    t->kacct = thr_key('a', 0, t->user);
    t->kknown = thr_key('k', ip, t->user);
    t->kaddr = thr_key('i', ip, "");
    bool known = t->known = known_has(t->kknown);
    /* Another account's success never clears these: failures are per (address, user). The
       account-wide lock (only real accounts have one) spares addresses it signed in from before. */
    t->pair = thr_find(g_pair, PAIR_SLOTS, t->kpair);
    int w = thr_wait(t->pair, now);
    if (t->p && !known) w = MAX(w, thr_wait(thr_find(g_account, ACCOUNT_SLOTS, t->kacct), now));
    Thr *a = thr_find(g_addr, ADDR_SLOTS, t->kaddr);
    w = MAX(w, thr_wait(a, now));
    /* every slot holds fresh failures of other addresses (a flood from many): a new address
       couldn't be counted, so it waits - unless it signed in as this user before */
    if (!a && !known && thr_full(g_addr, ADDR_SLOTS, now)) w = MAX(w, FULL_WAIT_S);
    return w;
}

static void try_failed(Try *t, int64_t now) {
    addr_failed(t->ip, t->kaddr, now);
    thr_fail(thr_take(g_pair, PAIR_SLOTS, t->kpair, t->uk, now), PAIR_LIMIT, now);
    if (!t->p) return;
    Thr *a = thr_take(g_account, ACCOUNT_SLOTS, t->kacct, t->uk, now);
    bool was_locked = a && a->until > now;
    thr_fail(a, ACCOUNT_LIMIT, now);
    if (a && !was_locked && a->until > now) /* logged like the address lock (issue #3) */
        log_warn("%d wrong passwords or codes for \"%s\" from different addresses: new addresses wait %d min before "
                 "the next try (ones it signed in from before don't)",
                 a->fails, t->user, (int)((a->until - now + 59) / 60));
}

static void try_passed(Try *t, int64_t now) {
    if (t->pair) memset(t->pair, 0, sizeof *t->pair);
    known_add(t->kknown, now);
}

LoginResult auth_login(const char *username, const char *password, uint32_t ip, int64_t now, int *person, int *wait_s) {
    if (!auth_ready()) return LOGIN_UNAVAILABLE;
    Try t;
    int w = try_begin(&t, username, ip, now);
    if (w > 0) {
        if (wait_s) *wait_s = w;
        return LOGIN_THROTTLED;
    }
    if (!t.known && hash_budget() <= 0) { /* too many checks lately: new places wait a moment */
        if (!g_hash_said || now_ms() - g_hash_said > 10 * 60 * 1000) {
            g_hash_said = now_ms();
            log_warn("lots of sign-in tries right now: new devices wait a few seconds between them");
        }
        /* It counts against the address: a flood that keeps asking to grab each refill the
           moment it comes locks its own addresses out instead (review). */
        thr_fail(thr_take(g_addr, ADDR_SLOTS, t.kaddr, 0, now), ADDR_LIMIT, now);
        if (wait_s) *wait_s = (int)MAX(1, ((1 - g_hash_budget) * HASH_SHARE + 999) / 1000);
        return LOGIN_BUSY;
    }
    int64_t t0 = now_ms();
    Person *p = t.p;
    bool ok;
    if (p && p->pass[0]) {
        ok = auth_check(password ? password : "", p->pass);
    } else { /* same amount of work whether or not the user exists */
        static char dummy[128];
        if (!dummy[0] || auth_needs_rehash(dummy)) auth_hash("no such user here", dummy, sizeof dummy);
        auth_check(password ? password : "", dummy);
        ok = false;
    }
    g_hash_budget -= now_ms() - t0;
    if (!ok) {
        try_failed(&t, now);
        return LOGIN_WRONG;
    }
    try_passed(&t, now);
    salt_proven();
    Person rec = *p;
    rec.last_login = now;
    if (auth_needs_rehash(rec.pass)) { /* upgrade old hashes while we know the password */
        char h[128];
        if (auth_hash(password, h, sizeof h)) str_copy(rec.pass, sizeof rec.pass, h);
    }
    tbl_put(&T_person, &rec);
    if (person) *person = rec.id;
    return LOGIN_OK;
}

LoginResult auth_recheck(const Person *p, const char *password, uint32_t ip, int64_t now, int *wait_s) {
    if (!auth_ready()) return LOGIN_UNAVAILABLE;
    if (!p) return LOGIN_WRONG;
    uint64_t key = thr_key('r', (uint32_t)p->id, ""), kaddr = thr_key('i', ip, "");
    Thr *e = thr_find(g_recheck, ACCOUNT_SLOTS, key);
    int w = MAX(thr_wait(e, now), thr_wait(thr_find(g_addr, ADDR_SLOTS, kaddr), now)); /* before any hashing */
    if (w > 0) {
        if (wait_s) *wait_s = w;
        return LOGIN_THROTTLED;
    }
    if (p->pass[0] && auth_check(password ? password : "", p->pass)) {
        if (e) memset(e, 0, sizeof *e);
        salt_proven();
        return LOGIN_OK;
    }
    thr_fail(thr_take(g_recheck, ACCOUNT_SLOTS, key, 0, now), PAIR_LIMIT, now);
    addr_failed(ip, kaddr, now);
    return LOGIN_WRONG;
}

bool auth_temporary(int person) { return person > 0 && str_eq(pref(person, PREF_TEMP, ""), "1"); }

int auth_set_password(int person, const char *password, const Session *keep) {
    Person *p = person_get(person);
    if (!p) return -1;
    Person rec = *p;
    if (!auth_hash(password, rec.pass, sizeof rec.pass)) return -1;
    tbl_put(&T_person, &rec);
    salt_proven();
    auth_forget_failures(rec.username);
    if (auth_temporary(person)) pref_set(person, PREF_TEMP, "");
    return sessions_drop(person, keep);
}

/* --------------------------------------------------------- recovery codes */

typedef struct {
    int id;
    int person;
    char hash[65]; /* hex HMAC-SHA256(server salt, "recovery:<person>:<code>") */
    int64_t created;
} RecoveryCode;

static const FieldDef recovery_fields[] = {
    FIELD(RecoveryCode, id, F_INT),
    FIELD(RecoveryCode, person, F_INT),
    FIELD_SECRET(RecoveryCode, hash, F_STR),
    FIELD(RecoveryCode, created, F_I64),
};
static Table T_recovery = TABLE("recovery_code", RecoveryCode, recovery_fields);

/* 32 symbols (5 bits each) that can't be mistaken for each other: no 0 O 1 I */
static const char CODE_ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
#define CODE_CHARS 12 /* 60 bits */

/* The plain codes, kept only until the person who asked has saved them. */
#define FRESH_SLOTS 16
#define FRESH_TTL_S (30 * 60)
static struct {
    char token[65]; /* the session they're for */
    int person, n;
    int64_t until;
    char codes[AUTH_CODES][AUTH_CODE_LEN];
} g_fresh[FRESH_SLOTS];

static void fresh_reset(void) { memset(g_fresh, 0, sizeof g_fresh); }

/* "7kq4 m2xp-9rta" -> "7KQ4M2XP9RTA"; false unless exactly 12 symbols of the alphabet. */
static bool symbols(const char *in, char *out, int want) { /* exactly `want` of the alphabet, any case/dashes */
    int n = 0;
    for (const char *p = in ? in : ""; *p; p++) {
        char ch = *p;
        if (ch == '-' || ch == ' ' || ch == '\t' || ch == '.') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (!strchr(CODE_ALPHABET, ch) || n == want) return false;
        out[n++] = ch;
    }
    out[n] = '\0';
    return n == want;
}

static bool code_normalize(const char *in, char out[CODE_CHARS + 1]) { return symbols(in, out, CODE_CHARS); }

/* ---------------------------------------------------------- the setup code */

/* A fresh install's welcome page asks for a one-time code only someone who can see the server
   has: the installer prints it (and writes it into the settings file as HEMMA_SETUP_CODE), and
   Hemma shows it in its window/log when it starts. Without it, whoever on the network opened
   the page first would become the admin. 8 symbols of the recovery-code alphabet (40 bits),
   shown as "7KQ4-M2XP"; kept in a setting until the admin exists, then gone for good. Wrong
   codes count against the welcome page's throttle like any other refused submission. */
#define SETUP_CHARS 8
#define SET_SETUP_CODE "auth.setup_code"

void auth_new_setup_code(char out[16]) {
    uint8_t r[SETUP_CHARS];
    char raw[SETUP_CHARS + 1];
    rand_bytes(r, sizeof r);
    for (int k = 0; k < SETUP_CHARS; k++) raw[k] = CODE_ALPHABET[r[k] & 31]; /* 256 = 8 x 32: uniform */
    raw[SETUP_CHARS] = '\0';
    snprintf(out, 16, "%.4s-%.4s", raw, raw + 4);
}

static bool setup_code_now(char norm[SETUP_CHARS + 1]) { /* the code in force, made when there's none */
    const char *e = env("HEMMA_SETUP_CODE", "");
    if (*e && symbols(e, norm, SETUP_CHARS)) return true;
    if (*e) {
        static bool warned;
        if (!warned) log_warn("HEMMA_SETUP_CODE isn't a setup code (8 of 2-9 and A-Z without I, O) - using Hemma's own");
        warned = true;
    }
    if (symbols(setting(SET_SETUP_CODE, ""), norm, SETUP_CHARS)) return true;
    char fresh[16];
    auth_new_setup_code(fresh);
    if (!symbols(fresh, norm, SETUP_CHARS)) return false;
    setting_set(SET_SETUP_CODE, norm);
    return true;
}

const char *auth_setup_code(void) {
    static char shown[16];
    char norm[SETUP_CHARS + 1];
    shown[0] = '\0';
    if (auth_setup_open() && setup_code_now(norm)) snprintf(shown, sizeof shown, "%.4s-%.4s", norm, norm + 4);
    return shown;
}

static void code_hash(int person, const char *norm, char out[65]) {
    char msg[64];
    int n = snprintf(msg, sizeof msg, "recovery:%d:%s", person, norm);
    uint8_t mac[32];
    hmac_sha256(g_salt, strlen(g_salt), msg, (size_t)MAX(n, 0), mac);
    hex_encode(mac, sizeof mac, out);
}

static bool can_have_codes(const Person *p) { return p && p->role != ROLE_DISPLAY && p->username[0] && p->pass[0]; }

int auth_codes_left(int person) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_recovery); i++) n += ((RecoveryCode *)tbl_at(&T_recovery, i))->person == person;
    return n;
}

void auth_codes_drop(int person) {
    for (int i = tbl_count(&T_recovery) - 1; i >= 0; i--) {
        RecoveryCode *rc = tbl_at(&T_recovery, i);
        if (rc->person == person) tbl_del(&T_recovery, rc->id);
    }
    for (int i = 0; i < FRESH_SLOTS; i++)
        if (g_fresh[i].person == person) memset(&g_fresh[i], 0, sizeof g_fresh[i]);
}

static int fresh_find(const Session *s) {
    if (!s || !s->token[0]) return -1;
    int64_t now = now_sec();
    for (int i = 0; i < FRESH_SLOTS; i++) {
        if (!g_fresh[i].token[0]) continue;
        if (g_fresh[i].until <= now) memset(&g_fresh[i], 0, sizeof g_fresh[i]); /* expired: forget them */
        else if (str_eq(g_fresh[i].token, s->token) && g_fresh[i].person == s->person) return i;
    }
    return -1;
}

bool auth_codes_new(int person, const Session *shown_to) {
    if (!auth_ready() || !can_have_codes(person_get(person))) return false;
    auth_codes_drop(person);
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    int64_t now = now_sec();
    for (int i = 0; i < AUTH_CODES; i++) {
        uint8_t r[CODE_CHARS];
        char raw[CODE_CHARS + 1];
        rand_bytes(r, sizeof r);
        for (int k = 0; k < CODE_CHARS; k++) raw[k] = CODE_ALPHABET[r[k] & 31]; /* 256 = 8 x 32: uniform */
        raw[CODE_CHARS] = '\0';
        snprintf(codes[i], AUTH_CODE_LEN, "%.4s-%.4s-%.4s", raw, raw + 4, raw + 8);
        RecoveryCode rc = {0};
        rc.person = person;
        rc.created = now;
        code_hash(person, raw, rc.hash);
        tbl_put(&T_recovery, &rc);
        memset(raw, 0, sizeof raw);
    }
    if (shown_to && shown_to->token[0]) {
        int slot = 0;
        for (int i = 0; i < FRESH_SLOTS; i++) {
            if (str_eq(g_fresh[i].token, shown_to->token)) {
                slot = i;
                break;
            }
            if (g_fresh[i].until < g_fresh[slot].until) slot = i; /* free (0) or the oldest */
        }
        str_copy(g_fresh[slot].token, sizeof g_fresh[slot].token, shown_to->token);
        g_fresh[slot].person = person;
        g_fresh[slot].n = AUTH_CODES;
        g_fresh[slot].until = now + FRESH_TTL_S;
        memcpy(g_fresh[slot].codes, codes, sizeof codes);
    }
    memset(codes, 0, sizeof codes);
    return true;
}

int auth_codes_fresh(const Session *s, char codes[AUTH_CODES][AUTH_CODE_LEN]) {
    int i = fresh_find(s);
    if (i < 0) return 0;
    memcpy(codes, g_fresh[i].codes, sizeof g_fresh[i].codes);
    return g_fresh[i].n;
}

void auth_codes_seen(const Session *s) {
    int i = fresh_find(s);
    if (i >= 0) memset(&g_fresh[i], 0, sizeof g_fresh[i]);
}

LoginResult auth_recover(const char *username, const char *code, uint32_t ip, int64_t now, int *person, int *wait_s) {
    if (!auth_ready()) return LOGIN_UNAVAILABLE;
    Try t;
    int w = try_begin(&t, username, ip, now);
    if (w > 0) {
        if (wait_s) *wait_s = w;
        return LOGIN_THROTTLED;
    }
    char norm[CODE_CHARS + 1], h[65];
    bool shaped = code_normalize(code, norm);
    bool can = can_have_codes(t.p);
    int pid = can ? t.p->id : 0, hit = 0;
    code_hash(pid, shaped ? norm : "-", h); /* the same work whether or not the account exists */
    for (int i = 0; can && shaped && i < tbl_count(&T_recovery); i++) {
        RecoveryCode *rc = tbl_at(&T_recovery, i);
        if (rc->person == pid && ct_equal(rc->hash, h, 64)) hit = rc->id;
    }
    if (!hit) {
        try_failed(&t, now);
        return LOGIN_WRONG;
    }
    tbl_del(&T_recovery, hit); /* each code works once */
    try_passed(&t, now);
    if (person) *person = pid;
    return LOGIN_OK;
}

/* ------------------------------------------- server-side reset (command line) */

/* Requests from `hemma --reset-password`, another process: raw SQL, outside the store. */
#define RESET_TABLE_SQL                                                                                                \
    "CREATE TABLE IF NOT EXISTS auth_reset (username TEXT PRIMARY KEY, pass TEXT NOT NULL, at INTEGER NOT NULL "      \
    "DEFAULT 0)"

/* 16 symbols in groups of four, lower case for phone keyboards: no i l o 0 1 (31 symbols, ~79 bits). */
static void temp_password(char out[24]) {
    static const char a[] = "abcdefghjkmnpqrstuvwxyz23456789";
    int n = 0, k = 0;
    while (k < 16) {
        uint8_t r[32];
        rand_bytes(r, sizeof r);
        for (int i = 0; i < (int)sizeof r && k < 16; i++) {
            if (r[i] >= 248) continue; /* 248 = 8 x 31: keep it uniform */
            if (k && k % 4 == 0) out[n++] = '-';
            out[n++] = a[r[i] % 31];
            k++;
        }
    }
    out[n] = '\0';
}

static int64_t g_data_version = -1;

/* Applies up to 8 requests waiting in auth_reset (left by the command line); returns how many. */
static int apply_reset_batch(void) {
    sqlite3_stmt *st = db_prep("SELECT username, pass FROM auth_reset");
    if (!st) return 0;
    struct {
        char user[40], pass[128];
    } req[8];
    int n = 0;
    while (n < countof(req) && sqlite3_step(st) == SQLITE_ROW) {
        const char *u = (const char *)sqlite3_column_text(st, 0), *h = (const char *)sqlite3_column_text(st, 1);
        str_copy(req[n].user, sizeof req[n].user, u ? u : "");
        str_copy(req[n].pass, sizeof req[n].pass, h ? h : "");
        n++;
    }
    sqlite3_reset(st);
    if (!n) return 0;
    db_begin();
    for (int i = 0; i < n; i++) {
        Person *p = person_by_username(req[i].user);
        int it;
        uint8_t s[16], d[32];
        if (p && parse_hash(req[i].pass, &it, s, d)) {
            Person rec = *p;
            str_copy(rec.pass, sizeof rec.pass, req[i].pass);
            tbl_put(&T_person, &rec);
            int dropped = sessions_drop(rec.id, NULL);
            auth_forget_failures(rec.username);
            if (rec.role != ROLE_DISPLAY) pref_set(rec.id, PREF_TEMP, "1"); /* displays can't pick their own */
            log_warn("password for \"%s\" reset on the server (hemma --reset-password); signed out of %d device%s",
                     rec.username, dropped, dropped == 1 ? "" : "s");
        } else {
            log_warn("hemma --reset-password \"%s\": no such account any more - nothing changed", req[i].user);
        }
        sqlite3_stmt *del = db_prep("DELETE FROM auth_reset WHERE username = ?");
        if (del) {
            sqlite3_bind_text(del, 1, req[i].user, -1, SQLITE_TRANSIENT);
            sqlite3_step(del);
            sqlite3_reset(del);
        }
    }
    db_commit();
    app_changed();
    return n;
}

static void apply_resets(void) {
    for (int i = 0; i < 64 && apply_reset_batch() == 8; i++) { /* bounded: a failing DELETE can't spin */
    }
}

void auth_init(void) {
    T_recovery.api_hidden = true;
    T_recovery.what = "recovery codes";
    store_register(&T_recovery);
    fresh_reset();
}

void auth_start(void) {
    db_exec(RESET_TABLE_SQL);
    g_data_version = -1;
    if (auth_salt_mismatch())
        log_warn("HEMMA_AUTH_SALT isn't the secret this household's passwords were made with, so nobody can sign "
                 "in. Put the original HEMMA_AUTH_SALT line back into the settings file (from a backup) and "
                 "restart Hemma - see \"Locked out\" in docs/accounts.md");
    /* codes of people who were removed or can't sign in any more */
    for (int i = tbl_count(&T_recovery) - 1; i >= 0; i--) {
        RecoveryCode *rc = tbl_at(&T_recovery, i);
        Person *p = person_get(rc->person);
        if (!p || !p->username[0] || p->role == ROLE_DISPLAY) tbl_del(&T_recovery, rc->id);
    }
    apply_resets();
}

/* SQLite bumps data_version when another connection (the command line) commits: only
   then is the request table worth a look. */
void auth_tick(void) {
    sqlite3_stmt *st = db_prep("PRAGMA data_version");
    if (!st || sqlite3_step(st) != SQLITE_ROW) return;
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_reset(st);
    if (v == g_data_version) return;
    g_data_version = v;
    apply_resets();
}

static bool cli_step_ok(sqlite3 *d, const char *sql, const char *a, const char *b) {
    sqlite3_stmt *st = NULL;
    bool ok = sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(st, 1, a, -1, SQLITE_STATIC);
        if (b) sqlite3_bind_text(st, 2, b, -1, SQLITE_STATIC);
        if (b) sqlite3_bind_int64(st, 3, now_sec());
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    return ok;
}

int auth_reset_cli(const char *data_dir, const char *username, int wait_ms, Buf *out, char *temp_out) {
    if (temp_out) temp_out[0] = '\0';
    if (!auth_ready()) {
        buf_puts(out, "HEMMA_AUTH_SALT isn't set. Use the same settings file as the server, e.g.\n"
                      "  hemma --env /etc/hemma/hemma.env --data /var/lib/hemma --reset-password NAME\n");
        return 1;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/hemma.db", data_dir && *data_dir ? data_dir : "data");
    bool is_dir = false;
    if (!path_stat(path, NULL, NULL, &is_dir) || is_dir) {
        buf_printf(out,
                   "There's no Hemma database at %s.\nPoint --data at the folder the server uses (its --data or "
                   "HEMMA_DATA), e.g. --data /var/lib/hemma\n",
                   path);
        return 1;
    }
    sqlite3 *d = NULL;
    if (sqlite3_open_v2(path, &d, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        buf_printf(out, "Can't open %s: %s\n", path, d ? sqlite3_errmsg(d) : "out of memory");
        sqlite3_close(d);
        return 1;
    }
    sqlite3_busy_timeout(d, 5000);
    char user[40], name[64] = "";
    normalize_username(username, user, sizeof user);
    int id = 0;
    sqlite3_stmt *st = NULL;
    if (user[0] && sqlite3_prepare_v2(d, "SELECT id, name FROM person WHERE username = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, user, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            id = sqlite3_column_int(st, 0);
            const char *nm = (const char *)sqlite3_column_text(st, 1);
            str_copy(name, sizeof name, nm ? nm : "");
        }
    }
    sqlite3_finalize(st);
    if (!id) {
        buf_printf(out, "No account has the username \"%s\".\n", user);
        int k = 0;
        if (sqlite3_prepare_v2(d, "SELECT username, role FROM person WHERE username <> '' ORDER BY username", -1, &st,
                               NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                int role = sqlite3_column_int(st, 1);
                buf_printf(out, "%s%s%s", k++ ? ", " : "Accounts here: ", (const char *)sqlite3_column_text(st, 0),
                           role == ROLE_ADMIN ? " (admin)" : role == ROLE_DISPLAY ? " (display)" : "");
            }
        }
        sqlite3_finalize(st);
        buf_puts(out, k ? "\n" : "There are no accounts that can sign in: open Hemma in a browser to create the admin, "
                                 "or see HEMMA_ADMIN_RESET in docs/accounts.md.\n");
        sqlite3_close(d);
        return 1;
    }
    char temp[24], hash[128];
    temp_password(temp);
    bool ok = auth_hash(temp, hash, sizeof hash) && sqlite3_exec(d, RESET_TABLE_SQL, NULL, NULL, NULL) == SQLITE_OK &&
              cli_step_ok(d, "INSERT OR REPLACE INTO auth_reset (username, pass, at) VALUES (?, ?, ?)", user, hash);
    if (!ok) {
        buf_printf(out, "Couldn't write to %s: %s\nRun this as the user Hemma runs as (on a Pi: sudo -u hemma ...).\n",
                   path, sqlite3_errmsg(d));
        sqlite3_close(d);
        return 1;
    }
    /* a running Hemma takes the request within a tick (or two); a stopped one at its next start */
    bool applied = false;
    int64_t end = now_ms() + MAX(wait_ms, 0);
    if (sqlite3_prepare_v2(d, "SELECT 1 FROM auth_reset WHERE username = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, user, -1, SQLITE_STATIC);
        for (;;) {
            int rc = sqlite3_step(st);
            sqlite3_reset(st);
            if (rc == SQLITE_DONE) {
                applied = true;
                break;
            }
            if (now_ms() >= end) break;
            plat_sleep_ms(200);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(d);
    buf_printf(out, "Temporary password for %s (@%s):\n\n    %s\n\n", name, user, temp);
    if (applied) buf_puts(out, "Hemma is running and has applied it: the old password no longer works and every device "
                               "was signed out.\n");
    else buf_puts(out, "Hemma didn't pick it up (is it running?): it takes effect when Hemma starts.\n");
    buf_puts(out, "Sign in with it - Hemma then asks for a new password of your own.\n");
    if (temp_out) str_copy(temp_out, 24, temp);
    memset(temp, 0, sizeof temp);
    return 0;
}

/* ----------------------------------------------------------------- pages */

static bool anyone_can_sign_in(void) {
    for (int i = 0; i < account_count(); i++)
        if (account_at(i)->username[0] && account_at(i)->pass[0]) return true;
    return false;
}

static void wait_notice(Ctx *c, Buf *b) {
    long long w = ctx_qint(c, "w", 60);
    if (*ctx_q(c, "busy")) { /* the hashing budget (LOGIN_BUSY), not a lock */
        H(b, "<p class=\"notice bad\" role=\"alert\">Lots of sign-in tries right now. Wait %L second%r and try again.</p>",
          MAX(1, w), w > 1 ? "s" : "");
        return;
    }
    H(b, "<p class=\"notice bad\" role=\"alert\">Too many tries. Wait %L minute%r and try again.</p>",
      MAX(1, (w + 59) / 60), w > 60 ? "s" : "");
}

static void password_pair(Buf *b, const char *label) {
    H(b,
      "<div class=\"field-row\"><label class=\"field\"><span>%s</span><input type=\"password\" name=\"password\" "
      "autocomplete=\"new-password\" required minlength=\"8\" maxlength=\"200\"></label><label class=\"field\">"
      "<span>Again</span><input type=\"password\" name=\"again\" autocomplete=\"new-password\" required "
      "minlength=\"8\" maxlength=\"200\"></label></div>",
      label);
}

static void remember_box(Buf *b) {
    buf_puts(b, "<label class=\"check\"><input type=\"checkbox\" name=\"remember\" value=\"1\" checked> Keep me signed in "
                "on this device</label>");
}

/* Passwords can't be hashed without the server salt: nothing works until it is set. */
static void no_salt_page(Ctx *c, Buf *b) {
    ctx_title(c, "Almost there");
    H(b, "<h1>Almost there</h1><p class=\"sub\">%s needs a secret key before anyone can sign in.</p>", app_name());
    buf_puts(b, "<ol class=\"steps\"><li>Make one: run <code>hemma --gen-salt</code>.</li><li>Put it in Hemma's settings "
                "file as <code>HEMMA_AUTH_SALT=...</code>: <code>.env</code> next to Hemma, or "
                "<code>/etc/hemma/hemma.env</code> on a Pi set up with install-pi.sh (see <code>.env.example</code>)."
                "</li><li>Restart Hemma and open this page again to create the admin account.</li></ol>");
}

/* The household had an admin, but nobody can sign in any more. */
static void locked_page(Ctx *c, Buf *b) {
    ctx_title(c, "Sign in");
    H(b, "<h1>Nobody can sign in</h1><p class=\"sub\">%s has no account that can sign in right now.</p>", app_name());
    buf_puts(b, "<p>Whoever runs the Hemma server can bring the admin back: see <b>Locked out</b> in Hemma's "
                "account docs (<code>docs/accounts.md</code>).</p>");
}

/* Issue #87: a rescue that brought no admin back used to show "Welcome to Hemma" (set it up again, over
   the household's data) or "Nobody can sign in" with no word of the damage - and the rescue notice was
   for admins only. This page is for everyone: what happened, and where the way back is. */
static void damaged_page(Ctx *c, Buf *b, bool others_can_sign_in) {
    ctx_title(c, "Hemma's data was damaged");
    buf_puts(b, "<h1>Hemma's data was damaged</h1><p class=\"sub\">Nothing has been deleted.</p>");
    H(b, "<p class=\"notice bad\">%s</p>", setting(SETTING_RESCUED, ""));
    buf_puts(b, "<p>No admin account could be read back, so nobody can look after Hemma right now. Whoever runs the "
                "Hemma server can get more back from the damaged copy - see <b>A damaged database</b> in Hemma's "
                "troubleshooting guide (<code>docs/troubleshooting.md</code>) - and bring an admin back: <b>Locked "
                "out</b> in the account docs (<code>docs/accounts.md</code>).</p>");
    if (others_can_sign_in) buf_puts(b, "<p class=\"small center mt\"><a href=\"/login\">Sign in</a></p>");
}

static const char *setup_error(const char *e) {
    static const struct {
        const char *key, *text;
    } msgs[] = {
        {"setup", "That isn't the setup code. It was shown where Hemma was installed - see below where to look."},
        {"name", "Please tell us your name."},
        {"user", "Usernames are 2–32 characters: a–z, 0–9 and . _ -"},
        {"taken", "That username is taken - pick another one."},
        {"short", "Use at least 8 characters for the password."},
        {"long", "That password is a bit long - 200 characters at most."},
        {"again", "The two passwords don't match."},
        {"code", "That username and recovery code don't match."},
    };
    for (int i = 0; i < countof(msgs); i++)
        if (str_eq(e, msgs[i].key)) return msgs[i].text;
    return NULL;
}

static void error_notice(Ctx *c, Buf *b) {
    const char *e = ctx_q(c, "e");
    if (str_eq(e, "wait")) wait_notice(c, b);
    else if (setup_error(e)) H(b, "<p class=\"notice bad\" role=\"alert\">%r</p>", setup_error(e)); /* constants */
}

static void setup_page(Ctx *c, Buf *b, bool others_can_sign_in) {
    ctx_title(c, "Welcome");
    H(b, "<h1>Welcome to Hemma</h1><p class=\"sub\">Let's create the admin account: the person who adds everyone else "
         "and looks after the settings.</p>");
    error_notice(c, b);
    const char *name = ctx_q(c, "name"), *u = ctx_q(c, "u");
    buf_puts(b, "<form method=\"post\" action=\"/setup\" class=\"stack\">"
                "<label class=\"field\"><span>Setup code</span><input name=\"code\" autocomplete=\"off\" "
                "autocapitalize=\"characters\" autocorrect=\"off\" spellcheck=\"false\" required maxlength=\"20\" "
                "placeholder=\"XXXX-XXXX\" autofocus><small class=\"muted\">From where Hemma was installed - see "
                "below</small></label>");
    H(b,
      "<label class=\"field\"><span>Your name</span><input name=\"name\" autocomplete=\"name\" required "
      "maxlength=\"40\" value=\"%s\" placeholder=\"e.g. Alex\"></label>"
      "<label class=\"field\"><span>Username</span><input name=\"username\" autocomplete=\"username\" "
      "autocapitalize=\"none\" autocorrect=\"off\" spellcheck=\"false\" required maxlength=\"32\" value=\"%s\" "
      "placeholder=\"e.g. alex\"><small class=\"muted\">You sign in with this: a–z, 0–9 and . _ -</small></label>",
      name, u);
    password_pair(b, "Password");
    H(b,
      "<label class=\"field\"><span>Name of your home <span class=\"muted\">(optional)</span></span><input "
      "name=\"home\" maxlength=\"40\" value=\"%s\" placeholder=\"Hemma\"></label>",
      ctx_q(c, "home"));
    remember_box(b);
    buf_puts(b, "<button class=\"btn primary big\">Create the admin account</button></form>"
                "<p class=\"muted small center mt\">Next you get recovery codes: your way back in if you ever forget the "
                "password.</p>"
                "<div class=\"notice mt\"><p><b>Where's the setup code?</b> It shows that you're the one setting Hemma "
                "up - not just anyone on your Wi-Fi who opens this page first. You'll find it where Hemma was "
                "installed:</p><ul class=\"small\">"
                "<li><b>Raspberry Pi or Linux</b>: in the installer's last lines. Missed them? It's also in "
                "<code>/etc/hemma/hemma.env</code>, or run <code>sudo journalctl -u hemma | grep \"setup code\"</code></li>"
                "<li><b>Windows or Mac</b>: in Hemma's own window (or its log), right after it starts</li></ul>"
                "<p class=\"small\">It works once: after this the welcome page and the code are gone for good.</p></div>");
    if (others_can_sign_in) buf_puts(b, "<p class=\"small center mt\"><a href=\"/login\">I already have an account</a></p>");
}

/* Signed-out page: no names of the household on it (review). */
static void forgot_page(Ctx *c, Buf *b) {
    ctx_title(c, "Forgot password");
    buf_puts(b, "<h1>Forgot your password?</h1><p class=\"sub\">Use one of your recovery codes to choose a new one.</p>");
    error_notice(c, b);
    const char *u = ctx_q(c, "u");
    buf_puts(b, "<form method=\"post\" action=\"/forgot\" class=\"stack\">");
    H(b,
      "<label class=\"field\"><span>Username or name</span><input name=\"username\" autocomplete=\"username\" "
      "autocapitalize=\"none\" autocorrect=\"off\" spellcheck=\"false\" required maxlength=\"48\" value=\"%s\"%r>"
      "</label><label class=\"field\"><span>Recovery code</span><input name=\"code\" autocomplete=\"off\" "
      "autocapitalize=\"characters\" autocorrect=\"off\" spellcheck=\"false\" required maxlength=\"40\" "
      "placeholder=\"XXXX-XXXX-XXXX\"%r></label>",
      u, *u ? "" : " autofocus", *u ? " autofocus" : "");
    password_pair(b, "New password");
    remember_box(b);
    buf_puts(b, "<button class=\"btn primary big\">Set the new password</button></form>"
                "<p class=\"muted small center mt\">No codes left? Ask an admin of your household to set a new password "
                "for you - or whoever runs the Hemma server can reset it there (see Hemma's account docs).</p>"
                "<p class=\"small center mt\"><a href=\"/login\">Back to sign in</a></p>");
}

static void signin_page(Ctx *c, Buf *b) {
    ctx_title(c, "Sign in");
    Person *guess = person_get(presence_guess(c->ip));
    const char *u = ctx_q(c, "u");
    const char *next = ctx_q(c, "next");
    if (!*next) next = str_starts(c->path, "/login") ? "/" : c->url;
    bool welcome_back = false;
    if (!*u && guess && guess->username[0] && !*ctx_q(c, "other")) {
        u = guess->username;
        welcome_back = true;
    }
    if (welcome_back) H(b, "<h1>Welcome back, %s</h1>", guess->name);
    else buf_puts(b, "<h1>Sign in</h1>");
    H(b, "<p class=\"sub\">to %s</p>", app_name());
    if (rescued()) /* (issue #87: the details are on the System page, for admins) */
        buf_puts(b, "<p class=\"notice\">Hemma's data was damaged, and it rescued what it could still read: some "
                    "things may be missing.</p>");
    /* the guess is this address's last person: on a shared or reassigned device that's someone
       else (issue #3), so signing in as another person is one tap away */
    if (welcome_back)
        H(b, "<p class=\"small center\">Not %s? <a href=\"/login?other=1&amp;next=%u\">Sign in as someone else</a></p>",
          guess->name, next);
    long long e = ctx_qint(c, "e", 0);
    if (e == 1) buf_puts(b, "<p class=\"notice bad\" role=\"alert\">That username and password don't match.</p>");
    else if (e == 2) wait_notice(c, b);
    else if (e == 3) buf_puts(b, "<p class=\"notice\">You're signed out.</p>");
    else if (e == 4) buf_puts(b, "<p class=\"notice\">This Hemma already has an admin. Sign in, or ask them to add you.</p>");
    buf_puts(b, "<form method=\"post\" action=\"/login\" class=\"stack\">");
    ui_hidden(b, "next", next);
    H(b,
      "<label class=\"field\"><span>Username or name</span><input name=\"username\" autocomplete=\"username\" "
      "autocapitalize=\"none\" autocorrect=\"off\" spellcheck=\"false\" required maxlength=\"48\" value=\"%s\"%r>"
      "</label><label class=\"field\"><span>Password</span><input type=\"password\" name=\"password\" "
      "autocomplete=\"current-password\" required maxlength=\"200\"%r></label>",
      u, *u ? "" : " autofocus", *u ? " autofocus" : "");
    remember_box(b);
    buf_puts(b, "<button class=\"btn primary big\">Sign in</button></form>");
    /* the username comes along only when it was typed: the address-based guess stays on this page */
    const char *typed = welcome_back ? "" : u;
    H(b, "<p class=\"small center mt\"><a href=\"/forgot%r%u\">Forgot your password?</a></p>", *typed ? "?u=" : "",
      typed);
}

void auth_page(Ctx *c, Buf *b) {
    c->bare = true;
    buf_puts(b, "<div class=\"center-page\"><div class=\"welcome card signin\"><div class=\"welcome-logo\">");
    ui_icon(b, "logo");
    buf_puts(b, "</div>");
    bool can = anyone_can_sign_in();
    if (!auth_ready()) no_salt_page(c, b);
    else if (rescued() && !admin_can_sign_in() && !(can && str_starts(c->path, "/login"))) damaged_page(c, b, can);
    else if (auth_setup_open() && !(can && str_starts(c->path, "/login"))) setup_page(c, b, can);
    else if (!can) locked_page(c, b);
    else if (str_eq(c->path, "/forgot")) forgot_page(c, b);
    else signin_page(c, b);
    buf_puts(b, "</div></div>");
}

/* ------------------------------------------------------------------ HTTP */

/* Only visible ASCII without backslashes: browsers drop tabs and newlines from URLs and
   read "\" as "/", so "/\t/evil.example" or "/\evil.example" would leave the site. */
void auth_safe_next(const char *in, char *out, size_t cap) {
    bool ok = in && in[0] == '/' && in[1] != '/' && strlen(in) < cap && !str_starts(in, "/login") &&
              !str_starts(in, "/logout");
    for (const char *p = in; ok && *p; p++)
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7F || *p == '\\') ok = false;
    str_copy(out, cap, ok ? in : "/");
}

static void redirect(Conn *conn, const char *location, const char *cookie) {
    Buf extra = {0};
    buf_printf(&extra, "Location: %s\r\nCache-Control: no-store\r\n%s", location, cookie ? cookie : "");
    conn_reply(conn, 303, "text/plain", "", 0, extra.p);
    buf_free(&extra);
}

/* Signs `person` in on this browser with a fresh session (any other session cookie it
   carried ends: no session fixation). Writes the Set-Cookie header line into cookie. */
/* The Set-Cookie line for a session: "keep me signed in" lasts a year - an admin's a month,
   the most the server keeps an admin's session (ADMIN_SESSION_DAYS, issue #5). */
static void session_cookie(char *out, size_t cap, const char *sid, int person, bool remember) {
    const Person *p = person_get(person);
    long age = !remember ? 0 : p && p->role == ROLE_ADMIN ? ADMIN_SESSION_DAYS * 86400L : 31536000L;
    char max_age[32] = "";
    if (age) snprintf(max_age, sizeof max_age, " Max-Age=%ld;", age);
    snprintf(out, cap, "Set-Cookie: hemma=%s; Path=/;%s SameSite=Lax; HttpOnly\r\n", sid, max_age);
}

static void fresh_session(HttpReq *r, int person, bool remember, char sid[40], char *cookie, size_t cap) {
    char old[40];
    if (http_cookie(r, "hemma", old, sizeof old)) session_end(old);
    session_create(person, remember, http_header(r, "User-Agent"), sid);
    session_cookie(cookie, cap, sid, person, remember);
}

static const char *password_error(const char *pw, const char *again) {
    if (auth_password_problem(pw)) return utf8_count(pw) < 8 ? "short" : "long";
    return str_eq(pw, again) ? NULL : "again";
}

/* POST /setup: the first admin, from the welcome page. */
static void setup_post(Conn *conn, HttpReq *r, Fields *f) {
    int64_t now = now_sec();
    uint64_t key = thr_key('s', conn->ip, "");
    Thr *e = thr_find(g_pair, PAIR_SLOTS, key);
    if (!auth_setup_open()) { /* someone was quicker - or a crafted POST long after setup */
        log_warn("refused a welcome-page submission from %s: Hemma already has an admin", conn->peer);
        thr_fail(thr_take(g_pair, PAIR_SLOTS, key, 0, now), SETUP_LIMIT, now);
        redirect(conn, "/login?e=4", NULL);
        return;
    }
    char name[64], user[40], home[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    normalize_username(fields_str(f, "username"), user, sizeof user);
    str_copy(home, sizeof home, fields_str(f, "home"));
    str_trim(home);
    const char *pw = fields_str(f, "password");
    int wait = thr_wait(e, now);
    /* every slot holds someone's fresh failures, so this one couldn't be counted: it waits, the
       way sign-in does (issue #5: the setup code's throttle used to fail open) */
    if (!e && thr_full(g_pair, PAIR_SLOTS, now)) wait = MAX(wait, FULL_WAIT_S);
    char want[SETUP_CHARS + 1], got[SETUP_CHARS + 1];
    bool code_ok = setup_code_now(want) && symbols(fields_str(f, "code"), got, SETUP_CHARS) &&
                   ct_equal(want, got, SETUP_CHARS);
    if (wait <= 0 && !code_ok) log_warn("welcome page: a wrong setup code from %s", conn->peer);
    const char *err = wait > 0                   ? "wait"
                      : !code_ok                 ? "setup"
                      : !name[0]                 ? "name"
                      : !auth_username_ok(user)  ? "user"
                      : person_by_username(user) ? "taken"
                                                 : password_error(pw, fields_str(f, "again"));
    int id = 0;
    Made made;
    if (!err) {
        db_begin();
        id = make_admin(user, name, pw, &made);
        if (id) {
            setting_set_int(SET_BOOTSTRAPPED, 1);
            setting_set(SET_SETUP_CODE, ""); /* used: gone for good */
            if (home[0]) setting_set("household.name", home);
        }
        db_commit();
        if (!id) err = "name";
    }
    if (err) {
        if (wait <= 0) thr_fail(thr_take(g_pair, PAIR_SLOTS, key, 0, now), SETUP_LIMIT, now);
        Buf loc = {0};
        buf_printf(&loc, "/setup?e=%s", err);
        if (wait > 0) buf_printf(&loc, "&w=%d", wait);
        buf_puts(&loc, "&name=");
        url_encode(&loc, name, false);
        buf_puts(&loc, "&u=");
        url_encode(&loc, user, false);
        buf_puts(&loc, "&home=");
        url_encode(&loc, home, false);
        redirect(conn, buf_cstr(&loc), NULL);
        buf_free(&loc);
        return;
    }
    if (e) memset(e, 0, sizeof *e);
    char sid[40], cookie[160];
    db_begin();
    fresh_session(r, id, fields_get(f, "remember") != NULL, sid, cookie, sizeof cookie);
    Session shown = {0};
    sha256_hex(sid, 32, shown.token);
    shown.person = id;
    auth_codes_new(id, &shown);
    Person rec = *person_get(id);
    rec.last_login = now;
    tbl_put(&T_person, &rec);
    db_commit();
    log_info("admin \"%s\" created on the welcome page from %s", user, conn->peer);
    redirect(conn, module_find("people") ? "/people/codes" : "/", cookie);
    app_changed(); /* other open welcome pages turn into the sign-in page */
}

/* A typed sign-in name as the log shows it: the account's username when it names one (by
   username or name), else only how long it was. People type their password into the name box
   (a phone fills the wrong one), and every admin reads the log (issue #34). */
static void typed_for_log(const char *typed, char *out, size_t cap) {
    const Person *p = person_for_sign_in(typed);
    if (p) snprintf(out, cap, "\"%s\"", p->username);
    else snprintf(out, cap, "an unknown name (%d characters)", (int)utf8_count(typed));
}

/* POST /forgot: username + recovery code + a new password. */
static void forgot_post(Conn *conn, HttpReq *r, Fields *f) {
    char user[64]; /* a username or a name */
    normalize_username(fields_str(f, "username"), user, sizeof user);
    const char *pw = fields_str(f, "password");
    const char *err = password_error(pw, fields_str(f, "again"));
    int pid = 0, wait = 0;
    if (!err) {
        LoginResult res = auth_recover(user, fields_str(f, "code"), conn->ip, now_sec(), &pid, &wait);
        if (res == LOGIN_THROTTLED) err = "wait";
        else if (res != LOGIN_OK) err = "code";
    }
    if (err) {
        if (str_eq(err, "code")) {
            char who[80];
            typed_for_log(user, who, sizeof who);
            log_info("failed password recovery for %s from %s", who, conn->peer);
        }
        Buf loc = {0};
        buf_printf(&loc, "/forgot?e=%s", err);
        if (wait > 0) buf_printf(&loc, "&w=%d", wait);
        buf_puts(&loc, "&u=");
        url_encode(&loc, user, false);
        redirect(conn, buf_cstr(&loc), NULL);
        buf_free(&loc);
        return;
    }
    char sid[40], cookie[160];
    db_begin();
    int ended = auth_set_password(pid, pw, NULL); /* whoever else was signed in as them is out */
    fresh_session(r, pid, fields_get(f, "remember") != NULL, sid, cookie, sizeof cookie);
    Person *p = person_get(pid);
    if (p) {
        Person rec = *p;
        rec.last_login = now_sec();
        tbl_put(&T_person, &rec);
    }
    db_commit();
    int left = auth_codes_left(pid);
    log_info("\"%s\" chose a new password with a recovery code from %s (%d code%s left, %d session%s ended)", user,
             conn->peer, left, left == 1 ? "" : "s", MAX(ended, 0), ended == 1 ? "" : "s");
    redirect(conn, module_find("people") ? "/people?recovered=1" : "/", cookie);
    app_changed();
}

bool auth_http(Ctx *c, Conn *conn, HttpReq *r) {
    bool login = str_eq(r->path, "/login"), logout = str_eq(r->path, "/logout");
    bool setup = str_eq(r->path, "/setup"), forgot = str_eq(r->path, "/forgot");
    if (!login && !logout && !setup && !forgot) return false;
    if (!str_eq(r->method, "POST")) {
        if (logout) {
            conn_reply(conn, 405, "text/plain", "POST only\n", 10, "Allow: POST\r\n");
            return true;
        }
        if (c->me) { /* already signed in: nothing to do on these pages */
            char next[600];
            if (login) auth_safe_next(ctx_q(c, "next"), next, sizeof next);
            else str_copy(next, sizeof next, forgot && module_find("people") ? "/people" : "/");
            redirect(conn, next, NULL);
            return true;
        }
        return false; /* GET: the normal page flow shows the page */
    }
    if (!http_same_origin(r)) {
        conn_reply(conn, 403, "text/plain", "cross-site request refused\n", 27, NULL);
        return true;
    }
    Fields f = {0};
    fields_parse(&f, r->body, r->body_len);
    if (setup) {
        setup_post(conn, r, &f);
        fields_free(&f);
        return true;
    }
    if (forgot) {
        forgot_post(conn, r, &f);
        fields_free(&f);
        return true;
    }
    if (logout) {
        char sid[40];
        bool had = c->sess && c->sess->token[0]; /* a real session (signed-out visitors get a blank stand-in) */
        if (http_cookie(r, "hemma", sid, sizeof sid)) session_end(sid);
        redirect(conn, "/login?e=3", "Set-Cookie: hemma=; Path=/; Max-Age=0; SameSite=Lax; HttpOnly\r\n");
        if (had) app_changed(); /* their other screens turn into the sign-in page; strangers re-render nothing */
        fields_free(&f);
        return true;
    }
    char next[600], user[64]; /* a username or a name */
    auth_safe_next(fields_str(&f, "next"), next, sizeof next);
    normalize_username(fields_str(&f, "username"), user, sizeof user);
    int pid = 0, wait = 0, cookie_person = c->me && c->sess && c->sess->token[0] ? c->me->id : 0;
    LoginResult res = auth_login(user, fields_str(&f, "password"), conn->ip, now_sec(), &pid, &wait);
    if (res == LOGIN_OK) {
        char old[40] = "", sid[40];
        bool remember = fields_get(&f, "remember") != NULL;
        bool had = http_cookie(r, "hemma", old, sizeof old);
        bool sid_shaped = strlen(old) == 32 && strspn(old, "0123456789abcdef") == 32;
        if (had && sid_shaped && cookie_person && cookie_person == pid) {
            /* already signed in as this person (another tab signed in first): keep that
               session so the other tab stays signed in; just honour "keep me signed in" */
            Session s = *c->sess;
            s.remember = remember;
            s.seen = s.created = now_sec(); /* the password again: an admin's 30 days start over */
            tbl_put(&T_session, &s);
            str_copy(sid, sizeof sid, old);
        } else {
            if (had) session_end(old); /* someone else's or stale: a fresh id (no session fixation) */
            session_create(pid, remember, http_header(r, "User-Agent"), sid);
        }
        char cookie[160];
        session_cookie(cookie, sizeof cookie, sid, pid, remember);
        Person *p = person_get(pid);
        log_info("%s signed in from %s", p ? p->username : "?", conn->peer);
        /* a temporary password from the server: straight to choosing their own */
        if (auth_temporary(pid) && module_find("people")) str_copy(next, sizeof next, "/people");
        redirect(conn, next, cookie);
        app_changed();
    } else {
        Buf loc = {0};
        bool wait_more = res == LOGIN_THROTTLED || res == LOGIN_BUSY;
        buf_printf(&loc, "/login?e=%d&u=", wait_more ? 2 : 1);
        url_encode(&loc, user, false);
        if (wait_more) buf_printf(&loc, "&w=%d%s", wait, res == LOGIN_BUSY ? "&busy=1" : "");
        buf_puts(&loc, "&next=");
        url_encode(&loc, next, false);
        if (res == LOGIN_WRONG) {
            char who[80];
            typed_for_log(user, who, sizeof who);
            log_info("failed sign-in for %s from %s%s", who, conn->peer,
                     auth_salt_mismatch() ? " (HEMMA_AUTH_SALT isn't the passwords' secret - see the warning at start)" : "");
        }
        redirect(conn, buf_cstr(&loc), NULL);
        buf_free(&loc);
    }
    fields_free(&f);
    return true;
}
