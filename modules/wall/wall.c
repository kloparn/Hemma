/* modules/wall — an always-on overview for a tablet in the hall or kitchen:
 * a big clock, who's home, and every module's wall widget.
 *
 * Not public: the tablet signs in once with a read-only "Display" account
 * (ROLE_DISPLAY), so widgets render with c->me set to that account (or to a
 * member who opened /wall) and must stay read-only for it. The core re-renders
 * every minute, which is all the clock needs — no JavaScript timers. Always
 * dark, whatever the system says. */
#include "../../core/app.h"
#include "../../core/ui.h"

static int by_widget_order(const void *a, const void *b) {
    const Module *x = *(const Module *const *)a, *y = *(const Module *const *)b;
    return x->widget_order != y->widget_order ? x->widget_order - y->widget_order : strcmp(x->id, y->id);
}

static void wall_clock(Ctx *c, Buf *b) {
    DateTime d = dt_local(c->now);
    char hm[6];
    fmt_hm(c->now, hm);
    H(b, "<div class=\"wall-clock\"><div class=\"wall-home\">%s</div><time datetime=\"%s\">%s</time>", app_name(), hm,
      hm);
    buf_printf(b, "<div class=\"wall-date\">%s %d %s · week %d</div></div>", weekday_name(d.wday, false), d.day,
               month_name(d.mon, false), iso_week(c->now, NULL));
}

static void wall_people(Ctx *c, Buf *b) {
    if (!person_count()) return;
    bool tracking = presence_tracking();
    buf_puts(b, "<div class=\"wall-people\">");
    for (int i = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        bool home = presence_is_home(p->id, NULL);
        bool online = presence_screens(p->id) > 0;
        const char *state = home ? "home" : tracking ? "away" : online ? "online" : "";
        H(b, "<div class=\"wall-person %s\">", state);
        ui_avatar(b, p, "xl");
        H(b, "<b>%s</b><small>%s</small></div>", p->name, *state ? state : "\xC2\xA0");
    }
    buf_puts(b, "</div>");
}

static void wall_latest(Ctx *c, Buf *b) {
    int n = tbl_count(&T_activity);
    if (!n) return;
    ui_card(b, "Latest", NULL, NULL);
    buf_puts(b, "<ul class=\"feed\">");
    for (int i = n - 1, shown = 0; i >= 0 && shown < 12; i--, shown++) { /* the card clips with a fade */
        Activity *a = tbl_at(&T_activity, i);
        H(b, "<li><span class=\"feed-i\">%s</span><span class=\"feed-t\">%s</span><time>", a->icon[0] ? a->icon : "•",
          a->text);
        ui_ago(b, a->ts, c->now);
        buf_puts(b, "</time></li>");
    }
    buf_puts(b, "</ul>");
    ui_card_end(b);
}

static void page(Ctx *c, Buf *b) {
    ctx_title(c, "Wall");
    c->bare = true;
    buf_puts(b, "<div class=\"wall\"><header class=\"wall-top\">");
    wall_clock(c, b);
    wall_people(c, b);
    buf_puts(b, "</header>");

    const Module *w[64];
    int nw = 0;
    for (int i = 0; i < module_count() && nw < countof(w); i++) {
        const Module *m = module_at(i);
        if (m->widget && !str_eq(m->id, "wall") && module_visible(m, c)) w[nw++] = m; /* MOD_ADMIN: admins only */
    }
    qsort(w, (size_t)nw, sizeof *w, by_widget_order);
    Buf grid = {0};
    for (int i = 0; i < nw; i++) w[i]->widget(c, &grid, WIDGET_WALL);
    wall_latest(c, &grid);
    if (grid.len) {
        buf_puts(b, "<section class=\"wall-grid\">");
        buf_put(b, grid.p, grid.len);
        buf_puts(b, "</section>");
    } else if (!person_count()) {
        ui_empty(b, "🏡", "Nobody's set up yet", "Open Hemma on a phone or computer to set up the household.");
    } else {
        ui_empty(b, "🌙", "All quiet", "Bookings, plans and notes show up here.");
    }
    buf_puts(b, "<a class=\"wall-exit\" href=\"/\">Exit</a></div>");
    buf_free(&grid);
}

static const Module wall = {
    .id = "wall",
    .title = "Wall",
    .icon = "monitor",
    .nav = 0,
    /* not public: a wall tablet signs in once with a read-only "Display" account */
    .page = page,
};
MODULE(wall)
