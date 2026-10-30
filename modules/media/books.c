/* modules/media/books.c — audiobooks (1.6; owner, 2026-10-28: "audio books library ... another tab or slot about
 * audio books, and also make it so that we can listen to it while the phone is locked").
 *
 *   /media/books        Watch's "Audiobooks" tab (there while the library has books): Continue listening, then
 *                       every book - a series' books together, in their order (booklist.c)
 *   /media/book/<id>    a book: its cover, who wrote and reads it, the listening player, its chapters, a download
 *                       for grown-ups; ?at=<s> starts there (a chapter's link without wire.js)
 *
 * A book's progress is a media_watch row like a video's - its id is its first part's file id (library.h
 * MediaBook), its place counted in the whole book - so Continue listening, Mark finished and its x
 * (media.watched, media.forget) are Watch's own actions. Done means its end (media_watch_report).
 *
 * THE PLAYER (wire.js "the listener"): an <audio data-book> - with the browser's own controls until wire.js
 * takes it, so it plays without it too - and Hemma's own controls in [data-listen]: the chapter that's on, a seek
 * bar over the whole book, back 15 s / forward 30 s, previous and next chapter, speed and a sleep timer. A book in
 * parts (a folder of MP3s) lists them in data-parts, [[address, start, length], ...] (seconds): the player
 * moves from one to the next in the same element. The lock screen (the Media Session API): the book's title,
 * author, cover and the chapter, play/pause, 15/30 s, the chapters as tracks. Both the element and the controls
 * are data-static: a re-render never touches them, so neither does a page's morph while it plays. */
#include "../../core/json.h"
#include "../../core/ui.h"
#include "download.h"
#include "media.h"
#include "room.h"

#define BOOK_CHAPTERS_MAX 3000

/* ------------------------------------------------------------------ pieces */

bool media_kids_book_ok(const Ctx *c, const MediaBook *bk) {
    return !bk || !bk->count || media_kids_ok(c, media_book_part(bk, 0)); /* (all of it, or none) */
}

/* The viewer's place in it (their progress row: its first part's) */
static const MediaWatch *book_watch(const Ctx *c, const MediaBook *bk) {
    int v = ctx_viewer(c);
    return v && bk ? media_watch_get(v, bk->id) : NULL;
}

/* Its picture's address: a picture file, else the one inside a part (/media/cover/<file>?v=); false = none */
static bool book_art_src(Buf *b, const MediaBook *bk) {
    if (bk->poster && media_file(bk->poster)) {
        media_img_src(b, bk->poster);
        return true;
    }
    const MediaFile *f = bk->cover ? media_file(bk->cover) : NULL;
    if (!f) return false;
    H(b, "/media/cover/%X?v=%X", f->id, (uint64_t)f->mtime * 1000003u + (uint64_t)f->size);
    return true;
}

/* A book's square: its picture, or one drawn in its title's colour; its progress along the foot */
static void book_art(Buf *b, const MediaBook *bk, int pct, bool done, bool pictures) {
    buf_puts(b, "<span class=\"media-art media-art-sq\"");
    media_art_style(b, bk->series[0] ? bk->series : bk->title);
    buf_putc(b, '>');
    Buf src = {0};
    if (pictures && book_art_src(&src, bk)) H(b, "<img src=\"%s\" alt=\"\" loading=\"lazy\" decoding=\"async\">", buf_cstr(&src));
    else {
        H(b, "<span class=\"media-art-k\">%s</span><span class=\"media-art-t\">%s</span>", T("Audiobook"), bk->title);
        if (bk->author[0]) H(b, "<span class=\"media-art-y\">%s</span>", bk->author);
    }
    buf_free(&src);
    if (done) {
        H(b, "<span class=\"media-art-done\" title=\"%s\">", T("Finished listening"));
        ui_icon(b, "check");
        buf_puts(b, "</span>");
    } else if (pct > 0) {
        H(b, "<span class=\"media-art-bar\"><i style=\"width:%d%%\"></i></span>", pct);
    }
    buf_puts(b, "</span>");
}

static int book_percent(const MediaWatch *w) {
    if (!w || w->dur <= 0 || w->pos <= 0) return 0;
    return CLAMP(w->pos * 100 / w->dur, 1, 100);
}

void media_book_card(Ctx *c, Buf *b, const MediaBook *bk) {
    const MediaWatch *w = book_watch(c, bk);
    H(b, "<a class=\"media-poster\" href=\"/media/book/%X\">", bk->id);
    book_art(b, bk, book_percent(w), w && w->done, c->me != NULL);
    H(b, "<span class=\"media-cap\">%s<small>", bk->title);
    char d[32];
    media_fmt_duration(bk->duration_ms, d, sizeof d);
    if (bk->author[0]) H(b, "%s%s", bk->author, d[0] ? " · " : "");
    H(b, "%s</small></span></a>", d);
}

/* A Continue listening card: the book, and an x that forgets the viewer's place in it (media.forget) */
static void going_card(Ctx *c, Buf *b, const MediaBook *bk) {
    H(b, "<div class=\"media-going\" id=\"media-going-%X\">", bk->id);
    media_book_card(c, b, bk);
    ui_form(b, "media.forget", "media-going-x");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><button title=\"%s\" aria-label=\"", bk->id,
      T("Remove from Continue listening"));
    H(b, T("Remove %s from Continue listening"), bk->title);
    buf_puts(b, "\">");
    ui_icon(b, "x");
    buf_puts(b, "</button>");
    ui_form_end(b);
    buf_puts(b, "</div>");
}

int media_books_going(const Ctx *c, const MediaBook **out, int max) {
    const MediaIndex *ix = media_lib_for(c);
    int v = ctx_viewer(c), n = 0;
    const MediaWatch *keep[24];
    max = MIN(max, countof(keep));
    for (int i = 0; v && ix->nbooks && i < tbl_count(&T_media_watch); i++) {
        const MediaWatch *w = tbl_at(&T_media_watch, i);
        uint64_t id = 0;
        if (w->person != v || w->done || w->pos < 10 || !media_parse_id(w->item, &id)) continue;
        bool in = false; /* (one of the books this viewer may see) */
        for (int k = 0; k < ix->nbooks && !in; k++) in = ix->books[k].id == id;
        if (!in || (n == max && keep[n - 1]->updated >= w->updated)) continue;
        int k = n < max ? n++ : n - 1;
        while (k > 0 && keep[k - 1]->updated < w->updated) keep[k] = keep[k - 1], k--;
        keep[k] = w;
    }
    for (int i = 0; i < n; i++) {
        uint64_t id = 0;
        media_parse_id(keep[i]->item, &id);
        out[i] = media_book(id);
    }
    return n;
}

void media_books_shelf(Ctx *c, Buf *b) {
    const MediaBook *going[12];
    int n = media_books_going(c, going, countof(going));
    if (!n) return;
    H(b, "<section class=\"media-row\"><header class=\"media-row-h\"><h2>%s</h2><a href=\"/media/books\">%s</a></header>"
         "<div class=\"media-shelf media-shelf-sq\">",
      T("Continue listening"), T("See all"));
    for (int i = 0; i < n; i++) going_card(c, b, going[i]);
    buf_puts(b, "</div></section>");
}

/* ----------------------------------------------------------------- chapters */

typedef struct {
    int64_t at_ms; /* in the whole book */
    const char *title;
    char own[120]; /* (a part's name, when it's the title) */
} BookChapter;

/* A part named only by its number ("01.mp3"): "Part 1" */
static void part_title(const MediaFile *f, int i, char *out, size_t cap) {
    const MediaTags *t = media_tags(f->id);
    if (t && t->title[0]) {
        str_copy(out, cap, t->title);
        return;
    }
    const char *b = strrchr(f->rel, '/');
    char name[256];
    str_copy(name, sizeof name, b ? b + 1 : f->rel);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = '\0';
    for (char *p = name; *p; p++)
        if (*p == '_') *p = ' ';
    const char *rest = name + strspn(name, "0123456789"); /* "01 - Chapter One" -> "Chapter One" */
    if (rest > name && *rest && strchr(" .-", *rest)) rest += strspn(rest, " .-");
    else rest = name;
    if (!*rest || strspn(name, "0123456789 .-") == strlen(name)) snprintf(out, cap, T("Part %d"), i + 1);
    else str_copy(out, cap, rest);
}

/* Its chapters, in the whole book's time: each part's own (an .m4b's), else each part is one */
static int book_chapters(const MediaBook *bk, BookChapter *out, int max) {
    int n = 0;
    int64_t at = 0;
    for (int i = 0; i < bk->count && n < max; i++) {
        const MediaFile *f = media_book_part(bk, i);
        const MediaTags *t = media_tags(f->id);
        if (t && t->nch > 0) {
            for (int k = 0; k < t->nch && n < max; k++)
                out[n++] = (BookChapter){at + MAX(t->ch[k].start_ms, 0), t->ch[k].title, ""};
        } else {
            BookChapter *c = &out[n++];
            c->at_ms = at;
            part_title(f, i, c->own, sizeof c->own);
            c->title = c->own;
        }
        at += MAX(f->probe.duration_ms, 0);
    }
    return n;
}

bool media_book_art_src(Buf *b, const MediaBook *bk) { return bk && book_art_src(b, bk); }

void media_book_chapters_json(const MediaBook *bk, Buf *b) {
    BookChapter *ch = xmalloc(BOOK_CHAPTERS_MAX * sizeof *ch);
    int n = bk ? book_chapters(bk, ch, BOOK_CHAPTERS_MAX) : 0;
    buf_putc(b, '[');
    for (int i = 0; i < n; i++) {
        buf_printf(b, "%s{\"t\":%lld,\"title\":", i ? "," : "", (long long)(ch[i].at_ms / 1000));
        json_str(b, ch[i].title);
        buf_putc(b, '}');
    }
    buf_putc(b, ']');
    free(ch);
}

static void book_clock(Buf *b, int64_t ms) { /* "1:03", "1:02:03" */
    int64_t s = ms / 1000;
    if (s >= 3600) buf_printf(b, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else buf_printf(b, "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
}

/* ------------------------------------------------------------------- pages */

static void empty_books(Ctx *c, Buf *b) {
    ui_empty(b, "🎧", T("No audiobooks yet"),
             T("An .m4b file anywhere in the library is an audiobook - and so is a folder of .mp3 files inside a folder "
               "called Audiobooks."));
    if (is_admin(c)) {
        buf_puts(b, "<p class=\"media-center\"><a class=\"btn primary\" href=\"/media/upload\">");
        ui_icon(b, "plus");
        H(b, "<span>%s</span></a></p>", T("Add audiobooks"));
    }
}

void media_books_page(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib_for(c);
    ctx_title(c, "%s", T("Audiobooks"));
    char sub[96] = "";
    if (ix->nbooks) {
        int64_t all = 0;
        for (int i = 0; i < ix->nbooks; i++) all += ix->books[i].duration_ms;
        char d[32];
        media_fmt_duration(all, d, sizeof d);
        snprintf(sub, sizeof sub, TN(ix->nbooks, "%d audiobook", "%d audiobooks"), ix->nbooks);
        if (d[0]) snprintf(sub + strlen(sub), sizeof sub - strlen(sub), " · %s", d);
    }
    media_lib_head(c, b, "books", T("Audiobooks"), sub, NULL);
    if (media_search_shown(c, b)) return;
    if (!ix->nbooks) {
        empty_books(c, b);
        return;
    }
    media_books_shelf(c, b);
    /* every book: each series' together under its name, then one "Books" for the rest (the list is in order of
       series-or-title: "Alice ...", "Oz", "The Velveteen Rabbit" - two "Books" once) */
    int alone = 0;
    for (int i = 0; i < ix->nbooks;) {
        const MediaBook *bk = &ix->books[i];
        if (!bk->series[0]) {
            alone++, i++;
            continue;
        }
        int k = i + 1;
        while (k < ix->nbooks && str_eq(ix->books[k].series, bk->series)) k++;
        H(b, "<section class=\"media-row\"><header class=\"media-row-h\"><h2>%s</h2></header>", bk->series);
        buf_puts(b, "<div class=\"media-grid media-grid-sq\">");
        for (int j = i; j < k; j++) media_book_card(c, b, &ix->books[j]);
        buf_puts(b, "</div></section>");
        i = k;
    }
    if (!alone) return;
    H(b, "<section class=\"media-row\"><header class=\"media-row-h\"><h2>%s</h2></header>", T("Books"));
    buf_puts(b, "<div class=\"media-grid media-grid-sq\">");
    for (int i = 0; i < ix->nbooks; i++)
        if (!ix->books[i].series[0]) media_book_card(c, b, &ix->books[i]);
    buf_puts(b, "</div></section>");
}

/* The listening player (wire.js "the listener"; the header comment above): <audio> + Hemma's controls */
static void player(Ctx *c, Buf *b, const MediaBook *bk, int64_t resume_ms, bool autoplay, const BookChapter *ch, int nch) {
    /* the part that holds the place to start from, and where in it */
    int part = 0;
    int64_t off = resume_ms;
    for (; part + 1 < bk->count; part++) {
        int64_t len = MAX(media_book_part(bk, part)->probe.duration_ms, 0);
        if (off < len) break;
        off -= len;
    }
    char src[160];
    media_file_src_at(c, media_book_part(bk, part)->id, off / 1000, src, sizeof src);
    H(b, "<div class=\"media-listen\" id=\"media-listen\"><audio id=\"media-audio\" src=\"%s\" controls preload=\"metadata\" "
         "data-static=\"%X\" data-book data-id=\"%X\" data-report=\"media.progress\" data-length=\"%L\"",
      src, bk->id, bk->id, (bk->duration_ms + 999) / 1000);
    if (resume_ms >= 1000) H(b, " data-resume=\"%L\"", resume_ms / 1000);
    if (autoplay) buf_puts(b, " autoplay");
    H(b, " data-title=\"%s\" data-author=\"%s\"", bk->title, bk->author);
    Buf art = {0};
    if (book_art_src(&art, bk)) { /* (with its address key: a phone's lock screen may fetch it without the cookie) */
        char path[80], k[CTX_KEY_LEN];
        str_ncopy(path, sizeof path, buf_cstr(&art), strcspn(buf_cstr(&art), "?"));
        ctx_url_key(c, path, k);
        H(b, " data-art=\"%s%s%s\"", buf_cstr(&art), k[0] ? "&k=" : "", k);
    }
    buf_free(&art);
    if (bk->count > 1) { /* [[address, start, length], ...] in seconds */
        Buf parts = {0};
        buf_putc(&parts, '[');
        int64_t at = 0;
        for (int i = 0; i < bk->count; i++) {
            const MediaFile *f = media_book_part(bk, i);
            char one[160];
            media_file_src(c, f->id, one, sizeof one);
            buf_printf(&parts, "%s[\"%s\",%.3f,%.3f]", i ? "," : "", one, (double)at / 1000.0, (double)MAX(f->probe.duration_ms, 0) / 1000.0);
            at += MAX(f->probe.duration_ms, 0);
        }
        buf_putc(&parts, ']');
        H(b, " data-parts=\"%s\"", buf_cstr(&parts));
        buf_free(&parts);
    }
    buf_puts(b, "></audio>");

    /* Hemma's controls: hidden until wire.js takes the element (the browser's own controls until then) */
    H(b, "<div class=\"media-ab\" data-listen=\"media-audio\" data-static=\"%X\" data-paused hidden>", bk->id);
    const char *on = bk->title; /* (the chapter it starts in) */
    for (int i = 0; nch > 1 && i < nch && ch[i].at_ms <= resume_ms; i++) on = ch[i].title; /* (one: the book itself) */
    H(b, "<p class=\"media-ab-ch\" data-b=\"chapter\">%s</p>", on);
    H(b, "<p class=\"media-ab-err\" role=\"alert\">%s</p>", T("This book won't play in this browser."));
    H(b, "<div class=\"media-ab-seek\"><span class=\"media-ab-fill\"></span><input type=\"range\" data-b=\"seek\" min=\"0\" "
         "max=\"%L\" step=\"any\" value=\"%L\" aria-label=\"%s\"></div>",
      MAX((bk->duration_ms + 999) / 1000, 1), resume_ms / 1000, T("Where in the book"));
    buf_puts(b, "<div class=\"media-ab-times\"><span data-b=\"at\">");
    book_clock(b, resume_ms);
    buf_puts(b, "</span><span data-b=\"left\">-");
    book_clock(b, MAX(bk->duration_ms - resume_ms, 0));
    buf_puts(b, "</span></div><div class=\"media-ab-row\">");
    static const struct {
        const char *part, *icon, *label;
    } btn[] = {{"prev", "prev", N_("Previous chapter")}, {"back", "back15", N_("Back 15 seconds")}, {"play", NULL, NULL},
               {"fwd", "fwd30", N_("Forward 30 seconds")}, {"next", "next", N_("Next chapter")}};
    for (int i = 0; i < countof(btn); i++) {
        if (!btn[i].icon) { /* play / pause: the big one */
            H(b, "<button type=\"button\" class=\"media-ab-play\" data-b=\"play\" aria-label=\"%s\"><span class=\"media-ab-a\">",
              T("Play"));
            ui_icon(b, "go");
            buf_puts(b, "</span><span class=\"media-ab-z\">");
            ui_icon(b, "pause");
            buf_puts(b, "</span></button>");
            continue;
        }
        H(b, "<button type=\"button\" data-b=\"%s\" aria-label=\"%s\" title=\"%s\">", btn[i].part, T(btn[i].label), T(btn[i].label));
        ui_icon(b, btn[i].icon);
        buf_puts(b, "</button>");
    }
    buf_puts(b, "</div><div class=\"media-ab-more\">");
    /* speed: the menu's choices (the device remembers the pick: wire.js) */
    H(b, "<span class=\"media-ab-menu\"><button type=\"button\" class=\"btn\" data-b=\"speed\" aria-haspopup=\"menu\" "
         "aria-label=\"%s\"><span data-b=\"speed-t\">1×</span></button><span role=\"menu\" hidden>",
      T("Speed"));
    static const char *const speeds[] = {"0.8", "1", "1.25", "1.5", "1.75", "2"};
    for (int i = 0; i < countof(speeds); i++) {
        char shown[16];
        snprintf(shown, sizeof shown, "%s×", speeds[i]);
        for (char *p = shown; *p; p++)
            if (*p == '.') *p = i18n_decimal(); /* ("1,25×" in Swedish and Spanish) */
        H(b, "<button type=\"button\" role=\"menuitemradio\" data-speed=\"%s\" aria-checked=\"%s\">%s</button>", speeds[i],
          str_eq(speeds[i], "1") ? "true" : "false", shown);
    }
    buf_puts(b, "</span></span>");
    /* the sleep timer */
    H(b, "<span class=\"media-ab-menu\"><button type=\"button\" class=\"btn\" data-b=\"sleep\" aria-haspopup=\"menu\">");
    ui_icon(b, "moon");
    H(b, "<span data-b=\"sleep-t\">%s</span></button><span role=\"menu\" hidden>", T("Sleep timer"));
    H(b, "<button type=\"button\" role=\"menuitemradio\" data-sleep=\"0\" aria-checked=\"true\">%s</button>", T("Off"));
    static const int mins[] = {15, 30, 45, 60};
    for (int i = 0; i < countof(mins); i++) {
        H(b, "<button type=\"button\" role=\"menuitemradio\" data-sleep=\"%d\" aria-checked=\"false\">", mins[i]);
        H(b, T("%d minutes"), mins[i]);
        buf_puts(b, "</button>");
    }
    if (nch > 1)
        H(b, "<button type=\"button\" role=\"menuitemradio\" data-sleep=\"ch\" aria-checked=\"false\">%s</button>",
          T("End of this chapter"));
    buf_puts(b, "</span></span></div></div></div>");
}

void media_book_page(Ctx *c, Buf *b, const char *idhex) {
    uint64_t id = 0;
    const MediaBook *bk = media_parse_id(idhex, &id) ? media_book(id) : NULL;
    if (!media_kids_book_ok(c, bk)) bk = NULL;
    if (!bk) {
        ctx_title(c, "%s", T("Audiobooks"));
        ui_empty(b, "🎧", T("This audiobook isn't in the library"), T("It may have been moved, renamed or deleted."));
        return;
    }
    ctx_title(c, "%s", bk->title);
    const MediaWatch *w = book_watch(c, bk);
    long long at = -1;
    const char *aq = ctx_q(c, "at");
    bool asked = *aq && str_int(aq, &at) && at >= 0 && at < 7 * 86400;
    int64_t resume_ms = asked ? at * 1000 : (int64_t)media_resume_at(w) * 1000;

    H(b, "<a class=\"media-back\" href=\"/media/books\">");
    ui_icon(b, "left");
    H(b, "%s</a>", T("Audiobooks"));
    BookChapter *ch = xmalloc(BOOK_CHAPTERS_MAX * sizeof *ch);
    int nch = book_chapters(bk, ch, BOOK_CHAPTERS_MAX);

    buf_puts(b, "<div class=\"media-book\"><div class=\"media-book-art\">");
    book_art(b, bk, 0, false, true);
    buf_puts(b, "</div><div class=\"media-book-t\">");
    H(b, "<h1>%s</h1><p class=\"sub\">", bk->title);
    Buf about = {0}; /* "J.K. Rowling · read by Full Cast · 2025 · 8 h 41 min" */
    if (bk->author[0]) H(&about, "%s", bk->author);
    if (bk->narrator[0] && !str_eq(bk->narrator, bk->author)) {
        buf_puts(&about, about.len ? " · " : "");
        H(&about, T("read by %s"), bk->narrator);
    }
    if (bk->year) H(&about, "%s%d", about.len ? " · " : "", bk->year);
    char d[32];
    media_fmt_duration(bk->duration_ms, d, sizeof d);
    if (d[0]) H(&about, "%s%s", about.len ? " · " : "", d);
    H(b, "%r</p>", buf_cstr(&about));
    buf_free(&about);
    if (bk->series[0]) {
        buf_puts(b, "<p class=\"muted media-book-series\">");
        if (bk->number) H(b, T("%s · book %d"), bk->series, bk->number);
        else H(b, "%s", bk->series);
        buf_puts(b, "</p>");
    }
    player(c, b, bk, resume_ms, asked, ch, nch);
    buf_puts(b, "</div></div>");

    /* what the viewer can do with it */
    buf_puts(b, "<div class=\"ph-a media-book-a\">");
    if (ctx_viewer(c)) {
        bool done = w && w->done;
        ui_form(b, "media.watched", "inline");
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"done\" value=\"%d\">", bk->id,
          done ? 0 : 1);
        H(b, "<button class=\"btn%r\" aria-pressed=\"%r\">", done ? " media-watched" : "", done ? "true" : "false");
        ui_icon(b, "check");
        H(b, "<span>%s</span></button>", done ? T("Finished listening") : T("Mark as finished"));
        ui_form_end(b);
    }
    if (media_grown_up(c)) { /* (members and admins: download.h) */
        int64_t bytes = 0;
        for (int i = 0; i < bk->count; i++) bytes += MAX(media_book_part(bk, i)->size, 0);
        char size[24];
        media_size_text(bytes, size, sizeof size);
        if (bk->count == 1) H(b, "<a class=\"btn\" href=\"/media/file/%X?download=1\" download data-native>", bk->id);
        else H(b, "<a class=\"btn\" href=\"/media/zip/%X\" download data-native>", bk->id);
        H(b, T("Download (%s)"), size);
        buf_puts(b, "</a>");
    }
    media_offline_book(c, b, bk); /* (in the app: "Save for offline" - offline.c, 2.3) */
    buf_puts(b, "</div>");

    const MediaTags *t = media_tags(bk->id);
    if (t && t->desc) H(b, "<details class=\"media-book-about\"><summary>%s</summary><p>%s</p></details>", T("About this book"), t->desc);

    if (nch > 1) {
        /* data-static: wire.js marks the chapter that's on (aria-current) - a re-render mustn't take that away */
        H(b, "<h2 class=\"mt\">%s</h2><ol class=\"media-chapters\" id=\"media-chapters\" data-static=\"%X-%d\">", T("Chapters"),
          bk->id, nch);
        for (int i = 0; i < nch; i++) {
            H(b, "<li><a href=\"/media/book/%X?at=%L\" data-at=\"%L\"><span class=\"media-ch-t\">%s</span><span class=\"media-ch-at\">",
              bk->id, ch[i].at_ms / 1000, ch[i].at_ms / 1000, ch[i].title);
            book_clock(b, ch[i].at_ms);
            buf_puts(b, "</span></a></li>");
        }
        buf_puts(b, "</ol>");
    }
    free(ch);

    /* the next book of its series */
    const MediaIndex *ix = media_lib_for(c);
    for (int i = 0; bk->series[0] && i + 1 < ix->nbooks; i++) {
        if (ix->books[i].id != bk->id || !str_eq(ix->books[i + 1].series, bk->series)) continue;
        const MediaBook *nx = &ix->books[i + 1];
        H(b, "<p class=\"media-book-next\"><span class=\"muted\">%s</span> <a href=\"/media/book/%X\">%s</a></p>",
          T("Next in the series:"), nx->id, nx->title);
    }
}
