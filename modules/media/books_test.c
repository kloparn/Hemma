/* Audiobooks (1.6): which files are books and how they group (booklist.c), the Audiobooks tab, a book's page and
 * its player's markup, progress (Continue listening, finished only at the end), what children see, search. */
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "library.h"
#include "media.h"

static void put(const char *root, const char *rel, const char *text) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, text, strlen(text));
}

static char g_root[512];

static int boot_in(const char *data) {
    if (data) th_boot_in(data);
    else th_boot();
    int adam = th_person("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put(g_root, "Harry Potter Full-Cast/1 Harry Potter and the Stone.m4b", "b1");
    put(g_root, "Harry Potter Full-Cast/2 Harry Potter and the Chamber.m4b", "b2");
    put(g_root, "Harry Potter Full-Cast/cover.jpg", "jpg"); /* (the series' folder: not one book's picture) */
    put(g_root, "Audiobooks/The Hobbit/10 - Chapter Ten.mp3", "p10");
    put(g_root, "Audiobooks/The Hobbit/01 - Chapter One.mp3", "p1");
    put(g_root, "Audiobooks/The Hobbit/02 - Chapter Two.mp3", "p2");
    put(g_root, "Audiobooks/The Hobbit/folder.jpg", "jpg");
    put(g_root, "Audiobooks/Big Book/CD2/01.mp3", "cd2");
    put(g_root, "Audiobooks/Big Book/CD1/01.mp3", "cd1");
    put(g_root, "Audiobooks/Loose Story.mp3", "loose");
    put(g_root, "Ljudböcker/Pippi/pippi.m4a", "pippi");
    put(g_root, "Single.m4b", "single");
    put(g_root, "Single.png", "png");
    /* audio that isn't a book: a show's theme song, a film's soundtrack */
    put(g_root, "Series/Garden Show/theme.mp3", "theme");
    put(g_root, "Series/Garden Show/Season 1/Garden.Show.S01E01.mp4", "e1");
    put(g_root, "Movies/Film (2020)/Film.mkv", "film");
    put(g_root, "Movies/Film (2020)/soundtrack.m4a", "music");
    setting_set("media.roots", g_root);
    media_scan_sync();
    return adam;
}

static int boot_with_books(void) { return boot_in(NULL); }

static const MediaBook *book_titled(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nbooks; i++)
        if (str_eq(ix->books[i].title, title)) return &ix->books[i];
    return NULL;
}

static const char *part_rel(const MediaBook *b, int i) {
    const MediaFile *f = media_book_part(b, i);
    return f ? f->rel : "";
}

static const char *book_url(const char *fmt, uint64_t id) {
    static char u[200];
    char hex[17];
    media_id_hex(id, hex);
    snprintf(u, sizeof u, fmt, hex);
    return u;
}

static const char *progress(uint64_t id, int pos, int dur, bool ended) {
    static char form[160];
    snprintf(form, sizeof form, "id=%016llx&pos=%d&dur=%d%s", (unsigned long long)id, pos, dur, ended ? "&ended=1" : "");
    return form;
}

TEST(books_which_files_and_how_they_group) {
    CHECK(media_is_book_file(NULL, "Anything/Book.m4b"));
    CHECK(media_is_book_file(NULL, "Audiobooks/A/01.mp3"));
    CHECK(media_is_book_file(NULL, "ljudböcker/a.M4A"));
    CHECK(media_is_book_file(NULL, "Audio_Books/x.mp3"));
    CHECK(media_is_book_file("Audiolibros", "x.mp3")); /* (the library folder itself is one) */
    CHECK(!media_is_book_file(NULL, "Series/Show/theme.mp3"));
    CHECK(!media_is_book_file(NULL, "Movies/Film/soundtrack.m4a"));
    CHECK(!media_is_book_file(NULL, "Audiobooks/x.flac"));
    CHECK(media_is_books_folder("Hörbücher") && media_is_books_folder("AUDIO-BOOKS") && !media_is_books_folder("Books"));

    char data[512];
    str_copy(data, sizeof data, test_tmpdir());
    boot_in(data);
    const MediaIndex *ix = media_lib();
    CHECK_INT(ix->nbooks, 7);
    /* a series: two .m4b in one folder, numbered by their names, its picture not taken by either */
    const MediaBook *b1 = book_titled("Harry Potter and the Stone"), *b2 = book_titled("Harry Potter and the Chamber");
    CHECK(b1 && b2);
    CHECK_STR(b1->series, "Harry Potter Full-Cast");
    CHECK_INT(b1->number, 1);
    CHECK_INT(b2->number, 2);
    CHECK(b1 < b2); /* (listed in their order) */
    CHECK(!b1->poster && !b2->poster);
    /* a folder of MP3s: one book, its parts in reading order, the folder's picture */
    const MediaBook *h = book_titled("The Hobbit");
    CHECK(h);
    CHECK_INT(h->count, 3);
    CHECK(str_ends(part_rel(h, 0), "01 - Chapter One.mp3"));
    CHECK(str_ends(part_rel(h, 1), "02 - Chapter Two.mp3"));
    CHECK(str_ends(part_rel(h, 2), "10 - Chapter Ten.mp3"));
    CHECK(h->poster != 0);
    CHECK_STR(h->series, ""); /* (its folder holds no other book: "Audiobooks" is a books folder anyway) */
    /* CD folders are one book, by disc */
    const MediaBook *big = book_titled("Big Book");
    CHECK(big && big->count == 2);
    CHECK(str_ends(part_rel(big, 0), "CD1/01.mp3"));
    CHECK(str_ends(part_rel(big, 1), "CD2/01.mp3"));
    /* loose in a books folder: a book of its own; a lone part: its folder's name */
    CHECK(book_titled("Loose Story") && book_titled("Loose Story")->count == 1);
    CHECK(book_titled("Pippi"));
    /* an .m4b with a picture of its own name beside it */
    const MediaBook *single = book_titled("Single");
    CHECK(single && single->poster != 0);
    /* each part knows its book; the book's id is its first part's */
    CHECK(media_book(h->id) == h);
    CHECK(media_file(h->id) == media_book_part(h, 0));
    CHECK(media_book_part(h, 2)->series == h->id);
    /* not books: the theme song and the soundtrack aren't in the library at all */
    for (int i = 0; i < ix->nfiles; i++) CHECK(!str_ends(ix->files[i].rel, "theme.mp3") && !str_ends(ix->files[i].rel, "soundtrack.m4a"));
    /* and the same after a start, from the library's cache (no scan) */
    th_halt();
    th_boot_in(data);
    CHECK_INT(media_lib()->nbooks, 7);
    h = book_titled("The Hobbit");
    CHECK(h && h->count == 3 && str_ends(part_rel(h, 2), "10 - Chapter Ten.mp3"));
    CHECK(book_titled("Harry Potter and the Stone") && book_titled("Harry Potter and the Stone")->number == 1);
    th_halt();
}

TEST(books_tab_page_and_player) {
    int adam = boot_with_books();
    /* Watch has the tab now */
    const char *html = th_page(adam, "/media");
    CHECK_HAS(html, "href=\"/media/books\"");
    html = th_page(adam, "/media/books");
    CHECK_HAS(html, "Audiobooks");
    CHECK_HAS(html, "7 audiobooks");
    CHECK_HAS(html, "<h2>Harry Potter Full-Cast</h2>"); /* the series' books together, under its name */
    CHECK_HAS(html, "The Hobbit");
    /* then the other books, in one section - "Big Book" sorts before the series and "The Hobbit" after it */
    const char *books = strstr(html, "<h2>Books</h2>");
    CHECK(books && !strstr(books + 1, "<h2>Books</h2>") && books > strstr(html, "<h2>Harry Potter Full-Cast</h2>"));
    CHECK(books && strstr(books, ">Big Book<") && strstr(books, ">The Hobbit<"));
    CHECK_HAS(html, "placeholder=\"Search audiobooks…\"");
    CHECK(!strstr(html, "Continue listening"));
    const MediaBook *h = book_titled("The Hobbit");
    html = th_page(adam, book_url("/media/book/%s", h->id));
    CHECK_HAS(html, "<h1>The Hobbit</h1>");
    CHECK_HAS(html, "<audio id=\"media-audio\"");
    CHECK_HAS(html, "data-book");
    CHECK_HAS(html, "data-report=\"media.progress\"");
    CHECK_HAS(html, "data-parts=\"[[&quot;/media/file/"); /* a book in parts: each part's address, start, length */
    CHECK_HAS(html, "data-listen=\"media-audio\"");
    CHECK_HAS(html, "data-b=\"play\"");
    CHECK_HAS(html, "data-speed=\"1.25\"");
    CHECK_HAS(html, "data-sleep=\"ch\""); /* (it has chapters: its parts) */
    CHECK_HAS(html, "id=\"media-chapters\"");
    CHECK_HAS(html, "Chapter One");
    CHECK_HAS(html, "Chapter Ten");
    CHECK_HAS(html, "/media/zip/"); /* a download of all its parts (a member) */
    /* an .m4b of its own: one file, no parts list, a plain download */
    const MediaBook *s = book_titled("Single");
    html = th_page(adam, book_url("/media/book/%s", s->id));
    CHECK(!strstr(html, "data-parts"));
    CHECK(!strstr(html, "id=\"media-chapters\""));
    CHECK_HAS(html, book_url("/media/file/%s?download=1", s->id));
    /* the next in its series */
    const MediaBook *b1 = book_titled("Harry Potter and the Stone"), *b2 = book_titled("Harry Potter and the Chamber");
    html = th_page(adam, book_url("/media/book/%s", b1->id));
    CHECK_HAS(html, "Harry Potter Full-Cast · book 1");
    CHECK_HAS(html, "Next in the series:");
    CHECK_HAS(html, book_url("/media/book/%s\">Harry Potter and the Chamber", b2->id));
    /* a book that isn't there */
    CHECK_HAS(th_page(adam, "/media/book/0123456789abcdef"), "This audiobook isn&#39;t in the library");
    th_halt();
}

TEST(books_progress_continue_listening_and_finished) {
    int adam = boot_with_books();
    const MediaBook *b1 = book_titled("Harry Potter and the Stone");
    uint64_t id = b1->id;
    /* (the files are text here: their lengths come from progress reports) */
    CHECK(th_act(adam, "media.progress", progress(id, 600, 30000, false)).ok);
    const char *html = th_page(adam, "/media/books");
    CHECK_HAS(html, "Continue listening");
    CHECK_HAS(html, book_url("media-going-%s", id));
    CHECK_HAS(th_page(adam, "/media"), "Continue listening"); /* Watch's home too */
    CHECK(!strstr(th_page(adam, "/media"), "Continue watching")); /* (a book isn't a video) */
    /* the page starts where they were */
    html = th_page(adam, book_url("/media/book/%s", id));
    CHECK_HAS(html, "#t=600");
    CHECK_HAS(html, "data-resume=\"600\"");
    /* 92% of a book isn't the end of it (a video's credits are) */
    CHECK(th_act(adam, "media.progress", progress(id, 29000, 30000, false)).ok);
    CHECK(!media_watch_get(adam, id)->done);
    CHECK(th_act(adam, "media.progress", progress(id, 29990, 30000, false)).ok);
    CHECK(media_watch_get(adam, id)->done);
    CHECK(!strstr(th_page(adam, "/media/books"), "Continue listening"));
    CHECK_HAS(th_page(adam, book_url("/media/book/%s", id)), "Finished listening");
    /* ticked back, and the x */
    ThResult r = th_act(adam, "media.watched", book_url("id=%s&done=0", id));
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Marked as not finished");
    CHECK(th_act(adam, "media.progress", progress(id, 900, 30000, false)).ok);
    r = th_act(adam, "media.forget", book_url("id=%s", id));
    CHECK_HAS(r.toast, "Removed Harry Potter and the Stone from Continue listening");
    CHECK(!media_watch_get(adam, id));
    /* ?at= (a chapter's link, without wire.js): from there, playing */
    html = th_page(adam, book_url("/media/book/%s?at=75", id));
    CHECK_HAS(html, "#t=75");
    CHECK_HAS(html, " autoplay");
    th_halt();
}

TEST(books_in_parts_start_in_the_right_part) {
    int adam = boot_with_books();
    /* parts of known lengths (as the scan would have read them) */
    const MediaBook *h = book_titled("The Hobbit");
    for (int i = 0; i < h->count; i++) ((MediaFile *)media_book_part(h, i))->probe.duration_ms = 100000; /* 100 s each */
    ((MediaBook *)h)->duration_ms = 300000;
    CHECK(th_act(adam, "media.progress", progress(h->id, 150, 300, false)).ok);
    const char *html = th_page(adam, book_url("/media/book/%s", h->id));
    /* 150 s in = 50 s into the second part */
    char want[120];
    snprintf(want, sizeof want, "src=\"/media/file/%016llx", (unsigned long long)media_book_part(h, 1)->id);
    CHECK_HAS(html, want);
    CHECK_HAS(html, "#t=50");
    CHECK_HAS(html, "data-length=\"300\"");
    CHECK_HAS(html, ",100.000,100.000]"); /* the second part: from 100 s, 100 s long */
    th_halt();
}

TEST(books_children_and_search) {
    int adam = boot_with_books();
    int leo = th_person("Leo");
    Person p = *person_get(leo);
    p.role = ROLE_CHILD;
    tbl_put(&T_person, &p);
    const MediaBook *h = book_titled("The Hobbit");
    uint64_t hid = h->id;
    media_kids_set(g_root, "Audiobooks/The Hobbit", true);
    const char *html = th_page(leo, "/media/books");
    CHECK(!strstr(html, "The Hobbit"));
    CHECK_HAS(html, "Big Book");
    CHECK_HAS(th_page(leo, book_url("/media/book/%s", hid)), "This audiobook isn&#39;t in the library");
    CHECK(!strstr(th_page(leo, book_url("/media/book/%s", book_titled("Big Book")->id)), "?download=1")); /* (grown-ups) */
    CHECK_HAS(th_page(adam, "/media/books"), "The Hobbit");
    /* search finds books by title (and not for a child, what's marked) */
    html = th_page(adam, "/media?q=hobbit");
    CHECK_HAS(html, "Audiobooks");
    CHECK_HAS(html, "The Hobbit");
    CHECK(!strstr(th_page(leo, "/media?q=hobbit"), "The Hobbit"));
    CHECK_HAS(th_page(adam, "/media/books?q=potter"), "Harry Potter and the Chamber"); /* (from the tab too) */
    th_halt();
}

TEST(books_tab_hidden_without_books) {
    th_boot();
    int adam = th_person("Adam");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put(root, "Movies/Film (2020)/Film.mkv", "film");
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK(!strstr(th_page(adam, "/media"), "href=\"/media/books\""));
    CHECK_HAS(th_page(adam, "/media/books"), "No audiobooks yet"); /* (the address itself still answers) */
    th_halt();
}

TEST(books_speak_swedish_and_spanish) {
    int adam = boot_with_books();
    const MediaBook *b1 = book_titled("Harry Potter and the Stone"), *h = book_titled("The Hobbit");
    CHECK(th_act(adam, "media.progress", progress(b1->id, 600, 30000, false)).ok);
    /* data, not words: the books' and their folders' names */
    static const char *const allow[] = {"Adam", "A", "Harry Potter Full-Cast", "Harry Potter and the Stone",
                                        "Harry Potter and the Chamber", "The Hobbit", "Chapter One", "Chapter Two",
                                        "Chapter Ten", "Big Book", "Loose Story", "Pippi", "Single", "Garden Show", "Film", NULL};
    char urls[4][200];
    str_copy(urls[0], sizeof urls[0], "/media/books");
    str_copy(urls[1], sizeof urls[1], book_url("/media/book/%s", h->id));
    str_copy(urls[2], sizeof urls[2], book_url("/media/book/%s", b1->id));
    str_copy(urls[3], sizeof urls[3], "/media");
    Buf report = {0};
    int n = 0;
    for (int i = 0; i < countof(urls); i++) n += th_untranslated(adam, urls[i], allow, &report);
    if (n) printf("%s", buf_cstr(&report));
    CHECK_INT(n, 0);
    buf_free(&report);
    th_halt();
}
