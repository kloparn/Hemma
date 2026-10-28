/* modules/media/booklist.c — the library's audiobooks (1.6), made from its audio files (MF_AUDIO) and their tags
 * whenever an index is built (library.c index_finish: on the scan's thread, or at a start from the cache).
 *
 *   - An .m4b is a book of its own - two or more named as parts of one ("Book Part 1.m4b", "Book CD2.m4b") are
 *     one book, in part order.
 *   - .mp3 and .m4a files are a book's parts: every one in a folder, a short "CD1" / "Disc 2" / "Del 3" folder's
 *     with its parent's - by disc, then their tags' track, then their names (numbers by value: "2" before "10").
 *     One lying loose in a books folder ("Audiobooks/Book.mp3") or the library folder is a book of its own.
 *   - A book's title: an .m4b's own title tag, else its album, else its file's name; a book of parts: their
 *     album, else its folder's name. "(Unabridged)" at the end is left off. Its author and narrator: the first
 *     part that names one.
 *   - Books sitting in one folder with other books (not a books folder like "Audiobooks", not the library folder)
 *     are a series named by that folder ("Harry Potter Full-Cast Audio Editions"), numbered by the number their
 *     name starts with ("1 Harry Potter and ..."); numbered books right in the library folder, by its own name.
 *     Listed by series (else title), number, title.
 *   - Its picture: "<its name>.jpg" (.png, .webp) beside an .m4b, else - the only book in its folder, or a book of
 *     parts - that folder's cover / folder / poster picture; else one inside a part (MediaTags.cover); else drawn.
 * Its id is its first part's file id (library.h MediaBook); each part's MediaFile.series is its book's id. */
#include "library.h"

#include <ctype.h>

static const char *base_of(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

static size_t dir_len(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? (size_t)(s - rel) : 0;
}

static const char *seg_of(const char *rel, size_t dl) { /* the last folder's name in rel[0..dl) */
    const char *seg = rel;
    for (const char *p = rel; p < rel + dl; p++)
        if (*p == '/') seg = p + 1;
    return seg;
}

/* The folder a part's book is: its own, or its parent when its own is a short "CD1" / "Disc 2" / "Del 3" (*disc
   says which) - "Harry Potter Part 1" is a book's own folder */
static size_t book_dir(const char *rel, int *disc) {
    size_t dl = dir_len(rel);
    const char *seg = seg_of(rel, dl);
    size_t n = (size_t)(rel + dl - seg);
    char d[24];
    *disc = 0;
    if (seg > rel && n && n <= 10) {
        str_ncopy(d, sizeof d, seg, n);
        *disc = media_name_part(d);
    }
    return *disc ? (size_t)(seg - rel - 1) : dl;
}

/* Names in reading order: numbers by their value ("Chapter 2" before "Chapter 10"), the rest by letter, any case */
static int nat_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0' && isdigit((unsigned char)a[1])) a++;
            while (*b == '0' && isdigit((unsigned char)b[1])) b++;
            size_t la = strspn(a, "0123456789"), lb = strspn(b, "0123456789");
            if (la != lb) return la < lb ? -1 : 1;
            int c = strncmp(a, b, la);
            if (c) return c;
            a += la, b += lb;
            continue;
        }
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x < y ? -1 : 1;
        a++, b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}

/* The number a name starts with ("1 Harry Potter", "01 - The Hobbit", "3. Book"): 1..999, else 0; *rest = the
   name after it */
static int lead_number(const char *name, const char **rest) {
    int n = 0, k = 0;
    while (k < 3 && isdigit((unsigned char)name[k])) n = n * 10 + (name[k++] - '0');
    if (!k || isdigit((unsigned char)name[k]) || !name[k] || !strchr(" ._-", name[k])) return 0;
    const char *r = name + k;
    while (*r && strchr(" ._-", *r)) r++;
    if (!*r) return 0;
    if (rest) *rest = r;
    return n;
}

/* A file's or folder's name as a title: no extension (a file's), no number in front, '_' as spaces */
static void name_title(const char *name, size_t n, bool file, char *out, size_t cap) {
    char s[256];
    str_ncopy(s, sizeof s, name, n);
    if (file) {
        char *dot = strrchr(s, '.');
        if (dot && dot != s) *dot = '\0';
    }
    for (char *p = s; *p; p++)
        if (*p == '_') *p = ' ';
    const char *rest = s;
    lead_number(s, &rest);
    str_copy(out, cap, rest);
    str_trim(out);
    if (!out[0]) str_copy(out, cap, s);
}

/* "... (Unabridged)" at the end: not part of the title */
static void tidy_title(char *t) {
    static const char *const ends[] = {"(unabridged)", "[unabridged]", "(ungekürzt)", "(oförkortad)", "(íntegro)", NULL};
    char lo[256];
    fold_copy(lo, sizeof lo, t);
    size_t n = strlen(lo);
    for (int i = 0; ends[i]; i++) {
        size_t e = strlen(ends[i]);
        if (n > e && str_eq(lo + n - e, ends[i])) {
            t[n - e] = '\0';
            str_trim(t);
            return;
        }
    }
}

static const MediaTags *tags_of(const MediaIndex *ix, uint64_t file) {
    int lo = 0, hi = ix->ntags;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ix->tags[mid].file < file) lo = mid + 1;
        else hi = mid;
    }
    return lo < ix->ntags && ix->tags[lo].file == file ? &ix->tags[lo] : NULL;
}

typedef struct {
    int idx;       /* in ix->files */
    size_t dl;     /* its book's folder: rel[0..dl) - an .m4b's own folder */
    int disc;      /* its disc folder's number, 0 = none */
    int part;      /* an .m4b's part number (media_name_part), 0 = none */
    char key[160]; /* an .m4b's name without its part ("Book Part 2" -> "m4b:book"): its book; "" = a part */
    const MediaTags *t;
} Item;

static _Thread_local const MediaIndex *t_ix;

static int item_cmp(const void *x, const void *y) {
    const Item *a = x, *b = y;
    const MediaFile *fa = &t_ix->files[a->idx], *fb = &t_ix->files[b->idx];
    if (fa->root != fb->root) return fa->root - fb->root;
    int c = strncmp(fa->rel, fb->rel, MIN(a->dl, b->dl));
    if (c) return c;
    if (a->dl != b->dl) return a->dl < b->dl ? -1 : 1;
    if ((c = strcmp(a->key, b->key))) return c;
    if (a->disc != b->disc) return a->disc - b->disc;
    if (a->part != b->part) return a->part - b->part;
    int ta = a->t ? a->t->track : 0, tb = b->t ? b->t->track : 0;
    if (ta && tb && ta != tb) return ta - tb;
    return nat_cmp(base_of(fa->rel), base_of(fb->rel));
}

static bool same_book(const Item *a, const Item *b, const MediaIndex *ix) {
    const MediaFile *fa = &ix->files[a->idx], *fb = &ix->files[b->idx];
    return fa->root == fb->root && a->dl == b->dl && !strncmp(fa->rel, fb->rel, a->dl) && str_eq(a->key, b->key);
}

/* A picture in rel[0..dl) named `stem` (.jpg .jpeg .png .webp, any case) */
static uint64_t picture_named(const MediaIndex *ix, const int *img, int nimg, int root, const char *rel, size_t dl,
                              const char *stem) {
    size_t sl = strlen(stem);
    for (int k = 0; k < nimg; k++) {
        const MediaFile *p = &ix->files[img[k]];
        const char *b = base_of(p->rel), *dot = strrchr(b, '.');
        if (p->root != root || dir_len(p->rel) != dl || strncmp(p->rel, rel, dl) != 0 || !dot) continue;
        char name[256];
        str_ncopy(name, sizeof name, b, (size_t)(dot - b));
        if ((size_t)(dot - b) == sl && str_ieq(name, stem)) return p->id;
    }
    return 0;
}

static uint64_t folder_picture(const MediaIndex *ix, const int *img, int nimg, int root, const char *rel, size_t dl) {
    static const char *const names[] = {"cover", "folder", "poster", NULL};
    for (int i = 0; names[i]; i++) {
        uint64_t id = picture_named(ix, img, nimg, root, rel, dl, names[i]);
        if (id) return id;
    }
    return 0;
}

static int book_cmp(const void *x, const void *y) {
    const MediaBook *a = x, *b = y;
    char ka[200], kb[200];
    fold_copy(ka, sizeof ka, a->series[0] ? a->series : a->title);
    fold_copy(kb, sizeof kb, b->series[0] ? b->series : b->title);
    int c = strcmp(ka, kb);
    if (c) return c;
    if (a->number != b->number) return a->number - b->number;
    fold_copy(ka, sizeof ka, a->title);
    fold_copy(kb, sizeof kb, b->title);
    return strcmp(ka, kb);
}

void media_books_build(MediaIndex *ix) {
    ix->books = NULL, ix->book_parts = NULL, ix->nbooks = 0;
    int n = 0, nimg = 0;
    for (int i = 0; i < ix->nfiles; i++) {
        n += ix->files[i].type == MF_AUDIO;
        nimg += ix->files[i].type == MF_IMAGE;
    }
    if (!n) return;
    Item *it = xcalloc((size_t)n, sizeof *it);
    int *img = xmalloc((size_t)(nimg ? nimg : 1) * sizeof *img);
    n = nimg = 0;
    for (int i = 0; i < ix->nfiles; i++) {
        MediaFile *f = &ix->files[i];
        if (f->type == MF_IMAGE) img[nimg++] = i;
        if (f->type != MF_AUDIO) continue;
        Item *m = &it[n++];
        m->idx = i;
        m->t = tags_of(ix, f->id);
        if (!str_iends(f->rel, ".m4b")) {
            m->dl = book_dir(f->rel, &m->disc);
            const char *seg = seg_of(f->rel, m->dl);
            char name[96];
            str_ncopy(name, sizeof name, seg, (size_t)(f->rel + m->dl - seg));
            if (m->dl && !media_is_books_folder(name)) continue; /* (a folder of parts: the folder is the book) */
            m->dl = dir_len(f->rel); /* loose in "Audiobooks" or the library folder: a book of its own */
            m->disc = 0;
            str_copy(m->key, sizeof m->key, "one:");
            str_copy(m->key + 4, sizeof m->key - 4, base_of(f->rel));
            continue;
        }
        m->dl = dir_len(f->rel); /* a book of its own - unless its name says it's one part of one */
        m->part = media_name_part(f->rel);
        char lo[256]; /* (its whole name: "1 Story" and "2 Story" are two books) */
        fold_copy(lo, sizeof lo, base_of(f->rel));
        char *dot = strrchr(lo, '.');
        if (dot && dot != lo) *dot = '\0';
        if (m->part) { /* "book part 2" -> "book" */
            static const char *const marks[] = {" part", " pt", " cd", " disc", " disk", " del", NULL};
            for (int k = 0; marks[k]; k++) {
                char *at = strstr(lo, marks[k]);
                if (at) *at = '\0';
            }
            str_trim(lo);
        }
        str_copy(m->key, sizeof m->key, "m4b:");
        str_copy(m->key + 4, sizeof m->key - 4, lo);
    }
    t_ix = ix;
    qsort(it, (size_t)n, sizeof *it, item_cmp);
    t_ix = NULL;

    ix->book_parts = xmalloc((size_t)n * sizeof *ix->book_parts);
    ix->books = xcalloc((size_t)n, sizeof *ix->books);
    size_t *home = xmalloc((size_t)n * sizeof *home); /* per book: the folder it sits in, rel[0..home) of its first part */
    for (int a = 0, z; a < n; a = z) {
        z = a + 1;
        while (z < n && same_book(&it[a], &it[z], ix)) z++;
        int k = ix->nbooks++;
        MediaBook *b = &ix->books[k];
        const MediaFile *first = &ix->files[it[a].idx];
        b->id = first->id;
        b->root = first->root;
        b->first = a;
        b->count = z - a;
        for (int j = a; j < z; j++) {
            MediaFile *p = &ix->files[it[j].idx];
            ix->book_parts[j] = it[j].idx;
            p->series = b->id;
            b->duration_ms += p->probe.duration_ms > 0 ? p->probe.duration_ms : 0;
            if (p->mtime > b->newest) b->newest = p->mtime;
            const MediaTags *t = it[j].t;
            if (!t) continue;
            if (!b->author[0] && t->author[0]) str_copy(b->author, sizeof b->author, t->author);
            if (!b->narrator[0] && t->narrator[0]) str_copy(b->narrator, sizeof b->narrator, t->narrator);
            if (!b->year && t->year) b->year = t->year;
            if (!b->cover && t->cover_len > 0) b->cover = p->id;
        }
        const MediaTags *t = it[a].t;
        size_t dl = it[a].dl;
        const char *seg = seg_of(first->rel, dl);
        if (it[a].key[0]) { /* an .m4b: its own name and picture, in its folder */
            if (t && t->title[0]) str_copy(b->title, sizeof b->title, t->title);
            else if (t && t->album[0]) str_copy(b->title, sizeof b->title, t->album);
            b->number = lead_number(base_of(first->rel), NULL);
            char stem[256];
            str_copy(stem, sizeof stem, base_of(first->rel));
            char *dot = strrchr(stem, '.');
            if (dot && dot != stem) *dot = '\0';
            b->poster = picture_named(ix, img, nimg, b->root, first->rel, dl, stem);
            home[k] = dl;
        } else { /* a folder of parts: the folder is the book */
            if (t && t->album[0]) str_copy(b->title, sizeof b->title, t->album);
            else if (dl) name_title(seg, (size_t)(first->rel + dl - seg), false, b->title, sizeof b->title);
            b->number = dl ? lead_number(seg, NULL) : 0;
            b->poster = folder_picture(ix, img, nimg, b->root, first->rel, dl);
            home[k] = seg > first->rel ? (size_t)(seg - first->rel - 1) : 0;
        }
        if (!b->title[0]) name_title(base_of(first->rel), strlen(base_of(first->rel)), true, b->title, sizeof b->title);
        tidy_title(b->title);
    }
    /* series: books sharing a folder (not the library folder, not a books folder); an .m4b alone in its folder
       takes the folder's picture */
    for (int i = 0; i < ix->nbooks; i++) {
        MediaBook *b = &ix->books[i];
        const char *rel = ix->files[ix->book_parts[b->first]].rel;
        int same = 0, numbered = 0;
        for (int j = 0; j < ix->nbooks; j++) {
            const MediaBook *o = &ix->books[j];
            const char *orel = ix->files[ix->book_parts[o->first]].rel;
            bool here = o->root == b->root && home[j] == home[i] && !strncmp(orel, rel, home[i]);
            same += here;
            numbered += here && o->number;
        }
        const char *seg = seg_of(rel, home[i]);
        char name[200];
        str_ncopy(name, sizeof name, seg, (size_t)(rel + home[i] - seg));
        bool named = home[i] != 0;
        if (!home[i] && numbered > 1 && b->root >= 0 && b->root < ix->nroots) { /* numbered books right in the library
            folder: a series by the library folder's own name ("D:/.../Harry Potter Full-Cast Audio Editions") */
            const char *r = strrchr(ix->roots[b->root], '/');
            str_copy(name, sizeof name, r ? r + 1 : ix->roots[b->root]);
            named = name[0] && name[strlen(name) - 1] != ':'; /* (not a drive: "D:") */
        }
        if (same > 1 && named && !media_is_books_folder(name)) name_title(name, strlen(name), false, b->series, sizeof b->series);
        if (same == 1 && !b->poster && str_iends(rel, ".m4b")) b->poster = folder_picture(ix, img, nimg, b->root, rel, home[i]);
    }
    free(home);
    qsort(ix->books, (size_t)ix->nbooks, sizeof *ix->books, book_cmp);
    free(img);
    free(it);
}
