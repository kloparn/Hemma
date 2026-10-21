/* Screens in Swedish and Spanish (issue #46): the list and its dialog, the "On the TV" steps, a TV's
 * "Who's watching?" and its PIN, a TV screen, walls (the clock band, now and next, who's home, all quiet)
 * and the builder - as an admin, members and display accounts - with nothing left in English but names. */
#include "../../core/auth.h"
#include "../../tests/harness.h"
#include "screens.h"

/* What other parts of Hemma show around these pages - the shell, a dialog's Close, the colour picker, the
   player, Watch's tabs and search, other modules' names and rows - is theirs to translate: allowed here
   only while this build has no translation for that part (the gate: a word of its own), so this test
   reads this part's words now, and every word once all parts are translated. */
static bool untranslated(const char *en) { return !strcmp(T_in(LANG_SV, en), en); }

static int allow_others(const char **out, int n, int max) {
    static const struct {
        const char *gate, *text;
    } fixed[] = {
        {"Skip to content", "Skip to content"}, {"Skip to content", "More"}, {"Skip to content", "Here now"},
        {"Skip to content", "Close"},
        {"Amber", "Colour"}, {"Amber", "Red"}, {"Amber", "Orange"}, {"Amber", "Amber"}, {"Amber", "Lime"},
        {"Amber", "Green"}, {"Amber", "Teal"}, {"Amber", "Sky"}, {"Amber", "Blue"}, {"Amber", "Indigo"},
        {"Amber", "Purple"}, {"Amber", "Magenta"}, {"Amber", "Pink"},
        {"Volume", "Volume"}, {"Volume", "Live"}, {"Volume", "Sound off"}, {"Volume", "Sound on"},
        {"Volume", "Full screen"}, {"Volume", "Full screen (F)"}, {"Volume", "Mute (M)"}, {"Volume", "Sound (M)"},
        {"Volume", "Play"}, {"Volume", "Play (Space)"}, {"Volume", "Pause (Space)"},
        {"Volume", "Back 10 seconds (Left)"}, {"Volume", "Forward 10 seconds (Right)"}, {"Volume", "Subtitles"},
        {"Library settings", "Library settings"}, {"Library settings", "Search the library"},
        {"Library settings", "Search movies and series…"}, {"Library settings", "Movies"},
        {"Library settings", "Series"}, {"Library settings", "Folders"}, {"Library settings", "Live TV"},
        {"Library settings", "Back"}, /* (TV mode's Back: media_tv_open) */
    };
    for (int i = 0; i < countof(fixed) && n < max; i++)
        if (untranslated(fixed[i].gate)) out[n++] = fixed[i].text;
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (str_eq(m->id, "screens") || str_eq(m->id, "tv") || str_eq(m->id, "net")) continue;
        if (n < max && untranslated(m->title)) out[n++] = m->title;
        for (const Block *bl = m->blocks; bl && bl->id && n < max; bl++)
            if (untranslated(bl->title)) out[n++] = bl->title;
    }
    return n;
}

static int with_role(const char *name, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    tbl_put(&T_person, &p);
    if (role == ROLE_DISPLAY) th_act(id, "screens.viewer", "who=0"); /* (the harness keeps a pick across tests) */
    return id;
}

/* An action's toast (in the actor's language) */
static const char *toast(int person, const char *action, const char *form) {
    static ThResult r;
    r = th_act(person, action, form);
    return r.toast;
}

static int new_screen(int admin, const char *form) {
    ThResult r = th_act(admin, "screens.create", form);
    return r.ok && str_starts(r.redirect, "/screens/") ? (int)strtol(r.redirect + 9, NULL, 10) : 0;
}

static bool add(int admin, int screen, const char *block) {
    char form[120];
    snprintf(form, sizeof form, "id=%d&block=%s", screen, block);
    return th_act(admin, "screens.row_add", form).ok;
}

static int untranslated_on(int person, const char *const *urls, int n, const char *const *allow, Buf *r) {
    int bad = 0;
    for (int i = 0; i < n; i++) bad += th_untranslated(person, urls[i], allow, r);
    return bad;
}

TEST(screens_speak_swedish_and_spanish) {
    th_boot();
    auth_configure("screens-i18n-salt", 1000); /* (PINs are kept as an HMAC) */
    g_fake_now = dt_make(2026, 10, 10, 13, 5); /* a Saturday */
    screens_test_addresses("http://192.168.1.50:8080", "http://hemma.local:8080");
    int anna = with_role("Anna", ROLE_ADMIN), jonas = th_person("Jonas"), maja = th_person("Maja");
    const char *allow[160] = {"Anna", "Jonas", "Maja", "Living room TV", "Hall tablet", "Kids", "Hall", "Kitchen",
                              "Upstairs", "Cartoons", "gone.block", "Car", "dentist", "Dentist", "party", "Party",
                              "localhost", "http://192.168.1.50:8080", "http://hemma.local:8080",
                              "A", "J", "M", "HT", "LR"}; /* (people's, screens' and things' names; avatars' letters) */
    int na = 0;
    while (allow[na]) na++;
    allow_others(allow, na, countof(allow) - 1);
    Buf r = {0};
    int bad = 0;
    char u[12][64];

    /* the seeded wall gets a name of the household's (its seeded name is data too) */
    snprintf(u[0], sizeof u[0], "id=%d&name=Upstairs", screens_first(SCREEN_WALL));
    CHECK(th_act(anna, "screens.save", u[0]).ok);

    /* a TV screen of our own: On TV (nothing on the air yet: left out), a heading of its own, a row whose
       module is gone; an empty TV screen; a wall with no cards; a wall whose only card is empty */
    int kids = new_screen(anna, "name=Kids&kind=tv"), empty = new_screen(anna, "name=Kitchen&kind=tv");
    int hall = new_screen(anna, "name=Hall&kind=wall"), quiet = new_screen(anna, "name=Cartoons&kind=wall");
    CHECK(kids && empty && hall && quiet);
    CHECK(add(anna, kids, "tv.onnow"));
    CHECK(add(anna, kids, "tv.onnow"));
    CHECK(add(anna, quiet, "tv.onnow"));
    ScreenRow *rows[SCREENS_MAX_ROWS];
    CHECK_INT(screen_rows(kids, rows, SCREENS_MAX_ROWS), 2);
    snprintf(u[0], sizeof u[0], "row=%d&title=Cartoons", rows[1]->id);
    CHECK(th_act(anna, "screens.row_save", u[0]).ok);
    ScreenRow gone = {0};
    gone.screen = kids;
    gone.pos = 9;
    str_copy(gone.block, sizeof gone.block, "gone.block");
    tbl_put(&T_screen_row, &gone);

    /* the builder before there are display accounts: how a TV signs in */
    snprintf(u[0], sizeof u[0], "/screens/%d/edit", kids);
    bad += th_untranslated(anna, u[0], allow, &r);

    int tv = with_role("Living room TV", ROLE_DISPLAY), tablet = with_role("Hall tablet", ROLE_DISPLAY);
    snprintf(u[0], sizeof u[0], "id=%d&acc=%d", kids, tv);
    CHECK(th_act(anna, "screens.shows", u[0]).ok);
    snprintf(u[0], sizeof u[0], "id=%d&acc=%d", hall, tablet);
    CHECK(th_act(anna, "screens.shows", u[0]).ok);
    CHECK(th_act(maja, "screens.pin", "pin=1234").ok);

    /* now and next on a wall: the car out now, the dentist later, a party tomorrow; who's home */
    CHECK(th_act(anna, "book.preset", "name=Car").ok);
    g_fake_now = dt_make(2026, 10, 10, 11, 0);
    CHECK(th_act(anna, "book.create", "res=1&date=2026-10-10&from=12:00&to=15:00").ok);
    g_fake_now = dt_make(2026, 10, 10, 13, 5);
    CHECK(th_act(anna, "plan.event_add", "q=dentist+today+16%3A30").ok);
    CHECK(th_act(anna, "plan.event_add", "q=party+tomorrow").ok);
    presence_set_home(jonas, true, g_fake_now);
    presence_set_home(maja, false, g_fake_now);

    /* an admin: the list (the big address and its QR code), New screen, every screen and builder */
    snprintf(u[0], sizeof u[0], "/screens/%d", kids);
    snprintf(u[1], sizeof u[1], "/screens/%d/edit", kids);
    snprintf(u[2], sizeof u[2], "/screens/%d", empty);
    snprintf(u[3], sizeof u[3], "/screens/%d/edit", empty);
    snprintf(u[4], sizeof u[4], "/screens/%d", hall);
    snprintf(u[5], sizeof u[5], "/screens/%d/edit", hall);
    snprintf(u[6], sizeof u[6], "/screens/%d", quiet);
    snprintf(u[7], sizeof u[7], "/screens/%d/edit", quiet);
    const char *admin_urls[] = {"/screens", "/screens?new=1", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                                "/screens/999"};
    bad += untranslated_on(anna, admin_urls, countof(admin_urls), allow, &r);
    /* ... the other two ways to say where to open Hemma */
    screens_test_addresses("", NULL);
    bad += th_untranslated(anna, "/screens", allow, &r);
    app_test_host("localhost");
    bad += th_untranslated(anna, "/screens", allow, &r);
    app_test_host("*");
    screens_test_addresses(NULL, NULL);

    /* members: their place on the TV, with and without a PIN; a builder that isn't theirs */
    const char *member_urls[] = {"/screens", u[0], u[1], u[4]};
    bad += untranslated_on(jonas, member_urls, countof(member_urls), allow, &r);
    bad += th_untranslated(maja, "/screens", allow, &r);

    /* the TV: Who's watching?, Maja's PIN, too many wrong ones, then someone watching */
    snprintf(u[8], sizeof u[8], "/screens/%d?pin=%d", kids, maja);
    bad += th_untranslated(tv, u[0], allow, &r);
    bad += th_untranslated(tv, u[8], allow, &r);
    snprintf(u[9], sizeof u[9], "who=%d&pin=0000", maja);
    for (int i = 0; i < 5; i++) CHECK(!th_act(tv, "screens.viewer", u[9]).ok);
    bad += th_untranslated(tv, u[8], allow, &r);
    snprintf(u[9], sizeof u[9], "who=%d", jonas);
    CHECK(th_act(tv, "screens.viewer", u[9]).ok);
    bad += th_untranslated(tv, u[0], allow, &r);
    /* the hall tablet: its own wall at "/", and another */
    bad += th_untranslated(tablet, "/", allow, &r);
    bad += th_untranslated(tablet, u[6], allow, &r);

    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);

    /* the wall's band in Swedish and Spanish: the date (lower case names, starting the line), now and next */
    pref_set(tablet, PREF_LANG, "sv");
    const char *h = th_page(tablet, u[4]);
    CHECK_HAS(h, "Lördag 10 oktober · vecka 41");
    CHECK_HAS(h, "<span class=\"screens-wn-when\">Nu</span>");
    CHECK_HAS(h, "<small>till 15:00</small>");
    CHECK_HAS(h, "<span class=\"screens-wn-when\">Imorgon </span>");
    CHECK_HAS(h, "<small>hemma</small>");
    CHECK_HAS(h, "<small>borta</small>");
    pref_set(tablet, PREF_LANG, "es");
    h = th_page(tablet, u[4]);
    CHECK_HAS(h, "Sábado 10 de octubre · semana 41");
    CHECK_HAS(h, "<small>hasta las 15:00</small>");
    pref_set(tablet, PREF_LANG, "");

    /* toasts are in the actor's language */
    pref_set(anna, PREF_LANG, "sv");
    CHECK_STR(toast(anna, "screens.create", "name="), "Ge skärmen ett namn");
    snprintf(u[9], sizeof u[9], "id=%d&block=tv.onnow", empty);
    CHECK_STR(toast(anna, "screens.row_add", u[9]), "Lade till På TV");
    CHECK_STR(toast(jonas, "screens.pin", "pin=12"), "A PIN is 4 digits"); /* (Jonas reads English) */
    pref_set(jonas, PREF_LANG, "es");
    CHECK_STR(toast(jonas, "screens.pin", "pin=12"), "Un PIN tiene 4 cifras");
    pref_set(jonas, PREF_LANG, "");
    pref_set(anna, PREF_LANG, "");
    g_fake_now = 0;
    th_halt();
}
