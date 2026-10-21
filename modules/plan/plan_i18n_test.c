/* The planner in Swedish and Spanish (issue #46): every page and dialog - to-dos (groups, taking turns, a swap,
 * done ones), the shopping list (the basket, often bought), the calendar (a month, the agenda with a booking,
 * repeating events) - read as an admin, a member and a child, with nothing left in English but the household's
 * own words. */
#include "../../tests/harness.h"
#include "plan.h"

/* Wednesday 7 October 2026, 10:00 local time */
#define NOW dt_make(2026, 10, 7, 10, 0)

/* what an action said (kept until the next call) */
static const char *toast(int person, const char *action, const char *form) {
    static char said[240];
    str_copy(said, sizeof said, th_act(person, action, form).toast);
    return said;
}

static int with_role(int person, int role) {
    Person p = *person_get(person);
    p.role = role;
    tbl_put(&T_person, &p);
    return person;
}

static int task_named(const char *title) {
    for (int i = 0; i < tbl_count(&T_task); i++) {
        const Task *t = tbl_at(&T_task, i);
        if (str_eq(t->title, title) && !t->done_at) return t->id;
    }
    return 0;
}

static int event_named(const char *title) {
    for (int i = 0; i < tbl_count(&T_event); i++) {
        const Event *e = tbl_at(&T_event, i);
        if (str_eq(e->title, title)) return e->id;
    }
    return 0;
}

/* the household's own words: names (and their avatars' initials), to-dos, the list, events, a thing, a note */
static const char *const ALLOW[] = {"Anna", "Erik", "Leo", "A", "E", "L", "Pay the bills", "Water plants",
                                    "Take out trash", "Call grandma", "Fix bike", "Paint the fence", "Clean the garage",
                                    "Vacuum", "Dishes", "Wash the car", "Tidy room", "Milk", "Eggs", "Bread", "Coffee",
                                    "Butter", "Dentist", "Grandma", "visits", "Football", "Car", "ikea", NULL};

/* The frame around every page (the navigation, "Skip to content") and the core's own pieces inside ours (a
   dialog's close button, the colour picker) are the core's words, checked by its own tests: while they still
   read in English in this build they're left out of this count - once the core says them, nothing is. */
typedef struct {
    char text[48][120];
    int n;
} CoreWords;

static bool core_word(const CoreWords *w, const char *s, size_t len) {
    for (int i = 0; i < w->n; i++)
        if (strlen(w->text[i]) == len && !strncmp(w->text[i], s, len)) return true;
    return false;
}

/* Each report line (<url> (<lang>): "<text>"): its text joins `add`; and, when it is none of `skip`, counts and
   goes to `keep`. */
static int each_line(const char *report, const CoreWords *skip, CoreWords *add, Buf *keep) {
    int n = 0;
    for (const char *p = report; *p;) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *q = strstr(p, "): \"");
        if (q && q + 4 < p + len) {
            const char *t = q + 4;
            size_t tl = (size_t)(p + len - 1 - t); /* (up to the closing quote) */
            if (add && add->n < 48 && !core_word(add, t, tl)) snprintf(add->text[add->n++], 120, "%.*s", (int)tl, t);
            if (skip && !core_word(skip, t, tl)) {
                n++;
                if (keep) buf_printf(keep, "%.*s\n", (int)len, p);
            }
        }
        p += len + (e != NULL);
    }
    return n;
}

static void core_words(int person, CoreWords *w) {
    memset(w, 0, sizeof *w);
    Buf r = {0};
    th_untranslated(person, "/nowhere-at-all", NULL, &r); /* the frame and the core's "not found" */
    each_line(buf_cstr(&r), NULL, w, NULL);
    buf_free(&r);
    static const char *const helpers[] = {"Close", "Colour", "Red", "Orange", "Amber", "Lime", "Green", "Teal", "Sky", "Blue",
                                          "Indigo", "Purple", "Magenta", "Pink"};
    Lang was = i18n_use(LANG_SV);
    Buf b = {0};
    ui_modal(&b, "x", "/");
    ui_hue_picker(&b, "hue", 0);
    ui_modal_end(&b);
    i18n_use(was);
    char attr[64];
    for (int i = 0; i < countof(helpers) && w->n < 48; i++) {
        snprintf(attr, sizeof attr, "=\"%s\"", helpers[i]);
        if (strstr(buf_cstr(&b), attr)) str_copy(w->text[w->n++], 120, helpers[i]);
    }
    buf_free(&b);
}

/* What reads the same in English, Swedish and Spanish on these pages, as `person` (the core's words aside) */
static int untranslated(int person, const char *const *urls, int n, Buf *report) {
    CoreWords core;
    core_words(person, &core);
    int bad = 0;
    Buf r = {0};
    for (int i = 0; i < n; i++) {
        buf_reset(&r);
        th_untranslated(person, urls[i], ALLOW, &r);
        bad += each_line(buf_cstr(&r), &core, NULL, report);
    }
    buf_free(&r);
    return bad;
}

TEST(plan_speaks_swedish_and_spanish) {
    th_boot();
    g_fake_now = NOW;
    int anna = with_role(th_person("Anna"), ROLE_ADMIN), erik = th_person("Erik");
    int leo = with_role(th_person("Leo"), ROLE_CHILD);
    Buf r = {0};
    int bad = 0;

    /* nothing yet */
    static const char *const empty[] = {"/plan", "/plan/shop", "/plan/cal", "/plan?who=me"};
    bad += untranslated(anna, empty, countof(empty), &r);

    /* to-dos: every group, taking turns, a swap, done ones */
    char f[200];
    static const char *const tasks[] = {"title=Pay+the+bills&due=2026-10-03", "title=Water+plants&due=2026-10-06",
                                        "title=Take+out+trash&due=2026-10-07", "title=Call+grandma&due=2026-10-08",
                                        "title=Fix+bike&due=2026-10-10",       "title=Paint+the+fence&due=2026-10-20",
                                        "title=Clean+the+garage",              "title=Wash+the+car&due=2026-10-07"};
    for (int i = 0; i < countof(tasks); i++) CHECK(th_act(anna, "plan.task_save", tasks[i]).ok);
    snprintf(f, sizeof f, "title=Vacuum&who=%d&due=2026-10-09&repeat=2&turns=Erik%%2C+Anna", erik);
    CHECK(th_act(anna, "plan.task_save", f).ok);
    snprintf(f, sizeof f, "title=Dishes&who=%d&due=2026-10-08&repeat=1&turns=Anna%%2C+Erik%%2C+Leo", anna);
    CHECK(th_act(anna, "plan.task_save", f).ok);
    snprintf(f, sizeof f, "id=%d", task_named("Dishes"));
    CHECK(th_act(anna, "plan.task_swap", f).ok); /* "Swap back: Anna does this one" */
    snprintf(f, sizeof f, "title=Tidy+room&who=%d&due=2026-10-07", leo);
    CHECK(th_act(anna, "plan.task_save", f).ok);
    snprintf(f, sizeof f, "id=%d", task_named("Wash the car"));
    CHECK(th_act(erik, "plan.task_done", f).ok);

    /* shopping: on the list, in the basket, often bought */
    CHECK(th_act(erik, "plan.shop_add", "q=milk%2C+eggs%2C+bread%2C+coffee%2C+butter").ok);
    int butter = 0;
    for (int i = 0; i < tbl_count(&T_item); i++) {
        const Item *it = tbl_at(&T_item, i);
        if (str_eq(it->text, "Butter")) butter = it->id;
        if (!str_eq(it->text, "Coffee") && !str_eq(it->text, "Butter")) continue;
        snprintf(f, sizeof f, "id=%d", it->id);
        CHECK(th_act(anna, "plan.shop_toggle", f).ok);
    }
    tbl_del(&T_item, butter); /* butter: bought before, not on the list now */

    /* the calendar: a timed one, an all-day span, a repeating one, and a booking from the agenda */
    CHECK(th_act(anna, "plan.event_add", "q=dentist+tue+14%3A30+%40anna").ok);
    CHECK(th_act(anna, "plan.event_add", "q=grandma+visits+fri-sun").ok);
    snprintf(f, sizeof f, "title=Football&date=2026-10-08&from=17:00&to=18:30&who=%d&note=ikea&rep=2"
                          "&rep_until=2026-12-01", erik);
    CHECK(th_act(anna, "plan.event_save", f).ok);
    CHECK(th_act(anna, "book.preset", "name=Car").ok);
    CHECK(th_act(anna, "book.create", "res=1&date=2026-10-08&from=09:00&to=11:00").ok);

    char edit_vac[40], edit_dishes[40], edit_bills[40], edit_tidy[40], edit_dentist[48], edit_football[48];
    char who_erik[40];
    snprintf(edit_vac, sizeof edit_vac, "/plan?edit=%d", task_named("Vacuum"));
    snprintf(edit_dishes, sizeof edit_dishes, "/plan?edit=%d", task_named("Dishes"));
    snprintf(edit_bills, sizeof edit_bills, "/plan?edit=%d", task_named("Pay the bills"));
    snprintf(edit_tidy, sizeof edit_tidy, "/plan?edit=%d", task_named("Tidy room"));
    snprintf(edit_dentist, sizeof edit_dentist, "/plan/cal?edit=%d", event_named("Dentist"));
    snprintf(edit_football, sizeof edit_football, "/plan/cal?edit=%d&day=2026-10-08", event_named("Football"));
    snprintf(who_erik, sizeof who_erik, "/plan?who=%d", erik);
    const char *admin[] = {"/plan",         "/plan?who=me",      "/plan?new=1", edit_vac,         edit_dishes,
                           edit_bills,      who_erik,            "/plan/shop",  "/plan/cal",      "/plan/cal?m=2027-01",
                           "/plan/cal?new=1", edit_dentist,      edit_football, "/plan/cal?day=2026-10-20"};
    bad += untranslated(anna, admin, countof(admin), &r);
    const char *member[] = {"/plan", edit_vac, "/plan/shop", "/plan/cal", edit_football};
    bad += untranslated(erik, member, countof(member), &r);
    const char *child[] = {"/plan", "/plan?new=1", edit_tidy, "/plan/shop", "/plan/cal"};
    bad += untranslated(leo, child, countof(child), &r);

    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);
    g_fake_now = 0;
    th_halt();
}

/* The wall's cards (no address of their own: drawn here in each language) */
TEST(plan_wall_cards_speak_swedish_and_spanish) {
    th_boot();
    g_fake_now = NOW;
    int anna = th_person("Anna");
    char f[120];
    snprintf(f, sizeof f, "title=Take+out+trash&due=2026-10-07");
    CHECK(th_act(anna, "plan.task_save", f).ok);
    CHECK(th_act(anna, "plan.task_save", "title=Call+grandma&due=2026-10-08").ok);
    for (int i = 0; i < 18; i++) CHECK(th_act(anna, "plan.task_save", "title=Fix+bike&due=2026-10-07").ok);
    CHECK(th_act(anna, "plan.event_add", "q=grandma+visits+fri-sun").ok);
    Ctx c;
    app_ctx(&c, anna, "/screens/2");
    static const char *const ids[] = {"plan.today", "plan.days"};
    static const char *const en[] = {"more on the list", "Tomorrow"};
    static const char *const sv[] = {"till på listan", "Imorgon"};
    static const char *const es[] = {"más en la lista", "Mañana"};
    for (int i = 0; i < countof(ids); i++) {
        const Block *bl = block_find(ids[i], NULL);
        CHECK(bl != NULL);
        if (!bl) continue;
        for (int l = 0; l < 3; l++) {
            Lang was = i18n_use(l == 0 ? LANG_EN : l == 1 ? LANG_SV : LANG_ES);
            Buf b = {0};
            bl->render(&c, &b, SCREEN_WALL, NULL);
            CHECK_HAS(buf_cstr(&b), l == 0 ? en[i] : l == 1 ? sv[i] : es[i]);
            if (l && strstr(buf_cstr(&b), en[i])) test_fail(__FILE__, __LINE__, "%s in English: %s", ids[i], buf_cstr(&b));
            buf_free(&b);
            i18n_use(was);
        }
    }
    app_ctx_free(&c);
    g_fake_now = 0;
    th_halt();
}

/* What an action says is in the language of whoever did it; the Latest line in each reader's; the repeat words */
TEST(plan_says_what_happened_in_each_language) {
    th_boot();
    g_fake_now = NOW;
    int anna = th_person("Anna"), erik = th_person("Erik");
    pref_set(anna, PREF_LANG, "sv");
    CHECK_STR(toast(anna, "plan.shop_add", "q=milk%2C+eggs"), "Lade till Milk, Eggs");
    CHECK_STR(toast(anna, "plan.shop_add", "q=milk"), "Finns redan på listan");
    CHECK_STR(toast(anna, "plan.event_add", "q=%40erik"), "Vad händer?");
    pref_set(anna, PREF_LANG, "es");
    CHECK_STR(toast(anna, "plan.task_add", "q=vacuum+%40erik+fri"), "Añadida para Erik");
    CHECK_STR(toast(anna, "plan.task_save", "title="), "Ponle un nombre");
    const Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "Anna gave Erik a to-do: Vacuum"); /* (the English stays for /api) */
    Buf b = {0};
    Lang was = i18n_use(LANG_SV);
    activity_text(a, &b);
    CHECK_STR(buf_cstr(&b), "Anna gav Erik en uppgift: Vacuum");
    buf_reset(&b);
    plan_repeat_label(&b, REPEAT_BIWEEKLY, 2);
    CHECK_STR(buf_cstr(&b), "varannan tisdag");
    EventSeries s = {.start = dt_make(2026, 10, 1, 18, 0), .repeat = REPEAT_MONTHLY, .until = dt_make(2026, 12, 20, 0, 0)};
    buf_reset(&b);
    plan_series_label(&b, &s);
    CHECK_STR(buf_cstr(&b), "varje månad den 1:a, till 20 dec");
    i18n_use(LANG_ES);
    buf_reset(&b);
    plan_series_label(&b, &s);
    CHECK_STR(buf_cstr(&b), "cada mes el día 1, hasta el 20 dic");
    buf_reset(&b);
    activity_text(a, &b);
    CHECK_STR(buf_cstr(&b), "Anna le dio a Erik una tarea: Vacuum");
    i18n_use(was);
    buf_free(&b);
    /* a Spanish household's chores get their pictures too */
    CHECK_STR(plan_task_icon("Pasar la aspiradora"), "🧹");
    CHECK_STR(plan_task_icon("Sacar la basura"), "🗑️");
    CHECK_STR(plan_task_icon("Regards to the team"), "");
    (void)erik;
    g_fake_now = 0;
    th_halt();
}
