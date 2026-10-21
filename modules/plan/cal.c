/* modules/plan/cal.c — the shared calendar (/plan/cal): quick add, a
 * mini-month with busy-day dots, and a five-week agenda. */
#include "plan.h"

#define MAX_EVENTS 4096
#define AGENDA_DAYS 35

static int cmp_event(const void *a, const void *b) {
    const Event *x = tbl_at(&T_event, *(const int *)a), *y = tbl_at(&T_event, *(const int *)b);
    if (x->allday != y->allday) return x->allday ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->id - y->id;
}

static bool overlaps(const Event *e, int64_t from, int64_t to) { return e->start < to && e->end > from; }

/* Events touching [from, to), sorted; returns the count. */
static int events_between(int64_t from, int64_t to, int *out, int max) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_event) && n < max; i++)
        if (overlaps(tbl_at(&T_event, i), from, to)) out[n++] = i;
    qsort(out, (size_t)n, sizeof *out, cmp_event);
    return n;
}

static int64_t selected_day(Ctx *c) {
    int y, m, d;
    if (parse_ymd(ctx_q(c, "day"), &y, &m, &d)) return dt_make(y, m, d, 0, 0);
    return day_start(c->now);
}

/* span: one row for an event that lasts several days ("until Sun") instead of one per day */
static void time_label(Buf *b, const Event *e, int64_t day, bool span) {
    int64_t next = add_days(day, 1);
    if (span && e->end > next) {
        buf_printf(b, TC("day", "until %s"), weekday_name(dt_local(e->end - 1).wday, true));
    } else if (e->allday || (e->start <= day && e->end >= next)) buf_puts(b, T("All day"));
    else if (e->start >= day && e->end <= next) ui_span(b, e->start, e->end);
    else if (e->start >= day) {
        ui_hm(b, e->start);
        buf_puts(b, " →");
    } else {
        buf_puts(b, "→ ");
        ui_hm(b, e->end);
    }
}

static void event_row(Ctx *c, Buf *b, const Event *e, int64_t day, const char *href, const char *idp, bool span) {
    Person *p = person_get(e->who);
    H(b, "<li class=\"plan-ev%r\" id=\"%s%d-%L\"", e->allday ? " plan-allday" : "", idp, e->id, (long long)day);
    if (p) ui_hue_style(b, p->hue);
    buf_puts(b, "><span class=\"plan-ev-time\">");
    time_label(b, e, day, span);
    buf_puts(b, "</span>");
    const char *icon = plan_task_icon(e->title);
    if (href) H(b, "<a class=\"plan-ev-main\" href=\"%s\">", href);
    else buf_puts(b, "<div class=\"plan-ev-main\">");
    H(b, "<b>%s%r%s</b>", icon, icon[0] ? " " : "", e->title);
    if (e->series)
        H(b, " <span class=\"plan-rep\" aria-hidden=\"true\">↻</span><span class=\"sr-only\">%s</span>", T(", repeats"));
    if (e->note[0]) H(b, "<small>%s</small>", e->note);
    buf_puts(b, href ? "</a>" : "</div>");
    if (p) {
        H(b, "<span class=\"plan-ev-who\"><span class=\"sr-only\">%s </span>", T("for"));
        ui_avatar(b, p, "sm");
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</li>");
}

/* Other modules' timed things (bookings, ...) via the shared agenda hook. */
static bool ext_overlaps(const AgendaItem *it, int64_t from, int64_t to) {
    return it->start < to && (it->end > from || (it->end == it->start && it->start >= from));
}

static int ext_count(const Agenda *ag, int64_t from, int64_t to) {
    int n = 0;
    for (int i = 0; i < ag->n; i++) n += ext_overlaps(&ag->items[i], from, to);
    return n;
}

/* link = the row opens the item (not for a screen that only looks: issue #96) */
static void ext_row(Buf *b, const AgendaItem *it, int64_t day, bool link) {
    Person *p = person_get(it->who);
    const Module *m = module_find(it->module);
    H(b, "<li class=\"plan-ev plan-ext%r\"", it->allday ? " plan-allday" : "");
    if (p) ui_hue_style(b, p->hue);
    buf_puts(b, "><span class=\"plan-ev-time\">");
    int64_t next = add_days(day, 1);
    if (it->allday || (it->start <= day && it->end >= next)) buf_puts(b, T("All day"));
    else if (it->start >= day && it->end <= next) ui_span(b, it->start, it->end);
    else if (it->start >= day) {
        ui_hm(b, it->start);
        buf_puts(b, " →");
    } else {
        buf_puts(b, "→ ");
        ui_hm(b, it->end);
    }
    if (link) H(b, "</span><a class=\"plan-ev-main\" href=\"%s\">", it->link);
    else buf_puts(b, "</span><div class=\"plan-ev-main\">");
    /* (kind stays English in the item - the calendar feed tells to-dos by it - and is said here) */
    H(b, "<b>%s%r%s</b><small>%s</small>%r", it->icon, it->icon[0] ? " " : "", it->title,
      it->kind[0] ? T(it->kind) : m ? T(m->title) : "", link ? "</a>" : "</div>");
    if (p) {
        H(b, "<span class=\"plan-ev-who\"><span class=\"sr-only\">%s </span>", T("for"));
        ui_avatar(b, p, "sm");
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</li>");
}

static void sep(Buf *b) { buf_puts(b, "<span class=\"plan-sep\">·</span>"); }

static void preview(Ctx *c, Buf *b) {
    const char *q = ctx_live(c, "plan.eq");
    if (str_blank(q)) return;
    EventDraft d;
    char err[160];
    if (!plan_parse_event(q, c->now, &d, err, sizeof err)) {
        H(b, "<div class=\"preview bad\" data-say>%s</div>", err);
        return;
    }
    char when[96];
    when_describe(d.start, d.end, c->now, when, sizeof when);
    const char *icon = plan_task_icon(d.title);
    H(b, "<div class=\"preview ok\" data-say>%s%r<b>%s</b>", icon, icon[0] ? " " : "", d.title);
    sep(b);
    html_esc(b, when);
    Person *p = person_get(d.who);
    if (p) {
        sep(b);
        ui_person(b, p);
    }
    if (d.repeat) {
        sep(b);
        buf_puts(b, "↻ ");
        plan_repeat_label(b, d.repeat, dt_local(d.start).wday + 1);
    }
    buf_puts(b, "</div>");
}

/* ------------------------------------------------------------ mini month */

static void mini_month(Ctx *c, Buf *b, int year, int mon, int64_t sel) {
    int64_t first = dt_make(year, mon, 1, 0, 0), today = day_start(c->now);
    int64_t grid = week_start(first);
    int py = mon == 1 ? year - 1 : year, pm = mon == 1 ? 12 : mon - 1;
    int ny = mon == 12 ? year + 1 : year, nm = mon == 12 ? 1 : mon + 1;
    char ym[8], ymd[11];
    Buf u = {0};
    buf_puts(b, "<div class=\"card plan-mm\"><div class=\"plan-mm-h\">");
    snprintf(ym, sizeof ym, "%04d-%02d", py, pm);
    ctx_url(c, &u, "m", ym, "edit", NULL, "new", NULL, NULL);
    H(b, "<a class=\"icon-btn\" href=\"%s\" aria-label=\"%s\">", buf_cstr(&u), T("Previous month"));
    ui_icon(b, "left");
    char my[48]; /* (a heading: Swedish and Spanish months are lower case) */
    snprintf(my, sizeof my, "%s %d", month_name(mon, false), year);
    plan_capitalise(my);
    H(b, "</a><b>%s</b>", my);
    buf_reset(&u);
    snprintf(ym, sizeof ym, "%04d-%02d", ny, nm);
    ctx_url(c, &u, "m", ym, "edit", NULL, "new", NULL, NULL);
    H(b, "<a class=\"icon-btn\" href=\"%s\" aria-label=\"%s\">", buf_cstr(&u), T("Next month"));
    ui_icon(b, "right");
    H(b, "</a></div><div class=\"plan-mm-grid\"><span class=\"plan-mm-wk plan-mm-hd\">%s</span>", T("wk"));
    for (int d = 0; d < 7; d++) H(b, "<span class=\"plan-mm-hd\">%c</span>", weekday_name(d, true)[0]);
    snprintf(ym, sizeof ym, "%04d-%02d", year, mon);
    static int idx[MAX_EVENTS];
    Agenda ext = {0};
    agenda_collect(c, grid, add_days(grid, 42), "plan", &ext);
    for (int w = 0; w < 6; w++) {
        int64_t w0 = add_days(grid, w * 7);
        if (w >= 4 && dt_local(w0).mon != mon) break;
        H(b, "<span class=\"plan-mm-wk\">%d</span>", iso_week(w0, NULL));
        for (int d = 0; d < 7; d++) {
            int64_t day = add_days(w0, d);
            DateTime dd = dt_local(day);
            int n = events_between(day, add_days(day, 1), idx, MAX_EVENTS);
            int nx = ext_count(&ext, day, add_days(day, 1));
            fmt_ymd(day, ymd);
            buf_reset(&u);
            ctx_url(c, &u, "m", ym, "day", ymd, "edit", NULL, "new", NULL, NULL);
            H(b, "<a class=\"plan-mm-d%r%r%r%r\" href=\"%s\" aria-label=\"%s %d %s%s\">", dd.mon != mon ? " plan-out" : "",
              day == today ? " plan-today" : "", day == sel ? " plan-sel" : "", n + nx ? " plan-busy" : "", buf_cstr(&u),
              weekday_name(dd.wday, false), dd.day, month_name(dd.mon, false), n + nx ? T(", has events") : "");
            H(b, "<span>%d</span><i class=\"plan-dots\">", dd.day);
            int dots = 0;
            for (int k = 0; k < n && dots < 3; k++, dots++) {
                Person *p = person_get(((Event *)tbl_at(&T_event, idx[k]))->who);
                buf_puts(b, "<i");
                if (p) ui_hue_style(b, p->hue);
                buf_puts(b, "></i>");
            }
            for (int k = 0; k < ext.n && dots < 3; k++) {
                if (!ext_overlaps(&ext.items[k], day, add_days(day, 1))) continue;
                Person *p = person_get(ext.items[k].who);
                buf_puts(b, "<i class=\"plan-dot-ext\"");
                if (p) ui_hue_style(b, p->hue);
                buf_puts(b, "></i>");
                dots++;
            }
            buf_puts(b, "</i></a>");
        }
    }
    buf_puts(b, "</div></div>");
    agenda_free(&ext);
    buf_free(&u);
}

/* ---------------------------------------------------------------- modal */

static void event_modal(Ctx *c, Buf *b, const Event *e, int64_t day) {
    Buf close = {0};
    ctx_url(c, &close, "edit", NULL, "new", NULL, NULL);
    ui_modal(b, e ? T("Edit event") : T("New event"), buf_cstr(&close));
    ui_form(b, "plan.event_save", "stack");
    if (e) ui_hidden_int(b, "id", e->id);
    char date[11], until[11] = "", from[6] = "", to[6] = "";
    fmt_ymd(e ? e->start : day, date);
    if (e) {
        fmt_ymd(e->allday ? add_days(e->end, -1) : e->end, until);
        if (!e->allday) {
            fmt_hm(e->start, from);
            fmt_hm(e->end, to);
        }
    }
    H(b,
      "<label class=\"field\"><span>%s</span><input name=\"title\" required maxlength=\"110\" autocomplete=\"off\" "
      "value=\"%s\" autofocus></label>"
      "<div class=\"field-row\"><label class=\"field\"><span>%s</span><input type=\"date\" name=\"date\" required "
      "value=\"%s\"></label><label class=\"field\"><span>%s</span><input type=\"date\" name=\"until\" "
      "value=\"%s\"></label></div>"
      "<label class=\"plan-check\"><input type=\"checkbox\" name=\"allday\" value=\"1\"%r> %s</label>"
      "<div class=\"field-row\"><label class=\"field\"><span>%s</span><input type=\"time\" name=\"from\" "
      "value=\"%s\"></label><label class=\"field\"><span>%s</span><input type=\"time\" name=\"to\" "
      "value=\"%s\"></label></div><label class=\"field\"><span>%s</span>",
      T("What"), e ? e->title : "", T("Date"), date, T("Until"), until, e && e->allday ? " checked" : "", T("All day"),
      T("From"), from, T("To"), to, T("Who"));
    ui_person_select(b, "who", e ? e->who : 0, T("Everyone"));
    H(b,
      "</label><label class=\"field\"><span>%s</span><textarea name=\"note\" maxlength=\"190\" rows=\"2\">%s"
      "</textarea></label>",
      T("Note"), e ? e->note : "");
    const EventSeries *s = e && e->series ? tbl_get(&T_event_series, e->series) : NULL;
    if (!e) { /* a new one may repeat (issue #47); one of a series changes on its own */
        H(b, "<div class=\"field-row\"><label class=\"field\"><span>%s</span><select name=\"rep\">", T("Repeat"));
        static const char *const reps[] = {N_("Never"), N_("Every day"), N_("Every week"), N_("Every 2 weeks"),
                                           N_("Every month")};
        for (int i = 0; i < countof(reps); i++) H(b, "<option value=\"%d\">%s</option>", i, T(reps[i]));
        H(b, "</select></label><label class=\"field\"><span>%s</span><input type=\"date\" name=\"rep_until\"></label></div>",
          T("Repeat until"));
    } else if (s) {
        Buf lab = {0};
        plan_series_label(&lab, s);
        buf_puts(b, "<p class=\"muted small plan-series\">↻ ");
        H(b, T("Repeats %s. Saving changes this one only."), buf_cstr(&lab));
        buf_puts(b, "</p>");
        buf_free(&lab);
    }
    H(b, "<div class=\"modal-f\"><button class=\"btn primary\">%s</button></div>", T("Save"));
    ui_form_end(b);
    if (e) {
        ui_form(b, "plan.event_delete", "danger-zone"); /* no "Delete ...?": the toast offers Undo (issue #60) */
        ui_hidden_int(b, "id", e->id);
        buf_puts(b, "<button class=\"btn danger\">");
        ui_icon(b, "trash");
        H(b, "<span>%s</span></button>", s ? T("Delete this one") : T("Delete"));
        ui_form_end(b);
        if (s && !s->stopped) { /* the rest of a series: more than Undo can bring back, so asked first */
            ui_form_confirm(b, "plan.event_delete", "danger-zone", T("Delete this one and every later one?"));
            ui_hidden_int(b, "id", e->id);
            ui_hidden(b, "later", "1");
            H(b, "<button class=\"btn danger\"><span>%s</span></button>", T("Delete this and later ones"));
            ui_form_end(b);
        }
    }
    ui_modal_end(b);
    buf_free(&close);
}

/* ----------------------------------------------------------------- page */

void plan_cal_page(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Calendar"));
    Buf sub = {0};
    plan_subtitle(c, &sub);
    ui_head(b, T("Plan"), buf_cstr(&sub));
    plan_tabs(c, b, 2);
    ui_head_end(b);
    buf_free(&sub);

    int64_t sel = selected_day(c), today = day_start(c->now);
    DateTime sd = dt_local(sel);
    int year = sd.year, mon = sd.mon;
    int yy, mm, dd0;
    char probe[16];
    snprintf(probe, sizeof probe, "%.7s-01", ctx_q(c, "m")); /* "2026-11" -> "2026-11-01" */
    if (parse_ymd(probe, &yy, &mm, &dd0)) year = yy, mon = mm;

    buf_puts(b, "<div class=\"plan-cal\"><div class=\"plan-cal-side\">");
    ui_form(b, "plan.event_add", "quick plan-quick");
    H(b, "<div class=\"quick-row\"><input name=\"q\" required maxlength=\"200\" autocomplete=\"off\" "
         "enterkeyhint=\"done\" data-live=\"plan.event_preview\" data-key=\"/\" aria-label=\"%s\" placeholder=\"%s\">"
         "<button class=\"btn primary\" aria-label=\"%s\">", /* (on phones only the + shows: issue #65) */
      T("New event"), T("Add an event — “dentist tue 14:30 @adam”"), T("Add event"));
    ui_icon(b, "plus");
    H(b, "<span>%s</span></button></div>", T("Add"));
    preview(c, b);
    ui_form_end(b);
    mini_month(c, b, year, mon, sel);
    char ymd[11];
    fmt_ymd(sel, ymd);
    Buf u = {0};
    ctx_url(c, &u, "new", "1", "edit", NULL, "day", ymd, NULL);
    if (ui_can("plan.event_add")) { /* (a display account only looks: issue #96) */
        H(b, "<a class=\"btn plan-cal-new\" href=\"%s\">", buf_cstr(&u));
        ui_icon(b, "calendar");
        H(b, "<span>%s</span></a>", T("New event with details"));
    }
    buf_puts(b, "</div>");

    /* agenda: our events plus everything other modules put on the shared agenda */
    buf_puts(b, "<div class=\"plan-cal-main card\">");
    static int idx[MAX_EVENTS];
    Agenda ext = {0};
    agenda_collect(c, sel, add_days(sel, AGENDA_DAYS), "plan", &ext);
    int total = events_between(sel, add_days(sel, AGENDA_DAYS), idx, MAX_EVENTS) + ext.n;
    if (!total) {
        ui_empty(b, "📅", sel == today ? T("Nothing planned") : T("Nothing planned from here"),
                 T("Add things like “grandma visits fri-sun” or “dentist tue 14:30 @adam”."));
    }
    char ids[16];
    int last_week = -1;
    for (int i = 0; i < AGENDA_DAYS && total; i++) {
        int64_t day = add_days(sel, i);
        int n = events_between(day, add_days(day, 1), idx, MAX_EVENTS);
        int nx = ext_count(&ext, day, add_days(day, 1));
        if (!n && !nx && day != today) continue;
        DateTime dd = dt_local(day);
        int wk = iso_week(day, NULL);
        if (wk != last_week) {
            buf_puts(b, "<div class=\"plan-ag-week\">");
            H(b, T("Week %d"), wk);
            buf_puts(b, "</div>");
            last_week = wk;
        }
        fmt_ymd(day, ymd);
        H(b, "<section class=\"plan-ag-day%r\" id=\"plan-d%s\"><h3>", day == today ? " plan-today" : "", ymd);
        Buf label = {0};
        char date[32];
        ui_day(&label, day, c->now);
        snprintf(date, sizeof date, "%s %d %s", weekday_name(dd.wday, true), dd.day, month_name(dd.mon, true));
        html_esc(b, buf_cstr(&label));
        /* "Friday" + "Fri 16 Oct" (ui_day gives a Swedish or Spanish date its capital: "Fre 16 okt") */
        if (!str_ieq(buf_cstr(&label), date)) H(b, "<small>%s</small>", date);
        buf_puts(b, "</h3>");
        buf_free(&label);
        if (!n && !nx) {
            H(b, "<p class=\"muted plan-ag-none\">%s</p>", T("Nothing planned"));
        } else {
            buf_puts(b, "<ul class=\"plan-evs\">");
            for (int k = 0; k < n; k++) {
                Event *e = tbl_at(&T_event, idx[k]);
                snprintf(ids, sizeof ids, "%d", e->id);
                buf_reset(&u);
                ctx_url(c, &u, "edit", ids, "new", NULL, NULL);
                event_row(c, b, e, day, ui_can("plan.event_save") ? buf_cstr(&u) : NULL, "plan-e", false);
            }
            for (int k = 0; k < ext.n; k++)
                if (ext_overlaps(&ext.items[k], day, add_days(day, 1))) ext_row(b, &ext.items[k], day, ui_can("plan.event_save"));
            buf_puts(b, "</ul>");
        }
        buf_puts(b, "</section>");
    }
    buf_puts(b, "</div></div>");
    agenda_free(&ext);

    int edit = (int)ctx_qint(c, "edit", 0);
    Event *e = edit ? tbl_get(&T_event, edit) : NULL;
    if (e || str_eq(ctx_q(c, "new"), "1")) event_modal(c, b, e, sel);
    buf_free(&u);
}

/* --------------------------------------------------------------- widget */

int plan_today_events(Ctx *c, Buf *b, bool wall) {
    static int idx[MAX_EVENTS];
    int64_t today = day_start(c->now);
    int n = events_between(today, add_days(today, 1), idx, MAX_EVENTS);
    if (!n) return 0;
    buf_puts(b, "<ul class=\"plan-evs plan-compact\">");
    char href[48];
    for (int k = 0; k < n && k < 6; k++) {
        Event *e = tbl_at(&T_event, idx[k]);
        snprintf(href, sizeof href, "/plan/cal?edit=%d", e->id);
        event_row(c, b, e, today, wall || !c->me || !ui_can("plan.event_save") ? NULL : href, wall ? "plan-we" : "plan-he", false);
    }
    buf_puts(b, "</ul>");
    return n;
}

void plan_agenda(Ctx *c, int64_t from, int64_t to, Agenda *out) {
    if (is_guest(c)) return; /* (the household's plans aren't a guest's: issue #44) */
    static int idx[MAX_EVENTS];
    int n = events_between(from, to, idx, MAX_EVENTS);
    for (int k = 0; k < n; k++) {
        const Event *e = tbl_at(&T_event, idx[k]);
        AgendaItem it = {0};
        it.start = e->start;
        it.end = e->end;
        it.allday = e->allday;
        str_copy(it.title, sizeof it.title, e->title);
        str_copy(it.icon, sizeof it.icon, plan_task_icon(e->title));
        char ymd[11];
        fmt_ymd(e->start, ymd);
        snprintf(it.link, sizeof it.link, "/plan/cal?day=%s&edit=%d", ymd, e->id);
        it.who = e->who;
        str_copy(it.kind, sizeof it.kind, N_("Event")); /* (English: what it is - said where it's shown) */
        agenda_add(out, &it);
    }
    if (!out->todos) return;
    /* to-dos with a day (the calendar feed, issue #41): one still open after its day shows today, saying so */
    int64_t today = day_start(c->now);
    for (int i = 0; i < tbl_count(&T_task); i++) {
        const Task *t = tbl_at(&T_task, i);
        if (t->done_at || !t->due) continue;
        int64_t day = t->due < today ? today : t->due;
        if (day < from || day >= to) continue;
        AgendaItem it = {0};
        it.start = day;
        it.end = add_days(day, 1);
        it.allday = true;
        char since[48] = "";
        if (t->due < today) {
            DateTime d = dt_local(t->due);
            char due[44];
            snprintf(due, sizeof due, T("(due %s %d %s)"), weekday_name(d.wday, true), d.day, month_name(d.mon, true));
            snprintf(since, sizeof since, " %s", due);
        }
        str_copy(it.title, sizeof it.title - strlen(since), t->title); /* (a long title gives way, whole letters) */
        strcat(it.title, since);
        str_copy(it.icon, sizeof it.icon, plan_task_icon(t->title)[0] ? plan_task_icon(t->title) : "✅");
        snprintf(it.link, sizeof it.link, "/plan?edit=%d", t->id);
        it.who = t->who;
        str_copy(it.kind, sizeof it.kind, N_("To-do")); /* (core/feed.c tells to-dos by it) */
        agenda_add(out, &it);
    }
}

int plan_days(Ctx *c, Buf *b, int days, int max) {
    static int idx[MAX_EVENTS];
    int64_t today = day_start(c->now);
    int rows = 0;
    for (int d = 1; d <= days && rows < max; d++) {
        int64_t day = add_days(today, d), next = add_days(day, 1);
        int found = events_between(day, next, idx, MAX_EVENTS), n = 0, due = 0;
        for (int k = 0; k < found; k++) /* once, on its first day (one that began earlier is today's) */
            if (((const Event *)tbl_at(&T_event, idx[k]))->start >= day) idx[n++] = idx[k];
        for (int i = 0; i < tbl_count(&T_task); i++) {
            const Task *t = tbl_at(&T_task, i);
            due += !t->done_at && t->due && day_start(t->due) == day;
        }
        if (!n && !due) continue;
        DateTime dt = dt_local(day);
        char head[48];
        if (d == 1) str_copy(head, sizeof head, T("Tomorrow"));
        else snprintf(head, sizeof head, "%s %d", weekday_name(dt.wday, false), dt.day);
        plan_capitalise(head); /* (a heading: Swedish and Spanish weekdays are lower case) */
        H(b, "<h3 class=\"plan-day-h\" data-fit-head>%s</h3>", head); /* (a wall too short: wire.js) */
        buf_puts(b, "<ul class=\"plan-evs plan-compact\">");
        for (int k = 0; k < n && rows < max; k++, rows++) event_row(c, b, tbl_at(&T_event, idx[k]), day, NULL, "plan-wd", true);
        for (int i = 0; i < tbl_count(&T_task) && rows < max; i++) {
            const Task *t = tbl_at(&T_task, i);
            if (t->done_at || !t->due || day_start(t->due) != day) continue;
            Person *p = person_get(t->who);
            H(b, "<li class=\"plan-ev plan-day-task\" id=\"plan-wdt%d\"", t->id);
            if (p) ui_hue_style(b, p->hue);
            const char *icon = plan_task_icon(t->title);
            H(b, "><span class=\"plan-ev-time\">%s</span><div class=\"plan-ev-main\"><b>%s%r%s</b></div>", T("To do"), icon,
              icon[0] ? " " : "", t->title);
            if (p) {
                H(b, "<span class=\"plan-ev-who\"><span class=\"sr-only\">%s </span>", T("for"));
                ui_avatar(b, p, "sm");
                buf_puts(b, "</span>");
            }
            buf_puts(b, "</li>");
            rows++;
        }
        buf_puts(b, "</ul>");
    }
    return rows;
}

/* ---------------------------------------------------------------- actions */

static void close_modal(Ctx *c) {
    Buf u = {0};
    ctx_url(c, &u, "edit", NULL, "new", NULL, NULL);
    ctx_redirect(c, "%s", buf_cstr(&u));
    buf_free(&u);
}

static void announce(Ctx *c, const Event *e) {
    char when[96];
    when_describe(e->start, e->end, c->now, when, sizeof when);
    const EventSeries *s = e->series ? tbl_get(&T_event_series, e->series) : NULL;
    const char *icon = plan_task_icon(e->title);
    if (s) { /* (the repeat in the words of whoever added it: "every Wednesday") */
        Buf rep = {0};
        plan_series_label(&rep, s);
        activity_add(c->me->id, icon[0] ? icon : "📅", "/plan/cal", N_("%s added %s · %s, %s"), c->me->name, e->title, when,
                     buf_cstr(&rep));
        app_notify(c, TOAST_INFO, N_("%s added %s · %s, %s"), c->me->name, e->title, when, buf_cstr(&rep));
        buf_free(&rep);
    } else {
        activity_add(c->me->id, icon[0] ? icon : "📅", "/plan/cal", N_("%s added %s · %s"), c->me->name, e->title, when);
        app_notify(c, TOAST_INFO, N_("%s added %s · %s"), c->me->name, e->title, when);
    }
}

/* A repeating event (issue #47): the series, its first event (e, put here) and the ones ahead */
static void start_series(Ctx *c, Event *e, int repeat, int64_t until) {
    EventSeries s = {.start = e->start, .end = e->end, .allday = e->allday, .who = e->who, .repeat = repeat,
                     .until = until, .made = 1, .by = e->by, .created = c->now};
    str_copy(s.title, sizeof s.title, e->title);
    str_copy(s.note, sizeof s.note, e->note);
    tbl_put(&T_event_series, &s);
    e->series = s.id;
    tbl_put(&T_event, e);
    plan_series_fill(c->now);
}

void plan_act_event_preview(Ctx *c, Fields *f) { ctx_set_live(c, "plan.eq", fields_str(f, "q")); }

void plan_act_event_add(Ctx *c, Fields *f) {
    EventDraft d;
    char err[160];
    if (!plan_parse_event(fields_str(f, "q"), c->now, &d, err, sizeof err)) {
        ctx_fail(c, "%s", err);
        return;
    }
    Event e = {0};
    str_copy(e.title, sizeof e.title, d.title);
    e.start = d.start;
    e.end = d.end;
    e.allday = d.allday;
    e.who = d.who;
    e.by = c->me->id;
    e.created = c->now;
    if (d.repeat) start_series(c, &e, d.repeat, 0);
    else tbl_put(&T_event, &e);
    ctx_set_live(c, "plan.eq", "");
    char when[96];
    when_describe(e.start, e.end, c->now, when, sizeof when);
    if (d.repeat) {
        Buf rep = {0};
        plan_repeat_label(&rep, d.repeat, dt_local(e.start).wday + 1);
        ctx_ok(c, T("Added · %s, %s"), when, buf_cstr(&rep));
        buf_free(&rep);
    } else {
        ctx_ok(c, T("Added · %s"), when);
    }
    announce(c, &e);
}

void plan_act_event_save(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    Event e = {0};
    if (id) {
        Event *row = tbl_get(&T_event, id);
        if (!row) {
            ctx_fail(c, "%s", T("That event is gone"));
            return;
        }
        e = *row;
    } else {
        e.by = c->me->id;
        e.created = c->now;
    }
    char title[160];
    str_copy(title, sizeof title, fields_str(f, "title"));
    str_trim(title);
    if (!title[0]) {
        ctx_fail_at(c, "title", "%s", T("What's happening?"));
        return;
    }
    plan_capitalise(title);
    str_copy(e.title, sizeof e.title, title);
    int y, m, d, uy, um, ud;
    if (!parse_ymd(fields_str(f, "date"), &y, &m, &d)) {
        ctx_fail_at(c, "date", "%s", T("Pick a date"));
        return;
    }
    bool has_until = parse_ymd(fields_str(f, "until"), &uy, &um, &ud);
    if (!has_until) uy = y, um = m, ud = d;
    if (dt_make(uy, um, ud, 0, 0) < dt_make(y, m, d, 0, 0)) {
        ctx_fail_at(c, "until", "%s", T("“Until” is before the start"));
        return;
    }
    e.allday = fields_get(f, "allday") != NULL;
    if (e.allday) {
        e.start = dt_make(y, m, d, 0, 0);
        e.end = dt_make(uy, um, ud + 1, 0, 0);
    } else {
        int from, to;
        if (!parse_hm(fields_str(f, "from"), &from)) {
            ctx_fail_at(c, "from", "%s", T("Pick a start time (or tick “All day”)"));
            return;
        }
        e.start = dt_make(y, m, d, from / 60, from % 60);
        if (parse_hm(fields_str(f, "to"), &to)) {
            e.end = dt_make(uy, um, ud, to / 60, to % 60);
            if (e.end <= e.start) {
                if (has_until && (uy != y || um != m || ud != d)) {
                    ctx_fail_at(c, "to", "%s", T("It ends before it starts"));
                    return;
                }
                e.end = add_days(e.end, 1); /* 22:00–02:00 */
            }
        } else {
            e.end = e.start + 3600;
        }
    }
    int who = (int)fields_int(f, "who", 0);
    e.who = person_real(who) ? who : 0; /* pickers list real people only */
    str_copy(e.note, sizeof e.note, fields_str(f, "note"));
    str_trim(e.note);
    int rep = id ? 0 : (int)CLAMP(fields_int(f, "rep", 0), REPEAT_NONE, REPEAT_MONTHLY);
    int64_t rep_until = 0;
    if (rep && fields_str(f, "rep_until")[0]) {
        int ry, rm, rd;
        if (!parse_ymd(fields_str(f, "rep_until"), &ry, &rm, &rd) || dt_make(ry, rm, rd, 0, 0) < day_start(e.start)) {
            ctx_fail_at(c, "rep_until", "%s", T("Repeat until a day on or after the first one"));
            return;
        }
        rep_until = dt_make(ry, rm, rd, 0, 0);
    }
    if (rep) start_series(c, &e, rep, rep_until);
    else tbl_put(&T_event, &e);
    ctx_ok(c, "%s", id ? T("Saved") : T("Added"));
    if (!id) announce(c, &e);
    close_modal(c);
}

void plan_act_event_delete(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    Event *row = tbl_get(&T_event, id);
    if (row && row->series && str_eq(fields_str(f, "later"), "1")) { /* the rest of a repeating event */
        Event first = *row;
        EventSeries *sp = tbl_get(&T_event_series, first.series);
        int n = 0;
        for (int i = tbl_count(&T_event) - 1; i >= 0; i--) {
            const Event *x = tbl_at(&T_event, i);
            if (x->series != first.series || x->start < first.start) continue;
            tbl_del(&T_event, x->id);
            n++;
        }
        if (sp) {
            EventSeries s = *sp;
            s.stopped = true;
            tbl_put(&T_event_series, &s);
        }
        char when[96];
        when_describe(first.start, first.end, c->now, when, sizeof when);
        ctx_ok(c, T("Deleted %s from %s on (%d)"), first.title, when, n);
        activity_add(c->me->id, "🗓️", "/plan/cal", N_("%s stopped %s from %s on"), c->me->name, first.title, when);
        close_modal(c);
        return;
    }
    ctx_undo(c, &T_event, id); /* Undo rather than "Delete ...?" (issue #60) */
    if (!tbl_del(&T_event, id)) {
        ctx_fail(c, "%s", T("That event is gone"));
        return;
    }
    ctx_ok(c, "%s", T("Deleted"));
    close_modal(c);
}
