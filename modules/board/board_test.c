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
