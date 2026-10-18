/* modules/screens — screens an admin puts together out of what the other modules offer: the TV
 * view (big rows you scroll down with the remote: Continue watching, On TV, Series, Movies...)
 * and walls (a tablet in the hall: a big clock, now and next, who's home, then cards - Today,
 * Bookings, Coming days, the shopping list, notes... - as tall as what they show, in columns).
 * One of each is made once (TV_ROWS, WALL_CARDS); "/wall", the old wall's address, opens the
 * first wall.
 *
 * A screen is a list of rows. Each row names a Block some module offers ("media.continue",
 * core/app.h), what it shows when the block has choices (one series out of the library) and,
 * optionally, a heading of its own. /screens/<id> draws them full-screen and dark; a row whose
 * block has nothing to show right now - or whose module isn't there any more - is left out,
 * never an error. Admins arrange rows with plain buttons (Up, Down, Remove: no dragging, which
 * neither a remote nor a phone does well) and tick the display accounts whose "/" opens the
 * screen (PREF_START).
 *
 * Who's watching? A TV signs in once with a display account. A TV screen opened there first
 * shows everyone's avatar; the pick (screens.viewer, one of the few actions a display account
 * may run) is whose progress Continue watching shows and the player saves. Nobody types a
 * password: on the TV that person can only watch. The pick lapses once the TV has gone
 * SCREENS_IDLE_S (4 hours) without being used - nothing opened, pressed or played on it: the
 * session's "seen", which every request, button and a playing video's progress report moves -
 * so whoever turns it on next is asked again instead of watching as someone else. "Switch" asks
 * at once. People on their own phone or computer are always their own viewer, never asked. */
#include "screens.h"
#include "../../core/auth.h"

static const FieldDef screen_fields[] = {
    FIELD(Screen, id, F_INT),         FIELD(Screen, name, F_STR),          FIELD(Screen, kind, F_INT),
    FIELD(Screen, created, F_I64),    FIELD(Screen, created_by, F_INT),
};
Table T_screen = TABLE("screen", Screen, screen_fields);

static const FieldDef row_fields[] = {
    FIELD(ScreenRow, id, F_INT),    FIELD(ScreenRow, screen, F_INT), FIELD(ScreenRow, pos, F_INT),
    FIELD(ScreenRow, block, F_STR), FIELD(ScreenRow, arg, F_STR),    FIELD(ScreenRow, title, F_STR),
};
Table T_screen_row = TABLE("screen_row", ScreenRow, row_fields);

/* ================================================================== data */

Screen *screen_get(int id) { return id > 0 ? tbl_get(&T_screen, id) : NULL; }

void screens_url(int screen, char out[32]) { snprintf(out, 32, "/screens/%d", screen); }

static int by_pos(const void *a, const void *b) {
    const ScreenRow *x = *(const ScreenRow *const *)a, *y = *(const ScreenRow *const *)b;
    return x->pos != y->pos ? (x->pos < y->pos ? -1 : 1) : (x->id < y->id ? -1 : x->id > y->id);
}

int screen_rows(int screen, ScreenRow **out, int max) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_screen_row) && n < max; i++) {
        ScreenRow *r = tbl_at(&T_screen_row, i);
        if (r->screen == screen) out[n++] = r;
    }
    qsort(out, (size_t)n, sizeof *out, by_pos);
    return n;
}

int screens_choices(Ctx *c, const Block *bl, BlockChoice **out) {
    *out = NULL;
    if (!bl || !bl->choices) return 0;
    BlockChoice *ch = xcalloc(SCREENS_MAX_CHOICES, sizeof *ch);
    int n = CLAMP(bl->choices(c, ch, SCREENS_MAX_CHOICES), 0, SCREENS_MAX_CHOICES);
    if (!n) {
        free(ch);
        return 0;
    }
    *out = ch;
    return n;
}

/* Positions 1..n in the current order (moves need neighbours that differ). Ids, not pointers:
   every put may move the rows. */
static void renumber(int screen) {
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int ids[SCREENS_MAX_ROWS], n = screen_rows(screen, rows, SCREENS_MAX_ROWS);
    for (int i = 0; i < n; i++) ids[i] = rows[i]->id;
    for (int i = 0; i < n; i++) {
        ScreenRow *r = tbl_get(&T_screen_row, ids[i]);
        if (!r || r->pos == i + 1) continue;
        ScreenRow rec = *r;
        rec.pos = i + 1;
        tbl_put(&T_screen_row, &rec);
    }
}

/* A screen to start from, with those of `blocks` whose module is here. */
static void seed(const char *name, int kind, const char *const *blocks, int n) {
    Screen s = {0};
    str_copy(s.name, sizeof s.name, name);
    s.kind = kind;
    s.created = now_sec();
    tbl_put(&T_screen, &s);
    int pos = 0;
    for (int i = 0; i < n; i++) {
        const Block *bl = block_find(blocks[i], NULL);
        if (!bl || !(bl->kinds & kind)) continue;
        ScreenRow r = {0};
        r.screen = s.id;
        r.pos = ++pos;
        str_copy(r.block, sizeof r.block, blocks[i]);
        tbl_put(&T_screen_row, &r);
    }
}

/* The TV: the rows a streaming app opens with. */
static const char *const TV_ROWS[] = {"media.continue", "tv.onnow", "media.recent", "media.series", "media.movies"};
/* A wall: the day first (today, bookings, what's coming), then the list and the notes, then what's
   on and what's new. */
static const char *const WALL_CARDS[] = {"plan.today", "book.now",     "plan.days",    "plan.shop",
                                         "board.notes", "tv.onnow",    "media.recent", "home.latest"};

int screens_first(int kind) {
    for (int i = 0; i < tbl_count(&T_screen); i++) {
        const Screen *s = tbl_at(&T_screen, i);
        if (s->kind == kind) return s->id;
    }
    return 0;
}

/* Where a display account no screen claims starts: the first wall (issue #53), kept for the core. */
static void display_default(void) {
    char url[32] = "";
    int wall = screens_first(SCREEN_WALL);
    if (wall) screens_url(wall, url);
    if (!str_eq(setting(SETTING_DISPLAY_START, ""), url)) setting_set(SETTING_DISPLAY_START, url);
}

/* ======================================================= who's watching */

/* What's known about each TV's pick: when it was last in use. Memory only - after a restart the
   sessions' own "seen" (saved hourly) stands in. */
typedef struct {
    int account;    /* the display account */
    char token[65]; /* its session: one TV's browser */
    int64_t active; /* last known in use */
} Seat;
static Seat g_seats[64];
static int g_nseats;

static Seat *seat_find(int account, const char *token) {
    for (int i = 0; i < g_nseats; i++)
        if (g_seats[i].account == account && str_eq(g_seats[i].token, token)) return &g_seats[i];
    return NULL;
}

static void seat_drop(int account, const char *token) {
    Seat *s = seat_find(account, token);
    if (s) *s = g_seats[--g_nseats];
}

static Seat *seat_set(int account, const char *token, int64_t active) {
    Seat *s = seat_find(account, token);
    if (!s && g_nseats < countof(g_seats)) s = &g_seats[g_nseats++];
    if (!s) { /* full: the one unused the longest makes room */
        s = &g_seats[0];
        for (int i = 1; i < g_nseats; i++)
            if (g_seats[i].active < s->active) s = &g_seats[i];
    }
    s->account = account;
    str_copy(s->token, sizeof s->token, token);
    s->active = active;
    return s;
}

/* A session met for the first time (picked elsewhere, or before a restart): in use when it was
   last seen. */
static int64_t first_use(const Session *s, int64_t now) {
    int64_t at = MAX(s->seen, s->created);
    return at > 0 && at <= now ? at : now;
}

/* Has the pick lapsed? `seen` = when the TV's session was last used. Use within SCREENS_IDLE_S
   of the last known use keeps the pick; use after a longer break - the TV turned on next
   morning - doesn't: that's someone else, probably. Both checks matter: the core moves "seen"
   before the page that asks renders. */
static bool seat_lapsed(Seat *s, int64_t seen, int64_t now) {
    if (seen > s->active) {
        if (seen - s->active > SCREENS_IDLE_S) return true;
        s->active = seen;
    }
    return now - s->active > SCREENS_IDLE_S;
}

int screens_viewer(Ctx *c) {
    int v = ctx_viewer(c);
    if (!v || !c->me || c->me->role != ROLE_DISPLAY || !c->sess) return v; /* people watch as themselves */
    if (!screens_on_picker(person_get(v))) { /* (off the list since: an admin who unticked it - issue #90) */
        ctx_set_viewer(c, 0);
        return 0;
    }
    Seat *s = seat_find(c->me->id, c->sess->token);
    if (!s) s = seat_set(c->me->id, c->sess->token, first_use(c->sess, c->now));
    if (!seat_lapsed(s, c->sess->seen, c->now)) return v;
    seat_drop(c->me->id, c->sess->token);
    ctx_set_viewer(c, 0);
    return 0;
}

static bool seat_live(const Seat *st) {
    for (int i = 0; i < tbl_count(&T_session); i++) {
        const Session *s = tbl_at(&T_session, i);
        if (s->person == st->account && s->viewer && str_eq(s->token, st->token)) return true;
    }
    return false;
}

/* Every TV with somebody picked: note its use, and end the picks that lapsed - the screen then
   asks by itself (also when the TV sits on another page, say a paused film). */
static void seats_watch(int64_t now) {
    bool changed = false;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        Person *p = s->viewer ? person_get(s->person) : NULL;
        if (!p || p->role != ROLE_DISPLAY) continue;
        Seat *st = seat_find(p->id, s->token);
        if (!st) st = seat_set(p->id, s->token, first_use(s, now));
        if (!seat_lapsed(st, s->seen, now)) continue;
        seat_drop(p->id, s->token);
        Session rec = *s; /* an update: the row stays where it is */
        rec.viewer = 0;
        tbl_put(&T_session, &rec);
        changed = true;
    }
    for (int i = g_nseats - 1; i >= 0; i--) /* signed out, or nobody picked any more */
        if (!seat_live(&g_seats[i])) g_seats[i] = g_seats[--g_nseats];
    if (changed) app_changed();
}

/* ================================================================ actions */

static bool admin_only(Ctx *c) {
    if (is_admin(c)) return true;
    ctx_fail(c, "Only an admin can change screens");
    return false;
}

static Screen *screen_arg(Ctx *c, Fields *f) {
    Screen *s = screen_get((int)fields_int(f, "id", 0));
    if (!s) ctx_fail(c, "That screen is gone");
    return s;
}

static ScreenRow *row_arg(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "row", 0);
    ScreenRow *r = id > 0 ? tbl_get(&T_screen_row, id) : NULL;
    if (!r || !screen_get(r->screen)) {
        ctx_fail(c, "That row is gone");
        return NULL;
    }
    return r;
}

static bool name_arg(Ctx *c, Fields *f, char *out, size_t cap) {
    str_copy(out, cap, fields_str(f, "name"));
    str_trim(out);
    if (out[0]) return true;
    ctx_fail_at(c, "name", "Give the screen a name");
    return false;
}

/* The pick for a block with choices: `want` when it's on offer, else the first one ("" = take the
   first). False when there's nothing to pick from, or `want` isn't on offer. */
static bool pick_arg(Ctx *c, const Block *bl, const char *want, char out[64]) {
    out[0] = '\0';
    if (!bl->choices) return true;
    BlockChoice *ch;
    int n = screens_choices(c, bl, &ch);
    bool ok = false;
    for (int i = 0; i < n && !ok; i++)
        if (!want[0] || str_eq(ch[i].arg, want)) {
            str_copy(out, 64, ch[i].arg);
            ok = true;
        }
    free(ch);
    if (!ok) ctx_fail(c, n ? "That isn't on offer any more - pick another" : "There's nothing to pick for that row yet");
    return ok;
}

bool screens_drives_tv(int account) {
    const char *start = pref(account, PREF_START, "");
    const Screen *s = str_starts(start, "/screens/") ? screen_get((int)str_int_or(start + 9, 0)) : NULL;
    return s && s->kind == SCREEN_TV;
}

/* A display account that no longer drives a TV (unticked, its screen deleted or made a wall
   tablet's) keeps nobody's watching: its sessions forget the pick. */
static void forget_viewer(int account) {
    if (screens_drives_tv(account)) return;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        if (s->person != account || !s->viewer) continue;
        Session rec = *s;
        rec.viewer = 0;
        tbl_put(&T_session, &rec);
    }
}

/* TVs watching as `person` - or, with 0, as anyone no longer on the picker (issue #90) - ask again. */
static void forget_viewer_of(int person) {
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        if (!s->viewer || (person ? s->viewer != person : screens_on_picker(person_get(s->viewer)))) continue;
        Person *acc = person_get(s->person);
        if (!acc || acc->role != ROLE_DISPLAY) continue; /* (people watch as themselves) */
        Session rec = *s;
        rec.viewer = 0;
        tbl_put(&T_session, &rec);
    }
}

static void forget_viewers(void) {
    for (int i = 0; i < account_count(); i++)
        if (account_at(i)->role == ROLE_DISPLAY) forget_viewer(account_at(i)->id);
    forget_viewer_of(0);
}

bool screens_on_picker(const Person *p) {
    if (!p || p->role == ROLE_DISPLAY) return false;
    return p->role != ROLE_ADMIN || str_eq(pref(p->id, PREF_ON_TV, ""), "1");
}

bool screens_has_pin(int person) { return pref(person, PREF_PIN, "")[0] != 0; }

/* A PIN is kept as an HMAC with the server's secret (a copy of the database alone tells nothing). */
static bool pin_hash(int person, const char *pin, char out[33]) {
    char msg[48];
    uint8_t mac[32];
    snprintf(msg, sizeof msg, "pin:%d:%s", person, pin);
    if (!auth_mac(msg, strlen(msg), mac)) return false;
    hex_encode(mac, 16, out);
    return true;
}

static bool four_digits(const char *s) { return strlen(s) == 4 && strspn(s, "0123456789") == 4; }

/* Wrong guesses lock that person's PIN a while - 5 free, then 1, 2, 4 ... 15 minutes - on every TV:
   10,000 PINs at that pace take days. Forgiven after an hour without one. */
#define PIN_SLOTS 32
static struct {
    int person, fails;
    int64_t until, last;
} g_pin[PIN_SLOTS];

static int pin_slot(int person, int64_t now) {
    int k = 0;
    for (int i = 0; i < PIN_SLOTS; i++) {
        if (g_pin[i].person == person) {
            if (now - g_pin[i].last > 3600) g_pin[i].fails = 0, g_pin[i].until = 0;
            return i;
        }
        if (g_pin[i].last < g_pin[k].last) k = i;
    }
    memset(&g_pin[k], 0, sizeof g_pin[k]);
    g_pin[k].person = person;
    return k;
}

int screens_pin_wait(int person, int64_t now) {
    for (int i = 0; i < PIN_SLOTS; i++)
        if (g_pin[i].person == person && g_pin[i].until > now) return (int)(g_pin[i].until - now);
    return 0;
}

static void pin_failed(int person, int64_t now) {
    int k = pin_slot(person, now);
    g_pin[k].last = now;
    if (++g_pin[k].fails >= 5) {
        int64_t lock = 60LL << MIN(g_pin[k].fails - 5, 4);
        g_pin[k].until = now + MIN(lock, 900);
    }
}

static void pin_passed(int person, int64_t now) {
    int k = pin_slot(person, now);
    memset(&g_pin[k], 0, sizeof g_pin[k]);
}

/* "Who's watching?" on a TV: who=<person>, or who=0 to ask again (Switch). People on their own
   devices always watch as themselves: nothing to pick. Only a display account that drives a TV
   picks (issue #33: any display account - the hallway tablet too - could pick anyone, then read,
   rewrite or clear their Continue watching, for as long as its year-long sign-in lasted). */
static void act_viewer(Ctx *c, Fields *f) {
    int who = (int)fields_int(f, "who", 0);
    if (who && !person_real(who)) {
        ctx_fail(c, "Pick someone from the list");
        return;
    }
    if (c->me->role != ROLE_DISPLAY || !c->sess) return;
    if (who && !screens_drives_tv(c->me->id)) {
        ctx_fail(c, "Only a TV picks who's watching: an admin ticks it under Screens, Edit, Shows on");
        return;
    }
    if (who && !screens_on_picker(person_get(who))) { /* (issue #90: admins only when they said so) */
        ctx_fail(c, "Pick someone from the list");
        return;
    }
    if (who && screens_has_pin(who)) {
        int wait = screens_pin_wait(who, c->now);
        char want[33];
        if (wait > 0) {
            ctx_fail(c, "Too many wrong PINs - try again in %d min", (wait + 59) / 60);
            return;
        }
        if (!pin_hash(who, fields_str(f, "pin"), want) || !str_eq(want, pref(who, PREF_PIN, ""))) {
            pin_failed(who, c->now);
            ctx_fail_at(c, "pin", "That PIN isn't right");
            return;
        }
        pin_passed(who, c->now);
    }
    ctx_set_viewer(c, who);
    if (who) seat_set(c->me->id, c->sess->token, c->now);
    else seat_drop(c->me->id, c->sess->token);
}

/* Your own place on the TV's picker (issue #90): an admin's "Show me on the TV", and anyone's PIN. */
static void act_on_tv(Ctx *c, Fields *f) {
    bool on = str_eq(fields_str(f, "on"), "1");
    pref_set(c->me->id, PREF_ON_TV, on ? "1" : "");
    if (!on) forget_viewer_of(c->me->id);
    ctx_ok(c, on ? "You're on the TV's Who's watching? now" : "You're off the TV's Who's watching?");
}

static void act_pin(Ctx *c, Fields *f) {
    if (str_eq(fields_str(f, "clear"), "1")) {
        pref_set(c->me->id, PREF_PIN, "");
        ctx_ok(c, "No PIN any more: the TV picks you straight away");
        return;
    }
    const char *pin = fields_str(f, "pin");
    char h[33];
    if (!four_digits(pin)) {
        ctx_fail_at(c, "pin", "A PIN is 4 digits");
        return;
    }
    if (!pin_hash(c->me->id, pin, h)) {
        ctx_fail(c, "Hemma has no password secret yet, so it can't keep a PIN (System says how to add one)");
        return;
    }
    pref_set(c->me->id, PREF_PIN, h);
    forget_viewer_of(c->me->id); /* (a TV showing you now asks again, with the PIN) */
    ctx_ok(c, "PIN saved: the TV asks for it when you're picked");
}

static void act_create(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Screen s = {0};
    if (!name_arg(c, f, s.name, sizeof s.name)) return;
    if (tbl_count(&T_screen) >= SCREENS_MAX) {
        ctx_fail(c, "That's %d screens already - remove one first", SCREENS_MAX);
        return;
    }
    s.kind = str_eq(fields_str(f, "kind"), "wall") ? SCREEN_WALL : SCREEN_TV;
    s.created = c->now;
    s.created_by = c->me->id;
    tbl_put(&T_screen, &s);
    display_default();
    ctx_ok(c, "%s is ready for its rows", s.name);
    ctx_redirect(c, "/screens/%d/edit", s.id);
}

static void act_save(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Screen *s = screen_arg(c, f);
    if (!s) return;
    Screen rec = *s;
    if (!name_arg(c, f, rec.name, sizeof rec.name)) return;
    tbl_put(&T_screen, &rec);
    ctx_ok(c, "Saved");
}

static void act_delete(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Screen *s = screen_arg(c, f);
    if (!s) return;
    int id = s->id;
    char name[48], url[32];
    str_copy(name, sizeof name, s->name);
    screens_url(id, url);
    for (int i = tbl_count(&T_screen_row) - 1; i >= 0; i--) {
        ScreenRow *r = tbl_at(&T_screen_row, i);
        if (r->screen == id) tbl_del(&T_screen_row, r->id);
    }
    for (int i = 0; i < account_count(); i++) /* displays that opened it: the household's wall again */
        if (str_eq(pref(account_at(i)->id, PREF_START, ""), url)) pref_set(account_at(i)->id, PREF_START, "");
    tbl_del(&T_screen, id);
    display_default();
    forget_viewers();
    ctx_ok(c, "%s is gone", name);
    ctx_redirect(c, "/screens");
}

static void act_row_add(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Screen *s = screen_arg(c, f);
    if (!s) return;
    int screen = s->id, kind = s->kind;
    const char *id = fields_str(f, "block");
    const Module *owner = NULL;
    const Block *bl = block_find(id, &owner);
    if (!bl || !(bl->kinds & kind)) {
        ctx_fail(c, "That row isn't available here");
        return;
    }
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(screen, rows, SCREENS_MAX_ROWS);
    if (n >= SCREENS_MAX_ROWS) {
        ctx_fail(c, "A screen holds %d rows at most", SCREENS_MAX_ROWS);
        return;
    }
    ScreenRow r = {0};
    if (!pick_arg(c, bl, fields_str(f, "arg"), r.arg)) return;
    r.screen = screen;
    r.pos = n ? rows[n - 1]->pos + 1 : 1;
    str_copy(r.block, sizeof r.block, bl->id);
    tbl_put(&T_screen_row, &r);
    ctx_ok(c, "Added %s", bl->title);
}

static void act_row_save(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    ScreenRow *r = row_arg(c, f);
    if (!r) return;
    ScreenRow rec = *r;
    str_copy(rec.title, sizeof rec.title, fields_str(f, "title"));
    str_trim(rec.title);
    const Block *bl = block_find(rec.block, NULL);
    if (bl && bl->choices && fields_get(f, "arg") && !pick_arg(c, bl, fields_str(f, "arg"), rec.arg)) return;
    tbl_put(&T_screen_row, &rec);
    ctx_ok(c, "Saved");
}

static void act_row_move(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    ScreenRow *r = row_arg(c, f);
    if (!r) return;
    int id = r->id, screen = r->screen, step = str_eq(fields_str(f, "dir"), "up") ? -1 : 1;
    renumber(screen);
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(screen, rows, SCREENS_MAX_ROWS), at = -1;
    for (int i = 0; i < n; i++)
        if (rows[i]->id == id) at = i;
    if (at < 0 || at + step < 0 || at + step >= n) return; /* already first / last */
    ScreenRow a = *rows[at], b = *rows[at + step];
    int pos = a.pos;
    a.pos = b.pos;
    b.pos = pos;
    tbl_put(&T_screen_row, &a);
    tbl_put(&T_screen_row, &b);
}

static void act_row_remove(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    ScreenRow *r = row_arg(c, f);
    if (!r) return;
    int screen = r->screen;
    tbl_del(&T_screen_row, r->id);
    renumber(screen);
    ctx_ok(c, "Removed");
}

/* "Shows on": every ticked display account opens this screen at its "/" (one screen each, so a
   tick here moves it from another); unticked ones that did open it get their home page back. */
static void act_shows(Ctx *c, Fields *f) {
    if (!admin_only(c)) return;
    Screen *s = screen_arg(c, f);
    if (!s) return;
    char url[32];
    screens_url(s->id, url);
    for (int i = 0; i < account_count(); i++) {
        int id = account_at(i)->id;
        if (account_at(i)->role != ROLE_DISPLAY) continue;
        bool on = false;
        for (int k = 0; k < f->n; k++) on |= str_eq(f->k[k], "acc") && str_int_or(f->v[k], 0) == id;
        if (on) pref_set(id, PREF_START, url);
        else if (str_eq(pref(id, PREF_START, ""), url)) pref_set(id, PREF_START, "");
    }
    forget_viewers();
    ctx_ok(c, "Saved");
}

static const Action actions[] = {
    {"screens.viewer", act_viewer, false, true}, /* the one thing a TV may do here */
    {"screens.on_tv", act_on_tv}, {"screens.pin", act_pin},
    {"screens.create", act_create},
    {"screens.save", act_save},
    {"screens.delete", act_delete},
    {"screens.row_add", act_row_add},
    {"screens.row_save", act_row_save},
    {"screens.row_move", act_row_move},
    {"screens.row_remove", act_row_remove},
    {"screens.shows", act_shows},
    {0},
};

/* ============================================================ the address */

/* Where a TV or tablet opens Hemma, for the "On the TV" steps (issue #61: they said to type the
   address and never showed it). app_addresses() asks the OS - not for every render: once a minute
   from tick (a DHCP lease can change it). */
static char g_addr[2][96];
static int g_naddr;
static int64_t g_addr_at; /* monotonic ms; 0 = not asked yet */
static bool g_addr_fixed; /* tests */

static void refresh_addresses(int64_t now) {
    g_addr_at = now ? now : 1;
    if (!g_addr_fixed) g_naddr = app_addresses(g_addr, countof(g_addr));
}

int screens_addresses(char out[2][96]) {
    for (int i = 0; i < g_naddr; i++) str_copy(out[i], sizeof out[i], g_addr[i]);
    return g_naddr;
}

void screens_test_addresses(const char *a, const char *b) {
    g_addr_fixed = a != NULL;
    g_naddr = 0;
    g_addr_at = 0;
    if (a && *a) str_copy(g_addr[g_naddr++], sizeof g_addr[0], a);
    if (a && *a && b && *b) str_copy(g_addr[g_naddr++], sizeof g_addr[0], b);
}

/* ================================================================ module */

static void init(void) {
    T_screen.what = "screens";
    store_register(&T_screen);
    T_screen_row.what = "screen rows";
    store_register(&T_screen_row);
}

static void start(void) {
    g_nseats = 0;
    if (!g_addr_fixed) {
        g_naddr = 0;
        g_addr_at = 0;
    }
    forget_viewers(); /* (picks made before only TVs could pick) */
    seats_watch(now_sec());
    /* once each: a household that removed them keeps none */
    if (!setting_int("screens.seeded", 0)) {
        if (!tbl_count(&T_screen)) seed("TV", SCREEN_TV, TV_ROWS, countof(TV_ROWS));
        setting_set_int("screens.seeded", 1);
    }
    if (!setting_int("screens.wall_seeded", 0)) { /* (walls came later: a household with a TV gets one too) */
        if (!screens_first(SCREEN_WALL)) seed("Wall", SCREEN_WALL, WALL_CARDS, countof(WALL_CARDS));
        setting_set_int("screens.wall_seeded", 1);
    }
    display_default();
}

/* "/wall" - the old wall display's address (bookmarks, the home page's Wall button): the first
   wall screen, or the list when there's none. */
static bool http(Ctx *c, Conn *conn, HttpReq *r) {
    (void)c;
    if (!str_eq(r->path, "/wall") || (!str_eq(r->method, "GET") && !str_eq(r->method, "HEAD"))) return false;
    char loc[64];
    int id = screens_first(SCREEN_WALL);
    if (id) snprintf(loc, sizeof loc, "Location: /screens/%d\r\n", id);
    else snprintf(loc, sizeof loc, "Location: /screens\r\n");
    conn_reply(conn, 302, "text/plain; charset=utf-8", "", 0, loc);
    return true;
}

static void tick(int64_t now) {
    seats_watch(now_sec());
    if (!app_testing() && !g_addr_fixed && (!g_addr_at || now - g_addr_at >= 60000)) refresh_addresses(now);
}

static const Module screens = {
    .id = "screens",
    .title = "Screens",
    .icon = "monitor",
    .nav = 85, /* after the household's sections, before Household and System */
    .init = init,
    .start = start,
    .tick = tick,
    .page = screens_page,
    .actions = actions,
    .http = http,
};
MODULE(screens)
