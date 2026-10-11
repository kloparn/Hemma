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
        ui_avatar(b, p, "md");
        H(b, "<span><b>%s</b><small>", p->name);
        if (home) buf_puts(b, "home");
        else if (screens) {
            const char *where = presence_page(p->id);
            H(b, "on %s", where ? where : "Hemma");
        } else if (known) buf_puts(b, "away");
        else buf_puts(b, "&nbsp;");
        buf_puts(b, "</small></span></a>");
    }
    buf_puts(b, "</div>");
}

static void activity_card(Ctx *c, Buf *b) {
    ui_card(b, "Latest", NULL, NULL);
    int n = tbl_count(&T_activity);
    if (!n) {
        buf_puts(b, "<p class=\"muted\">Things people do show up here.</p>");
    } else {
        buf_puts(b, "<ul class=\"feed\">");
        for (int i = n - 1, shown = 0; i >= 0 && shown < 10; i--, shown++) {
            Activity *a = tbl_at(&T_activity, i);
            H(b, "<li><span class=\"feed-i\">%s</span><span class=\"feed-t\">", a->icon[0] ? a->icon : "•");
            if (a->link[0]) H(b, "<a href=\"%s\">%s</a>", a->link, a->text);
            else html_esc(b, a->text);
            buf_puts(b, "</span><time>");
            ui_ago(b, a->ts, c->now);
            buf_puts(b, "</time></li>");
        }
        buf_puts(b, "</ul>");
    }
    ui_card_end(b);
}

static void page(Ctx *c, Buf *b) {
    ctx_title(c, "Home");
    DateTime d = dt_local(c->now);
    const char *greet = d.hour < 5    ? "Good night"
                        : d.hour < 10 ? "Good morning"
                        : d.hour < 17 ? "Hello"
                        : d.hour < 22 ? "Good evening"
                                      : "Good night";
    char title[120], sub[120];
    snprintf(title, sizeof title, "%s, %s", greet, c->me ? c->me->name : "there");
    snprintf(sub, sizeof sub, "%s %d %s · week %d", weekday_name(d.wday, false), d.day, month_name(d.mon, false),
             iso_week(c->now, NULL));
    ui_head(b, title, sub);
    H(b, "<a class=\"btn ghost\" href=\"/wall\" title=\"Wall display\">");
    ui_icon(b, "monitor");
    buf_puts(b, "<span>Wall</span></a>");
    ui_head_end(b);

    people_strip(c, b);

    const Module *w[64];
    int nw = 0;
    for (int i = 0; i < module_count() && nw < countof(w); i++)
        if (module_at(i)->widget) w[nw++] = module_at(i);
    qsort(w, (size_t)nw, sizeof *w, by_widget_order);
    buf_puts(b, "<div class=\"widgets\">");
    for (int i = 0; i < nw; i++) w[i]->widget(c, b, WIDGET_HOME);
    activity_card(c, b);
    buf_puts(b, "</div>");
}

static const Module home = {
    .id = "home",
    .title = "Home",
    .icon = "home",
    .nav = 1,
    .page = page,
};
MODULE(home)
