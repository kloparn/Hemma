/* modules/plan — the household planner: to-dos/chores, the shopping list and
 * a shared calendar. This file holds the data, the quick-add parsing and the
 * module glue; tasks.c, shop.c and cal.c render the three tabs. */
#include "plan.h"

/* ================================================================== data */

static const FieldDef task_fields[] = {
    FIELD(Task, id, F_INT),        FIELD(Task, title, F_STR),   FIELD(Task, who, F_INT),
    FIELD(Task, due, F_I64),       FIELD(Task, repeat, F_INT),  FIELD(Task, rep_day, F_INT),
    FIELD(Task, done_by, F_INT),   FIELD(Task, done_at, F_I64), FIELD(Task, by, F_INT),
    FIELD(Task, created, F_I64),   FIELD(Task, spawned, F_INT),
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
};
Table T_event = TABLE("plan_event", Event, event_fields);

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

void plan_tabs(Buf *b, int active) {
    static const char *hrefs[] = {"/plan", "/plan/shop", "/plan/cal"};
    static const char *labels[] = {"To-dos", "Shopping", "Calendar"};
    int counts[3] = {plan_open_tasks(0), plan_open_items(), 0};
    buf_puts(b, "<nav class=\"seg plan-tabs\" aria-label=\"Plan sections\">");
    for (int i = 0; i < 3; i++) {
        H(b, "<a href=\"%s\"%r>%s", hrefs[i], i == active ? " class=\"on\" aria-current=\"page\"" : "", labels[i]);
        if (counts[i]) H(b, " <span class=\"plan-n\">%d</span>", counts[i]);
        buf_puts(b, "</a>");
    }
    buf_puts(b, "</nav>");
}

void plan_subtitle(Buf *b) {
    int t = plan_open_tasks(0), s = plan_open_items();
    if (!t && !s) {
        buf_puts(b, "All clear — nothing waiting");
        return;
    }
    if (t) buf_printf(b, "%d to-do%s", t, t == 1 ? "" : "s");
    if (t && s) buf_puts(b, " · ");
    if (s) buf_printf(b, "%d on the shopping list", s);
}

/* =============================================================== parsing */

static void claim_people(Tokens *t, int *who) {
    for (int i = 0; i < t->n; i++) {
        if (t->used[i] || t->raw[i][0] != '@' || !t->raw[i][1]) continue;
        Person *p = person_find(t->raw[i] + 1);
        if (p) {
            *who = p->id;
            t->used[i] = true;
        }
    }
}

bool plan_parse_task(const char *text, int64_t now, TaskDraft *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    Tokens t;
    tok_split(&t, text ? text : "");
    claim_people(&t, &out->who);
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
    claim_people(&t, &out->who);
    When w;
    bool timed = when_parse(&t, now, &w);
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
}

static void page(Ctx *c, Buf *b) {
    if (str_starts(c->path, "/plan/shop")) plan_shop_page(c, b);
    else if (str_starts(c->path, "/plan/cal")) plan_cal_page(c, b);
    else plan_tasks_page(c, b);
}

static void widget(Ctx *c, Buf *b, int where) {
    (void)where;
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

static const Action actions[] = {
    {"plan.task_preview", plan_act_task_preview, true},   {"plan.task_add", plan_act_task_add},
    {"plan.task_done", plan_act_task_done},         {"plan.task_save", plan_act_task_save},
    {"plan.task_delete", plan_act_task_delete},     {"plan.shop_preview", plan_act_shop_preview, true},
    {"plan.shop_add", plan_act_shop_add},           {"plan.shop_toggle", plan_act_shop_toggle},
    {"plan.shop_delete", plan_act_shop_delete},     {"plan.shop_clear", plan_act_shop_clear},
    {"plan.event_preview", plan_act_event_preview, true}, {"plan.event_add", plan_act_event_add},
    {"plan.event_save", plan_act_event_save},       {"plan.event_delete", plan_act_event_delete},
    {0},
};

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
        if (!rezone_span(z, e.allday, e.created, &e.start, &e.end)) continue;
        n++;
        if (z->apply) tbl_put(&T_event, &e);
        else if (!z->days) rezone_note(z, e.title, was, e.start);
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
    .page = page,
    .actions = actions,
    .widget = widget,
    .widget_order = 20,
    .agenda = plan_agenda,
    .rezone = rezone,
    .blocks = blocks,
};
MODULE(plan)
