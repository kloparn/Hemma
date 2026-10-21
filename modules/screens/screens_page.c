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
        H(b, T("On its web browser, open Hemma and sign in with it. Hemma only listens on this computer (%s) now, so "
               "a TV can't reach it: start it for the home network (see <a href=\"/system\">System</a>)."),
          host);
        return;
    }
    if (!n) {
        buf_puts(b, T("On its web browser, open Hemma - type the address this browser shows, with <code>http://</code> "
                      "in front (not <i>localhost</i>) - and sign in with it."));
        return;
    }
    buf_puts(b, T("On its web browser, open Hemma at this address - all of it, with <code>http://</code> - and sign in "
                  "with it:"));
    buf_puts(b, "<div class=\"screens-addr\"><div><p class=\"screens-addr-big\">");
    html_esc(b, addr[0]);
    buf_puts(b, "</p>");
    if (n > 1) {
        buf_puts(b, "<p class=\"muted small\">");
        H(b, T("Phones and computers can use <b>%s</b> too; many TVs can't."), addr[1]);
        buf_puts(b, "</p>");
    }
    buf_puts(b, "</div>");
    QrCode q;
    if (qr_make(&q, addr[0], strlen(addr[0]), QR_M, -1)) {
        buf_puts(b, "<figure class=\"screens-qr\">");
        qr_svg(b, &q, addr[0]);
        H(b, "<figcaption class=\"muted small\">%s</figcaption></figure>", T("A tablet or phone can scan this instead"));
    }
    buf_puts(b, "</div>");
}

static void new_modal(Buf *b) {
    ui_modal(b, T("New screen"), "/screens");
    ui_form(b, "screens.create", "stack");
    H(b,
      "<label class=\"field\"><span>%s</span><input name=\"name\" maxlength=\"40\" required placeholder=\"%s\" "
      "autofocus></label><div class=\"field\"><span>%s</span><div class=\"chips\"><label class=\"chip-r\"><input "
      "type=\"radio\" name=\"kind\" value=\"tv\" checked><span>TV</span></label><label class=\"chip-r\"><input "
      "type=\"radio\" name=\"kind\" value=\"wall\"><span>%s</span></label></div><small class=\"muted\">%s</small></div>",
      T("Name"), T("Living room TV"), T("Kind"), T("Wall"),
      T("TV: big rows you scroll with the remote, like a streaming app. Wall: an always-on overview for a tablet - "
        "the clock, who's home, today, bookings, the shopping list..."));
    H(b,
      "<div class=\"modal-f\"><a class=\"btn ghost\" href=\"/screens\">%s</a><button class=\"btn primary\">%s</button>"
      "</div>",
      T("Cancel"), T("Make it"));
    ui_form_end(b);
    ui_modal_end(b);
}

/* Your own place on the TV's "Who's watching?" (issues #90, #42): anyone chooses to be there (admins are off
   until they tick it, everyone else on until they untick it), and anyone can ask for a PIN first. */
static void you_card(Ctx *c, Buf *b) {
    if (!c->me || c->me->role == ROLE_DISPLAY) return;
    ui_card(b, T("You on the TV"), NULL, NULL);
    bool on = privacy_on_tv(c->me);
    ui_form(b, "screens.on_tv", "screens-ontv");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> %r</label>",
      on ? " checked" : "", T("Show me on the TV's <i>Who's watching?</i>"));
    ui_form_end(b);
    H(b, "<p class=\"muted small\">%s</p>",
      T("Whoever has the TV's sign-in can see and clear the watch history of anyone it lists."));
    if (screens_has_pin(c->me->id)) {
        H(b, "<p>%s</p>", T("Your PIN is set: the TV asks for it when you're picked."));
        ui_act(b, "screens.pin", "btn sm", T("Remove the PIN"), "clear", "1", NULL);
    } else {
        ui_form(b, "screens.pin", "screens-pinset");
        H(b,
          "<label class=\"field\"><span>%s <span class=\"muted\">%s</span></span><input type=\"password\" name=\"pin\" "
          "inputmode=\"numeric\" pattern=\"[0-9]{4}\" maxlength=\"4\" autocomplete=\"new-password\" placeholder=\"%s\">"
          "<small class=\"muted\">%s</small></label><button class=\"btn\">%s</button>",
          T("A PIN for the TV"), T("(optional)"), T("4 digits"),
          T("The TV asks for it when someone picks you, so nobody watches - or clears your history - as you."),
          T("Save the PIN"));
        ui_form_end(b);
    }
    ui_card_end(b);
}

/* "TV · 5 rows" / "Wall · 8 cards" */
static void kind_rows(Buf *b, int kind, int n) {
    if (kind == SCREEN_WALL) H(b, TN(n, "Wall · %d card", "Wall · %d cards"), n);
    else H(b, TN(n, "TV · %d row", "TV · %d rows"), n);
}

static void list_page(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Screens"));
    ui_head(b, T("Screens"), T("What the TV and the wall tablets show"));
    if (is_admin(c)) {
        buf_puts(b, "<a class=\"btn primary\" href=\"/screens?new=1\">");
        ui_icon(b, "plus");
        H(b, "<span>%s</span></a>", T("New screen"));
    }
    ui_head_end(b);
    int n = tbl_count(&T_screen);
    if (!n) {
        ui_empty(b, "📺", T("No screens yet"),
                 is_admin(c) ? T("Make one and pick its rows: Continue watching, On TV, Series, Movies...")
                             : T("An admin can put one together for the TV."));
    } else {
        buf_puts(b, "<section class=\"card\"><ul class=\"rows screens-list\">");
        for (int i = 0; i < n; i++) {
            Screen *s = tbl_at(&T_screen, i);
            ScreenRow *rows[SCREENS_MAX_ROWS];
            int nr = screen_rows(s->id, rows, SCREENS_MAX_ROWS);
            H(b, "<li class=\"row\" id=\"screens-s%d\"><span class=\"screens-kind\">", s->id);
            ui_icon(b, s->kind == SCREEN_WALL ? "grid" : "tv");
            H(b, "</span><div class=\"row-main\"><b>%s</b><small>", s->name);
            kind_rows(b, s->kind, nr);
            Buf on = {0};
            shown_on(s->id, &on);
            if (on.len) {
                buf_puts(b, " · ");
                H(b, T("on %s"), buf_cstr(&on));
            }
            buf_free(&on);
            H(b, "</small></div><div class=\"screens-list-a\"><a class=\"btn\" href=\"/screens/%d\">%s</a>", s->id,
              T("Open"));
            if (is_admin(c)) {
                H(b, "<a class=\"btn ghost\" href=\"/screens/%d/edit\">", s->id);
                ui_icon(b, "edit");
                H(b, "<span>%s</span></a>", T("Edit"));
            }
            buf_puts(b, "</div></li>");
        }
        buf_puts(b, "</ul></section>");
    }
    if (is_admin(c)) {
        ui_card(b, T("On the TV"), NULL, NULL);
        buf_puts(b, "<ol class=\"steps\"><li>");
        /* the sign-in kind's name is the people module's (ROLE_NAMES) */
        H(b, T("Under <a href=\"/people\">Household</a>, add someone for the TV (or the wall tablet) and choose the "
               "sign-in kind <b>%s</b>: it can only look."),
          T("Display"));
        buf_puts(b, "</li><li>");
        address_step(b);
        buf_puts(b, "</li><li>");
        buf_puts(b, T("Edit a screen and tick it under <b>Shows on</b>: it opens that screen by itself. A TV asks "
                      "<i>Who's watching?</i> - nobody types a password there."));
        buf_puts(b, "</li></ol>");
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
        H(b, "<button class=\"screens-btn\">%s</button>", T("Switch"));
        ui_form_end(b);
    }
    if (is_admin(c)) H(b, "<a class=\"screens-btn\" href=\"/screens/%d/edit\">%s</a>", s->id, T("Edit"));
    if (!display_home_is(c, url)) /* (a TV's own screen is its home) */
        H(b, "<a class=\"screens-btn\" href=\"/screens\">%s</a>", T("Exit"));
    buf_puts(b, "</div></header>");
}

/* The household's avatars, big, for the remote: no password - on the TV they can only watch. Admins
   only once they ticked "Show me on the TV", and someone with a PIN gets asked for it (issue #90:
   whoever holds the TV's sign-in could read and clear anyone's watch history). */
static void picker(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Who's watching?"));
    H(b, "<div class=\"screens-dark screens-tv screens-pick\" id=\"screens-pick\" data-arrows><div "
         "class=\"screens-pick-in\"><p class=\"screens-home\">%s</p>",
      app_name());
    Person *ask = person_get((int)ctx_qint(c, "pin", 0));
    if (ask && screens_on_picker(ask) && screens_has_pin(ask->id)) { /* the PIN, then the pick */
        buf_puts(b, "<h1>");
        H(b, T("%s's PIN"), ask->name);
        buf_puts(b, "</h1>");
        int wait = screens_pin_wait(ask->id, c->now);
        if (wait > 0) {
            buf_puts(b, "<p class=\"screens-note\">");
            H(b, T("Too many wrong PINs - try again in %d min."), (wait + 59) / 60);
            buf_puts(b, "</p>");
        }
        ui_form(b, "screens.viewer", "screens-pinform");
        ui_hidden_int(b, "who", ask->id);
        buf_puts(b, "<input class=\"screens-pin\" type=\"password\" name=\"pin\" inputmode=\"numeric\" "
                    "pattern=\"[0-9]*\" maxlength=\"4\" autocomplete=\"off\" aria-label=\"PIN\" autofocus>"
                    "<button class=\"screens-btn\">OK</button>");
        ui_form_end(b);
        H(b, "<p><a class=\"screens-btn\" href=\"%s\">%s</a></p></div></div>", c->path, T("Back"));
        return;
    }
    H(b, "<h1>%r</h1>", T("Who's watching?"));
    int shown = 0;
    for (int i = 0; i < person_count(); i++) shown += screens_on_picker(person_at(i));
    if (!shown)
        H(b, "<p class=\"screens-note\">%s</p>", T("Nobody's set up yet: add the household on a phone or computer."));
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
            /* a heading the admin typed is theirs; the block's own is its module's N_() word */
            const char *title = rows[i]->title[0] ? rows[i]->title : T(bl->title);
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
        if (now) buf_puts(b, it->allday ? T("Today") : T("Now"));
        else {
            if (it->start >= tomorrow) H(b, "%s ", T("Tomorrow")); /* (then its time) */
            if (!it->allday) ui_hm(b, it->start);
        }
        H(b, "</span><span class=\"screens-wn-what\">%s%r%s", it->icon, it->icon[0] ? " " : "", it->title);
        Person *p = person_get(it->who);
        if (p) H(b, " <small>· %s</small>", p->name);
        if (now && !it->allday && it->end > it->start) {
            char hm[6];
            fmt_hm(it->end, hm);
            buf_puts(b, " <small>");
            H(b, TC("time", "until %s"), hm);
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
        int show = privacy_wall(p->id); /* their own choice (issue #42): with home/away, the name only, or not */
        if (show == WALL_HIDE) continue;
        bool home = show == WALL_SHOW && presence_is_home(p->id, NULL);
        const char *state = show == WALL_NAME ? ""
                            : home            ? "home"
                            : tracking && presence_known(p->id) ? "away"
                            : presence_screens(p->id)           ? "online"
                                                                : "";
        /* the class stays English; the words under the name are the reader's */
        const char *words = home ? T("home") : str_eq(state, "away") ? T("away") : *state ? T("online") : "\xC2\xA0";
        H(b, "<div class=\"screens-wp %s\">", state);
        ui_avatar_deco(b, p, "lg");
        H(b, "<b>%s</b><small>%s</small></div>", p->name, words);
    }
    buf_puts(b, "</div>");
}

/* A tablet on the wall: big clock, the date, now and next, who's home - then the cards, as tall
   as what they show, flowing in columns. Cards with nothing to show stay away (a calm wall). The
   core re-renders every minute: the clock needs nothing else. */
static void wall_page(Ctx *c, Buf *b, const Screen *s) {
    DateTime d = dt_local(c->now);
    char hm[6], date[96];
    fmt_hm(c->now, hm);
    /* "Saturday 10 October · week 41": Swedish and Spanish names are lower case - this one starts the line */
    snprintf(date, sizeof date, T("%s %d %s · week %d"), weekday_name(d.wday, false), d.day, month_name(d.mon, false),
             iso_week(c->now, NULL));
    if (date[0] >= 'a' && date[0] <= 'z') date[0] = (char)(date[0] - 'a' + 'A');
    H(b, "<div class=\"screens-dark screens-wall\" id=\"screens-view\"><header class=\"screens-wtop\"><div "
         "class=\"screens-wclock\"><span class=\"screens-home\">%s</span><time datetime=\"%s\">%s</time><span "
         "class=\"screens-wdate\">%s</span></div>",
      app_name(), hm, hm, date);
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
        H(b, "<p class=\"screens-calm\">%s</p>",
          total ? T("All quiet - nothing booked, planned or on the list.") : T("No cards on this wall yet."));
    /* out of the way, for whoever stands at the tablet - and, for an admin, what didn't fit its screen
       (wire.js; issue #97: the note stayed on the hall tablet for everyone) */
    buf_puts(b, "<nav class=\"screens-wfoot\">");
    if (is_admin(c)) {
        buf_puts(b, "<span class=\"screens-fitsay\" id=\"screens-fitsay\"></span>");
        H(b, "<a class=\"screens-btn\" href=\"/screens/%d/edit\">%s</a>", s->id, T("Edit"));
    }
    char url[32];
    screens_url(s->id, url);
    if (!display_home_is(c, url)) H(b, "<a class=\"screens-btn\" href=\"/\">%s</a>", T("Exit"));
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
        ui_empty(b, "📺", total ? T("Nothing to show yet") : T("No rows yet"),
                 total ? T("When there's something to watch, it shows up here.")
                       : T("This screen has nothing on it yet."));
        if (is_admin(c))
            H(b, "<p class=\"center\"><a class=\"screens-btn\" href=\"/screens/%d/edit\">%s</a></p>", s->id,
              T("Add rows"));
    }
    buf_puts(b, "</div>");
}

/* ============================================================ the builder */

static void move_btn(Buf *b, int row, const char *dir, bool off, const char *what) {
    bool up = str_eq(dir, "up");
    char label[160];
    snprintf(label, sizeof label, up ? T("Move %s up") : T("Move %s down"), what);
    H(b, "<form method=\"post\" action=\"/a/screens.row_move\" data-a=\"screens.row_move\" class=\"inline\"><input "
         "type=\"hidden\" name=\"row\" value=\"%d\"><input type=\"hidden\" name=\"dir\" value=\"%s\"><button "
         "class=\"icon-btn\" title=\"%s\" aria-label=\"%s\"%r>%r</button></form>",
      row, dir, up ? T("Move up") : T("Move down"), label, off ? " disabled" : "", up ? ICON_UP : ICON_DOWN);
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
        if (cur[0] && !found)
            H(b, "<option value=\"%s\" selected disabled>%s</option>", cur, T("Not available any more"));
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
    ui_card(b, wall ? T("Cards") : T("Rows"), NULL, NULL);
    if (!n)
        H(b, "<p class=\"muted screens-hint\">%s</p>",
          wall ? T("No cards yet: add some below.") : T("No rows yet: add some below."));
    else if (wall)
        H(b, "<p class=\"muted small screens-hint\">%s</p>",
          T("In this order, flowing down the wall's columns. A card with nothing to show right now is left out."));
    else
        H(b, "<p class=\"muted small screens-hint\">%s</p>",
          T("Top to bottom, as the screen shows them. A row with nothing to show right now is left out."));
    buf_puts(b, "<ol class=\"screens-ed\">");
    for (int i = 0; i < n; i++) {
        const ScreenRow *r = rows[i]; /* (blocks' choices() don't touch this table: the pointers hold) */
        const Module *owner = NULL;
        const Block *bl = block_find(r->block, &owner);
        bool usable = bl && (bl->kinds & s->kind);
        H(b, "<li class=\"screens-ed-row%r\" id=\"screens-e%d\"><div class=\"screens-ed-head\"><span "
             "class=\"screens-ed-n\">%d</span><div class=\"screens-ed-main\">",
          usable ? "" : " screens-missing", r->id, i + 1);
        if (usable)
            H(b, "<b>%s</b><small>%s · %s</small>", r->title[0] ? r->title : T(bl->title), T(owner->title),
              T(bl->title));
        else {
            H(b, "<b>%s</b><small><span class=\"badge warn\">%s</span> ", r->title[0] ? r->title : r->block,
              T("missing"));
            H(b, T("%s isn't offered here any more - remove it, or leave it for when it's back"), r->block);
            buf_puts(b, "</small>");
        }
        buf_puts(b, "</div><div class=\"screens-ed-btns\">");
        const char *what = r->title[0] ? r->title : usable ? T(bl->title) : r->block; /* (which row: issue #69) */
        move_btn(b, r->id, "up", i == 0, what);
        move_btn(b, r->id, "down", i == n - 1, what);
        char id[16], label[160];
        snprintf(id, sizeof id, "%d", r->id);
        snprintf(label, sizeof label, T("Remove %s"), what);
        ui_act(b, "screens.row_remove", "icon-btn", buf_cstr(&trash), "row", id, label);
        buf_puts(b, "</div></div>");
        if (usable) {
            ui_form(b, "screens.row_save", "screens-ed-form");
            ui_hidden_int(b, "row", r->id);
            H(b, "<input name=\"title\" maxlength=\"60\" value=\"%s\" placeholder=\"%s\" aria-label=\"%s\">", r->title,
              T(bl->title), T("Heading"));
            if (bl->choices) choice_select(c, b, bl, r->arg, T("What it shows"));
            H(b, "<button class=\"btn sm\">%s</button>", T("Save"));
            ui_form_end(b);
        }
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ol>");
    ui_card_end(b);
    buf_free(&trash);
}

static void add_card(Ctx *c, Buf *b, const Screen *s) {
    ui_card(b, s->kind == SCREEN_WALL ? T("Add a card") : T("Add a row"), NULL, NULL);
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
            H(b, "<div class=\"row-main\"><b>%s</b><small>%s</small></div>", T(bl->title), T(m->title));
            bool can = !bl->choices || choice_select(c, b, bl, "", T(bl->title));
            if (!can) H(b, "<small class=\"muted\">%s</small>", T("Nothing to pick yet"));
            H(b, "<button class=\"btn sm\"%r>", can ? "" : " disabled");
            ui_icon(b, "plus");
            H(b, "<span>%s</span></button>", T("Add"));
            ui_form_end(b);
            buf_puts(b, "</li>");
        }
    }
    if (!k) H(b, "<li class=\"row muted\">%s</li>", T("Nothing offers rows for this kind of screen yet."));
    buf_puts(b, "</ul>");
    ui_card_end(b);
}

static void shows_card(Buf *b, const Screen *s) {
    char url[32];
    screens_url(s->id, url);
    ui_card(b, T("Shows on"), NULL, NULL);
    int n = 0;
    for (int i = 0; i < account_count(); i++) n += account_at(i)->role == ROLE_DISPLAY;
    if (!n) {
        buf_puts(b, "<p class=\"muted\">");
        H(b, T("A TV signs in with a <b>%s</b> account: it can only look. Add one under <a "
               "href=\"/people\">Household</a>, sign in with it on the TV, then tick it here - it opens this screen by "
               "itself."),
          T("Display"));
        buf_puts(b, "</p>");
        ui_card_end(b);
        return;
    }
    H(b, "<p class=\"muted small screens-hint\">%s</p>",
      T("Ticked displays open this screen by themselves. Each one shows one screen."));
    ui_form(b, "screens.shows", "stack screens-shows");
    ui_hidden_int(b, "id", s->id);
    for (int i = 0; i < account_count(); i++) {
        Person *p = account_at(i);
        if (p->role != ROLE_DISPLAY) continue;
        Screen *now = start_screen(p->id);
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"acc\" value=\"%d\"%r> %s", p->id,
          now && now->id == s->id ? " checked" : "", p->name);
        if (now && now->id != s->id) {
            buf_puts(b, " <small class=\"muted\">");
            H(b, T("shows %s now"), now->name);
            buf_puts(b, "</small>");
        }
        buf_puts(b, "</label>");
    }
    H(b, "<div><button class=\"btn\">%s</button></div>", T("Save"));
    ui_form_end(b);
    ui_card_end(b);
}

static void screen_card(Buf *b, const Screen *s) {
    ui_card(b, T("Screen"), NULL, NULL);
    ui_form(b, "screens.save", "row-form");
    ui_hidden_int(b, "id", s->id);
    H(b, "<input name=\"name\" maxlength=\"40\" required value=\"%s\" aria-label=\"%s\"><button "
         "class=\"btn\">%s</button>",
      s->name, T("Screen name"), T("Rename"));
    ui_form_end(b);
    char q[160];
    snprintf(q, sizeof q, T("Delete %s? Its rows go with it."), s->name);
    ui_form_confirm(b, "screens.delete", "danger-zone", q);
    ui_hidden_int(b, "id", s->id);
    H(b, "<button class=\"btn danger\">%s</button>", T("Delete screen"));
    ui_form_end(b);
    ui_card_end(b);
}

static void edit_page(Ctx *c, Buf *b, const Screen *s) {
    char url[32];
    screens_url(s->id, url);
    ctx_title(c, T("Edit %s"), s->name);
    if (!is_admin(c)) {
        ui_empty(b, "🔒", T("Only an admin changes screens"), NULL);
        buf_puts(b, "<p class=\"center\">");
        H(b, "<a class=\"btn\" href=\"%s\">", url);
        H(b, T("Open %s"), s->name);
        buf_puts(b, "</a></p>");
        return;
    }
    ui_head(b, s->name, s->kind == SCREEN_WALL ? T("Wall screen") : T("TV screen"));
    H(b, "<a class=\"btn ghost\" href=\"/screens\">%s</a>", T("All screens"));
    H(b, "<a class=\"btn primary\" href=\"%s\">", url);
    ui_icon(b, "play");
    H(b, "<span>%s</span></a>", T("Open"));
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
        ctx_title(c, "%s", T("Not found"));
        ui_empty(b, "📺", T("That screen is gone"), T("It may have been deleted."));
        H(b, "<p class=\"center\"><a class=\"btn\" href=\"/screens\">%s</a></p>", T("All screens"));
    }
}
