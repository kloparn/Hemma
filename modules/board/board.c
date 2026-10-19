/* modules/board — the fridge door: short notes for the whole household.
 *
 * Notes can be pinned, coloured and set to disappear after a while ("the
 * plumber comes Tuesday"). Expired notes are hidden but kept for 60 days in
 * case someone wants them back, then cleaned up on start. Anyone in the
 * household may edit or remove any note; the activity feed records who did. */
#include "../../core/app.h"
#include "../../core/ui.h"

#include <ctype.h>

#define NOTE_MAX_CHARS 600
#define DAY 86400
#define KEEP_EXPIRED_DAYS 60

typedef struct {
    int id;
    char text[NOTE_MAX_CHARS * 4 + 4]; /* 600 characters of any width */
    int hue;
    bool pinned;
    int by;        /* author */
    int edited_by; /* last editor, 0 = never edited */
    int64_t created, updated;
    int64_t expires; /* 0 = keep; hidden from this moment on */
} Note;

static const FieldDef note_fields[] = {
    FIELD(Note, id, F_INT),        FIELD(Note, text, F_STR),      FIELD(Note, hue, F_INT),
    FIELD(Note, pinned, F_BOOL),   FIELD(Note, by, F_INT),        FIELD(Note, edited_by, F_INT),
    FIELD(Note, created, F_I64),   FIELD(Note, updated, F_I64),   FIELD(Note, expires, F_I64),
};
static Table T_note = TABLE("board_note", Note, note_fields);

/* ---------------------------------------------------------------- helpers */

static bool is_expired(const Note *n, int64_t now) { return n->expires > 0 && now >= n->expires; }

static int cmp_notes(const void *a, const void *b) {
    const Note *x = *(const Note *const *)a, *y = *(const Note *const *)b;
    if (x->pinned != y->pinned) return x->pinned ? -1 : 1;
    if (x->created != y->created) return x->created > y->created ? -1 : 1;
    return y->id - x->id;
}

/* Notes that are (not) expired, pinned first then newest. Free *out. */
static int collect(int64_t now, bool expired, Note ***out) {
    int n = tbl_count(&T_note), k = 0;
    Note **v = xmalloc((size_t)(n ? n : 1) * sizeof *v);
    for (int i = 0; i < n; i++) {
        Note *x = tbl_at(&T_note, i);
        if (is_expired(x, now) == expired) v[k++] = x;
    }
    qsort(v, (size_t)k, sizeof *v, cmp_notes);
    *out = v;
    return k;
}

/* Trim, drop spaces before line breaks (a CR arrives as a space), allow at
 * most one empty line in a row. out must hold strlen(in) + 1 bytes. */
static void clean_text(const char *in, char *out) {
    size_t o = 0;
    int breaks = 0;
    while (*in == ' ' || *in == '\n' || *in == '\t') in++;
    for (; *in; in++) {
        char ch = *in == '\t' ? ' ' : *in;
        if (ch == '\n') {
            while (o && out[o - 1] == ' ') o--;
            if (++breaks <= 2) out[o++] = '\n';
            continue;
        }
        if (ch != ' ') breaks = 0;
        out[o++] = ch;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '\n')) o--;
    out[o] = '\0';
}

/* First `max` characters on one line, with an ellipsis when cut. */
static void snippet(const char *text, char *out, size_t cap, int max) {
    size_t o = 0, left = strlen(text);
    int chars = 0;
    const char *p = text;
    while (left && chars < max && o + 4 < cap) {
        uint32_t cp;
        size_t k = (size_t)utf8_decode(p, left, &cp);
        if (*p == '\n') out[o++] = ' ';
        else {
            memcpy(out + o, p, k);
            o += k;
        }
        p += k;
        left -= k;
        chars++;
    }
    if (*p && o + 4 <= cap) {
        memcpy(out + o, "…", 3);
        o += 3;
    }
    out[o] = '\0';
}

static bool url_char(unsigned char ch) {
    return ch > ' ' && ch != '<' && ch != '>' && ch != '"' && ch != '\'' && ch != '`' && ch != 0x7F;
}

/* Escaped text with line breaks as <br> and http(s) URLs as links. The URL
 * is escaped like any other text, so nothing typed can leave the href. */
static void render_text(Buf *b, const char *s, bool links) {
    size_t n = strlen(s), i = 0, run = 0;
    while (i < n) {
        if (s[i] == '\n') {
            html_escn(b, s + run, i - run);
            buf_puts(b, "<br>");
            run = ++i;
            continue;
        }
        bool at_url = links && (i == 0 || !isalnum((unsigned char)s[i - 1])) &&
                      (str_istarts(s + i, "http://") || str_istarts(s + i, "https://"));
        if (at_url) {
            size_t scheme = (s[i + 4] == 's' || s[i + 4] == 'S') ? 8 : 7;
            size_t j = i;
            while (j < n && url_char((unsigned char)s[j])) j++;
            while (j > i && strchr(".,;:!?", s[j - 1])) j--;
            int open = 0, close = 0;
            for (size_t k = i; k < j; k++) open += s[k] == '(', close += s[k] == ')';
            while (j > i && s[j - 1] == ')' && close > open) j--, close--;
            if (j > i + scheme) {
                html_escn(b, s + run, i - run);
                buf_puts(b, "<a href=\"");
                html_escn(b, s + i, j - i);
                buf_puts(b, "\" target=\"_blank\" rel=\"noopener noreferrer\">");
                const char *disp = s + i + scheme;
                size_t dl = j - i - scheme;
                if (dl > 4 && str_istarts(disp, "www.")) disp += 4, dl -= 4;
                if (dl > 42) {
                    char tmp[64];
                    str_ncopy(tmp, sizeof tmp, disp, 40);
                    html_esc(b, tmp);
                    buf_puts(b, "…");
                } else {
                    html_escn(b, disp, dl);
                }
                buf_puts(b, "</a>");
                i = run = j;
                continue;
            }
        }
        i++;
    }
    html_escn(b, s + run, n - run);
}

static bool checked(const Fields *f, const char *key) {
    const char *v = fields_get(f, key);
    return v && *v && !str_eq(v, "0");
}

static bool read_text(Ctx *c, Fields *f, char *dst, size_t cap) {
    const char *raw = fields_str(f, "text");
    char *t = xmalloc(strlen(raw) + 1);
    clean_text(raw, t);
    size_t chars = utf8_count(t);
    bool ok = false;
    if (!t[0]) {
        ctx_fail_at(c, "text", "Write something first");
    } else if (chars > NOTE_MAX_CHARS) {
        ctx_fail_at(c, "text", "Keep it under %d characters (that one has %zu)", NOTE_MAX_CHARS, chars);
    } else {
        str_copy(dst, cap, t);
        ok = true;
    }
    free(t);
    return ok;
}

/* until=YYYY-MM-DD wins when filled in (visible through that day); otherwise
 * keep = "0" | "1d" | "7d" | "keep" (leave as is). */
static bool read_expiry(Ctx *c, Fields *f, int64_t current, int64_t *out) {
    const char *keep = fields_str(f, "keep"), *until = fields_str(f, "until");
    if (until[0] || str_eq(keep, "date")) {
        int y, m, d;
        if (!parse_ymd(until, &y, &m, &d)) {
            ctx_fail_at(c, "until", "Pick a date for the note to disappear");
            return false;
        }
        *out = dt_make(y, m, d + 1, 0, 0);
        if (*out <= c->now) {
            ctx_fail_at(c, "until", "That date has already passed");
            return false;
        }
    } else if (str_eq(keep, "keep")) *out = current;
    else if (str_eq(keep, "1d")) *out = c->now + DAY;
    else if (str_eq(keep, "7d")) *out = c->now + 7 * DAY;
    else *out = 0;
    return true;
}

static Note *note_arg(Ctx *c, Fields *f) {
    Note *n = tbl_get(&T_note, (int)fields_int(f, "id", 0));
    if (!n) ctx_fail(c, "That note is gone");
    return n;
}

static void back_to_board(Ctx *c) { ctx_redirect(c, "%s", str_eq(ctx_q(c, "expired"), "1") ? "/board?expired=1" : "/board"); }

/* ------------------------------------------------------------------ pieces */

/* ui_day() says "Today"/"Tomorrow"; mid-sentence those read better lower-case. */
static void day_inline(Buf *b, int64_t t, int64_t now) {
    size_t at = b->len;
    ui_day(b, t, now);
    if (b->len > at && (str_starts(b->p + at, "Today") || str_starts(b->p + at, "Tomorrow") ||
                        str_starts(b->p + at, "Yesterday")))
        b->p[at] = (char)(b->p[at] + 32);
}

static void expiry_label(Buf *b, const Note *n, int64_t now) {
    DateTime e = dt_local(n->expires);
    bool midnight = e.hour == 0 && e.min == 0;
    buf_puts(b, is_expired(n, now) ? "hidden since " : "until ");
    if (midnight && !is_expired(n, now)) {
        day_inline(b, n->expires - 1, now);
    } else {
        day_inline(b, n->expires, now);
        buf_putc(b, ' ');
        ui_hm(b, n->expires);
    }
}

static void note_actions(Buf *b, const Note *n, bool expired_view) {
    char id[16];
    snprintf(id, sizeof id, "%d", n->id);
    buf_puts(b, "<div class=\"board-note-x\">");
    /* each button names its note: a screen reader's list of buttons said "Pin to the top" over and over (#69) */
    char snip[80], what[160];
    snippet(n->text, snip, sizeof snip, 30);
    Buf icon = {0};
    ui_icon(&icon, expired_view ? "check" : "pin");
    if (expired_view) snprintf(what, sizeof what, "Keep “%s”", snip);
    else snprintf(what, sizeof what, n->pinned ? "Unpin “%s”" : "Pin “%s” to the top", snip);
    ui_act(b, expired_view ? "board.keep" : "board.pin", "icon-btn", buf_cstr(&icon), "id", id, what);
    buf_free(&icon);
    H(b, "<a class=\"icon-btn\" href=\"/board?%redit=%d\" title=\"Edit\" aria-label=\"Edit “%s”\">",
      expired_view ? "expired=1&amp;" : "", n->id, snip);
    ui_icon(b, "edit");
    buf_puts(b, "</a>");
    ui_form(b, "board.delete", "inline");
    ui_hidden_int(b, "id", n->id);
    H(b, "<button class=\"icon-btn\" title=\"Remove\" aria-label=\"Remove “%s”\">", snip);
    ui_icon(b, "trash");
    buf_puts(b, "</button>");
    ui_form_end(b);
    buf_puts(b, "</div>");
}

static void note_card(Ctx *c, Buf *b, const Note *n, bool expired_view) {
    H(b, "<article class=\"board-note%r\" id=\"board-note-%d\"", n->pinned ? " pinned" : "", n->id);
    ui_hue_style(b, n->hue);
    buf_putc(b, '>');
    if (n->pinned) buf_puts(b, "<span class=\"board-note-pin\" title=\"Pinned\"></span>");
    buf_puts(b, "<div class=\"board-note-text\">");
    render_text(b, n->text, true);
    buf_puts(b, "</div>");
    if (n->expires) {
        buf_puts(b, "<div class=\"board-note-exp\">");
        ui_icon(b, "clock");
        expiry_label(b, n, c->now);
        buf_puts(b, "</div>");
    }
    buf_puts(b, "<footer class=\"board-note-f\">");
    Person *p = person_get(n->by);
    ui_avatar_deco(b, p, "xs");
    H(b, "<span class=\"board-note-m\"><b>%s</b> · ", p ? p->name : "Someone");
    ui_ago(b, n->created, c->now);
    if (n->edited_by && n->updated > n->created + 60) {
        Person *e = person_get(n->edited_by);
        H(b, " · <span title=\"Edited by %s\">edited</span>", e ? e->name : "someone");
    }
    buf_puts(b, "</span>");
    if (c->me && c->me->role != ROLE_DISPLAY) note_actions(b, n, expired_view); /* (a hall tablet only reads: #54) */
    buf_puts(b, "</footer></article>");
}

static void keep_radio(Buf *b, const char *value, const char *label, bool on) {
    H(b, "<label class=\"board-keep-o\"><input type=\"radio\" name=\"keep\" value=\"%s\"%r><span>%s</span></label>", value,
      on ? " checked" : "", label);
}

static void post_box(Buf *b) {
    buf_puts(b, "<section class=\"card board-post\">");
    ui_form(b, "board.post", "board-post-f");
    H(b, "<textarea name=\"text\" rows=\"3\" maxlength=\"%d\" required aria-label=\"Note\" "
         "placeholder=\"Leave a note for everyone… reminders, the Wi-Fi password for guests, who picks up whom\">"
         "</textarea><div class=\"board-post-o\">",
      NOTE_MAX_CHARS);
    ui_hue_picker(b, "hue", 45);
    buf_puts(b, "<label class=\"board-pin-toggle\"><input type=\"checkbox\" name=\"pin\" value=\"1\"><span>");
    ui_icon(b, "pin");
    buf_puts(b, "Pin</span></label><div class=\"board-keep\" role=\"radiogroup\" aria-label=\"Keep it for\">");
    keep_radio(b, "0", "Keep", true);
    keep_radio(b, "1d", "1 day", false);
    keep_radio(b, "7d", "1 week", false);
    buf_puts(b, "</div><button class=\"btn primary board-post-go\">Post</button></div>");
    ui_form_end(b);
    buf_puts(b, "</section>");
}

static void edit_modal(Ctx *c, Buf *b, const Note *n, bool expired_view) {
    const char *close = expired_view ? "/board?expired=1" : "/board";
    ui_modal(b, "Edit note", close);
    ui_form(b, "board.save", "stack");
    ui_hidden_int(b, "id", n->id);
    H(b, "<textarea name=\"text\" rows=\"6\" maxlength=\"%d\" required autofocus aria-label=\"Note\">%s</textarea>",
      NOTE_MAX_CHARS, n->text);
    buf_puts(b, "<div class=\"field\"><span>Colour</span>");
    ui_hue_picker(b, "hue", n->hue);
    H(b, "</div><label class=\"board-pin-toggle\"><input type=\"checkbox\" name=\"pin\" value=\"1\"%r><span>",
      n->pinned ? " checked" : "");
    ui_icon(b, "pin");
    buf_puts(b, "Pinned to the top</span></label><div class=\"field\"><span>Show it</span><div class=\"board-keep wrap\">");
    bool has = n->expires > 0 && !is_expired(n, c->now);
    if (has) {
        Buf lbl = {0};
        buf_puts(&lbl, "As now (");
        expiry_label(&lbl, n, c->now);
        buf_putc(&lbl, ')');
        keep_radio(b, "keep", buf_cstr(&lbl), true);
        buf_free(&lbl);
    }
    keep_radio(b, "0", "Always", !has);
    keep_radio(b, "1d", "1 more day", false);
    keep_radio(b, "7d", "1 more week", false);
    char today[11];
    fmt_ymd(c->now, today);
    H(b, "</div><label class=\"board-until\"><span>or until</span><input type=\"date\" name=\"until\" min=\"%s\" "
         "aria-label=\"Show until date\"></label></div>",
      today);
    buf_puts(b, "<div class=\"modal-f\"><button class=\"btn primary\">Save</button></div>");
    ui_form_end(b);
    ui_form(b, "board.delete", "danger-zone");
    ui_hidden_int(b, "id", n->id);
    buf_puts(b, "<button class=\"btn danger\">Remove note</button>");
    ui_form_end(b);
    ui_modal_end(b);
}

/* -------------------------------------------------------------------- page */

static void grid(Ctx *c, Buf *b, Note **v, int n, bool expired_view) {
    buf_puts(b, "<div class=\"board-grid\">");
    for (int i = 0; i < n; i++) note_card(c, b, v[i], expired_view);
    buf_puts(b, "</div>");
}

static void page(Ctx *c, Buf *b) {
    ctx_title(c, "Board");
    bool expired_view = str_eq(ctx_q(c, "expired"), "1");
    Note **live, **old;
    int nlive = collect(c->now, false, &live);
    int nold = collect(c->now, true, &old);
    int npinned = 0;
    while (npinned < nlive && live[npinned]->pinned) npinned++;

    if (expired_view) {
        ctx_title(c, "Hidden notes");
        ui_head(b, "Hidden notes", "They passed their date. Kept for 60 days, then cleaned up.");
        H(b, "<a class=\"btn\" href=\"/board\">");
        ui_icon(b, "left");
        buf_puts(b, "<span>Back to the board</span></a>");
        ui_head_end(b);
        if (nold) grid(c, b, old, nold, true);
        else ui_empty(b, "🧹", "Nothing hidden", "Notes that expire end up here for a while.");
    } else {
        char sub[96];
        if (nlive) snprintf(sub, sizeof sub, "%d note%s%s", nlive, nlive == 1 ? "" : "s", npinned ? " · " : "");
        else snprintf(sub, sizeof sub, "The household's fridge door");
        if (nlive && npinned) snprintf(sub + strlen(sub), sizeof sub - strlen(sub), "%d pinned", npinned);
        ui_head(b, "Board", sub);
        ui_head_end(b);
        post_box(b);
        if (!nlive) {
            ui_empty(b, "📝", "The board is empty", "Leave a note for the household — it shows up on every screen.");
        } else {
            if (npinned) {
                buf_puts(b, "<h2 class=\"board-h\">");
                ui_icon(b, "pin");
                buf_puts(b, "Pinned</h2>");
                grid(c, b, live, npinned, false);
            }
            if (nlive > npinned) {
                if (npinned) buf_puts(b, "<h2 class=\"board-h\">Notes</h2>");
                grid(c, b, live + npinned, nlive - npinned, false);
            }
        }
        if (nold)
            H(b, "<p class=\"board-hidden\"><a href=\"/board?expired=1\">%d hidden note%s</a> — past their date</p>", nold,
              nold == 1 ? "" : "s");
    }

    Note *edit = tbl_get(&T_note, (int)ctx_qint(c, "edit", 0));
    if (edit) edit_modal(c, b, edit, expired_view);
    free(live);
    free(old);
}

/* ------------------------------------------------------------------ widget */

/* The newest notes (pinned first) as little coloured notes; links only where they lead somewhere
   one can go (the dashboard, not a wall). */
/* (a list: a wall that runs out of room shortens it to "+N more" - wire.js [data-fit], issue #97) */
static void minis(Ctx *c, Buf *b, Note **v, int show, bool links) {
    buf_puts(b, "<ul class=\"board-minis\">");
    for (int i = 0; i < show; i++) {
        Note *x = v[i];
        H(b, "<li class=\"board-mini%r\"", x->pinned ? " pinned" : "");
        ui_hue_style(b, x->hue);
        buf_puts(b, "><p>");
        render_text(b, x->text, links);
        buf_puts(b, "</p><small>");
        if (x->pinned) ui_icon(b, "pin");
        Person *p = person_get(x->by);
        H(b, "%s · ", p ? p->name : "Someone");
        ui_ago(b, x->created, c->now);
        buf_puts(b, "</small></li>");
    }
    buf_puts(b, "</ul>");
}

static void widget(Ctx *c, Buf *b, int where) {
    (void)where;
    Note **v;
    int n = collect(c->now, false, &v);
    int show = MIN(n, 4);
    ui_card(b, "Board", "/board", n > show ? "All notes" : "Open");
    if (!n) buf_puts(b, "<p class=\"muted\">Nothing on the board. <a href=\"/board\">Leave a note</a></p>");
    else minis(c, b, v, show, true);
    ui_card_end(b);
    free(v);
}

/* The wall's "Notes": a calm wall leaves the card out when there are none. */
static bool row_notes(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)kind, (void)arg;
    Note **v;
    int n = collect(c->now, false, &v);
    if (n) minis(c, b, v, MIN(n, 5), false);
    free(v);
    return n > 0;
}

static const Block blocks[] = {
    {"board.notes", "Notes", SCREEN_WALL, row_notes, NULL},
    {0},
};

/* ----------------------------------------------------------------- actions */

static bool known(Ctx *c) {
    if (c->me) return true;
    ctx_fail(c, "Pick who you are first");
    return false;
}

static void act_post(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note n = {0};
    if (!read_text(c, f, n.text, sizeof n.text) || !read_expiry(c, f, 0, &n.expires)) return;
    n.hue = (int)CLAMP(fields_int(f, "hue", 45), 0, 359);
    n.pinned = checked(f, "pin");
    n.by = c->me->id;
    n.created = n.updated = c->now;
    tbl_put(&T_note, &n);
    char snip[160];
    snippet(n.text, snip, sizeof snip, 40);
    activity_add(c->me->id, n.pinned ? "📌" : "📝", "/board", "%s %s a note: “%s”", c->me->name,
                 n.pinned ? "pinned" : "left", snip);
    app_notify(c, TOAST_INFO, "%s left a note on the board", c->me->name);
    ctx_ok(c, n.pinned ? "Pinned to the board" : "Posted");
}

static void act_save(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note *cur = note_arg(c, f);
    if (!cur) return;
    Note n = *cur;
    if (!read_text(c, f, n.text, sizeof n.text) || !read_expiry(c, f, cur->expires, &n.expires)) return;
    n.hue = (int)CLAMP(fields_int(f, "hue", n.hue), 0, 359);
    n.pinned = checked(f, "pin");
    n.edited_by = c->me->id;
    n.updated = c->now;
    tbl_put(&T_note, &n);
    char snip[160];
    snippet(n.text, snip, sizeof snip, 40);
    Person *author = person_get(n.by);
    if (n.by == c->me->id) activity_add(c->me->id, "✏️", "/board", "%s edited their note: “%s”", c->me->name, snip);
    else
        activity_add(c->me->id, "✏️", "/board", "%s edited %s's note: “%s”", c->me->name,
                     author ? author->name : "someone", snip);
    ctx_ok(c, "Saved");
    back_to_board(c);
}

static void act_pin(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note *cur = note_arg(c, f);
    if (!cur) return;
    Note n = *cur;
    n.pinned = !n.pinned;
    tbl_put(&T_note, &n);
    char snip[160];
    snippet(n.text, snip, sizeof snip, 30);
    if (n.pinned) activity_add(c->me->id, "📌", "/board", "%s pinned “%s”", c->me->name, snip);
    ctx_ok(c, n.pinned ? "Pinned" : "Unpinned");
}

static void act_keep(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note *cur = note_arg(c, f);
    if (!cur) return;
    Note n = *cur;
    n.expires = 0;
    n.updated = c->now;
    tbl_put(&T_note, &n);
    ctx_ok(c, "Back on the board");
}

static void act_delete(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note *cur = note_arg(c, f);
    if (!cur) return;
    char snip[160];
    snippet(cur->text, snip, sizeof snip, 40);
    int author_id = cur->by;
    ctx_undo(c, &T_note, cur->id); /* no "Remove this note?" any more: Undo instead (issue #60) */
    tbl_del(&T_note, cur->id);
    Person *author = person_get(author_id);
    if (author_id == c->me->id) activity_add(c->me->id, "🗑️", "/board", "%s took down their note: “%s”", c->me->name, snip);
    else
        activity_add(c->me->id, "🗑️", "/board", "%s took down %s's note: “%s”", c->me->name,
                     author ? author->name : "someone", snip);
    ctx_ok(c, "Removed");
    back_to_board(c);
}

/* ------------------------------------------------------------------ module */

static void init(void) {
    T_note.what = "notes on the board";
    store_register(&T_note);
}

static void start(void) {
    int64_t cutoff = now_sec() - (int64_t)KEEP_EXPIRED_DAYS * DAY;
    for (int i = tbl_count(&T_note) - 1; i >= 0; i--) {
        Note *n = tbl_at(&T_note, i);
        if (n->expires > 0 && n->expires < cutoff) tbl_del(&T_note, n->id);
    }
}

static const Action actions[] = {
    {"board.post", act_post}, {"board.save", act_save},     {"board.pin", act_pin},
    {"board.keep", act_keep}, {"board.delete", act_delete}, {0},
};

/* Issue #39: a note kept "until" a day disappears at the local midnight after it; the others at a moment. */
static int rezone(Rezone *z) {
    int n = 0;
    for (int i = 0; z->days && i < tbl_count(&T_note); i++) {
        Note note = *(Note *)tbl_at(&T_note, i);
        if (!rezone_day(z, &note.expires)) continue;
        n++;
        if (z->apply) tbl_put(&T_note, &note);
    }
    return n;
}

static const Module board = {
    .id = "board",
    .title = "Board",
    .icon = "note",
    .nav = 5,
    .init = init,
    .start = start,
    .rezone = rezone,
    .page = page,
    .actions = actions,
    .widget = widget,
    .widget_order = 40,
    .blocks = blocks,
};
MODULE(board)
