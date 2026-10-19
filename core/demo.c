/* core/demo.c — `hemma --demo` (issue #31): a made-up household to try Hemma on, in a throwaway
 * folder (main.c makes it). Anna, the admin, comes from the demo's settings file the way a headless
 * install makes its first admin; everything else goes in through the actions people use - so the data,
 * the news on Home and every rule are the real thing, and the core names no module's tables. A step
 * whose module isn't built in is left out. Days are counted from today, so nothing lands in the past. */
#include "app.h"
#include "auth.h"

typedef struct {
    const char *who;    /* the username doing it */
    const char *action;
    const char *form;   /* url-encoded; one %s = the weekday `days` from today ("fri") */
    int days;           /* -1: no weekday in it */
} DemoStep;

static const DemoStep STEPS[] = {
    {"anna", "household.save", "name=Björkbacken", -1},
    {"anna", "people.save", "name=Maja&emoji=🦄&hue=300", -1},
    {"anna", "people.save", "name=Leo&emoji=🐢&hue=120", -1},
    {"anna", "book.preset", "name=Car", -1},
    {"anna", "book.preset", "name=Laundry", -1},
    {"anna", "book.preset", "name=Sauna", -1},
    {"anna", "book.quick", "q=car+%s+14-17+ikea+trip", 2},
    {"jonas", "book.quick", "q=laundry+tomorrow+18-21", -1},
    {"anna", "book.quick", "q=bastu+%s+19-21+@maja", 4},
    {"anna", "plan.task_add", "q=vacuum+@jonas+%s+every+week", 3},
    {"anna", "plan.task_add", "q=water+the+plants+@maja+today", -1},
    {"jonas", "plan.task_add", "q=take+out+the+recycling+@leo+tomorrow", -1},
    {"anna", "plan.task_add", "q=pay+the+electricity+bill+%s", 5},
    {"jonas", "plan.shop_add", "q=milk,+eggs,+coffee,+2+bread,+bananas", -1},
    {"anna", "plan.event_add", "q=dentist+%s+14:30+@leo", 2},
    {"anna", "plan.event_add", "q=grandma+visits+%s+13", 6},
    {"anna", "board.post", "text=The+plumber+comes+on+Thursday+morning+-+someone+needs+to+be+home.&hue=45", -1},
    {"jonas", "board.post", "text=Leo's+football+practice+has+moved+to+17:30.&hue=200", -1},
};

static bool step(const char *who, const char *action, const char *form) {
    const Person *p = person_by_username(who);
    if (!p) return false;
    Ctx c;
    app_ctx(&c, p->id, "/");
    Fields f = {0};
    fields_parse(&f, form, strlen(form));
    bool known = app_action(&c, action, &f);
    if (known && c.failed) log_warn("demo: %s didn't go in: %s", action, c.toast);
    fields_free(&f);
    app_ctx_free(&c);
    return known && !c.failed;
}

void app_demo_seed(const char *password) {
    if (setting_int("demo.seeded", 0) || !password || !password[0]) return;
    Person *anna = person_by_username("anna");
    if (!anna) return; /* (the demo's settings file makes her: main.c) */
    Person a = *anna;
    str_copy(a.emoji, sizeof a.emoji, "🦊");
    a.hue = 25;
    tbl_put(&T_person, &a);
    /* Jonas can sign in too, with the same password: one to remember */
    char form[400], day[8];
    snprintf(form, sizeof form, "name=Jonas&emoji=🐻&hue=205&username=jonas&password=%s&role=0", password);
    step("anna", "people.save", form);
    for (int i = 0; i < countof(STEPS); i++) {
        const DemoStep *s = &STEPS[i];
        if (s->days >= 0) {
            DateTime d = dt_local(now_sec() + (int64_t)s->days * 86400);
            str_copy(day, sizeof day, weekday_name(d.wday, true));
            for (char *q = day; *q; q++)
                if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
            snprintf(form, sizeof form, s->form, day);
        } else {
            str_copy(form, sizeof form, s->form);
        }
        step(s->who, s->action, form);
    }
    setting_set("demo.seeded", "1");
}
