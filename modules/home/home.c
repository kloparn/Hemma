/* modules/home — the dashboard at "/". It knows nothing about other
 * features: every module can contribute a card through its widget hook. */
#include "../../core/app.h"
#include "../../core/ui.h"

static int by_widget_order(const void *a, const void *b) {
    const Module *x = *(const Module *const *)a, *y = *(const Module *const *)b;
    return x->widget_order - y->widget_order;
}

static void people_strip(Ctx *c, Buf *b) {
    buf_puts(b, "<div class=\"strip\">");
    for (int i = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        int64_t since = 0;
        bool home = presence_is_home(p->id, &since);
        bool known = presence_known(p->id); /* people without a tracked device are never "away" */
        int screens = presence_screens(p->id);
        const char *state = home ? "home" : screens ? "online" : known ? "away" : "";
        H(b, "<a class=\"strip-p %s\" href=\"/people\">", state);
        ui_avatar_deco(b, p, "md");
        H(b, "<span><b>%s</b><small>", p->name);
        if (home) H(b, "%s", T("home"));
        else if (screens) {
            const char *where = presence_page(p->id); /* (a section's title: English, marked where it's set) */
            H(b, T("on %s"), where ? T(where) : "Hemma");
        } else if (known) H(b, "%s", T("away"));
        else buf_puts(b, "&nbsp;");
        buf_puts(b, "</small></span></a>");
    }
    buf_puts(b, "</div>");
}

/* A line about a section this person doesn't have (a child: the Network page's news) stays out. */
static bool for_them(const Ctx *c, const Activity *a) {
    if (a->link[0] != '/') return true;
    char id[32];
    str_ncopy(id, sizeof id, a->link + 1, strcspn(a->link + 1, "/?#"));
    const Module *m = id[0] ? module_find(id) : NULL;
    return !m || module_visible(m, c);
}

static void activity_card(Ctx *c, Buf *b) {
    ui_card(b, T("Latest"), NULL, NULL);
    int n = tbl_count(&T_activity);
    if (!n) {
        H(b, "<p class=\"muted\">%s</p>", T("Things people do show up here."));
    } else {
        buf_puts(b, "<ul class=\"feed\">");
        Buf line = {0};
        for (int i = n - 1, shown = 0; i >= 0 && shown < 10; i--) {
            Activity *a = tbl_at(&T_activity, i);
            if (!for_them(c, a)) continue;
            shown++;
            buf_reset(&line);
            activity_text(a, &line); /* (in the reader's language: issue #46) */
            H(b, "<li><span class=\"feed-i\">%s</span><span class=\"feed-t\">", a->icon[0] ? a->icon : "•");
            if (a->link[0]) H(b, "<a href=\"%s\">%s</a>", a->link, buf_cstr(&line));
            else html_esc(b, buf_cstr(&line));
            buf_puts(b, "</span><time>");
            ui_ago(b, a->ts, c->now);
            buf_puts(b, "</time></li>");
        }
        buf_free(&line);
        buf_puts(b, "</ul>");
    }
    ui_card_end(b);
}

/* The wall's "Latest": what people did (not the network's own news - a new device, say), one
   line each, newest first. Nothing yet: the wall leaves the card out. */
static bool row_latest(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)kind, (void)arg;
    int shown = 0;
    Buf line = {0};
    for (int i = tbl_count(&T_activity) - 1; i >= 0 && shown < 6; i--) {
        Activity *a = tbl_at(&T_activity, i);
        if (!a->who) continue;
        if (!shown++) buf_puts(b, "<ul class=\"feed home-wall-feed\">");
        buf_reset(&line);
        activity_text(a, &line);
        H(b, "<li><span class=\"feed-i\">%s</span><span class=\"feed-t\">%s</span><time>", a->icon[0] ? a->icon : "•",
          buf_cstr(&line));
        ui_ago(b, a->ts, c->now);
        buf_puts(b, "</time></li>");
    }
    buf_free(&line);
    if (shown) buf_puts(b, "</ul>");
    return shown > 0;
}

static const Block blocks[] = {
    {"home.latest", N_("Latest"), SCREEN_WALL, row_latest, NULL},
    {0},
};

static void page(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Home"));
    DateTime d = dt_local(c->now);
    /* one whole greeting each (word order differs between languages) */
    const char *greet = c->me ? (d.hour < 5    ? T("Good night, %s")
                                 : d.hour < 10 ? T("Good morning, %s")
                                 : d.hour < 17 ? T("Hello, %s")
                                 : d.hour < 22 ? T("Good evening, %s")
                                               : T("Good night, %s"))
                              : (d.hour < 5    ? T("Good night")
                                 : d.hour < 10 ? T("Good morning")
                                 : d.hour < 17 ? T("Hello")
                                 : d.hour < 22 ? T("Good evening")
                                               : T("Good night"));
    char title[160], sub[160];
    snprintf(title, sizeof title, greet, c->me ? c->me->name : "");
    snprintf(sub, sizeof sub, T("%s %d %s · week %d"), weekday_name(d.wday, false), d.day, month_name(d.mon, false),
             iso_week(c->now, NULL));
    if (sub[0] >= 'a' && sub[0] <= 'z') sub[0] = (char)(sub[0] - 'a' + 'A'); /* ("tisdag", "martes": a line's start) */
    ui_head(b, title, sub);
    bool guest = is_guest(c); /* a guest: their own things - not who's where, nor the household's news (#44) */
    const Module *screens = module_find("screens");
    if (screens && module_visible(screens, c)) {
        /* /wall is the household's first wall screen (modules/screens answers it): a full load */
        H(b, "<a class=\"btn ghost\" href=\"/wall\" data-native title=\"%s\">", T("Wall display"));
        ui_icon(b, "monitor");
        H(b, "<span>%s</span></a>", T("Wall"));
    }
    ui_head_end(b);

    if (!guest) people_strip(c, b);

    const Module *w[64];
    int nw = 0;
    for (int i = 0; i < module_count() && nw < countof(w); i++) /* an admin's section shows only to admins */
        if (module_at(i)->widget && module_visible(module_at(i), c)) w[nw++] = module_at(i);
    qsort(w, (size_t)nw, sizeof *w, by_widget_order);
    buf_puts(b, "<div class=\"widgets\">");
    for (int i = 0; i < nw; i++) w[i]->widget(c, b, WIDGET_HOME);
    if (!guest) activity_card(c, b);
    buf_puts(b, "</div>");
}

static const Module home = {
    .id = "home",
    .title = N_("Home"),
    .icon = "home",
    .nav = 1,
    .page = page,
    .blocks = blocks,
};
MODULE(home)
