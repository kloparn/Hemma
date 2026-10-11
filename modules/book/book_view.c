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
        buf_puts(b, "<span class=\"book-st book-busy\">Busy until ");
        if (day_start(cur->end - 1) == day_start(c->now) && !res_daily(r)) ui_hm(b, cur->end);
        else ui_day(b, res_daily(r) ? add_days(cur->end, -1) : cur->end, c->now);
        H(b, "</span> · %s", p ? p->name : "someone");
        return;
    }
    buf_puts(b, "<span class=\"book-st book-free\">Free now</span>");
    Booking *nx = book_next(r->id, c->now);
    if (nx && nx->start < add_days(c->now, 14)) {
        Person *p = person_get(nx->who);
        buf_puts(b, " · next ");
        ui_day(b, nx->start, c->now);
        if (!res_daily(r)) {
            buf_putc(b, ' ');
            ui_hm(b, nx->start);
        }
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
      "placeholder=\"Book something… e.g. “car fri 14-17 ikea trip”\" aria-label=\"Quick booking\">"
      "<button class=\"btn primary\">Book</button></div>",
      q);
    if (str_blank(q)) {
        buf_puts(b, "<p class=\"quick-hint\">Type what, when and (optionally) for whom: <i>laundry tomorrow 18-21</i>, "
                    "<i>guest room fri-sun @eva</i>, <i>bastu ikväll 19</i>. Press <span class=\"kbd\">/</span> to "
                    "start typing.</p>");
    } else {
        Quick k;
        book_quick_parse(q, res, c->now, &k);
        Resource *r = res_get(k.res);
        bool ok = !k.problem[0] && r;
        H(b, "<div class=\"preview %s\">", ok ? "ok" : "bad");
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
        }
        if (k.problem[0]) H(b, "<span class=\"book-why\">%s</span>", k.problem);
        else buf_puts(b, "<span class=\"book-why ok\">Free — press Enter to book</span>");
        buf_puts(b, "</div>");
    }
    ui_form_end(b);
    /* A free alternative after a clash: one tap books it (own form, after the quick form). */
    if (!str_blank(q)) {
        Quick k;
        book_quick_parse(q, res, c->now, &k);
        Resource *r = res_get(k.res);
        if (k.has_alt && r) {
            ui_form(b, "book.create", "book-alt");
            ui_hidden_int(b, "res", r->id);
            ui_hidden_int(b, "start", k.alt_start);
            ui_hidden_int(b, "end", k.alt_end);
            ui_hidden_int(b, "who", k.who);
            ui_hidden(b, "note", k.note);
            buf_puts(b, "<button class=\"btn\">");
            ui_icon(b, "sparkle");
            buf_puts(b, "<span>Book ");
            book_span_text(r, k.alt_start, k.alt_end, c->now, b);
            buf_puts(b, " instead</span></button>");
            ui_form_end(b);
        }
    }
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
      res_daily(r) ? "First day" : "Day", ymd);
    if (res_daily(r)) {
        H(b, "<label class=\"field\"><span>Last day</span><input type=\"date\" name=\"date_to\" value=\"%s\"></label>",
          ymd2);
    } else {
        int step = r->slot * 60;
        H(b,
          "<label class=\"field\"><span>From</span><input type=\"time\" name=\"from\" value=\"%s\" step=\"%d\" "
          "required></label><label class=\"field\"><span>Until</span><input type=\"time\" name=\"to\" value=\"%s\" "
          "step=\"%d\" required></label>",
          hm1, step, hm2, step);
    }
    buf_puts(b, "</div><label class=\"field\"><span>For</span>");
    ui_person_select(b, "who", edit ? edit->who : (c->me ? c->me->id : 0), NULL);
    H(b,
      "</label><label class=\"field\"><span>Note</span><input name=\"note\" maxlength=\"100\" value=\"%s\" "
      "placeholder=\"Optional — where, why…\"></label>",
      edit ? edit->note : "");
    if (r->rules[0]) H(b, "<p class=\"book-rules\">%s</p>", r->rules);
    buf_puts(b, "<div class=\"modal-f\"><button class=\"btn primary\">");
    buf_puts(b, edit ? "Save changes" : "Book it");
    buf_puts(b, "</button></div>");
    ui_form_end(b);
}

static void booking_modal(Ctx *c, Buf *b, Booking *bk, const char *close) {
    Resource *r = res_get(bk->res);
    if (!r) return;
    Person *who = person_get(bk->who), *by = person_get(bk->by);
    ui_modal(b, r->name, close);
    if (str_eq(ctx_q(c, "edit"), "1")) {
        booking_form(c, b, r, bk, 0);
        ui_modal_end(b);
        return;
    }
    buf_puts(b, "<div class=\"book-bk-detail\">");
    ui_avatar(b, who, "lg");
    H(b, "<div><b class=\"book-bk-who\">%s</b><div class=\"book-bk-when\">", who ? who->name : "Someone");
    book_span_text(r, bk->start, bk->end, c->now, b);
    buf_puts(b, "</div>");
    if (bk->note[0]) H(b, "<p class=\"book-bk-note\">%s</p>", bk->note);
    buf_puts(b, "<small class=\"muted\">Booked ");
    if (by && by != who) H(b, "by %s ", by->name);
    ui_ago(b, bk->created, c->now);
    buf_puts(b, "</small></div></div><div class=\"modal-f spread\">");
    ui_form(b, "book.cancel", "inline");
    ui_hidden_int(b, "id", bk->id);
    buf_puts(b, "<button class=\"btn danger\">Cancel booking</button>");
    ui_form_end(b);
    Buf u = {0};
    ctx_url(c, &u, "edit", "1", NULL);
    H(b, "<a class=\"btn\" href=\"%s\">Change</a></div>", buf_cstr(&u));
    buf_free(&u);
    ui_modal_end(b);
}

static void resource_modal(Ctx *c, Buf *b, Resource *r, const char *close) {
    ui_modal(b, r ? "Edit" : "Something new to book", close);
    if (!r) {
        buf_puts(b, "<p class=\"muted\">Quick picks:</p><div class=\"chips book-presets\">");
        for (int i = 0; i < book_presets_count(); i++) {
            ui_form(b, "book.preset", "inline");
            ui_hidden(b, "name", book_presets_name(i));
            H(b, "<button class=\"chip\">%s %s</button>", book_presets_emoji(i), book_presets_name(i));
            ui_form_end(b);
        }
        buf_puts(b, "</div><h3 class=\"mt\">Or describe it</h3>");
    }
    ui_form(b, "book.res_save", "stack");
    if (r) ui_hidden_int(b, "id", r->id);
    H(b,
      "<div class=\"field-row\"><label class=\"field grow\"><span>Name</span><input name=\"name\" required "
      "maxlength=\"36\" value=\"%s\" placeholder=\"Car, laundry room, sauna…\"%r></label>"
      "<label class=\"field narrow\"><span>Emoji</span><input name=\"emoji\" maxlength=\"8\" value=\"%s\" "
      "placeholder=\"🚗\"></label></div>",
      r ? r->name : "", r ? "" : " autofocus", r ? r->emoji : "");
    bool daily = r && res_daily(r);
    bool always = !r || (r->open_min <= 0 && r->close_min >= 1440);
    char open[6], close_hm[6];
    snprintf(open, sizeof open, "%02d:%02d", (r ? r->open_min : 420) / 60, (r ? r->open_min : 420) % 60);
    int cm = r && r->close_min < 1440 ? r->close_min : 22 * 60;
    snprintf(close_hm, sizeof close_hm, "%02d:%02d", cm / 60, cm % 60);
    H(b,
      "<div class=\"field\"><span>Booked by</span><div class=\"chips\">"
      "<label class=\"chip-r\"><input type=\"radio\" name=\"kind\" value=\"hourly\"%r><span>The hour</span></label>"
      "<label class=\"chip-r\"><input type=\"radio\" name=\"kind\" value=\"daily\"%r><span>Whole days</span></label>"
      "</div></div>",
      daily ? "" : " checked", daily ? " checked" : "");
    buf_puts(b, "<div class=\"field-row\"><label class=\"field\"><span>Steps</span><select name=\"slot\">");
    static const int slots[] = {15, 30, 60};
    for (int i = 0; i < 3; i++)
        H(b, "<option value=\"%d\"%r>%d min</option>", slots[i], (r ? r->slot : 30) == slots[i] ? " selected" : "",
          slots[i]);
    H(b,
      "</select></label><label class=\"field\"><span>Usual length (min)</span><input type=\"number\" name=\"dflt\" "
      "min=\"15\" step=\"15\" value=\"%d\"></label><label class=\"field\"><span>Max (min, 0 = none)</span>"
      "<input type=\"number\" name=\"max\" min=\"0\" step=\"15\" value=\"%d\"></label></div>",
      r && !res_daily(r) ? r->default_min : 120, r ? r->max_min : 0);
    H(b,
      "<div class=\"field\"><span>Hours</span><label class=\"check\"><input type=\"checkbox\" name=\"always\" "
      "value=\"1\"%r> Around the clock</label><div class=\"field-row\"><label class=\"field\"><span>From</span>"
      "<input type=\"time\" name=\"open\" value=\"%s\"></label><label class=\"field\"><span>Until</span>"
      "<input type=\"time\" name=\"close\" value=\"%s\"></label></div></div>",
      always ? " checked" : "", open, close_hm);
    H(b,
      "<label class=\"field\"><span>House rules (optional)</span><input name=\"rules\" maxlength=\"150\" "
      "value=\"%s\" placeholder=\"Fill up the tank, clean the filter…\"></label>",
      r ? r->rules : "");
    buf_puts(b, "<div class=\"field\"><span>Colour</span>");
    ui_hue_picker(b, "hue", r ? r->hue : 200);
    buf_puts(b, "</div><div class=\"modal-f\"><button class=\"btn primary\">Save</button></div>");
    ui_form_end(b);
    if (r) {
        ui_form(b, "book.res_archive", "danger-zone");
        ui_hidden_int(b, "id", r->id);
        H(b, "<button class=\"btn danger\">%s</button>", r->archived ? "Make bookable again" : "Hide from bookings");
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
    H(b, "<div class=\"book-wk-nav\"><a class=\"icon-btn\" href=\"%s\" aria-label=\"Earlier\">", buf_cstr(&up));
    ui_icon(b, "left");
    H(b, "</a><a class=\"icon-btn\" href=\"%s\" aria-label=\"Later\">", buf_cstr(&un));
    ui_icon(b, "right");
    buf_puts(b, "</a><b class=\"book-wk-range\">");
    if (!daily) buf_printf(b, "Week %d · ", iso_week(ws, NULL));
    if (a.mon == z.mon) buf_printf(b, "%d–%d %s", a.day, z.day, month_name(a.mon, false));
    else buf_printf(b, "%d %s – %d %s", a.day, month_name(a.mon, true), z.day, month_name(z.mon, true));
    bool here = c->now >= ws && c->now < add_days(ws, daily ? 35 : 7);
    buf_puts(b, "</b></div>");
    if (!here) H(b, "<a class=\"btn sm\" href=\"%s\">Today</a>", buf_cstr(&ut));
    buf_free(&up);
    buf_free(&un);
    buf_free(&ut);
}

static void hourly_grid(Ctx *c, Buf *b, Resource *r, int64_t ws) {
    int slot = MAX(r->slot, 15);
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
            if (e <= c->now) {
                H(b, "<span class=\"book-wk-s book-past%r\"></span>", hour ? " book-hr" : "");
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
            H(b, "<a class=\"book-wk-b hued%r%r\" href=\"%s\" style=\"--h:%d;--top:%d.%02d;--len:%d.%02d\">",
              mine ? " book-mine" : "", bk->end <= c->now ? " book-done" : "", buf_cstr(&u), p ? p->hue : 210,
              (a - vis_from) / slot, (a - vis_from) % slot * 100 / slot, (z - a) / slot, (z - a) % slot * 100 / slot);
            buf_free(&u);
            H(b, "<b>%s</b><small>", p ? p->name : "Someone");
            ui_span(b, bk->start, bk->end);
            buf_puts(b, "</small>");
            if (bk->note[0] && z - a >= 2 * slot) H(b, "<small class=\"book-n\">%s</small>", bk->note);
            buf_puts(b, "</a>");
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
            H(b, "<a class=\"book-mo-b hued%r\" href=\"%s\" style=\"--h:%d\">", first ? " book-first" : "", buf_cstr(&u),
              p ? p->hue : 210);
            ui_avatar(b, p, "xs");
            H(b, "<span>%s</span></a>", first ? (p ? p->name : "Someone") : "");
            buf_free(&u);
        } else if (!past) {
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

static void upcoming(Ctx *c, Buf *b, int only_res, int max) {
    /* the `max` soonest bookings that haven't ended yet, in order — picked from all of them,
       so today's never drown behind a pile booked months ahead */
    Booking *list[64];
    int cap = CLAMP(max, 1, countof(list)), n = 0;
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        Booking *bk = tbl_at(&T_booking, i);
        if (bk->end <= c->now || (only_res && bk->res != only_res)) continue;
        Resource *r = res_get(bk->res);
        if (!r || r->archived) continue;
        if (n == cap && !sooner(bk, list[n - 1])) continue;
        int j = n < cap ? n++ : n - 1; /* insert in order; when full, the latest drops out */
        for (; j > 0 && sooner(bk, list[j - 1]); j--) list[j] = list[j - 1];
        list[j] = bk;
    }
    if (!n) {
        buf_puts(b, "<p class=\"muted\">Nothing booked. Everything's free!</p>");
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
            buf_puts(b, "<li class=\"book-up-day\">");
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
        H(b, "<li><a class=\"book-up-row%r\" href=\"%s\"><span class=\"book-up-t\">", now ? " book-now" : "", buf_cstr(&u));
        if (res_daily(r)) buf_puts(b, "all day");
        else ui_span(b, bk->start, bk->end);
        buf_puts(b, "</span><span class=\"book-up-r\">");
        res_title(b, r);
        if (bk->note[0]) H(b, " <small>· %s</small>", bk->note);
        buf_puts(b, "</span>");
        ui_person(b, p);
        buf_puts(b, "</a></li>");
        buf_free(&u);
    }
    buf_puts(b, "</ul>");
}

static void overview(Ctx *c, Buf *b) {
    ctx_title(c, "Bookings");
    Resource *rs[64];
    int n = res_list(rs, 64, false);
    ui_head(b, "Bookings", n ? "What's booked and what's free" : NULL);
    buf_puts(b, "<a class=\"btn\" href=\"/book?res=new\">");
    ui_icon(b, "plus");
    buf_puts(b, "<span>Add a thing</span></a>");
    ui_head_end(b);
    if (!n) {
        ui_empty(b, "🗝️", "What do you share at home?",
                 "Add the things people take turns with — then anyone can book them from their phone.");
        buf_puts(b, "<div class=\"chips book-presets center\">");
        for (int i = 0; i < book_presets_count(); i++) {
            ui_form(b, "book.preset", "inline");
            ui_hidden(b, "name", book_presets_name(i));
            H(b, "<button class=\"chip\">%s %s</button>", book_presets_emoji(i), book_presets_name(i));
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
        ui_card(b, "Coming up", NULL, NULL);
        upcoming(c, b, 0, 30);
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
    H(b, "<a class=\"icon-btn\" href=\"/book/%d?res=%d\" aria-label=\"Edit %s\">", r->id, r->id, r->name);
    ui_icon(b, "settings");
    buf_puts(b, "</a>");
    int64_t len = res_daily(r) ? 86400 : (int64_t)(r->default_min > 0 ? r->default_min : 60) * 60, free_at;
    if (book_next_free(r, c->now, len, 0, &free_at)) {
        char at[24], w[11];
        snprintf(at, sizeof at, "%lld", (long long)free_at);
        fmt_ymd(free_at, w);
        H(b, "<a class=\"btn primary\" href=\"/book/%d?w=%s&new=%s\">", r->id, w, at);
        ui_icon(b, "plus");
        buf_puts(b, "<span>Book</span></a>");
    }
    buf_puts(b, "</div></header>");
    buf_free(&title);

    /* switch between things */
    Resource *rs[64];
    int n = res_list(rs, 64, false);
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

    ui_card(b, "Coming up", NULL, NULL);
    upcoming(c, b, r->id, 12);
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
        buf_printf(&t, "Book %s", r->name);
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
    if (r) resource_page(c, b, r);
    else overview(c, b);
    if (!r && ctx_qint(c, "res", 0) > 0) {
        Resource *e = res_get((int)ctx_qint(c, "res", 0));
        if (e) resource_modal(c, b, e, "/book");
    }
}

/* ----------------------------------------------------------------- widgets */

void book_widget(Ctx *c, Buf *b, int where) {
    Resource *rs[64];
    int n = res_list(rs, 64, false);
    if (!n && where == WIDGET_WALL) return;
    ui_card(b, "Bookings", "/book", n ? "Book" : "Set up");
    if (!n) {
        buf_puts(b, "<p class=\"muted\">Share the car, the laundry room or the guest room — add them under "
                    "<a href=\"/book\">Book</a>.</p>");
        ui_card_end(b);
        return;
    }
    buf_puts(b, "<div class=\"book-res-now\">");
    for (int i = 0; i < n && i < 8; i++) {
        Booking *cur = book_current(rs[i]->id, c->now);
        Person *p = cur ? person_get(cur->who) : NULL;
        H(b, "<a class=\"book-res-pill%r\" href=\"/book/%d\" title=\"%s\">%s<span>%s</span>", cur ? " book-busy" : "", rs[i]->id,
          rs[i]->name, rs[i]->emoji[0] ? rs[i]->emoji : "📅", cur ? (p ? p->name : "busy") : "free");
        buf_puts(b, "</a>");
    }
    buf_puts(b, "</div>");
    upcoming(c, b, 0, where == WIDGET_WALL ? 8 : 5);
    ui_card_end(b);
}
