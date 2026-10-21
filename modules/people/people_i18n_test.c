/* The household page in Swedish and Spanish (issue #46): every page and dialog of modules/people, with
   people of every kind, and the language picker on Your account and in the person dialog. */
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

/* ------------------------------------------------ a household of every kind */

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

static const char *const ALLOW[] = {"Anna", "anna", "Eva",     "eva",     "Leo",     "leo", "Gun", "gun",
                                    "Hallway", "hallway", "Bo", "Love", "Aa" /* (the initials' sample) */,
                                    "iPhone", "Android", /* (the calendar card's phones) */
                                    /* each language is named in itself, the same in every language */
                                    "English", "Svenska", "Español", NULL};

static void check_pages(int person, const char *const *urls, size_t n) {
    Buf r = {0};
    int bad = 0;
    for (size_t i = 0; i < n; i++) bad += untranslated(person, urls[i], ALLOW, &r);
    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);
}

TEST(people_speaks_swedish_and_spanish) {
    th_boot();
    auth_configure("test-server-salt", 1000);
    int anna = with_sign_in("Anna", "anna", ROLE_ADMIN);
    int eva = with_sign_in("Eva", "eva", ROLE_MEMBER);
    int leo = with_sign_in("Leo", "leo", ROLE_CHILD);
    int gun = with_sign_in("Gun", "gun", ROLE_GUEST);
    int hall = with_sign_in("Hallway", "hallway", ROLE_DISPLAY);
    int bo = th_person("Bo");
    th_person("Love"); /* (no sign-in) */
    char f[64], sid[33];
    CHECK(th_act(eva, "board.post", "text=Milk&hue=45").ok); /* (the remove dialog says what goes) */
    snprintf(f, sizeof f, "id=%d&how=keep", bo);
    CHECK(th_act(anna, "people.delete", f).ok); /* moved out */
    session_create(anna, true, "Mozilla/5.0 (Windows NT 10.0) Chrome/130", sid);
    session_create(anna, false, "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0) Safari/604.1", sid);
    session_create(eva, false, "Mozilla/5.0 (Linux; Android 14) Firefox/120", sid);
    presence_set_home(eva, true, now_sec());
    Person e = *person_get(eva);
    e.last_login = now_sec() - 7200;
    tbl_put(&T_person, &e);
    CHECK(th_act(anna, "people.codes", "current=correct+horse").ok); /* new codes, waiting to be saved */

    char edit_eva[40], edit_anna[40], edit_hall[40], edit_gun[40], rm_eva[40], rm_hall[40], rm_bo[40];
    snprintf(edit_eva, sizeof edit_eva, "/people?edit=%d", eva);
    snprintf(edit_anna, sizeof edit_anna, "/people?edit=%d", anna);
    snprintf(edit_hall, sizeof edit_hall, "/people?edit=%d", hall);
    snprintf(edit_gun, sizeof edit_gun, "/people?edit=%d", gun);
    snprintf(rm_eva, sizeof rm_eva, "/people?remove=%d", eva);
    snprintf(rm_hall, sizeof rm_hall, "/people?remove=%d", hall);
    snprintf(rm_bo, sizeof rm_bo, "/people?remove=%d", bo);
    const char *const admin_urls[] = {"/people",  "/people?add=1", edit_eva,  edit_anna, edit_hall,
                                      edit_gun,   rm_eva,          rm_hall,   rm_bo,     "/people/codes",
                                      "/people?recovered=1"};
    check_pages(anna, admin_urls, countof(admin_urls));

    /* the codes saved: how many are left, and the page that has none to show */
    CHECK(th_act(anna, "people.codes_saved", "").ok);
    const char *const after[] = {"/people", "/people/codes"};
    check_pages(anna, after, countof(after));

    /* the calendar link: no address yet, this computer only, and a phone's (with its code to scan) */
    CHECK(th_act(anna, "people.cal_make", "").ok);
    const char *const cal[] = {"/people"};
    check_pages(anna, cal, 1);
    app_test_ctx_host("127.0.0.1:8080");
    check_pages(anna, cal, 1);
    app_test_ctx_host("hemma.local:8080");
    check_pages(anna, cal, 1);
    app_test_ctx_host(NULL);

    /* a temporary password; the browser asking for a language of ours */
    pref_set(anna, "auth.temp", "1");
    app_test_dev_lang(LANG_SV);
    check_pages(anna, cal, 1);
    app_test_dev_lang(-1);
    pref_set(anna, "auth.temp", "");

    /* everyone else's view */
    const char *const own[] = {"/people", edit_eva};
    check_pages(eva, own, countof(own));
    check_pages(leo, cal, 1);
    check_pages(gun, cal, 1);
    check_pages(hall, cal, 1);
    th_halt();
}

TEST(people_speaks_swedish_and_spanish_in_toasts_and_the_latest_feed) {
    th_boot();
    auth_configure("test-server-salt", 1000);
    int anna = with_sign_in("Anna", "anna", ROLE_ADMIN);
    pref_set(anna, PREF_LANG, "sv");
    ThResult r = th_act(anna, "people.save", "name=Eva");
    CHECK_STR(r.toast, "Lade till Eva");
    r = th_act(anna, "people.save", "name=Eva");
    CHECK_STR(r.toast, "Det finns redan någon som heter Eva här");
    r = th_act(anna, "people.save", "name=Bo&username=bo&password=short");
    CHECK_STR(r.toast, "Använd minst 8 tecken"); /* (core/auth.c's words, said here) */
    pref_set(anna, PREF_LANG, "es");
    r = th_act(anna, "people.signout_others", "");
    CHECK_STR(r.toast, "Se cerró la sesión en 0 dispositivos más");
    /* the Latest line is kept in English and shown in each reader's language */
    const Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "Eva joined the household");
    Buf b = {0};
    Lang was = i18n_use(LANG_SV);
    activity_text(a, &b);
    CHECK_STR(buf_cstr(&b), "Eva gick med i hushållet");
    i18n_use(was);
    buf_free(&b);
    th_halt();
}

/* ------------------------------------------------ the language picker */

TEST(people_choose_their_language) {
    th_boot();
    auth_configure("test-server-salt", 1000);
    int anna = with_sign_in("Anna", "anna", ROLE_ADMIN);
    int eva = with_sign_in("Eva", "eva", ROLE_MEMBER);
    int leo = with_sign_in("Leo", "leo", ROLE_CHILD);
    int gun = with_sign_in("Gun", "gun", ROLE_GUEST);
    int hall = with_sign_in("Hallway", "hallway", ROLE_DISPLAY);
    app_test_dev_lang(-1);

    /* Your account: Automatic (and what this browser asks for), then each language named in itself */
    const char *h = th_page(eva, "/people");
    CHECK_HAS(h, "data-a=\"people.lang\"");
    CHECK_HAS(h, "name=\"lang\" value=\"\" data-auto checked> Automatic");
    CHECK_HAS(h, "asks for none of these, so English is used");
    CHECK_HAS(h, "value=\"sv\" data-auto> <span lang=\"sv\">Svenska</span>");
    CHECK_HAS(h, "<span lang=\"es\">Español</span>");
    app_test_dev_lang(LANG_ES);
    CHECK_HAS(th_page(eva, "/people"), "(este navegador: Español)"); /* (automatic: the page is Spanish too) */
    app_test_dev_lang(-1);

    /* her own: saved, the page then in Swedish, the toast in the new language */
    ThResult r = th_act(eva, "people.lang", "lang=sv");
    CHECK(r.ok);
    CHECK_STR(r.toast, "Sparat");
    CHECK_STR(pref(eva, PREF_LANG, ""), "sv");
    h = th_page(eva, "/people");
    CHECK_HAS(h, "data-lang=\"sv\"");
    CHECK_HAS(h, "value=\"sv\" data-auto checked>");
    CHECK(th_act(eva, "people.lang", "lang=").ok); /* back to automatic */
    CHECK_STR(pref(eva, PREF_LANG, ""), "");
    CHECK_HAS(th_page(eva, "/people"), "data-lang=\"en\"");
    r = th_act(eva, "people.lang", "lang=de");
    CHECK(!r.ok);
    CHECK_STR(r.field, "lang");
    CHECK(th_act(eva, "people.lang", "lang=ES").ok); /* (any case, stored as the code) */
    CHECK_STR(pref(eva, PREF_LANG, ""), "es");

    /* only an admin sets someone else's */
    char f[64];
    snprintf(f, sizeof f, "id=%d&lang=sv", leo);
    r = th_act(eva, "people.lang", f);
    CHECK(!r.ok);
    CHECK_STR(pref(leo, PREF_LANG, ""), "");
    snprintf(f, sizeof f, "id=%d&name=Leo&lang=sv", leo);
    CHECK(!th_act(eva, "people.save", f).ok);
    CHECK_STR(pref(leo, PREF_LANG, ""), "");

    /* children and guests choose their own */
    CHECK(th_act(leo, "people.lang", "lang=sv").ok);
    CHECK_HAS(th_page(leo, "/people"), "data-lang=\"sv\"");
    CHECK(th_act(gun, "people.lang", "lang=es").ok);
    CHECK_HAS(th_page(gun, "/people"), "data-lang=\"es\"");

    /* a display account can't send forms: its card says who picks, and the admin's dialog does */
    h = th_page(hall, "/people");
    CHECK_HAS(h, "An admin picks this screen's language");
    CHECK(!strstr(h, "people.lang"));
    CHECK(!th_act(hall, "people.lang", "lang=sv").ok);
    snprintf(f, sizeof f, "/people?edit=%d", hall);
    h = th_page(anna, f);
    CHECK_HAS(h, "<select name=\"lang\"><option value=\"\">Automatic</option>");
    CHECK_HAS(h, "<option value=\"sv\" lang=\"sv\">Svenska</option>");
    snprintf(f, sizeof f, "id=%d&name=Hallway&lang=sv", hall);
    r = th_act(anna, "people.save", f);
    CHECK(r.ok);
    CHECK_STR(r.toast, "Saved"); /* (the admin's own language hasn't changed) */
    CHECK_STR(pref(hall, PREF_LANG, ""), "sv");
    snprintf(f, sizeof f, "/people?edit=%d", hall);
    CHECK_HAS(th_page(anna, f), "<option value=\"sv\" lang=\"sv\" selected>Svenska</option>");
    pref_set(hall, PREF_START, "/"); /* (its home page, not the wall) */
    CHECK_HAS(th_page(hall, "/"), "data-lang=\"sv\"");
    snprintf(f, sizeof f, "id=%d&lang=es", hall); /* people.lang with an id: an admin's too */
    CHECK(th_act(anna, "people.lang", f).ok);
    CHECK_STR(pref(hall, PREF_LANG, ""), "es");
    snprintf(f, sizeof f, "id=%d&name=Hallway&lang=xx", hall);
    r = th_act(anna, "people.save", f);
    CHECK(!r.ok);
    CHECK_STR(r.field, "lang");
    CHECK_STR(pref(hall, PREF_LANG, ""), "es");

    /* an admin changing their own: the toast already in the new language; leaving it out changes nothing */
    snprintf(f, sizeof f, "id=%d&name=Anna&lang=es", anna);
    r = th_act(anna, "people.save", f);
    CHECK_STR(r.toast, "Guardado");
    snprintf(f, sizeof f, "id=%d&name=Anna", anna);
    CHECK(th_act(anna, "people.save", f).ok);
    CHECK_STR(pref(anna, PREF_LANG, ""), "es");
    /* a new person with a language from the start */
    r = th_act(anna, "people.save", "name=Maja&lang=sv");
    CHECK(r.ok);
    CHECK_STR(r.toast, "Añadido: Maja");
    CHECK_STR(pref(person_find("Maja")->id, PREF_LANG, ""), "sv");
    th_halt();
}
