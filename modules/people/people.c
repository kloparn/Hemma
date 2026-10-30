/* modules/people — the household: people, their sign-ins, your account.
 *
 * Admins add people and decide who can sign in (and as what: member, child,
 * guest until a day, admin, or a read-only display for wall tablets), and
 * remove them - as moved out (their history stays) or erased (issue #45).
 * Everyone manages their own profile, password, recovery codes, signed-in
 * devices, privacy (issue #42) and language (issue #46). Signing in itself is core (core/auth.c),
 * so is the first admin (the welcome page, or the environment).
 * /people/codes shows freshly made recovery codes once. */
#include "../../core/app.h"
#include "../../core/auth.h"
#include "../../core/qr.h"
#include "../../core/ui.h"

static const char *const EMOJI[] = {"🦊", "🐻", "🐼", "🐨", "🐯", "🦁", "🐸", "🐵", "🦄", "🐙",
                                    "🐝", "🦉", "🐳", "🐢", "🌻", "🍄", "⭐", "🌙", "🔥", "🎸"};
static const char *const ROLE_NAMES[ROLE_COUNT] = {N_("Member"), N_("Admin"), N_("Display"), N_("Child"), N_("Guest")};
/* the role picker, in this order */
static const int ROLE_ORDER[] = {ROLE_MEMBER, ROLE_CHILD, ROLE_GUEST, ROLE_ADMIN, ROLE_DISPLAY};
static const char *const ROLE_HINTS[ROLE_COUNT] = {
    N_("use everything"), N_("also manage people"), N_("only look (wall tablet, TV)"),
    N_("their own to-dos and the lists"), N_("signs in until a day")};
#define GUEST_DAYS 7 /* a new guest's last day: a week from today, to change */

static const char *role_name(int role) { return T(ROLE_NAMES[CLAMP(role, 0, ROLE_COUNT - 1)]); }

/* A guest's last day: the day before Person.until (the local midnight after it) */
static int64_t last_day(const Person *p) { return add_days(p->until, -1); }

/* A day inside a sentence ("Guest until %s"): English keeps ui_day's capital ("ended Yesterday", as it
   always said); Swedish and Spanish write today, tomorrow and the weekdays in lower case there. */
static void day_in(const Ctx *c, Buf *b, int64_t day) {
    if (c->lang == LANG_EN) ui_day(b, day, c->now);
    else ui_day_inline(b, day, c->now);
}

/* Issue #46: a pref PREF_LANG value ("" = automatic, else one of ours) as its code; false = none of those */
static bool lang_value(const char *v, const char **code) {
    Lang l;
    if (!v[0]) *code = "";
    else if (i18n_parse(v, &l) && str_ieq(v, i18n_code(l))) *code = i18n_code(l);
    else return false;
    return true;
}

/* the language person's screens show in: their own pick, else (here) this browser's, else English */
static Lang lang_for(const Ctx *c, int person) {
    Lang l;
    const char *p = pref(person, PREF_LANG, "");
    if (p[0] && i18n_parse(p, &l)) return l;
    return c->dev_lang >= 0 && c->dev_lang < LANG_COUNT ? (Lang)c->dev_lang : LANG_EN;
}

/* the admin's choice in the person dialog: Automatic, or a language named in itself */
static void lang_select(Buf *b, const char *cur) {
    Lang now;
    bool set = cur[0] && i18n_parse(cur, &now);
    H(b, "<label class=\"field\"><span>%s</span><select name=\"lang\"><option value=\"\">%s</option>", T("Language"),
      T("Automatic"));
    for (int l = 0; l < LANG_COUNT; l++)
        H(b, "<option value=\"%s\" lang=\"%s\"%r>%s</option>", i18n_code((Lang)l), i18n_code((Lang)l),
          set && (int)now == l ? " selected" : "", i18n_name((Lang)l));
    buf_puts(b, "</select></label>");
}

static int pick_hue(void) {
    static const int hues[] = {200, 335, 150, 25, 255, 45, 175, 310, 90, 0, 220, 280};
    return hues[person_count() % countof(hues)];
}

static void emoji_picker(Buf *b, const char *selected) {
    buf_puts(b, "<div class=\"emojis\"><label class=\"emo\"><input type=\"radio\" name=\"emoji\" value=\"\"");
    if (!selected || !selected[0]) buf_puts(b, " checked");
    H(b, "><span title=\"%s\">Aa</span></label>", T("Initials"));
    for (int i = 0; i < countof(EMOJI); i++)
        H(b, "<label class=\"emo\"><input type=\"radio\" name=\"emoji\" value=\"%s\"%r><span>%s</span></label>", EMOJI[i],
          str_eq(selected, EMOJI[i]) ? " checked" : "", EMOJI[i]);
    buf_puts(b, "</div>");
}

static bool can_edit(const Ctx *c, const Person *p) { return is_admin(c) || (c->me && p && p->id == c->me->id); }

/* ------------------------------------------------------------------ pages */

static void person_modal(Ctx *c, Buf *b, Person *p) {
    bool admin = is_admin(c);
    ui_modal(b, p ? (p->id == c->me->id ? T("Your profile") : T("Edit person")) : T("Add someone"), "/people");
    ui_form(b, "people.save", "stack");
    if (p) ui_hidden_int(b, "id", p->id);
    H(b,
      "<label class=\"field\"><span>%s</span><input name=\"name\" required maxlength=\"40\" autocomplete=\"off\" "
      "value=\"%s\"%r></label>",
      T("Name"), p ? p->name : "", p ? "" : " autofocus");
    H(b, "<div class=\"field\"><span>%s</span>", T("Avatar"));
    emoji_picker(b, p ? p->emoji : "");
    H(b, "</div><div class=\"field\"><span>%s</span>", T("Colour"));
    ui_hue_picker(b, "hue", p ? p->hue : pick_hue());
    buf_puts(b, "</div>");
    if (admin) {
        bool self = p && p->id == c->me->id;
        lang_select(b, p ? pref(p->id, PREF_LANG, "") : ""); /* (any account's, a display's too: issue #46) */
        H(b,
          "<fieldset class=\"people-signin\"><legend>%s</legend><p class=\"muted small\">%r</p>"
          "<div class=\"field-row\"><label class=\"field\"><span>%s</span><input name=\"username\" "
          "maxlength=\"32\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\" value=\"%s\" "
          "placeholder=\"%s\"></label><label class=\"field\"><span>%s</span><select name=\"role\"%r>",
          T("Signing in"),
          T("Leave the username empty for people who don't need their own sign-in (things can still be booked for "
            "them)."),
          T("Username"), p ? p->username : "", T("e.g. eva"), T("Can"), self ? " disabled" : "");
        for (int k = 0; k < countof(ROLE_ORDER); k++) {
            int r = ROLE_ORDER[k];
            H(b, "<option value=\"%d\"%r>%s — %s</option>", r, (p ? p->role : ROLE_MEMBER) == r ? " selected" : "",
              T(ROLE_NAMES[r]), T(ROLE_HINTS[r]));
        }
        char day[11];
        fmt_ymd(p && p->until ? last_day(p) : add_days(day_start(c->now), GUEST_DAYS), day);
        H(b,
          "</select></label></div><label class=\"field people-until\"><span>%r</span><input "
          "type=\"date\" name=\"until\" value=\"%s\"><small class=\"muted\">%s</small></label>",
          T("A guest's last day"), day, T("Their sign-in ends after it, by itself. Change it to give them more days."));
        if (self) /* your own: under Your account, which asks for the current one first */
            H(b, "<p class=\"muted small\">%r</p>", T("Change your own password under <b>Your account</b>."));
        else
            H(b,
              "<label class=\"field\"><span>%s</span><input type=\"password\" name=\"password\" "
              "autocomplete=\"new-password\" maxlength=\"200\" placeholder=\"%s\"></label>",
              p && p->pass[0] ? T("New password") : T("Password"),
              p && p->pass[0] ? T("Leave empty to keep the current one") : T("At least 8 characters"));
        buf_puts(b, "</fieldset>");
    }
    H(b, "<div class=\"modal-f\"><button class=\"btn primary\">%s</button></div>", T("Save"));
    ui_form_end(b);
    if (admin && p && p->id != c->me->id) {
        buf_puts(b, "<div class=\"danger-zone people-danger\">");
        if (p->pass[0]) {
            ui_form(b, "people.signout", "inline");
            ui_hidden_int(b, "id", p->id);
            buf_puts(b, "<button class=\"btn\">");
            H(b, T("Sign %s out everywhere"), p->name);
            buf_puts(b, "</button>");
            ui_form_end(b);
        }
        /* what happens first, then the choice (issue #45) */
        H(b, "<a class=\"btn danger\" href=\"/people?remove=%d\">", p->id);
        H(b, T("Remove %s"), p->name);
        buf_puts(b, "</a></div>");
    }
    ui_modal_end(b);
}

/* What removing p would do (person_forget without applying it): one list item per note. */
static void will_list(Buf *b, int person, bool keep, int64_t now) {
    Forget f = {.person = person, .keep = keep, .now = now};
    person_forget(&f); /* (the modules' notes come in the language in use: they're made right now) */
    buf_puts(b, "<ul class=\"people-will\">");
    for (int i = 0; i < f.nnotes; i++) H(b, "<li>%s</li>", f.notes[i]);
    if (!f.nnotes) H(b, "<li>%s</li>", T("Nothing else of theirs is waiting"));
    buf_puts(b, "</ul>");
}

/* /people?remove=<id>: moved out (their history stays) or erased - each with what it does, before it does it */
static void remove_modal(Ctx *c, Buf *b, const Person *p) {
    char title[160];
    snprintf(title, sizeof title, p->left ? T("Erase %s") : T("Remove %s"), p->name);
    ui_modal(b, title, "/people");
    ui_form(b, "people.delete", "stack");
    ui_hidden_int(b, "id", p->id);
    if (p->role == ROLE_DISPLAY) { /* (a TV or a tablet has no history of its own) */
        ui_hidden(b, "how", "erase");
        buf_puts(b, "<p>");
        H(b, T("%s stops working where it's signed in, and its settings go."), p->name);
        buf_puts(b, "</p>");
        will_list(b, p->id, false, c->now);
    } else if (p->left) {
        ui_hidden(b, "how", "erase");
        buf_puts(b, "<p>");
        H(b,
          T("Everything about %s goes: what they booked, their to-dos and notes, what they watched, their lines in "
            "Latest. Things they added for everyone stay, without their name."),
          p->name);
        buf_puts(b, "</p>");
        will_list(b, p->id, false, c->now);
    } else {
        buf_puts(b, "<fieldset class=\"people-how\"><legend class=\"sr-only\">");
        H(b, T("How %s leaves"), p->name);
        buf_puts(b, "</legend><label class=\"people-how-o\"><input type=\"radio\" name=\"how\" value=\"keep\" checked>"
                    "<span><b>");
        H(b, T("%s moved out - keep their history"), p->name);
        buf_puts(b, "</b><small>");
        H(b,
          T("What they did stays under their name, “%s (moved out)”: bookings that happened, notes, Latest, what they "
            "watched. What's still ahead of them:"),
          p->name);
        buf_puts(b, "</small>");
        will_list(b, p->id, true, c->now);
        buf_puts(b, "</span></label><label class=\"people-how-o\"><input type=\"radio\" name=\"how\" value=\"erase\">"
                    "<span><b>");
        H(b, T("Erase everything about %s"), p->name);
        H(b, "</b><small>%s</small>",
          T("Their bookings, to-dos, notes, watch history and lines in Latest go too. Things they added for everyone "
            "stay, without their name:"));
        will_list(b, p->id, false, c->now);
        buf_puts(b, "</span></label></fieldset>");
    }
    H(b, "<div class=\"modal-f\"><a class=\"btn\" href=\"/people\">%s</a><button class=\"btn danger\">%s</button>"
         "</div>",
      T("Cancel"), title);
    ui_form_end(b);
    ui_modal_end(b);
}

/* "Guest until Sun" / "Guest, ended 18 Oct" (after " · ") */
static void guest_until(Ctx *c, Buf *b, const Person *p) {
    Buf d = {0};
    day_in(c, &d, last_day(p));
    H(b, p->until > c->now ? T("Guest until %s") : T("Guest, ended %s"), buf_cstr(&d));
    buf_free(&d);
}

static void people_card(Ctx *c, Buf *b) {
    ui_card(b, T("People"), NULL, NULL);
    buf_puts(b, "<ul class=\"rows\">");
    int gone = 0;
    for (int i = 0; i < account_count(); i++) {
        Person *p = account_at(i);
        if (p->left) {
            gone++;
            continue;
        }
        H(b, "<li class=\"row\" id=\"people-p%d\">", p->id);
        ui_avatar_deco(b, p, "md");
        H(b, "<div class=\"row-main\"><b>%s", p->name);
        if (p->id == c->me->id) H(b, " <span class=\"muted\">%s</span>", T("(you)"));
        buf_puts(b, "</b><small>");
        if (p->username[0]) {
            H(b, "@%s", p->username);
            if (p->role == ROLE_GUEST && p->until) {
                buf_puts(b, " · ");
                guest_until(c, b, p);
            } else if (p->role != ROLE_MEMBER) {
                H(b, " · %s", role_name(p->role));
            }
        } else {
            H(b, "%s", T("no sign-in"));
            if (p->role == ROLE_CHILD) H(b, " · %s", role_name(ROLE_CHILD));
        }
        int screens = presence_screens(p->id);
        if (presence_is_home(p->id, NULL)) H(b, " · %s", T("home"));
        if (screens) {
            buf_puts(b, " · ");
            H(b, TN(screens, "online on %d screen", "online on %d screens"), screens);
        } else if (p->last_login) {
            Buf ago = {0};
            ui_ago(&ago, p->last_login, c->now);
            buf_puts(b, " · ");
            H(b, T("signed in %s"), buf_cstr(&ago));
            buf_free(&ago);
        }
        buf_puts(b, "</small></div>");
        if (can_edit(c, p) && ui_can("people.save")) {
            H(b, "<a class=\"icon-btn\" href=\"/people?edit=%d\" aria-label=\"", p->id);
            H(b, T("Edit %s"), p->name);
            buf_puts(b, "\">");
            ui_icon(b, "edit");
            buf_puts(b, "</a>");
        }
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ul>");
    if (gone && is_admin(c)) { /* moved out (issue #45): their name stays on what they did */
        H(b, "<h3 class=\"mt\" id=\"people-gone-h\">%s</h3><ul class=\"rows people-gone\">", T("Moved out"));
        for (int i = 0; i < account_count(); i++) {
            Person *p = account_at(i);
            if (!p->left) continue;
            H(b, "<li class=\"row\" id=\"people-p%d\">", p->id);
            ui_avatar_deco(b, p, "md");
            Buf d = {0};
            day_in(c, &d, p->left);
            H(b, "<div class=\"row-main\"><b>%s</b><small>", p->name);
            H(b, T("moved out %s"), buf_cstr(&d));
            buf_free(&d);
            buf_puts(b, "</small></div><div class=\"people-gone-a\">");
            char ids[16], aria[160];
            snprintf(ids, sizeof ids, "%d", p->id);
            snprintf(aria, sizeof aria, T("%s is back in the household"), p->name);
            ui_act(b, "people.return", "btn sm", T("Back in the household"), "id", ids, aria);
            H(b, "<a class=\"btn sm\" href=\"/people?remove=%d\" aria-label=\"", p->id);
            H(b, T("Erase everything about %s"), p->name);
            H(b, "\">%s</a></div></li>", T("Erase"));
        }
        buf_puts(b, "</ul>");
    }
    ui_card_end(b);
}

/* "Recovery codes - N left - Make new codes" (not for display accounts, nor guests: an admin sets a new password). */
static void codes_section(Ctx *c, Buf *b) {
    if (c->me->role == ROLE_DISPLAY || c->me->role == ROLE_GUEST) return;
    int left = auth_codes_left(c->me->id);
    char fresh[AUTH_CODES][AUTH_CODE_LEN];
    bool waiting = auth_codes_fresh(c->sess, fresh) > 0;
    memset(fresh, 0, sizeof fresh);
    H(b, "<h3 class=\"mt\" id=\"people-codes-h\">%s</h3>", T("Recovery codes"));
    if (waiting)
        H(b, "<p class=\"notice\" id=\"people-codes-waiting\">%r</p>",
          T("Your new codes are waiting to be saved. <a href=\"/people/codes\">Show them</a>"));
    buf_puts(b, "<p class=\"muted\" id=\"people-codes-left\">");
    if (left) H(b, T("%d of %d left. Each one gets you back in once if you forget your password."), left, AUTH_CODES);
    else H(b, "%s", T("None yet. Make some: they get you back in if you ever forget your password."));
    buf_puts(b, "</p>");
    H(b, "<details class=\"people-pw\" id=\"people-codes-new\"><summary>%s</summary>",
      left ? T("Make new codes") : T("Make recovery codes"));
    ui_form(b, "people.codes", "stack");
    H(b,
      "<label class=\"field\"><span>%s</span><input type=\"password\" name=\"current\" "
      "autocomplete=\"current-password\" required></label>",
      T("Your password"));
    if (left) H(b, "<p class=\"muted small\">%s</p>", T("Your old codes stop working."));
    buf_puts(b, "<div><button class=\"btn primary\">");
    H(b, T("Make %d new codes"), AUTH_CODES);
    buf_puts(b, "</button></div>");
    ui_form_end(b);
    buf_puts(b, "</details>");
}

/* /people/codes: codes just made, shown on this device until "I've saved them". */
static void codes_page(Ctx *c, Buf *b) {
    c->bare = true;
    ctx_title(c, "%s", T("Recovery codes"));
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    int n = auth_codes_fresh(c->sess, codes);
    buf_puts(b, "<main class=\"center-page\" id=\"main\"><div class=\"welcome card people-codes-card\"><div class=\"welcome-logo\">");
    ui_icon(b, "logo");
    buf_puts(b, "</div>");
    if (!n) {
        int left = auth_codes_left(c->me->id);
        H(b, "<h1>%s</h1><p class=\"sub\">", T("Recovery codes"));
        H(b, T("Codes are only shown right after they're made. You have %d left; you can make new ones under Household."),
          left);
        H(b, "</p><a class=\"btn primary big\" href=\"/people\">%s</a></div></main>", T("Back to Household"));
        return;
    }
    H(b, "<h1>%s</h1><p class=\"sub\">%r</p><ol class=\"people-codes\">", T("Your recovery codes"),
      T("<b>Save these now</b> - this is the only time Hemma shows them."));
    for (int i = 0; i < n; i++) H(b, "<li><code>%s</code></li>", codes[i]);
    memset(codes, 0, sizeof codes);
    buf_puts(b, "</ol><p>");
    H(b,
      T("Forgot your password? Choose <b>Forgot your password?</b> on the sign-in page and enter your username "
        "(<b>%s</b>) and one of these codes. Each code works once."),
      c->me->username);
    H(b, "</p><p class=\"muted small mt\">%s</p>",
      T("Keep them somewhere safe: in a password manager, or printed and put away. Anyone with one of them and your "
        "username can choose a new password for you."));
    H(b, "<div class=\"people-codes-a\"><a class=\"btn\" href=\"/people/codes.txt\" "
         "download=\"hemma-recovery-codes.txt\">%s</a></div>",
      T("Download as a text file"));
    ui_form(b, "people.codes_saved", "people-codes-done");
    H(b, "<button class=\"btn primary big\">%r</button>", T("I've saved them"));
    ui_form_end(b);
    buf_puts(b, "</div></main>");
}

/* GET /people/codes.txt: the same codes as a file to keep (in the language of the request). */
static bool http(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_eq(r->path, "/people/codes.txt")) return false;
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    int n = c->me ? auth_codes_fresh(c->sess, codes) : 0;
    if (!n) {
        const char *msg = T("Recovery codes are only available right after they're made.\n");
        conn_reply(conn, 404, "text/plain; charset=utf-8", msg, strlen(msg), "Cache-Control: no-store\r\n");
        return true;
    }
    char day[11];
    fmt_ymd(c->now, day);
    Buf t = {0};
    buf_printf(&t, T("%s - recovery codes for %s (username: %s), made %s"), app_name(), c->me->name, c->me->username,
               day);
    buf_puts(&t, "\n\n");
    for (int i = 0; i < n; i++) buf_printf(&t, "%2d. %s\n", i + 1, codes[i]);
    memset(codes, 0, sizeof codes);
    buf_puts(&t, "\n");
    buf_puts(&t, T("Forgot your password? On the sign-in page choose \"Forgot your password?\" and enter your\n"
                   "username and one of these codes. Each code works once. Keep this file somewhere safe.\n"));
    conn_reply(conn, 200, "text/plain; charset=utf-8", t.p, t.len,
               "Content-Disposition: attachment; filename=\"hemma-recovery-codes.txt\"\r\nCache-Control: no-store\r\n");
    buf_free(&t);
    return true;
}

/* Issue #41: your calendar on your phone - a feed its own calendar subscribes to (core/feed.c) */
static bool loopback_host(const char *host) {
    return !host[0] || str_starts(host, "127.") || str_starts(host, "localhost") || str_starts(host, "[::1]");
}

static void calendar_card(Ctx *c, Buf *b) {
    ui_card(b, T("Your calendar on your phone"), NULL, NULL);
    char path[96];
    if (!cal_feed_path(c->me->id, path, sizeof path)) {
        H(b, "<p class=\"muted\">%r</p>",
          T("Your bookings, your to-dos and the household's events in your phone's own calendar. It keeps a copy for "
            "when you're out, and its reminders work as usual. Hemma isn't on the internet, so the phone brings it up "
            "to date while it's at home."));
        ui_act(b, "people.cal_make", "btn primary", T("Make my calendar link"), NULL, NULL, NULL);
        ui_card_end(b);
        return;
    }
    if (!c->host[0]) { /* (no address to make the link for) */
        H(b, "<p class=\"notice\">%r</p>", T("Open this page at Hemma's address to see your link."));
    } else {
        bool here_only = loopback_host(c->host); /* 127.0.0.1: a calendar on this computer can, a phone can't */
        if (here_only) {
            buf_puts(b, "<p class=\"notice\">");
            H(b,
              T("You opened Hemma at <b>%s</b>, which only this computer can reach: this link works for a calendar on "
                "this computer. For your phone, open this page on the phone, or at Hemma's address at home."),
              c->host);
            buf_puts(b, "</p>");
        }
        char webcal[200], web[200];
        snprintf(webcal, sizeof webcal, "webcal://%s%s", c->host, path);
        snprintf(web, sizeof web, "http://%s%s", c->host, path);
        H(b, "<p><a class=\"btn primary\" href=\"%s\" data-native>%r</a></p>", webcal, T("Add to this device's calendar"));
        static QrCode q;
        if (!here_only && qr_make(&q, webcal, strlen(webcal), QR_M, -1)) {
            buf_puts(b, "<div class=\"people-cal-qr\">");
            qr_svg(b, &q, T("A code with your calendar link, for a phone's camera"));
            buf_puts(b, "</div>");
        }
        H(b, "<p class=\"small\">%r</p><p><code class=\"people-cal-url\">%s</code></p>",
          here_only ? T("<b>iPhone:</b> tap the button on the phone, and choose Subscribe. <b>Android:</b> install "
                        "ICSx⁵ (free, from Google Play or F-Droid) and add this address in it. A computer's calendar "
                        "subscribes to it too:")
                    : T("<b>iPhone:</b> tap the button on the phone, or scan the code with its camera, and choose "
                        "Subscribe. <b>Android:</b> install ICSx⁵ (free, from Google Play or F-Droid) and add this "
                        "address in it. A computer's calendar subscribes to it too:"),
          web);
    }
    ui_form(b, "people.cal_all", "");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"all\" value=\"1\" data-auto%r> %s</label>",
      str_eq(pref(c->me->id, PREF_CAL_ALL, ""), "1") ? " checked" : "", T("Every booking in the household, not only yours"));
    ui_form_end(b);
    H(b, "<p class=\"muted small\">%s</p><div class=\"people-actions\">",
      T("Anyone with the link can read this calendar: keep it to your own phone and computer. A new link stops the old "
        "one."));
    ui_form_confirm(b, "people.cal_make", "inline", T("Make a new link? Phones with the old one stop getting updates."));
    H(b, "<button class=\"btn\">%s</button>", T("Make a new link"));
    ui_form_end(b);
    ui_act(b, "people.cal_off", "btn", T("Turn it off"), NULL, NULL, NULL);
    buf_puts(b, "</div>");
    ui_card_end(b);
}

static void account_card(Ctx *c, Buf *b) {
    ui_card(b, T("Your account"), NULL, NULL);
    buf_puts(b, "<div class=\"device-me\">");
    ui_avatar_deco(b, c->me, "lg");
    H(b, "<div><b>%s</b><p class=\"muted\">", c->me->name);
    if (c->me->username[0]) H(b, T("Signed in as @%s"), c->me->username);
    if (c->me->role == ROLE_GUEST && c->me->until) { /* (it ends by itself: issue #44) */
        Buf d = {0};
        day_in(c, &d, last_day(c->me));
        buf_puts(b, " · ");
        H(b, T("a guest until %s"), buf_cstr(&d));
        buf_free(&d);
    }
    buf_puts(b, "</p></div></div>");
    if (c->me->pass[0]) {
        bool temp = auth_temporary(c->me->id);
        if (temp)
            H(b, "<p class=\"notice\" id=\"people-temp\" role=\"alert\">%s</p>",
              T("You signed in with a temporary password. Choose your own now."));
        H(b, "<details class=\"people-pw\" id=\"people-pw\"%r><summary>%s</summary>", temp ? " open" : "",
          T("Change password"));
        ui_form(b, "people.password", "stack");
        H(b,
          "<label class=\"field\"><span>%s</span><input type=\"password\" name=\"current\" "
          "autocomplete=\"current-password\" required></label><div class=\"field-row\"><label class=\"field\">"
          "<span>%s</span><input type=\"password\" name=\"new\" autocomplete=\"new-password\" "
          "required minlength=\"8\"></label><label class=\"field\"><span>%s</span><input type=\"password\" "
          "name=\"again\" autocomplete=\"new-password\" required minlength=\"8\"></label></div>"
          "<div><button class=\"btn primary\">%s</button></div>",
          temp ? T("Temporary password") : T("Current password"), T("New password"), T("Again"), T("Change password"));
        ui_form_end(b);
        buf_puts(b, "</details>");
        codes_section(c, b);
    }
    /* signed-in devices */
    H(b, "<h3 class=\"mt\">%s</h3><ul class=\"rows people-devices\">", T("Signed in on"));
    int others = 0;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        if (s->person != c->me->id) continue;
        bool here = c->sess && str_eq(s->token, c->sess->token);
        if (!here) others++;
        Buf agent = {0}, ago = {0};
        if (s->agent[0]) i18n_unpack(&agent, s->agent); /* ("Chrome on Windows", kept packed by the core) */
        else buf_puts(&agent, T("A browser"));
        H(b, "<li class=\"row\"><div class=\"row-main\"><b>%s</b><small>", buf_cstr(&agent));
        if (here) H(b, "%s · ", T("this device"));
        ui_ago(&ago, MAX(s->seen, s->created), c->now);
        H(b, T("active %s"), buf_cstr(&ago));
        buf_puts(b, "</small></div></li>");
        buf_free(&agent);
        buf_free(&ago);
    }
    buf_puts(b, "</ul><div class=\"people-actions\">");
    if (others) ui_act(b, "people.signout_others", "btn", T("Sign out other devices"), NULL, NULL, NULL);
    /* a real POST (not over the WebSocket): the cookie has to be cleared by an HTTP response */
    H(b, "<form method=\"post\" action=\"/logout\" class=\"inline\"><button class=\"btn\">%s</button></form></div>",
      T("Sign out"));
    ui_card_end(b);
}

/* 2.0: the Android app, served by this Hemma itself (core: /static/hemma.apk, built in when the build found Android's
   tools) - not shown inside the app, nor to a display account */
/* What the app does (2.3; owner: "add more information about the app") - the same list in the browser and in the app */
static void app_what(Buf *b) {
    H(b, "<ul class=\"people-app-what\"><li>%s</li><li>%s</li><li>%s</li><li>%s</li><li>%s</li></ul>",
      T("Finds Hemma at home by itself - no address to type."), T("Keeps you signed in, and opens straight to Hemma."),
      T("Plays audiobooks with the phone locked, with the book and the chapter on the lock screen."),
      T("Saves videos and audiobooks for offline: on a trip, on the train, wherever there's no Hemma."),
      T("Talks only to Hemma on your home network - nothing else, nowhere else."));
}

static void app_card(Ctx *c, Buf *b) {
    long long size = app_static_size("/hemma.apk");
    if (size < 0 || c->me->role == ROLE_DISPLAY) return;
    if (session_in_app(c->sess)) { /* in the app: what it does, and where its saved videos are */
        ui_card(b, T("The Hemma app"), NULL, NULL);
        H(b, "<p>%s</p>", T("You're using Hemma's app for Android. Besides Hemma itself, it:"));
        app_what(b);
        H(b, "<div class=\"people-actions\"><a class=\"btn\" href=\"hemma-app:offline\">%s</a></div>", T("Saved for offline"));
        H(b, "<p class=\"muted small\">%s</p>",
          T("Save something with Save for offline on its page in Watch. A new Hemma brings a new app: Get the app for "
            "Android in a phone's browser installs it over this one."));
        ui_card_end(b);
        return;
    }
    char kb[24];
    snprintf(kb, sizeof kb, "%lld KB", (size + 1023) / 1024);
    ui_card(b, T("Hemma on your phone"), NULL, NULL);
    H(b, "<p>%s</p>", T("Hemma has an app for Android phones. Besides Hemma itself, it:"));
    app_what(b);
    H(b,
      "<div class=\"people-actions\"><a class=\"btn primary\" id=\"people-app\" href=\"/static/hemma.apk?v=%s\" "
      "download=\"Hemma.apk\">%s</a><span class=\"muted small\">%s</span></div>",
      app_asset_version(), T("Get the app for Android"), kb);
    H(b, "<p class=\"muted small\">%s</p><p class=\"muted small\">%s</p>",
      T("Open the file once it's downloaded. Android asks whether your browser may install apps: allow it, this once. "
        "The app only ever talks to Hemma on your home network."),
      T("On an iPhone: open Hemma in Safari, tap Share, then Add to Home Screen."));
    ui_card_end(b);
}

/* 2.1: Home offers the app to a browser on Android (owner, 2026-10-29: "Noticed you are in the web, download the app
   instead!" - not tucked away under Your account) until "Not now" (PREF_APP_HINT), or once the person has signed in
   from the app anywhere. Session.agent is the core's summary of the browser, "Chrome on Android" (packed: its names
   kept as they are). */
#define PREF_APP_HINT "people.app_hint"

static bool uses_the_app(int person) {
    for (int i = 0; i < tbl_count(&T_session); i++) {
        const Session *s = tbl_at(&T_session, i);
        if (s->person == person && session_in_app(s)) return true;
    }
    return false;
}

static void widget(Ctx *c, Buf *b, int where) {
    if (where != WIDGET_HOME || !c->me || c->me->role == ROLE_DISPLAY || !c->sess || session_in_app(c->sess)) return;
    if (!strstr(c->sess->agent, "Android") || app_static_size("/hemma.apk") < 0) return;
    if (str_eq(pref(c->me->id, PREF_APP_HINT, ""), "off")) return;
    if (uses_the_app(c->me->id) && c->host[0]) {
        /* someone with the app, in the browser (2.2; owner: "open the app instead!"): an Android intent link opens it
           at its Home (the app's hemma://open/ filter); where no app takes it - none here, or one from before 2.2 -
           Chrome goes to the fallback: the download, which installs over the old one */
        char fallback[160];
        snprintf(fallback, sizeof fallback, "http://%s/static/hemma.apk?v=%s", c->host, app_asset_version());
        H(b, "<section class=\"card\" id=\"people-apphint\"><header class=\"card-h\"><h2>%s</h2></header><p>%s</p>",
          T("Open the app instead!"),
          T("You're using Hemma in the browser, and you've signed in from the Hemma app before. The app keeps you "
            "signed in and plays audiobooks with the phone locked."));
        H(b,
          "<div class=\"people-actions\"><a class=\"btn primary\" href=\"intent://open/#Intent;scheme=hemma;"
          "package=io.github.kloparn.hemma;S.browser_fallback_url=%u;end\">%s</a>",
          fallback, T("Open the app"));
        ui_act(b, "people.app_hint", "btn", T("Not now"), NULL, NULL, NULL);
        H(b, "</div><p class=\"muted small\">%s</p></section>",
          T("Not on this phone, or an app from before Hemma 2.2? The button gets the app instead: open the file to "
            "install it."));
        return;
    }
    if (uses_the_app(c->me->id)) return;
    H(b, "<section class=\"card\" id=\"people-apphint\"><header class=\"card-h\"><h2>%s</h2></header><p>%s</p>",
      T("Hemma has an app for your phone"),
      T("You're using Hemma in the browser. The app finds Hemma at home by itself, keeps you signed in and plays "
        "audiobooks with the phone locked."));
    H(b,
      "<div class=\"people-actions\"><a class=\"btn primary\" href=\"/static/hemma.apk?v=%s\" download=\"Hemma.apk\">"
      "%s</a>",
      app_asset_version(), T("Get the app"));
    ui_act(b, "people.app_hint", "btn", T("Not now"), NULL, NULL, NULL);
    H(b, "</div><p class=\"muted small\">%s</p></section>",
      T("Open the file once it's downloaded. Android asks whether your browser may install apps: allow it, this once. "
        "The app only ever talks to Hemma on your home network."));
}

static void act_app_hint(Ctx *c, Fields *f) {
    (void)f;
    pref_set(c->me->id, PREF_APP_HINT, "off");
    ctx_ok(c, "%s", T("The app stays under Household → Your account."));
}

/* Issue #46: the language Hemma speaks to you - Automatic (the browser's), or one of ours, each named in itself.
   A display account can't send forms: an admin picks its language in its Edit dialog. */
static void lang_card(Ctx *c, Buf *b) {
    ui_card(b, T("Language"), NULL, NULL);
    if (c->me->role == ROLE_DISPLAY) {
        H(b, "<p class=\"muted\">%r</p>", T("An admin picks this screen's language, under Household."));
        ui_card_end(b);
        return;
    }
    Lang now;
    const char *cur = pref(c->me->id, PREF_LANG, "");
    bool set = cur[0] && i18n_parse(cur, &now);
    ui_form(b, "people.lang", "stack people-lang");
    H(b, "<fieldset class=\"people-lang-f\"><legend class=\"sr-only\">%s</legend>", T("Language"));
    H(b, "<label class=\"check\"><input type=\"radio\" name=\"lang\" value=\"\" data-auto%r> %s <small class=\"muted\">",
      set ? "" : " checked", T("Automatic"));
    if (c->dev_lang >= 0 && c->dev_lang < LANG_COUNT) H(b, T("(this browser: %s)"), i18n_name((Lang)c->dev_lang));
    else H(b, "%s", T("(this browser asks for none of these, so English is used)"));
    buf_puts(b, "</small></label>");
    for (int l = 0; l < LANG_COUNT; l++)
        H(b,
          "<label class=\"check\"><input type=\"radio\" name=\"lang\" value=\"%s\" data-auto%r> <span lang=\"%s\">%s"
          "</span></label>",
          i18n_code((Lang)l), set && (int)now == l ? " checked" : "", i18n_code((Lang)l), i18n_name((Lang)l));
    H(b, "</fieldset><noscript><button class=\"btn\">%s</button></noscript>", T("Save"));
    ui_form_end(b);
    ui_card_end(b);
}

/* Privacy inside the household (issue #42): what others see of you. One form, sent as a whole on any change. */
static void privacy_card(Ctx *c, Buf *b) {
    if (c->me->role == ROLE_DISPLAY) return;
    int me = c->me->id;
    ui_card(b, T("Privacy"), NULL, NULL);
    H(b, "<p class=\"muted\">%s</p>", T("What the rest of the household sees of you."));
    ui_form(b, "people.privacy", "stack people-privacy");
    ui_hidden(b, "all", "1");
    static const char *const wall[] = {N_("With home or away"), N_("Just my name"), N_("Not at all")};
    static const char *const wall_v[] = {"", "name", "no"};
    int w = privacy_wall(me);
    H(b, "<label class=\"field\"><span>%s</span><select name=\"wall\" data-auto>", T("On the wall tablet"));
    for (int i = 0; i < countof(wall); i++)
        H(b, "<option value=\"%s\"%r>%s</option>", wall_v[i], i == w ? " selected" : "", T(wall[i]));
    buf_puts(b, "</select></label>");
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"doing\" value=\"1\" data-auto%r> %s</label>",
      privacy_doing(me) ? " checked" : "", T("Others see when I have Hemma open, and which part"));
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"tv\" value=\"1\" data-auto%r> %r</label>",
      privacy_on_tv(c->me) ? " checked" : "", T("On the TV's <i>Who's watching?</i>"));
    H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"home\" value=\"1\" data-auto%r> %r</label>",
      privacy_home(me) ? " checked" : "", T("My phone and computer may tell Hemma when I'm home"));
    ui_form_end(b);
    const Module *net = module_find("net"), *screens = module_find("screens");
    buf_puts(b, "<p class=\"muted small\">");
    if (screens && module_visible(screens, c)) {
        buf_puts(b, T("Whoever picks you on the TV sees and can clear what you're watching: a PIN for it is under "
                      "<a href=\"/screens\">Screens</a>."));
        buf_puts(b, " ");
    }
    if (net && module_visible(net, c))
        buf_puts(b, T("Who's home comes from the devices an admin marked as yours, on the <a href=\"/net\">Network</a> "
                      "page."));
    buf_puts(b, "</p>");
    ui_card_end(b);
}

/* (the card's heading says "Your home": "Home" is the dashboard's name, and Spanish words them apart) */
static void home_card(Ctx *c, Buf *b) {
    ui_card(b, T("Your home"), NULL, NULL);
    if (is_admin(c)) {
        ui_form(b, "household.save", "row-form");
        H(b, "<input name=\"name\" maxlength=\"40\" value=\"%s\" aria-label=\"%s\"><button class=\"btn\">%s</button>",
          app_name(), T("Home name"), T("Rename"));
        ui_form_end(b);
    } else {
        H(b, "<p><b>%s</b></p>", app_name());
    }
    H(b, "<h3 class=\"mt\">%s</h3><p class=\"muted\">%r</p><a class=\"btn\" href=\"/wall\" data-native>%s</a>",
      T("Wall display"),
      T("An always-on overview for a tablet in the hallway or kitchen. Tip: give the tablet its own <i>Display</i> "
        "sign-in so it can only look, and pick its cards under <a href=\"/screens\">Screens</a>."),
      T("Open wall display"));
    ui_card_end(b);
}

static void page(Ctx *c, Buf *b) {
    if (str_eq(c->path, "/people/codes")) {
        codes_page(c, b);
        return;
    }
    ctx_title(c, "%s", T("Household"));
    ui_head(b, T("Household"), app_name());
    if (is_admin(c)) {
        buf_puts(b, "<a class=\"btn\" href=\"/people?add=1\">");
        ui_icon(b, "plus");
        H(b, "<span>%s</span></a>", T("Add someone"));
    }
    ui_head_end(b);
    if (str_eq(ctx_q(c, "recovered"), "1")) { /* back in with a recovery code (core/auth.c) */
        int left = auth_codes_left(c->me->id);
        buf_puts(b, "<p class=\"notice\" id=\"people-recovered\">");
        if (left > 2)
            H(b, T("Your new password is set and you're signed in. You have %d recovery codes left."), left);
        else
            H(b,
              TN(left,
                 "Your new password is set and you're signed in. You have %d recovery code left - make new ones under "
                 "Recovery codes.",
                 "Your new password is set and you're signed in. You have %d recovery codes left - make new ones under "
                 "Recovery codes."),
              left);
        buf_puts(b, "</p>");
    }
    bool guest = is_guest(c); /* a guest: their own account only (issue #44) */
    buf_puts(b, "<div class=\"grid2\"><div>");
    if (!guest) {
        people_card(c, b);
        home_card(c, b);
    } else {
        account_card(c, b);
        lang_card(c, b);
        app_card(c, b);
    }
    buf_puts(b, "</div><div>");
    if (!guest) {
        account_card(c, b);
        lang_card(c, b);
        app_card(c, b);
    }
    privacy_card(c, b);
    if (c->me->pass[0] && c->me->role != ROLE_DISPLAY && !guest) calendar_card(c, b);
    buf_puts(b, "</div></div>");

    int edit = (int)ctx_qint(c, "edit", 0), rm = (int)ctx_qint(c, "remove", 0);
    Person *ep = person_get(edit), *rp = person_get(rm);
    if (str_eq(ctx_q(c, "add"), "1") && is_admin(c)) person_modal(c, b, NULL);
    else if (ep && !ep->left && can_edit(c, ep)) person_modal(c, b, ep);
    else if (rp && is_admin(c) && rp->id != c->me->id) remove_modal(c, b, rp);
}

/* ---------------------------------------------------------------- actions */

static bool name_taken(const char *name, int except) {
    for (int i = 0; i < account_count(); i++)
        if (account_at(i)->id != except && str_ieq(account_at(i)->name, name)) return true;
    return false;
}

/* Admins who can sign in once a change lands: `after` stands in for its stored row (NULL = no
   edit), `removed` (0 = nobody) is gone. Counted after the change, not before: blanking your own
   username and then removing the other admin used to leave nobody who could sign in (issue #14). */
static int admins_after(const Person *after, int removed) {
    int n = 0;
    for (int i = 0; i < account_count(); i++) {
        const Person *q = account_at(i);
        if (q->id == removed) continue;
        if (after && q->id == after->id) q = after;
        n += q->role == ROLE_ADMIN && q->username[0] && q->pass[0];
    }
    return n;
}

/* Refuses a change that would leave the household without an admin who can sign in (only the
   server's reset brings one back). */
static bool keeps_an_admin(Ctx *c, const Person *after, int removed) {
    if (admins_after(after, removed) > 0 || admins_after(NULL, 0) == 0) return true;
    ctx_fail(c, "%s", T("Keep at least one admin who can sign in"));
    return false;
}

static void act_save(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    int me = c->me->id; /* c->me dangles once tbl_put grows the person table */
    Person p = {0};
    if (id) {
        Person *old = person_get(id);
        if (!old) {
            ctx_fail(c, "%s", T("Unknown person"));
            return;
        }
        if (!can_edit(c, old)) {
            ctx_fail(c, "%s", T("Only an admin can change other people"));
            return;
        }
        p = *old;
    } else if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can add people"));
        return;
    }
    char name[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    if (!name[0]) {
        ctx_fail_at(c, "name", "%s", T("A name, please"));
        return;
    }
    if (name_taken(name, p.id)) {
        ctx_fail_at(c, "name", T("There's already a %s here"), name);
        return;
    }
    /* the language (issue #46): an admin's dialog sends it, for anyone's account */
    const char *lang = NULL;
    if (is_admin(c) && fields_get(f, "lang") && !lang_value(fields_str(f, "lang"), &lang)) {
        ctx_fail_at(c, "lang", "%s", T("Pick a language from the list"));
        return;
    }
    str_copy(p.name, sizeof p.name, name);
    if (!id || fields_get(f, "emoji")) str_copy(p.emoji, sizeof p.emoji, fields_str(f, "emoji"));
    if (!id || fields_get(f, "hue")) p.hue = (int)CLAMP(fields_int(f, "hue", 200), 0, 359);
    if (is_admin(c)) {
        /* sign-in fields change only when they were sent: {"id":2,"name":"B"} renames, nothing more */
        bool set_user = fields_get(f, "username") != NULL, set_role = fields_get(f, "role") != NULL;
        char user[48];
        str_copy(user, sizeof user, set_user ? fields_str(f, "username") : p.username);
        str_trim(user);
        for (char *q = user; *q; q++)
            if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
        const char *pw = fields_str(f, "password");
        if (*pw && id && id == me) { /* review: that skipped the current-password check and kept other sessions */
            ctx_fail_at(c, "password", "%s", T("Change your own password under Your account - it asks for the current one first"));
            return;
        }
        if (set_user && user[0]) {
            if (!auth_username_ok(user)) {
                ctx_fail_at(c, "username", "%s", T("Usernames are 2–32 of a–z, 0–9 and . _ -"));
                return;
            }
            Person *other = person_by_username(user);
            if (other && other->id != p.id) {
                ctx_fail_at(c, "username", T("@%s is taken"), user);
                return;
            }
            if (!p.pass[0] && !*pw) {
                ctx_fail_at(c, "password", T("Give %s a password to sign in with"), p.name);
                return;
            }
        }
        if (*pw) {
            const char *why = auth_password_problem(pw);
            if (why) {
                ctx_fail_at(c, "password", "%s", T(why)); /* (core/auth.c's English, translated here) */
                return;
            }
            if (!user[0]) {
                ctx_fail_at(c, "username", "%s", T("A password needs a username too"));
                return;
            }
            if (!auth_hash(pw, p.pass, sizeof p.pass)) {
                ctx_fail(c, "%s", T("Passwords can't be set: Hemma has no password secret yet (System says how to add one)"));
                return;
            }
        }
        if (set_user) {
            str_copy(p.username, sizeof p.username, user);
            if (!user[0]) p.pass[0] = '\0'; /* no username, no sign-in */
        }
        if (set_role && !(id && id == me)) p.role = (int)CLAMP(fields_int(f, "role", ROLE_MEMBER), 0, ROLE_COUNT - 1);
        const char *day = fields_str(f, "until");
        int y, m, d;
        if (p.role != ROLE_GUEST) {
            p.until = 0;
        } else if (day[0] || !p.until) { /* a guest signs in until a day (issue #44); not sent: it stays */
            if (!parse_ymd(day, &y, &m, &d)) {
                ctx_fail_at(c, "until", "%s", T("Pick the guest's last day"));
                return;
            }
            if (dt_make(y, m, d, 0, 0) < day_start(c->now)) {
                ctx_fail_at(c, "until", "%s", T("Pick a day from today on"));
                return;
            }
            p.until = dt_make(y, m, d + 1, 0, 0);
        }
        if (!keeps_an_admin(c, &p, 0)) return;
        if (*pw) auth_forget_failures(p.username); /* a fresh password: earlier lockouts are moot */
    }
    if (!id) p.created = c->now;
    bool signin_changed = (fields_get(f, "username") && !p.username[0]) || fields_str(f, "password")[0];
    tbl_put(&T_person, &p); /* from here on only locals: c->me may dangle */
    if (!p.username[0] || p.role == ROLE_DISPLAY || p.role == ROLE_GUEST) auth_codes_drop(p.id); /* (no codes) */
    if (id && signin_changed && p.id != me)
        sessions_drop(p.id, NULL); /* new password or sign-in removed: their old sessions end */
    if (lang) pref_set(p.id, PREF_LANG, lang);
    if (!id) activity_add(me, "👋", "/people", N_("%s joined the household"), p.name);
    Lang says = lang_for(c, me); /* (an admin who changed their own language reads the toast in the new one) */
    if (id) ctx_ok(c, "%s", T_in(says, "Saved"));
    else ctx_ok(c, T_in(says, "Added %s"), p.name);
    ctx_redirect(c, "/people");
}

/* Issue #45: "keep" = they moved out (their history stays, what's ahead of them is handed on - person_forget),
   "erase" = everything about them goes. A display account, or someone who moved out already: erased. */
static void act_delete(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can remove people"));
        return;
    }
    Person *p = person_get((int)fields_int(f, "id", 0));
    if (!p || p->id == c->me->id) {
        ctx_fail(c, "%s", T("Can't remove that person"));
        return;
    }
    char name[48];
    str_copy(name, sizeof name, p->name);
    int id = p->id;
    bool keep = !str_eq(fields_str(f, "how"), "erase") && p->role != ROLE_DISPLAY && !p->left;
    if (!keeps_an_admin(c, NULL, id)) return;
    Forget fg = {.person = id, .keep = keep, .apply = true, .now = c->now};
    person_forget(&fg); /* (p is gone or changed from here on) */
    if (keep) {
        activity_add(c->me->id, "👋", "/people", N_("%s moved out"), name);
        ctx_ok(c, T("%s moved out - their history stays"), name);
    } else {
        ctx_ok(c, T("Removed %s and everything about them"), name);
    }
    ctx_redirect(c, "/people");
}

/* Someone who moved out is back (issue #45): in the household again, without a sign-in until an admin gives one. */
static void act_return(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    Person *p = person_get((int)fields_int(f, "id", 0));
    if (!p || !p->left) {
        ctx_fail(c, "%s", T("They're not among those who moved out"));
        return;
    }
    Person rec = *p;
    rec.left = 0;
    tbl_put(&T_person, &rec);
    activity_add(c->me->id, "👋", "/people", N_("%s is back in the household"), rec.name);
    ctx_ok(c, T("%s is back - give them a sign-in under Edit if they need one"), rec.name);
}

/* Your privacy (issue #42): the whole form comes each time ("all"), so an unticked box means no; without "all"
   only the fields sent change ("tv=0"). */
static bool said_yes(Fields *f, const char *key) { return str_eq(fields_str(f, key), "1"); }

static void act_privacy(Ctx *c, Fields *f) {
    int me = c->me->id;
    bool all = fields_get(f, "all") != NULL;
    const char *wall = fields_str(f, "wall");
    if (all || fields_get(f, "wall")) pref_set(me, PREF_WALL, str_eq(wall, "name") || str_eq(wall, "no") ? wall : "");
    if (all || fields_get(f, "doing")) pref_set(me, PREF_DOING, said_yes(f, "doing") ? "" : "no");
    if (all || fields_get(f, "tv")) pref_set(me, PREF_ON_TV, said_yes(f, "tv") ? "1" : "0");
    if (all || fields_get(f, "home")) {
        bool home = said_yes(f, "home");
        pref_set(me, PREF_NOT_HOME, home ? "" : "1");
        if (!home) presence_forget(me); /* (unknown from now on, never "away") */
    }
    ctx_ok(c, "%s", T("Saved"));
}

/* Your language (issue #46): "" = automatic (the browser's), else one of ours. "id" names someone else's:
   only an admin's (a display account can't send forms - this is how its language gets set, besides people.save).
   The core re-renders the person's screens; wire.js reloads one whose language changed. */
static void act_lang(Ctx *c, Fields *f) {
    int me = c->me->id, id = (int)fields_int(f, "id", me);
    if (id != me && !is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change other people"));
        return;
    }
    if (!person_get(id)) {
        ctx_fail(c, "%s", T("Unknown person"));
        return;
    }
    const char *lang = NULL;
    if (!lang_value(fields_str(f, "lang"), &lang)) {
        ctx_fail_at(c, "lang", "%s", T("Pick a language from the list"));
        return;
    }
    pref_set(id, PREF_LANG, lang);
    ctx_ok(c, "%s", T_in(lang_for(c, me), "Saved")); /* (in the language this screen shows from now on) */
}

/* "Too many tries. Wait N minutes and try again." */
static void too_many(Ctx *c, int wait) {
    int min = MAX(1, (wait + 59) / 60);
    ctx_fail(c, TN(min, "Too many tries. Wait %d minute and try again.", "Too many tries. Wait %d minutes and try again."),
             min);
}

static void act_password(Ctx *c, Fields *f) {
    const char *cur = fields_str(f, "current"), *nw = fields_str(f, "new"), *again = fields_str(f, "again");
    int wait = 0;
    LoginResult r = auth_recheck(c->me, cur, c->ip, c->now, &wait); /* throttled: no guessing from a borrowed session */
    if (r == LOGIN_THROTTLED) {
        too_many(c, wait);
        return;
    }
    if (r != LOGIN_OK) {
        ctx_fail_at(c, "current", "%s", T("Your current password isn't right"));
        return;
    }
    const char *why = auth_password_problem(nw);
    if (why) {
        ctx_fail_at(c, "new", "%s", T(why));
        return;
    }
    if (!str_eq(nw, again)) {
        ctx_fail_at(c, "again", "%s", T("The new passwords don't match"));
        return;
    }
    int n = auth_set_password(c->me->id, nw, c->sess); /* c->me may dangle from here on */
    if (n < 0) {
        ctx_fail(c, "%s", T("Passwords can't be set: Hemma has no password secret yet (System says how to add one)"));
        return;
    }
    if (n)
        ctx_ok(c, TN(n, "Password changed — signed out %d other device", "Password changed — signed out %d other devices"),
               n);
    else ctx_ok(c, "%s", T("Password changed"));
}

/* New recovery codes (the old ones stop working): asks for the password, like changing it,
   so a borrowed, signed-in phone can't quietly mint a way into the account. */
static void act_codes(Ctx *c, Fields *f) {
    int wait = 0;
    LoginResult r = auth_recheck(c->me, fields_str(f, "current"), c->ip, c->now, &wait);
    if (r == LOGIN_THROTTLED) {
        too_many(c, wait);
        return;
    }
    if (r != LOGIN_OK) {
        ctx_fail_at(c, "current", "%s", T("Your password isn't right"));
        return;
    }
    if (!auth_codes_new(c->me->id, c->sess)) {
        ctx_fail(c, "%s", T("This account can't have recovery codes"));
        return;
    }
    ctx_redirect(c, "/people/codes");
}

static void act_codes_saved(Ctx *c, Fields *f) {
    auth_codes_seen(c->sess);
    ctx_ok(c, "%s", T("Recovery codes saved - keep them safe"));
    ctx_redirect(c, "/people");
}

static void act_signout_others(Ctx *c, Fields *f) {
    int n = sessions_drop(c->me->id, c->sess);
    ctx_ok(c, TN(n, "Signed out %d other device", "Signed out %d other devices"), n);
}

static void act_signout(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    Person *p = person_get((int)fields_int(f, "id", 0));
    if (!p) {
        ctx_fail(c, "%s", T("Unknown person"));
        return;
    }
    char name[48];
    str_copy(name, sizeof name, p->name);
    int n = sessions_drop(p->id, p->id == c->me->id ? c->sess : NULL);
    ctx_ok(c, TN(n, "Signed %s out of %d device", "Signed %s out of %d devices"), name, n);
}

static void act_household(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can rename the home"));
        return;
    }
    char name[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    setting_set("household.name", name[0] ? name : "Hemma");
    ctx_ok(c, "%s", T("Saved"));
}

/* The calendar link (issue #41): PREF_CAL counts the links made - off is that count made negative, so a
   link turned off never works again, and the next one is new. */
static void act_cal_make(Ctx *c, Fields *f) {
    (void)f;
    if (!c->me->pass[0]) {
        ctx_fail(c, "%s", T("A calendar link is for people who sign in"));
        return;
    }
    long long n = llabs(atoll(pref(c->me->id, PREF_CAL, "0"))) + 1;
    char v[24];
    snprintf(v, sizeof v, "%lld", n);
    pref_set(c->me->id, PREF_CAL, v);
    ctx_ok(c, "%s", n == 1 ? T("Your calendar link is ready") : T("A new calendar link - the old one stopped working"));
}

static void act_cal_off(Ctx *c, Fields *f) {
    (void)f;
    long long n = atoll(pref(c->me->id, PREF_CAL, "0"));
    if (n > 0) {
        char v[24];
        snprintf(v, sizeof v, "%lld", -n);
        pref_set(c->me->id, PREF_CAL, v);
    }
    ctx_ok(c, "%s", T("Turned off - phones that used it stop getting updates"));
}

static void act_cal_all(Ctx *c, Fields *f) {
    bool all = fields_get(f, "all") != NULL;
    pref_set(c->me->id, PREF_CAL_ALL, all ? "1" : "");
    ctx_ok(c, "%s", all ? T("Every booking goes in your calendar") : T("Only your own bookings go in your calendar"));
}

/* Children and guests (issue #44) look after their own account: profile, password, devices, privacy - a child
   their calendar link and recovery codes too (a guest's sign-in ends by itself). */
static const Action actions[] = {
    {"people.cal_make", act_cal_make, .may = ACT_CHILD},
    {"people.cal_off", act_cal_off, .may = ACT_CHILD},
    {"people.cal_all", act_cal_all, .may = ACT_CHILD},
    {"people.save", act_save, .may = ACT_ALL},
    {"people.delete", act_delete},
    {"people.return", act_return},
    {"people.privacy", act_privacy, .may = ACT_ALL},
    {"people.lang", act_lang, .may = ACT_ALL},
    {"people.password", act_password, .may = ACT_ALL},
    {"people.codes", act_codes, .may = ACT_CHILD},
    {"people.codes_saved", act_codes_saved, .may = ACT_CHILD},
    {"people.signout_others", act_signout_others, .may = ACT_ALL},
    {"people.signout", act_signout},
    {"people.app_hint", act_app_hint, .may = ACT_ALL},
    {"household.save", act_household},
    {0},
};

/* Issue #39: a guest's sign-in ends at a local midnight (the one after their last day) */
static int rezone(Rezone *z) {
    int n = 0;
    for (int i = 0; z->days && i < account_count(); i++) {
        Person p = *account_at(i);
        if (!p.until || !rezone_day(z, &p.until)) continue;
        n++;
        if (z->apply) tbl_put(&T_person, &p);
    }
    return n;
}

static const Module people = {
    .id = "people",
    .title = N_("Household"),
    .icon = "users",
    .nav = 90,
    .page = page,
    .actions = actions,
    .http = http,
    .rezone = rezone,
    .widget = widget,
    .widget_order = 2, /* (2.1: the app for an Android phone's browser - near the top, after System's) */
};
MODULE(people)
