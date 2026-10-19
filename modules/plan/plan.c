/* modules/plan — the household planner: to-dos/chores, the shopping list and
 * a shared calendar. This file holds the data, the quick-add parsing and the
 * module glue; tasks.c, shop.c and cal.c render the three tabs. */
#include "plan.h"
#include "../../core/db.h"

/* ================================================================== data */

static const FieldDef task_fields[] = {
    FIELD(Task, id, F_INT),        FIELD(Task, title, F_STR),   FIELD(Task, who, F_INT),
    FIELD(Task, due, F_I64),       FIELD(Task, repeat, F_INT),  FIELD(Task, rep_day, F_INT),
    FIELD(Task, done_by, F_INT),   FIELD(Task, done_at, F_I64), FIELD(Task, by, F_INT),
    FIELD(Task, created, F_I64),   FIELD(Task, spawned, F_INT), FIELD(Task, turns, F_STR),
    FIELD(Task, turn, F_INT),      FIELD(Task, swap, F_INT),
};
Table T_task = TABLE("plan_task", Task, task_fields);

static const FieldDef item_fields[] = {
    FIELD(Item, id, F_INT), FIELD(Item, text, F_STR),       FIELD(Item, done, F_BOOL),
    FIELD(Item, by, F_INT), FIELD(Item, created, F_I64), FIELD(Item, done_at, F_I64),
};
Table T_item = TABLE("plan_item", Item, item_fields);

static const FieldDef bought_fields[] = {
    FIELD(Bought, id, F_INT),    FIELD(Bought, key, F_STR),  FIELD(Bought, text, F_STR),
    FIELD(Bought, count, F_INT), FIELD(Bought, last, F_I64),
};
Table T_bought = TABLE("plan_bought", Bought, bought_fields);

static const FieldDef event_fields[] = {
    FIELD(Event, id, F_INT),    FIELD(Event, title, F_STR), FIELD(Event, start, F_I64),
    FIELD(Event, end, F_I64),   FIELD(Event, allday, F_BOOL), FIELD(Event, who, F_INT),
    FIELD(Event, note, F_STR),  FIELD(Event, by, F_INT),    FIELD(Event, created, F_I64),
    FIELD(Event, series, F_INT),
};
Table T_event = TABLE("plan_event", Event, event_fields);

static const FieldDef series_fields[] = {
    FIELD(EventSeries, id, F_INT),      FIELD(EventSeries, title, F_STR),  FIELD(EventSeries, start, F_I64),
    FIELD(EventSeries, end, F_I64),     FIELD(EventSeries, allday, F_BOOL), FIELD(EventSeries, who, F_INT),
    FIELD(EventSeries, note, F_STR),    FIELD(EventSeries, repeat, F_INT), FIELD(EventSeries, until, F_I64),
    FIELD(EventSeries, made, F_INT),    FIELD(EventSeries, by, F_INT),     FIELD(EventSeries, created, F_I64),
    FIELD(EventSeries, stopped, F_BOOL),
};
Table T_event_series = TABLE("plan_event_series", EventSeries, series_fields);

/* =============================================================== helpers */

void plan_capitalise(char *s) {
    unsigned char *u = (unsigned char *)s;
    if (u[0] >= 'a' && u[0] <= 'z') u[0] = (unsigned char)(u[0] - 32);
    else if (u[0] == 0xC3 && u[1] >= 0xA0 && u[1] <= 0xBE && u[1] != 0xB7) u[1] = (unsigned char)(u[1] - 0x20);
}

/* Keyword (folded, matched at the start of a word) -> emoji. First match wins. */
static const struct {
    const char *kw, *icon;
} ICONS[] = {
    {"vacuum", "🧹"},     {"dammsug", "🧹"},   {"sweep", "🧹"},      {"sopa ", "🧹"},      {"plant", "🪴"},
    {"vattna", "🪴"},     {"blom", "🪴"},      {"trash", "🗑️"},      {"garbage", "🗑️"},    {"rubbish", "🗑️"},
    {"sopor", "🗑️"},      {"soporna", "🗑️"},   {"recycl", "♻️"},     {"pant ", "♻️"},      {"återvinn", "♻️"},
    {"laundry", "🧺"},    {"tvätt", "🧺"},     {"dishes", "🍽️"},     {"dishwasher", "🍽️"}, {"disk", "🍽️"},
    {"diska", "🍽️"},      {"mop", "🧽"},       {"clean", "🧽"},      {"städ", "🧽"},       {"wipe", "🧽"},
    {"torka", "🧽"},      {"dog", "🐕"},       {"hund", "🐕"},       {"cat ", "🐈"},       {"cats", "🐈"},
    {"katt", "🐈"},       {"vet ", "🐾"},      {"veterinär", "🐾"},  {"cook", "🍳"},       {"dinner", "🍳"},
    {"middag", "🍳"},     {"laga mat", "🍳"},  {"lunch", "🥪"},      {"breakfast", "🥐"},  {"frukost", "🥐"},
    {"bake", "🧁"},       {"baka", "🧁"},      {"cake", "🍰"},       {"tårta", "🍰"},      {"groceries", "🛒"},
    {"shop", "🛒"},       {"buy", "🛒"},       {"handla", "🛒"},     {"köp", "🛒"},        {"bathroom", "🛁"},
    {"badrum", "🛁"},     {"toilet", "🚽"},    {"toalett", "🚽"},    {"sheets", "🛏️"},     {"bed", "🛏️"},
    {"bädda", "🛏️"},      {"sängkläder", "🛏️"}, {"call", "📞"},      {"ring ", "📞"},      {"phone", "📞"},
    {"pay", "💸"},        {"bill", "💸"},      {"invoice", "💸"},    {"betala", "💸"},     {"räkning", "💸"},
    {"faktura", "💸"},    {"tyre", "🛞"},      {"tire", "🛞"},       {"däck", "🛞"},       {"car ", "🚗"},
    {"cars", "🚗"},       {"bil ", "🚗"},      {"bilen", "🚗"},      {"homework", "📚"},   {"läx", "📚"},
    {"study", "📚"},      {"plugga", "📚"},    {"mail", "✉️"},       {"post ", "✉️"},      {"posten", "✉️"},
    {"letter", "✉️"},     {"brev", "✉️"},
    {"package", "📦"},    {"paket", "📦"},     {"mow", "🌱"},        {"lawn", "🌱"},       {"gräs", "🌱"},
    {"garden", "🌱"},     {"trädgård", "🌱"},  {"rake", "🍂"},       {"leaves", "🍂"},     {"löv", "🍂"},
    {"snow", "❄️"},       {"snö", "❄️"},       {"fix", "🔧"},        {"repair", "🔧"},     {"laga", "🔧"},
    {"bulb", "💡"},       {"lamp", "💡"},      {"dentist", "🦷"},    {"tandläkare", "🦷"}, {"doctor", "🩺"},
    {"läkare", "🩺"},     {"vårdcentral", "🩺"}, {"medicine", "💊"},  {"medicin", "💊"},    {"birthday", "🎂"},
    {"födelsedag", "🎂"}, {"kalas", "🎉"},     {"party", "🎉"},      {"fest", "🎉"},       {"gym", "🏋️"},
    {"run ", "🏃"},       {"running", "🏃"},   {"löpning", "🏃"},    {"football", "⚽"},   {"fotboll", "⚽"},
    {"swim", "🏊"},       {"simma", "🏊"},     {"simning", "🏊"},    {"flight", "✈️"},     {"flyg", "✈️"},
    {"travel", "🧳"},     {"pack", "🧳"},      {"resa", "🧳"},       {"school", "🏫"},     {"skola", "🏫"},
    {"meeting", "👥"},    {"möte", "👥"},      {"haircut", "💇"},    {"frisör", "💇"},     {"movie", "🎬"},
    {"bio ", "🎬"},       {"film", "🎬"},      {"concert", "🎵"},    {"konsert", "🎵"},    {"grandma", "👵"},
    {"mormor", "👵"},     {"farmor", "👵"},    {"grandpa", "👴"},    {"morfar", "👴"},     {"farfar", "👴"},
};

const char *plan_task_icon(const char *title) {
    char low[160];
    fold_copy(low, sizeof low - 1, title);
    size_t n = strlen(low);
    low[n] = ' '; /* lets keywords with a trailing space match at the very end */
    low[n + 1] = '\0';
    for (int i = 0; i < countof(ICONS); i++) {
        for (const char *p = strstr(low, ICONS[i].kw); p; p = strstr(p + 1, ICONS[i].kw)) {
            if (p == low || p[-1] == ' ' || p[-1] == '-' || p[-1] == '(') return ICONS[i].icon;
        }
    }
    return "";
}

void plan_repeat_label(Buf *b, int repeat, int rep_day) {
    switch (repeat) {
    case REPEAT_DAILY: buf_puts(b, "every day"); break;
    case REPEAT_WEEKLY:
        if (rep_day >= 1 && rep_day <= 7) buf_printf(b, "every %s", weekday_name(rep_day - 1, false));
        else buf_puts(b, "every week");
        break;
    case REPEAT_BIWEEKLY:
        if (rep_day >= 1 && rep_day <= 7) buf_printf(b, "every other %s", weekday_name(rep_day - 1, false));
        else buf_puts(b, "every 2 weeks");
        break;
    case REPEAT_MONTHLY: buf_puts(b, "every month"); break;
    default: break;
    }
}

int plan_mine(const Ctx *c) { return is_child(c) ? c->me->id : 0; }

void plan_tabs(Ctx *c, Buf *b, int active) {
    static const char *hrefs[] = {"/plan", "/plan/shop", "/plan/cal"};
    static const char *labels[] = {"To-dos", "Shopping", "Calendar"};
    if (is_guest(c)) return; /* (only the shopping list: issue #44) */
    int counts[3] = {plan_open_tasks(plan_mine(c)), plan_open_items(), 0};
    buf_puts(b, "<nav class=\"seg plan-tabs\" aria-label=\"Plan sections\">");
    for (int i = 0; i < 3; i++) {
        H(b, "<a href=\"%s\"%r>%s", hrefs[i], i == active ? " class=\"on\" aria-current=\"page\"" : "", labels[i]);
        if (counts[i]) H(b, " <span class=\"plan-n\">%d</span>", counts[i]);
        buf_puts(b, "</a>");
    }
    buf_puts(b, "</nav>");
}

void plan_subtitle(Ctx *c, Buf *b) {
    int t = is_guest(c) ? 0 : plan_open_tasks(plan_mine(c)), s = plan_open_items();
    if (!t && !s) {
        buf_puts(b, "All clear — nothing waiting");
        return;
    }
    if (t) buf_printf(b, "%d to-do%s", t, t == 1 ? "" : "s");
    if (t && s) buf_puts(b, " · ");
    if (s) buf_printf(b, "%d on the shopping list", s);
}

/* =============================================================== parsing */

/* Everyone named with an @, in order: "@anna @erik", "@anna, @erik" or "@anna,@erik" (one word) - up to max,
   once each. The first is who it's for; more take turns (a chore that repeats). Returns how many. */
static int claim_people(Tokens *t, int *who, int *all, int max) {
    int n = 0;
    for (int i = 0; i < t->n; i++) {
        if (t->used[i] || t->raw[i][0] != '@' || !t->raw[i][1]) continue;
        char word[TOK_LEN];
        str_copy(word, sizeof word, t->raw[i]);
        bool any = false;
        for (char *part = strtok(word, ","); part; part = strtok(NULL, ",")) {
            while (*part == '@' || *part == ' ') part++;
            Person *p = *part ? person_find(part) : NULL;
            if (!p) continue;
            any = true;
            bool dup = false;
            for (int k = 0; k < n; k++) dup |= all[k] == p->id;
            if (!dup && n < max) all[n++] = p->id;
        }
        if (any) t->used[i] = true;
    }
    if (n) *who = all[0];
    return n;
}

bool plan_parse_task(const char *text, int64_t now, TaskDraft *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    Tokens t;
    tok_split(&t, text ? text : "");
    out->nturns = claim_people(&t, &out->who, out->turns, PLAN_TURNS);
    When w;
    bool when = when_parse(&t, now, &w);
    if (!when && w.refused[0]) { /* "pay rent 31 sep": not an undated to-do called that */
        snprintf(err, errcap, "%s", w.refused);
        return false;
    }
    if (when) {
        out->repeat = w.repeat;
        bool dated = w.has_date || w.has_end_date || w.has_start || w.now || w.rel_min;
        int64_t s, e;
        if (dated && when_resolve(&w, now, 0, &s, &e)) out->due = day_start(s);
        else if (w.repeat) out->due = day_start(now);
        if ((w.repeat == REPEAT_WEEKLY || w.repeat == REPEAT_BIWEEKLY) && w.repeat_wday >= 0)
            out->rep_day = w.repeat_wday + 1;
        if (w.repeat == REPEAT_MONTHLY && out->due) out->rep_day = dt_local(out->due).day;
    }
    tok_rest(&t, out->title, sizeof out->title);
    str_trim(out->title);
    if (!out->title[0]) {
        snprintf(err, errcap, "What needs doing?");
        return false;
    }
    plan_capitalise(out->title);
    return true;
}

bool plan_parse_event(const char *text, int64_t now, EventDraft *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    Tokens t;
    tok_split(&t, text ? text : "");
    int people[PLAN_TURNS];
    claim_people(&t, &out->who, people, PLAN_TURNS);
    When w;
    bool timed = when_parse(&t, now, &w);
    if (timed) out->repeat = w.repeat;
    tok_rest(&t, out->title, sizeof out->title);
    str_trim(out->title);
    if (!out->title[0]) {
        snprintf(err, errcap, "What's happening?");
        return false;
    }
    plan_capitalise(out->title);
    int64_t s = 0, e = 0;
    if (!timed || !when_resolve(&w, now, 60, &s, &e)) {
        if (w.refused[0]) snprintf(err, errcap, "%s", w.refused); /* "31 sep", an end before the start */
        else snprintf(err, errcap, "When? Try “fri 18:00” or “sat-sun”");
        return false;
    }
    out->allday = !w.has_start && !w.now && !w.rel_min && !w.has_end && !w.dur_min;
    if (out->allday) {
        s = day_start(s);
        e = day_start(e);
        if (e <= s) e = add_days(s, 1);
    }
    if (e <= s) e = s + 3600;
    out->start = s;
    out->end = e;
    return true;
}

int plan_split_items(const char *text, char out[][100], int max) {
    int n = 0;
    const char *p = text ? text : "";
    while (*p && n < max) {
        size_t len = strcspn(p, ",;\n");
        char item[256];
        str_ncopy(item, sizeof item, p, len);
        /* collapse whitespace */
        char clean[256];
        size_t o = 0;
        bool space = false;
        for (const char *q = item; *q && o < sizeof clean - 1; q++) {
            if (*q == ' ' || *q == '\t' || *q == '\r') {
                space = o > 0;
                continue;
            }
            if (space && o < sizeof clean - 2) clean[o++] = ' ';
            space = false;
            clean[o++] = *q;
        }
        clean[o] = '\0';
        if (clean[0]) {
            str_copy(out[n], 100, clean);
            plan_capitalise(out[n]);
            bool dup = false;
            char a[100], b[100];
            fold_copy(a, sizeof a, out[n]);
            for (int k = 0; k < n && !dup; k++) {
                fold_copy(b, sizeof b, out[k]);
                dup = str_eq(a, b);
            }
            if (!dup) n++;
        }
        p += len;
        if (*p) p++;
    }
    return n;
}

/* ========================================================= repeating tasks */

static int64_t repeat_step(int64_t d, const Task *t) {
    switch (t->repeat) {
    case REPEAT_DAILY: return add_days(d, 1);
    case REPEAT_WEEKLY:
    case REPEAT_BIWEEKLY: {
        int64_t x = add_days(d, t->repeat == REPEAT_WEEKLY ? 7 : 14);
        if (t->rep_day >= 1 && t->rep_day <= 7) {
            x = add_days(x, (t->rep_day - 1) - dt_local(x).wday); /* snap to the chosen weekday */
            if (x <= d) x = add_days(x, 7);
        }
        return x;
    }
    case REPEAT_MONTHLY: {
        DateTime dt = dt_local(d);
        int anchor = t->rep_day >= 1 && t->rep_day <= 31 ? t->rep_day : dt.day;
        int y = dt.year, m = dt.mon + 1;
        if (m > 12) m = 1, y++;
        return dt_make(y, m, MIN(anchor, days_in_month(y, m)), 0, 0);
    }
    default: return add_days(d, 1);
    }
}

int64_t plan_next_due(const Task *t, int64_t now) {
    int64_t today = day_start(now);
    int64_t next = t->due ? day_start(t->due) : today;
    for (int guard = 0; guard < 5000; guard++) {
        next = repeat_step(next, t);
        if (next > today) return next;
    }
    return add_days(today, 1);
}

/* ============================================================ taking turns */

int plan_turn_ids(const Task *t, int out[PLAN_TURNS]) {
    int n = 0;
    for (const char *p = t->turns; *p && n < PLAN_TURNS;) {
        int id = atoi(p);
        if (id > 0) out[n++] = id;
        p += strcspn(p, ",");
        if (*p) p++;
    }
    return n;
}

int plan_next_turn(const Task *t, int *place) {
    int ids[PLAN_TURNS], n = plan_turn_ids(t, ids);
    if (n < 2) return 0;
    for (int k = 1; k <= n; k++) { /* the next place in the line with someone still in the household */
        int q = (t->turn + k) % n;
        if (!person_real(ids[q])) continue;
        *place = q;
        return person_real(t->swap) ? t->swap : ids[q];
    }
    return 0;
}

void plan_turn_names(Buf *b, const Task *t) {
    int ids[PLAN_TURNS], n = plan_turn_ids(t, ids), shown = 0;
    for (int k = 0; k < n; k++) {
        Person *p = person_real(ids[k]);
        if (p) buf_printf(b, "%s%s", shown++ ? ", " : "", p->name);
    }
}

/* ======================================================== repeating events */

#define SERIES_AHEAD_DAYS 400 /* the phone's calendar shows a year ahead */
#define SERIES_AHEAD_DAILY 60 /* ... but a daily one isn't made 400 times */

static int64_t month_shift(int64_t t, int months) {
    DateTime d = dt_local(t);
    int y = d.year, m = d.mon - 1 + months;
    y += m / 12;
    m = m % 12 + 1;
    return dt_make(y, m, MIN(d.day, days_in_month(y, m)), d.hour, d.min);
}

int64_t plan_series_at(const EventSeries *s, int k, int64_t *end) {
    int step = s->repeat == REPEAT_DAILY ? 1 : s->repeat == REPEAT_BIWEEKLY ? 14 : 7;
    if (s->repeat == REPEAT_MONTHLY) {
        int64_t a = month_shift(s->start, k);
        *end = s->allday ? add_days(a, (int)((s->end - s->start + 43200) / 86400)) : a + (s->end - s->start);
        return a;
    }
    *end = add_days(s->end, step * k);
    return add_days(s->start, step * k);
}

int plan_series_fill(int64_t now) {
    int made = 0;
    for (int i = 0; i < tbl_count(&T_event_series); i++) {
        EventSeries s = *(EventSeries *)tbl_at(&T_event_series, i);
        if (s.stopped || s.repeat == REPEAT_NONE) continue;
        int64_t horizon = add_days(day_start(now), s.repeat == REPEAT_DAILY ? SERIES_AHEAD_DAILY : SERIES_AHEAD_DAYS);
        bool changed = false;
        for (int guard = 0; guard < 500; guard++) {
            int64_t e, st = plan_series_at(&s, s.made, &e);
            if (s.until && day_start(st) > s.until) {
                s.stopped = true; /* (its last one is made: nothing more to come) */
                changed = true;
                break;
            }
            if (st >= horizon) break;
            s.made++;
            changed = true;
            if (e <= now) continue; /* (a series that missed its time: no events in the past) */
            Event ev = {.start = st, .end = e, .allday = s.allday, .who = s.who, .by = s.by, .created = now,
                        .series = s.id};
            str_copy(ev.title, sizeof ev.title, s.title);
            str_copy(ev.note, sizeof ev.note, s.note);
            tbl_put(&T_event, &ev);
            made++;
        }
        if (changed) tbl_put(&T_event_series, &s);
    }
    return made;
}

static const char *ordinal(int d) { /* 1st 2nd 3rd 4th ... 11th 12th 13th ... 21st */
    if (d % 100 >= 11 && d % 100 <= 13) return "th";
    return d % 10 == 1 ? "st" : d % 10 == 2 ? "nd" : d % 10 == 3 ? "rd" : "th";
}

void plan_series_label(Buf *b, const EventSeries *s) {
    int wday = dt_local(s->start).wday;
    switch (s->repeat) {
    case REPEAT_DAILY: buf_puts(b, "every day"); break;
    case REPEAT_WEEKLY: buf_printf(b, "every %s", weekday_name(wday, false)); break;
    case REPEAT_BIWEEKLY: buf_printf(b, "every other %s", weekday_name(wday, false)); break;
    case REPEAT_MONTHLY: {
        int d = dt_local(s->start).day;
        buf_printf(b, "every month on the %d%s", d, ordinal(d));
        break;
    }
    default: break;
    }
    if (s->stopped) buf_puts(b, ", stopped");
    else if (s->until) {
        DateTime u = dt_local(s->until);
        buf_printf(b, ", until %d %s", u.day, month_name(u.mon, true));
    }
}

int plan_task_group(const Task *t, int64_t now) {
    if (!t->due) return GRP_SOMEDAY;
    int64_t today = day_start(now), due = day_start(t->due);
    if (due < today) return GRP_OVERDUE;
    if (due == today) return GRP_TODAY;
    if (due == add_days(today, 1)) return GRP_TOMORROW;
    if (due < add_days(week_start(now), 7)) return GRP_WEEK;
    return GRP_LATER;
}

int plan_open_tasks(int who) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_task); i++) {
        Task *t = tbl_at(&T_task, i);
        if (!t->done_at && (!who || t->who == who)) n++;
    }
    return n;
}

int plan_open_items(void) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_item); i++) n += !((Item *)tbl_at(&T_item, i))->done;
    return n;
}

/* ================================================================ module */

static void init(void) {
    T_task.what = "to-dos";
    store_register(&T_task);
    T_item.what = "shopping list items";
    store_register(&T_item);
    T_bought.what = "shopping history";
    store_register(&T_bought);
    T_event.what = "calendar events";
    store_register(&T_event);
    T_event_series.what = "repeating events";
    store_register(&T_event_series);
}

static void start(void) {
    /* forget finished chores after three months and events after a year */
    int64_t now = now_sec();
    for (int i = tbl_count(&T_task) - 1; i >= 0; i--) {
        Task *t = tbl_at(&T_task, i);
        if (t->done_at && t->done_at < now - 90 * 86400) tbl_del(&T_task, t->id);
    }
    for (int i = tbl_count(&T_event) - 1; i >= 0; i--) {
        Event *e = tbl_at(&T_event, i);
        if (e->end < now - 365 * 86400) tbl_del(&T_event, e->id);
    }
    db_begin();
    plan_series_fill(now); /* (the days passed while Hemma was off) */
    db_commit();
}

/* repeating events: a day's worth more, an hour at a time */
static int64_t g_filled_ms;
static void tick(int64_t now_ms) {
    if (g_filled_ms && now_ms - g_filled_ms < 3600 * 1000) return;
    g_filled_ms = now_ms;
    db_begin();
    if (plan_series_fill(now_sec())) app_changed();
    db_commit();
}

static void page(Ctx *c, Buf *b) {
    if (str_starts(c->path, "/plan/shop") || is_guest(c)) plan_shop_page(c, b); /* (a guest: only that - #44) */
    else if (str_starts(c->path, "/plan/cal")) plan_cal_page(c, b);
    else plan_tasks_page(c, b);
}

static void widget(Ctx *c, Buf *b, int where) {
    (void)where;
    if (is_guest(c)) { /* (the household's day isn't a guest's: the list only) */
        plan_shop_widget(c, b);
        return;
    }
    buf_puts(b, "<section class=\"card plan-w\"><header class=\"card-h\"><h2>Today</h2><a class=\"more\" "
                "href=\"/plan\">Plan</a></header>");
    int n = plan_today_events(c, b, false);
    n += plan_today_tasks(c, b, false);
    if (!n) buf_puts(b, "<p class=\"muted plan-w-empty\">Nothing due today ✨</p>");
    buf_puts(b, "</section>");
    plan_shop_widget(c, b);
}

/* ---------------------------------------------------- blocks for walls */

/* A wall shows, it doesn't tick off: no forms, no edit links (the plan-wall look: big type).
   Nothing to show = false: the wall leaves the card out. */
static bool wall_card(Buf *b, Buf *in, int n) {
    if (n) H(b, "<div class=\"plan-wall\">%r</div>", buf_cstr(in));
    buf_free(in);
    return n > 0;
}

static bool row_today(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)kind, (void)arg;
    Buf t = {0};
    int n = plan_today_events(c, &t, true);
    n += plan_today_tasks(c, &t, true);
    return wall_card(b, &t, n);
}

static bool row_days(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)kind, (void)arg;
    Buf t = {0};
    int n = plan_days(c, &t, 7, 8);
    return wall_card(b, &t, n);
}

static bool row_shop(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)c, (void)kind, (void)arg;
    Buf t = {0};
    int n = plan_shop_items(&t, 14);
    return wall_card(b, &t, n);
}

static const Block blocks[] = {
    {"plan.today", "Today", SCREEN_WALL, row_today, NULL},
    {"plan.days", "Coming days", SCREEN_WALL, row_days, NULL},
    {"plan.shop", "Shopping list", SCREEN_WALL, row_shop, NULL},
    {0},
};

/* Issue #44: a child looks after their own to-dos (tasks.c checks whose) and the shopping list; a guest the
   shopping list. The calendar's events and swapping turns are the grown-ups'. */
static const Action actions[] = {
    {"plan.task_preview", plan_act_task_preview, true, .may = ACT_CHILD},
    {"plan.task_add", plan_act_task_add, .may = ACT_CHILD},
    {"plan.task_done", plan_act_task_done, .may = ACT_CHILD},
    {"plan.task_save", plan_act_task_save, .may = ACT_CHILD},
    {"plan.task_delete", plan_act_task_delete, .may = ACT_CHILD},
    {"plan.task_swap", plan_act_task_swap},
    {"plan.shop_preview", plan_act_shop_preview, true, .may = ACT_ALL},
    {"plan.shop_add", plan_act_shop_add, .may = ACT_ALL},
    {"plan.shop_toggle", plan_act_shop_toggle, .may = ACT_ALL},
    {"plan.shop_delete", plan_act_shop_delete, .may = ACT_ALL},
    {"plan.shop_clear", plan_act_shop_clear, .may = ACT_ALL},
    {"plan.event_preview", plan_act_event_preview, true},
    {"plan.event_add", plan_act_event_add},
    {"plan.event_save", plan_act_event_save},
    {"plan.event_delete", plan_act_event_delete},
    {0},
};

/* Someone leaves (issue #45): their open to-dos go to the next one in line (taking turns) or to anyone, and
   their events still to come go; erasing, their done to-dos and past events go too, and what they added for
   everyone keeps no name. */
static int forget(Forget *f) {
    int anyone = 0, lines = 0, done = 0, ahead = 0, past = 0, series = 0, made = 0, n = 0;
    for (int i = tbl_count(&T_task) - 1; i >= 0; i--) {
        Task t = *(Task *)tbl_at(&T_task, i);
        if (t.done_at && t.who == f->person && !f->keep) {
            done++;
            if (f->apply) tbl_del(&T_task, t.id);
            continue;
        }
        bool changed = false;
        if (plan_drop_turn(&t, f->person)) {
            changed = true;
            lines += !t.done_at;
        }
        if (!t.done_at && t.who == f->person) { /* (not taking turns, or nobody else left in line) */
            t.who = 0;
            anyone++;
            changed = true;
        }
        if (t.swap == f->person) t.swap = 0, changed = true;
        if (!f->keep && t.done_by == f->person) t.done_by = 0, changed = true;
        if (!f->keep && t.by == f->person) t.by = 0, changed = true, made++;
        if (changed && f->apply) tbl_put(&T_task, &t);
        n += changed;
    }
    for (int i = tbl_count(&T_event) - 1; i >= 0; i--) {
        Event e = *(Event *)tbl_at(&T_event, i);
        bool future = e.end > f->now;
        if (e.who == f->person && (future || !f->keep)) {
            if (future) ahead++;
            else past++;
            if (f->apply) tbl_del(&T_event, e.id);
            continue;
        }
        if (!f->keep && e.by == f->person) {
            made++;
            e.by = 0;
            if (f->apply) tbl_put(&T_event, &e);
        }
    }
    for (int i = tbl_count(&T_event_series) - 1; i >= 0; i--) {
        EventSeries s = *(EventSeries *)tbl_at(&T_event_series, i);
        if (s.who == f->person) {
            series += !s.stopped;
            s.stopped = true;
            if (f->apply && f->keep) tbl_put(&T_event_series, &s);
            else if (f->apply) tbl_del(&T_event_series, s.id);
        } else if (!f->keep && s.by == f->person && f->apply) {
            s.by = 0;
            tbl_put(&T_event_series, &s);
        }
    }
    for (int i = 0; !f->keep && i < tbl_count(&T_item); i++) {
        Item it = *(Item *)tbl_at(&T_item, i);
        if (it.by != f->person) continue;
        made++;
        it.by = 0;
        if (f->apply) tbl_put(&T_item, &it);
    }
    if (anyone) forget_note(f, "%d open to-do%s of theirs go%s to anyone", anyone, anyone == 1 ? "" : "s", anyone == 1 ? "es" : "");
    if (lines) forget_note(f, "%d chore%s taking turns go%s on without them", lines, lines == 1 ? "" : "s", lines == 1 ? "es" : "");
    if (ahead) forget_note(f, "%d event%s of theirs still to come %s deleted", ahead, ahead == 1 ? "" : "s", ahead == 1 ? "is" : "are");
    if (series) forget_note(f, "%d repeating event%s of theirs stop%s", series, series == 1 ? "" : "s", series == 1 ? "s" : "");
    if (done) forget_note(f, "%d done to-do%s of theirs %s deleted", done, done == 1 ? "" : "s", done == 1 ? "is" : "are");
    if (past) forget_note(f, "%d past event%s of theirs %s deleted", past, past == 1 ? "" : "s", past == 1 ? "is" : "are");
    if (made) forget_note(f, "%d thing%s they added for everyone stay%s, without their name", made, made == 1 ? "" : "s",
                          made == 1 ? "s" : "");
    return n + done + ahead + past + series + made;
}

/* Issue #39: a chore's day and an all-day event's days are kept as local midnights; a timed event's times
   were typed. (Shopping keeps only moments.) */
static int rezone(Rezone *z) {
    int n = 0;
    for (int i = 0; z->days && i < tbl_count(&T_task); i++) {
        Task t = *(Task *)tbl_at(&T_task, i);
        if (!rezone_day(z, &t.due)) continue;
        n++;
        if (z->apply) tbl_put(&T_task, &t);
    }
    for (int i = 0; i < tbl_count(&T_event); i++) {
        Event e = *(Event *)tbl_at(&T_event, i);
        int64_t was = e.start;
        /* one a series made counts as typed when the series was (made at a start after the change) */
        const EventSeries *from = e.series ? tbl_get(&T_event_series, e.series) : NULL;
        int64_t typed = from && from->created < e.created ? from->created : e.created;
        if (!rezone_span(z, e.allday, typed, &e.start, &e.end)) continue;
        n++;
        if (z->apply) tbl_put(&T_event, &e);
        else if (!z->days) rezone_note(z, e.title, was, e.start);
    }
    for (int i = 0; i < tbl_count(&T_event_series); i++) { /* what the events still to be made follow (#47) */
        EventSeries s = *(EventSeries *)tbl_at(&T_event_series, i);
        bool moved = rezone_span(z, s.allday, s.created, &s.start, &s.end);
        moved |= z->days && rezone_day(z, &s.until);
        if (moved && z->apply) tbl_put(&T_event_series, &s); /* (not counted: no event of its own) */
    }
    return n;
}

static const Module plan = {
    .id = "plan",
    .title = "Plan",
    .icon = "list",
    .nav = 3,
    .init = init,
    .start = start,
    .tick = tick,
    .page = page,
    .actions = actions,
    .widget = widget,
    .widget_order = 20,
    .agenda = plan_agenda,
    .rezone = rezone,
    .forget = forget,
    .blocks = blocks,
};
MODULE(plan)
