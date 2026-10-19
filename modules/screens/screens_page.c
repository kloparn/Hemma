/* modules/screens/screens_page.c — the screens themselves (a TV: big rows for a remote, always
 * dark, overscan-safe; a wall: the same rows as cards) and the builder admins make them with.
 *
 *   /screens            every screen (everyone may open them; admins also make and edit)
 *   /screens/<id>       the screen, full-screen - or "Who's watching?" on a TV nobody picked on
 *   /screens/<id>/edit  the builder (admins)
 *
 * Screens are often a display account's "/" (PREF_START): every link here is absolute. */
#include "screens.h"
#include "../../core/qr.h"
#include "../../core/ui.h"

/* ui_icon has nothing pointing up or down: the builder's Up / Down arrows */
static const char ICON_UP[] = "<svg class=\"i\" viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"m6 15 6-6 6 6\"/></svg>";
static const char ICON_DOWN[] = "<svg class=\"i\" viewBox=\"0 0 24 24\" aria-hidden=\"true\"><path d=\"m6 9 6 6 6-6\"/></svg>";

static const char *kind_name(int kind) { return kind == SCREEN_WALL ? "Wall" : "TV"; }

/* The screen a display account opens at its "/" (NULL: its usual home page). */
static Screen *start_screen(int account) {
    const char *u = pref(account, PREF_START, "");
    return str_starts(u, "/screens/") ? screen_get((int)str_int_or(u + 9, 0)) : NULL;
}

/* "Living room TV, Kitchen" - the display accounts that open this screen; "" for none. */
static void shown_on(int screen, Buf *out) {
    for (int i = 0; i < account_count(); i++) {
        Person *p = account_at(i);
        Screen *s = p->role == ROLE_DISPLAY ? start_screen(p->id) : NULL;
        if (!s || s->id != screen) continue;
        if (out->len) buf_puts(out, ", ");
        buf_puts(out, p->name);
    }
}

/* ================================================================ the list */

/* Step 2 of "On the TV": the address to type, big (issue #61), with http:// - a TV's browser may
   try https and get nowhere - and as a QR code a tablet or phone can scan instead. The numbers
   come first: many TVs can't look up <name>.local. */
static void address_step(Buf *b) {
    char addr[2][96];
    int n = screens_addresses(addr);
    const char *host = app_config()->host;
    if (app_local_only()) { /* listening on this computer only (127.x or localhost - issue #99) */
        H(b, "On its web browser, open Hemma and sign in with it. Hemma only listens on this computer (%s) now, so "
             "a TV can't reach it: start it for the home network (see <a href=\"/system\">System</a>).",
          host);
        return;
    }
    if (!n) {
        buf_puts(b, "On its web browser, open Hemma - type the address this browser shows, with <code>http://</code> "
                    "in front (not <i>localhost</i>) - and sign in with it.");
        return;
    }
    buf_puts(b, "On its web browser, open Hemma at this address - all of it, with <code>http://</code> - and sign in "
                "with it:<div class=\"screens-addr\"><div><p class=\"screens-addr-big\">");
    html_esc(b, addr[0]);
    buf_puts(b, "</p>");
    if (n > 1) {
        buf_puts(b, "<p class=\"muted small\">Phones and computers can use <b>");
        html_esc(b, addr[1]);
        buf_puts(b, "</b> too; many TVs can't.</p>");
    }
    buf_puts(b, "</div>");
    QrCode q;
    if (qr_make(&q, addr[0], strlen(addr[0]), QR_M, -1)) {
        buf_puts(b, "<figure class=\"screens-qr\">");
        qr_svg(b, &q, addr[0]);
        buf_puts(b, "<figcaption class=\"muted small\">A tablet or phone can scan this instead</figcaption></figure>");
    }
    buf_puts(b, "</div>");
}

static void new_modal(Buf *b) {
    ui_modal(b, "New screen", "/screens");
    ui_form(b, "screens.create", "stack");
    buf_puts(b, "<label class=\"field\"><span>Name</span><input name=\"name\" maxlength=\"40\" required "
                "placeholder=\"Living room TV\" autofocus></label>"
                "<div class=\"field\"><span>Kind</span><div class=\"chips\"><label class=\"chip-r\"><input "
                "type=\"radio\" name=\"kind\" value=\"tv\" checked><span>TV</span></label><label class=\"chip-r\"><input "
                "type=\"radio\" name=\"kind\" value=\"wall\"><span>Wall</span></label></div><small class=\"muted\">TV: "
                "big rows you scroll with the remote, like a streaming app. Wall: an always-on overview for a tablet - "
                "the clock, who's home, today, bookings, the shopping list...</small></div>");
    buf_puts(b, "<div class=\"modal-f\"><a class=\"btn ghost\" href=\"/screens\">Cancel</a>"
                "<button class=\"btn primary\">Make it</button></div>");
    ui_form_end(b);
    ui_modal_end(b);
}

/* Your own place on the TV's "Who's watching?" (issue #90): admins choose to be there, anyone can
   ask for a PIN first. */
static void you_card(Ctx *c, Buf *b) {
    if (!c->me || c->me->role == ROLE_DISPLAY) return;
    ui_card(b, "You on the TV", NULL, NULL);
    if (is_admin(c)) {
        bool on = str_eq(pref(c->me->id, PREF_ON_TV, ""), "1");
        ui_form(b, "screens.on_tv", "screens-ontv");
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> Show me on the "
             "TV's <i>Who's watching?</i></label>",
          on ? " checked" : "");
        ui_form_end(b);
        buf_puts(b, "<p class=\"muted small\">Admins are left off unless they tick this: whoever has the TV's sign-in "
                    "could see and clear the watch history of anyone it lists.</p>");
    } else {
        buf_puts(b, "<p>The TV's <i>Who's watching?</i> lists you.</p>");
    }
    if (screens_has_pin(c->me->id)) {
        buf_puts(b, "<p>Your PIN is set: the TV asks for it when you're picked.</p>");
        ui_act(b, "screens.pin", "btn sm", "Remove the PIN", "clear", "1", NULL);
    } else {
        ui_form(b, "screens.pin", "screens-pinset");
        buf_puts(b, "<label class=\"field\"><span>A PIN for the TV <span class=\"muted\">(optional)</span></span><input "
                    "type=\"password\" name=\"pin\" inputmode=\"numeric\" pattern=\"[0-9]{4}\" maxlength=\"4\" "
                    "autocomplete=\"new-password\" placeholder=\"4 digits\"><small class=\"muted\">The TV asks for it "
                    "when someone picks you, so nobody watches - or clears your history - as you.</small></label>"
                    "<button class=\"btn\">Save the PIN</button>");
        ui_form_end(b);
    }
    ui_card_end(b);
}

static void list_page(Ctx *c, Buf *b) {
    ctx_title(c, "Screens");
    ui_head(b, "Screens", "What the TV and the wall tablets show");
    if (is_admin(c)) {
        buf_puts(b, "<a class=\"btn primary\" href=\"/screens?new=1\">");
        ui_icon(b, "plus");
        buf_puts(b, "<span>New screen</span></a>");
    }
    ui_head_end(b);
    int n = tbl_count(&T_screen);
    if (!n) {
        ui_empty(b, "📺", "No screens yet",
                 is_admin(c) ? "Make one and pick its rows: Continue watching, On TV, Series, Movies..."
                             : "An admin can put one together for the TV.");
    } else {
        buf_puts(b, "<section class=\"card\"><ul class=\"rows screens-list\">");
        for (int i = 0; i < n; i++) {
            Screen *s = tbl_at(&T_screen, i);
            ScreenRow *rows[SCREENS_MAX_ROWS];
            int nr = screen_rows(s->id, rows, SCREENS_MAX_ROWS);
            H(b, "<li class=\"row\" id=\"screens-s%d\"><span class=\"screens-kind\">", s->id);
            ui_icon(b, s->kind == SCREEN_WALL ? "grid" : "tv");
            H(b, "</span><div class=\"row-main\"><b>%s</b><small>%s · %d %s%r", s->name, kind_name(s->kind), nr,
              s->kind == SCREEN_WALL ? "card" : "row", nr == 1 ? "" : "s");
            Buf on = {0};
            shown_on(s->id, &on);
            if (on.len) H(b, " · on %s", buf_cstr(&on));
            buf_free(&on);
            H(b, "</small></div><div class=\"screens-list-a\"><a class=\"btn\" href=\"/screens/%d\">Open</a>", s->id);
            if (is_admin(c)) {
                H(b, "<a class=\"btn ghost\" href=\"/screens/%d/edit\">", s->id);
                ui_icon(b, "edit");
                buf_puts(b, "<span>Edit</span></a>");
            }
            buf_puts(b, "</div></li>");
        }
        buf_puts(b, "</ul></section>");
    }
    if (is_admin(c)) {
        ui_card(b, "On the TV", NULL, NULL);
        buf_puts(b, "<ol class=\"steps\"><li>Under <a href=\"/people\">Household</a>, add someone for the TV (or the "
                    "wall tablet) and choose the sign-in kind <b>Display</b>: it can only look.</li><li>");
        address_step(b);
        buf_puts(b, "</li><li>Edit a screen and tick it under <b>Shows on</b>: it opens that screen by itself. A TV "
                    "asks <i>Who's watching?</i> - nobody types a password there.</li></ol>");
        ui_card_end(b);
    }
    you_card(c, b);
    if (is_admin(c) && str_eq(ctx_q(c, "new"), "1")) new_modal(b);
}

/* ============================================================= the screen */

/* Is this screen where "/" takes the display account looking at it? Its own start page, else the
   household's wall (core/app.c does the same). Then Exit would only bring it back here (issue #97). */
static bool display_home_is(const Ctx *c, const char *url) {
    if (c->me->role != ROLE_DISPLAY) return false;
    const char *start = pref(c->me->id, PREF_START, "");
    if (!start[0]) start = setting(SETTING_DISPLAY_START, "");
    return str_eq(start, url);
}

/* Home name, the clock (the core re-renders every minute), and who's watching. */
static void top_bar(Ctx *c, Buf *b, const Screen *s, int viewer) {
    char hm[6], url[32];
    fmt_hm(c->now, hm);
    screens_url(s->id, url);
    bool display = c->me->role == ROLE_DISPLAY, tv = s->kind == SCREEN_TV;
    H(b, "<header class=\"screens-top\"><span class=\"screens-home\">%s</span><time class=\"screens-clock\" "
         "datetime=\"%s\">%s</time><div class=\"screens-who\">",
      app_name(), hm, hm);
    Person *p = tv ? person_get(viewer) : NULL;
    if (p) {
        ui_avatar_deco(b, p, "md");
        H(b, "<b>%s</b>", p->name);
    }
    if (display && tv && screens_drives_tv(c->me->id)) { /* someone else's turn: back to "Who's watching?" */
        ui_form(b, "screens.viewer", "inline");
        ui_hidden(b, "who", "0");
        buf_puts(b, "<button class=\"screens-btn\">Switch</button>");
        ui_form_end(b);
    }
    if (is_admin(c)) H(b, "<a class=\"screens-btn\" href=\"/screens/%d/edit\">Edit</a>", s->id);
    if (!display_home_is(c, url)) /* (a TV's own screen is its home) */
        buf_puts(b, "<a class=\"screens-btn\" href=\"/screens\">Exit</a>");
    buf_puts(b, "</div></header>");
}

/* The household's avatars, big, for the remote: no password - on the TV they can only watch. Admins
   only once they ticked "Show me on the TV", and someone with a PIN gets asked for it (issue #90:
   whoever holds the TV's sign-in could read and clear anyone's watch history). */
static void picker(Ctx *c, Buf *b) {
    ctx_title(c, "Who's watching?");
    H(b, "<div class=\"screens-dark screens-tv screens-pick\" id=\"screens-pick\" data-arrows><div "
         "class=\"screens-pick-in\"><p class=\"screens-home\">%s</p>",
      app_name());
    Person *ask = person_get((int)ctx_qint(c, "pin", 0));
    if (ask && screens_on_picker(ask) && screens_has_pin(ask->id)) { /* the PIN, then the pick */
        H(b, "<h1>%s's PIN</h1>", ask->name);
        int wait = screens_pin_wait(ask->id, c->now);
        if (wait > 0)
            H(b, "<p class=\"screens-note\">Too many wrong PINs - try again in %d min.</p>", (wait + 59) / 60);
        ui_form(b, "screens.viewer", "screens-pinform");
        ui_hidden_int(b, "who", ask->id);
        buf_puts(b, "<input class=\"screens-pin\" type=\"password\" name=\"pin\" inputmode=\"numeric\" "
                    "pattern=\"[0-9]*\" maxlength=\"4\" autocomplete=\"off\" aria-label=\"PIN\" autofocus>"
                    "<button class=\"screens-btn\">OK</button>");
        ui_form_end(b);
        H(b, "<p><a class=\"screens-btn\" href=\"%s\">Back</a></p></div></div>", c->path);
        return;
    }
    buf_puts(b, "<h1>Who's watching?</h1>");
    int shown = 0;
    for (int i = 0; i < person_count(); i++) shown += screens_on_picker(person_at(i));
    if (!shown)
        buf_puts(b, "<p class=\"screens-note\">Nobody's set up yet: add the household on a phone or computer.</p>");
    buf_puts(b, "<div class=\"screens-people\">");
    for (int i = 0, k = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        if (!screens_on_picker(p)) continue;
        if (screens_has_pin(p->id)) { /* (asks for it first) */
            H(b, "<a class=\"screens-person\" id=\"screens-p%d\" href=\"%s?pin=%d\"%r>", p->id, c->path, p->id,
              k++ == 0 ? " autofocus" : "");
            ui_avatar_deco(b, p, "xl");
            H(b, "<b>%s</b></a>", p->name);
            continue;
        }
        H(b, "<form method=\"post\" action=\"/a/screens.viewer\" data-a=\"screens.viewer\" class=\"inline\" "
             "id=\"screens-p%d\"><input type=\"hidden\" name=\"who\" value=\"%d\"><button class=\"screens-person\"%r>",
          p->id, p->id, k++ == 0 ? " autofocus" : "");
        ui_avatar_deco(b, p, "xl");
        H(b, "<b>%s</b></button></form>", p->name);
    }
    buf_puts(b, "</div></div></div>");
}

/* The screen's rows that have something to show right now, in order: a TV's rows of tiles, or a
   wall's cards. Returns how many. */
static int draw_rows(Ctx *c, Buf *b, const Screen *s, int *total) {
    bool wall = s->kind == SCREEN_WALL;
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(s->id, rows, SCREENS_MAX_ROWS), shown = 0;
    *total = n;
    for (int i = 0; i < n; i++) {
        const Module *owner = NULL;
        const Block *bl = block_find(rows[i]->block, &owner);
        if (!bl || !(bl->kinds & s->kind) || !module_visible(owner, c)) continue; /* gone, or not for this screen */
        Buf row = {0};
        if (bl->render(c, &row, s->kind, rows[i]->arg)) {
            const char *title = rows[i]->title[0] ? rows[i]->title : bl->title;
            if (wall) H(b, "<section class=\"screens-card\" id=\"screens-r%d\"><h2>%s</h2>", rows[i]->id, title);
            else H(b, "<section class=\"screens-row\" id=\"screens-r%d\"><h2>%s</h2>", rows[i]->id, title);
            buf_put(b, row.p, row.len);
            buf_puts(b, "</section>");
            shown++;
        }
        buf_free(&row);
    }
    return shown;
}

/* ---- a wall: the top band, then the cards ---- */

/* Now and next from the shared agenda (bookings, the calendar...): what's on right now, then what
   comes - today, else tomorrow. Three at most: the cards below have the rest. */
static void wall_next(Ctx *c, Buf *b) {
    int64_t today = day_start(c->now), tomorrow = add_days(today, 1);
    Agenda ag = {0};
    agenda_collect(c, c->now, add_days(today, 2), NULL, &ag);
    int shown = 0;
    for (int i = 0; i < ag.n && shown < 3; i++) {
        const AgendaItem *it = &ag.items[i];
        bool now = it->start <= c->now && it->end > c->now;
        if (!now && it->start < c->now) continue; /* (a moment that's passed) */
        if (!shown++) buf_puts(b, "<ul class=\"screens-wnext\">");
        H(b, "<li class=\"%r\"><span class=\"screens-wn-when\">", now ? "screens-wn-now" : "");
        if (now) buf_puts(b, it->allday ? "Today" : "Now");
        else {
            if (it->start >= tomorrow) buf_puts(b, "Tomorrow ");
            if (!it->allday) ui_hm(b, it->start);
        }
        H(b, "</span><span class=\"screens-wn-what\">%s%r%s", it->icon, it->icon[0] ? " " : "", it->title);
        Person *p = person_get(it->who);
        if (p) H(b, " <small>· %s</small>", p->name);
        if (now && !it->allday && it->end > it->start) {
            buf_puts(b, " <small>until ");
            ui_hm(b, it->end);
            buf_puts(b, "</small>");
        }
        buf_puts(b, "</span></li>");
    }
    if (shown) buf_puts(b, "</ul>");
    agenda_free(&ag);
}

/* Everyone, with whether they're home (when Hemma can tell: a device of theirs on the network) or
   have Hemma open somewhere. */
static void wall_people(Buf *b) {
    if (!person_count()) return;
    bool tracking = presence_tracking();
    buf_puts(b, "<div class=\"screens-wpeople\">");
    for (int i = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        bool home = presence_is_home(p->id, NULL);
        const char *state = home ? "home" : tracking && presence_known(p->id) ? "away" : presence_screens(p->id) ? "online" : "";
        H(b, "<div class=\"screens-wp %s\">", state);
        ui_avatar_deco(b, p, "lg");
        H(b, "<b>%s</b><small>%s</small></div>", p->name, *state ? state : "\xC2\xA0");
    }
    buf_puts(b, "</div>");
}

/* A tablet on the wall: big clock, the date, now and next, who's home - then the cards, as tall
   as what they show, flowing in columns. Cards with nothing to show stay away (a calm wall). The
   core re-renders every minute: the clock needs nothing else. */
static void wall_page(Ctx *c, Buf *b, const Screen *s) {
    DateTime d = dt_local(c->now);
    char hm[6];
    fmt_hm(c->now, hm);
    H(b, "<div class=\"screens-dark screens-wall\" id=\"screens-view\"><header class=\"screens-wtop\"><div "
         "class=\"screens-wclock\"><span class=\"screens-home\">%s</span><time datetime=\"%s\">%s</time><span "
         "class=\"screens-wdate\">%s %d %s · week %d</span></div>",
      app_name(), hm, hm, weekday_name(d.wday, false), d.day, month_name(d.mon, false), iso_week(c->now, NULL));
    wall_next(c, b);
    wall_people(b);
    buf_puts(b, "</header>");
    /* columns at least ~300px wide, never more of them than cards: a few cards on a wide screen
       spread out instead of huddling on the left */
    Buf cards = {0};
    int total = 0, shown = draw_rows(c, &cards, s, &total);
    /* data-fit (web/wire.js): the cards fit the tablet's screen - rows give way with "+N more", whole
       cards only when nothing else can (issue #77: cards below the edge were never seen); wire.js lays
       them into the shortest column (issue #97), the CSS columns are the no-JS fallback */
    H(b, "<div class=\"screens-cards\" data-fit=\"screens-fitsay\" data-fit-cols=\"%d\" style=\"column-count:%d\">",
      CLAMP(shown, 1, 4), CLAMP(shown, 1, 4));
    buf_put(b, cards.p, cards.len);
    buf_puts(b, "</div>");
    buf_free(&cards);
    if (!shown)
        buf_puts(b, total ? "<p class=\"screens-calm\">All quiet - nothing booked, planned or on the list.</p>"
                          : "<p class=\"screens-calm\">No cards on this wall yet.</p>");
    /* out of the way, for whoever stands at the tablet - and, for an admin, what didn't fit its screen
       (wire.js; issue #97: the note stayed on the hall tablet for everyone) */
    buf_puts(b, "<nav class=\"screens-wfoot\">");
    if (is_admin(c)) {
        buf_puts(b, "<span class=\"screens-fitsay\" id=\"screens-fitsay\"></span>");
        H(b, "<a class=\"screens-btn\" href=\"/screens/%d/edit\">Edit</a>", s->id);
    }
    char url[32];
    screens_url(s->id, url);
    if (!display_home_is(c, url)) buf_puts(b, "<a class=\"screens-btn\" href=\"/\">Exit</a>");
    buf_puts(b, "</nav></div>");
}

static void view_page(Ctx *c, Buf *b, const Screen *s) {
    ctx_title(c, "%s", s->name);
    c->bare = true;
    if (s->kind == SCREEN_WALL) {
        wall_page(c, b, s);
        return;
    }
    int viewer = screens_viewer(c);
    if (!viewer && c->me->role == ROLE_DISPLAY && screens_drives_tv(c->me->id)) { /* (others watch as nobody) */
        picker(c, b);
        return;
    }
    buf_puts(b, "<div class=\"screens-dark screens-tv\" id=\"screens-view\" data-arrows>");
    top_bar(c, b, s, viewer);
    /* autofocus: the remote starts on the first row, not on the buttons up top (web/wire.js) */
    buf_puts(b, "<div class=\"screens-rows\" autofocus>");
    int total = 0, shown = draw_rows(c, b, s, &total);
    buf_puts(b, "</div>");
    if (!shown) {
        ui_empty(b, "📺", total ? "Nothing to show yet" : "No rows yet",
                 total ? "When there's something to watch, it shows up here." : "This screen has nothing on it yet.");
        if (is_admin(c)) H(b, "<p class=\"center\"><a class=\"screens-btn\" href=\"/screens/%d/edit\">Add rows</a></p>", s->id);
    }
    buf_puts(b, "</div>");
}

/* ============================================================ the builder */

static void move_btn(Buf *b, int row, const char *dir, bool off, const char *what) {
    bool up = str_eq(dir, "up");
    H(b, "<form method=\"post\" action=\"/a/screens.row_move\" data-a=\"screens.row_move\" class=\"inline\"><input "
         "type=\"hidden\" name=\"row\" value=\"%d\"><input type=\"hidden\" name=\"dir\" value=\"%s\"><button "
         "class=\"icon-btn\" title=\"Move %s\" aria-label=\"Move %s %s\"%r>%r</button></form>",
      row, dir, dir, what, dir, off ? " disabled" : "", up ? ICON_UP : ICON_DOWN);
}

/* <select name="arg"> with what the block offers, `cur` selected (one that's gone stays visible,
   unsendable: saving keeps it). False = nothing on offer. */
static bool choice_select(Ctx *c, Buf *b, const Block *bl, const char *cur, const char *label) {
    BlockChoice *ch;
    int n = screens_choices(c, bl, &ch);
    bool found = false;
    for (int i = 0; i < n; i++) found |= str_eq(ch[i].arg, cur);
    if (n || cur[0]) {
        H(b, "<select name=\"arg\" aria-label=\"%s\">", label);
        if (cur[0] && !found) H(b, "<option value=\"%s\" selected disabled>Not available any more</option>", cur);
        for (int i = 0; i < n; i++)
            H(b, "<option value=\"%s\"%r>%s</option>", ch[i].arg, str_eq(ch[i].arg, cur) ? " selected" : "",
              ch[i].label);
        buf_puts(b, "</select>");
    }
    free(ch);
    return n > 0;
}

static void rows_card(Ctx *c, Buf *b, const Screen *s) {
    ScreenRow *rows[SCREENS_MAX_ROWS];
    int n = screen_rows(s->id, rows, SCREENS_MAX_ROWS);
    Buf trash = {0};
    ui_icon(&trash, "trash");
    bool wall = s->kind == SCREEN_WALL;
    ui_card(b, wall ? "Cards" : "Rows", NULL, NULL);
    if (!n) H(b, "<p class=\"muted screens-hint\">No %s yet: add some below.</p>", wall ? "cards" : "rows");
    else if (wall)
        buf_puts(b, "<p class=\"muted small screens-hint\">In this order, flowing down the wall's columns. A card with "
                    "nothing to show right now is left out.</p>");
    else
        buf_puts(b, "<p class=\"muted small screens-hint\">Top to bottom, as the screen shows them. A row with nothing "
                    "to show right now is left out.</p>");
    buf_puts(b, "<ol class=\"screens-ed\">");
    for (int i = 0; i < n; i++) {
        const ScreenRow *r = rows[i]; /* (blocks' choices() don't touch this table: the pointers hold) */
        const Module *owner = NULL;
        const Block *bl = block_find(r->block, &owner);
        bool usable = bl && (bl->kinds & s->kind);
        H(b, "<li class=\"screens-ed-row%r\" id=\"screens-e%d\"><div class=\"screens-ed-head\"><span "
             "class=\"screens-ed-n\">%d</span><div class=\"screens-ed-main\">",
          usable ? "" : " screens-missing", r->id, i + 1);
        if (usable) H(b, "<b>%s</b><small>%s · %s</small>", r->title[0] ? r->title : bl->title, owner->title, bl->title);
        else
            H(b, "<b>%s</b><small><span class=\"badge warn\">missing</span> %s isn't offered here any more - "
                 "remove it, or leave it for when it's back</small>",
              r->title[0] ? r->title : r->block, r->block);
        buf_puts(b, "</div><div class=\"screens-ed-btns\">");
        const char *what = r->title[0] ? r->title : usable ? bl->title : r->block; /* (which row: issue #69) */
        move_btn(b, r->id, "up", i == 0, what);
        move_btn(b, r->id, "down", i == n - 1, what);
        char id[16], label[120];
        snprintf(id, sizeof id, "%d", r->id);
        snprintf(label, sizeof label, "Remove %s", what);
        ui_act(b, "screens.row_remove", "icon-btn", buf_cstr(&trash), "row", id, label);
        buf_puts(b, "</div></div>");
        if (usable) {
            ui_form(b, "screens.row_save", "screens-ed-form");
            ui_hidden_int(b, "row", r->id);
            H(b, "<input name=\"title\" maxlength=\"60\" value=\"%s\" placeholder=\"%s\" aria-label=\"Heading\">",
              r->title, bl->title);
            if (bl->choices) choice_select(c, b, bl, r->arg, "What it shows");
            buf_puts(b, "<button class=\"btn sm\">Save</button>");
            ui_form_end(b);
        }
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ol>");
    ui_card_end(b);
    buf_free(&trash);
}

static void add_card(Ctx *c, Buf *b, const Screen *s) {
    ui_card(b, s->kind == SCREEN_WALL ? "Add a card" : "Add a row", NULL, NULL);
    buf_puts(b, "<ul class=\"rows screens-add\">");
    int k = 0;
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        for (const Block *bl = m->blocks; bl && bl->id; bl++) {
            if (!(bl->kinds & s->kind)) continue;
            k++;
            H(b, "<li class=\"row\" id=\"screens-a-%s\">", bl->id);
            ui_form(b, "screens.row_add", "screens-add-f");
            ui_hidden_int(b, "id", s->id);
            ui_hidden(b, "block", bl->id);
            H(b, "<div class=\"row-main\"><b>%s</b><small>%s</small></div>", bl->title, m->title);
            bool can = !bl->choices || choice_select(c, b, bl, "", bl->title);
            if (!can) buf_puts(b, "<small class=\"muted\">Nothing to pick yet</small>");
            H(b, "<button class=\"btn sm\"%r>", can ? "" : " disabled");
            ui_icon(b, "plus");
            buf_puts(b, "<span>Add</span></button>");
            ui_form_end(b);
            buf_puts(b, "</li>");
        }
    }
    if (!k) buf_puts(b, "<li class=\"row muted\">Nothing offers rows for this kind of screen yet.</li>");
    buf_puts(b, "</ul>");
    ui_card_end(b);
}

static void shows_card(Buf *b, const Screen *s) {
    char url[32];
    screens_url(s->id, url);
    ui_card(b, "Shows on", NULL, NULL);
    int n = 0;
    for (int i = 0; i < account_count(); i++) n += account_at(i)->role == ROLE_DISPLAY;
    if (!n) {
        buf_puts(b, "<p class=\"muted\">A TV signs in with a <b>Display</b> account: it can only look. Add one under "
                    "<a href=\"/people\">Household</a>, sign in with it on the TV, then tick it here - it opens this "
                    "screen by itself.</p>");
        ui_card_end(b);
        return;
    }
    buf_puts(b, "<p class=\"muted small screens-hint\">Ticked displays open this screen by themselves. Each one shows "
                "one screen.</p>");
    ui_form(b, "screens.shows", "stack screens-shows");
    ui_hidden_int(b, "id", s->id);
    for (int i = 0; i < account_count(); i++) {
        Person *p = account_at(i);
        if (p->role != ROLE_DISPLAY) continue;
        Screen *now = start_screen(p->id);
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"acc\" value=\"%d\"%r> %s", p->id,
          now && now->id == s->id ? " checked" : "", p->name);
        if (now && now->id != s->id) H(b, " <small class=\"muted\">shows %s now</small>", now->name);
        buf_puts(b, "</label>");
    }
    buf_puts(b, "<div><button class=\"btn\">Save</button></div>");
    ui_form_end(b);
    ui_card_end(b);
}

static void screen_card(Buf *b, const Screen *s) {
    ui_card(b, "Screen", NULL, NULL);
    ui_form(b, "screens.save", "row-form");
    ui_hidden_int(b, "id", s->id);
    H(b, "<input name=\"name\" maxlength=\"40\" required value=\"%s\" aria-label=\"Screen name\"><button "
         "class=\"btn\">Rename</button>",
      s->name);
    ui_form_end(b);
    char q[120];
    snprintf(q, sizeof q, "Delete %s? Its rows go with it.", s->name);
    ui_form_confirm(b, "screens.delete", "danger-zone", q);
    ui_hidden_int(b, "id", s->id);
    buf_puts(b, "<button class=\"btn danger\">Delete screen</button>");
    ui_form_end(b);
    ui_card_end(b);
}

static void edit_page(Ctx *c, Buf *b, const Screen *s) {
    char url[32];
    screens_url(s->id, url);
    ctx_title(c, "Edit %s", s->name);
    if (!is_admin(c)) {
        ui_empty(b, "🔒", "Only an admin changes screens", NULL);
        H(b, "<p class=\"center\"><a class=\"btn\" href=\"%s\">Open %s</a></p>", url, s->name);
        return;
    }
    char sub[64];
    snprintf(sub, sizeof sub, "%s screen", kind_name(s->kind));
    ui_head(b, s->name, sub);
    buf_puts(b, "<a class=\"btn ghost\" href=\"/screens\">All screens</a>");
    H(b, "<a class=\"btn primary\" href=\"%s\">", url);
    ui_icon(b, "play");
    buf_puts(b, "<span>Open</span></a>");
    ui_head_end(b);
    buf_puts(b, "<div class=\"grid2 screens-build\"><div>");
    rows_card(c, b, s);
    add_card(c, b, s);
    buf_puts(b, "</div><div>");
    shows_card(b, s);
    screen_card(b, s);
    buf_puts(b, "</div></div>");
}

/* ================================================================== routes */

void screens_page(Ctx *c, Buf *b) {
    const char *p = c->path + strlen("/screens");
    if (!*p || str_eq(p, "/")) {
        list_page(c, b);
        return;
    }
    char *end = NULL;
    long id = *p == '/' ? strtol(p + 1, &end, 10) : 0;
    const Screen *s = end && end != p + 1 && id > 0 && id < 1000000000 ? screen_get((int)id) : NULL;
    if (s && !*end) view_page(c, b, s);
    else if (s && str_eq(end, "/edit")) edit_page(c, b, s);
    else {
        ctx_title(c, "Not found");
        ui_empty(b, "📺", "That screen is gone", "It may have been deleted.");
        buf_puts(b, "<p class=\"center\"><a class=\"btn\" href=\"/screens\">All screens</a></p>");
    }
}
