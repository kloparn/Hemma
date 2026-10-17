/* modules/people — the household: people, their sign-ins, your account.
 *
 * Admins add people and decide who can sign in (and as what: member, admin,
 * or a read-only display for wall tablets). Everyone manages their own
 * profile, password, recovery codes and signed-in devices. Signing in itself
 * is core (core/auth.c), so is the first admin (the welcome page, or the
 * environment). /people/codes shows freshly made recovery codes once. */
#include "../../core/app.h"
#include "../../core/auth.h"
#include "../../core/ui.h"

static const char *const EMOJI[] = {"🦊", "🐻", "🐼", "🐨", "🐯", "🦁", "🐸", "🐵", "🦄", "🐙",
                                    "🐝", "🦉", "🐳", "🐢", "🌻", "🍄", "⭐", "🌙", "🔥", "🎸"};
static const char *const ROLE_NAMES[] = {"Member", "Admin", "Display"};

static int pick_hue(void) {
    static const int hues[] = {200, 335, 150, 25, 255, 45, 175, 310, 90, 0, 220, 280};
    return hues[person_count() % countof(hues)];
}

static void emoji_picker(Buf *b, const char *selected) {
    buf_puts(b, "<div class=\"emojis\"><label class=\"emo\"><input type=\"radio\" name=\"emoji\" value=\"\"");
    if (!selected || !selected[0]) buf_puts(b, " checked");
    buf_puts(b, "><span title=\"Initials\">Aa</span></label>");
    for (int i = 0; i < countof(EMOJI); i++)
        H(b, "<label class=\"emo\"><input type=\"radio\" name=\"emoji\" value=\"%s\"%r><span>%s</span></label>", EMOJI[i],
          str_eq(selected, EMOJI[i]) ? " checked" : "", EMOJI[i]);
    buf_puts(b, "</div>");
}

static bool can_edit(const Ctx *c, const Person *p) { return is_admin(c) || (c->me && p && p->id == c->me->id); }

/* ------------------------------------------------------------------ pages */

static void person_modal(Ctx *c, Buf *b, Person *p) {
    bool admin = is_admin(c);
    ui_modal(b, p ? (p->id == c->me->id ? "Your profile" : "Edit person") : "Add someone", "/people");
    ui_form(b, "people.save", "stack");
    if (p) ui_hidden_int(b, "id", p->id);
    H(b,
      "<label class=\"field\"><span>Name</span><input name=\"name\" required maxlength=\"40\" autocomplete=\"off\" "
      "value=\"%s\"%r></label>",
      p ? p->name : "", p ? "" : " autofocus");
    buf_puts(b, "<div class=\"field\"><span>Avatar</span>");
    emoji_picker(b, p ? p->emoji : "");
    buf_puts(b, "</div><div class=\"field\"><span>Colour</span>");
    ui_hue_picker(b, "hue", p ? p->hue : pick_hue());
    buf_puts(b, "</div>");
    if (admin) {
        bool self = p && p->id == c->me->id;
        H(b,
          "<fieldset class=\"people-signin\"><legend>Signing in</legend><p class=\"muted small\">Leave the username "
          "empty for people who don't need their own sign-in (things can still be booked for them).</p>"
          "<div class=\"field-row\"><label class=\"field\"><span>Username</span><input name=\"username\" "
          "maxlength=\"32\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\" value=\"%s\" "
          "placeholder=\"e.g. eva\"></label><label class=\"field\"><span>Can</span><select name=\"role\"%r>",
          p ? p->username : "", self ? " disabled" : "");
        for (int r = 0; r < countof(ROLE_NAMES); r++) {
            static const char *hints[] = {"use everything", "also manage people", "only look (wall tablet)"};
            H(b, "<option value=\"%d\"%r>%s — %s</option>", r, (p ? p->role : ROLE_MEMBER) == r ? " selected" : "",
              ROLE_NAMES[r], hints[r]);
        }
        buf_puts(b, "</select></label></div>");
        if (self) /* your own: under Your account, which asks for the current one first */
            buf_puts(b, "<p class=\"muted small\">Change your own password under <b>Your account</b>.</p>");
        else
            H(b,
              "<label class=\"field\"><span>%s</span><input type=\"password\" name=\"password\" "
              "autocomplete=\"new-password\" maxlength=\"200\" placeholder=\"%s\"></label>",
              p && p->pass[0] ? "New password" : "Password",
              p && p->pass[0] ? "Leave empty to keep the current one" : "At least 8 characters");
        buf_puts(b, "</fieldset>");
    }
    buf_puts(b, "<div class=\"modal-f\"><button class=\"btn primary\">Save</button></div>");
    ui_form_end(b);
    if (admin && p && p->id != c->me->id) {
        buf_puts(b, "<div class=\"danger-zone people-danger\">");
        if (p->pass[0]) {
            ui_form(b, "people.signout", "inline");
            ui_hidden_int(b, "id", p->id);
            H(b, "<button class=\"btn\">Sign %s out everywhere</button>", p->name);
            ui_form_end(b);
        }
        H(b, "<form method=\"post\" action=\"/a/people.delete\" data-a=\"people.delete\" class=\"inline\" "
             "data-confirm=\"Remove %s from the household?\">",
          p->name);
        ui_hidden_int(b, "id", p->id);
        H(b, "<button class=\"btn danger\">Remove %s</button></form></div>", p->name);
    }
    ui_modal_end(b);
}

static void people_card(Ctx *c, Buf *b) {
    ui_card(b, "People", NULL, NULL);
    buf_puts(b, "<ul class=\"rows\">");
    for (int i = 0; i < account_count(); i++) {
        Person *p = account_at(i);
        buf_puts(b, "<li class=\"row\">");
        ui_avatar(b, p, "md");
        H(b, "<div class=\"row-main\"><b>%s%r</b><small>", p->name, p->id == c->me->id ? " <span class=\"muted\">(you)</span>" : "");
        if (p->username[0]) {
            H(b, "@%s", p->username);
            if (p->role != ROLE_MEMBER) H(b, " · %s", ROLE_NAMES[CLAMP(p->role, 0, 2)]);
        } else {
            buf_puts(b, "no sign-in");
        }
        int screens = presence_screens(p->id);
        if (presence_is_home(p->id, NULL)) buf_puts(b, " · home");
        if (screens) buf_printf(b, " · online on %d screen%s", screens, screens == 1 ? "" : "s");
        else if (p->last_login) {
            buf_puts(b, " · signed in ");
            ui_ago(b, p->last_login, c->now);
        }
        buf_puts(b, "</small></div>");
        if (can_edit(c, p) && ui_can("people.save")) {
            H(b, "<a class=\"icon-btn\" href=\"/people?edit=%d\" aria-label=\"Edit %s\">", p->id, p->name);
            ui_icon(b, "edit");
            buf_puts(b, "</a>");
        }
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ul>");
    ui_card_end(b);
}

/* "Recovery codes - N left - Make new codes" (not for display accounts). */
static void codes_section(Ctx *c, Buf *b) {
    if (c->me->role == ROLE_DISPLAY) return;
    int left = auth_codes_left(c->me->id);
    char fresh[AUTH_CODES][AUTH_CODE_LEN];
    bool waiting = auth_codes_fresh(c->sess, fresh) > 0;
    memset(fresh, 0, sizeof fresh);
    buf_puts(b, "<h3 class=\"mt\" id=\"people-codes-h\">Recovery codes</h3>");
    if (waiting)
        buf_puts(b, "<p class=\"notice\" id=\"people-codes-waiting\">Your new codes are waiting to be saved. "
                    "<a href=\"/people/codes\">Show them</a></p>");
    if (left)
        H(b, "<p class=\"muted\" id=\"people-codes-left\">%d of %d left. Each one gets you back in once if you forget "
             "your password.</p>",
          left, AUTH_CODES);
    else
        buf_puts(b, "<p class=\"muted\" id=\"people-codes-left\">None yet. Make some: they get you back in if you ever "
                    "forget your password.</p>");
    H(b, "<details class=\"people-pw\" id=\"people-codes-new\"><summary>%s</summary>",
      left ? "Make new codes" : "Make recovery codes");
    ui_form(b, "people.codes", "stack");
    H(b,
      "<label class=\"field\"><span>Your password</span><input type=\"password\" name=\"current\" "
      "autocomplete=\"current-password\" required></label>%r<div><button class=\"btn primary\">Make %d new codes"
      "</button></div>",
      left ? "<p class=\"muted small\">Your old codes stop working.</p>" : "", AUTH_CODES);
    ui_form_end(b);
    buf_puts(b, "</details>");
}

/* /people/codes: codes just made, shown on this device until "I've saved them". */
static void codes_page(Ctx *c, Buf *b) {
    c->bare = true;
    ctx_title(c, "Recovery codes");
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    int n = auth_codes_fresh(c->sess, codes);
    buf_puts(b, "<div class=\"center-page\"><div class=\"welcome card people-codes-card\"><div class=\"welcome-logo\">");
    ui_icon(b, "logo");
    buf_puts(b, "</div>");
    if (!n) {
        int left = auth_codes_left(c->me->id);
        H(b,
          "<h1>Recovery codes</h1><p class=\"sub\">Codes are only shown right after they're made. You have %d left; "
          "you can make new ones under Household.</p><a class=\"btn primary big\" href=\"/people\">Back to Household"
          "</a></div></div>",
          left);
        return;
    }
    buf_puts(b, "<h1>Your recovery codes</h1><p class=\"sub\"><b>Save these now</b> - this is the only time Hemma "
                "shows them.</p><ol class=\"people-codes\">");
    for (int i = 0; i < n; i++) H(b, "<li><code>%s</code></li>", codes[i]);
    memset(codes, 0, sizeof codes);
    H(b,
      "</ol><p>Forgot your password? Choose <b>Forgot your password?</b> on the sign-in page and enter your "
      "username (<b>%s</b>) and one of these codes. Each code works once.</p><p class=\"muted small mt\">Keep them "
      "somewhere safe: in a password manager, or printed and put away. Anyone with one of them and your username "
      "can choose a new password for you.</p>",
      c->me->username);
    buf_puts(b, "<div class=\"people-codes-a\"><a class=\"btn\" href=\"/people/codes.txt\" "
                "download=\"hemma-recovery-codes.txt\">Download as a text file</a></div>");
    ui_form(b, "people.codes_saved", "people-codes-done");
    buf_puts(b, "<button class=\"btn primary big\">I've saved them</button>");
    ui_form_end(b);
    buf_puts(b, "</div></div>");
}

/* GET /people/codes.txt: the same codes as a file to keep. */
static bool http(Ctx *c, Conn *conn, HttpReq *r) {
    if (!str_eq(r->path, "/people/codes.txt")) return false;
    char codes[AUTH_CODES][AUTH_CODE_LEN];
    int n = c->me ? auth_codes_fresh(c->sess, codes) : 0;
    if (!n) {
        static const char msg[] = "Recovery codes are only available right after they're made.\n";
        conn_reply(conn, 404, "text/plain; charset=utf-8", msg, sizeof msg - 1, "Cache-Control: no-store\r\n");
        return true;
    }
    char day[11];
    fmt_ymd(c->now, day);
    Buf t = {0};
    buf_printf(&t, "%s - recovery codes for %s (username: %s), made %s\n\n", app_name(), c->me->name,
               c->me->username, day);
    for (int i = 0; i < n; i++) buf_printf(&t, "%2d. %s\n", i + 1, codes[i]);
    memset(codes, 0, sizeof codes);
    buf_puts(&t, "\nForgot your password? On the sign-in page choose \"Forgot your password?\" and enter your\n"
                 "username and one of these codes. Each code works once. Keep this file somewhere safe.\n");
    conn_reply(conn, 200, "text/plain; charset=utf-8", t.p, t.len,
               "Content-Disposition: attachment; filename=\"hemma-recovery-codes.txt\"\r\nCache-Control: no-store\r\n");
    buf_free(&t);
    return true;
}

static void account_card(Ctx *c, Buf *b) {
    ui_card(b, "Your account", NULL, NULL);
    buf_puts(b, "<div class=\"device-me\">");
    ui_avatar(b, c->me, "lg");
    H(b, "<div><b>%s</b><p class=\"muted\">", c->me->name);
    if (c->me->username[0]) H(b, "Signed in as @%s", c->me->username);
    buf_puts(b, "</p></div></div>");
    if (c->me->pass[0]) {
        bool temp = auth_temporary(c->me->id);
        if (temp)
            buf_puts(b, "<p class=\"notice\" id=\"people-temp\" role=\"alert\">You signed in with a temporary password. "
                        "Choose your own now.</p>");
        H(b, "<details class=\"people-pw\" id=\"people-pw\"%r><summary>Change password</summary>", temp ? " open" : "");
        ui_form(b, "people.password", "stack");
        H(b,
          "<label class=\"field\"><span>%s</span><input type=\"password\" name=\"current\" "
          "autocomplete=\"current-password\" required></label><div class=\"field-row\"><label class=\"field\">"
          "<span>New password</span><input type=\"password\" name=\"new\" autocomplete=\"new-password\" "
          "required minlength=\"8\"></label><label class=\"field\"><span>Again</span><input type=\"password\" "
          "name=\"again\" autocomplete=\"new-password\" required minlength=\"8\"></label></div>"
          "<div><button class=\"btn primary\">Change password</button></div>",
          temp ? "Temporary password" : "Current password");
        ui_form_end(b);
        buf_puts(b, "</details>");
        codes_section(c, b);
    }
    /* signed-in devices */
    buf_puts(b, "<h3 class=\"mt\">Signed in on</h3><ul class=\"rows people-devices\">");
    int others = 0;
    for (int i = 0; i < tbl_count(&T_session); i++) {
        Session *s = tbl_at(&T_session, i);
        if (s->person != c->me->id) continue;
        bool here = c->sess && str_eq(s->token, c->sess->token);
        if (!here) others++;
        H(b, "<li class=\"row\"><div class=\"row-main\"><b>%s</b><small>%r", s->agent[0] ? s->agent : "A browser",
          here ? "this device · " : "");
        buf_puts(b, "active ");
        ui_ago(b, MAX(s->seen, s->created), c->now);
        buf_puts(b, "</small></div></li>");
    }
    buf_puts(b, "</ul><div class=\"people-actions\">");
    if (others) ui_act(b, "people.signout_others", "btn", "Sign out other devices", NULL, NULL, NULL);
    /* a real POST (not over the WebSocket): the cookie has to be cleared by an HTTP response */
    buf_puts(b, "<form method=\"post\" action=\"/logout\" class=\"inline\"><button class=\"btn\">Sign out</button></form>"
                "</div>");
    ui_card_end(b);
}

static void home_card(Ctx *c, Buf *b) {
    ui_card(b, "Home", NULL, NULL);
    if (is_admin(c)) {
        ui_form(b, "household.save", "row-form");
        H(b, "<input name=\"name\" maxlength=\"40\" value=\"%s\" aria-label=\"Home name\"><button class=\"btn\">Rename"
             "</button>",
          app_name());
        ui_form_end(b);
    } else {
        H(b, "<p><b>%s</b></p>", app_name());
    }
    H(b, "<h3 class=\"mt\">Wall display</h3><p class=\"muted\">An always-on overview for a tablet in the hallway or "
         "kitchen. Tip: give the tablet its own <i>Display</i> sign-in so it can only look, and pick its cards under "
         "<a href=\"/screens\">Screens</a>.</p>"
         "<a class=\"btn\" href=\"/wall\" data-native>Open wall display</a>");
    ui_card_end(b);
}

static void page(Ctx *c, Buf *b) {
    if (str_eq(c->path, "/people/codes")) {
        codes_page(c, b);
        return;
    }
    ctx_title(c, "Household");
    ui_head(b, "Household", app_name());
    if (is_admin(c)) {
        buf_puts(b, "<a class=\"btn\" href=\"/people?add=1\">");
        ui_icon(b, "plus");
        buf_puts(b, "<span>Add someone</span></a>");
    }
    ui_head_end(b);
    if (str_eq(ctx_q(c, "recovered"), "1")) { /* back in with a recovery code (core/auth.c) */
        int left = auth_codes_left(c->me->id);
        H(b, "<p class=\"notice\" id=\"people-recovered\">Your new password is set and you're signed in. You have %d "
             "recovery code%r left%r</p>",
          left, left == 1 ? "" : "s", left <= 2 ? " - make new ones under Recovery codes." : ".");
    }
    buf_puts(b, "<div class=\"grid2\"><div>");
    people_card(c, b);
    home_card(c, b);
    buf_puts(b, "</div><div>");
    account_card(c, b);
    buf_puts(b, "</div></div>");

    int edit = (int)ctx_qint(c, "edit", 0);
    if (str_eq(ctx_q(c, "add"), "1") && is_admin(c)) person_modal(c, b, NULL);
    else if (edit && person_get(edit) && can_edit(c, person_get(edit))) person_modal(c, b, person_get(edit));
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
    ctx_fail(c, "Keep at least one admin who can sign in");
    return false;
}

static void act_save(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    int me = c->me->id; /* c->me dangles once tbl_put grows the person table */
    Person p = {0};
    if (id) {
        Person *old = person_get(id);
        if (!old) {
            ctx_fail(c, "Unknown person");
            return;
        }
        if (!can_edit(c, old)) {
            ctx_fail(c, "Only an admin can change other people");
            return;
        }
        p = *old;
    } else if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can add people");
        return;
    }
    char name[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    if (!name[0]) {
        ctx_fail(c, "A name, please");
        return;
    }
    if (name_taken(name, p.id)) {
        ctx_fail(c, "There's already a %s here", name);
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
            ctx_fail(c, "Change your own password under Your account - it asks for the current one first");
            return;
        }
        if (set_user && user[0]) {
            if (!auth_username_ok(user)) {
                ctx_fail(c, "Usernames are 2–32 of a–z, 0–9 and . _ -");
                return;
            }
            Person *other = person_by_username(user);
            if (other && other->id != p.id) {
                ctx_fail(c, "@%s is taken", user);
                return;
            }
            if (!p.pass[0] && !*pw) {
                ctx_fail(c, "Give %s a password to sign in with", p.name);
                return;
            }
        }
        if (*pw) {
            const char *why = auth_password_problem(pw);
            if (why) {
                ctx_fail(c, "%s", why);
                return;
            }
            if (!user[0]) {
                ctx_fail(c, "A password needs a username too");
                return;
            }
            if (!auth_hash(pw, p.pass, sizeof p.pass)) {
                ctx_fail(c, "Passwords can't be set: HEMMA_AUTH_SALT isn't configured");
                return;
            }
        }
        if (set_user) {
            str_copy(p.username, sizeof p.username, user);
            if (!user[0]) p.pass[0] = '\0'; /* no username, no sign-in */
        }
        if (set_role && !(id && id == me)) p.role = (int)CLAMP(fields_int(f, "role", ROLE_MEMBER), 0, 2);
        if (!keeps_an_admin(c, &p, 0)) return;
        if (*pw) auth_forget_failures(p.username); /* a fresh password: earlier lockouts are moot */
    }
    if (!id) p.created = c->now;
    bool signin_changed = (fields_get(f, "username") && !p.username[0]) || fields_str(f, "password")[0];
    tbl_put(&T_person, &p); /* from here on only locals: c->me may dangle */
    if (!p.username[0] || p.role == ROLE_DISPLAY) auth_codes_drop(p.id); /* no sign-in (of their own): no codes */
    if (id && signin_changed && p.id != me)
        sessions_drop(p.id, NULL); /* new password or sign-in removed: their old sessions end */
    if (!id) activity_add(me, "👋", "/people", "%s joined the household", p.name);
    ctx_ok(c, id ? "Saved" : "Added %s", p.name);
    ctx_redirect(c, "/people");
}

static void act_delete(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can remove people");
        return;
    }
    Person *p = person_get((int)fields_int(f, "id", 0));
    if (!p || p->id == c->me->id) {
        ctx_fail(c, "Can't remove that person");
        return;
    }
    char name[48];
    str_copy(name, sizeof name, p->name);
    int id = p->id;
    if (!keeps_an_admin(c, NULL, id)) return;
    sessions_drop(id, NULL);
    auth_codes_drop(id);
    tbl_del(&T_person, id);
    ctx_ok(c, "Removed %s", name);
    ctx_redirect(c, "/people");
}

static void act_password(Ctx *c, Fields *f) {
    const char *cur = fields_str(f, "current"), *nw = fields_str(f, "new"), *again = fields_str(f, "again");
    int wait = 0;
    LoginResult r = auth_recheck(c->me, cur, c->ip, c->now, &wait); /* throttled: no guessing from a borrowed session */
    if (r == LOGIN_THROTTLED) {
        int min = MAX(1, (wait + 59) / 60);
        ctx_fail(c, "Too many tries. Wait %d minute%s and try again.", min, min == 1 ? "" : "s");
        return;
    }
    if (r != LOGIN_OK) {
        ctx_fail(c, "Your current password isn't right");
        return;
    }
    const char *why = auth_password_problem(nw);
    if (why) {
        ctx_fail(c, "%s", why);
        return;
    }
    if (!str_eq(nw, again)) {
        ctx_fail(c, "The new passwords don't match");
        return;
    }
    int n = auth_set_password(c->me->id, nw, c->sess); /* c->me may dangle from here on */
    if (n < 0) {
        ctx_fail(c, "Passwords can't be set: HEMMA_AUTH_SALT isn't configured");
        return;
    }
    if (n) ctx_ok(c, "Password changed — signed out %d other device%s", n, n == 1 ? "" : "s");
    else ctx_ok(c, "Password changed");
}

/* New recovery codes (the old ones stop working): asks for the password, like changing it,
   so a borrowed, signed-in phone can't quietly mint a way into the account. */
static void act_codes(Ctx *c, Fields *f) {
    int wait = 0;
    LoginResult r = auth_recheck(c->me, fields_str(f, "current"), c->ip, c->now, &wait);
    if (r == LOGIN_THROTTLED) {
        int min = MAX(1, (wait + 59) / 60);
        ctx_fail(c, "Too many tries. Wait %d minute%s and try again.", min, min == 1 ? "" : "s");
        return;
    }
    if (r != LOGIN_OK) {
        ctx_fail(c, "Your password isn't right");
        return;
    }
    if (!auth_codes_new(c->me->id, c->sess)) {
        ctx_fail(c, "This account can't have recovery codes");
        return;
    }
    ctx_redirect(c, "/people/codes");
}

static void act_codes_saved(Ctx *c, Fields *f) {
    auth_codes_seen(c->sess);
    ctx_ok(c, "Recovery codes saved - keep them safe");
    ctx_redirect(c, "/people");
}

static void act_signout_others(Ctx *c, Fields *f) {
    int n = sessions_drop(c->me->id, c->sess);
    ctx_ok(c, "Signed out %d other device%s", n, n == 1 ? "" : "s");
}

static void act_signout(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can do that");
        return;
    }
    Person *p = person_get((int)fields_int(f, "id", 0));
    if (!p) {
        ctx_fail(c, "Unknown person");
        return;
    }
    char name[48];
    str_copy(name, sizeof name, p->name);
    int n = sessions_drop(p->id, p->id == c->me->id ? c->sess : NULL);
    ctx_ok(c, "Signed %s out of %d device%s", name, n, n == 1 ? "" : "s");
}

static void act_household(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "Only an admin can rename the home");
        return;
    }
    char name[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    setting_set("household.name", name[0] ? name : "Hemma");
    ctx_ok(c, "Saved");
}

static const Action actions[] = {
    {"people.save", act_save},
    {"people.delete", act_delete},
    {"people.password", act_password},
    {"people.codes", act_codes},
    {"people.codes_saved", act_codes_saved},
    {"people.signout_others", act_signout_others},
    {"people.signout", act_signout},
    {"household.save", act_household},
    {0},
};

static const Module people = {
    .id = "people",
    .title = "Household",
    .icon = "users",
    .nav = 90,
    .page = page,
    .actions = actions,
    .http = http,
};
MODULE(people)
