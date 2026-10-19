#include "../../core/codec.h"
#include "../../tests/harness.h"

static Activity *last_activity(void) {
    int n = tbl_count(&T_activity);
    return n ? tbl_at(&T_activity, n - 1) : NULL;
}

TEST(board_post_and_show) {
    th_boot();
    int adam = th_person("Adam");
    ThResult r = th_act(adam, "board.post", "text=Milk+is+in+the+fridge&hue=45");
    CHECK(r.ok);
    const char *html = th_page(adam, "/board");
    CHECK_HAS(html, "Milk is in the fridge");
    CHECK_HAS(html, "<b>Adam</b>");
    CHECK_HAS(html, "1 note");
    CHECK_HAS(html, "--h:45");
    CHECK_HAS(last_activity()->text, "Adam left a note: “Milk is in the fridge”");
    th_halt();
}

TEST(board_rejects_empty_and_too_long_notes) {
    th_boot();
    int adam = th_person("Adam");
    CHECK(!th_act(adam, "board.post", "text=%20%0A%20").ok);
    CHECK(!th_act(adam, "board.post", "hue=10").ok);
    Buf form = {0};
    buf_puts(&form, "text=");
    for (int i = 0; i < 600; i++) buf_puts(&form, "%C3%A5"); /* 600 characters, 1200 bytes */
    CHECK(th_act(adam, "board.post", buf_cstr(&form)).ok);
    buf_puts(&form, "x");
    ThResult r = th_act(adam, "board.post", buf_cstr(&form));
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "601");
    buf_free(&form);
    th_halt();
}

TEST(board_pinned_first_then_newest) {
    th_boot();
    int adam = th_person("Adam");
    g_fake_now = dt_make(2026, 10, 7, 9, 0);
    CHECK(th_act(adam, "board.post", "text=first+note").ok);
    g_fake_now += 60;
    CHECK(th_act(adam, "board.post", "text=second+note").ok);
    g_fake_now += 60;
    CHECK(th_act(adam, "board.post", "text=third+note").ok);
    CHECK(th_act(adam, "board.pin", "id=1").ok);
    const char *html = th_page(adam, "/board");
    const char *p1 = strstr(html, "first note"), *p2 = strstr(html, "second note"), *p3 = strstr(html, "third note");
    CHECK(p1 && p2 && p3);
    CHECK(p1 < p3 && p3 < p2); /* pinned, then newest first */
    CHECK_HAS(html, "3 notes · 1 pinned");
    CHECK(th_act(adam, "board.pin", "id=1").ok); /* unpin */
    html = th_page(adam, "/board");
    p1 = strstr(html, "first note"), p2 = strstr(html, "second note"), p3 = strstr(html, "third note");
    CHECK(p3 < p2 && p2 < p1);
    CHECK(!strstr(html, "1 pinned"));
    th_halt();
}

TEST(board_expired_notes_hide_and_can_come_back) {
    th_boot();
    int adam = th_person("Adam");
    g_fake_now = dt_make(2026, 10, 7, 9, 0);
    CHECK(th_act(adam, "board.post", "text=Plumber+on+Tuesday&keep=1d").ok);
    CHECK(th_act(adam, "board.post", "text=Forever+note&keep=0").ok);
    const char *html = th_page(adam, "/board");
    CHECK_HAS(html, "Plumber on Tuesday");
    CHECK_HAS(html, "until tomorrow 09:00");
    g_fake_now += 2 * 86400;
    html = th_page(adam, "/board");
    CHECK(!strstr(html, "Plumber on Tuesday"));
    CHECK_HAS(html, "Forever note");
    CHECK_HAS(html, "1 hidden note");
    CHECK_HAS(th_page(adam, "/board?expired=1"), "Plumber on Tuesday");
    CHECK(th_act(adam, "board.keep", "id=1").ok);
    CHECK_HAS(th_page(adam, "/board"), "Plumber on Tuesday");
    th_halt();
}

TEST(board_until_date_keeps_the_whole_day) {
    th_boot();
    int adam = th_person("Adam");
    g_fake_now = dt_make(2026, 10, 7, 9, 0);
    CHECK(th_act(adam, "board.post", "text=Dentist+Friday&keep=date&until=2026-10-09").ok);
    g_fake_now = dt_make(2026, 10, 9, 23, 30);
    CHECK_HAS(th_page(adam, "/board"), "Dentist Friday");
    g_fake_now = dt_make(2026, 10, 10, 0, 1);
    CHECK(!strstr(th_page(adam, "/board"), "Dentist Friday"));
    CHECK(!th_act(adam, "board.post", "text=Old&keep=date&until=2026-10-01").ok);
    CHECK(!th_act(adam, "board.post", "text=Bad&keep=date&until=someday").ok);
    CHECK(!th_act(adam, "board.post", "text=Bad&keep=date").ok);
    /* a filled-in date wins over the quick choices (the edit sheet has both) */
    CHECK(th_act(adam, "board.post", "text=Party+prep&keep=0&until=2026-10-12").ok);
    g_fake_now = dt_make(2026, 10, 13, 0, 1);
    CHECK(!strstr(th_page(adam, "/board"), "Party prep"));
    th_halt();
}

TEST(board_links_are_inert_except_http) {
    th_boot();
    int adam = th_person("Adam");
    const char *text = "see https://example.com/a?b=1&c=\"x\"onmouseover=alert(1) <script>alert(1)</script> "
                       "javascript:alert(2) and (https://x.se/(paren)). end";
    Buf form = {0};
    buf_puts(&form, "text=");
    url_encode(&form, text, false);
    CHECK(th_act(adam, "board.post", buf_cstr(&form)).ok);
    const char *html = th_page(adam, "/board");
    CHECK_HAS(html, "<a href=\"https://example.com/a?b=1&amp;c=\" target=\"_blank\" rel=\"noopener noreferrer\">");
    CHECK_HAS(html, "&quot;x&quot;onmouseover=alert(1)");
    CHECK_HAS(html, "&lt;script&gt;alert(1)&lt;/script&gt;");
    CHECK(!strstr(html, "<script>"));
    CHECK(!strstr(html, "\"onmouseover"));
    CHECK(!strstr(html, "href=\"javascript"));
    CHECK_HAS(html, "href=\"https://x.se/(paren)\"");
    CHECK_HAS(html, "</a>). end");
    buf_free(&form);
    th_halt();
}

TEST(board_keeps_line_breaks) {
    th_boot();
    int adam = th_person("Adam");
    CHECK(th_act(adam, "board.post", "text=line+one%0D%0Aline+two%0A%0A%0A%0Aline+three%0A%0A").ok);
    CHECK_HAS(th_page(adam, "/board"), "line one<br>line two<br><br>line three</div>");
    th_halt();
}

TEST(board_edit_and_delete_any_note) {
    th_boot();
    int adam = th_person("Adam");
    int eva = th_person("Eva");
    CHECK(th_act(eva, "board.post", "text=Eva%27s+note&hue=150").ok);
    const char *html = th_page(adam, "/board?edit=1");
    CHECK_HAS(html, "Edit note");
    CHECK_HAS(html, ">Eva&#39;s note</textarea>");
    ThResult r = th_act(adam, "board.save", "id=1&text=Eva%27s+note+%28fixed%29&hue=200&pin=1&keep=0");
    CHECK(r.ok);
    CHECK_STR(r.redirect, "/board");
    html = th_page(adam, "/board");
    CHECK_HAS(html, "Eva&#39;s note (fixed)");
    CHECK_HAS(html, "--h:200");
    CHECK_HAS(html, "board-note pinned");
    CHECK_HAS(last_activity()->text, "Adam edited Eva's note");
    CHECK(th_act(adam, "board.delete", "id=1").ok);
    CHECK(!strstr(th_page(adam, "/board"), "(fixed)"));
    CHECK_HAS(last_activity()->text, "Adam took down Eva's note");
    CHECK(!th_act(adam, "board.delete", "id=1").ok);
    CHECK(!th_act(adam, "board.pin", "id=42").ok);
    CHECK(!th_act(adam, "board.save", "id=42&text=x").ok);
    th_halt();
}

TEST(board_requires_identity) {
    th_boot();
    int adam = th_person("Adam");
    CHECK(!th_act(0, "board.post", "text=zebra+crossing").ok);
    CHECK(!strstr(th_page(adam, "/board"), "zebra crossing"));
    th_halt();
}

TEST(board_widgets_render_for_anonymous_screens) {
    th_boot();
    int adam = th_person("Adam");
    g_fake_now = dt_make(2026, 10, 7, 9, 0);
    CHECK(th_act(adam, "board.post", "text=Pinned+thing&pin=1").ok);
    g_fake_now += 60;
    CHECK(th_act(adam, "board.post", "text=Loose+thing").ok);
    const Module *m = module_find("board");
    const Block *bl = block_find("board.notes", NULL); /* a wall's "Notes" */
    CHECK(m && m->widget && bl && (bl->kinds & SCREEN_WALL));
    Ctx c;
    app_ctx(&c, 0, "/screens/2");
    Buf b = {0};
    CHECK(bl->render(&c, &b, SCREEN_WALL, ""));
    const char *s = buf_cstr(&b);
    CHECK(strstr(s, "Pinned thing") && strstr(s, "Loose thing"));
    CHECK(strstr(s, "Pinned thing") < strstr(s, "Loose thing"));
    CHECK(!strstr(s, "href=\"/board\""));
    buf_reset(&b);
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "href=\"/board\"");
    buf_free(&b);
    app_ctx_free(&c);
    th_halt();
}

TEST(board_empty_states) {
    th_boot();
    int adam = th_person("Adam");
    CHECK_HAS(th_page(adam, "/board"), "The board is empty");
    const Module *m = module_find("board");
    Ctx c;
    app_ctx(&c, 0, "/");
    Buf b = {0};
    CHECK(!block_find("board.notes", NULL)->render(&c, &b, SCREEN_WALL, ""));
    CHECK_INT(b.len, 0); /* a calm wall leaves the card out rather than show it empty */
    m->widget(&c, &b, WIDGET_HOME);
    CHECK_HAS(buf_cstr(&b), "Nothing on the board");
    buf_free(&b);
    app_ctx_free(&c);
    th_halt();
}

/* Issue #69: a list of notes' buttons says which note each is for (it was "Pin to the top",
   "Edit", "Remove" over and over); and issue #60: no "Remove this note?" - Undo instead. */
TEST(board_note_buttons_name_their_note) {
    th_boot();
    int adam = th_person("Adam");
    CHECK(th_act(adam, "board.post", "text=Milk+is+in+the+fridge&hue=45").ok);
    const char *h = th_page(adam, "/board");
    CHECK_HAS(h, "aria-label=\"Pin “Milk is in the fridge” to the top\"");
    CHECK_HAS(h, "aria-label=\"Edit “Milk is in the fridge”\"");
    CHECK_HAS(h, "aria-label=\"Remove “Milk is in the fridge”\"");
    CHECK(!strstr(h, "Remove this note?"));
    th_halt();
}

/* Issue #39: a note kept until Friday still goes at Friday's midnight after the zone changed */
TEST(board_note_keeps_its_day_when_the_zone_changes) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    plat_set_tz("UTC0");
    g_fake_now = dt_make(2026, 10, 7, 9, 0);
    th_boot_in(dir);
    int adam = th_person("Adam");
    CHECK(th_act(adam, "board.post", "text=Dentist+Friday&keep=date&until=2026-10-09").ok);
    th_halt();
    plat_set_tz("XXX-2"); /* UTC+2: without the move it would stay until 02:00 on Saturday */
    th_boot_in(dir);
    g_fake_now = dt_make(2026, 10, 9, 23, 30);
    CHECK_HAS(th_page(adam, "/board"), "Dentist Friday");
    g_fake_now = dt_make(2026, 10, 10, 0, 1);
    CHECK(!strstr(th_page(adam, "/board"), "Dentist Friday"));
    g_fake_now = 0;
    plat_set_tz(NULL);
    th_halt();
}

/* ------------------------------------------------ photos on notes (issue #47) */

/* a JPEG in the fewest bytes that still say its size: 20 wide, 10 high */
static const uint8_t TINY_JPEG[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J',  'F',  'I',  'F',  0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x0A, 0x00, 0x14, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
    0xFF, 0xD9,
};

/* form + "&photo=" + a data: URL of the bytes, as wire.js sends it */
static const char *with_photo(const char *form, const char *type, const uint8_t *data, size_t n) {
    static Buf b;
    buf_reset(&b);
    char *enc = xmalloc(4 * ((n + 2) / 3) + 1);
    base64_encode(data, n, enc);
    Buf url = {0};
    buf_printf(&url, "data:%s;base64,%s", type, enc);
    buf_puts(&b, form);
    buf_puts(&b, "&photo=");
    url_encode(&b, url.p, false);
    free(enc);
    buf_free(&url);
    return b.p;
}

/* the name of the first photo on a page ("" = none) */
static void photo_on(const char *html, char out[24]) {
    const char *p = strstr(html, "/board/photo/");
    out[0] = '\0';
    if (p) snprintf(out, 24, "%.20s", p + strlen("/board/photo/"));
}

static bool photo_file(const char *name) {
    char path[700];
    snprintf(path, sizeof path, "%s/board/%s", app_config()->data_dir, name);
    int64_t size = 0, mtime = 0;
    bool dir = false;
    return path_stat(path, &size, &mtime, &dir) && size == (int64_t)sizeof TINY_JPEG;
}

TEST(board_notes_hold_a_photo) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    th_boot_in(dir);
    int adam = th_person("Adam");
    ThResult r = th_act(adam, "board.post", with_photo("text=The+school+letter&hue=45", "image/jpeg", TINY_JPEG, sizeof TINY_JPEG));
    CHECK(r.ok);
    const char *h = th_page(adam, "/board");
    char name[24];
    photo_on(h, name);
    CHECK_INT((int)strlen(name), 20);
    CHECK(str_ends(name, ".jpg"));
    CHECK(photo_file(name));
    char want[120];
    snprintf(want, sizeof want, "<img src=\"/board/photo/%s\" width=\"20\" height=\"10\"", name);
    CHECK_HAS(h, want);
    CHECK_HAS(h, "The school letter");
    CHECK_HAS(h, "data-shrink=\"1600\"");
    CHECK_HAS(last_activity()->text, "Adam left a note with a photo: “The school letter”");
    /* a photo alone will do; neither is nothing */
    CHECK(th_act(adam, "board.post", with_photo("text=", "image/jpeg", TINY_JPEG, sizeof TINY_JPEG)).ok);
    CHECK_HAS(last_activity()->text, "Adam put a photo on the board");
    r = th_act(adam, "board.post", "text=");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Write something first, or add a photo");
    /* what isn't a JPEG doesn't go in */
    r = th_act(adam, "board.post", with_photo("text=x", "image/png", TINY_JPEG, sizeof TINY_JPEG));
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "isn't a photo Hemma can keep");
    r = th_act(adam, "board.post", with_photo("text=x", "image/jpeg", (const uint8_t *)"GIF89a not a jpeg", 17));
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "couldn't be read");
    r = th_act(adam, "board.post", "text=x&photo=data%3Aimage%2Fjpeg%3Bbase64%2C%21%21%21");
    CHECK(!r.ok);
    /* the wall shows it, without a link */
    CHECK_HAS(th_page(adam, "/"), "<div class=\"board-photo\"><img src=\"/board/photo/");
    th_halt();
}

TEST(board_photo_is_taken_away_and_swept) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    th_boot_in(dir);
    int adam = th_person("Adam");
    CHECK(th_act(adam, "board.post", with_photo("text=Parking+permit&hue=45", "image/jpeg", TINY_JPEG, sizeof TINY_JPEG)).ok);
    char name[24];
    photo_on(th_page(adam, "/board"), name);
    CHECK(photo_file(name));
    /* edited: the photo taken away - the file stays while an Undo could want it */
    const char *h = th_page(adam, "/board?edit=1");
    CHECK_HAS(h, "Take the photo away");
    CHECK_HAS(h, "Another photo");
    CHECK(th_act(adam, "board.save", "id=1&text=Parking+permit&hue=45&keep=0&photo_off=1").ok);
    char now[24];
    photo_on(th_page(adam, "/board"), now);
    CHECK(!now[0]);
    CHECK(photo_file(name));
    /* taken away with no text left: refused, it'd be an empty note */
    CHECK(th_act(adam, "board.post", with_photo("text=", "image/jpeg", TINY_JPEG, sizeof TINY_JPEG)).ok);
    char only[24];
    photo_on(th_page(adam, "/board"), only);
    ThResult r = th_act(adam, "board.save", "id=2&text=&hue=45&keep=0&photo_off=1");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Write something first, or add a photo");
    /* deleted, then Undo: the note comes back with its photo */
    r = th_act(adam, "board.delete", "id=2");
    CHECK(r.ok);
    CHECK(photo_file(only));
    char k[40];
    snprintf(k, sizeof k, "k=%s", r.undo);
    CHECK(th_act(adam, "core.undo", k).ok);
    char back[24];
    photo_on(th_page(adam, "/board"), back);
    CHECK_STR(back, only);
    /* deleted for good this time: the next start sweeps what no note has, and keeps the rest */
    CHECK(th_act(adam, "board.delete", "id=2").ok);
    CHECK(th_act(adam, "board.post", with_photo("text=Keeps", "image/jpeg", TINY_JPEG, sizeof TINY_JPEG)).ok);
    char keeps[24];
    photo_on(th_page(adam, "/board"), keeps);
    th_halt();
    th_boot_in(dir);
    CHECK(!photo_file(name));
    CHECK(!photo_file(only));
    CHECK(photo_file(keeps));
    th_halt();
}
