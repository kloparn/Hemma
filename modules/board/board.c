/* modules/board — the fridge door: short notes for the whole household.
 *
 * Notes can be pinned, coloured and set to disappear after a while ("the
 * plumber comes Tuesday"). Expired notes are hidden but kept for 60 days in
 * case someone wants them back, then cleaned up on start. Anyone in the
 * household may edit or remove any note; the activity feed records who did.
 * A note may hold a photo (issue #47: the school letter, the parking permit): the browser shrinks it
 * (wire.js [data-shrink]) and it travels in the form as a data: URL of a JPEG; it's kept as
 * <data>/board/<random>.jpg, served at /board/photo/<name>, and swept once no note has it. */
#include "../../core/app.h"
#include "../../core/codec.h"
#include "../../core/ui.h"

#include <ctype.h>

#define NOTE_MAX_CHARS 600
#define DAY 86400
#define KEEP_EXPIRED_DAYS 60
#define PHOTO_MAX (700 * 1024) /* the browser keeps a photo under 600 KB (it rides one socket message) */
#define PHOTO_SIDE 4096         /* (the browser makes it 1600 px at most; a hand-made request no more than this) */
#define PHOTO_SHRINK "1600"     /* what the browser shrinks a photo to, its longest side */

typedef struct {
    int id;
    char text[NOTE_MAX_CHARS * 4 + 4]; /* 600 characters of any width */
    int hue;
    bool pinned;
    int by;        /* author */
    int edited_by; /* last editor, 0 = never edited */
    int64_t created, updated;
    int64_t expires; /* 0 = keep; hidden from this moment on */
    char photo[24];  /* "<16 hex>.jpg" in <data>/board, "" = none */
    int photo_w, photo_h;
} Note;

static const FieldDef note_fields[] = {
    FIELD(Note, id, F_INT),        FIELD(Note, text, F_STR),      FIELD(Note, hue, F_INT),
    FIELD(Note, pinned, F_BOOL),   FIELD(Note, by, F_INT),        FIELD(Note, edited_by, F_INT),
    FIELD(Note, created, F_I64),   FIELD(Note, updated, F_I64),   FIELD(Note, expires, F_I64),
    FIELD(Note, photo, F_STR),     FIELD(Note, photo_w, F_INT),   FIELD(Note, photo_h, F_INT),
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

/* First `max` characters on one line, with an ellipsis when cut. (No text: a photo's note - "the photo", in the
   language in use: for what's shown now, never for what's kept - the Latest feed has formats of its own.) */
static void snippet(const char *text, char *out, size_t cap, int max) {
    if (!*text) {
        str_copy(out, cap, T("the photo"));
        return;
    }
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

static bool read_text(Ctx *c, Fields *f, char *dst, size_t cap, bool photo) {
    const char *raw = fields_str(f, "text");
    char *t = xmalloc(strlen(raw) + 1);
    clean_text(raw, t);
    size_t chars = utf8_count(t);
    bool ok = false;
    if (!t[0] && !photo) { /* (a photo can say it all) */
        ctx_fail_at(c, "text", "%s", T("Write something first, or add a photo"));
    } else if (chars > NOTE_MAX_CHARS) {
        ctx_fail_at(c, "text", T("Keep it under %d characters (that one has %zu)"), NOTE_MAX_CHARS, chars);
    } else {
        str_copy(dst, cap, t);
        ok = true;
    }
    free(t);
    return ok;
}

/* ----------------------------------------------------------------- photos */

static void photo_path(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/board/%s", app_config()->data_dir, name);
}

static bool photo_name_ok(const char *s) { /* <16 hex>.jpg: what read_photo names them, nothing else */
    for (int i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return false;
    return str_eq(s + 16, ".jpg");
}

/* A JPEG's size, from its first frame header (SOFn) */
static bool jpeg_size(const uint8_t *p, size_t n, int *w, int *h) {
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8 || p[2] != 0xFF) return false;
    size_t i = 2;
    while (i + 9 <= n) {
        if (p[i] != 0xFF) return false;
        uint8_t m = p[i + 1];
        if (m == 0xFF) { /* fill */
            i++;
            continue;
        }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { /* markers without a length */
            i += 2;
            continue;
        }
        size_t len = (size_t)p[i + 2] << 8 | p[i + 3];
        if (len < 2) return false;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = p[i + 5] << 8 | p[i + 6];
            *w = p[i + 7] << 8 | p[i + 8];
            return *w > 0 && *h > 0;
        }
        i += 2 + len;
    }
    return false;
}

/* The photo in the form ("photo": a data: URL of a JPEG the browser made), saved under a new name; and
   "photo_off" takes the note's photo away. false (said beside the field) for something that isn't one. */
static bool read_photo(Ctx *c, Fields *f, Note *n) {
    if (checked(f, "photo_off")) {
        n->photo[0] = '\0';
        n->photo_w = n->photo_h = 0;
    }
    const char *v = fields_str(f, "photo");
    if (!v[0]) return true;
    static const char pre[] = "data:image/jpeg;base64,";
    if (!str_starts(v, pre)) {
        ctx_fail_at(c, "photo", "%s", T("That isn't a photo Hemma can keep - try another"));
        return false;
    }
    const char *b64 = v + sizeof pre - 1;
    uint8_t *jpg = xmalloc(PHOTO_MAX);
    long len = base64_decode(b64, strlen(b64), jpg, PHOTO_MAX);
    int w = 0, h = 0;
    if (len <= 0 || !jpeg_size(jpg, (size_t)len, &w, &h) || w > PHOTO_SIDE || h > PHOTO_SIDE) {
        free(jpg);
        ctx_fail_at(c, "photo", "%s", T("That photo couldn't be read - try another"));
        return false;
    }
    char name[24], dir[600], path[640];
    rand_hex(name, 8);
    strcat(name, ".jpg");
    snprintf(dir, sizeof dir, "%s/board", app_config()->data_dir);
    photo_path(name, path, sizeof path);
    bool saved = dir_make(dir) && file_write_atomic(path, jpg, (size_t)len);
    free(jpg);
    if (!saved) {
        ctx_fail_at(c, "photo", "%s", T("The photo couldn't be saved - is the disk full?"));
        return false;
    }
    str_copy(n->photo, sizeof n->photo, name);
    n->photo_w = w;
    n->photo_h = h;
    return true;
}

/* Photos no note has any more (deleted, replaced, cleaned up) go: at the start, and on the hour once
   they're an hour old - a deleted note's Undo (UNDO_S) brings its photo back meanwhile. */
static void photo_sweep(int64_t older_than) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/board", app_config()->data_dir);
    DirIter *it = dir_open(dir);
    if (!it) return;
    DirEntry e;
    while (dir_next(it, &e)) {
        if (e.is_dir || !photo_name_ok(e.name) || e.mtime >= older_than) continue;
        bool kept = false;
        for (int i = 0; i < tbl_count(&T_note) && !kept; i++) kept = str_eq(((Note *)tbl_at(&T_note, i))->photo, e.name);
        if (kept) continue;
        char path[640];
        photo_path(e.name, path, sizeof path);
        file_remove(path);
    }
    dir_close(it);
}

static void photo_img(Buf *b, const Note *n, bool link) {
    if (!n->photo[0]) return;
    if (link) H(b, "<a class=\"board-photo\" href=\"/board/photo/%s\" data-native target=\"_blank\" rel=\"noopener\">", n->photo);
    else buf_puts(b, "<div class=\"board-photo\">");
    H(b, "<img src=\"/board/photo/%s\" width=\"%d\" height=\"%d\" alt=\"%s\" loading=\"lazy\">", n->photo, n->photo_w,
      n->photo_h, link ? T("Photo - open it full size") : T("Photo"));
    buf_puts(b, link ? "</a>" : "</div>");
}

/* the form's photo field: wire.js shrinks what's picked; it only shows while the socket is up (a plain
   POST can't carry it) */
static void photo_field(Buf *b, const char *label) {
    H(b, "<label class=\"board-photo-f\"><span>%s</span><input type=\"file\" name=\"photo\" accept=\"image/*\" "
         "data-shrink=\"" PHOTO_SHRINK "\"></label>",
      label);
}

/* until=YYYY-MM-DD wins when filled in (visible through that day); otherwise
 * keep = "0" | "1d" | "7d" | "keep" (leave as is). */
static bool read_expiry(Ctx *c, Fields *f, int64_t current, int64_t *out) {
    const char *keep = fields_str(f, "keep"), *until = fields_str(f, "until");
    if (until[0] || str_eq(keep, "date")) {
        int y, m, d;
        if (!parse_ymd(until, &y, &m, &d)) {
            ctx_fail_at(c, "until", "%s", T("Pick a date for the note to disappear"));
            return false;
        }
        *out = dt_make(y, m, d + 1, 0, 0);
        if (*out <= c->now) {
            ctx_fail_at(c, "until", "%s", T("That date has already passed"));
            return false;
        }
    } else if (str_eq(keep, "keep")) *out = current;
    else if (str_eq(keep, "1d")) *out = c->now + DAY;
    else if (str_eq(keep, "7d")) *out = c->now + 7 * DAY;
    else *out = 0;
    return true;
}

/* A child or a guest changes only their own notes (issue #44); everyone else anyone's. */
static bool may_change(const Ctx *c, const Note *n) { return !(is_child(c) || is_guest(c)) || n->by == c->me->id; }

static Note *note_arg(Ctx *c, Fields *f) {
    Note *n = tbl_get(&T_note, (int)fields_int(f, "id", 0));
    if (!n) ctx_fail(c, "%s", T("That note is gone"));
    else if (!may_change(c, n)) {
        ctx_fail(c, "%s", T("That note isn't yours"));
        return NULL;
    }
    return n;
}

static void back_to_board(Ctx *c) { ctx_redirect(c, "%s", str_eq(ctx_q(c, "expired"), "1") ? "/board?expired=1" : "/board"); }

/* ------------------------------------------------------------------ pieces */

/* "until tomorrow 09:00" / "hidden since Tuesday" (ui_day_inline: "today" mid-sentence) */
static void expiry_label(Buf *b, const Note *n, int64_t now) {
    DateTime e = dt_local(n->expires);
    bool midnight = e.hour == 0 && e.min == 0;
    Buf when = {0};
    if (midnight && !is_expired(n, now)) {
        ui_day_inline(&when, n->expires - 1, now);
    } else {
        ui_day_inline(&when, n->expires, now);
        buf_putc(&when, ' ');
        ui_hm(&when, n->expires);
    }
    buf_printf(b, is_expired(n, now) ? T("hidden since %s") : T("until %s"), buf_cstr(&when));
    buf_free(&when);
}

static void note_actions(Buf *b, const Note *n, bool expired_view) {
    char id[16];
    snprintf(id, sizeof id, "%d", n->id);
    buf_puts(b, "<div class=\"board-note-x\">");
    /* each button names its note: a screen reader's list of buttons said "Pin to the top" over and over (#69) */
    char snip[80], what[240];
    snippet(n->text, snip, sizeof snip, 30);
    Buf icon = {0};
    ui_icon(&icon, expired_view ? "check" : "pin");
    if (expired_view) snprintf(what, sizeof what, T("Keep “%s”"), snip);
    else snprintf(what, sizeof what, n->pinned ? T("Unpin “%s”") : T("Pin “%s” to the top"), snip);
    ui_act(b, expired_view ? "board.keep" : "board.pin", "icon-btn", buf_cstr(&icon), "id", id, what);
    buf_free(&icon);
    H(b, "<a class=\"icon-btn\" href=\"/board?%redit=%d\" title=\"%s\" aria-label=\"", expired_view ? "expired=1&amp;" : "",
      n->id, T("Edit"));
    H(b, T("Edit “%s”"), snip);
    buf_puts(b, "\">");
    ui_icon(b, "edit");
    buf_puts(b, "</a>");
    ui_form(b, "board.delete", "inline");
    ui_hidden_int(b, "id", n->id);
    H(b, "<button class=\"icon-btn\" title=\"%s\" aria-label=\"", T("Remove"));
    H(b, T("Remove “%s”"), snip);
    buf_puts(b, "\">");
    ui_icon(b, "trash");
    buf_puts(b, "</button>");
    ui_form_end(b);
    buf_puts(b, "</div>");
}

static void note_card(Ctx *c, Buf *b, const Note *n, bool expired_view) {
    H(b, "<article class=\"board-note%r\" id=\"board-note-%d\"", n->pinned ? " pinned" : "", n->id);
    ui_hue_style(b, n->hue);
    buf_putc(b, '>');
    if (n->pinned) H(b, "<span class=\"board-note-pin\" title=\"%s\"></span>", T("Pinned"));
    photo_img(b, n, true);
    if (n->text[0]) {
        buf_puts(b, "<div class=\"board-note-text\">");
        render_text(b, n->text, true);
        buf_puts(b, "</div>");
    }
    if (n->expires) {
        buf_puts(b, "<div class=\"board-note-exp\">");
        ui_icon(b, "clock");
        expiry_label(b, n, c->now);
        buf_puts(b, "</div>");
    }
    buf_puts(b, "<footer class=\"board-note-f\">");
    Person *p = person_get(n->by);
    ui_avatar_deco(b, p, "xs");
    H(b, "<span class=\"board-note-m\"><b>%s</b> · ", p ? p->name : T("Someone"));
    ui_ago(b, n->created, c->now);
    if (n->edited_by && n->updated > n->created + 60) {
        Person *e = person_get(n->edited_by);
        buf_puts(b, " · <span title=\"");
        if (e) H(b, T("Edited by %s"), e->name);
        else H(b, "%s", T("Edited by someone"));
        H(b, "\">%s</span>", T("edited"));
    }
    buf_puts(b, "</span>");
    if (c->me && c->me->role != ROLE_DISPLAY && may_change(c, n)) note_actions(b, n, expired_view); /* (#54, #44) */
    buf_puts(b, "</footer></article>");
}

static void keep_radio(Buf *b, const char *value, const char *label, bool on) {
    H(b, "<label class=\"board-keep-o\"><input type=\"radio\" name=\"keep\" value=\"%s\"%r><span>%s</span></label>", value,
      on ? " checked" : "", label);
}

static void post_box(Buf *b) {
    buf_puts(b, "<section class=\"card board-post\">");
    ui_form(b, "board.post", "board-post-f");
    H(b, "<textarea name=\"text\" rows=\"3\" maxlength=\"%d\" aria-label=\"%s\" placeholder=\"%s\"></textarea>"
         "<div class=\"board-post-o\">",
      NOTE_MAX_CHARS, T("Note"),
      T("Leave a note for everyone… reminders, the Wi-Fi password for guests, who picks up whom"));
    ui_hue_picker(b, "hue", 45);
    buf_puts(b, "<label class=\"board-pin-toggle\"><input type=\"checkbox\" name=\"pin\" value=\"1\"><span>");
    ui_icon(b, "pin");
    H(b, "%s</span></label><div class=\"board-keep\" role=\"radiogroup\" aria-label=\"%s\">", T("Pin"), T("Keep it for"));
    keep_radio(b, "0", T("Keep"), true);
    keep_radio(b, "1d", T("1 day"), false);
    keep_radio(b, "7d", T("1 week"), false);
    buf_puts(b, "</div>");
    photo_field(b, T("📷 Photo"));
    H(b, "<button class=\"btn primary board-post-go\">%s</button></div>", T("Post"));
    ui_form_end(b);
    buf_puts(b, "</section>");
}

static void edit_modal(Ctx *c, Buf *b, const Note *n, bool expired_view) {
    const char *close = expired_view ? "/board?expired=1" : "/board";
    ui_modal(b, T("Edit note"), close);
    ui_form(b, "board.save", "stack");
    ui_hidden_int(b, "id", n->id);
    H(b, "<textarea name=\"text\" rows=\"6\" maxlength=\"%d\" autofocus aria-label=\"%s\">%s</textarea>",
      NOTE_MAX_CHARS, T("Note"), n->text);
    if (n->photo[0]) {
        buf_puts(b, "<div class=\"board-photo-edit\">");
        photo_img(b, n, false);
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"photo_off\" value=\"1\"> %s</label></div>",
          T("Take the photo away"));
    }
    photo_field(b, n->photo[0] ? T("Another photo") : T("📷 Add a photo"));
    H(b, "<div class=\"field\"><span>%s</span>", T("Colour"));
    ui_hue_picker(b, "hue", n->hue);
    H(b, "</div><label class=\"board-pin-toggle\"><input type=\"checkbox\" name=\"pin\" value=\"1\"%r><span>",
      n->pinned ? " checked" : "");
    ui_icon(b, "pin");
    H(b, "%s</span></label><div class=\"field\"><span>%s</span><div class=\"board-keep wrap\">", T("Pinned to the top"),
      T("Show it"));
    bool has = n->expires > 0 && !is_expired(n, c->now);
    if (has) {
        Buf now = {0}, lbl = {0};
        expiry_label(&now, n, c->now);
        buf_printf(&lbl, T("As now (%s)"), buf_cstr(&now));
        keep_radio(b, "keep", buf_cstr(&lbl), true);
        buf_free(&now);
        buf_free(&lbl);
    }
    keep_radio(b, "0", T("Always"), !has);
    keep_radio(b, "1d", T("1 more day"), false);
    keep_radio(b, "7d", T("1 more week"), false);
    char today[11];
    fmt_ymd(c->now, today);
    H(b, "</div><label class=\"board-until\"><span>%s</span><input type=\"date\" name=\"until\" min=\"%s\" "
         "aria-label=\"%s\"></label></div>",
      T("or until"), today, T("Show until date"));
    H(b, "<div class=\"modal-f\"><button class=\"btn primary\">%s</button></div>", T("Save"));
    ui_form_end(b);
    ui_form(b, "board.delete", "danger-zone");
    ui_hidden_int(b, "id", n->id);
    H(b, "<button class=\"btn danger\">%s</button>", T("Remove note"));
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
    ctx_title(c, "%s", T("Board"));
    bool expired_view = str_eq(ctx_q(c, "expired"), "1");
    Note **live, **old;
    int nlive = collect(c->now, false, &live);
    int nold = collect(c->now, true, &old);
    int npinned = 0;
    while (npinned < nlive && live[npinned]->pinned) npinned++;

    if (expired_view) {
        ctx_title(c, "%s", T("Hidden notes"));
        ui_head(b, T("Hidden notes"), T("They passed their date. Kept for 60 days, then cleaned up."));
        H(b, "<a class=\"btn\" href=\"/board\">");
        ui_icon(b, "left");
        H(b, "<span>%s</span></a>", T("Back to the board"));
        ui_head_end(b);
        if (nold) grid(c, b, old, nold, true);
        else ui_empty(b, "🧹", T("Nothing hidden"), T("Notes that expire end up here for a while."));
    } else {
        char sub[200];
        if (nlive) snprintf(sub, sizeof sub, TN(nlive, "%d note", "%d notes"), nlive);
        else snprintf(sub, sizeof sub, "%s", T("The household's fridge door"));
        if (nlive && npinned) {
            size_t at = strlen(sub);
            snprintf(sub + at, sizeof sub - at, " · ");
            at = strlen(sub);
            snprintf(sub + at, sizeof sub - at, T("%d pinned"), npinned);
        }
        ui_head(b, T("Board"), sub);
        ui_head_end(b);
        post_box(b);
        if (!nlive) {
            ui_empty(b, "📝", T("The board is empty"), T("Leave a note for the household — it shows up on every screen."));
        } else {
            if (npinned) {
                buf_puts(b, "<h2 class=\"board-h\">");
                ui_icon(b, "pin");
                H(b, "%s</h2>", T("Pinned"));
                grid(c, b, live, npinned, false);
            }
            if (nlive > npinned) {
                if (npinned) H(b, "<h2 class=\"board-h\">%s</h2>", T("Notes"));
                grid(c, b, live + npinned, nlive - npinned, false);
            }
        }
        if (nold) {
            buf_puts(b, "<p class=\"board-hidden\">");
            H(b,
              TN(nold, "<a href=\"/board?expired=1\">%d hidden note</a> — past their date",
                 "<a href=\"/board?expired=1\">%d hidden notes</a> — past their date"),
              nold);
            buf_puts(b, "</p>");
        }
    }

    Note *edit = tbl_get(&T_note, (int)ctx_qint(c, "edit", 0));
    if (edit && c->me && may_change(c, edit)) edit_modal(c, b, edit, expired_view);
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
        buf_puts(b, ">");
        photo_img(b, x, false);
        buf_puts(b, "<p>");
        render_text(b, x->text, links);
        buf_puts(b, "</p><small>");
        if (x->pinned) ui_icon(b, "pin");
        Person *p = person_get(x->by);
        H(b, "%s · ", p ? p->name : T("Someone"));
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
    ui_card(b, T("Board"), "/board", n > show ? T("All notes") : T("Open"));
    if (!n) H(b, "<p class=\"muted\">%r</p>", T("Nothing on the board. <a href=\"/board\">Leave a note</a>"));
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
    {"board.notes", N_("Notes"), SCREEN_WALL, row_notes, NULL},
    {0},
};

/* ----------------------------------------------------------------- actions */

static bool known(Ctx *c) {
    if (c->me) return true;
    ctx_fail(c, "%s", T("Pick who you are first"));
    return false;
}

static void act_post(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note n = {0};
    bool photo = fields_str(f, "photo")[0] != '\0';
    if (!read_text(c, f, n.text, sizeof n.text, photo) || !read_expiry(c, f, 0, &n.expires) || !read_photo(c, f, &n))
        return;
    n.hue = (int)CLAMP(fields_int(f, "hue", 45), 0, 359);
    n.pinned = checked(f, "pin");
    n.by = c->me->id;
    n.created = n.updated = c->now;
    tbl_put(&T_note, &n);
    char snip[160];
    snippet(n.text, snip, sizeof snip, 40);
    int me = c->me->id;
    const char *name = c->me->name, *icon = n.pinned ? "📌" : "📝";
    /* (the Latest feed: one whole format each, said in every reader's language - issue #46) */
    if (!n.text[0] && n.pinned) activity_add(me, "📷", "/board", N_("%s pinned a photo on the board"), name);
    else if (!n.text[0]) activity_add(me, "📷", "/board", N_("%s put a photo on the board"), name);
    else if (n.photo[0] && n.pinned) activity_add(me, icon, "/board", N_("%s pinned a note with a photo: “%s”"), name, snip);
    else if (n.photo[0]) activity_add(me, icon, "/board", N_("%s left a note with a photo: “%s”"), name, snip);
    else if (n.pinned) activity_add(me, icon, "/board", N_("%s pinned a note: “%s”"), name, snip);
    else activity_add(me, icon, "/board", N_("%s left a note: “%s”"), name, snip);
    app_notify(c, TOAST_INFO, N_("%s left a note on the board"), name);
    ctx_ok(c, "%s", n.pinned ? T("Pinned to the board") : T("Posted"));
}

static void act_save(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note *cur = note_arg(c, f);
    if (!cur) return;
    Note n = *cur;
    bool photo = fields_str(f, "photo")[0] || (cur->photo[0] && !checked(f, "photo_off"));
    if (!read_text(c, f, n.text, sizeof n.text, photo) || !read_expiry(c, f, cur->expires, &n.expires) ||
        !read_photo(c, f, &n))
        return;
    n.hue = (int)CLAMP(fields_int(f, "hue", n.hue), 0, 359);
    n.pinned = checked(f, "pin");
    n.edited_by = c->me->id;
    n.updated = c->now;
    tbl_put(&T_note, &n);
    char snip[160];
    snippet(n.text, snip, sizeof snip, 40);
    Person *author = person_get(n.by);
    int me = c->me->id;
    const char *name = c->me->name;
    if (!n.text[0]) { /* a photo's note */
        if (n.by == me) activity_add(me, "✏️", "/board", N_("%s edited their note: “the photo”"), name);
        else if (author) activity_add(me, "✏️", "/board", N_("%s edited %s's note: “the photo”"), name, author->name);
        else activity_add(me, "✏️", "/board", N_("%s edited someone's note: “the photo”"), name);
    } else {
        if (n.by == me) activity_add(me, "✏️", "/board", N_("%s edited their note: “%s”"), name, snip);
        else if (author) activity_add(me, "✏️", "/board", N_("%s edited %s's note: “%s”"), name, author->name, snip);
        else activity_add(me, "✏️", "/board", N_("%s edited someone's note: “%s”"), name, snip);
    }
    ctx_ok(c, "%s", T("Saved"));
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
    if (n.pinned && !n.text[0]) activity_add(c->me->id, "📌", "/board", N_("%s pinned “the photo”"), c->me->name);
    else if (n.pinned) activity_add(c->me->id, "📌", "/board", N_("%s pinned “%s”"), c->me->name, snip);
    ctx_ok(c, "%s", n.pinned ? T("Pinned") : T("Unpinned"));
}

static void act_keep(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note *cur = note_arg(c, f);
    if (!cur) return;
    Note n = *cur;
    n.expires = 0;
    n.updated = c->now;
    tbl_put(&T_note, &n);
    ctx_ok(c, "%s", T("Back on the board"));
}

static void act_delete(Ctx *c, Fields *f) {
    if (!known(c)) return;
    Note *cur = note_arg(c, f);
    if (!cur) return;
    char snip[160];
    snippet(cur->text, snip, sizeof snip, 40);
    bool photo_only = !cur->text[0];
    int author_id = cur->by;
    ctx_undo(c, &T_note, cur->id); /* no "Remove this note?" any more: Undo instead (issue #60) */
    tbl_del(&T_note, cur->id);
    Person *author = person_get(author_id);
    int me = c->me->id;
    const char *name = c->me->name;
    if (photo_only) {
        if (author_id == me) activity_add(me, "🗑️", "/board", N_("%s took down their note: “the photo”"), name);
        else if (author) activity_add(me, "🗑️", "/board", N_("%s took down %s's note: “the photo”"), name, author->name);
        else activity_add(me, "🗑️", "/board", N_("%s took down someone's note: “the photo”"), name);
    } else {
        if (author_id == me) activity_add(me, "🗑️", "/board", N_("%s took down their note: “%s”"), name, snip);
        else if (author) activity_add(me, "🗑️", "/board", N_("%s took down %s's note: “%s”"), name, author->name, snip);
        else activity_add(me, "🗑️", "/board", N_("%s took down someone's note: “%s”"), name, snip);
    }
    ctx_ok(c, "%s", T("Removed"));
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
    photo_sweep(INT64_MAX); /* (no Undo survives a start) */
}

static int64_t g_swept_ms;
static void tick(int64_t now_ms) {
    if (g_swept_ms && now_ms - g_swept_ms < 3600 * 1000) return;
    g_swept_ms = now_ms;
    photo_sweep(now_sec() - 3600);
}

/* GET /board/photo/<name>: for people signed in (and the wall); a name is never reused, so it keeps */
static bool http(Ctx *c, Conn *conn, HttpReq *r) {
    (void)c;
    if (!str_starts(r->path, "/board/photo/")) return false;
    const char *name = r->path + strlen("/board/photo/");
    Buf jpg = {0};
    char path[640];
    if (photo_name_ok(name)) photo_path(name, path, sizeof path);
    if (!photo_name_ok(name) || !file_read_all(path, &jpg, PHOTO_MAX)) {
        const char *msg = T("That photo isn't here any more.\n"); /* (opened by itself: the request's language) */
        conn_reply(conn, 404, "text/plain; charset=utf-8", msg, strlen(msg), NULL);
    } else {
        conn_reply(conn, 200, "image/jpeg", jpg.p, jpg.len, "Cache-Control: private, max-age=31536000, immutable\r\n");
    }
    buf_free(&jpg);
    return true;
}

/* Children and guests use the board too (issue #44), changing only their own notes (note_arg). */
static const Action actions[] = {
    {"board.post", act_post, .may = ACT_ALL},     {"board.save", act_save, .may = ACT_ALL},
    {"board.pin", act_pin, .may = ACT_ALL},       {"board.keep", act_keep, .may = ACT_ALL},
    {"board.delete", act_delete, .may = ACT_ALL}, {0},
};

/* Someone leaves (issue #45): moved out, their notes stay up until they'd go anyway; erased, they go now
   (their photos at the next hourly photo_sweep) and nobody shows as having edited one. */
static int forget(Forget *f) {
    int mine = 0, edited = 0;
    for (int i = tbl_count(&T_note) - 1; i >= 0 && !f->keep; i--) {
        Note n = *(Note *)tbl_at(&T_note, i);
        if (n.by == f->person) {
            mine++;
            if (f->apply) tbl_del(&T_note, n.id);
        } else if (n.edited_by == f->person) {
            edited++;
            n.edited_by = 0;
            if (f->apply) tbl_put(&T_note, &n);
        }
    }
    if (mine) /* (said while the remove dialog renders: in its language) */
        forget_note(f, TN(mine, "%d note of theirs on the board is taken down", "%d notes of theirs on the board are taken down"),
                    mine);
    return mine + edited;
}

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
    .title = N_("Board"),
    .icon = "note",
    .nav = 5,
    .init = init,
    .start = start,
    .tick = tick,
    .rezone = rezone,
    .forget = forget,
    .page = page,
    .http = http,
    .actions = actions,
    .widget = widget,
    .widget_order = 40,
    .blocks = blocks,
};
MODULE(board)
