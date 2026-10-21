/* The dashboard in Swedish and Spanish (issue #46): modules/home for an admin, a member, a child, a guest and
   a display account, at every hour's greeting, with the Latest feed in each reader's language. */
#include "../../core/auth.h"
#include "../../core/ui.h"
#include "../../tests/harness.h"

/* ------------------------------------------------ what's left in English (th_untranslated) */

/* What other parts of Hemma put on these pages, rendered in `l`: the shell (/more has the navigation and the
   side blocks), the dashboard's other cards, the core's pickers, dialog frame, names ("Bo (moved out)") and
   devices ("Chrome on Windows"), and what each section says it would do when someone leaves. */
static void theirs(int person, Lang l, Buf *out) {
    char was[16];
    str_copy(was, sizeof was, pref(person, PREF_LANG, ""));
    pref_set(person, PREF_LANG, i18n_code(l));
    buf_puts(out, th_page(person, "/more"));
    Ctx c;
    app_ctx(&c, person, "/");
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (m->widget && module_visible(m, &c) && !str_eq(m->id, "people") && !str_eq(m->id, "home") &&
            !str_eq(m->id, "board"))
            m->widget(&c, out, WIDGET_HOME);
    }
    ui_hue_picker(out, "hue", 0);
    ui_modal(out, "-", "/");
    ui_modal_end(out);
    /* the core's "just now", "1 h ago", also where a note puts them: "Anna · just now", "· 1 h ago ·" */
    static const int AGO[] = {0, 3600, 7200};
    Buf ago = {0};
    for (int k = 0; k < countof(AGO); k++) {
        buf_reset(&ago);
        ui_ago(&ago, c.now - AGO[k], c.now);
        H(out, "|%s|· %s ·|· %s|", buf_cstr(&ago), buf_cstr(&ago), buf_cstr(&ago));
        for (int i = 0; i < account_count(); i++) H(out, "|%s · %s|", account_at(i)->name, buf_cstr(&ago));
    }
    char label[160];
    for (int i = 0; i < account_count(); i++) {
        html_esc(out, person_label(account_at(i), label, sizeof label));
        for (int keep = 0; keep < 2; keep++) {
            Forget f = {.person = account_at(i)->id, .keep = keep, .now = c.now};
            person_forget(&f);
            for (int k = 0; k < f.nnotes; k++) html_esc(out, f.notes[k]);
        }
    }
    Buf agent = {0};
    for (int i = 0; i < tbl_count(&T_session); i++) {
        buf_reset(&agent);
        i18n_unpack(&agent, ((Session *)tbl_at(&T_session, i))->agent);
        html_esc(out, buf_cstr(&agent));
    }
    buf_free(&agent);
    buf_free(&ago);
    app_ctx_free(&c);
    pref_set(person, PREF_LANG, was);
}

/* th_untranslated, less what other parts of Hemma leave in English there (they're translated alongside this
   one, issue #46): once they're done, their words read differently in Swedish and Spanish and nothing is left
   out. What this module itself leaves in English always counts. */
static int untranslated(int person, const char *url, const char *const *allow, Buf *out) {
    Buf r = {0}, sv = {0}, es = {0}, esc = {0};
    th_untranslated(person, url, allow, &r);
    if (r.len) {
        theirs(person, LANG_SV, &sv);
        theirs(person, LANG_ES, &es);
    }
    int n = 0;
    for (char *line = r.p; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        const char *q = strstr(line, "): \"");
        bool skip = false;
        if (q) {
            char text[600];
            str_copy(text, sizeof text, q + 4);
            size_t k = strlen(text);
            if (k && text[k - 1] == '"') text[k - 1] = '\0';
            buf_reset(&esc);
            html_esc(&esc, text);
            const char *h = buf_cstr(strstr(line, "(sv)") ? &sv : &es);
            skip = strstr(h, text) || strstr(h, buf_cstr(&esc));
        }
        if (!skip) {
            n++;
            buf_printf(out, "%s\n", line);
        }
        line = end ? end + 1 : NULL;
    }
    buf_free(&r);
    buf_free(&sv);
    buf_free(&es);
    buf_free(&esc);
    return n;
}

/* ------------------------------------------------ the dashboard */

static int with_sign_in(const char *name, const char *user, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash("correct horse", p.pass, sizeof p.pass);
    p.role = role;
    if (role == ROLE_GUEST) p.until = add_days(day_start(now_sec()), 4);
    tbl_put(&T_person, &p);
    return id;
}

static const char *const ALLOW[] = {"Anna", "Eva", "Leo", "Gun", "Hallway", "Milk", "Dentist", NULL};

static void check_home(int person) {
    Buf r = {0};
    int bad = untranslated(person, "/", ALLOW, &r);
    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);
}

TEST(home_speaks_swedish_and_spanish) {
    th_boot();
    auth_configure("test-server-salt", 1000);
    int anna = with_sign_in("Anna", "anna", ROLE_ADMIN);
    CHECK(th_act(anna, "people.save", "name=Eva&username=eva&password=longenough1").ok); /* (Latest: joined) */
    int eva = person_find("Eva")->id;
    int leo = with_sign_in("Leo", "leo", ROLE_CHILD);
    int gun = with_sign_in("Gun", "gun", ROLE_GUEST);
    int hall = with_sign_in("Hallway", "hallway", ROLE_DISPLAY);
    pref_set(hall, PREF_START, "/"); /* (the dashboard, not the wall) */
    CHECK(th_act(eva, "board.post", "text=Milk&hue=45").ok);
    CHECK(th_act(anna, "board.post", "text=Dentist&hue=45&pin=1").ok);
    presence_set_home(eva, true, now_sec()); /* who's where: home, away */
    presence_set_home(leo, false, now_sec());

    check_home(anna);
    check_home(eva);
    check_home(leo);
    check_home(gun);
    check_home(hall);

    /* every greeting - night, morning, day, evening, late - and the day under it, which starts with a capital */
    static const struct {
        int hour;
        const char *en, *sv, *es;
    } greet[] = {{3, "Good night, Anna", "God natt, Anna", "Buenas noches, Anna"},
                 {8, "Good morning, Anna", "God morgon, Anna", "Buenos días, Anna"},
                 {12, "Hello, Anna", "Hej, Anna", "Hola, Anna"},
                 {19, "Good evening, Anna", "God kväll, Anna", "Buenas tardes, Anna"},
                 {23, "Good night, Anna", "God natt, Anna", "Buenas noches, Anna"}};
    for (int i = 0; i < countof(greet); i++) {
        g_fake_now = dt_make(2026, 10, 21, greet[i].hour, 0);
        pref_set(anna, PREF_LANG, "sv");
        const char *h = th_page(anna, "/");
        CHECK_HAS(h, greet[i].sv);
        CHECK_HAS(h, "Onsdag 21 oktober · vecka 43");
        pref_set(anna, PREF_LANG, "es");
        h = th_page(anna, "/");
        CHECK_HAS(h, greet[i].es);
        CHECK_HAS(h, "Miércoles 21 de octubre · semana 43");
        pref_set(anna, PREF_LANG, "");
        h = th_page(anna, "/");
        CHECK_HAS(h, greet[i].en);
        CHECK_HAS(h, "Wednesday 21 October · week 43");
    }
    g_fake_now = 0;
    th_halt();
}

/* Latest is kept in English (/api) and read in each reader's language - on the dashboard and on the wall */
TEST(home_latest_is_in_the_readers_language) {
    th_boot();
    auth_configure("test-server-salt", 1000);
    int anna = with_sign_in("Anna", "anna", ROLE_ADMIN);
    CHECK(th_act(anna, "people.save", "name=Eva").ok);
    CHECK(th_act(anna, "board.post", "text=Milk&hue=45").ok);
    pref_set(anna, PREF_LANG, "sv");
    const char *h = th_page(anna, "/");
    CHECK_HAS(h, "Senaste");
    CHECK_HAS(h, "Eva gick med i hushållet");
    CHECK_HAS(h, "Anna satte upp en lapp: “Milk”");
    const Module *owner = NULL;
    const Block *bl = block_find("home.latest", &owner);
    CHECK(bl && owner);
    CHECK_STR(T_in(LANG_SV, bl->title), "Senaste");
    pref_set(anna, PREF_LANG, "es");
    Ctx c;
    app_ctx(&c, anna, "/");
    Buf b = {0};
    CHECK(bl->render(&c, &b, SCREEN_WALL, ""));
    app_ctx_free(&c);
    CHECK_HAS(buf_cstr(&b), "Eva se unió al hogar");
    CHECK_HAS(buf_cstr(&b), "Anna dejó una nota: “Milk”");
    buf_free(&b);
    pref_set(anna, PREF_LANG, "");
    CHECK_HAS(th_page(anna, "/"), "Anna left a note: “Milk”");
    th_halt();
}
