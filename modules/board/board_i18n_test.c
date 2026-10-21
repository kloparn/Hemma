/* The board in Swedish and Spanish (issue #46): modules/board's page, its dialogs, hidden notes, the dashboard's
   card and the wall's notes - with notes of every kind (pinned, a photo, one until a day, edited, hidden). */
#include "../../core/auth.h"
#include "../../core/codec.h"
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

/* ------------------------------------------------ notes of every kind */

/* a JPEG in the fewest bytes that still say its size (as board_test.c's) */
static const uint8_t TINY_JPEG[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J',  'F',  'I',  'F',  0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x0A, 0x00, 0x14, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
    0xFF, 0xD9,
};

/* form + "&photo=" + the photo as a data: URL, as wire.js sends it */
static const char *with_photo(const char *form) {
    static Buf b;
    buf_reset(&b);
    char *enc = xmalloc(4 * ((sizeof TINY_JPEG + 2) / 3) + 1);
    base64_encode(TINY_JPEG, sizeof TINY_JPEG, enc);
    Buf url = {0};
    buf_printf(&url, "data:image/jpeg;base64,%s", enc);
    buf_puts(&b, form);
    buf_puts(&b, "&photo=");
    url_encode(&b, url.p, false);
    free(enc);
    buf_free(&url);
    return b.p;
}

static const char *const ALLOW[] = {"Anna", "Eva", "Leo", "Milk", "Dentist", "Football", "Lunch", NULL};

static void check_pages(int person, const char *const *urls, size_t n) {
    Buf r = {0};
    int bad = 0;
    for (size_t i = 0; i < n; i++) bad += untranslated(person, urls[i], ALLOW, &r);
    if (bad) printf("%s", buf_cstr(&r));
    CHECK_INT(bad, 0);
    buf_free(&r);
}

TEST(board_speaks_swedish_and_spanish) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir()); /* (photos are kept in the data folder) */
    th_boot_in(dir);
    auth_configure("test-server-salt", 1000);
    int anna = th_person("Anna");
    Person p = *person_get(anna);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    int eva = th_person("Eva");
    int leo = th_person("Leo");
    p = *person_get(leo);
    p.role = ROLE_CHILD;
    tbl_put(&T_person, &p);

    /* nothing yet: the empty board and the dashboard's empty card */
    const char *const empty[] = {"/board", "/", "/board?expired=1"};
    check_pages(anna, empty, countof(empty));

    /* one that's past its date (posted two days ago for a day), then the rest */
    int64_t now = now_sec();
    g_fake_now = now - 2 * 86400;
    CHECK(th_act(eva, "board.post", "text=Lunch&keep=1d").ok);                           /* 1: hidden */
    g_fake_now = now - 3600;
    CHECK(th_act(eva, "board.post", "text=Milk&hue=200&keep=1d").ok);                    /* 2: until tomorrow */
    g_fake_now = 0;
    CHECK(th_act(anna, "board.post", "text=Dentist&pin=1&keep=7d").ok);                  /* 3: pinned */
    CHECK(th_act(anna, "board.post", with_photo("text=&hue=45")).ok);                    /* 4: a photo only */
    CHECK(th_act(leo, "board.post", with_photo("text=Football&hue=90&keep=date&until=2099-06-01")).ok); /* 5 */
    CHECK(th_act(anna, "board.save", "id=2&text=Milk&hue=200&keep=keep").ok);            /* edited by another */

    const char *const admin[] = {"/board",           "/board?edit=2", "/board?edit=3",           "/board?edit=4",
                                 "/board?edit=5",    "/",             "/board?expired=1&edit=1", "/board?expired=1"};
    check_pages(anna, admin, countof(admin));
    const char *const child[] = {"/board", "/board?edit=5", "/"}; /* (a child changes only their own) */
    check_pages(leo, child, countof(child));

    /* the wall's notes */
    const Module *owner = NULL;
    const Block *bl = block_find("board.notes", &owner);
    CHECK(bl && owner);
    CHECK_STR(T_in(LANG_SV, bl->title), "Lappar");
    CHECK_STR(T_in(LANG_ES, bl->title), "Notas");
    pref_set(anna, PREF_LANG, "es");
    Ctx c;
    app_ctx(&c, anna, "/");
    Buf b = {0};
    CHECK(bl->render(&c, &b, SCREEN_WALL, ""));
    app_ctx_free(&c);
    CHECK_HAS(buf_cstr(&b), "alt=\"Foto\"");
    buf_free(&b);
    pref_set(anna, PREF_LANG, "");
    th_halt();
}

/* Toasts in the actor's language; the Latest feed in each reader's, its English kept for /api */
TEST(board_says_it_in_the_readers_language) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    th_boot_in(dir);
    int anna = th_person("Anna");
    int eva = th_person("Eva");
    pref_set(anna, PREF_LANG, "sv");
    ThResult r = th_act(anna, "board.post", with_photo("text=&hue=45"));
    CHECK_STR(r.toast, "Uppsatt");
    const Activity *a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "Anna put a photo on the board");
    Buf t = {0};
    Lang was = i18n_use(LANG_SV);
    activity_text(a, &t);
    CHECK_STR(buf_cstr(&t), "Anna satte upp ett foto på anslagstavlan");
    i18n_use(was);
    CHECK(th_act(eva, "board.save", "id=1&text=&hue=45&keep=keep").ok); /* (the photo stays: a photo's note) */
    a = tbl_at(&T_activity, tbl_count(&T_activity) - 1);
    CHECK_STR(a->text, "Eva edited Anna's note: “the photo”");
    buf_reset(&t);
    was = i18n_use(LANG_ES);
    activity_text(a, &t);
    CHECK_STR(buf_cstr(&t), "Eva editó la nota de Anna: “la foto”");
    i18n_use(was);
    buf_free(&t);
    r = th_act(anna, "board.post", "text=");
    CHECK_STR(r.toast, "Skriv något först, eller lägg till ett foto");
    r = th_act(anna, "board.pin", "id=1");
    CHECK_STR(r.toast, "Fäst");
    pref_set(anna, PREF_LANG, "es");
    r = th_act(anna, "board.delete", "id=1");
    CHECK_STR(r.toast, "Quitado");
    th_halt();
}
