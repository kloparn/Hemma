/* modules/plan/tasks.c — the to-do / chores tab (/plan). */
#include "plan.h"

#define MAX_LIST 2048

static const char *const GROUP_NAMES[GRP_COUNT] = {N_("Overdue"), N_("Today"), N_("Tomorrow"),
                                                   N_("This week"), N_("Later"), N_("Someday")};

static int cmp_task(const void *a, const void *b) {
    const Task *x = tbl_at(&T_task, *(const int *)a), *y = tbl_at(&T_task, *(const int *)b);
    int64_t dx = x->due ? x->due : INT64_MAX, dy = y->due ? y->due : INT64_MAX;
    if (dx != dy) return dx < dy ? -1 : 1;
    return x->id - y->id;
}

static int cmp_done(const void *a, const void *b) {
    const Task *x = tbl_at(&T_task, *(const int *)a), *y = tbl_at(&T_task, *(const int *)b);
    if (x->done_at != y->done_at) return x->done_at > y->done_at ? -1 : 1;
    return y->id - x->id;
}

static void due_label(Buf *b, const Task *t, int64_t now) {
    int64_t today = day_start(now), due = day_start(t->due);
    if (due < today) {
        int days = (int)((today - due + 43200) / 86400); /* rounding keeps DST days whole */
        if (days <= 1) buf_puts(b, T("Yesterday"));
        else buf_printf(b, T("%d days overdue"), days); /* (2 or more) */
    } else {
        ui_day(b, due, now);
    }
}

static void sep(Buf *b) { buf_puts(b, "<span class=\"plan-sep\">·</span>"); }

/* Ticking off and editing need a person; a wall tablet's display account only looks. */
static bool can_act(const Ctx *c) { return c->me && c->me->role != ROLE_DISPLAY; }

/* One to-do. href: where the title links (edit modal), NULL = no link. */
static void task_row(Ctx *c, Buf *b, const Task *t, const char *idp, const char *href, bool interactive) {
    bool done = t->done_at != 0;
    bool late = !done && plan_task_group(t, c->now) == GRP_OVERDUE;
    H(b, "<li class=\"plan-task%r%r\" id=\"%s%d\">", done ? " plan-done" : "", late ? " plan-late" : "", idp, t->id);
    if (interactive) {
        ui_form(b, "plan.task_done", "inline");
        ui_hidden_int(b, "id", t->id);
        H(b, "<button class=\"tick%r\" title=\"%s\" aria-label=\"%s: %s\">", done ? " on" : "",
          done ? T("Not done after all") : T("Done"), done ? T("Undo") : T("Done"), t->title);
        ui_icon(b, "check");
        buf_puts(b, "</button>");
        ui_form_end(b);
    } else if (done) {
        buf_puts(b, "<span class=\"tick on\" aria-hidden=\"true\">");
        ui_icon(b, "check");
        buf_puts(b, "</span>");
    } else { /* a dot, not a tick circle: there's nothing to tap here (issue #80, the wall) */
        buf_puts(b, "<span class=\"plan-dot\" aria-hidden=\"true\"></span>");
    }
    const char *icon = plan_task_icon(t->title);
    if (href) H(b, "<a class=\"plan-task-main\" href=\"%s\">", href);
    else buf_puts(b, "<div class=\"plan-task-main\">");
    H(b, "<span class=\"plan-task-e\">%s</span><span class=\"plan-task-t\"><b>%s</b><small>", icon, t->title);
    if (done) {
        Person *p = person_get(t->done_by);
        Buf ago = {0};
        ui_ago(&ago, t->done_at, c->now);
        if (p) H(b, T("Done by %s %s"), p->name, buf_cstr(&ago));
        else H(b, T("Done %s"), buf_cstr(&ago));
        buf_free(&ago);
    } else {
        bool any = false;
        if (t->due) {
            due_label(b, t, c->now);
            any = true;
        }
        if (t->repeat) {
            if (any) sep(b);
            buf_puts(b, "↻ ");
            plan_repeat_label(b, t->repeat, t->rep_day);
            any = true;
            int place;
            Person *next = person_get(plan_next_turn(t, &place));
            if (next) {
                sep(b);
                H(b, T("next: %s"), next->name);
            }
        }
        if (!any) buf_puts(b, T("No date"));
    }
    buf_puts(b, "</small></span>");
    buf_puts(b, href ? "</a>" : "</div>");
    Person *who = person_get(t->who);
    if (who) {
        H(b, "<span class=\"plan-task-who\"><span class=\"sr-only\">%s </span>", T("for")); /* (issue #69) */
        ui_avatar(b, who, "sm");
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</li>");
}

static void edit_href(Ctx *c, Buf *out, int id) {
    char ids[16];
    snprintf(ids, sizeof ids, "%d", id);
    ctx_url(c, out, "edit", ids, "new", NULL, NULL);
}

/* ---------------------------------------------------------------- preview */

static void preview(Ctx *c, Buf *b) {
    const char *q = ctx_live(c, "plan.tq");
    if (str_blank(q)) return;
    TaskDraft d;
    char err[120];
    if (!plan_parse_task(q, c->now, &d, err, sizeof err)) {
        H(b, "<div class=\"preview bad\" data-say>%s</div>", err);
        return;
    }
    const char *icon = plan_task_icon(d.title);
    H(b, "<div class=\"preview ok\" data-say>%s%r<b>%s</b>", icon, icon[0] ? " " : "", d.title);
    Person *p = person_get(d.who);
    if (p) {
        sep(b);
        ui_person(b, p);
    }
    if (d.nturns > 1) { /* "@anna,@erik": taking turns - when it repeats */
        if (d.repeat) {
            Buf names = {0};
            for (int k = 1; k < d.nturns; k++) {
                Person *q = person_get(d.turns[k]);
                if (q) buf_printf(&names, "%s%s", names.len ? ", " : "", q->name);
            }
            buf_puts(b, ", ");
            H(b, T("then %s (taking turns)"), buf_cstr(&names));
            buf_free(&names);
        } else {
            H(b, " <span class=\"muted\">%s</span>", T("(to take turns it needs to repeat, like “every week”)"));
        }
    }
    sep(b);
    if (d.due) ui_day(b, d.due, c->now);
    else buf_puts(b, T("no date"));
    if (d.repeat) {
        sep(b);
        buf_puts(b, "↻ ");
        plan_repeat_label(b, d.repeat, d.rep_day);
    }
    buf_puts(b, "</div>");
}

/* ------------------------------------------------------------------ modal */

static void task_modal(Ctx *c, Buf *b, const Task *t) {
    Buf close = {0};
    ctx_url(c, &close, "edit", NULL, "new", NULL, NULL);
    ui_modal(b, t ? T("Edit to-do") : T("New to-do"), buf_cstr(&close));
    ui_form(b, "plan.task_save", "stack");
    if (t) ui_hidden_int(b, "id", t->id);
    bool child = is_child(c); /* their own to-dos only (issue #44): no Who, no turns */
    H(b,
      "<label class=\"field\"><span>%s</span><input name=\"title\" required maxlength=\"110\" autocomplete=\"off\" "
      "value=\"%s\" autofocus></label><div class=\"field-row\">",
      T("What"), t ? t->title : "");
    if (!child) {
        H(b, "<label class=\"field\"><span>%s</span>", T("Who"));
        ui_person_select(b, "who", t ? t->who : 0, T("Anyone"));
        buf_puts(b, "</label>");
    }
    char ymd[11] = "";
    if (t && t->due) fmt_ymd(t->due, ymd);
    H(b, "<label class=\"field\"><span>%s</span><input type=\"date\" name=\"due\" value=\"%s\"></label></div>", T("When"),
      ymd);
    static const char *const reps[] = {N_("Never"), N_("Every day"), N_("Every week"), N_("Every 2 weeks"),
                                       N_("Every month")};
    H(b, "<label class=\"field\"><span>%s</span><select name=\"repeat\">", T("Repeat"));
    for (int i = 0; i < countof(reps); i++)
        H(b, "<option value=\"%d\"%r>%s</option>", i, t && t->repeat == i ? " selected" : "", T(reps[i]));
    buf_puts(b, "</select></label>");
    Buf names = {0};
    if (t) plan_turn_names(&names, t);
    if (!child)
        H(b,
          "<label class=\"field\"><span>%s</span><input name=\"turns\" maxlength=\"200\" autocomplete=\"off\" "
          "value=\"%s\" placeholder=\"%s\" aria-describedby=\"plan-turns-h\"></label>"
          "<p class=\"muted small\" id=\"plan-turns-h\">%s</p>",
          T("Taking turns"), buf_cstr(&names), T("Names in turn order, like Anna, Erik"),
          T("For something that repeats: each time it's done, the next one in line gets it. Leave it empty for one "
            "person."));
    buf_free(&names);
    H(b, "<div class=\"modal-f\"><button class=\"btn primary\">%s</button></div>", T("Save"));
    ui_form_end(b);
    int place;
    Person *next = t && !t->done_at ? person_get(plan_next_turn(t, &place)) : NULL;
    Person *now_p = t ? person_get(t->who) : NULL;
    if (next && now_p && next != now_p && !t->swap) { /* this time to the next one, who's then followed by them */
        ui_form(b, "plan.task_swap", "plan-swap");
        ui_hidden_int(b, "id", t->id);
        buf_puts(b, "<button class=\"btn\">");
        H(b, T("Swap: %s does this one, %s the next"), next->name, now_p->name);
        buf_puts(b, "</button>");
        ui_form_end(b);
    } else if (t && t->swap && !t->done_at) {
        Person *gave = person_get(t->swap);
        ui_form(b, "plan.task_swap", "plan-swap");
        ui_hidden_int(b, "id", t->id);
        buf_puts(b, "<button class=\"btn\">");
        H(b, T("Swap back: %s does this one"), gave ? gave->name : T("the one in line"));
        buf_puts(b, "</button>");
        ui_form_end(b);
    }
    if (t && (!child || t->by == c->me->id)) { /* (a child takes off only what they put on) */
        ui_form(b, "plan.task_delete", "danger-zone"); /* no "Delete ...?": the toast offers Undo (issue #60) */
        ui_hidden_int(b, "id", t->id);
        buf_puts(b, "<button class=\"btn danger\">");
        ui_icon(b, "trash");
        H(b, "<span>%s</span></button>", T("Delete"));
        ui_form_end(b);
    }
    ui_modal_end(b);
    buf_free(&close);
}

/* ------------------------------------------------------------------- page */

static int filter_person(Ctx *c) {
    const char *w = ctx_q(c, "who");
    if (str_eq(w, "me")) return c->me ? c->me->id : 0;
    return person_get((int)str_int_or(w, 0)) ? (int)str_int_or(w, 0) : 0;
}

static void filter_chips(Ctx *c, Buf *b) {
    const char *w = ctx_q(c, "who");
    H(b, "<div class=\"chips plan-filter\" role=\"navigation\" aria-label=\"%s\">", T("Whose to-dos"));
    H(b, "<a class=\"chip%r\" href=\"/plan\">%s</a>", w[0] ? "" : " on", T("Everyone"));
    if (c->me) {
        int n = plan_open_tasks(c->me->id);
        H(b, "<a class=\"chip%r\" href=\"/plan?who=me\">%s", str_eq(w, "me") ? " on" : "", T("Mine"));
        if (n) H(b, " <span class=\"plan-n\">%d</span>", n);
        buf_puts(b, "</a>");
    }
    for (int i = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        if (c->me && p->id == c->me->id) continue;
        char ids[16];
        snprintf(ids, sizeof ids, "%d", p->id);
        int n = plan_open_tasks(p->id);
        H(b, "<a class=\"chip%r\" href=\"/plan?who=%d\">", str_eq(w, ids) ? " on" : "", p->id);
        ui_avatar_deco(b, p, "xs");
        H(b, "%s", p->name);
        if (n) H(b, " <span class=\"plan-n\">%d</span>", n);
        buf_puts(b, "</a>");
    }
    buf_puts(b, "</div>");
}

void plan_tasks_page(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Plan"));
    Buf sub = {0};
    plan_subtitle(c, &sub);
    ui_head(b, T("Plan"), buf_cstr(&sub));
    plan_tabs(c, b, 0);
    ui_head_end(b);
    buf_free(&sub);

    ui_form(b, "plan.task_add", "quick plan-quick");
    H(b, "<div class=\"quick-row\"><input name=\"q\" required maxlength=\"200\" autocomplete=\"off\" "
         "enterkeyhint=\"done\" data-live=\"plan.task_preview\" data-key=\"/\" aria-label=\"%s\" "
         "placeholder=\"%s\"><button class=\"btn primary\" aria-label=\"%s\">", /* (phones: only the +, #65) */
      T("New to-do"),
      is_child(c) ? T("Add a to-do for yourself — “tidy my room sat”") : T("Add a to-do — “vacuum @eva fri every week”"),
      T("Add to-do"));
    ui_icon(b, "plus");
    H(b, "<span>%s</span></button></div>", T("Add"));
    preview(c, b);
    ui_form_end(b);

    if (person_count() > 1 && !is_child(c)) filter_chips(c, b);

    int who = is_child(c) ? c->me->id : filter_person(c); /* a child: their own (issue #44) */
    static int idx[MAX_LIST];
    int n = 0;
    for (int i = 0; i < tbl_count(&T_task) && n < MAX_LIST; i++) {
        Task *t = tbl_at(&T_task, i);
        if (!t->done_at && (!who || t->who == who)) idx[n++] = i;
    }
    qsort(idx, (size_t)n, sizeof *idx, cmp_task);

    buf_puts(b, "<div class=\"plan-groups\">");
    if (!n) {
        buf_puts(b, "<section class=\"card\">");
        ui_empty(b, "✨",
                 !who                         ? T("All done!")
                 : c->me && who == c->me->id ? T("Nothing on your plate")
                                             : T("Nothing on their plate"),
                 T("Add chores like “water plants every sunday” or “call grandma tomorrow”."));
        buf_puts(b, "</section>");
    }
    Buf href = {0};
    for (int g = 0; g < GRP_COUNT; g++) {
        int cnt = 0;
        for (int k = 0; k < n; k++) cnt += plan_task_group(tbl_at(&T_task, idx[k]), c->now) == g;
        if (!cnt) continue;
        H(b,
          "<section class=\"card plan-grp plan-g%d\"><header class=\"card-h\"><h2>%s <span class=\"plan-n\">%d</span>"
          "</h2></header><ul class=\"plan-tasks\">",
          g, T(GROUP_NAMES[g]), cnt);
        for (int k = 0; k < n; k++) {
            Task *t = tbl_at(&T_task, idx[k]);
            if (plan_task_group(t, c->now) != g) continue;
            buf_reset(&href);
            edit_href(c, &href, t->id);
            task_row(c, b, t, "plan-t", can_act(c) ? buf_cstr(&href) : NULL, can_act(c));
        }
        buf_puts(b, "</ul></section>");
    }

    /* recently finished: a little dopamine, and an easy undo */
    n = 0;
    for (int i = 0; i < tbl_count(&T_task) && n < MAX_LIST; i++) {
        Task *t = tbl_at(&T_task, i);
        if (t->done_at && t->done_at > c->now - 3 * 86400 && (!who || t->who == who || t->done_by == who)) idx[n++] = i;
    }
    if (n) {
        qsort(idx, (size_t)n, sizeof *idx, cmp_done);
        H(b, "<section class=\"card plan-grp plan-done-grp\"><header class=\"card-h\"><h2>%s</h2></header>"
             "<ul class=\"plan-tasks\">",
          T("Done recently"));
        for (int k = 0; k < n && k < 10; k++) task_row(c, b, tbl_at(&T_task, idx[k]), "plan-t", NULL, can_act(c));
        buf_puts(b, "</ul></section>");
    }
    buf_puts(b, "</div>");
    buf_free(&href);

    int edit = (int)ctx_qint(c, "edit", 0);
    Task *t = edit ? tbl_get(&T_task, edit) : NULL;
    if (t && is_child(c) && t->who != c->me->id) t = NULL, edit = 0; /* (someone else's: not a child's to open) */
    if (t || (!edit && str_eq(ctx_q(c, "new"), "1"))) task_modal(c, b, t);
}

/* --------------------------------------------------------------- widget */

int plan_today_tasks(Ctx *c, Buf *b, bool wall) {
    static int idx[MAX_LIST];
    int n = 0, mine = wall ? 0 : plan_mine(c); /* (a child's home card: their own) */
    for (int i = 0; i < tbl_count(&T_task) && n < MAX_LIST; i++) {
        Task *t = tbl_at(&T_task, i);
        int g = plan_task_group(t, c->now);
        if (!t->done_at && (g == GRP_OVERDUE || g == GRP_TODAY) && (!mine || t->who == mine)) idx[n++] = i;
    }
    if (!n) return 0;
    qsort(idx, (size_t)n, sizeof *idx, cmp_task);
    int max = wall ? 8 : 6;
    buf_puts(b, "<ul class=\"plan-tasks plan-compact\">");
    char href[32];
    for (int k = 0; k < n && k < max; k++) {
        Task *t = tbl_at(&T_task, idx[k]);
        snprintf(href, sizeof href, "/plan?edit=%d", t->id);
        /* ticking needs a person; the wall display just shows */
        bool act = can_act(c) && !wall;
        task_row(c, b, t, wall ? "plan-wt" : "plan-ht", act ? href : NULL, act);
    }
    buf_puts(b, "</ul>");
    if (n > max) {
        H(b, "<p class=\"muted plan-more\" data-fit-more=\"%d\">", n - max);
        H(b, T("+%d more on the list"), n - max);
        buf_puts(b, "</p>");
    }
    return n;
}

/* ---------------------------------------------------------------- actions */

/* The line for taking turns, as ids "3,5,9"; this time is the first one's. */
static void set_turns(Task *t, const int *ids, int n) {
    Buf b = {0};
    for (int k = 0; k < n; k++) buf_printf(&b, "%s%d", k ? "," : "", ids[k]);
    str_copy(t->turns, sizeof t->turns, buf_cstr(&b));
    buf_free(&b);
    t->who = ids[0];
    t->turn = 0;
    t->swap = 0;
}

bool plan_drop_turn(Task *t, int person) {
    int ids[PLAN_TURNS], n = plan_turn_ids(t, ids), rest[PLAN_TURNS], k = 0, at = -1;
    for (int i = 0; i < n; i++) {
        if (ids[i] == person) at = i;
        else rest[k++] = ids[i];
    }
    if (at < 0) return false;
    int turn = t->turn - (at < t->turn); /* whose place it is follows; theirs: the one after them sits there now */
    Buf b = {0};
    for (int i = 0; i < k && k > 1; i++) buf_printf(&b, "%s%d", i ? "," : "", rest[i]);
    str_copy(t->turns, sizeof t->turns, buf_cstr(&b));
    buf_free(&b);
    t->turn = k > 1 ? turn % k : 0;
    if (t->who == person && !t->done_at) t->who = k > 1 ? rest[t->turn] : k ? rest[0] : 0;
    return true;
}

/* A child's own to-do (issue #44): false, and said, for anyone else's */
static bool childs_own(Ctx *c, const Task *t) {
    if (!is_child(c) || t->who == c->me->id) return true;
    ctx_fail(c, "%s", T("That one isn't yours - ask a grown-up"));
    return false;
}

/* "Anna, Erik" -> their ids, in that order; false (said beside the field) for a name nobody has */
static bool read_turns(Ctx *c, const char *text, int out[PLAN_TURNS], int *n) {
    char copy[256];
    str_copy(copy, sizeof copy, text);
    *n = 0;
    for (char *name = strtok(copy, ",;"); name; name = strtok(NULL, ",;")) {
        str_trim(name);
        while (*name == '@') name++;
        if (!*name) continue;
        Person *p = person_find(name);
        if (!p || !person_real(p->id)) {
            ctx_fail_at(c, "turns", T("Who's “%s”? Use the names in the household"), name);
            return false;
        }
        bool dup = false;
        for (int k = 0; k < *n; k++) dup |= out[k] == p->id;
        if (dup) continue;
        if (*n == PLAN_TURNS) {
            ctx_fail_at(c, "turns", T("At most %d people can take turns"), PLAN_TURNS);
            return false;
        }
        out[(*n)++] = p->id;
    }
    return true;
}

static void close_modal(Ctx *c) {
    Buf u = {0};
    ctx_url(c, &u, "edit", NULL, "new", NULL, NULL);
    ctx_redirect(c, "%s", buf_cstr(&u));
    buf_free(&u);
}

void plan_act_task_preview(Ctx *c, Fields *f) { ctx_set_live(c, "plan.tq", fields_str(f, "q")); }

void plan_act_task_add(Ctx *c, Fields *f) {
    TaskDraft d;
    char err[160];
    if (!plan_parse_task(fields_str(f, "q"), c->now, &d, err, sizeof err)) {
        ctx_fail(c, "%s", err);
        return;
    }
    if (is_child(c) && ((d.who && d.who != c->me->id) || d.nturns > 1)) { /* (their own: issue #44) */
        ctx_fail(c, "%s", T("You can add to-dos for yourself"));
        return;
    }
    Task t = {0};
    str_copy(t.title, sizeof t.title, d.title);
    t.who = is_child(c) ? c->me->id : d.who;
    t.due = d.due;
    t.repeat = d.repeat;
    t.rep_day = d.rep_day;
    if (d.repeat && d.nturns > 1) set_turns(&t, d.turns, d.nturns);
    t.by = c->me->id;
    t.created = c->now;
    tbl_put(&T_task, &t);
    ctx_set_live(c, "plan.tq", "");
    Person *who = person_get(t.who);
    if (who && who->id != c->me->id) {
        ctx_ok(c, T("Added for %s"), who->name);
        activity_add(c->me->id, "📝", "/plan", N_("%s gave %s a to-do: %s"), c->me->name, who->name, t.title);
        app_notify(c, TOAST_INFO, N_("%s gave %s a to-do: %s"), c->me->name, who->name, t.title);
    } else {
        ctx_ok(c, "%s", T("Added"));
    }
}

void plan_act_task_done(Ctx *c, Fields *f) {
    Task *row = tbl_get(&T_task, (int)fields_int(f, "id", 0));
    if (!row) {
        ctx_fail(c, "%s", T("That to-do is gone"));
        return;
    }
    Task t = *row;
    if (!childs_own(c, &t)) return;
    if (t.done_at) { /* undo */
        t.done_at = 0;
        t.done_by = 0;
        if (t.spawned) {
            Task *next = tbl_get(&T_task, t.spawned);
            if (next && !next->done_at) tbl_del(&T_task, t.spawned);
            t.spawned = 0;
        }
        tbl_put(&T_task, &t);
        ctx_info(c, "%s", T("Back on the list"));
        return;
    }
    t.done_at = c->now;
    t.done_by = c->me->id;
    if (t.repeat) {
        Task next = t;
        next.id = 0;
        next.done_at = 0;
        next.done_by = 0;
        next.spawned = 0;
        next.created = c->now;
        next.due = plan_next_due(&t, c->now);
        int place, turn = plan_next_turn(&t, &place); /* taking turns: the next one in line (issue #47) */
        if (turn) {
            next.who = turn;
            next.turn = place;
            next.swap = 0;
        }
        tbl_put(&T_task, &next);
        t.spawned = next.id;
        tbl_put(&T_task, &t);
        Buf day = {0}; /* (mid-sentence: Swedish and Spanish days in lower case; English as it always was) */
        if (i18n_lang() == LANG_EN) ui_day(&day, next.due, c->now);
        else ui_day_inline(&day, next.due, c->now);
        Person *p = turn ? person_get(turn) : NULL;
        if (p && p->id != c->me->id) {
            ctx_ok(c, T("Done! %s's turn next: %s"), p->name, buf_cstr(&day));
            app_notify_person(p->id, TOAST_INFO, N_("Your turn next: %s · %s"), t.title, buf_cstr(&day));
        } else if (p) {
            ctx_ok(c, T("Done! Your turn again next: %s"), buf_cstr(&day));
        } else {
            ctx_ok(c, T("Done! Next time: %s"), buf_cstr(&day));
        }
        buf_free(&day);
        if (p) activity_add(c->me->id, "✅", "/plan", N_("%s ticked off %s · %s's turn next"), c->me->name, t.title, p->name);
        else activity_add(c->me->id, "✅", "/plan", N_("%s ticked off %s"), c->me->name, t.title);
    } else {
        tbl_put(&T_task, &t);
        ctx_ok(c, "%s", T("Done!"));
        activity_add(c->me->id, "✅", "/plan", N_("%s ticked off %s"), c->me->name, t.title);
    }
}

void plan_act_task_save(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    Task t = {0};
    if (id) {
        Task *row = tbl_get(&T_task, id);
        if (!row) {
            ctx_fail(c, "%s", T("That to-do is gone"));
            return;
        }
        t = *row;
        if (!childs_own(c, &t)) return;
    } else {
        t.by = c->me->id;
        t.created = c->now;
    }
    char title[160];
    str_copy(title, sizeof title, fields_str(f, "title"));
    str_trim(title);
    if (!title[0]) {
        ctx_fail_at(c, "title", "%s", T("Give it a name"));
        return;
    }
    plan_capitalise(title);
    str_copy(t.title, sizeof t.title, title);
    int who = is_child(c) ? c->me->id : (int)fields_int(f, "who", 0); /* (a child's: their own) */
    t.who = person_real(who) ? who : 0; /* pickers list real people only */
    const char *due = fields_str(f, "due");
    int y = 0, m = 0, d = 0;
    if (due[0] && !parse_ymd(due, &y, &m, &d)) {
        ctx_fail_at(c, "due", "%s", T("That date doesn't look right"));
        return;
    }
    t.due = due[0] ? dt_make(y, m, d, 0, 0) : 0;
    t.repeat = (int)CLAMP(fields_int(f, "repeat", 0), REPEAT_NONE, REPEAT_MONTHLY);
    if (t.repeat && !t.due) t.due = day_start(c->now);
    t.rep_day = 0;
    if (t.repeat == REPEAT_WEEKLY || t.repeat == REPEAT_BIWEEKLY) t.rep_day = dt_local(t.due).wday + 1;
    if (t.repeat == REPEAT_MONTHLY) t.rep_day = dt_local(t.due).day;
    int line[PLAN_TURNS], n = 0;
    if (!read_turns(c, is_child(c) ? "" : fields_str(f, "turns"), line, &n)) return;
    if (n > 1 && !t.repeat) {
        ctx_fail_at(c, "turns", "%s", T("Taking turns needs it to repeat - pick how often above"));
        return;
    }
    char was[sizeof t.turns];
    str_copy(was, sizeof was, t.turns);
    if (n > 1) {
        int now_who = t.who;
        set_turns(&t, line, n);
        for (int k = 0; k < n; k++) /* whoever was picked above does this one, if they're in the line */
            if (line[k] == now_who) t.who = now_who, t.turn = k;
        if (str_eq(was, t.turns) && t.swap) t.swap = 0;
    } else {
        t.turns[0] = '\0';
        t.turn = t.swap = 0;
        if (n == 1) t.who = line[0];
    }
    tbl_put(&T_task, &t);
    ctx_ok(c, "%s", id ? T("Saved") : T("Added"));
    close_modal(c);
}

/* Taking turns: this time goes to the next one in line, and the one whose turn it was takes the next -
   or, swapped already, back again (issue #47). */
void plan_act_task_swap(Ctx *c, Fields *f) {
    Task *row = tbl_get(&T_task, (int)fields_int(f, "id", 0));
    if (!row || row->done_at) {
        ctx_fail(c, "%s", T("That to-do is gone or done"));
        return;
    }
    Task t = *row;
    int place;
    if (t.swap) {
        Person *gave = person_real(t.swap);
        t.who = gave ? gave->id : t.who;
        t.swap = 0;
        tbl_put(&T_task, &t);
        if (gave) ctx_ok(c, T("Swapped back: %s's turn"), gave->name);
        else ctx_ok(c, "%s", T("Swapped back")); /* (whoever gave it away has left the household) */
        return;
    }
    int next = plan_next_turn(&t, &place);
    Person *to = person_real(next), *from = person_get(t.who);
    if (!to || to->id == t.who) {
        ctx_fail(c, "%s", T("There's nobody else in line to swap with"));
        return;
    }
    t.swap = t.who;
    t.who = to->id;
    tbl_put(&T_task, &t);
    ctx_ok(c, T("Swapped: %s does this one, %s the next"), to->name, from ? from->name : T("the one in line"));
    if (to->id != c->me->id)
        app_notify_person(to->id, TOAST_INFO, N_("%s swapped %s with you: it's your turn now"), c->me->name, t.title);
    activity_add(c->me->id, "🔁", "/plan", N_("%s swapped turns: %s does %s this time"), c->me->name, to->name, t.title);
    close_modal(c);
}

void plan_act_task_delete(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    const Task *row = tbl_get(&T_task, id);
    if (row && is_child(c) && row->by != c->me->id) { /* (what a grown-up gave them stays on: issue #44) */
        ctx_fail(c, "%s", T("Ask a grown-up to take that one off"));
        return;
    }
    ctx_undo(c, &T_task, id); /* Undo rather than "Delete ...?" (issue #60) */
    if (!tbl_del(&T_task, id)) {
        ctx_fail(c, "%s", T("That to-do is gone"));
        return;
    }
    ctx_ok(c, "%s", T("Deleted"));
    close_modal(c);
}
