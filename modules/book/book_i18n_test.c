/* Bookings in Swedish and Spanish (issue #46): the overview, a thing's week and its month of days, the booking
 * and thing dialogs (an admin's rules too), news of what others did, repeating bookings - read as an admin, a
 * member, a child and a guest, with nothing left in English but the household's own words. */
#include "../../tests/harness.h"
#include "../../core/ui.h"
#include "book.h"

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

static int thing(const char *name) {
    for (int i = 0; i < tbl_count(&T_resource); i++) {
        const Resource *r = tbl_at(&T_resource, i);
        if (str_eq(r->name, name)) return r->id;
    }
    return 0;
}

static int booking_at(int res, int64_t start) {
    for (int i = 0; i < tbl_count(&T_booking); i++) {
        const Booking *b = tbl_at(&T_booking, i);
        if (b->res == res && b->start == start) return b->id;
    }
    return 0;
}

static int put_booking(int res, int who, int by, int64_t start, int64_t end) {
    Booking b = {.res = res, .who = who, .by = by, .start = start, .end = end, .created = NOW - 2 * 86400};
    tbl_put(&T_booking, &b);
    return b.id;
}

/* the household's own words: names (and their avatars' initials), things, a note, the starters' rules as added */
static const char *const ALLOW[] = {"Anna", "Erik", "Leo", "Gun", "A", "E", "L", "G", "Car", "Laundry", "Guest", "room",
                                    "Sauna", "ikea", "Fill up when it's below a quarter tank",
                                    "Clean the lint filter afterwards", NULL};

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

TEST(book_speaks_swedish_and_spanish) {
    th_boot();
    g_fake_now = NOW;
    int anna = with_role(th_person("Anna"), ROLE_ADMIN), erik = th_person("Erik");
    int leo = with_role(th_person("Leo"), ROLE_CHILD), gun = with_role(th_person("Gun"), ROLE_GUEST);
    Buf r = {0};
    int bad = 0;

    /* nothing to book yet: the starters */
    static const char *const empty[] = {"/book", "/book?res=new"};
    bad += untranslated(anna, empty, countof(empty), &r);
    static const char *const empty_child[] = {"/book"};
    bad += untranslated(leo, empty_child, countof(empty_child), &r);

    static const char *const starters[] = {"name=Car", "name=Laundry", "name=Guest+room", "name=Sauna"};
    for (int i = 0; i < countof(starters); i++) CHECK(th_act(anna, "book.preset", starters[i]).ok);
    int car = thing("Car"), laundry = thing("Laundry"), guest = thing("Guest room"), sauna = thing("Sauna");
    CHECK(car && laundry && guest && sauna);
    Resource lr = *res_get(laundry); /* only theirs to change */
    lr.own_only = true;
    tbl_put(&T_resource, &lr);

    /* the car is in use now; Friday's is Erik's; the guest room over the weekend; Leo's sauna */
    put_booking(car, erik, erik, dt_make(2026, 10, 7, 9, 0), dt_make(2026, 10, 7, 12, 0));
    CHECK(th_act(erik, "book.quick", "q=car+fri+14-17+ikea").ok);
    CHECK(th_act(erik, "book.quick", "q=guest+room+fri-sun").ok);
    CHECK(th_act(leo, "book.quick", "q=sauna+fri+18-19").ok);
    /* Erik has the laundry on Tuesday 27th (Anna booked it for him); Anna's repeating Tuesdays leave that one out
       (her news) */
    put_booking(laundry, erik, anna, dt_make(2026, 10, 27, 18, 0), dt_make(2026, 10, 27, 21, 0));
    CHECK(th_act(anna, "book.quick", "q=laundry+every+tuesday+18-21").ok);
    /* Anna moves Erik's Friday car and cancels his guest room: his news (in his own Swedish) */
    pref_set(erik, PREF_LANG, "sv");
    int fri_car = booking_at(car, dt_make(2026, 10, 9, 14, 0));
    char f[200];
    snprintf(f, sizeof f, "res=%d&id=%d&date=2026-10-09&from=15:00&to=18:00&who=%d", car, fri_car, erik);
    CHECK(th_act(anna, "book.create", f).ok);
    snprintf(f, sizeof f, "id=%d", booking_at(guest, dt_make(2026, 10, 9, 0, 0)));
    CHECK(th_act(anna, "book.cancel", f).ok);
    CHECK(th_act(erik, "book.quick", "q=guest+room+fri-sun").ok); /* (back: the month shows one) */
    pref_set(erik, PREF_LANG, "");
    for (int i = 0; i < tbl_count(&T_book_news); i++) { /* seen a few days later: "Sunday", not "just now" */
        BookNews n = *(BookNews *)tbl_at(&T_book_news, i);
        n.ts = NOW - 3 * 86400;
        tbl_put(&T_book_news, &n);
    }
    CHECK(tbl_count(&T_book_news) >= 3);

    int series_bk = booking_at(laundry, dt_make(2026, 10, 20, 18, 0));
    int car_now = booking_at(car, dt_make(2026, 10, 7, 9, 0));
    int erik_laundry = booking_at(laundry, dt_make(2026, 10, 27, 18, 0));
    int leo_sauna = booking_at(sauna, dt_make(2026, 10, 9, 18, 0));
    CHECK(series_bk && car_now && erik_laundry && leo_sauna);
    char u[16][80];
    snprintf(u[0], 80, "/book?res=%d", car);
    snprintf(u[1], 80, "/book/%d", car);
    snprintf(u[2], 80, "/book/%d?res=%d", car, car);
    snprintf(u[3], 80, "/book/%d?b=%d", car, car_now);
    snprintf(u[4], 80, "/book/%d?b=%d&edit=1", car, car_now);
    snprintf(u[5], 80, "/book/%d?w=2026-10-07&new=%lld", car, (long long)dt_make(2026, 10, 8, 10, 0));
    snprintf(u[6], 80, "/book/%d?w=2026-10-20&b=%d", laundry, series_bk);
    snprintf(u[7], 80, "/book/%d", guest);
    snprintf(u[8], 80, "/book/%d?w=2026-10-07&new=%lld", guest, (long long)dt_make(2026, 10, 14, 0, 0));
    snprintf(u[9], 80, "/book/%d?w=2026-10-21", car);
    snprintf(u[10], 80, "/book/%d?res=%d", laundry, laundry);
    snprintf(u[11], 80, "/book/%d?w=2026-10-20&b=%d", laundry, series_bk);
    snprintf(u[12], 80, "/book/%d?w=2026-10-27&b=%d", laundry, erik_laundry);
    snprintf(u[13], 80, "/book/%d", sauna);
    snprintf(u[14], 80, "/book/%d?w=2026-10-09&b=%d", sauna, leo_sauna);
    snprintf(u[15], 80, "/book/%d?w=2026-10-07&new=%lld", sauna, (long long)dt_make(2026, 10, 10, 18, 0));
    const char *admin[] = {"/book", "/book?res=new", u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9]};
    bad += untranslated(anna, admin, countof(admin), &r);
    const char *member[] = {"/book", u[1], u[10], u[11]}; /* news; Anna's laundry: hers to change */
    bad += untranslated(erik, member, countof(member), &r);
    const char *child[] = {"/book", u[13], u[14], u[15], u[1]}; /* the sauna; the car isn't for children */
    bad += untranslated(leo, child, countof(child), &r);
    const char *guest_urls[] = {"/book", u[1], u[12]}; /* the car isn't open to guests; Erik's laundry by Anna */
    bad += untranslated(gun, guest_urls, countof(guest_urls), &r);

    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);
    g_fake_now = 0;
    th_halt();
}

/* Kept news reads in whoever reads it's language; a line from an older Hemma (plain text) as it was written */
TEST(book_news_is_kept_in_any_language) {
    th_boot();
    g_fake_now = NOW;
    int anna = th_person("Anna"), erik = th_person("Erik");
    CHECK(th_act(anna, "book.preset", "name=Car").ok);
    CHECK(th_act(erik, "book.quick", "q=car+fri+14-17").ok);
    char f[200];
    snprintf(f, sizeof f, "id=%d", booking_at(1, dt_make(2026, 10, 9, 14, 0)));
    CHECK(th_act(anna, "book.cancel", f).ok);
    CHECK_INT(tbl_count(&T_book_news), 1);
    const BookNews *n = tbl_at(&T_book_news, 0);
    CHECK_HAS(n->text, "Anna cancelled your Car booking · Friday 14:00–17:00"); /* (English: /api) */
    CHECK(n->msg[0]);
    pref_set(erik, PREF_LANG, "sv");
    CHECK_HAS(th_page(erik, "/book"), "Anna avbokade din bokning av Car");
    pref_set(erik, PREF_LANG, "es");
    CHECK_HAS(th_page(erik, "/book"), "Anna canceló tu reserva de Car");
    BookNews old = {.person = erik, .ts = NOW, .link = "/book"};
    str_copy(old.text, sizeof old.text, "Adam cancelled your Sauna booking");
    tbl_put(&T_book_news, &old);
    CHECK_HAS(th_page(erik, "/book"), "Adam cancelled your Sauna booking");
    /* what quick booking and the actions say: in the language of whoever typed */
    pref_set(anna, PREF_LANG, "es");
    CHECK_STR(toast(anna, "book.quick", "q=boat+fri+10-12"), "No conozco «boat». Puedes reservar: Car.");
    CHECK_STR(toast(anna, "book.quick", "q=car"), "¿Cuándo? Añade un día y una hora, p. ej. «Car viernes de 14 a 17»");
    CHECK_STR(toast(anna, "book.quick", "q=car+fri+14-17"), "Reservado: Car · viernes 14:00–17:00");
    CHECK_HAS(toast(anna, "book.quick", "q=car+fri+15-16"), "Anna tiene Car viernes 14:00–17:00");
    /* a starter is named in the words of whoever adds it */
    pref_set(anna, PREF_LANG, "sv");
    CHECK(th_act(anna, "book.preset", "name=Laundry").ok);
    CHECK(thing("Tvättstuga") != 0);
    CHECK_STR(res_get(thing("Tvättstuga"))->rules, "Rensa luddfiltret efteråt");
    Quick q; /* (and still found by its Swedish starter words) */
    book_quick_parse("tvätt fre 10-12", 0, NOW, &q);
    CHECK_INT(q.res, thing("Tvättstuga"));
    g_fake_now = 0;
    th_halt();
}

/* The wall's card (no address of its own: drawn here in each language) */
TEST(book_wall_card_speaks_swedish_and_spanish) {
    th_boot();
    g_fake_now = NOW;
    int anna = th_person("Anna");
    CHECK(th_act(anna, "book.preset", "name=Car").ok);
    CHECK(th_act(anna, "book.preset", "name=Sauna").ok);
    put_booking(thing("Car"), anna, anna, dt_make(2026, 10, 7, 9, 0), dt_make(2026, 10, 8, 2, 0));
    Ctx c;
    app_ctx(&c, anna, "/screens/2");
    const Block *bl = block_find("book.now", NULL);
    CHECK(bl != NULL);
    static const char *const want[3][2] = {{"until Tomorrow 02:00", ">Free<"},
                                           {"till imorgon 02:00", ">Ledig<"},
                                           {"hasta mañana a las 02:00", ">Libre<"}};
    for (int l = 0; bl && l < 3; l++) {
        Lang was = i18n_use(l == 0 ? LANG_EN : l == 1 ? LANG_SV : LANG_ES);
        Buf b = {0};
        bl->render(&c, &b, SCREEN_WALL, NULL);
        CHECK_HAS(buf_cstr(&b), want[l][0]);
        CHECK_HAS(buf_cstr(&b), want[l][1]);
        buf_free(&b);
        i18n_use(was);
    }
    app_ctx_free(&c);
    g_fake_now = 0;
    th_halt();
}

TEST(book_a_starter_answers_to_its_name_in_every_language) {
    g_fake_now = NOW;
    th_boot();
    int adam = with_role(th_person("Adam"), ROLE_ADMIN);
    CHECK(th_act(adam, "book.preset", "name=Car").ok); /* (added in English: named "Car") */
    int car = thing("Car");
    CHECK(car > 0);
    pref_set(adam, PREF_LANG, "es");
    CHECK(th_act(adam, "book.quick", "q=coche+mañana+de+14+a+17").ok);
    CHECK(booking_at(car, dt_make(2026, 10, 8, 14, 0)) > 0);
    pref_set(adam, PREF_LANG, "sv");
    CHECK(th_act(adam, "book.quick", "q=bil+fre+14-17").ok);
    CHECK(booking_at(car, dt_make(2026, 10, 9, 14, 0)) > 0);
    pref_set(adam, PREF_LANG, "");
    th_halt();
    g_fake_now = 0;
}
