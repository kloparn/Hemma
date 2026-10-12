/* core/ui.c — see ui.h */
#include "ui.h"

/* ------------------------------------------------------------------ icons */

static const struct {
    const char *name, *svg;
} ICONS[] = {
    {"home", "<path d=\"M3 10.5 12 3l9 7.5\"/><path d=\"M5 9.5V21h14V9.5\"/><path d=\"M10 21v-6h4v6\"/>"},
    {"calendar", "<rect x=\"3\" y=\"5\" width=\"18\" height=\"16\" rx=\"2.5\"/><path d=\"M3 10h18M8 3v4M16 3v4\"/>"},
    {"list", "<path d=\"M10 6h10M10 12h10M10 18h10\"/><path d=\"m3.5 6 1.5 1.5L7.5 5M3.5 12l1.5 1.5 2.5-2.5M3.5 "
             "18l1.5 1.5 2.5-2.5\"/>"},
    {"play", "<rect x=\"2.5\" y=\"4.5\" width=\"19\" height=\"15\" rx=\"3\"/><path d=\"m10 9 5 3-5 3z\"/>"},
    {"note", "<path d=\"M5 3h10l4 4v14H5z\"/><path d=\"M15 3v4h4M9 12h6M9 16h4\"/>"},
    {"wifi", "<path d=\"M2 8.8a15 15 0 0 1 20 0M5.2 12.3a10.5 10.5 0 0 1 13.6 0M8.6 15.8a5.5 5.5 0 0 1 6.8 "
             "0\"/><circle cx=\"12\" cy=\"19.3\" r=\"1\"/>"},
    {"users", "<circle cx=\"9\" cy=\"8\" r=\"3.5\"/><path d=\"M2.5 20a6.5 6.5 0 0 1 13 0\"/><path d=\"M16 4.6a3.5 "
              "3.5 0 0 1 0 6.8M18.2 14.2A6.5 6.5 0 0 1 21.5 20\"/>"},
    {"plus", "<path d=\"M12 5v14M5 12h14\"/>"},
    {"x", "<path d=\"M6 6l12 12M18 6 6 18\"/>"},
    {"left", "<path d=\"m15 5-7 7 7 7\"/>"},
    {"right", "<path d=\"m9 5 7 7-7 7\"/>"},
    {"trash", "<path d=\"M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3\"/>"},
    {"clock", "<circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 7v5l3 2\"/>"},
    {"monitor", "<rect x=\"3\" y=\"4\" width=\"18\" height=\"12\" rx=\"2\"/><path d=\"M8 20h8M12 16v4\"/>"},
    {"power", "<path d=\"M12 3v9\"/><path d=\"M6.3 6.3a8 8 0 1 0 11.4 0\"/>"},
    {"search", "<circle cx=\"11\" cy=\"11\" r=\"7\"/><path d=\"m20 20-4-4\"/>"},
    {"settings", "<path d=\"M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1\"/><circle cx=\"15\" cy=\"6\" "
                 "r=\"2\"/><circle cx=\"9\" cy=\"12\" r=\"2\"/><circle cx=\"17\" cy=\"18\" r=\"2\"/>"},
    {"edit", "<path d=\"M4 20h4L19 9l-4-4L4 16z\"/><path d=\"m13 7 4 4\"/>"},
    {"bell", "<path d=\"M6 16v-5a6 6 0 0 1 12 0v5l2 2H4z\"/><path d=\"M10 21h4\"/>"},
    {"check", "<path d=\"m5 12.5 4.5 4.5L19 7.5\"/>"},
    {"arrow", "<path d=\"M5 12h14M13 6l6 6-6 6\"/>"},
    {"pin", "<path d=\"M9 3h6l-1 6 4 4H6l4-4zM12 13v8\"/>"},
    {"film", "<rect x=\"3\" y=\"3\" width=\"18\" height=\"18\" rx=\"2\"/><path d=\"M7 3v18M17 3v18M3 8h4M3 "
             "16h4M17 8h4M17 16h4M3 12h18\"/>"},
    {"tv", "<rect x=\"3\" y=\"7\" width=\"18\" height=\"13\" rx=\"2\"/><path d=\"m8 3 4 4 4-4\"/>"},
    {"sparkle", "<path d=\"M12 3c.6 4.5 2.5 6.4 7 7-4.5.6-6.4 2.5-7 7-.6-4.5-2.5-6.4-7-7 4.5-.6 6.4-2.5 "
                "7-7z\"/><path d=\"M19 15c.3 1.8 1 2.6 2.5 3-1.5.3-2.2 1.2-2.5 3-.3-1.8-1-2.6-2.5-3 1.5-.4 2.2-1.2 "
                "2.5-3z\"/>"},
    {"folder", "<path d=\"M3 6.5A1.5 1.5 0 0 1 4.5 5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 "
               "1.5 0 0 1 3 18.5z\"/>"},
    {"grid", "<rect x=\"3.5\" y=\"3.5\" width=\"7\" height=\"7\" rx=\"2\"/><rect x=\"13.5\" y=\"3.5\" width=\"7\" "
             "height=\"7\" rx=\"2\"/><rect x=\"3.5\" y=\"13.5\" width=\"7\" height=\"7\" rx=\"2\"/><rect x=\"13.5\" "
             "y=\"13.5\" width=\"7\" height=\"7\" rx=\"2\"/>"},
    {"pulse", "<path d=\"M3 12h4l2.5-6 5 12 2.5-6h4\"/>"},
    {"logo", "<path d=\"M4 11 12 4l8 7v8.5a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 4 19.5z\"/><path d=\"M12 "
             "17.5s-3.2-1.9-3.2-4a1.7 1.7 0 0 1 3.2-.8 1.7 1.7 0 0 1 3.2.8c0 2.1-3.2 4-3.2 4z\"/>"},
};

void ui_icon(Buf *b, const char *name) {
    for (int i = 0; i < countof(ICONS); i++) {
        if (str_eq(ICONS[i].name, name)) {
            buf_puts(b, "<svg class=\"i\" viewBox=\"0 0 24 24\" aria-hidden=\"true\">");
            buf_puts(b, ICONS[i].svg);
            buf_puts(b, "</svg>");
            return;
        }
    }
}

/* ---------------------------------------------------------------- people */

static uint32_t upper_cp(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 0x20;
    return cp;
}

static void initials(const char *name, char *out, size_t cap) {
    size_t o = 0;
    int words = 0;
    bool at_start = true;
    for (const char *p = name; *p && words < 2;) {
        uint32_t cp;
        int k = utf8_decode(p, strlen(p), &cp);
        if (cp == ' ' || cp == '-') {
            at_start = true;
        } else if (at_start) {
            char enc[4];
            size_t n = utf8_encode(upper_cp(cp), enc);
            if (o + n < cap) memcpy(out + o, enc, n), o += n;
            words++;
            at_start = false;
        }
        p += k;
    }
    out[o] = '\0';
}

void ui_hue_style(Buf *b, int hue) { H(b, " style=\"--h:%d\"", ((hue % 360) + 360) % 360); }

void ui_avatar(Buf *b, const Person *p, const char *size) {
    if (!p) {
        H(b, "<span class=\"av av-%s av-none\">?</span>", size);
        return;
    }
    H(b, "<span class=\"av av-%s\" title=\"%s\"", size, p->name);
    ui_hue_style(b, p->hue);
    buf_putc(b, '>');
    if (p->emoji[0]) {
        html_esc(b, p->emoji);
    } else {
        char ini[16];
        initials(p->name, ini, sizeof ini);
        html_esc(b, ini);
    }
    buf_puts(b, "</span>");
}

void ui_person(Buf *b, const Person *p) {
    H(b, "<span class=\"who\">");
    ui_avatar(b, p, "xs");
    H(b, "<span>%s</span></span>", p ? p->name : "Someone");
}

/* ---------------------------------------------------------- page pieces */

void ui_head(Buf *b, const char *title, const char *sub) {
    H(b, "<header class=\"ph\"><div class=\"ph-t\"><h1>%s</h1>", title);
    if (sub && *sub) H(b, "<p class=\"sub\">%s</p>", sub);
    buf_puts(b, "</div><div class=\"ph-a\">");
}

void ui_head_end(Buf *b) { buf_puts(b, "</div></header>"); }

void ui_card(Buf *b, const char *title, const char *more_href, const char *more_label) {
    buf_puts(b, "<section class=\"card\">");
    if (title) {
        H(b, "<header class=\"card-h\"><h2>%s</h2>", title);
        if (more_href) H(b, "<a class=\"more\" href=\"%s\">%s</a>", more_href, more_label ? more_label : "All");
        buf_puts(b, "</header>");
    }
}

void ui_card_end(Buf *b) { buf_puts(b, "</section>"); }

void ui_empty(Buf *b, const char *emoji, const char *title, const char *text) {
    H(b, "<div class=\"empty\"><div class=\"empty-e\">%s</div><h3>%s</h3>", emoji, title);
    if (text) H(b, "<p>%s</p>", text);
    buf_puts(b, "</div>");
}

/* ----------------------------------------------------------------- forms */

void ui_form(Buf *b, const char *action, const char *cls) {
    H(b, "<form method=\"post\" action=\"/a/%s\" data-a=\"%s\"", action, action);
    if (cls) H(b, " class=\"%s\"", cls);
    buf_putc(b, '>');
}

void ui_form_confirm(Buf *b, const char *action, const char *cls, const char *question) {
    H(b, "<form method=\"post\" action=\"/a/%s\" data-a=\"%s\" data-confirm=\"%s\"", action, action, question);
    if (cls) H(b, " class=\"%s\"", cls);
    buf_putc(b, '>');
}

void ui_form_end(Buf *b) { buf_puts(b, "</form>"); }

void ui_hidden(Buf *b, const char *name, const char *value) {
    H(b, "<input type=\"hidden\" name=\"%s\" value=\"%s\">", name, value);
}

void ui_hidden_int(Buf *b, const char *name, long long v) {
    H(b, "<input type=\"hidden\" name=\"%s\" value=\"%L\">", name, v);
}

void ui_act(Buf *b, const char *action, const char *cls, const char *label_html, const char *name, const char *value,
            const char *title) {
    ui_form(b, action, "inline");
    if (name) ui_hidden(b, name, value ? value : "");
    H(b, "<button class=\"%s\"", cls ? cls : "btn");
    if (title) H(b, " title=\"%s\" aria-label=\"%s\"", title, title);
    H(b, ">%r</button>", label_html);
    ui_form_end(b);
}

void ui_person_select(Buf *b, const char *name, int selected, const char *none_label) {
    H(b, "<select name=\"%s\">", name);
    if (none_label) H(b, "<option value=\"0\"%r>%s</option>", selected == 0 ? " selected" : "", none_label);
    for (int i = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        H(b, "<option value=\"%d\"%r>%s%r%s</option>", p->id, p->id == selected ? " selected" : "", p->emoji,
          p->emoji[0] ? " " : "", p->name);
    }
    buf_puts(b, "</select>");
}

void ui_hue_picker(Buf *b, const char *name, int selected) {
    static const int hues[] = {0, 25, 45, 90, 150, 175, 200, 220, 255, 280, 310, 335};
    static const char *names[] = {"Red",   "Orange", "Amber", "Lime",   "Green",   "Teal",
                                  "Sky",   "Blue",   "Indigo", "Purple", "Magenta", "Pink"};
    buf_puts(b, "<div class=\"hues\" role=\"radiogroup\" aria-label=\"Colour\">");
    for (int i = 0; i < countof(hues); i++) {
        H(b, "<label class=\"hue\" title=\"%s\"", names[i]);
        ui_hue_style(b, hues[i]);
        H(b, "><input type=\"radio\" name=\"%s\" value=\"%d\" aria-label=\"%s\"%r><span></span></label>", name, hues[i],
          names[i], hues[i] == selected ? " checked" : "");
    }
    buf_puts(b, "</div>");
}

/* ----------------------------------------------------------------- modal */

void ui_modal(Buf *b, const char *title, const char *close_href) {
    H(b,
      "<div class=\"modal\" role=\"dialog\" aria-modal=\"true\" aria-label=\"%s\">"
      "<a class=\"modal-bg\" href=\"%s\" data-esc aria-label=\"Close\"></a><div class=\"modal-card\">"
      "<header class=\"modal-h\"><h2>%s</h2><a class=\"icon-btn\" href=\"%s\" aria-label=\"Close\">",
      title, close_href, title, close_href);
    ui_icon(b, "x");
    buf_puts(b, "</a></header><div class=\"modal-b\">");
}

void ui_modal_end(Buf *b) { buf_puts(b, "</div></div></div>"); }

/* ------------------------------------------------------------------ time */

void ui_hm(Buf *b, int64_t t) {
    char s[6];
    fmt_hm(t, s);
    buf_puts(b, s);
}

void ui_day(Buf *b, int64_t day, int64_t now) {
    int64_t d0 = day_start(day), today = day_start(now);
    DateTime d = dt_local(day), n = dt_local(now);
    if (d0 == today) buf_puts(b, "Today");
    else if (d0 == add_days(today, 1)) buf_puts(b, "Tomorrow");
    else if (d0 == add_days(today, -1)) buf_puts(b, "Yesterday");
    else if (d0 > today && d0 < add_days(today, 7)) buf_puts(b, weekday_name(d.wday, false));
    else if (d.year == n.year) buf_printf(b, "%s %d %s", weekday_name(d.wday, true), d.day, month_name(d.mon, true));
    else buf_printf(b, "%d %s %d", d.day, month_name(d.mon, true), d.year);
}

void ui_day_inline(Buf *b, int64_t day, int64_t now) {
    int64_t d0 = day_start(day), today = day_start(now);
    if (d0 == today) buf_puts(b, "today");
    else if (d0 == add_days(today, 1)) buf_puts(b, "tomorrow");
    else if (d0 == add_days(today, -1)) buf_puts(b, "yesterday");
    else ui_day(b, day, now); /* weekday names and dates keep their capitals */
}

void ui_ago(Buf *b, int64_t t, int64_t now) {
    int64_t s = now - t;
    if (s < 60) buf_puts(b, "just now");
    else if (s < 3600) buf_printf(b, "%lld min ago", (long long)(s / 60));
    else if (s < 6 * 3600 || day_start(t) == day_start(now)) buf_printf(b, "%lld h ago", (long long)(s / 3600));
    else if (day_start(t) == add_days(day_start(now), -1)) buf_puts(b, "yesterday");
    else if (s < 6 * 86400) buf_puts(b, weekday_name(dt_local(t).wday, false));
    else {
        DateTime d = dt_local(t);
        if (d.year == dt_local(now).year) buf_printf(b, "%d %s", d.day, month_name(d.mon, true));
        else buf_printf(b, "%d %s %d", d.day, month_name(d.mon, true), d.year);
    }
}

void ui_span(Buf *b, int64_t start, int64_t end) {
    DateTime s = dt_local(start), e = dt_local(end);
    bool whole_days = s.hour == 0 && s.min == 0 && e.hour == 0 && e.min == 0 && end > start;
    if (whole_days) {
        int64_t last = add_days(end, -1);
        DateTime l = dt_local(last);
        if (day_start(last) == day_start(start)) buf_puts(b, "all day");
        else if (l.mon == s.mon)
            buf_printf(b, "%s %d – %s %d %s", weekday_name(s.wday, true), s.day, weekday_name(l.wday, true), l.day,
                       month_name(l.mon, true));
        else
            buf_printf(b, "%d %s – %d %s", s.day, month_name(s.mon, true), l.day, month_name(l.mon, true));
        return;
    }
    if (day_start(start) == day_start(end) || (e.hour == 0 && e.min == 0 && end - start < 86400)) {
        buf_printf(b, "%02d:%02d–%02d:%02d", s.hour, s.min, e.hour == 0 && e.min == 0 ? 24 : e.hour, e.min);
        return;
    }
    buf_printf(b, "%s %02d:%02d – %s %02d:%02d", weekday_name(s.wday, true), s.hour, s.min, weekday_name(e.wday, true),
               e.hour, e.min);
}

void ui_duration(Buf *b, int64_t sec) {
    int64_t m = (sec + 30) / 60;
    if (m < 60) buf_printf(b, "%lld min", (long long)m);
    else if (m % 60 == 0) buf_printf(b, "%lld h", (long long)(m / 60));
    else buf_printf(b, "%lld h %lld min", (long long)(m / 60), (long long)(m % 60));
}

/* ----------------------------------------------------------------- shell */

#define TAB_SLOTS 4 /* narrow phones: this many sections in the bottom bar, then "More" */

static void nav_links(Ctx *c, Buf *b, const Module *active, bool tabbar) {
    int n = 0, k = 0;
    for (int i = 0; i < module_count(); i++) n += module_at(i)->nav != 0 && module_visible(module_at(i), c);
    tabbar = tabbar && n > TAB_SLOTS + 1; /* one extra fits; two or more go behind "More" */
    bool more_on = tabbar && str_eq(c->path, "/more");
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (!m->nav || !module_visible(m, c)) continue;
        bool on = m == active, extra = tabbar && k++ >= TAB_SLOTS;
        if (on && extra) more_on = true;
        H(b, "<a href=\"%s%s\"%r%r>", "/", str_eq(m->id, "home") ? "" : m->id,
          extra ? (on ? " class=\"tab-x on\"" : " class=\"tab-x\"") : on ? " class=\"on\"" : "",
          on ? " aria-current=\"page\"" : "");
        ui_icon(b, m->icon);
        H(b, "<span>%s</span></a>", m->title);
    }
    if (tabbar) { /* shown by CSS only where the bar runs out of room */
        H(b, "<a class=\"tab-more%r\" href=\"/more\">", more_on ? " on" : "");
        ui_icon(b, "grid");
        buf_puts(b, "<span>More</span></a>");
    }
}

void ui_more_page(Ctx *c, Buf *b) {
    ctx_title(c, "More");
    ui_head(b, "Everything", NULL);
    ui_head_end(b);
    buf_puts(b, "<nav class=\"more-grid\">");
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (!m->nav || !module_visible(m, c)) continue;
        H(b, "<a href=\"/%s\">", str_eq(m->id, "home") ? "" : m->id);
        ui_icon(b, m->icon);
        H(b, "<span>%s</span></a>", m->title);
    }
    buf_puts(b, "</nav><div class=\"more-side\">"); /* the sidebar's blocks: phones have no sidebar */
    for (int i = 0; i < module_count(); i++)
        if (module_at(i)->side && module_visible(module_at(i), c)) module_at(i)->side(c, b);
    buf_puts(b, "</div>");
}

void ui_shell(Ctx *c, Buf *b, const Module *active, const char *page, size_t page_len) {
    buf_puts(b, "<aside class=\"side\"><a class=\"brand\" href=\"/\"><span class=\"logo\">");
    ui_icon(b, "logo");
    H(b, "</span><b>%s</b></a><nav class=\"nav\">", app_name());
    nav_links(c, b, active, false);
    buf_puts(b, "</nav><div class=\"side-foot\">");
    /* who else is here right now (not where: every screen would change at each click -
       the home dashboard says where people are) */
    int shown = 0;
    for (int i = 0; i < person_count(); i++) {
        Person *p = person_at(i);
        if (c->me && p->id == c->me->id) continue;
        if (presence_screens(p->id) <= 0) continue;
        if (!shown++) buf_puts(b, "<div class=\"here\"><span class=\"here-l\">Here now</span><div class=\"here-a\">");
        H(b, "<span class=\"here-p\" title=\"%s\">", p->name);
        ui_avatar(b, p, "sm");
        buf_puts(b, "</span>");
    }
    if (shown) buf_puts(b, "</div></div>");
    for (int i = 0; i < module_count(); i++) /* modules' small blocks (e.g. the library's disk space) */
        if (module_at(i)->side && module_visible(module_at(i), c)) module_at(i)->side(c, b);
    if (c->me) {
        buf_puts(b, "<a class=\"me\" href=\"/people\">");
        ui_avatar(b, c->me, "sm");
        H(b, "<span>%s</span></a>", c->me->name);
    }
    buf_puts(b, "</div></aside><main class=\"main\" id=\"main\">");
    buf_put(b, page, page_len);
    buf_puts(b, "</main><nav class=\"tabbar\">");
    nav_links(c, b, active, true);
    buf_puts(b, "</nav>");
}

void ui_document(Ctx *c, Buf *b, const char *app_html, size_t n, const char *flash, int flash_kind) {
    const char *v = app_asset_version();
    /* data-h: the hash of the #app below; the wire's first hello sends it back, so the server
       doesn't resend a page the browser already has (core/app.c, Client.html_hash) */
    char h[17];
    snprintf(h, sizeof h, "%016llx", (unsigned long long)hash64(app_html, n));
    H(b,
      "<!doctype html><html lang=\"en\" data-v=\"%s\" data-h=\"%s\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
      "<meta name=\"color-scheme\" content=\"light dark\">"
      "<meta name=\"theme-color\" content=\"#f4f1ea\" media=\"(prefers-color-scheme: light)\">"
      "<meta name=\"theme-color\" content=\"#121316\" media=\"(prefers-color-scheme: dark)\">"
      "<meta name=\"mobile-web-app-capable\" content=\"yes\"><meta name=\"apple-mobile-web-app-capable\" content=\"yes\">",
      v, h);
    if (c->title[0] && !str_eq(c->title, app_name())) H(b, "<title>%s · %s</title>", c->title, app_name());
    else H(b, "<title>%s</title>", app_name());
    H(b,
      "<link rel=\"icon\" href=\"/static/icon.svg?v=%s\" type=\"image/svg+xml\">"
      "<link rel=\"manifest\" href=\"/static/manifest.webmanifest?v=%s\">"
      "<link rel=\"stylesheet\" href=\"/static/app.css?v=%s\">"
      "<script src=\"/static/wire.js?v=%s\" defer></script></head><body>",
      v, v, v, v);
    buf_put(b, app_html, n);
    buf_puts(b, "<div id=\"toasts\" aria-live=\"polite\">");
    if (flash) {
        static const char *kinds[] = {"ok", "info", "err"};
        H(b, "<div class=\"toast t-%s flash\">%s</div>", kinds[CLAMP(flash_kind, 0, 2)], flash);
    }
    buf_puts(b, "</div></body></html>");
}
