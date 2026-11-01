/* core/ui.c — see ui.h */
#include "ui.h"
#include "json.h"

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
    {"expand", "<path d=\"M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5\"/>"},
    {"shrink", "<path d=\"M9 4v5H4M20 9h-5V4M15 20v-5h5M4 15h5v5\"/>"},
    {"go", "<path fill=\"currentColor\" d=\"M7.5 4.8v14.4L19 12z\"/>"},
    {"pause", "<path fill=\"currentColor\" d=\"M6.5 5h3.5v14H6.5zM14 5h3.5v14H14z\"/>"},
    {"back10", "<path d=\"M4.6 13A7.5 7.5 0 1 0 6.8 6.7\"/><path d=\"M6.5 2.8v4.1h4.1\"/><text x=\"12.4\" y=\"15.9\" "
               "text-anchor=\"middle\" font-size=\"7.6\" font-weight=\"700\" fill=\"currentColor\" "
               "stroke=\"none\">10</text>"},
    {"fwd10", "<path d=\"M19.4 13A7.5 7.5 0 1 1 17.2 6.7\"/><path d=\"M17.5 2.8v4.1h-4.1\"/><text x=\"11.6\" y=\"15.9\" "
              "text-anchor=\"middle\" font-size=\"7.6\" font-weight=\"700\" fill=\"currentColor\" "
              "stroke=\"none\">10</text>"},
    {"back15", "<path d=\"M4.6 13A7.5 7.5 0 1 0 6.8 6.7\"/><path d=\"M6.5 2.8v4.1h4.1\"/><text x=\"12.4\" y=\"15.9\" "
               "text-anchor=\"middle\" font-size=\"7.6\" font-weight=\"700\" fill=\"currentColor\" "
               "stroke=\"none\">15</text>"},
    {"fwd30", "<path d=\"M19.4 13A7.5 7.5 0 1 1 17.2 6.7\"/><path d=\"M17.5 2.8v4.1h-4.1\"/><text x=\"11.6\" y=\"15.9\" "
              "text-anchor=\"middle\" font-size=\"7.6\" font-weight=\"700\" fill=\"currentColor\" "
              "stroke=\"none\">30</text>"},
    {"prev", "<path fill=\"currentColor\" d=\"M18 5.5v13L8.5 12z\"/><path d=\"M6 5.5v13\"/>"},
    {"next", "<path fill=\"currentColor\" d=\"M6 5.5v13l9.5-6.5z\"/><path d=\"M18 5.5v13\"/>"},
    {"moon", "<path d=\"M20 14.5A8 8 0 0 1 9.5 4 8 8 0 1 0 20 14.5z\"/>"},
    {"book", "<path d=\"M5 4.5A1.5 1.5 0 0 1 6.5 3H19v15H6.5A1.5 1.5 0 0 0 5 19.5z\"/><path d=\"M5 19.5A1.5 1.5 0 0 0 "
             "6.5 21H19v-3\"/><path d=\"M9 7h6\"/>"},
    {"sound", "<path d=\"M4 9.5h3.5L12 5.5v13l-4.5-4H4z\"/><path d=\"M15.5 9a4 4 0 0 1 0 6M18 6.5a7.5 7.5 0 0 1 0 11\"/>"},
    {"muted", "<path d=\"M4 9.5h3.5L12 5.5v13l-4.5-4H4z\"/><path d=\"m16 9.5 5 5M21 9.5l-5 5\"/>"},
    {"cc", "<rect x=\"3\" y=\"5.5\" width=\"18\" height=\"13\" rx=\"2.5\"/><path d=\"M10.6 10.2a2.3 2.3 0 1 0 0 3.6M17 "
           "10.2a2.3 2.3 0 1 0 0 3.6\"/>"},
    {"speech", "<path d=\"M4 5h16v10.5H10.5L6 19v-3.5H4z\"/><path d=\"M8 9h8M8 12h5\"/>"},
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

/* named: the avatar alone says who (issue #69: a to-do's 🐻 was read as nothing); decorative: the
   name is printed right beside it, so a screen reader skips the picture */
static void avatar(Buf *b, const Person *p, const char *size, bool named) {
    if (!p) {
        if (named) H(b, "<span class=\"av av-%s av-none\" role=\"img\" aria-label=\"%s\">?</span>", size, T("Someone"));
        else H(b, "<span class=\"av av-%s av-none\" aria-hidden=\"true\">?</span>", size);
        return;
    }
    char label[80];
    person_label(p, label, sizeof label); /* "Eva (moved out)" on what they did (issue #45) */
    H(b, "<span class=\"av av-%s%r\" title=\"%s\"", size, p->left ? " av-left" : "", label);
    if (named) H(b, " role=\"img\" aria-label=\"%s\"", label);
    else buf_puts(b, " aria-hidden=\"true\"");
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

void ui_avatar(Buf *b, const Person *p, const char *size) { avatar(b, p, size, true); }
void ui_avatar_deco(Buf *b, const Person *p, const char *size) { avatar(b, p, size, false); }

void ui_person(Buf *b, const Person *p) {
    H(b, "<span class=\"who\">");
    ui_avatar_deco(b, p, "xs");
    H(b, "<span>%s</span></span>", p ? p->name : T("Someone"));
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
        if (more_href) H(b, "<a class=\"more\" href=\"%s\">%s</a>", more_href, more_label ? more_label : T("See all"));
        buf_puts(b, "</header>");
    }
}

void ui_card_end(Buf *b) { buf_puts(b, "</section>"); }

void ui_admins(Buf *b) {
    const Person *a[4];
    int n = 0;
    for (int i = 0; i < account_count() && n < 4; i++) {
        const Person *p = account_at(i);
        if (p->role == ROLE_ADMIN && p->username[0] && p->pass[0]) a[n++] = p;
    }
    if (n == 0 || n > 3) buf_puts(b, T("an admin"));
    else if (n == 1) H(b, T("%s (admin)"), a[0]->name);
    else if (n == 2) H(b, T("%s or %s (admins)"), a[0]->name, a[1]->name);
    else H(b, T("%s, %s or %s (admins)"), a[0]->name, a[1]->name, a[2]->name);
}

void ui_empty(Buf *b, const char *emoji, const char *title, const char *text) {
    H(b, "<div class=\"empty\"><div class=\"empty-e\">%s</div><h3>%s</h3>", emoji, title);
    if (text) H(b, "<p>%s</p>", text);
    buf_puts(b, "</div>");
}

/* ---------------------------------------------------------------- player */

/* "1:02:03" / "22:35" */
static void clock_text(Buf *b, int s) {
    s = MAX(s, 0);
    if (s >= 3600) H(b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else H(b, "%d:%02d", s / 60, s % 60);
}

/* One of the bar's buttons: data-p names what it does (wire.js), two icons when it swaps (CSS picks
   by the overlay's state: .pl-a shows in the first state, .pl-b in the other). */
static void pl_button(Buf *b, const char *what, const char *label, const char *icon, const char *icon2) {
    H(b, "<button type=\"button\" class=\"pl-%s\" data-p=\"%s\" aria-label=\"%s\" title=\"%s\">", what, what, label, label);
    if (icon2) buf_puts(b, "<span class=\"pl-a\">");
    ui_icon(b, icon);
    if (icon2) {
        buf_puts(b, "</span><span class=\"pl-b\">");
        ui_icon(b, icon2);
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</button>");
}

void ui_player(Buf *b, const UiPlayer *p) {
    H(b, "<div class=\"pl%r\" id=\"%s-pl\" data-player=\"%s\" data-static=\"%s\" data-paused hidden>", p->live ? " pl-live" : "",
      p->box, p->video, p->key);
    /* big in the middle while paused (the bar's own play button is the one to tab to) - a live
       channel too: a browser that won't start one by itself waits for a press */
    H(b, "<button type=\"button\" class=\"pl-big\" data-p=\"play\" tabindex=\"-1\" aria-label=\"%s\">", T("Play"));
    ui_icon(b, "go");
    buf_puts(b, "</button>");
    /* with the browser's own player in Hemma's place (wire.js: data-native) the bar is gone, but full
       screen stays the box's - the "Next episode" card shows there, and the next episode keeps it */
    H(b, "<button type=\"button\" class=\"pl-natfs\" id=\"%s-natfs\" data-fullscreen=\"%s\" data-static hidden "
         "aria-pressed=\"false\" aria-label=\"%s\" title=\"%s\"><span class=\"pl-a\">",
      p->box, p->box, T("Full screen"), T("Full screen"));
    ui_icon(b, "expand");
    buf_puts(b, "</span><span class=\"pl-b\">");
    ui_icon(b, "shrink");
    buf_puts(b, "</span></button>");
    /* the browser can't play it (wire.js sets data-err on the video's "error"): say so, not spin */
    H(b, "<p class=\"pl-err\" role=\"alert\">%s</p>", p->trouble ? p->trouble : T("This video won't play in this browser."));
    H(b, "<span class=\"pl-spin\" aria-hidden=\"true\"></span><div class=\"pl-bar\"%r>", p->tv ? " data-arrows" : "");
    if (!p->live)
        H(b, "<div class=\"pl-seek\"><span class=\"pl-buf\"></span><span class=\"pl-fill\"></span><input type=\"range\" "
             "class=\"pl-range\" data-p=\"seek\" min=\"0\" max=\"%d\" step=\"any\" value=\"0\" aria-label=\"%s\"></div>",
          MAX(p->seconds, 1), T("Where in the video"));
    buf_puts(b, "<div class=\"pl-row\">");
    if (p->live) {
        H(b, "<span class=\"pl-onair\">%s</span>", T("Live"));
    } else {
        pl_button(b, "play", T("Play (Space)"), "go", "pause"); /* wire.js: "Pause (Space)" while playing (#64) */
        pl_button(b, "back", T("Back 10 seconds (Left)"), "back10", NULL);
        pl_button(b, "fwd", T("Forward 10 seconds (Right)"), "fwd10", NULL);
        buf_puts(b, "<span class=\"pl-time\"><span data-p=\"at\">0:00</span> / <span data-p=\"dur\">");
        if (p->seconds > 0) clock_text(b, p->seconds);
        else buf_puts(b, "-:--");
        buf_puts(b, "</span></span>");
    }
    buf_puts(b, "<span class=\"pl-gap\"></span>");
    if (p->nsubs > 0) {
        bool any = false;
        for (int i = 0; i < p->nsubs; i++) any |= p->subs[i].on;
        buf_puts(b, "<span class=\"pl-subs\">");
        pl_button(b, "subs", T("Subtitles (C)"), "cc", NULL);
        H(b, "<span class=\"pl-menu\" role=\"menu\" hidden><button type=\"button\" role=\"menuitemradio\" data-sub=\"\" "
             "aria-checked=\"%s\">%s</button>",
          any ? "false" : "true", T("Off"));
        for (int i = 0; i < p->nsubs; i++) {
            H(b, "<button type=\"button\" role=\"menuitemradio\" data-sub=\"%s\"", p->subs[i].key);
            if (p->subs[i].pic) H(b, " data-subpic=\"%s\"", p->subs[i].pic);
            if (p->subs[i].pref) buf_puts(b, " data-subpref");
            H(b, " aria-checked=\"%s\">%s</button>", p->subs[i].on ? "true" : "false", p->subs[i].label);
        }
        buf_puts(b, "</span></span>");
    }
    if (p->nsounds > 1) { /* the sound's language (2.2): each item the same video with that sound - wire.js swaps it */
        buf_puts(b, "<span class=\"pl-subs pl-snds\">");
        pl_button(b, "snd", T("Language"), "speech", NULL);
        buf_puts(b, "<span class=\"pl-menu\" role=\"menu\" hidden>");
        for (int i = 0; i < p->nsounds; i++)
            H(b, "<button type=\"button\" role=\"menuitemradio\" data-snd=\"%d\" data-src=\"%s\" aria-checked=\"%s\">%s</button>",
              p->sounds[i].n, p->sounds[i].src, p->sounds[i].on ? "true" : "false", p->sounds[i].label);
        buf_puts(b, "</span></span>");
    }
    /* the sound: muted swaps the icon; on a live channel it also says so in words (a TV that won't
       start with sound starts it muted) */
    H(b, "<button type=\"button\" class=\"pl-mute\" data-p=\"mute\" aria-label=\"%s\" aria-pressed=\"false\" "
         "title=\"%s\"%r>"
         "<span class=\"pl-a\">",
      T("Mute (M)"), T("Sound (M)"), p->focus_sound ? " autofocus" : "");
    ui_icon(b, "sound");
    buf_puts(b, "</span><span class=\"pl-b\">");
    ui_icon(b, "muted");
    H(b, "<span class=\"pl-mute-t\">%s</span></span></button>"
         "<input type=\"range\" class=\"pl-vol\" data-p=\"vol\" min=\"0\" max=\"1\" step=\"0.05\" value=\"1\" "
         "aria-label=\"%s\">",
      T("Sound off"), T("Volume"));
    /* full screen: the box, not the video (what's drawn over it stays) - wire.js [data-fullscreen] */
    H(b, "<button type=\"button\" class=\"pl-fs\" id=\"%s-fs\" data-fullscreen=\"%s\" data-static hidden aria-pressed=\"false\" "
         "aria-label=\"%s\" title=\"%s\"><span class=\"pl-a\">",
      p->box, p->box, T("Full screen (F)"), T("Full screen (F)"));
    ui_icon(b, "expand");
    buf_puts(b, "</span><span class=\"pl-b\">");
    ui_icon(b, "shrink");
    buf_puts(b, "</span></button></div></div></div>");
}

/* ----------------------------------------------------------------- forms */

/* Open forms, innermost last: where each began, and whether it goes again at its end (ui_role). */
static int g_role = ROLE_MEMBER;
static bool g_view_only; /* a role that may not run everything: forms and dialogs get cut */
static struct {
    Buf *b;
    size_t mark;
    bool cut;
} g_forms[8];
static int g_nforms;
/* The open dialog in view-only mode: where it began, and whether a form in it was cut - then the whole
   dialog goes (issue #96: a hall tablet opened "New event" boxes with nothing in them). */
static struct {
    Buf *b;
    size_t mark;
    bool open, cut;
} g_modal;

void ui_role(int role) {
    g_role = role;
    g_view_only = role == ROLE_DISPLAY || role == ROLE_CHILD || role == ROLE_GUEST;
    g_nforms = 0;
    g_modal.open = false;
}

bool ui_can(const char *action) { return !g_view_only || app_action_open(action, g_role); }

static void form_begins(Buf *b, const char *action) {
    if (g_nforms < countof(g_forms)) {
        g_forms[g_nforms].b = b;
        g_forms[g_nforms].mark = b->len;
        g_forms[g_nforms].cut = g_view_only && !app_action_open(action, g_role);
    }
    g_nforms++;
}

void ui_form(Buf *b, const char *action, const char *cls) {
    form_begins(b, action);
    H(b, "<form method=\"post\" action=\"/a/%s\" data-a=\"%s\"", action, action);
    if (cls) H(b, " class=\"%s\"", cls);
    buf_putc(b, '>');
}

void ui_form_confirm(Buf *b, const char *action, const char *cls, const char *question) {
    form_begins(b, action);
    H(b, "<form method=\"post\" action=\"/a/%s\" data-a=\"%s\" data-confirm=\"%s\"", action, action, question);
    if (cls) H(b, " class=\"%s\"", cls);
    buf_putc(b, '>');
}

void ui_form_end(Buf *b) {
    buf_puts(b, "</form>");
    if (g_nforms <= 0) return;
    int i = --g_nforms;
    if (i < countof(g_forms) && g_forms[i].cut && g_forms[i].b == b && g_forms[i].mark <= b->len) {
        b->len = g_forms[i].mark; /* the whole form, gone */
        if (b->p) b->p[b->len] = '\0';
        if (g_modal.open && g_modal.b == b) g_modal.cut = true;
    }
}

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
    static const char *names[] = {N_("Red"),  N_("Orange"), N_("Amber"),  N_("Lime"),   N_("Green"),   N_("Teal"),
                                  N_("Sky"),  N_("Blue"),   N_("Indigo"), N_("Purple"), N_("Magenta"), N_("Pink")};
    H(b, "<div class=\"hues\" role=\"radiogroup\" aria-label=\"%s\">", T("Colour"));
    for (int i = 0; i < countof(hues); i++) {
        H(b, "<label class=\"hue\" title=\"%s\"", T(names[i]));
        ui_hue_style(b, hues[i]);
        H(b, "><input type=\"radio\" name=\"%s\" value=\"%d\" aria-label=\"%s\"%r><span></span></label>", name, hues[i],
          T(names[i]), hues[i] == selected ? " checked" : "");
    }
    buf_puts(b, "</div>");
}

/* ----------------------------------------------------------------- modal */

void ui_modal(Buf *b, const char *title, const char *close_href) {
    if (g_view_only) {
        g_modal.b = b;
        g_modal.mark = b->len;
        g_modal.open = true;
        g_modal.cut = false;
    }
    H(b,
      "<div class=\"modal\" role=\"dialog\" aria-modal=\"true\" aria-labelledby=\"modal-t\">"
      "<a class=\"modal-bg\" href=\"%s\" data-esc tabindex=\"-1\" aria-hidden=\"true\"></a><div class=\"modal-card\">"
      "<header class=\"modal-h\"><h2 id=\"modal-t\">%s</h2><a class=\"icon-btn\" href=\"%s\" aria-label=\"%s\">",
      close_href, title, close_href, T("Close")); /* (wire.js moves the focus in and keeps it there: issue #63) */
    ui_icon(b, "x");
    buf_puts(b, "</a></header><div class=\"modal-b\">");
}

void ui_modal_end(Buf *b) {
    buf_puts(b, "</div></div></div>");
    if (!g_modal.open || g_modal.b != b) return;
    g_modal.open = false;
    if (g_modal.cut && g_modal.mark <= b->len) { /* a dialog for doing what this screen may not: all of it */
        b->len = g_modal.mark;
        if (b->p) b->p[b->len] = '\0';
    }
}

/* ------------------------------------------------------------------ time */

void ui_hm(Buf *b, int64_t t) {
    char s[6];
    fmt_hm(t, s);
    buf_puts(b, s);
}

/* A day in words. Swedish and Spanish write weekdays and months in lower case (base.c): first =
   the words start a line, so their first letter gets its capital (every name starts with a plain
   letter). English names keep theirs anyway. */
static void day_words(Buf *b, int64_t day, int64_t now, bool first) {
    int64_t d0 = day_start(day), today = day_start(now);
    DateTime d = dt_local(day), n = dt_local(now);
    size_t at = b->len;
    if (d0 == today) buf_puts(b, first ? T("Today") : T("today"));
    else if (d0 == add_days(today, 1)) buf_puts(b, first ? T("Tomorrow") : T("tomorrow"));
    else if (d0 == add_days(today, -1)) buf_puts(b, first ? T("Yesterday") : T("yesterday"));
    else if (d0 > today && d0 < add_days(today, 7)) buf_puts(b, weekday_name(d.wday, false));
    else if (d.year == n.year) buf_printf(b, "%s %d %s", weekday_name(d.wday, true), d.day, month_name(d.mon, true));
    else buf_printf(b, "%d %s %d", d.day, month_name(d.mon, true), d.year);
    if (first && b->len > at && b->p[at] >= 'a' && b->p[at] <= 'z') b->p[at] = (char)(b->p[at] - 'a' + 'A');
}

void ui_day(Buf *b, int64_t day, int64_t now) { day_words(b, day, now, true); }

void ui_day_inline(Buf *b, int64_t day, int64_t now) { day_words(b, day, now, false); }

void ui_ago(Buf *b, int64_t t, int64_t now) {
    int64_t s = now - t;
    if (s < 60) buf_puts(b, T("just now"));
    else if (s < 3600) buf_printf(b, T("%lld min ago"), (long long)(s / 60));
    else if (s < 6 * 3600 || day_start(t) == day_start(now)) buf_printf(b, T("%lld h ago"), (long long)(s / 3600));
    else if (day_start(t) == add_days(day_start(now), -1)) buf_puts(b, T("yesterday"));
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
        if (day_start(last) == day_start(start)) buf_puts(b, T("all day"));
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
        /* a section's pages that aren't in the navigation light up their parent (tv -> Watch) */
        bool on = m == active || (active && active->parent && str_eq(active->parent, m->id));
        bool extra = tabbar && k++ >= TAB_SLOTS;
        if (on && extra) more_on = true;
        H(b, "<a href=\"%s%s\"%r%r>", "/", str_eq(m->id, "home") ? "" : m->id,
          extra ? (on ? " class=\"tab-x on\"" : " class=\"tab-x\"") : on ? " class=\"on\"" : "",
          on ? " aria-current=\"page\"" : "");
        ui_icon(b, m->icon);
        H(b, "<span>%s</span></a>", T(m->title));
    }
    if (tabbar) { /* shown by CSS only where the bar runs out of room */
        H(b, "<a class=\"tab-more%r\" href=\"/more\">", more_on ? " on" : "");
        ui_icon(b, "grid");
        H(b, "<span>%s</span></a>", T("More"));
    }
}

void ui_more_page(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("More"));
    ui_head(b, T("Everything"), NULL);
    ui_head_end(b);
    buf_puts(b, "<nav class=\"more-grid\">");
    for (int i = 0; i < module_count(); i++) {
        const Module *m = module_at(i);
        if (!m->nav || !module_visible(m, c)) continue;
        H(b, "<a href=\"/%s\">", str_eq(m->id, "home") ? "" : m->id);
        ui_icon(b, m->icon);
        H(b, "<span>%s</span></a>", T(m->title));
    }
    buf_puts(b, "</nav><div class=\"more-side\">"); /* the sidebar's blocks: phones have no sidebar */
    for (int i = 0; i < module_count(); i++)
        if (module_at(i)->side && module_visible(module_at(i), c)) module_at(i)->side(c, b);
    buf_puts(b, "</div>");
}

void ui_shell(Ctx *c, Buf *b, const Module *active, const char *page, size_t page_len) {
    /* the first thing Tab reaches, past the sidebar's dozen links (issue #68) */
    H(b, "<a class=\"skip\" href=\"#main\">%s</a><aside class=\"side\"><a class=\"brand\" href=\"/\"><span class=\"logo\">",
      T("Skip to content"));
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
        if (!shown++) H(b, "<div class=\"here\"><span class=\"here-l\">%s</span><div class=\"here-a\">", T("Here now"));
        H(b, "<span class=\"here-p\" title=\"%s\">", p->name);
        ui_avatar(b, p, "sm");
        buf_puts(b, "</span>");
    }
    if (shown) buf_puts(b, "</div></div>");
    for (int i = 0; i < module_count(); i++) /* modules' small blocks (e.g. the library's disk space) */
        if (module_at(i)->side && module_visible(module_at(i), c)) module_at(i)->side(c, b);
    if (c->me) {
        buf_puts(b, "<a class=\"me\" href=\"/people\">");
        ui_avatar_deco(b, c->me, "sm");
        H(b, "<span>%s</span></a>", c->me->name);
    }
    buf_puts(b, "</div></aside><main class=\"main\" id=\"main\" tabindex=\"-1\">");
    if (app_demo()) /* hemma --demo: every page says so (issue #31) */
        H(b, "<p class=\"demo-band\" role=\"note\">%r</p>",
          T("A demo with a made-up household: try anything, nothing here is real. The next <code>hemma --demo</code> "
            "starts afresh."));
    buf_put(b, page, page_len);
    buf_puts(b, "</main><nav class=\"tabbar\">");
    nav_links(c, b, active, true);
    buf_puts(b, "</nav>");
}

/* wire.js's own words (its T('...') calls - core/i18n_test.c finds every one here): each page
   carries them in its language as JSON, <script type="application/json" id="wire-tr"> (data, not a
   script: the CSP doesn't mind). Their translations are core/ui_lang.c's. */
static const char *const WIRE_TEXTS[] = {
    "Photos need the connection to Hemma - wait a moment and send it again",
    "That doesn't look like a photo",
    "The photo couldn't be read",
    "That photo is too big to send - try another",
    "Unexpected answer (%d)",
    "An upload is already running",
    "Choose a folder or some files first",
    "The upload stopped",
    "The converter stopped",
    "The upload was stopped",
    "The connection keeps dropping",
    "The browser lost the copy it made - is its disk full? Make room and start the upload again: what was sent stays",
    "Undo",
    "+%d more",
    "Didn't fit: %s",
    "Play",
    "Pause",
    "Play (Space)",
    "Pause (Space)",
    "%s of %s",
    "This browser can't convert videos",
    "Converting on this device - keep this page open until it's done",
    "Done: the new copies play everywhere",
};

const char *const *ui_wire_texts(int *n) {
    *n = (int)(sizeof WIRE_TEXTS / sizeof WIRE_TEXTS[0]);
    return WIRE_TEXTS;
}

static void wire_words(Buf *b) {
    if (i18n_lang() == LANG_EN) return; /* (the keys are the English) */
    Buf j = {0};
    buf_putc(&j, '{');
    for (size_t i = 0; i < sizeof WIRE_TEXTS / sizeof WIRE_TEXTS[0]; i++) {
        const char *to = T(WIRE_TEXTS[i]);
        if (to == WIRE_TEXTS[i]) continue;
        if (j.len > 1) buf_putc(&j, ',');
        json_str(&j, WIRE_TEXTS[i]);
        buf_putc(&j, ':');
        json_str(&j, to);
    }
    buf_putc(&j, '}');
    buf_puts(b, "<script type=\"application/json\" id=\"wire-tr\">");
    for (size_t i = 0; i < j.len; i++) /* (no "</script>" can end it early) */
        if (j.p[i] == '<') buf_puts(b, "\\u003c");
        else buf_putc(b, j.p[i]);
    buf_puts(b, "</script>");
    buf_free(&j);
}

void ui_document(Ctx *c, Buf *b, const char *app_html, size_t n, const char *flash, int flash_kind) {
    const char *v = app_asset_version();
    /* data-h: the hash of the #app below; the wire's first hello sends it back, so the server
       doesn't resend a page the browser already has (core/app.c, Client.html_hash) */
    char h[17];
    snprintf(h, sizeof h, "%016llx", (unsigned long long)hash64(app_html, n));
    H(b,
      "<!doctype html><html lang=\"%s\" data-v=\"%s\" data-h=\"%s\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
      "<meta name=\"color-scheme\" content=\"light dark\">"
      "<meta name=\"theme-color\" content=\"#f4f1ea\" media=\"(prefers-color-scheme: light)\">"
      "<meta name=\"theme-color\" content=\"#121316\" media=\"(prefers-color-scheme: dark)\">"
      "<meta name=\"mobile-web-app-capable\" content=\"yes\"><meta name=\"apple-mobile-web-app-capable\" content=\"yes\">",
      i18n_code(c->lang), v, h);
    if (c->title[0] && !str_eq(c->title, app_name())) H(b, "<title>%s · %s</title>", c->title, app_name());
    else H(b, "<title>%s</title>", app_name());
    H(b,
      "<link rel=\"icon\" href=\"/static/icon.svg?v=%s\" type=\"image/svg+xml\">"
      "<link rel=\"manifest\" href=\"/static/manifest.webmanifest?v=%s\">"
      "<link rel=\"stylesheet\" href=\"/static/app.css?v=%s\">"
      "<script src=\"/static/wire.js?v=%s\" defer></script>",
      v, v, v, v);
    wire_words(b);
    buf_puts(b, "</head><body>");
    buf_put(b, app_html, n);
    buf_puts(b, "<div id=\"toasts\" aria-live=\"polite\">");
    if (flash) {
        static const char *kinds[] = {"ok", "info", "err"};
        H(b, "<div class=\"toast t-%s flash\">%s</div>", kinds[CLAMP(flash_kind, 0, 2)], flash);
    }
    buf_puts(b, "</div></body></html>");
}
