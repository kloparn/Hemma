#include "../../tests/harness.h"
#include "plan.h"

/* Wednesday 7 October 2026, 10:00 local time */
#define NOW dt_make(2026, 10, 7, 10, 0)

static void ymd_of(int64_t t, char out[11]) { fmt_ymd(t, out); }

/* ------------------------------------------------------------ parsing */

typedef struct {
    const char *in;
    const char *title;
    const char *who; /* person name or NULL */
    const char *due; /* YYYY-MM-DD or NULL */
    int repeat, rep_day;
} TaskCase;

TEST(plan_parses_task_quick_add) {
    th_boot();
    th_person("Adam");
    th_person("Eva");
    static const TaskCase cases[] = {
        {"vacuum @eva fri every week", "Vacuum", "Eva", "2026-10-09", REPEAT_WEEKLY, 0},
        {"take out trash every tuesday", "Take out trash", NULL, "2026-10-13", REPEAT_WEEKLY, 2},
        {"call grandma tomorrow", "Call grandma", NULL, "2026-10-08", REPEAT_NONE, 0},
        {"water plants @eva every sunday", "Water plants", "Eva", "2026-10-11", REPEAT_WEEKLY, 7},
        {"pay rent every month", "Pay rent", NULL, "2026-10-07", REPEAT_MONTHLY, 7},
        {"fix bike", "Fix bike", NULL, NULL, REPEAT_NONE, 0},
        {"dammsuga @Eva imorgon", "Dammsuga", "Eva", "2026-10-08", REPEAT_NONE, 0},
        {"ask @bob about the fence", "Ask @bob about the fence", NULL, NULL, REPEAT_NONE, 0},
        {"städa badrummet varannan fredag @adam", "Städa badrummet", "Adam", "2026-10-09", REPEAT_BIWEEKLY, 5},
        {"ölbryggning lördag", "Ölbryggning", NULL, "2026-10-10", REPEAT_NONE, 0},
        {"mow the lawn daily", "Mow the lawn", NULL, "2026-10-07", REPEAT_DAILY, 0},
    };
    for (int i = 0; i < countof(cases); i++) {
        const TaskCase *k = &cases[i];
        TaskDraft d;
        char err[100];
        if (!plan_parse_task(k->in, NOW, &d, err, sizeof err)) {
            test_fail(__FILE__, __LINE__, "\"%s\" failed: %s", k->in, err);
            continue;
        }
        char due[11] = "";
        if (d.due) ymd_of(d.due, due);
        Person *want = k->who ? person_find(k->who) : NULL;
        if (!str_eq(d.title, k->title) || d.who != (want ? want->id : 0) || !str_eq(due, k->due ? k->due : "") ||
            d.repeat != k->repeat || d.rep_day != k->rep_day) {
            test_fail(__FILE__, __LINE__, "\"%s\" -> title \"%s\" who %d due %s repeat %d day %d", k->in, d.title, d.who,
                      due, d.repeat, d.rep_day);
        }
    }
    TaskDraft d;
    char err[100];
    CHECK(!plan_parse_task("@eva fri", NOW, &d, err, sizeof err));
    CHECK_STR(err, "What needs doing?");
    CHECK(!plan_parse_task("", NOW, &d, err, sizeof err));
    CHECK(!plan_parse_task(NULL, NOW, &d, err, sizeof err));
    /* issue #13: "pay rent 31 sep" was an undated to-do called just that */
    CHECK(!plan_parse_task("pay rent 31 sep", NOW, &d, err, sizeof err));
    CHECK_HAS(err, "31 September");
    EventDraft ev;
    CHECK(!plan_parse_event("cabin 12/10/2026-10/10/2026", NOW, &ev, err, sizeof err));
    CHECK_HAS(err, "before the start");
    th_halt();
}

TEST(plan_parses_events) {
    th_boot();
    int adam = th_person("Adam");
    EventDraft d;
    char err[100];
    CHECK(plan_parse_event("dentist tue 14:30 @adam", NOW, &d, err, sizeof err));
    CHECK_STR(d.title, "Dentist");
    CHECK_INT(d.who, adam);
    CHECK(!d.allday);
    CHECK(d.start == dt_make(2026, 10, 13, 14, 30));
    CHECK(d.end == dt_make(2026, 10, 13, 15, 30));
    CHECK(plan_parse_event("grandma visits fri-sun", NOW, &d, err, sizeof err));
    CHECK_STR(d.title, "Grandma visits");
    CHECK(d.allday);
    CHECK(d.start == dt_make(2026, 10, 9, 0, 0));
    CHECK(d.end == dt_make(2026, 10, 12, 0, 0));
    CHECK(plan_parse_event("kalas lördag 15-18", NOW, &d, err, sizeof err));
    CHECK(d.start == dt_make(2026, 10, 10, 15, 0) && d.end == dt_make(2026, 10, 10, 18, 0));
    CHECK(!plan_parse_event("party", NOW, &d, err, sizeof err));
    CHECK_HAS(err, "When?");
    CHECK(!plan_parse_event("fri 18:00", NOW, &d, err, sizeof err));
    CHECK_STR(err, "What's happening?");
    th_halt();
}

TEST(plan_splits_shopping_items) {
    char items[16][100];
    CHECK_INT(plan_split_items("milk, eggs, 2 bread", items, 16), 3);
    CHECK_STR(items[0], "Milk");
    CHECK_STR(items[1], "Eggs");
    CHECK_STR(items[2], "2 bread");
    CHECK_INT(plan_split_items("  a ,, b ;\n  c   d ", items, 16), 3);
    CHECK_STR(items[2], "C d");
    CHECK_INT(plan_split_items("Milk, milk, MILK", items, 16), 1);
    CHECK_INT(plan_split_items("ägg, äpplen", items, 16), 2);
    CHECK_STR(items[0], "Ägg");
    CHECK_INT(plan_split_items(" , ; ", items, 16), 0);
    CHECK_INT(plan_split_items("a,b,c,d", items, 2), 2);
}

/* ------------------------------------------------------ repeats & groups */

static int64_t next_after(int repeat, int rep_day, int64_t due, int64_t now) {
    Task t = {0};
    t.repeat = repeat;
    t.rep_day = rep_day;
    t.due = due;
    return plan_next_due(&t, now);
}

TEST(plan_next_due_dates) {
    int64_t wed = dt_make(2026, 10, 7, 0, 0);
    CHECK(next_after(REPEAT_DAILY, 0, wed, NOW) == dt_make(2026, 10, 8, 0, 0));
    /* overdue dailies don't come back already late */
    CHECK(next_after(REPEAT_DAILY, 0, dt_make(2026, 10, 3, 0, 0), NOW) == dt_make(2026, 10, 8, 0, 0));
    /* trash day stays on Tuesday even when done a day late */
    CHECK(next_after(REPEAT_WEEKLY, 2, dt_make(2026, 10, 6, 0, 0), NOW) == dt_make(2026, 10, 13, 0, 0));
    /* finished early: the schedule keeps its rhythm */
    CHECK(next_after(REPEAT_WEEKLY, 0, dt_make(2026, 10, 9, 0, 0), NOW) == dt_make(2026, 10, 16, 0, 0));
    CHECK(next_after(REPEAT_BIWEEKLY, 0, wed, NOW) == dt_make(2026, 10, 21, 0, 0));
    CHECK(next_after(REPEAT_BIWEEKLY, 5, dt_make(2026, 10, 9, 0, 0), NOW) == dt_make(2026, 10, 23, 0, 0));
    /* month ends: 31 Jan -> 28 Feb -> 31 Mar (the anchor day survives short months) */
    int64_t jan31 = dt_make(2026, 1, 31, 0, 0);
    int64_t feb = next_after(REPEAT_MONTHLY, 31, jan31, dt_make(2026, 1, 31, 12, 0));
    CHECK(feb == dt_make(2026, 2, 28, 0, 0));
    CHECK(next_after(REPEAT_MONTHLY, 31, feb, dt_make(2026, 2, 28, 12, 0)) == dt_make(2026, 3, 31, 0, 0));
    CHECK(next_after(REPEAT_MONTHLY, 0, dt_make(2026, 12, 15, 0, 0), dt_make(2026, 12, 15, 9, 0)) ==
          dt_make(2027, 1, 15, 0, 0));
    /* leap year */
    CHECK(next_after(REPEAT_MONTHLY, 29, dt_make(2028, 1, 29, 0, 0), dt_make(2028, 1, 29, 9, 0)) ==
          dt_make(2028, 2, 29, 0, 0));
    /* someday + repeat starts from today */
    CHECK(next_after(REPEAT_DAILY, 0, 0, NOW) == dt_make(2026, 10, 8, 0, 0));
}

TEST(plan_groups_by_due_day) {
    Task t = {0};
    CHECK_INT(plan_task_group(&t, NOW), GRP_SOMEDAY);
    t.due = dt_make(2026, 10, 6, 0, 0);
    CHECK_INT(plan_task_group(&t, NOW), GRP_OVERDUE);
    t.due = dt_make(2026, 10, 7, 0, 0);
    CHECK_INT(plan_task_group(&t, NOW), GRP_TODAY);
    t.due = dt_make(2026, 10, 8, 0, 0);
    CHECK_INT(plan_task_group(&t, NOW), GRP_TOMORROW);
    t.due = dt_make(2026, 10, 11, 0, 0); /* Sunday: still this week */
    CHECK_INT(plan_task_group(&t, NOW), GRP_WEEK);
    t.due = dt_make(2026, 10, 12, 0, 0); /* next Monday */
    CHECK_INT(plan_task_group(&t, NOW), GRP_LATER);
}

TEST(plan_task_icons) {
    CHECK_STR(plan_task_icon("Vacuum the hall"), "🧹");
    CHECK_STR(plan_task_icon("Take out trash"), "🗑️");
    CHECK_STR(plan_task_icon("Ta ut soporna"), "🗑️");
    CHECK_STR(plan_task_icon("Wash the car"), "🚗");
    CHECK_STR(plan_task_icon("Peel carrots"), "");
    CHECK_STR(plan_task_icon("Dentist"), "🦷");
    CHECK_STR(plan_task_icon("Fix bike"), "🔧");
}

/* --------------------------------------------------------------- to-dos */

TEST(plan_tasks_add_complete_undo_edit_delete) {
    g_fake_now = NOW;
    th_boot();
    int adam = th_person("Adam");
    int eva = th_person("Eva");
    ThResult r = th_act(adam, "plan.task_add", "q=vacuum+%40eva+fri+every+week");
    CHECK(r.ok);
    CHECK_STR(r.toast, "Added for Eva");
    CHECK_INT(tbl_count(&T_task), 1);
    Task *t = tbl_get(&T_task, 1);
    CHECK(t && t->who == eva && t->repeat == REPEAT_WEEKLY && t->by == adam);
    CHECK_INT(tbl_count(&T_activity), 1);

    /* Eva finishes it: done + the next one appears a week later */
    r = th_act(eva, "plan.task_done", "id=1");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Next time");
    CHECK_INT(tbl_count(&T_task), 2);
    t = tbl_get(&T_task, 1);
    CHECK(t->done_at == NOW && t->done_by == eva && t->spawned == 2);
    Task *next = tbl_get(&T_task, 2);
    CHECK(next && !next->done_at && next->due == dt_make(2026, 10, 16, 0, 0) && next->who == eva);

    /* undo removes the spawned one again */
    CHECK(th_act(eva, "plan.task_done", "id=1").ok);
    CHECK_INT(tbl_count(&T_task), 1);
    CHECK(((Task *)tbl_get(&T_task, 1))->done_at == 0);

    /* edit: new title, no repeat, due Monday */
    r = th_act(adam, "plan.task_save", "id=1&title=hoover+upstairs&who=0&due=2026-10-12&repeat=0");
    CHECK(r.ok);
    t = tbl_get(&T_task, 1);
    CHECK_STR(t->title, "Hoover upstairs");
    CHECK(t->who == 0 && t->repeat == 0 && t->due == dt_make(2026, 10, 12, 0, 0));
    CHECK(!th_act(adam, "plan.task_save", "id=1&title=+&due=").ok);
    CHECK(!th_act(adam, "plan.task_save", "id=1&title=x&due=2026-13-40").ok);
    /* a new task from the modal; repeat without a date starts today */
    CHECK(th_act(adam, "plan.task_save", "title=Water+plants&repeat=2").ok);
    t = tbl_get(&T_task, 3);
    CHECK(t && t->due == dt_make(2026, 10, 7, 0, 0) && t->rep_day == 3);

    CHECK(th_act(adam, "plan.task_delete", "id=1").ok);
    CHECK(!th_act(adam, "plan.task_delete", "id=1").ok);
    CHECK(tbl_get(&T_task, 1) == NULL);
    th_halt();
}

TEST(plan_tasks_page_groups_and_filters) {
    g_fake_now = NOW;
    th_boot();
    int adam = th_person("Adam");
    th_person("Eva");
    CHECK(th_act(adam, "plan.task_save", "title=pay+bills&due=2026-10-06").ok); /* overdue since yesterday */
    CHECK(th_act(adam, "plan.task_add", "q=laundry+today+%40eva").ok);
    CHECK(th_act(adam, "plan.task_add", "q=bake+%3Cb%3Ebread%3C%2Fb%3E").ok);
    const char *html = th_page(adam, "/plan");
    CHECK_HAS(html, "Overdue");
    CHECK_HAS(html, "Yesterday");
    CHECK_HAS(html, "Today");
    CHECK_HAS(html, "Someday");
    CHECK_HAS(html, "&lt;b&gt;bread&lt;/b&gt;"); /* user text is escaped */
    CHECK(!strstr(html, "<b>bread</b>"));
    CHECK_HAS(html, "data-a=\"plan.task_add\"");
    CHECK_HAS(html, "🧺");
    /* Eva's filter only shows her laundry */
    html = th_page(adam, "/plan?who=2");
    CHECK_HAS(html, "Laundry");
    CHECK(!strstr(html, "Pay bills"));
    /* edit modal */
    html = th_page(adam, "/plan?edit=1");
    CHECK_HAS(html, "Edit to-do");
    CHECK_HAS(html, "data-a=\"plan.task_delete\"");
    CHECK_HAS(th_page(adam, "/plan?new=1"), "New to-do");
    th_halt();
}

TEST(plan_requires_identity) {
    th_boot();
    th_person("Adam");
    CHECK(!th_act(0, "plan.task_add", "q=vacuum").ok);
    CHECK(!th_act(0, "plan.shop_add", "q=milk").ok);
    CHECK(!th_act(0, "plan.event_add", "q=party+fri").ok);
    CHECK_INT(tbl_count(&T_task) + tbl_count(&T_item) + tbl_count(&T_event), 0);
    CHECK_HAS(th_page(0, "/plan"), "data-mod=\"auth\""); /* signed-out visitors get the sign-in page */
    th_halt();
}

/* ------------------------------------------------------------- shopping */

TEST(plan_shopping_add_check_clear_suggest) {
    g_fake_now = NOW;
    th_boot();
    int adam = th_person("Adam");
    ThResult r = th_act(adam, "plan.shop_add", "q=milk%2C+eggs%2C+2+bread");
    CHECK(r.ok);
    CHECK_STR(r.toast, "Added Milk, Eggs, 2 bread");
    CHECK_INT(plan_open_items(), 3);
    r = th_act(adam, "plan.shop_add", "q=MILK");
    CHECK(r.ok);
    CHECK_STR(r.toast, "Already on the list");
    CHECK_INT(tbl_count(&T_item), 3);
    CHECK(!th_act(adam, "plan.shop_add", "q=+%2C+").ok);

    CHECK(th_act(adam, "plan.shop_toggle", "id=1").ok); /* milk into the basket */
    CHECK_INT(plan_open_items(), 2);
    CHECK_INT(tbl_count(&T_bought), 1);
    const char *html = th_page(adam, "/plan/shop");
    CHECK_HAS(html, "In the basket");
    CHECK_HAS(html, "aria-pressed=\"true\"");
    CHECK(th_act(adam, "plan.shop_clear", "").ok);
    CHECK_INT(tbl_count(&T_item), 2);
    /* milk was bought before and isn't on the list: suggested */
    html = th_page(adam, "/plan/shop");
    CHECK_HAS(html, "Often bought");
    CHECK_HAS(html, "value=\"Milk\"");
    /* re-adding something still in the basket moves it back */
    CHECK(th_act(adam, "plan.shop_toggle", "id=2").ok);
    CHECK(th_act(adam, "plan.shop_add", "q=eggs").ok);
    CHECK_INT(tbl_count(&T_item), 2);
    CHECK(!((Item *)tbl_get(&T_item, 2))->done);
    CHECK(th_act(adam, "plan.shop_delete", "id=3").ok);
    CHECK_INT(plan_open_items(), 1);
    th_halt();
}

/* ------------------------------------------------------------- calendar */

TEST(plan_events_add_edit_and_agenda) {
    g_fake_now = NOW;
    th_boot();
    int adam = th_person("Adam");
    th_person("Eva");
    ThResult r = th_act(adam, "plan.event_add", "q=dentist+tue+14%3A30+%40eva");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Tue 13 Oct 14:30");
    CHECK(th_act(adam, "plan.event_add", "q=grandma+visits+fri-sun").ok);
    CHECK(!th_act(adam, "plan.event_add", "q=party").ok);
    CHECK_INT(tbl_count(&T_event), 2);
    Event *g = tbl_get(&T_event, 2);
    CHECK(g->allday && g->start == dt_make(2026, 10, 9, 0, 0) && g->end == dt_make(2026, 10, 12, 0, 0));

    const char *html = th_page(adam, "/plan/cal");
    CHECK_HAS(html, "Dentist");
    CHECK_HAS(html, "Grandma visits");
    CHECK_HAS(html, "Week 41");
    CHECK_HAS(html, "Week 42");
    CHECK_HAS(html, "plan-mm-d plan-busy");
    CHECK_HAS(html, "14:30–15:30");
    CHECK_HAS(html, "All day");
    CHECK_HAS(html, "October 2026");
    CHECK_HAS(th_page(adam, "/plan/cal?m=2026-11"), "November 2026");

    /* modal save: all-day across two days, then a timed event crossing midnight */
    r = th_act(adam, "plan.event_save", "id=1&title=Dentist&date=2026-10-14&until=2026-10-15&allday=1&who=0");
    CHECK(r.ok);
    Event *e = tbl_get(&T_event, 1);
    CHECK(e->allday && e->start == dt_make(2026, 10, 14, 0, 0) && e->end == dt_make(2026, 10, 16, 0, 0));
    /* a week out the day label already is the date: don't print it twice */
    html = th_page(adam, "/plan/cal");
    CHECK_HAS(html, "Wed 14 Oct</h3>");
    CHECK(!strstr(html, "Wed 14 Oct<small>"));
    CHECK_HAS(html, "Friday<small>Fri 9 Oct</small>");
    CHECK(th_act(adam, "plan.event_save", "title=Late+party&date=2026-10-10&from=22%3A00&to=02%3A00").ok);
    e = tbl_get(&T_event, 3);
    CHECK(e && e->start == dt_make(2026, 10, 10, 22, 0) && e->end == dt_make(2026, 10, 11, 2, 0));
    CHECK(!th_act(adam, "plan.event_save", "title=x&date=2026-10-10").ok); /* no time and not all-day */
    CHECK(!th_act(adam, "plan.event_save", "title=x&date=2026-10-10&until=2026-10-09&allday=1").ok);
    CHECK_HAS(th_page(adam, "/plan/cal?edit=3"), "Edit event");
    CHECK(th_act(adam, "plan.event_delete", "id=3").ok);
    CHECK_INT(tbl_count(&T_event), 2);
    th_halt();
}

/* -------------------------------------------------------------- widgets */

TEST(plan_widgets_render_today_and_shopping) {
    g_fake_now = NOW;
    th_boot();
    int adam = th_person("Adam");
    CHECK(th_act(adam, "plan.task_add", "q=take+out+trash+today").ok);
    CHECK(th_act(adam, "plan.event_add", "q=dinner+with+eva+today+18%3A00").ok);
    CHECK(th_act(adam, "plan.shop_add", "q=coffee%2C+oat+milk").ok);
    const char *home = th_page(adam, "/");
    CHECK_HAS(home, "plan-w");
    CHECK_HAS(home, "Take out trash");
    CHECK_HAS(home, "Dinner with eva");
    CHECK_HAS(home, "Shopping list");
    CHECK_HAS(home, "Oat milk");
    CHECK_HAS(home, "data-a=\"plan.task_done\""); /* tick it off right from the dashboard */

    /* the wall display: big type, read-only for anonymous viewers */
    const Module *m = module_find("plan");
    CHECK(m && m->widget);
    Ctx c;
    app_ctx(&c, 0, "/wall");
    Buf b = {0};
    m->widget(&c, &b, WIDGET_WALL);
    CHECK_HAS(buf_cstr(&b), "plan-wall");
    CHECK_HAS(buf_cstr(&b), "Take out trash");
    CHECK(!strstr(buf_cstr(&b), "<form"));
    /* nobody signed in on a home-style render either: still fine, just not interactive */
    buf_reset(&b);
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "Take out trash");
    CHECK(!strstr(buf_cstr(&b), "<form"));
    CHECK(!strstr(buf_cstr(&b), "?edit="));
    buf_free(&b);
    app_ctx_free(&c);
    th_halt();
}

TEST(plan_display_accounts_see_tasks_without_tick_buttons) {
    g_fake_now = NOW;
    th_boot();
    int adam = th_person("Adam");
    CHECK(th_act(adam, "plan.task_add", "q=water+the+plants+today").ok);
    int tablet = th_person("Kitchen tablet");
    Person p = *person_get(tablet);
    p.role = ROLE_DISPLAY;
    tbl_put(&T_person, &p);
    const char *home = th_page(tablet, "/");
    CHECK_HAS(home, "Water the plants");
    CHECK(!strstr(home, "data-a=\"plan.task_done\"")); /* ticking would only fail with "view-only" */
    CHECK(!strstr(home, "/plan?edit="));
    const char *list = th_page(tablet, "/plan");
    CHECK_HAS(list, "Water the plants");
    CHECK(!strstr(list, "data-a=\"plan.task_done\""));
    CHECK_HAS(th_page(adam, "/"), "data-a=\"plan.task_done\""); /* people still can */
    /* issue #14: the pickers list real people only, and a hand-made request can't pick the tablet */
    char f[120];
    snprintf(f, sizeof f, "id=1&title=water+the+plants&who=%d&due=&repeat=0", tablet);
    CHECK(th_act(adam, "plan.task_save", f).ok);
    CHECK_INT(((Task *)tbl_get(&T_task, 1))->who, 0);
    snprintf(f, sizeof f, "id=1&title=water+the+plants&who=%d&due=&repeat=0", adam);
    CHECK(th_act(adam, "plan.task_save", f).ok);
    CHECK_INT(((Task *)tbl_get(&T_task, 1))->who, adam);
    th_halt();
}
