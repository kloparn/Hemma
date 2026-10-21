/* modules/book/book_view.c — the booking pages and widgets. */
#include "../../core/ui.h"
#include "book.h"

const char *book_presets_name(int i);
const char *book_presets_emoji(int i);
int book_presets_count(void);

/* minutes after the local midnight of `day0`, clamped to [0, 1440] — wall clock, so DST days line up */
static int wall_min(int64_t t, int64_t day0) {
    if (t <= day0) return 0;
    if (day_start(t) > day0) return 1440;
    DateTime d = dt_local(t);
    return d.hour * 60 + d.min;
}

static void res_title(Buf *b, const Resource *r) {
    if (r->emoji[0]) H(b, "<span class=\"book-res-e\">%s</span> ", r->emoji);
    html_esc(b, r->name);
}

/* "Free now" / "Busy until 16:00 · Eva" / "Next: Fri 14:00 · Adam" */
static void res_status(Ctx *c, Buf *b, const Resource *r) {
    Booking *cur = book_current(r->id, c->now);
    if (cur) {
        Person *p = person_get(cur->who);
        buf_puts(b, "<span class=\"book-st book-busy\">");
        if (day_start(cur->end - 1) == day_start(c->now) && !res_daily(r)) {
            DateTime e = dt_local(cur->end); /* (a clock time: "hasta las 16:00") */
            H(b, T("Busy until %02d:%02d"), e.hour, e.min);
        } else {
            Buf day = {0};
            book_day(&day, res_daily(r) ? add_days(cur->end, -1) : cur->end, c->now);
            H(b, T("Busy until %s"), buf_cstr(&day));
            buf_free(&day);
        }
        H(b, "</span> · %s", p ? p->name : T("someone"));
        return;
    }
    H(b, "<span class=\"book-st book-free\">%s</span>", T("Free now"));
    Booking *nx = book_next(r->id, c->now);
    if (nx && nx->start < add_days(c->now, 14)) {
        Person *p = person_get(nx->who);
        Buf when = {0};
        book_day(&when, nx->start, c->now);
        if (!res_daily(r)) {
            buf_putc(&when, ' ');
            ui_hm(&when, nx->start);
        }
        buf_puts(b, " · ");
        H(b, T("next %s"), buf_cstr(&when));
        buf_free(&when);
        if (p) H(b, " (%s)", p->name);
    }
}

/* ---------------------------------------------------------------- quick add */

static void quick_bar(Ctx *c, Buf *b, int res) {
    ui_form(b, "book.quick", "quick");
    if (res) ui_hidden_int(b, "res", res);
    const char *q = ctx_live(c, "book.q");
    buf_puts(b, "<div class=\"quick-row\">");
    H(b,
      "<input name=\"q\" autocomplete=\"off\" data-live=\"book.preview\" data-key=\"/\" value=\"%s\" "
      "placeholder=\"%s\" aria-label=\"%s\"><button class=\"btn primary\">%s</button></div>",
      q, T("Book something… e.g. “car fri 14-17 ikea trip”"), T("Quick booking"), T("Book"));
    if (str_blank(q) && (is_child(c) || is_guest(c))) { /* (for themselves: issue #44) */
        H(b, "<p class=\"quick-hint\">%r</p>",
          T("Type what and when: <i>sauna tomorrow 18-19</i>, <i>bastu ikväll 19</i>. Press <span class=\"kbd\">/</span> "
            "to start typing."));
    } else if (str_blank(q)) {
        H(b, "<p class=\"quick-hint\">%r</p>",
          T("Type what, when and (optionally) for whom: <i>laundry tomorrow 18-21</i>, <i>guest room fri-sun @eva</i>, "
            "<i>bastu ikväll 19</i>. Press <span class=\"kbd\">/</span> to start typing."));
    } else {
        Quick k;
        book_quick_parse(q, res, c->now, &k);
        Resource *r = res_get(k.res);
        char no[120] = ""; /* a child's, a guest's: what they may book, for themselves (issue #44) */
        if (r && !book_open_to(c, r))
            snprintf(no, sizeof no, is_child(c) ? T("Ask a grown-up to book the %s") : T("The %s isn't open to guests"),
                     r->name);
        else if ((is_child(c) || is_guest(c)) && k.who && k.who != c->me->id)
            snprintf(no, sizeof no, "%s", T("You can book for yourself"));
        bool ok = !k.problem[0] && r && !no[0];
        H(b, "<div class=\"preview %s\" data-say>", ok ? "ok" : "bad"); /* read out once typing pauses (#69) */
        if (r && k.has_when && k.start) {
            res_title(b, r);
            buf_puts(b, " · <b>");
            book_span_text(r, k.start, k.end, c->now, b);
            buf_puts(b, "</b>");
            Person *who = person_get(k.who);
            if (who || c->me) {
                buf_puts(b, " · ");
                ui_person(b, who ? who : c->me);
            }
            if (k.note[0]) H(b, " · <i>%s</i>", k.note);
            if (k.repeat) { /* (issue #47) */
                BookSeries s = {.start = k.start, .repeat = k.repeat};
                buf_puts(b, " · ↻ ");
                book_series_label(b, &s);
            }
        }
        if (r && k.has_when && k.start) buf_puts(b, "<span class=\"sr-only\">. </span>"); /* (read as a pause: #69) */
        if (k.problem[0]) H(b, "<span class=\"book-why\">%s</span>", k.problem);
        else if (no[0]) H(b, "<span class=\"book-why\">%s</span>", no);
        else if (k.repeat) {
            buf_puts(b, "<span class=\"book-why ok\">");
            H(b, T("The first one is free — press Enter to book it and the next %d weeks"), SERIES_AHEAD_DAYS / 7);
            buf_puts(b, "</span>");
        } else H(b, "<span class=\"book-why ok\">%s</span>", T("Free — press Enter to book"));
        buf_puts(b, "</div>");
    }
    ui_form_end(b);
    /* A free alternative after a clash: one tap books it (own form, after the quick form). */
    if (!str_blank(q)) {
        Quick k;
        book_quick_parse(q, res, c->now, &k);
        Resource *r = res_get(k.res);
        if (k.has_alt && r && book_open_to(c, r)) {
            ui_form(b, "book.create", "book-alt");
            ui_hidden_int(b, "res", r->id);
            ui_hidden_int(b, "start", k.alt_start);
            ui_hidden_int(b, "end", k.alt_end);
            ui_hidden_int(b, "who", k.who);
            ui_hidden(b, "note", k.note);
            buf_puts(b, "<button class=\"btn\">");
            ui_icon(b, "sparkle");
            Buf span = {0};
            book_span_text(r, k.alt_start, k.alt_end, c->now, &span);
            buf_puts(b, "<span>");
            H(b, T("Book %s instead"), buf_cstr(&span));
            buf_puts(b, "</span></button>");
            buf_free(&span);
            ui_form_end(b);
        }
    }
}

/* What others did to your bookings (issue #43), newest first, until "Got it". */
static void news(Ctx *c, Buf *b) {
    if (!c->me) return;
    int n = 0;
    for (int i = 0; i < tbl_count(&T_book_news); i++) n += ((BookNews *)tbl_at(&T_book_news, i))->person == c->me->id;
    if (!n) return;
    buf_puts(b, "<div class=\"notice book-news\" id=\"book-news\" role=\"status\"><ul>");
    Buf text = {0};
    for (int i = tbl_count(&T_book_news) - 1; i >= 0; i--) {
        BookNews *x = tbl_at(&T_book_news, i);
        if (x->person != c->me->id) continue;
        buf_reset(&text);
        if (x->msg[0]) i18n_unpack(&text, x->msg); /* in the reader's language */
        else buf_puts(&text, x->text);            /* (a line from before: as it was written) */
        H(b, "<li id=\"book-news-%d\"><a href=\"%s\">%s</a> <small class=\"muted\">", x->id, x->link, buf_cstr(&text));
        ui_ago(b, x->ts, c->now);
        buf_puts(b, "</small></li>");
    }
    buf_free(&text);
    buf_puts(b, "</ul>");
    Buf got = {0}; /* (ui_act's label is HTML) */
    H(&got, "%s", T("Got it"));
    ui_act(b, "book.news_seen", "btn sm", buf_cstr(&got), NULL, NULL, NULL);
    buf_free(&got);
    buf_puts(b, "</div>");
}

/* ------------------------------------------------------------------ modals */

static void booking_form(Ctx *c, Buf *b, Resource *r, Booking *edit, int64_t at) {
    ui_form(b, "book.create", "stack");
    ui_hidden_int(b, "res", r->id);
    if (edit) ui_hidden_int(b, "id", edit->id);
    int64_t s = edit ? edit->start : at;
    int64_t e = edit ? edit->end
                     : (res_daily(r) ? add_days(s, 1) : s + (int64_t)(r->default_min > 0 ? r->default_min : 60) * 60);
    char ymd[11], hm1[6], hm2[6], ymd2[11];
    fmt_ymd(s, ymd);
    fmt_hm(s, hm1);
    fmt_hm(e, hm2);
    fmt_ymd(add_days(e, -1), ymd2);
    buf_puts(b, "<div class=\"field-row\">");
    H(b, "<label class=\"field\"><span>%s</span><input type=\"date\" name=\"date\" value=\"%s\" required></label>",
      res_daily(r) ? T("First day") : T("Day"), ymd);
    if (res_daily(r)) {
        H(b, "<label class=\"field\"><span>%s</span><input type=\"date\" name=\"date_to\" value=\"%s\"></label>",
          T("Last day"), ymd2);
    } else {
        int step = r->slot * 60;
        H(b,
          "<label class=\"field\"><span>%s</span><input type=\"time\" name=\"from\" value=\"%s\" step=\"%d\" "
          "required></label><label class=\"field\"><span>%s</span><input type=\"time\" name=\"to\" value=\"%s\" "
          "step=\"%d\" required></label>",
          T("From"), hm1, step, T("Until"), hm2, step);
    }
    if (is_child(c) || is_guest(c)) { /* they book for themselves (issue #44) */
        buf_puts(b, "</div>");
        ui_hidden_int(b, "who", c->me->id);
    } else {
        H(b, "</div><label class=\"field\"><span>%s</span>", T("For"));
        ui_person_select(b, "who", edit ? edit->who : (c->me ? c->me->id : 0), NULL);
        buf_puts(b, "</label>");
    }
    H(b,
      "<label class=\"field\"><span>%s</span><input name=\"note\" maxlength=\"100\" value=\"%s\" "
      "placeholder=\"%s\"></label>",
      T("Note"), edit ? edit->note : "", T("Optional — where, why…"));
    if (!edit) { /* (issue #47; one of a series is moved on its own) */
        H(b, "<div class=\"field-row\"><label class=\"field\"><span>%s</span><select name=\"rep\">", T("Repeat"));
        static const char *const reps[] = {N_("Never"), N_("Every day"), N_("Every week"), N_("Every 2 weeks"),
                                           N_("Every month")};
        for (int i = 0; i < countof(reps); i++) H(b, "<option value=\"%d\">%s</option>", i, T(reps[i]));
        H(b, "</select></label><label class=\"field\"><span>%s</span><input type=\"date\" name=\"rep_until\"></label></div>",
          T("Repeat until"));
    }
    if (r->rules[0]) H(b, "<p class=\"book-rules\">%s</p>", r->rules);
    H(b, "<div class=\"modal-f\"><button class=\"btn primary\">%s</button></div>", edit ? T("Save changes") : T("Book it"));
    ui_form_end(b);
}

static void booking_modal(Ctx *c, Buf *b, Booking *bk, const char *close) {
    Resource *r = res_get(bk->res);
    if (!r) return;
    Person *who = person_get(bk->who), *by = person_get(bk->by);
    ui_modal(b, r->name, close);
    bool may = book_may_change(c, r, bk);
    if (may && str_eq(ctx_q(c, "edit"), "1")) {
        booking_form(c, b, r, bk, 0);
        ui_modal_end(b);
        return;
    }
    buf_puts(b, "<div class=\"book-bk-detail\">");
    ui_avatar_deco(b, who, "lg");
    H(b, "<div><b class=\"book-bk-who\">%s</b><div class=\"book-bk-when\">", who ? who->name : T("Someone"));
    Buf when = {0};
    book_span_text(r, bk->start, bk->end, c->now, &when);
    if (when.len && when.p[0] >= 'a' && when.p[0] <= 'z') when.p[0] = (char)(when.p[0] - 'a' + 'A'); /* (a line) */
    buf_puts(b, buf_cstr(&when));
    buf_free(&when);
    buf_puts(b, "</div>");
    if (bk->note[0]) H(b, "<p class=\"book-bk-note\">%s</p>", bk->note);
    const BookSeries *series = book_series_of(bk);
    if (series) {
        Buf lab = {0};
        book_series_label(&lab, series);
        buf_puts(b, "<p class=\"book-bk-rep\">↻ ");
        H(b, T("Repeats %s"), buf_cstr(&lab));
        buf_puts(b, "</p>");
        buf_free(&lab);
    }
    Buf ago = {0};
    ui_ago(&ago, bk->created, c->now);
    buf_puts(b, "<small class=\"muted\">");
    if (by && by != who) H(b, T("Booked by %s %s"), by->name, buf_cstr(&ago));
    else H(b, T("Booked %s"), buf_cstr(&ago));
    buf_puts(b, "</small></div></div>");
    buf_free(&ago);
    /* the thing's rule: only theirs to change (issue #43). A display account keeps the forms below,
       so the core cuts the dialog whole (issue #96). */
    if (!may && ui_can("book.cancel")) {
        const char *whose = who ? who->name : T("the person it's for");
        buf_puts(b, "<p class=\"muted small\">");
        if (by && by != who) H(b, T("Only %s, %s or an admin can cancel or move it."), whose, by->name);
        else H(b, T("Only %s or an admin can cancel or move it."), whose);
        buf_puts(b, "</p>");
        ui_modal_end(b);
        return;
    }
    buf_puts(b, "<div class=\"modal-f spread\">");
    ui_form(b, "book.cancel", "inline");
    ui_hidden_int(b, "id", bk->id);
    H(b, "<button class=\"btn danger\">%s</button>", series ? T("Cancel this one") : T("Cancel booking"));
    ui_form_end(b);
    if (series && !series->stopped) { /* the rest of a repeating booking: asked first (more than one) */
        ui_form_confirm(b, "book.cancel", "inline", T("Cancel this one and every later one, and stop repeating?"));
        ui_hidden_int(b, "id", bk->id);
        ui_hidden(b, "later", "1");
        H(b, "<button class=\"btn danger\">%s</button>", T("Stop repeating"));
        ui_form_end(b);
    }
    Buf u = {0};
    ctx_url(c, &u, "edit", "1", NULL);
    H(b, "<a class=\"btn\" href=\"%s\">%s</a></div>", buf_cstr(&u), T("Change"));
    buf_free(&u);
    ui_modal_end(b);
}

static void resource_modal(Ctx *c, Buf *b, Resource *r, const char *close) {
    ui_modal(b, r ? T("Edit") : T("Something new to book"), close);
    if (!r) {
        H(b, "<p class=\"muted\">%s</p><div class=\"chips book-presets\">", T("Quick picks:"));
        for (int i = 0; i < book_presets_count(); i++) {
            ui_form(b, "book.preset", "inline");
            ui_hidden(b, "name", book_presets_name(i)); /* (English: which one) */
            H(b, "<button class=\"chip\">%s %s</button>", book_presets_emoji(i), T(book_presets_name(i)));
            ui_form_end(b);
        }
        H(b, "</div><h3 class=\"mt\">%s</h3>", T("Or describe it"));
    }
    ui_form(b, "book.res_save", "stack");
    if (r) ui_hidden_int(b, "id", r->id);
    H(b,
      "<div class=\"field-row\"><label class=\"field grow\"><span>%s</span><input name=\"name\" required "
      "maxlength=\"36\" value=\"%s\" placeholder=\"%s\"%r></label>"
      "<label class=\"field narrow\"><span>%s</span><input name=\"emoji\" maxlength=\"8\" value=\"%s\" "
      "placeholder=\"🚗\"></label></div>",
      T("Name"), r ? r->name : "", T("Car, laundry room, sauna…"), r ? "" : " autofocus", T("Emoji"), r ? r->emoji : "");
    /* what quick booking also knows it by (issue #55: "bastu ikväll 19" and a thing called "Sauna") */
    const char *starter = r ? book_starter_aliases(r->name) : "";
    H(b,
      "<label class=\"field\"><span>%s</span><input name=\"aliases\" maxlength=\"110\" "
      "value=\"%s\" placeholder=\"%s\" aria-describedby=\"book-aliases-h\"><small class=\"muted\" "
      "id=\"book-aliases-h\">%s",
      T("Other names (optional)"), r ? r->aliases : "", T("bastu, the cabin…"),
      T("Quick booking knows it by these too, comma-separated."));
    if (starter[0]) {
        buf_putc(b, ' ');
        H(b, T("It already knows “%s”."), starter);
    }
    buf_puts(b, "</small></label>");
    bool daily = r && res_daily(r);
    bool always = !r || (r->open_min <= 0 && r->close_min >= 1440);
    char open[6], close_hm[6];
    snprintf(open, sizeof open, "%02d:%02d", (r ? r->open_min : 420) / 60, (r ? r->open_min : 420) % 60);
    /* (open until midnight: 00:00 - it used to say 22:00, and saving the dialog shortened the hours) */
    int cm = !r ? 22 * 60 : r->close_min < 1440 ? r->close_min : 0;
    snprintf(close_hm, sizeof close_hm, "%02d:%02d", cm / 60, cm % 60);
    H(b,
      "<div class=\"field\"><span>%s</span><div class=\"chips\">"
      "<label class=\"chip-r\"><input type=\"radio\" name=\"kind\" value=\"hourly\"%r><span>%s</span></label>"
      "<label class=\"chip-r\"><input type=\"radio\" name=\"kind\" value=\"daily\"%r><span>%s</span></label>"
      "</div></div>",
      T("Booked by"), daily ? "" : " checked", T("The hour"), daily ? " checked" : "", T("Whole days"));
    H(b, "<div class=\"field-row\"><label class=\"field\"><span>%s</span><select name=\"slot\">", T("Steps"));
    static const int slots[] = {15, 30, 60};
    for (int i = 0; i < 3; i++)
        H(b, "<option value=\"%d\"%r>%d min</option>", slots[i], (r ? r->slot : 30) == slots[i] ? " selected" : "",
          slots[i]);
    H(b,
      "</select></label><label class=\"field\"><span>%s</span><input type=\"number\" name=\"dflt\" "
      "min=\"15\" step=\"15\" value=\"%d\"></label><label class=\"field\"><span>%s</span>"
      "<input type=\"number\" name=\"max\" min=\"0\" step=\"15\" value=\"%d\"></label></div>",
      T("Usual length (min)"), r && !res_daily(r) ? r->default_min : 120, T("Max (min, 0 = none)"), r ? r->max_min : 0);
    H(b,
      "<div class=\"field\"><span>%s</span><label class=\"check\"><input type=\"checkbox\" name=\"always\" "
      "value=\"1\"%r> %s</label><div class=\"field-row\"><label class=\"field\"><span>%s</span>"
      "<input type=\"time\" name=\"open\" value=\"%s\"></label><label class=\"field\"><span>%s</span>"
      "<input type=\"time\" name=\"close\" value=\"%s\"></label></div></div>",
      T("Hours"), always ? " checked" : "", T("Around the clock"), T("From"), open, T("Until"), close_hm);
    H(b,
      "<label class=\"field\"><span>%s</span><input name=\"rules\" maxlength=\"150\" "
      "value=\"%s\" placeholder=\"%s\"></label>",
      T("House rules (optional)"), r ? r->rules : "", T("Fill up the tank, clean the filter…"));
    if (is_admin(c)) { /* issue #43: in a household, usually anyone may - an admin can make it stricter */
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"own_only\" value=\"1\"%r> %s</label>",
          r && r->own_only ? " checked" : "",
          T("Only the person a booking is for, whoever made it, or an admin can cancel or move it"));
        H(b, /* issue #44: who else may book it */
          "<label class=\"check\"><input type=\"checkbox\" name=\"kids\" value=\"1\"%r> %s</label>"
          "<label class=\"check\"><input type=\"checkbox\" name=\"guests\" value=\"1\"%r> %s</label>",
          r && r->kids ? " checked" : "", T("Children may book it"), r && r->guests ? " checked" : "",
          T("Guests may book it"));
    } else if (r && r->own_only)
        H(b, "<p class=\"muted small\">%r</p>", /* (our own words: raw, the apostrophe as it is) */
          T("Only the person a booking is for, whoever made it, or an admin can cancel or move it (an admin's choice)."));
    H(b, "<div class=\"field\"><span>%s</span>", T("Colour"));
    ui_hue_picker(b, "hue", r ? r->hue : 200);
    H(b, "</div><div class=\"modal-f\"><button class=\"btn primary\">%s</button></div>", T("Save"));
    ui_form_end(b);
    if (r) {
        ui_form(b, "book.res_archive", "danger-zone");
        ui_hidden_int(b, "id", r->id);
        H(b, "<button class=\"btn danger\">%s</button>", r->archived ? T("Make bookable again") : T("Hide from bookings"));
        ui_form_end(b);
    }
    ui_modal_end(b);
}

/* ---------------------------------------------------------------- week grid */

static void week_nav(Ctx *c, Buf *b, int64_t ws, bool daily) {
    char prev[11], next[11], today[11];
    fmt_ymd(add_days(ws, daily ? -28 : -7), prev);
    fmt_ymd(add_days(ws, daily ? 28 : 7), next);
    fmt_ymd(c->now, today);
    Buf up = {0}, un = {0}, ut = {0};
    ctx_url(c, &up, "w", prev, "new", NULL, "b", NULL, NULL);
    ctx_url(c, &un, "w", next, "new", NULL, "b", NULL, NULL);
    ctx_url(c, &ut, "w", today, "new", NULL, "b", NULL, NULL);
    DateTime a = dt_local(ws), z = dt_local(add_days(ws, daily ? 34 : 6));
    H(b, "<div class=\"book-wk-nav\"><a class=\"icon-btn\" href=\"%s\" aria-label=\"%s\">", buf_cstr(&up), T("Earlier"));
    ui_icon(b, "left");
    H(b, "</a><a class=\"icon-btn\" href=\"%s\" aria-label=\"%s\">", buf_cstr(&un), T("Later"));
    ui_icon(b, "right");
    buf_puts(b, "</a><b class=\"book-wk-range\">");
    if (!daily) {
        buf_printf(b, T("Week %d"), iso_week(ws, NULL));
        buf_puts(b, " · ");
    }
    if (a.mon == z.mon) buf_printf(b, "%d–%d %s", a.day, z.day, month_name(a.mon, false));
    else buf_printf(b, "%d %s – %d %s", a.day, month_name(a.mon, true), z.day, month_name(z.mon, true));
    bool here = c->now >= ws && c->now < add_days(ws, daily ? 35 : 7);
    buf_puts(b, "</b></div>");
    if (!here) H(b, "<a class=\"btn sm\" href=\"%s\">%s</a>", buf_cstr(&ut), T("Today"));
    buf_free(&up);
    buf_free(&un);
    buf_free(&ut);
}

static void hourly_grid(Ctx *c, Buf *b, Resource *r, int64_t ws) {
    int slot = MAX(r->slot, 15);
    bool can = ui_can("book.create");
    int vis_from = r->open_min, vis_to = r->close_min;
    if (vis_from <= 0 && vis_to >= 1440) { /* around the clock: show the waking day, grow to fit bookings */
        vis_from = 6 * 60;
        vis_to = 24 * 60;
        for (int i = 0; i < tbl_count(&T_booking); i++) {
            Booking *bk = tbl_at(&T_booking, i);
            if (bk->res != r->id || bk->end <= ws || bk->start >= add_days(ws, 7)) continue;
            for (int d = 0; d < 7; d++) {
                int64_t d0 = add_days(ws, d);
                if (bk->end <= d0 || bk->start >= add_days(d0, 1)) continue;
                int a = wall_min(bk->start, d0);
                if (a < vis_from) vis_from = a / 60 * 60;
            }
        }
    }
    vis_from = vis_from / slot * slot;
    int rows = (vis_to - vis_from + slot - 1) / slot;
    int row_h = slot <= 15 ? 14 : slot <= 30 ? 22 : 40;
    bool this_week = day_start(c->now) >= ws && day_start(c->now) < add_days(ws, 7);
    /* phones show ~3 days at a time: open the scroller at today (wire.js data-start) */
    H(b, "<div class=\"book-wk-scroll\"%r><div class=\"book-wk\" style=\"--rows:%d;--row-h:%dpx;--per-hour:%d\">",
      this_week ? " data-start=\"book-now-day\"" : "", rows, row_h, 60 / slot);
    /* header */
    buf_puts(b, "<div class=\"book-wk-head\"><span></span>");
    for (int d = 0; d < 7; d++) {
        int64_t d0 = add_days(ws, d);
        DateTime dt = dt_local(d0);
        bool today = day_start(c->now) == d0;
        H(b, "<span class=\"book-wk-dh%r\"%r>%s <b>%d</b></span>", today ? " book-today" : "",
          today ? " id=\"book-now-day\"" : "", weekday_name(dt.wday, true), dt.day);
    }
    buf_puts(b, "</div><div class=\"book-wk-body\"><div class=\"book-wk-times\">");
    for (int m = (vis_from + 59) / 60 * 60; m < vis_to; m += 60) { /* full hours only: 07:30 opens at row 0 */
        int r0 = (m - vis_from) / slot;
        H(b, "<span style=\"--top:%d\">%02d:00</span>", r0, m / 60);
    }
    buf_puts(b, "</div>");
    char wq[11];
    fmt_ymd(ws, wq);
    for (int d = 0; d < 7; d++) {
        int64_t d0 = add_days(ws, d);
        DateTime dt = dt_local(d0);
        bool today = day_start(c->now) == d0;
        H(b, "<div class=\"book-wk-col%r\">", today ? " book-today" : "");
        for (int i = 0; i < rows; i++) {
            int m = vis_from + i * slot;
            int64_t s = dt_make(dt.year, dt.mon, dt.day, 0, m);
            int64_t e = dt_make(dt.year, dt.mon, dt.day, 0, m + slot);
            bool hour = m % 60 == 0;
            if (e <= c->now || !can) { /* (a display account only looks: no slots to tap - issue #96) */
                H(b, "<span class=\"book-wk-s%r%r\"></span>", e <= c->now ? " book-past" : "", hour ? " book-hr" : "");
            } else {
                Buf u = {0};
                char at[24];
                snprintf(at, sizeof at, "%lld", (long long)s);
                ctx_url(c, &u, "w", wq, "new", at, "b", NULL, NULL);
                /* pointer/touch shortcut only: keyboard and screen readers use "Book" + the quick bar
                   instead of wading through ~250 cells a week */
                H(b, "<a class=\"book-wk-s%r\" href=\"%s\" tabindex=\"-1\" aria-hidden=\"true\" title=\"%s %02d:%02d\"></a>",
                  hour ? " book-hr" : "", buf_cstr(&u), weekday_name(dt.wday, false), m / 60, m % 60);
                buf_free(&u);
            }
        }
        /* bookings, clipped to the visible part of this day */
        for (int i = 0; i < tbl_count(&T_booking); i++) {
            Booking *bk = tbl_at(&T_booking, i);
            if (bk->res != r->id || bk->end <= d0 || bk->start >= add_days(d0, 1)) continue;
            int a = MAX(wall_min(bk->start, d0), vis_from), z = MIN(wall_min(bk->end, d0), vis_to);
            if (z <= a) continue;
            Person *p = person_get(bk->who);
            Buf u = {0};
            char id[16];
            snprintf(id, sizeof id, "%d", bk->id);
            ctx_url(c, &u, "w", wq, "b", id, "new", NULL, "edit", NULL, NULL);
            bool mine = c->me && p && p->id == c->me->id;
            Buf at = {0}; /* a link to the booking - or, for a screen that only looks, the block alone */
            if (can) H(&at, "a href=\"%s\"", buf_cstr(&u));
            else buf_puts(&at, "div");
            H(b, "<%r class=\"book-wk-b hued%r%r\" style=\"--h:%d;--top:%d.%02d;--len:%d.%02d\">", buf_cstr(&at),
              mine ? " book-mine" : "", bk->end <= c->now ? " book-done" : "", p ? p->hue : 210,
              (a - vis_from) / slot, (a - vis_from) % slot * 100 / slot, (z - a) / slot, (z - a) % slot * 100 / slot);
            buf_free(&at);
            buf_free(&u);
            H(b, "<b>%s</b><small>", p ? p->name : T("Someone"));
            ui_span(b, bk->start, bk->end);
            buf_puts(b, "</small>");
            if (bk->note[0] && z - a >= 2 * slot) H(b, "<small class=\"book-n\">%s</small>", bk->note);
            buf_puts(b, can ? "</a>" : "</div>");
        }
        if (today) {
            int nm = wall_min(c->now, d0);
            if (nm >= vis_from && nm < vis_to)
                H(b, "<div class=\"book-wk-now\" style=\"--top:%d.%02d\"></div>", (nm - vis_from) / slot,
                  (nm - vis_from) % slot * 100 / slot);
        }
        buf_puts(b, "</div>");
    }
    buf_puts(b, "</div></div></div>");
}

static void daily_grid(Ctx *c, Buf *b, Resource *r, int64_t ws) {
    bool can = ui_can("book.create");
    char wq[11];
    fmt_ymd(ws, wq);
    buf_puts(b, "<div class=\"book-mo\"><div class=\"book-mo-head\">");
    for (int d = 0; d < 7; d++) H(b, "<span>%s</span>", weekday_name(d, true));
    buf_puts(b, "</div><div class=\"book-mo-grid\">");
    for (int i = 0; i < 35; i++) {
        int64_t d0 = add_days(ws, i);
        DateTime dt = dt_local(d0);
        bool past = add_days(d0, 1) <= c->now, today = day_start(c->now) == d0;
        Booking *bk = book_conflict(r->id, d0, add_days(d0, 1), 0);
        H(b, "<div class=\"book-mo-d%r%r%r\">", past ? " book-past" : "", today ? " book-today" : "", bk ? " book-taken" : "");
        H(b, "<span class=\"book-mo-n\">%d%r%s</span>", dt.day, dt.day == 1 ? " " : "", dt.day == 1 ? month_name(dt.mon, true) : "");
        if (bk) {
            Person *p = person_get(bk->who);
            Buf u = {0};
            char id[16];
            snprintf(id, sizeof id, "%d", bk->id);
            ctx_url(c, &u, "w", wq, "b", id, "new", NULL, "edit", NULL, NULL);
            bool first = bk->start >= d0;
            if (can) H(b, "<a class=\"book-mo-b hued%r\" href=\"%s\" style=\"--h:%d\">", first ? " book-first" : "",
                       buf_cstr(&u), p ? p->hue : 210);
            else H(b, "<div class=\"book-mo-b hued%r\" style=\"--h:%d\">", first ? " book-first" : "", p ? p->hue : 210);
            if (first) ui_avatar_deco(b, p, "xs"); /* (the name follows) */
            else ui_avatar(b, p, "xs");
            H(b, "<span>%s</span>%r", first ? (p ? p->name : T("Someone")) : "", can ? "</a>" : "</div>");
            buf_free(&u);
        } else if (!past && can) { /* (a display account only looks - issue #96) */
            Buf u = {0};
            char at[24];
            snprintf(at, sizeof at, "%lld", (long long)d0);
            ctx_url(c, &u, "w", wq, "new", at, "b", NULL, NULL);
            H(b, "<a class=\"book-mo-free\" href=\"%s\" tabindex=\"-1\" aria-hidden=\"true\" title=\"%d %s\">", buf_cstr(&u),
              dt.day, month_name(dt.mon, false));
            ui_icon(b, "plus");
            buf_puts(b, "</a>");
            buf_free(&u);
        }
        buf_puts(b, "</div>");
    }
    buf_puts(b, "</div></div>");
}

/* ------------------------------------------------------------------- pages */

static bool sooner(const Booking *a, const Booking *b) {
    return a->start != b->start ? a->start < b->start : a->id < b->id;
}

/* `links` = each row opens the booking (not on a wall: it only shows - issue #80). */
static void upcoming(Ctx *c, Buf *b, int only_res, int max, bool links) {
    /* the `max` soonest bookings that haven't ended yet, in order — picked from all of them,
       so today's never drown behind a pile booked months ahead */
    Booking *list[64];
    int cap = CLAMP(max, 1, countof(list)), n = 0, all = 0;
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        Booking *bk = tbl_at(&T_booking, i);
        if (bk->end <= c->now || (only_res && bk->res != only_res)) continue;
        Resource *r = res_get(bk->res);
        if (!r || r->archived || !book_open_to(c, r)) continue; /* (a child's, a guest's: what they may book) */
        all++;
        if (n == cap && !sooner(bk, list[n - 1])) continue;
        int j = n < cap ? n++ : n - 1; /* insert in order; when full, the latest drops out */
        for (; j > 0 && sooner(bk, list[j - 1]); j--) list[j] = list[j - 1];
        list[j] = bk;
    }
    if (!n) {
        H(b, "<p class=\"muted\">%r</p>", T("Nothing booked. Everything's free!")); /* (our own words: raw) */
        return;
    }
    int64_t last_day = -1;
    buf_puts(b, "<ul class=\"book-up\">");
    for (int i = 0; i < n && i < max; i++) {
        Booking *bk = list[i];
        Resource *r = res_get(bk->res);
        Person *p = person_get(bk->who);
        int64_t d = MAX(day_start(bk->start), day_start(c->now));
        if (d != last_day) {
            buf_puts(b, "<li class=\"book-up-day\" data-fit-head>"); /* (a wall too short: never left alone) */
            ui_day(b, d, c->now);
            buf_puts(b, "</li>");
            last_day = d;
        }
        char id[16];
        snprintf(id, sizeof id, "%d", bk->id);
        Buf u = {0};
        char ymd[11];
        fmt_ymd(bk->start, ymd);
        buf_printf(&u, "/book/%d?w=%s&b=%s", r->id, ymd, id);
        bool now = bk->start <= c->now;
        if (links) H(b, "<li><a class=\"book-up-row%r\" href=\"%s\">", now ? " book-now" : "", buf_cstr(&u));
        else H(b, "<li><div class=\"book-up-row%r\">", now ? " book-now" : "");
        buf_puts(b, "<span class=\"book-up-t\">");
        if (res_daily(r)) buf_puts(b, T("all day"));
        else ui_span(b, bk->start, bk->end);
        buf_puts(b, "</span><span class=\"book-up-r\">");
        res_title(b, r);
        if (bk->series)
            H(b, " <span class=\"book-rep\" aria-hidden=\"true\">↻</span><span class=\"sr-only\">%s</span>", T(", repeats"));
        if (bk->note[0]) H(b, " <small>· %s</small>", bk->note);
        buf_puts(b, "</span>");
        ui_person(b, p);
        buf_puts(b, links ? "</a></li>" : "</div></li>");
        buf_free(&u);
    }
    /* more than fit: say so, rather than letting them go unseen (issue #53: Friday's car vanished) */
    if (all > n) {
        if (links && !str_starts(c->path, "/book")) buf_puts(b, "<li class=\"book-up-more\" data-fit-keep><a href=\"/book\">");
        else H(b, "<li class=\"book-up-more\" data-fit-keep data-fit-more=\"%d\">", all - n);
        H(b, T("+%d more"), all - n);
        buf_puts(b, links && !str_starts(c->path, "/book") ? "</a></li>" : "</li>");
    }
    buf_puts(b, "</ul>");
}

static void overview(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Bookings"));
    Resource *rs[64];
    int n = book_res_list(c, rs, 64);
    ui_head(b, T("Bookings"), n ? T("What's booked and what's free") : NULL);
    if (ui_can("book.res_save")) { /* (a display account only looks: issue #96) */
        buf_puts(b, "<a class=\"btn\" href=\"/book?res=new\">");
        ui_icon(b, "plus");
        H(b, "<span>%s</span></a>", T("Add a thing"));
    }
    ui_head_end(b);
    news(c, b);
    if (!n && (is_child(c) || is_guest(c))) {
        ui_empty(b, "🗝️", T("Nothing to book yet"),
                 is_child(c) ? T("A grown-up decides what children may book.") : T("An admin decides what guests may book."));
    } else if (!n) {
        ui_empty(b, "🗝️", T("What do you share at home?"),
                 T("Add the things people take turns with — then anyone can book them from their phone."));
        buf_puts(b, "<div class=\"chips book-presets center\">");
        for (int i = 0; i < book_presets_count(); i++) {
            ui_form(b, "book.preset", "inline");
            ui_hidden(b, "name", book_presets_name(i)); /* (English: which one) */
            H(b, "<button class=\"chip\">%s %s</button>", book_presets_emoji(i), T(book_presets_name(i)));
            ui_form_end(b);
        }
        buf_puts(b, "</div>");
    } else {
        quick_bar(c, b, 0);
        buf_puts(b, "<div class=\"book-tiles\">");
        for (int i = 0; i < n; i++) {
            Resource *r = rs[i];
            H(b, "<a class=\"book-tile\" href=\"/book/%d\" style=\"--h:%d\"><span class=\"book-tile-e\">%s</span>", r->id, r->hue,
              r->emoji[0] ? r->emoji : "📅");
            H(b, "<b>%s</b><small>", r->name);
            res_status(c, b, r);
            buf_puts(b, "</small></a>");
        }
        buf_puts(b, "</div>");
        ui_card(b, TC("bookings", "Coming up"), NULL, NULL);
        upcoming(c, b, 0, 30, ui_can("book.cancel"));
        ui_card_end(b);
    }
    const char *rq = ctx_q(c, "res");
    if (str_eq(rq, "new")) resource_modal(c, b, NULL, "/book");
    int bid = (int)ctx_qint(c, "b", 0);
    if (bid && book_get(bid)) booking_modal(c, b, book_get(bid), "/book");
}

static void resource_page(Ctx *c, Buf *b, Resource *r) {
    ctx_title(c, "%s", r->name);
    Buf title = {0};
    res_title(&title, r);
    buf_puts(b, "<header class=\"ph\"><div class=\"ph-t\"><h1>");
    buf_put(b, title.p, title.len);
    buf_puts(b, "</h1>");
    if (r->rules[0]) H(b, "<p class=\"sub\">%s</p>", r->rules);
    buf_puts(b, "</div><div class=\"ph-a\">");
    if (ui_can("book.res_save")) {
        char label[80];
        snprintf(label, sizeof label, T("Edit %s"), r->name);
        H(b, "<a class=\"icon-btn\" href=\"/book/%d?res=%d\" aria-label=\"%s\">", r->id, r->id, label);
        ui_icon(b, "settings");
        buf_puts(b, "</a>");
    }
    int64_t len = res_daily(r) ? 86400 : (int64_t)(r->default_min > 0 ? r->default_min : 60) * 60, free_at;
    if (ui_can("book.create") && book_next_free(r, c->now, len, 0, &free_at)) {
        char at[24], w[11];
        snprintf(at, sizeof at, "%lld", (long long)free_at);
        fmt_ymd(free_at, w);
        H(b, "<a class=\"btn primary\" href=\"/book/%d?w=%s&new=%s\">", r->id, w, at);
        ui_icon(b, "plus");
        H(b, "<span>%s</span></a>", T("Book"));
    }
    buf_puts(b, "</div></header>");
    buf_free(&title);

    /* switch between things */
    Resource *rs[64];
    int n = book_res_list(c, rs, 64);
    if (n > 1) {
        buf_puts(b, "<nav class=\"chips book-res-switch\">");
        for (int i = 0; i < n; i++)
            H(b, "<a class=\"chip%r\" href=\"/book/%d\">%s %s</a>", rs[i]->id == r->id ? " on" : "", rs[i]->id,
              rs[i]->emoji, rs[i]->name);
        buf_puts(b, "</nav>");
    }
    quick_bar(c, b, r->id);

    int y, m, d;
    int64_t base = parse_ymd(ctx_q(c, "w"), &y, &m, &d) ? dt_make(y, m, d, 12, 0) : c->now;
    int64_t ws = week_start(base);
    buf_puts(b, "<div class=\"card book-grid-card\"><div class=\"toolbar\">");
    week_nav(c, b, ws, res_daily(r));
    buf_puts(b, "</div>");
    if (res_daily(r)) daily_grid(c, b, r, ws);
    else hourly_grid(c, b, r, ws);
    buf_puts(b, "</div>");

    ui_card(b, TC("bookings", "Coming up"), NULL, NULL);
    upcoming(c, b, r->id, 12, ui_can("book.cancel"));
    ui_card_end(b);

    /* modals */
    Buf close = {0};
    ctx_url(c, &close, "new", NULL, "b", NULL, "edit", NULL, "res", NULL, NULL);
    long long at = ctx_qint(c, "new", 0);
    int bid = (int)ctx_qint(c, "b", 0);
    if (str_eq(ctx_q(c, "res"), "new")) resource_modal(c, b, NULL, buf_cstr(&close));
    else if (ctx_qint(c, "res", 0) == r->id) resource_modal(c, b, r, buf_cstr(&close));
    else if (bid && book_get(bid)) booking_modal(c, b, book_get(bid), buf_cstr(&close));
    else if (at > 0) {
        Buf t = {0};
        buf_printf(&t, T("Book %s"), r->name);
        ui_modal(b, buf_cstr(&t), buf_cstr(&close));
        booking_form(c, b, r, NULL, (int64_t)at);
        ui_modal_end(b);
        buf_free(&t);
    }
    buf_free(&close);
}

void book_page(Ctx *c, Buf *b) {
    int id = 0;
    if (str_starts(c->path, "/book/")) id = (int)str_int_or(c->path + 6, 0);
    Resource *r = res_get(id);
    if (r && !book_open_to(c, r)) { /* a child's, a guest's (issue #44) */
        ctx_title(c, "%s", r->name);
        ui_empty(b, "🔒", is_child(c) ? T("Not for children") : T("Not open to guests"),
                 is_child(c) ? T("A grown-up books this one.") : T("Ask someone in the household about it."));
        H(b, "<p class=\"center\"><a class=\"btn\" href=\"/book\">%s</a></p>", T("What you can book"));
        return;
    }
    if (r) resource_page(c, b, r);
    else overview(c, b);
    if (!r && ctx_qint(c, "res", 0) > 0) {
        Resource *e = res_get((int)ctx_qint(c, "res", 0));
        if (e) resource_modal(c, b, e, "/book");
    }
}

/* ----------------------------------------------------------------- widgets */

/* Each thing, free or whose it is right now. */
static void now_pills(Ctx *c, Buf *b, Resource **rs, int n) {
    buf_puts(b, "<div class=\"book-res-now\">");
    for (int i = 0; i < n && i < 8; i++) {
        Booking *cur = book_current(rs[i]->id, c->now);
        Person *p = cur ? person_get(cur->who) : NULL;
        H(b, "<a class=\"book-res-pill%r\" href=\"/book/%d\" title=\"%s\">%s<span>%s</span>", cur ? " book-busy" : "", rs[i]->id,
          rs[i]->name, rs[i]->emoji[0] ? rs[i]->emoji : "📅", cur ? (p ? p->name : T("busy")) : T("free"));
        buf_puts(b, "</a>");
    }
    buf_puts(b, "</div>");
}

void book_widget(Ctx *c, Buf *b, int where) {
    (void)where;
    Resource *rs[64];
    int n = book_res_list(c, rs, 64);
    if (!n && (is_child(c) || is_guest(c))) return; /* (nothing they may book: no card) */
    ui_card(b, T("Bookings"), "/book", n ? T("Book") : T("Set up"));
    news(c, b);
    if (!n) {
        H(b, "<p class=\"muted\">%r</p>",
          T("Share the car, the laundry room or the guest room — add them under <a href=\"/book\">Book</a>."));
        ui_card_end(b);
        return;
    }
    now_pills(c, b, rs, n);
    upcoming(c, b, 0, 5, ui_can("book.cancel"));
    ui_card_end(b);
}

bool book_wall(Ctx *c, Buf *b) {
    Resource *rs[64];
    int n = res_list(rs, 64, false);
    if (!n) return false; /* nothing to book: the wall leaves the card out */
    /* each thing by name, read from across the room: free, or whose and until when */
    buf_puts(b, "<div class=\"book-wall\"><ul class=\"book-wall-now\">");
    for (int i = 0; i < n && i < 8; i++) {
        Booking *cur = book_current(rs[i]->id, c->now);
        Person *p = cur ? person_get(cur->who) : NULL;
        H(b, "<li class=\"%s\"><span class=\"book-wall-e\">%s</span><b>%s</b><span class=\"book-wall-s\">",
          cur ? "book-busy" : "book-free", rs[i]->emoji[0] ? rs[i]->emoji : "📅", rs[i]->name);
        if (!cur) buf_puts(b, T("Free"));
        else { /* "until 17:00", another day "until Tomorrow 02:00" (issue #80: the time went missing) */
            H(b, "%s · ", p ? p->name : T("Busy"));
            DateTime e = dt_local(cur->end); /* (clock times: "hasta las 17:00") */
            if (day_start(cur->end) != day_start(c->now)) {
                Buf day = {0};
                book_day(&day, cur->end, c->now);
                H(b, T("until %s %02d:%02d"), buf_cstr(&day), e.hour, e.min);
                buf_free(&day);
            } else {
                H(b, T("until %02d:%02d"), e.hour, e.min);
            }
        }
        buf_puts(b, "</span></li>");
    }
    buf_puts(b, "</ul>");
    upcoming(c, b, 0, 5, false); /* (a wall only shows: rows, not links) */
    buf_puts(b, "</div>");
    return true;
}
