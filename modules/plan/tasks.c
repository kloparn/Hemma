/* modules/plan/tasks.c — the to-do / chores tab (/plan). */
#include "plan.h"

#define MAX_LIST 2048

static const char *const GROUP_NAMES[GRP_COUNT] = {"Overdue", "Today", "Tomorrow", "This week", "Later", "Someday"};

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
        if (days <= 1) buf_puts(b, "Yesterday");
        else buf_printf(b, "%d days overdue", days);
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
          done ? "Not done after all" : "Done", done ? "Undo" : "Done", t->title);
        ui_icon(b, "check");
        buf_puts(b, "</button>");
        ui_form_end(b);
    } else {
        H(b, "<span class=\"tick%r\" aria-hidden=\"true\">", done ? " on" : "");
        ui_icon(b, "check");
        buf_puts(b, "</span>");
    }
    const char *icon = plan_task_icon(t->title);
    if (href) H(b, "<a class=\"plan-task-main\" href=\"%s\">", href);
    else buf_puts(b, "<div class=\"plan-task-main\">");
    H(b, "<span class=\"plan-task-e\">%s</span><span class=\"plan-task-t\"><b>%s</b><small>", icon, t->title);
    if (done) {
        Person *p = person_get(t->done_by);
        H(b, "Done%r%s ", p ? " by " : "", p ? p->name : "");
        ui_ago(b, t->done_at, c->now);
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
        }
        if (!any) buf_puts(b, "No date");
    }
    buf_puts(b, "</small></span>");
    buf_puts(b, href ? "</a>" : "</div>");
    Person *who = person_get(t->who);
    if (who) {
        buf_puts(b, "<span class=\"plan-task-who\">");
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
        H(b, "<div class=\"preview bad\">%s</div>", err);
        return;
    }
    const char *icon = plan_task_icon(d.title);
    H(b, "<div class=\"preview ok\">%s%r<b>%s</b>", icon, icon[0] ? " " : "", d.title);
    Person *p = person_get(d.who);
    if (p) {
        sep(b);
        ui_person(b, p);
    }
    sep(b);
    if (d.due) ui_day(b, d.due, c->now);
    else buf_puts(b, "no date");
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
    ui_modal(b, t ? "Edit to-do" : "New to-do", buf_cstr(&close));
    ui_form(b, "plan.task_save", "stack");
    if (t) ui_hidden_int(b, "id", t->id);
    H(b,
      "<label class=\"field\"><span>What</span><input name=\"title\" required maxlength=\"110\" autocomplete=\"off\" "
      "value=\"%s\" autofocus></label><div class=\"field-row\"><label class=\"field\"><span>Who</span>",
      t ? t->title : "");
    ui_person_select(b, "who", t ? t->who : 0, "Anyone");
    char ymd[11] = "";
    if (t && t->due) fmt_ymd(t->due, ymd);
    H(b, "</label><label class=\"field\"><span>When</span><input type=\"date\" name=\"due\" value=\"%s\"></label></div>",
      ymd);
    static const char *const reps[] = {"Never", "Every day", "Every week", "Every 2 weeks", "Every month"};
    buf_puts(b, "<label class=\"field\"><span>Repeat</span><select name=\"repeat\">");
    for (int i = 0; i < countof(reps); i++)
        H(b, "<option value=\"%d\"%r>%s</option>", i, t && t->repeat == i ? " selected" : "", reps[i]);
    buf_puts(b, "</select></label><div class=\"modal-f\"><button class=\"btn primary\">Save</button></div>");
    ui_form_end(b);
    if (t) {
        H(b,
          "<form method=\"post\" action=\"/a/plan.task_delete\" data-a=\"plan.task_delete\" class=\"danger-zone\" "
          "data-confirm=\"Delete “%s”?\">",
          t->title);
        ui_hidden_int(b, "id", t->id);
        buf_puts(b, "<button class=\"btn danger\">");
        ui_icon(b, "trash");
        buf_puts(b, "<span>Delete</span></button></form>");
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
    buf_puts(b, "<div class=\"chips plan-filter\" role=\"navigation\" aria-label=\"Whose to-dos\">");
    H(b, "<a class=\"chip%r\" href=\"/plan\">Everyone</a>", w[0] ? "" : " on");
    if (c->me) {
        int n = plan_open_tasks(c->me->id);
        H(b, "<a class=\"chip%r\" href=\"/plan?who=me\">Mine", str_eq(w, "me") ? " on" : "");
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
        ui_avatar(b, p, "xs");
        H(b, "%s", p->name);
        if (n) H(b, " <span class=\"plan-n\">%d</span>", n);
        buf_puts(b, "</a>");
    }
    buf_puts(b, "</div>");
}

void plan_tasks_page(Ctx *c, Buf *b) {
    ctx_title(c, "Plan");
    Buf sub = {0};
    plan_subtitle(&sub);
    ui_head(b, "Plan", buf_cstr(&sub));
    plan_tabs(b, 0);
    ui_head_end(b);
    buf_free(&sub);

    ui_form(b, "plan.task_add", "quick plan-quick");
    buf_puts(b, "<div class=\"quick-row\"><input name=\"q\" required maxlength=\"200\" autocomplete=\"off\" "
                "enterkeyhint=\"done\" data-live=\"plan.task_preview\" data-key=\"/\" aria-label=\"New to-do\" "
                "placeholder=\"Add a to-do — “vacuum @eva fri every week”\"><button class=\"btn primary\">");
    ui_icon(b, "plus");
    buf_puts(b, "<span>Add</span></button></div>");
    preview(c, b);
    ui_form_end(b);

    if (person_count() > 1) filter_chips(c, b);

    int who = filter_person(c);
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
        ui_empty(b, "✨", who ? "Nothing on their plate" : "All done!",
                 "Add chores like “water plants every sunday” or “call grandma tomorrow”.");
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
          g, GROUP_NAMES[g], cnt);
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
        H(b, "<section class=\"card plan-grp plan-done-grp\"><header class=\"card-h\"><h2>Done recently</h2></header>"
             "<ul class=\"plan-tasks\">");
        for (int k = 0; k < n && k < 10; k++) task_row(c, b, tbl_at(&T_task, idx[k]), "plan-t", NULL, can_act(c));
        buf_puts(b, "</ul></section>");
    }
    buf_puts(b, "</div>");
    buf_free(&href);

    int edit = (int)ctx_qint(c, "edit", 0);
    Task *t = edit ? tbl_get(&T_task, edit) : NULL;
    if (t || str_eq(ctx_q(c, "new"), "1")) task_modal(c, b, t);
}

/* --------------------------------------------------------------- widget */

int plan_today_tasks(Ctx *c, Buf *b, bool wall) {
    static int idx[MAX_LIST];
    int n = 0;
    for (int i = 0; i < tbl_count(&T_task) && n < MAX_LIST; i++) {
        Task *t = tbl_at(&T_task, i);
        int g = plan_task_group(t, c->now);
        if (!t->done_at && (g == GRP_OVERDUE || g == GRP_TODAY)) idx[n++] = i;
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
    if (n > max) H(b, "<p class=\"muted plan-more\">+%d more on the list</p>", n - max);
    return n;
}

/* ---------------------------------------------------------------- actions */

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
    Task t = {0};
    str_copy(t.title, sizeof t.title, d.title);
    t.who = d.who;
    t.due = d.due;
    t.repeat = d.repeat;
    t.rep_day = d.rep_day;
    t.by = c->me->id;
    t.created = c->now;
    tbl_put(&T_task, &t);
    ctx_set_live(c, "plan.tq", "");
    Person *who = person_get(t.who);
    if (who && who->id != c->me->id) {
        ctx_ok(c, "Added for %s", who->name);
        activity_add(c->me->id, "📝", "/plan", "%s gave %s a to-do: %s", c->me->name, who->name, t.title);
        app_notify(c, TOAST_INFO, "%s gave %s a to-do: %s", c->me->name, who->name, t.title);
    } else {
        ctx_ok(c, "Added");
    }
}

void plan_act_task_done(Ctx *c, Fields *f) {
    Task *row = tbl_get(&T_task, (int)fields_int(f, "id", 0));
    if (!row) {
        ctx_fail(c, "That to-do is gone");
        return;
    }
    Task t = *row;
    if (t.done_at) { /* undo */
        t.done_at = 0;
        t.done_by = 0;
        if (t.spawned) {
            Task *next = tbl_get(&T_task, t.spawned);
            if (next && !next->done_at) tbl_del(&T_task, t.spawned);
            t.spawned = 0;
        }
        tbl_put(&T_task, &t);
        ctx_info(c, "Back on the list");
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
        tbl_put(&T_task, &next);
        t.spawned = next.id;
        tbl_put(&T_task, &t);
        Buf day = {0};
        ui_day(&day, next.due, c->now);
        ctx_ok(c, "Done! Next time: %s", buf_cstr(&day));
        buf_free(&day);
    } else {
        tbl_put(&T_task, &t);
        ctx_ok(c, "Done!");
    }
    activity_add(c->me->id, "✅", "/plan", "%s ticked off %s", c->me->name, t.title);
}

void plan_act_task_save(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    Task t = {0};
    if (id) {
        Task *row = tbl_get(&T_task, id);
        if (!row) {
            ctx_fail(c, "That to-do is gone");
            return;
        }
        t = *row;
    } else {
        t.by = c->me->id;
        t.created = c->now;
    }
    char title[160];
    str_copy(title, sizeof title, fields_str(f, "title"));
    str_trim(title);
    if (!title[0]) {
        ctx_fail(c, "Give it a name");
        return;
    }
    plan_capitalise(title);
    str_copy(t.title, sizeof t.title, title);
    int who = (int)fields_int(f, "who", 0);
    t.who = person_get(who) ? who : 0;
    const char *due = fields_str(f, "due");
    int y = 0, m = 0, d = 0;
    if (due[0] && !parse_ymd(due, &y, &m, &d)) {
        ctx_fail(c, "That date doesn't look right");
        return;
    }
    t.due = due[0] ? dt_make(y, m, d, 0, 0) : 0;
    t.repeat = (int)CLAMP(fields_int(f, "repeat", 0), REPEAT_NONE, REPEAT_MONTHLY);
    if (t.repeat && !t.due) t.due = day_start(c->now);
    t.rep_day = 0;
    if (t.repeat == REPEAT_WEEKLY || t.repeat == REPEAT_BIWEEKLY) t.rep_day = dt_local(t.due).wday + 1;
    if (t.repeat == REPEAT_MONTHLY) t.rep_day = dt_local(t.due).day;
    tbl_put(&T_task, &t);
    ctx_ok(c, id ? "Saved" : "Added");
    close_modal(c);
}

void plan_act_task_delete(Ctx *c, Fields *f) {
    if (!tbl_del(&T_task, (int)fields_int(f, "id", 0))) {
        ctx_fail(c, "That to-do is gone");
        return;
    }
    ctx_ok(c, "Deleted");
    close_modal(c);
}
