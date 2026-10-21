/* The signed-out pages in Swedish and Spanish (issue #46): welcome (the first admin), sign-in, forgot
 * password, "Nobody can sign in", the damaged-data page, every error they show after a redirect - and
 * the language links on each of them. Signed out = the browser's language (th_untranslated person 0). */
#include "../tests/harness.h"
#include "auth.h"

static int with_login(const char *name, const char *user, const char *pw, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    auth_hash(pw, p.pass, sizeof p.pass);
    p.role = role;
    tbl_put(&T_person, &p);
    return id;
}

/* data, never words: people's names, the languages' own names, code shapes, the English docs' headings,
   and the store's rescue report (core/store.c keeps it in English) */
#define RESCUE_REPORT                                                                                                  \
    "2026-10-17 09:30: Hemma's data was damaged (parts of it couldn't be read). Nothing was deleted: the damaged "    \
    "file is kept as hemma.db.damaged-2026-10-17-0930."
static const char *const ALLOW[] = {"Anna", "Eva", "English", "Svenska", "Español", "XXXX-XXXX", "XXXX-XXXX-XXXX",
                                    "Locked out", "A damaged database", RESCUE_REPORT, NULL};

static int untranslated(const char *const *urls, int n, Buf *r) {
    int bad = 0;
    for (int i = 0; i < n; i++) bad += th_untranslated(0, urls[i], ALLOW, r);
    return bad;
}

static int g_guess;
static int guess_who(uint32_t ip) { return ip ? g_guess : 0; }

TEST(auth_signed_out_pages_speak_swedish_and_spanish) {
    th_boot();
    Buf r = {0};
    int bad = 0;

    /* no secret yet */
    auth_configure("", 1000);
    static const char *const no_salt[] = {"/", "/login"};
    bad += untranslated(no_salt, countof(no_salt), &r);

    /* the welcome page and its errors */
    auth_configure("pepper", 1000);
    CHECK(auth_setup_open());
    static const char *const welcome[] = {"/", "/book?w=2026-10-12", "/setup?e=setup", "/setup?e=name",
                                          "/setup?e=user&name=Anna", "/setup?e=taken&name=Anna&u=anna",
                                          "/setup?e=short", "/setup?e=long", "/setup?e=again&name=Anna&u=anna&home=Villa",
                                          "/setup?e=wait&w=120", "/setup?e=wait&w=30"};
    bad += untranslated(welcome, countof(welcome), &r);
    CHECK_HAS(th_page(0, "/"), "Welcome to Hemma");

    /* the data was damaged and no admin came back: nobody, then a member, can sign in */
    setting_set(SETTING_RESCUED, RESCUE_REPORT);
    static const char *const damaged[] = {"/", "/setup"};
    bad += untranslated(damaged, countof(damaged), &r);
    int eva = with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    bad += untranslated(damaged, countof(damaged), &r); /* (+ "Sign in") */
    static const char *const rescued_signin[] = {"/login"};
    bad += untranslated(rescued_signin, 1, &r); /* "some things may be missing" */
    setting_set(SETTING_RESCUED, "");

    /* the welcome page while a member can sign in: "I already have an account" */
    CHECK(auth_setup_open());
    CHECK_HAS(th_page(0, "/"), "I already have an account");
    bad += untranslated(welcome, 1, &r);

    /* nobody can sign in: the household had an admin */
    tbl_del(&T_person, eva);
    setting_set_int("auth.bootstrapped", 1);
    CHECK_HAS(th_page(0, "/"), "Nobody can sign in");
    bad += untranslated(no_salt, countof(no_salt), &r);

    /* signing in, every message a redirect leaves */
    with_login("Anna", "anna", "correct horse", ROLE_ADMIN);
    static const char *const signin[] = {"/",           "/login",      "/login?next=%2Fbook", "/login?e=1",
                                         "/login?e=2&w=120", "/login?e=2&w=30", "/login?e=2&w=9&busy=1",
                                         "/login?e=2&w=1&busy=1", "/login?e=3", "/login?e=4", "/login?e=5"};
    bad += untranslated(signin, countof(signin), &r);
    CHECK_HAS(th_page(0, "/login?e=5"), "ask Anna (admin) for more days"); /* (English as it was) */

    /* forgot your password? */
    static const char *const forgot[] = {"/forgot", "/forgot?e=code", "/forgot?e=short", "/forgot?e=long",
                                         "/forgot?e=again", "/forgot?e=wait&w=120"};
    bad += untranslated(forgot, countof(forgot), &r);

    /* "Welcome back" - and the way to someone else */
    g_guess = with_login("Eva", "eva", "eva password", ROLE_MEMBER);
    presence_set_guesser(guess_who);
    CHECK_HAS(th_page(0, "/login"), "Welcome back, Eva");
    static const char *const back[] = {"/login", "/login?next=%2Fbook", "/login?other=1"};
    bad += untranslated(back, countof(back), &r);
    app_test_dev_lang(LANG_SV);
    CHECK_HAS(th_page(0, "/login"), "Välkommen tillbaka, Eva");
    CHECK_HAS(th_page(0, "/login"), "Inte Eva? <a href=\"/login?other=1&amp;next=%2F\">Logga in som någon annan</a>");
    app_test_dev_lang(LANG_ES);
    CHECK_HAS(th_page(0, "/login"), "¿No eres Eva? <a href=\"/login?other=1&amp;next=%2F\">");
    app_test_dev_lang(-1);
    presence_set_guesser(NULL);

    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);
    auth_configure("", 1000);
    th_halt();
}

TEST(auth_signed_out_pages_link_to_the_other_languages) {
    th_boot();
    auth_configure("pepper", 1000);
    with_login("Anna", "anna", "correct horse", ROLE_ADMIN);
    /* English: Svenska and Español are links back to this page, English is plain text */
    const char *h = th_page(0, "/book?w=2026-10-12");
    CHECK_HAS(h, "<nav class=\"muted small center\" aria-label=\"Language\">");
    CHECK_HAS(h, "<a href=\"/lang?to=sv&amp;next=%2Fbook%3Fw%3D2026-10-12\" lang=\"sv\" hreflang=\"sv\" data-native");
    CHECK_HAS(h, "<a href=\"/lang?to=es&amp;next=%2Fbook%3Fw%3D2026-10-12\" lang=\"es\" hreflang=\"es\" data-native");
    CHECK_HAS(h, ">Svenska</a>");
    CHECK_HAS(h, ">Español</a>");
    CHECK_HAS(h, "<span lang=\"en\" aria-current=\"true\"");
    CHECK_HAS(h, ">English</span>");
    CHECK(!strstr(h, "/lang?to=en"));
    /* Swedish (the browser's): English and Español are links, Svenska isn't - and the label is Swedish */
    app_test_dev_lang(LANG_SV);
    h = th_page(0, "/login");
    CHECK_HAS(h, "aria-label=\"Språk\"");
    CHECK_HAS(h, "<a href=\"/lang?to=en&amp;next=%2Flogin\" lang=\"en\"");
    CHECK_HAS(h, "<a href=\"/lang?to=es&amp;next=%2Flogin\" lang=\"es\"");
    CHECK_HAS(h, "<span lang=\"sv\" aria-current=\"true\"");
    CHECK(!strstr(h, "/lang?to=sv"));
    app_test_dev_lang(-1);
    /* an error or a typed name isn't carried back (issue #100: no names in addresses); where to go next is */
    h = th_page(0, "/login?e=1&u=anna&next=%2Fplan");
    CHECK_HAS(h, "/lang?to=sv&amp;next=%2Flogin%3Fnext%3D%252Fplan\"");
    CHECK(!strstr(h, "anna"));
    CHECK_HAS(th_page(0, "/forgot?e=code"), "/lang?to=es&amp;next=%2Fforgot\"");
    /* on every signed-out page: the welcome page keeps what was typed (its address carries it anyway) */
    tbl_del(&T_person, person_by_username("anna")->id);
    CHECK(auth_setup_open());
    h = th_page(0, "/setup?e=again&name=Alex+D&u=alex&home=Villa");
    CHECK_HAS(h, "/lang?to=sv&amp;next=%2Fsetup%3Fname%3DAlex%2520D%26u%3Dalex%26home%3DVilla\"");
    setting_set(SETTING_RESCUED, "damaged");
    CHECK_HAS(th_page(0, "/"), "/lang?to=sv&amp;next=%2F\""); /* the damaged page */
    setting_set(SETTING_RESCUED, "");
    setting_set_int("auth.bootstrapped", 1);
    CHECK_HAS(th_page(0, "/"), "Nobody can sign in");
    CHECK_HAS(th_page(0, "/"), "/lang?to=es&amp;next=%2F\"");
    auth_configure("", 1000);
    CHECK_HAS(th_page(0, "/"), "hemma --gen-salt");
    CHECK_HAS(th_page(0, "/"), "/lang?to=sv&amp;next=%2F\"");
    th_halt();
}

/* A password's problem is told in the language in use (people's forms show it as it is); at the start,
   where the log gets it, that's English. */
TEST(auth_password_problems_in_the_language_in_use) {
    Lang was = i18n_use(LANG_SV);
    CHECK_STR(auth_password_problem("short"), "Använd minst 8 tecken");
    i18n_use(LANG_ES);
    CHECK_STR(auth_password_problem("short"), "Usa al menos 8 caracteres");
    i18n_use(LANG_EN);
    CHECK_STR(auth_password_problem("short"), "Use at least 8 characters");
    CHECK(auth_password_problem("long enough") == NULL);
    i18n_use(was);
}
