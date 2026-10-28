/* modules/av/audiotags.c — what an audio file says about itself (audiotags.h): an audiobook's title, author,
 * narrator, chapters, cover and length, from an MP4's boxes or an MP3's ID3 tags, without decoding a sound.
 *
 * MP4: only box headers and the few small boxes it needs are read. An audiobook's moov is nearly all the sound's
 * sample tables (4 million sizes in an 18 MB moov) and those are never touched - a chapter track's tables are,
 * one entry per chapter. MP3: the ID3v2 tag's frames are walked one by one (unsynchronisation undone on the way),
 * a cover only located, then the first MPEG audio frame found for the codec and the length.
 *
 * Every count, size and offset is checked against the file and capped, so a damaged or hostile file costs a
 * bounded number of small reads and never more than a few hundred KB of memory (tools/avfuzz.c -DAVFUZZ_TAGS). */
#include "audiotags.h"

#include <stdlib.h>
#include <string.h>

#define KIDS_MAX 4096              /* children looked at in one box */
#define TRAKS_MAX 16               /* tracks of a moov looked at */
#define TXT_READ (3 * AT_DESC_MAX) /* bytes of one tag read at most: more than any field keeps */
#define STSD_READ 4096             /* a sample description read at most */
#define STTS_MAX 4096              /* time-to-sample entries read for chapters */
#define SAMPLE_READ (4 * AT_CHAPTER_TEXT) /* a chapter's text read at most */
#define CHPL_READ (9 + AT_MAX_CHAPTERS * (9 + 255))
#define ID3_FRAMES_MAX 8192        /* frames read from one ID3v2 tag */
#define ID3_HEAD_READ 4096         /* the start of a picture or chapter frame read */
#define MPEG_SCAN (64 * 1024)      /* bytes searched for an MP3's first frame */

enum { ENC_LATIN1, ENC_UTF16, ENC_UTF16BE, ENC_UTF8 }; /* as ID3 numbers them; ENC_UTF16 starts with a BOM */
enum { TX_LINE, TX_JOIN, TX_MULTI }; /* one line; one line of NUL-separated values; lines kept (a description) */

typedef struct {
    const AtReader *r;
    AudioTags *t;
    int desc_rank, cover_rank; /* how good the description and cover found so far are */
    uint8_t *tmp;              /* TXT_READ + 16 bytes of scratch */
    AtChapter *ch;             /* chapters as found (sorted at the end) */
    int nch, chcap;
} Cx;

static bool rd(const Cx *c, int64_t off, void *dst, size_t n) {
    int64_t size = c->r->size;
    if (off < 0 || size < 0 || (uint64_t)n > (uint64_t)size || off > size - (int64_t)n) return false;
    return n == 0 || c->r->read_at(c->r->ctx, off, dst, n);
}

static uint32_t g16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t g24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | g24(p + 1); }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) << 32 | g32(p + 4); }
static uint32_t gsafe(const uint8_t *p) { /* ID3's synchsafe numbers: 7 bits a byte */
    return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}
static bool is4(const void *a, const char *b) { return !memcmp(a, b, 4); }

static void copy_str(char *dst, size_t cap, const char *s) { /* s is plain ASCII here */
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* ------------------------------------------------------------------ text */

typedef struct {
    char *p;
    size_t cap, len;
    int mode;
    bool full, cr, sep; /* sep: a value ended (TX_JOIN) - ", " before the next */
    int spaces, lines;  /* white space waiting for the next character: never at either end */
} Tx;

static Tx tx_open(char *p, size_t cap, int mode) {
    p[0] = 0;
    return (Tx){.p = p, .cap = cap, .mode = mode};
}

static void tx_cp(Tx *o, uint32_t cp) {
    bool cr = o->cr;
    o->cr = cp == '\r';
    if (o->full || (cp == '\n' && cr)) return; /* (CR LF: one line break) */
    if (cp == '\r' || cp == '\n' || cp == 0x2028 || cp == 0x2029) {
        if (o->mode == TX_MULTI) o->lines++;
        else o->spaces++;
        return;
    }
    if (cp == ' ' || cp == '\t' || cp == 0x0B || cp == 0x0C) {
        o->spaces++;
        return;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || cp == 0xFEFF) return; /* controls; a stray BOM */
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
    char pre[2];
    size_t np = 0;
    if (o->len) {
        if (o->sep) pre[0] = ',', pre[1] = ' ', np = 2;
        else if (o->lines) pre[0] = pre[1] = '\n', np = o->lines > 1 ? 2 : 1;
        else if (o->spaces) pre[0] = ' ', np = 1;
    }
    char u[4];
    size_t k;
    if (cp < 0x80) u[0] = (char)cp, k = 1;
    else if (cp < 0x800) u[0] = (char)(0xC0 | cp >> 6), u[1] = (char)(0x80 | (cp & 0x3F)), k = 2;
    else if (cp < 0x10000)
        u[0] = (char)(0xE0 | cp >> 12), u[1] = (char)(0x80 | (cp >> 6 & 0x3F)), u[2] = (char)(0x80 | (cp & 0x3F)), k = 3;
    else
        u[0] = (char)(0xF0 | cp >> 18), u[1] = (char)(0x80 | (cp >> 12 & 0x3F)), u[2] = (char)(0x80 | (cp >> 6 & 0x3F)),
        u[3] = (char)(0x80 | (cp & 0x3F)), k = 4;
    if (o->len + np + k + 1 > o->cap) { /* cut at a whole character */
        o->full = true;
        return;
    }
    memcpy(o->p + o->len, pre, np);
    memcpy(o->p + o->len + np, u, k);
    o->len += np + k;
    o->p[o->len] = 0;
    o->sep = false, o->spaces = o->lines = 0;
}

/* A NUL: the end of the text, or (TX_JOIN) of one of its values; false = stop */
static bool tx_nul(Tx *o) {
    if (o->mode != TX_JOIN) return false;
    if (o->len) o->sep = true;
    return true;
}

/* Windows-1252's letters where Latin-1 has controls: what ID3's "ISO-8859-1" text holds in practice */
static const uint16_t CP1252[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

/* Bytes that are UTF-8 with something past ASCII in them - Latin-1 text hardly ever is, and many taggers (FFmpeg's
   ID3v1 among them) write UTF-8 where Latin-1 belongs */
static bool utf8_in(const uint8_t *p, size_t n) {
    bool wide = false;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < 0x80) continue;
        size_t k = p[i] >= 0xC2 && p[i] <= 0xDF ? 1 : p[i] >= 0xE0 && p[i] <= 0xEF ? 2 : p[i] >= 0xF0 && p[i] <= 0xF4 ? 3 : 0;
        if (!k || n - i <= k) return false;
        for (size_t j = 1; j <= k; j++)
            if ((p[i + j] & 0xC0) != 0x80) return false;
        i += k, wide = true;
    }
    return wide;
}

/* Text in an encoding into o. cut: the bytes end where a read stopped, not where the text does - a character
   cut there is left out, not replaced. */
static void tx_put(Tx *o, const uint8_t *p, size_t n, int enc, bool cut) {
    size_t i = 0;
    if (enc == ENC_LATIN1 && utf8_in(p, n)) enc = ENC_UTF8;
    if (enc == ENC_LATIN1) {
        for (; i < n && !o->full; i++) {
            if (!p[i] && !tx_nul(o)) return;
            uint32_t cp = p[i] >= 0x80 && p[i] < 0xA0 ? CP1252[p[i] - 0x80] : p[i];
            if (cp) tx_cp(o, cp);
        }
    } else if (enc == ENC_UTF16 || enc == ENC_UTF16BE) {
        bool le = false, start = true;
        while (i + 1 < n && !o->full) {
            uint32_t u = le ? (uint32_t)p[i + 1] << 8 | p[i] : g16(p + i);
            i += 2;
            if (start && (u == 0xFEFF || u == 0xFFFE)) { /* a value's BOM */
                if (u == 0xFFFE) le = !le;
                continue;
            }
            start = false;
            if (!u) {
                if (!tx_nul(o)) return;
                start = enc == ENC_UTF16;
                continue;
            }
            if (u >= 0xD800 && u < 0xDC00) { /* a pair */
                if (i + 1 >= n) {
                    if (cut) return;
                    u = 0xFFFD;
                } else {
                    uint32_t v = le ? (uint32_t)p[i + 1] << 8 | p[i] : g16(p + i);
                    if (v >= 0xDC00 && v < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00), i += 2;
                    else u = 0xFFFD;
                }
            } else if (u >= 0xDC00 && u < 0xE000) u = 0xFFFD;
            tx_cp(o, u);
        }
    } else {
        while (i < n && !o->full) {
            uint8_t b = p[i];
            if (b < 0x80) {
                if (!b && !tx_nul(o)) return;
                if (b) tx_cp(o, b);
                i++;
                continue;
            }
            size_t k;
            uint32_t cp;
            uint8_t lo = 0x80, hi = 0xBF; /* (overlong forms, surrogates and past U+10FFFF are invalid) */
            if (b >= 0xC2 && b <= 0xDF) k = 1, cp = b & 0x1Fu;
            else if (b >= 0xE0 && b <= 0xEF) k = 2, cp = b & 0x0Fu, lo = b == 0xE0 ? 0xA0 : 0x80, hi = b == 0xED ? 0x9F : 0xBF;
            else if (b >= 0xF0 && b <= 0xF4) k = 3, cp = b & 0x07u, lo = b == 0xF0 ? 0x90 : 0x80, hi = b == 0xF4 ? 0x8F : 0xBF;
            else {
                tx_cp(o, 0xFFFD);
                i++;
                continue;
            }
            size_t j = 1;
            for (; j <= k && i + j < n; j++) {
                uint8_t x = p[i + j];
                if (x < (j == 1 ? lo : 0x80) || x > (j == 1 ? hi : 0xBF)) break;
                cp = cp << 6 | (x & 0x3Fu);
            }
            if (j > k) tx_cp(o, cp);
            else if (i + j >= n && cut) return;
            else tx_cp(o, 0xFFFD); /* the lead byte and the good ones after it */
            i += j;
        }
    }
}

/* A field takes text only while it's empty: the first tag that says it wins */
static void set_text(char *f, size_t cap, const uint8_t *p, size_t n, int enc, int mode, bool cut) {
    if (f[0]) return;
    Tx o = tx_open(f, cap, mode);
    tx_put(&o, p, n, enc, cut);
}

/* The description: kept when better than the one found so far (rank) and not empty */
static void set_desc(Cx *c, int rank, const uint8_t *p, size_t n, int enc, bool cut) {
    if (rank <= c->desc_rank) return;
    char *d = malloc(AT_DESC_MAX);
    if (!d) return;
    Tx o = tx_open(d, AT_DESC_MAX, TX_MULTI);
    tx_put(&o, p, n, enc, cut);
    if (!o.len) {
        free(d);
        return;
    }
    char *s = realloc(d, o.len + 1);
    free(c->t->description);
    c->t->description = s ? s : d;
    c->desc_rank = rank;
}

static bool digit(char ch) { return ch >= '0' && ch <= '9'; }

/* The first run of four digits or more: "2025", "2025-11-04", "04-Nov-2025" */
static int year_of(const char *s) {
    for (const char *p = s; *p; p++) {
        const char *q = p;
        while (digit(*q)) q++;
        if (q - p >= 4) return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        if (q > p) p = q - 1;
    }
    return 0;
}

/* "3/12", "3", "03 / 12" */
static void num_pair(const char *s, int *a, int *b) {
    int x = 0, y = 0, k = 0;
    while (*s == ' ') s++;
    for (; digit(*s); s++)
        if (k++ < 6) x = x * 10 + (*s - '0');
    while (*s == ' ') s++;
    if (*s == '/')
        for (s++, k = 0; *s == ' ' || digit(*s); s++)
            if (digit(*s) && k++ < 6) y = y * 10 + (*s - '0');
    if (x && !*a) *a = x, *b = y;
}

/* ID3v1's genres (Winamp's list), which ID3v2's TCON "(101)" and MP4's gnre (+1) number */
static const char *const GENRES[192] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age",
    "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult",
    "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro",
    "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop",
    "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
    "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson",
    "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus", "", "Satire", "Slow Jam", "Club", "Tango",
    "Samba", "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo",
    "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror", "Indie",
    "Britpop", "", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal", "Black Metal", "Crossover",
    "Contemporary Christian", "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "J-Pop", "Synthpop",
    "Abstract", "Art Rock", "Baroque", "Bhangra", "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM",
    "Eclectic", "Electro", "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient",
    "Industro-Goth", "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz",
    "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music", "Neoclassical",
    "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock", "G-Funk", "Dubstep", "Garage Rock",
    "Psybient"};

static void genre_number(AudioTags *t, uint32_t n) {
    if (!t->genre[0] && n < 192) copy_str(t->genre, sizeof t->genre, GENRES[n]);
}

/* TCON: "Audiobook", "(183)", "(183)Audiobooks", "183" */
static void genre_text(AudioTags *t, const char *s) {
    if (t->genre[0]) return;
    bool paren = *s == '(';
    const char *p = s + paren;
    uint32_t n = 0;
    int k = 0;
    for (; digit(*p) && k < 4; p++, k++) n = n * 10 + (uint32_t)(*p - '0');
    if (k && (paren ? *p == ')' : !*p)) {
        const char *rest = p + paren;
        while (*rest == ' ') rest++;
        if (!*rest || *rest == '(') { /* a number alone */
            genre_number(t, n);
            return;
        }
        s = rest; /* "(183)Audiobooks": its own words */
    }
    size_t len = strlen(s); /* (valid UTF-8 already: cut at a whole character, no blank at either end) */
    if (len >= sizeof t->genre) {
        len = sizeof t->genre - 1;
        while (len && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
    }
    while (len && s[len - 1] == ' ') len--;
    memcpy(t->genre, s, len);
    t->genre[len] = 0;
}

/* -------------------------------------------------------------- chapters */

static void add_chapter(Cx *c, int64_t ms, const char *title) {
    if (c->nch >= AT_MAX_CHAPTERS) return; /* more are left out */
    if (c->nch == c->chcap) {
        int cap = c->chcap ? c->chcap * 2 : 32;
        if (cap > AT_MAX_CHAPTERS) cap = AT_MAX_CHAPTERS;
        AtChapter *a = realloc(c->ch, (size_t)cap * sizeof *a);
        if (!a) return;
        c->ch = a, c->chcap = cap;
    }
    AtChapter *a = &c->ch[c->nch++];
    a->start_ms = ms < 0 ? 0 : ms;
    memcpy(a->title, title, strlen(title) + 1); /* (made by a Tx of AT_CHAPTER_TEXT bytes) */
}

typedef struct {
    int64_t start;
    int i;
} Ord;

static int ord_cmp(const void *a, const void *b) { /* by start, then as found: a stable order */
    const Ord *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start ? 1 : x->i - y->i;
}

static void chapters_done(Cx *c) {
    int n = c->nch;
    Ord *o = n ? malloc((size_t)n * sizeof *o) : NULL;
    AtChapter *s = n ? malloc((size_t)n * sizeof *s) : NULL;
    if (o && s) {
        for (int i = 0; i < n; i++) o[i] = (Ord){c->ch[i].start_ms, i};
        qsort(o, (size_t)n, sizeof *o, ord_cmp);
        for (int i = 0; i < n; i++) s[i] = c->ch[o[i].i];
        c->t->chapters = s, c->t->nchapters = n;
        s = NULL;
    }
    free(o);
    free(s);
    free(c->ch);
    c->ch = NULL;
}

/* ------------------------------------------------------------------- MP4 */

typedef struct {
    int64_t data, end; /* the payload, the end */
    char type[4];
} Box;

typedef struct {
    int64_t off, len; /* a box's payload */
} Run;

/* The box at *pos (inside one ending at end); *pos moves past it. One claiming more than its parent holds is
   taken as far as the parent goes (a cut file); size 0 = to the parent's end. */
static bool box_next(const Cx *c, int64_t *pos, int64_t end, Box *b) {
    uint8_t h[16];
    if (*pos < 0 || end - *pos < 8 || !rd(c, *pos, h, 8)) return false;
    uint64_t size = g32(h);
    int64_t hl = 8;
    if (size == 1) {
        if (end - *pos < 16 || !rd(c, *pos + 8, h + 8, 8)) return false;
        size = g64(h + 8), hl = 16;
    } else if (!size) size = (uint64_t)(end - *pos);
    if (size < (uint64_t)hl) return false;
    b->data = *pos + hl;
    b->end = size > (uint64_t)(end - *pos) ? end : *pos + (int64_t)size;
    memcpy(b->type, h + 4, 4);
    *pos = b->end;
    return true;
}

static bool kid(const Cx *c, const Box *in, const char *type, Box *out) {
    int64_t pos = in->data;
    Box b;
    for (int i = 0; i < KIDS_MAX && box_next(c, &pos, in->end, &b); i++)
        if (is4(b.type, type)) {
            *out = b;
            return true;
        }
    return false;
}

/* The first bytes of a box's payload, at most cap; -1 when they can't be read */
static int64_t load(const Cx *c, const Box *b, uint8_t *dst, size_t cap) {
    int64_t n = b->end - b->data;
    if ((uint64_t)n > cap) n = (int64_t)cap;
    return n >= 0 && rd(c, b->data, dst, (size_t)n) ? n : -1;
}

static Run run_of(const Box *b) { return (Run){b->data, b->end - b->data}; }

/* count entries of width bytes of a table, after its first skip bytes - as many as it holds; malloc'd */
static uint8_t *table(const Cx *c, Run r, int64_t skip, uint64_t count, size_t width, size_t *got) {
    *got = 0;
    if (r.len < skip) return NULL;
    uint64_t room = (uint64_t)(r.len - skip) / width;
    if (count > room) count = room;
    uint8_t *p = count ? malloc((size_t)count * width) : NULL;
    if (!p || !rd(c, r.off + skip, p, (size_t)count * width)) {
        free(p);
        return NULL;
    }
    *got = (size_t)count;
    return p;
}

/* A table's entry count (after its version and flags), at most cap; 0 when it can't be read */
static uint64_t count_of(const Cx *c, Run r, uint64_t cap) {
    uint8_t h[8];
    if (r.len < 8 || !rd(c, r.off, h, 8)) return 0;
    return g32(h + 4) < cap ? g32(h + 4) : cap;
}

static int64_t ms_of(uint64_t ticks, uint32_t scale) {
    if (!scale) return 0;
    uint64_t s = ticks / scale;
    if (s > (uint64_t)1 << 40) s = (uint64_t)1 << 40; /* (35,000 years: a damaged number) */
    return (int64_t)(s * 1000 + ticks % scale * 1000 / scale);
}

/* mvhd and mdhd: the timescale and the length (0 = unknown) */
static void head_times(const Cx *c, const Box *b, uint32_t *scale, uint64_t *dur) {
    uint8_t p[32];
    int64_t n = load(c, b, p, sizeof p);
    *scale = 0, *dur = 0;
    if (n >= 32 && p[0] == 1) *scale = g32(p + 20), *dur = g64(p + 24);
    else if (n >= 20 && p[0] == 0) *scale = g32(p + 12), *dur = g32(p + 16) == 0xFFFFFFFFu ? 0 : g32(p + 16);
    if (*dur == UINT64_MAX) *dur = 0;
}

typedef struct {
    uint32_t id, chap;   /* tkhd's track id; the first track its tref 'chap' names */
    char handler[4];     /* "soun", "text" ... */
    uint32_t scale;      /* mdhd */
    uint64_t dur;
    char entry[4];       /* the first sample description's type */
    int channels, rate, oti;
    uint64_t edit_empty; /* elst: an empty edit before it (movie ticks) */
    int64_t edit_from;   /* the media time shown first */
    Run stts, stsz, stsc, stco;
    bool co64;
} Trak;

/* An edit list as simple ones are: maybe an empty edit, then where the media starts */
static void edits(const Cx *c, const Box *b, Trak *k) {
    uint8_t p[8 + 2 * 20];
    int64_t n = load(c, b, p, sizeof p);
    if (n < 8) return;
    bool v1 = p[0] == 1;
    size_t w = v1 ? 20 : 12;
    uint32_t count = g32(p + 4);
    for (uint32_t i = 0; i < count && i < 2 && 8 + (i + 1) * w <= (uint64_t)n; i++) {
        const uint8_t *e = p + 8 + i * w;
        uint64_t seg = v1 ? g64(e) : g32(e);
        int64_t from = v1 ? (int64_t)g64(e + 8) : (int64_t)(int32_t)g32(e + 4);
        if (from == -1) {
            if (!i) k->edit_empty = seg;
            continue;
        }
        k->edit_from = from < 0 ? 0 : from;
        break;
    }
}

/* A box among in-memory boxes */
static bool mem_kid(const uint8_t *p, size_t n, const char *type, const uint8_t **out, size_t *len) {
    for (size_t i = 0, k = 0; n - i >= 8 && k < 64; k++) {
        size_t s = g32(p + i);
        if (s < 8 || s > n - i) return false;
        if (is4(p + i + 4, type)) {
            *out = p + i + 8, *len = s - 8;
            return true;
        }
        i += s;
    }
    return false;
}

/* An MPEG-4 descriptor's tag and length (up to 4 bytes of 7 bits) at *i; false past the end */
static bool es_desc(const uint8_t *p, size_t n, size_t *i, int *tag, size_t *len) {
    if (*i >= n) return false;
    *tag = p[(*i)++];
    *len = 0;
    for (int k = 0; k < 4; k++) {
        if (*i >= n) return false;
        uint8_t b = p[(*i)++];
        *len = *len << 7 | (b & 0x7Fu);
        if (!(b & 0x80)) break;
    }
    return true;
}

typedef struct {
    const uint8_t *p;
    size_t n, pos; /* bits */
} Bits;

static uint32_t bits_get(Bits *b, int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; i++, b->pos++)
        v = v << 1 | (b->pos < b->n * 8 ? (uint32_t)(b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1) : 0);
    return v;
}

/* AudioSpecificConfig: the rate and channels it decodes to (an SBR extension's rate when it signals one) */
static void asc_read(const uint8_t *p, size_t n, Trak *k) {
    static const int RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    Bits b = {p, n, 0};
    uint32_t aot = bits_get(&b, 5);
    if (aot == 31) aot = 32 + bits_get(&b, 6);
    uint32_t fi = bits_get(&b, 4);
    uint32_t rate = fi == 15 ? bits_get(&b, 24) : fi < 13 ? (uint32_t)RATES[fi] : 0;
    uint32_t ch = bits_get(&b, 4);
    if (aot == 5 || aot == 29) {
        fi = bits_get(&b, 4);
        rate = fi == 15 ? bits_get(&b, 24) : fi < 13 ? (uint32_t)RATES[fi] : rate;
    }
    if (b.pos > n * 8) return;
    if (rate && rate <= 768000) k->rate = (int)rate;
    if (ch >= 1 && ch <= 6) k->channels = (int)ch;
    else if (ch == 7) k->channels = 8;
}

static void esds_read(const uint8_t *p, size_t n, Trak *k) {
    size_t i = 4, len; /* (version and flags) */
    int tag;
    if (!es_desc(p, n, &i, &tag, &len) || tag != 3 || n - i < 3) return;
    i += 2; /* ES_ID */
    uint8_t fl = p[i++];
    if (fl & 0x80) i += 2;
    if ((fl & 0x40) && i < n) i += 1 + (size_t)p[i];
    if (fl & 0x20) i += 2;
    if (i >= n || !es_desc(p, n, &i, &tag, &len) || tag != 4 || n - i < 13) return;
    k->oti = p[i];
    i += 13;
    if (es_desc(p, n, &i, &tag, &len) && tag == 5 && len <= n - i) asc_read(p + i, len, k);
}

static void sample_desc(Cx *c, const Box *b, Trak *k) {
    uint8_t *p = c->tmp;
    int64_t n = load(c, b, p, STSD_READ);
    if (n < 16 || !g32(p + 4)) return;
    memcpy(k->entry, p + 12, 4);
    uint64_t es = g32(p + 8);
    if (es < 8 || !is4(k->handler, "soun")) return;
    size_t len = es - 8 < (uint64_t)n - 16 ? (size_t)(es - 8) : (size_t)n - 16;
    const uint8_t *e = p + 16, *x;
    if (len < 28) return;
    uint32_t ver = g16(e + 8);
    size_t kids = ver == 1 ? 44 : ver == 2 ? 64 : 28, xn;
    k->channels = (int)g16(e + 16), k->rate = (int)g16(e + 24); /* (the rate's whole part of 16.16) */
    if (ver == 2 && len >= 44) {                                /* QuickTime's version 2: a double, a count */
        uint64_t bits = g64(e + 32);
        double d;
        memcpy(&d, &bits, sizeof d);
        k->rate = d >= 1 && d <= 768000 ? (int)d : 0;
        k->channels = (int)(g32(e + 40) & 0xFFFF);
    }
    if (kids < len && (mem_kid(e + kids, len - kids, "esds", &x, &xn) ||
                       (mem_kid(e + kids, len - kids, "wave", &x, &xn) && mem_kid(x, xn, "esds", &x, &xn))))
        esds_read(x, xn, k);
    if (k->channels > 64) k->channels = 0;
}

static void trak_read(Cx *c, const Box *trak, Trak *k) {
    Box b, m, x, s;
    uint8_t p[24];
    memset(k, 0, sizeof *k);
    if (kid(c, trak, "tkhd", &b)) {
        int64_t n = load(c, &b, p, sizeof p);
        if (n >= 24 && p[0] == 1) k->id = g32(p + 20);
        else if (n >= 16 && p[0] == 0) k->id = g32(p + 12);
    }
    if (kid(c, trak, "tref", &b) && kid(c, &b, "chap", &x) && load(c, &x, p, 4) == 4) k->chap = g32(p);
    if (kid(c, trak, "edts", &b) && kid(c, &b, "elst", &x)) edits(c, &x, k);
    if (!kid(c, trak, "mdia", &m)) return;
    if (kid(c, &m, "mdhd", &b)) head_times(c, &b, &k->scale, &k->dur);
    if (kid(c, &m, "hdlr", &b) && load(c, &b, p, 12) == 12) memcpy(k->handler, p + 8, 4);
    if (!kid(c, &m, "minf", &x) || !kid(c, &x, "stbl", &s)) return;
    if (kid(c, &s, "stsd", &b)) sample_desc(c, &b, k);
    if (kid(c, &s, "stts", &b)) k->stts = run_of(&b);
    if (kid(c, &s, "stsz", &b)) k->stsz = run_of(&b);
    if (kid(c, &s, "stsc", &b)) k->stsc = run_of(&b);
    if (kid(c, &s, "stco", &b)) k->stco = run_of(&b);
    else if (kid(c, &s, "co64", &b)) k->stco = run_of(&b), k->co64 = true;
}

/* A QuickTime chapter track: a text sample per chapter (a 16-bit length, then UTF-8 or UTF-16 with a BOM), its
   start the sum of the samples' durations before it - only the first AT_MAX_CHAPTERS entries are ever read. */
static void qt_chapters(Cx *c, const Trak *k, uint32_t movie_scale) {
    uint8_t h[12];
    if (!k->scale || k->stsz.len < 12 || !rd(c, k->stsz.off, h, 12)) return;
    uint32_t fixed = g32(h + 4);
    uint64_t want = g32(h + 8), acc = 0, *tick = NULL, *off = NULL;
    if (want > AT_MAX_CHAPTERS) want = AT_MAX_CHAPTERS;
    size_t n = 0, nt = 0, nsc = 0, nco = 0, got = 0;
    uint8_t *sz = fixed ? NULL : table(c, k->stsz, 12, want, 4, &n), *tt = NULL, *sc = NULL, *co = NULL;
    uint32_t *size = NULL;
    int64_t shift = ms_of(k->edit_empty, movie_scale) - ms_of((uint64_t)k->edit_from, k->scale);
    if (fixed) n = (size_t)want;
    if (n && (tt = table(c, k->stts, 8, count_of(c, k->stts, STTS_MAX), 8, &nt)) && (tick = malloc(n * sizeof *tick)) &&
        (off = malloc(n * sizeof *off)) && (size = malloc(n * sizeof *size))) {
        for (size_t i = 0; i < n; i++) size[i] = fixed ? fixed : g32(sz + 4 * i);
        /* times: every sample's start (a count of 2^32 stops at the chapters wanted) */
        for (size_t e = 0; e < nt && got < n; e++)
            for (uint32_t j = 0, cnt = g32(tt + 8 * e), d = g32(tt + 8 * e + 4); j < cnt && got < n; j++)
                tick[got++] = acc, acc += d;
    }
    n = got, got = 0;
    /* places: the chunks (stco/co64) and how many samples each holds (stsc) */
    if (n && (co = table(c, k->stco, 8, count_of(c, k->stco, n), k->co64 ? 8 : 4, &nco)) &&
        (sc = table(c, k->stsc, 8, count_of(c, k->stsc, nco), 12, &nsc)))
        for (size_t ch = 0, e = 0; ch < nco && got < n; ch++) {
            while (e + 1 < nsc && g32(sc + 12 * (e + 1)) <= ch + 1) e++;
            uint64_t at = k->co64 ? g64(co + 8 * ch) : g32(co + 4 * ch);
            for (uint32_t j = 0, spc = g32(sc + 12 * e + 4); j < spc && got < n; j++)
                off[got] = at, at += size[got], got++;
        }
    for (size_t i = 0; i < got; i++) {
        char title[AT_CHAPTER_TEXT] = "";
        uint8_t b[2 + SAMPLE_READ];
        size_t take = size[i] < sizeof b ? size[i] : sizeof b;
        if (take >= 2 && off[i] <= INT64_MAX && rd(c, (int64_t)off[i], b, take)) {
            size_t tl = g16(b), have;
            if (tl > size[i] - 2) tl = size[i] - 2;
            have = tl < take - 2 ? tl : take - 2;
            bool bom = have >= 2 && ((b[2] == 0xFE && b[3] == 0xFF) || (b[2] == 0xFF && b[3] == 0xFE));
            Tx o = tx_open(title, sizeof title, TX_LINE);
            tx_put(&o, b + 2, have, bom ? ENC_UTF16 : ENC_UTF8, have < tl);
        }
        add_chapter(c, ms_of(tick[i], k->scale) + shift, title);
    }
    free(sz), free(tt), free(sc), free(co), free(tick), free(off), free(size);
}

/* Nero's chapters: version, flags, then (version 1) four bytes and a count of one byte - or, as mp4v2 writes it,
   one byte and a 32-bit count: the same bytes below 256; then each: a start in 100 ns, a length byte, the title */
static void chpl_read(Cx *c, const Box *b) {
    int64_t len = b->end - b->data;
    size_t n = len < CHPL_READ ? (size_t)(len > 0 ? len : 0) : CHPL_READ;
    uint8_t *p = n >= 5 ? malloc(n) : NULL;
    if (!p || !rd(c, b->data, p, n) || (p[0] && n < 9)) {
        free(p);
        return;
    }
    uint64_t count = p[0] ? g32(p + 5) : p[4];
    size_t i = p[0] ? 9 : 5;
    for (uint64_t k = 0; k < count && k < AT_MAX_CHAPTERS && n - i >= 9; k++) {
        uint64_t start = g64(p + i) / 10000;
        size_t tl = p[i + 8];
        i += 9;
        bool cut = tl > n - i;
        if (cut) tl = n - i;
        char title[AT_CHAPTER_TEXT];
        Tx o = tx_open(title, sizeof title, TX_LINE);
        tx_put(&o, p + i, tl, ENC_UTF8, cut);
        add_chapter(c, start > (uint64_t)1 << 50 ? (int64_t)1 << 50 : (int64_t)start, title);
        i += tl;
    }
    free(p);
}

static bool cover_at(Cx *c, int rank, int64_t off, int64_t len, const uint8_t *m, size_t mn) {
    const char *type = mn >= 2 && m[0] == 0xFF && m[1] == 0xD8                ? "image/jpeg"
                       : mn >= 4 && m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G' ? "image/png"
                                                                                : NULL;
    if (!type || len <= 0 || rank <= c->cover_rank) return false;
    c->t->cover_off = off, c->t->cover_len = len;
    copy_str(c->t->cover_type, sizeof c->t->cover_type, type);
    c->cover_rank = rank;
    return true;
}

enum {
    I_TITLE = 1, I_ARTIST, I_AARTIST, I_ALBUM, I_NARR, I_YEAR, I_GENRE, I_GNRE, I_TRKN, I_DISK, I_COVR,
    I_LDES, I_DESC, I_DES, I_CMT /* descriptions, the best first */
};

static const struct {
    char id[5];
    int what;
} ITEMS[] = {
    {"\251nam", I_TITLE}, {"\251ART", I_ARTIST}, {"aART", I_AARTIST}, {"\251alb", I_ALBUM}, {"\251nrt", I_NARR},
    {"\251day", I_YEAR},  {"\251gen", I_GENRE},  {"gnre", I_GNRE},    {"trkn", I_TRKN},    {"disk", I_DISK},
    {"covr", I_COVR},     {"ldes", I_LDES},      {"desc", I_DESC},    {"\251des", I_DES},  {"\251cmt", I_CMT},
};

/* iTunes' tags: each an item box holding a 'data' box - a type (1 = UTF-8, 2 = UTF-16, 13 = JPEG, 14 = PNG,
   0 = numbers), a locale, the value. Custom '----' items and anything else are passed over unread. */
static void ilst_read(Cx *c, const Box *ilst) {
    AudioTags *t = c->t;
    int64_t pos = ilst->data;
    Box it, d;
    for (int i = 0; i < KIDS_MAX && box_next(c, &pos, ilst->end, &it); i++) {
        int what = 0;
        for (size_t k = 0; k < sizeof ITEMS / sizeof ITEMS[0]; k++)
            if (is4(it.type, ITEMS[k].id)) what = ITEMS[k].what;
        if (what == I_COVR) { /* the first picture: located, not read */
            int64_t at = it.data;
            uint8_t p[16];
            for (int k = 0; k < 16 && box_next(c, &at, it.end, &d); k++)
                if (is4(d.type, "data") && load(c, &d, p, 16) == 16 && cover_at(c, 1, d.data + 8, d.end - d.data - 8, p + 8, 8))
                    break;
            continue;
        }
        if (!what || !kid(c, &it, "data", &d)) continue;
        uint8_t *p = c->tmp;
        int64_t n = load(c, &d, p, TXT_READ);
        if (n < 8) continue;
        const uint8_t *v = p + 8;
        size_t vn = (size_t)n - 8;
        bool cut = d.end - d.data > n;
        int enc = (g32(p) & 0xFFFFFF) == 2 ? ENC_UTF16BE : ENC_UTF8;
        char s[64];
        Tx o;
        switch (what) {
        case I_TITLE: set_text(t->title, sizeof t->title, v, vn, enc, TX_LINE, cut); break;
        case I_ARTIST: set_text(t->artist, sizeof t->artist, v, vn, enc, TX_LINE, cut); break;
        case I_AARTIST: set_text(t->album_artist, sizeof t->album_artist, v, vn, enc, TX_LINE, cut); break;
        case I_ALBUM: set_text(t->album, sizeof t->album, v, vn, enc, TX_LINE, cut); break;
        case I_NARR: set_text(t->narrator, sizeof t->narrator, v, vn, enc, TX_LINE, cut); break;
        case I_GENRE: set_text(t->genre, sizeof t->genre, v, vn, enc, TX_LINE, cut); break;
        case I_GNRE:
            if (vn >= 2 && g16(v)) genre_number(t, g16(v) - 1);
            break;
        case I_YEAR:
            o = tx_open(s, sizeof s, TX_LINE);
            tx_put(&o, v, vn, enc, cut);
            if (!t->year) t->year = year_of(s);
            break;
        case I_TRKN:
        case I_DISK: { /* two zero bytes, the number, the total */
            int *a = what == I_TRKN ? &t->track : &t->disc, *b = what == I_TRKN ? &t->tracks : &t->discs;
            if (vn >= 4 && !*a) *a = (int)g16(v + 2), *b = vn >= 6 ? (int)g16(v + 4) : 0;
            break;
        }
        default: set_desc(c, I_CMT + 1 - what, v, vn, enc, cut); /* ldes, then desc, then ©des, then ©cmt */
        }
    }
}

static bool type_ok(const uint8_t *p) {
    for (int i = 0; i < 4; i++)
        if ((p[i] < 0x20 || p[i] > 0x7E) && p[i] != 0xA9) return false;
    return true;
}

static void meta_read(Cx *c, const Box *meta) {
    uint8_t p[8];
    Box m = *meta, il;
    if (load(c, &m, p, 8) < 8) return;
    if (!(g32(p) >= 8 && type_ok(p + 4))) m.data += 4; /* a full box: version and flags first (QuickTime's has none) */
    if (m.data <= m.end && kid(c, &m, "ilst", &il)) ilst_read(c, &il);
}

static void codec_name(const Trak *k, char *out, size_t cap) {
    static const struct {
        char cc[5];
        const char *name;
    } MAP[] = {{"mp4a", "aac"},  {"ac-3", "ac3"},  {"ec-3", "eac3"}, {"Opus", "opus"}, {"opus", "opus"},
               {"fLaC", "flac"}, {"alac", "alac"}, {".mp3", "mp3"},  {"samr", "amr"}};
    const char *name = NULL;
    for (size_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (is4(k->entry, MAP[i].cc)) name = MAP[i].name;
    if (is4(k->entry, "mp4a"))
        name = k->oti == 0x69 || k->oti == 0x6B ? "mp3"
               : k->oti == 0xA5               ? "ac3"
               : k->oti == 0xA6               ? "eac3"
               : k->oti == 0xA9               ? "dts"
               : k->oti == 0xAD               ? "opus"
                                              : "aac";
    if (name) {
        copy_str(out, cap, name);
        return;
    }
    size_t n = 0;
    for (int i = 0; i < 4 && n + 1 < cap; i++) {
        char ch = k->entry[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || digit(ch)) out[n++] = ch;
    }
    out[n] = 0;
}

static void moov_read(Cx *c, const Box *moov) {
    AudioTags *t = c->t;
    Trak *tr = calloc(TRAKS_MAX, sizeof *tr);
    int nt = 0;
    uint32_t mscale = 0;
    uint64_t mdur = 0;
    Box b, udta = {0}, meta = {0}, x;
    int64_t pos = moov->data;
    if (!tr) return;
    for (int i = 0; i < KIDS_MAX && box_next(c, &pos, moov->end, &b); i++) {
        if (is4(b.type, "mvhd")) head_times(c, &b, &mscale, &mdur);
        else if (is4(b.type, "trak") && nt < TRAKS_MAX) trak_read(c, &b, &tr[nt++]);
        else if (is4(b.type, "udta") && !udta.end) udta = b;
        else if (is4(b.type, "meta") && !meta.end) meta = b;
    }
    const Trak *snd = NULL;
    for (int i = 0; i < nt && !snd; i++)
        if (is4(tr[i].handler, "soun")) snd = &tr[i];
    if (snd) {
        codec_name(snd, t->codec, sizeof t->codec);
        t->channels = snd->channels;
        t->sample_rate = snd->rate ? snd->rate : snd->scale <= 768000 ? (int)snd->scale : 0;
    }
    t->duration_ms = ms_of(mdur, mscale);
    if (!t->duration_ms && snd) t->duration_ms = ms_of(snd->dur, snd->scale);
    for (int i = 0; snd && snd->chap && i < nt && !c->nch; i++)
        if (tr[i].id == snd->chap && &tr[i] != snd) qt_chapters(c, &tr[i], mscale);
    if (udta.end && !c->nch && kid(c, &udta, "chpl", &x)) chpl_read(c, &x);
    if (udta.end && kid(c, &udta, "meta", &x)) meta_read(c, &x);
    if (meta.end) meta_read(c, &meta);
    free(tr);
}

/* An MP4 starts with ftyp - or, from old QuickTime, with one of its other top-level boxes and has a moov */
static bool mp4_read(Cx *c) {
    static const char *const TOP[] = {"moov", "mdat", "free", "skip", "wide", "pnot"};
    int64_t pos = 0;
    Box b;
    bool ftyp = false;
    for (int i = 0; i < KIDS_MAX && box_next(c, &pos, c->r->size, &b); i++) {
        if (is4(b.type, "moov")) {
            moov_read(c, &b);
            return true;
        }
        bool top = is4(b.type, "ftyp");
        for (size_t k = 0; k < sizeof TOP / sizeof TOP[0]; k++) top = top || is4(b.type, TOP[k]);
        if (!i && !top) return false;
        if (!i) ftyp = is4(b.type, "ftyp");
    }
    return ftyp;
}

/* ------------------------------------------------------------------- MP3 */

/* An ID3v2 tag's bytes, read on, its unsynchronisation (a 0 after each FF) undone on the way */
typedef struct {
    const Cx *c;
    int64_t pos, end, left; /* the next byte in the file, the end there; bytes it may still give */
    bool unsync, ff;
    int64_t dropped; /* zeros taken out */
    size_t at, n;
    uint8_t buf[2048];
} Src;

static void src_open(Src *s, const Cx *c, int64_t from, int64_t to, bool unsync) {
    s->c = c, s->pos = from, s->end = to, s->left = INT64_MAX;
    s->unsync = unsync, s->ff = false, s->dropped = 0, s->at = s->n = 0;
}

/* The next n bytes into dst (NULL: skipped); fewer at the end */
static int64_t src_get(Src *s, uint8_t *dst, int64_t n) {
    int64_t got = 0;
    if (n > s->left) n = s->left;
    while (got < n) {
        if (s->at == s->n) {
            int64_t k = s->end - s->pos;
            if (!dst && !s->unsync) { /* plain bytes skipped: unread */
                if (k > n - got) k = n - got;
                if (k > 0) s->pos += k, got += k;
                break;
            }
            if (k > (int64_t)sizeof s->buf) k = sizeof s->buf;
            if (k <= 0 || !rd(s->c, s->pos, s->buf, (size_t)k)) break;
            s->pos += k, s->at = 0, s->n = (size_t)k;
        }
        uint8_t b = s->buf[s->at++];
        if (s->unsync && s->ff && !b) {
            s->ff = false, s->dropped++;
            continue;
        }
        s->ff = b == 0xFF;
        if (dst) dst[got] = b;
        got++;
    }
    s->left -= got;
    return got;
}

static int64_t src_raw(const Src *s) { return s->pos - (int64_t)(s->n - s->at); }

static bool id_ok(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++)
        if (!((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) return false;
    return true;
}

/* The bytes of a string up to and with its terminator (one 0, or two at an even place in UTF-16) */
static size_t str_end(const uint8_t *p, size_t n, int enc) {
    if (enc == ENC_UTF16 || enc == ENC_UTF16BE) {
        for (size_t i = 0; i + 1 < n; i += 2)
            if (!p[i] && !p[i + 1]) return i + 2;
        return n;
    }
    const uint8_t *z = memchr(p, 0, n);
    return z ? (size_t)(z - p) + 1 : n;
}

/* Undoes unsynchronisation in memory; returns the new length */
static size_t unsync_mem(uint8_t *p, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!(i && p[i - 1] == 0xFF && !p[i])) p[k++] = p[i]; /* (p[i - 1] is read before it's written over) */
    return k;
}

/* APIC (v2.2 PIC): encoding, MIME type ("JPG" in v2.2), picture type, description, the picture. Its place in
   the file is kept only when unsynchronisation changed none of its bytes. */
static void apic(Cx *c, int ver, Src *f) {
    Src start = *f, g;
    uint8_t *p = c->tmp;
    int64_t n = src_get(f, p, ID3_HEAD_READ);
    if (n < 4 || p[0] > 3) return;
    const uint8_t *z = memchr(p + 1, 0, (size_t)n - 1); /* (the MIME type's end) */
    size_t i = ver == 2 ? 4 : z ? (size_t)(z - p) + 1 : (size_t)n;
    if (i >= (size_t)n) return;
    int rank = p[i++] == 3 ? 3 : 2; /* the front cover, else any picture */
    i += str_end(p + i, (size_t)n - i, p[0]);
    if (i + 4 > (size_t)n || rank <= c->cover_rank) return;
    g = start;
    if (src_get(&g, NULL, (int64_t)i) != (int64_t)i) return;
    int64_t raw = src_raw(&g), dropped = g.dropped, len = src_get(&g, NULL, INT64_MAX);
    if (g.dropped == dropped && src_raw(&g) - raw == len) cover_at(c, rank, raw, len, p + i, (size_t)n - i);
}

/* CHAP: an element id, start and end (ms), start and end offsets, then frames of its own - its TIT2 names it */
static void chap(Cx *c, int ver, Src *f) {
    uint8_t *p = c->tmp;
    size_t n = (size_t)src_get(f, p, ID3_HEAD_READ);
    const uint8_t *z = memchr(p, 0, n);
    if (!z || (size_t)(z - p) + 17 > n) return;
    size_t i = (size_t)(z - p) + 1;
    uint32_t start = g32(p + i);
    i += 16;
    char title[AT_CHAPTER_TEXT] = "";
    for (int k = 0; k < 64 && n - i >= 10; k++) {
        uint8_t *x = p + i, fl = x[9];
        if (!id_ok(x, 4)) break;
        size_t size = ver == 4 ? gsafe(x + 4) : g32(x + 4), have = size < n - i - 10 ? size : n - i - 10;
        if (is4(x, "TIT2")) {
            uint8_t *d = x + 10;
            size_t dn = ver == 4 && (fl & 0x02) ? unsync_mem(d, have) : have, skip = 0;
            if (ver == 4) skip = (fl & 0x40 ? 1 : 0) + (fl & 0x01 ? 4 : 0);
            else skip = fl & 0x20 ? 1 : 0;
            if (!(fl & (ver == 4 ? 0x0C : 0xC0)) && dn > skip && d[skip] <= 3) {
                Tx o = tx_open(title, sizeof title, TX_LINE);
                tx_put(&o, d + skip + 1, dn - skip - 1, d[skip], have < size);
            }
            break;
        }
        if (size > n - i - 10) break;
        i += 10 + size;
    }
    add_chapter(c, start, title);
}

static void id3_frame(Cx *c, int ver, const char *id, Src *f) {
    static const char *const TEXT[] = {"TIT2", "TPE1", "TPE2", "TALB", "TYER", "TDRC", "TRCK", "TPOS", "TCON", "COMM", "TXXX"};
    AudioTags *t = c->t;
    if (is4(id, "APIC")) {
        apic(c, ver, f);
        return;
    }
    if (is4(id, "CHAP")) {
        if (ver >= 3) chap(c, ver, f);
        return;
    }
    int what = -1;
    for (int k = 0; k < (int)(sizeof TEXT / sizeof TEXT[0]); k++)
        if (is4(id, TEXT[k])) what = k;
    uint8_t *p = c->tmp;
    int64_t n = what < 0 ? 0 : src_get(f, p, TXT_READ);
    if (n < 1 || p[0] > 3) return;
    int enc = p[0], mode = ver == 4 ? TX_JOIN : TX_LINE; /* (v2.4's values are NUL-separated; before, a NUL ends it) */
    const uint8_t *v = p + 1;
    size_t vn = (size_t)n - 1, dl;
    bool cut = n == TXT_READ;
    char s[96];
    Tx o = tx_open(s, sizeof s, mode);
    switch (what) {
    case 0: set_text(t->title, sizeof t->title, v, vn, enc, mode, cut); break;
    case 1: set_text(t->artist, sizeof t->artist, v, vn, enc, mode, cut); break;
    case 2: set_text(t->album_artist, sizeof t->album_artist, v, vn, enc, mode, cut); break;
    case 3: set_text(t->album, sizeof t->album, v, vn, enc, mode, cut); break;
    case 4:
    case 5:
        tx_put(&o, v, vn, enc, cut);
        if (!t->year) t->year = year_of(s);
        break;
    case 6:
    case 7:
        tx_put(&o, v, vn, enc, cut);
        num_pair(s, what == 6 ? &t->track : &t->disc, what == 6 ? &t->tracks : &t->discs);
        break;
    case 8:
        tx_put(&o, v, vn, enc, cut);
        genre_text(t, s);
        break;
    case 9: /* COMM: a language, a short description, the text - iTunes' own (iTunNORM ...) aren't comments */
        if (vn < 3) break;
        v += 3, vn -= 3, dl = str_end(v, vn, enc);
        o.mode = TX_LINE;
        tx_put(&o, v, dl, enc, false);
        if (strncmp(s, "iTun", 4)) set_desc(c, s[0] ? 1 : 2, v + dl, vn - dl, enc, cut);
        break;
    case 10: /* TXXX: a description, the value - a narrator here */
        dl = str_end(v, vn, enc);
        o.mode = TX_LINE;
        tx_put(&o, v, dl, enc, false);
        for (char *q = s; *q; q++)
            if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 'a' + 'A');
        if (!strcmp(s, "NARRATOR") || !strcmp(s, "NARRATEDBY") || !strcmp(s, "NARRATED BY"))
            set_text(t->narrator, sizeof t->narrator, v + dl, vn - dl, enc, mode, cut);
        break;
    }
}

/* A v2.4 frame's size is synchsafe, but iTunes once wrote plain numbers: the one a frame (or the end) follows */
static bool next_frame_ok(const Cx *c, int64_t at, int64_t end) {
    uint8_t x[4];
    if (at == end) return true;
    return at < end && end - at >= 4 && rd(c, at, x, 4) && (!x[0] || id_ok(x, 4));
}

static void id3v2_read(Cx *c, const uint8_t *h, int64_t end) {
    static const char V22[][2][5] = {{"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TP2", "TPE2"}, {"TAL", "TALB"},
                                     {"TYE", "TYER"}, {"TRK", "TRCK"}, {"TPA", "TPOS"}, {"TCO", "TCON"},
                                     {"COM", "COMM"}, {"PIC", "APIC"}, {"TXX", "TXXX"}};
    int ver = h[3];
    uint8_t fl = h[5], x[10];
    Src s, f;
    if (ver == 2 && (fl & 0x40)) return; /* v2.2's compression: never defined */
    if (ver == 4) {                    /* frame sizes are as stored; unsynchronisation is each frame's */
        int64_t pos = 10;
        if (fl & 0x40) pos += rd(c, 10, x, 4) ? gsafe(x) : end; /* an extended header, its size counting itself */
        for (int k = 0; k < ID3_FRAMES_MAX && end - pos >= 10 && rd(c, pos, x, 10) && id_ok(x, 4); k++) {
            int64_t safe = gsafe(x + 4), size = g32(x + 4);
            bool plain = (x[4] | x[5] | x[6] | x[7]) & 0x80; /* (not synchsafe at all) */
            if (!plain && (next_frame_ok(c, pos + 10 + safe, end) || !next_frame_ok(c, pos + 10 + size, end))) size = safe;
            if (size > end - pos - 10) break;
            src_open(&f, c, pos + 10, pos + 10 + size, (fl & 0x80) || (x[9] & 0x02));
            if (!(x[9] & 0x0C)) { /* not compressed or encrypted */
                src_get(&f, NULL, (x[9] & 0x40 ? 1 : 0) + (x[9] & 0x01 ? 4 : 0)); /* a group id, the data's length */
                id3_frame(c, 4, (const char *)x, &f);
            }
            pos += 10 + size;
        }
        return;
    }
    src_open(&s, c, 10, end, (fl & 0x80) != 0); /* v2.2 and v2.3: the whole tag unsynchronised */
    if (ver == 3 && (fl & 0x40) && src_get(&s, x, 4) == 4) src_get(&s, NULL, g32(x)); /* an extended header */
    int hl = ver == 2 ? 6 : 10;
    for (int k = 0; k < ID3_FRAMES_MAX && src_get(&s, x, hl) == hl && id_ok(x, ver == 2 ? 3 : 4); k++) {
        char id[5] = "";
        if (ver == 2) {
            for (size_t i = 0; i < sizeof V22 / sizeof V22[0]; i++)
                if (!memcmp(x, V22[i][0], 3)) memcpy(id, V22[i][1], 5);
        } else memcpy(id, x, 4);
        int64_t size = ver == 2 ? g24(x + 3) : g32(x + 4);
        uint8_t ff = ver == 3 ? x[9] : 0;
        f = s;
        f.left = size;
        if (id[0] && !(ff & 0xC0)) { /* not compressed or encrypted */
            src_get(&f, NULL, ff & 0x20 ? 1 : 0); /* a group id */
            id3_frame(c, ver, id, &f);
        }
        if (src_get(&s, NULL, size) != size) break;
    }
}

static void id3v1_read(Cx *c, const uint8_t *g) {
    AudioTags *t = c->t;
    char s[8];
    set_text(t->title, sizeof t->title, g + 3, 30, ENC_LATIN1, TX_LINE, false);
    set_text(t->artist, sizeof t->artist, g + 33, 30, ENC_LATIN1, TX_LINE, false);
    set_text(t->album, sizeof t->album, g + 63, 30, ENC_LATIN1, TX_LINE, false);
    Tx o = tx_open(s, sizeof s, TX_LINE);
    tx_put(&o, g + 93, 4, ENC_LATIN1, false);
    t->year = year_of(s);
    bool v11 = !g[125] && g[126]; /* ID3v1.1: the comment's last byte the track */
    set_desc(c, 0, g + 97, v11 ? 28 : 30, ENC_LATIN1, false);
    if (v11) t->track = g[126];
    genre_number(t, g[127]);
}

typedef struct {
    int ver, layer, kbps, rate, spf, len, side; /* ver 1, 2 or 25 (2.5); side: its side information's bytes */
    int channels;
} Mpa;

static bool mpa_head(const uint8_t *h, Mpa *m) {
    static const uint16_t KBPS[2][3][15] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
    static const int RATES[3] = {44100, 48000, 32000};
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    int v = h[1] >> 3 & 3, l = h[1] >> 1 & 3, bi = h[2] >> 4, ri = h[2] >> 2 & 3;
    if (v == 1 || !l || !bi || bi == 15 || ri == 3 || (h[3] & 3) == 2) return false;
    bool v1 = v == 3, mono = h[3] >> 6 == 3;
    m->ver = v1 ? 1 : v == 2 ? 2 : 25;
    m->layer = 4 - l;
    m->kbps = KBPS[!v1][m->layer - 1][bi];
    m->rate = RATES[ri] >> (v1 ? 0 : v == 2 ? 1 : 2);
    m->spf = m->layer == 1 ? 384 : m->layer == 2 || v1 ? 1152 : 576;
    int pad = h[2] >> 1 & 1;
    m->len = m->layer == 1 ? (12 * m->kbps * 1000 / m->rate + pad) * 4 : m->spf / 8 * m->kbps * 1000 / m->rate + pad;
    m->side = v1 ? (mono ? 17 : 32) : mono ? 9 : 17;
    m->channels = mono ? 1 : 2;
    return true;
}

/* A frame at off that `follow` more of its kind follow (the same version, layer and rate) - or the end */
static bool mpa_at(const Cx *c, int64_t off, int64_t end, int follow, Mpa *m) {
    uint8_t h[4];
    Mpa a;
    if (!rd(c, off, h, 4) || !mpa_head(h, m)) return false;
    for (int k = 0; k < follow; k++) {
        off += k ? a.len : m->len;
        if (off == end) return true;
        if (!rd(c, off, h, 4) || !mpa_head(h, &a) || a.ver != m->ver || a.layer != m->layer || a.rate != m->rate) return false;
    }
    return true;
}

static bool mpa_find(const Cx *c, int64_t from, int64_t limit, int64_t end, int follow, int64_t *at, Mpa *m) {
    uint8_t *b = c->tmp;
    for (int64_t off = from; off < limit && end - off >= 4;) {
        size_t n = end - off < 4096 ? (size_t)(end - off) : 4096;
        if (!rd(c, off, b, n)) return false;
        for (size_t i = 0; i + 1 < n && off + (int64_t)i < limit; i++)
            if (b[i] == 0xFF && (b[i + 1] & 0xE0) == 0xE0 && mpa_at(c, off + (int64_t)i, end, follow, m)) {
                *at = off + (int64_t)i;
                return true;
            }
        off += (int64_t)n - 1;
    }
    return false;
}

static bool mp3_name(const char *s) {
    size_t n = s ? strlen(s) : 0;
    const char *e = n >= 4 ? s + n - 4 : "";
    return (e[0] == '.' && (e[1] | 0x20) == 'm' && (e[2] | 0x20) == 'p' && (e[3] == '3' || e[3] == '2')) ||
           (n >= 5 && !memcmp(s + n - 5, ".mpga", 5));
}

static bool mp3_read(Cx *c, const char *hint) {
    AudioTags *t = c->t;
    int64_t size = c->r->size, from = 0, first;
    uint8_t h[10], g[128];
    bool v2 = false, v1 = false, named = mp3_name(hint), found;
    if (rd(c, 0, h, 10) && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4 &&
        !((h[6] | h[7] | h[8] | h[9]) & 0x80)) {
        int64_t end = 10 + (int64_t)gsafe(h + 6);
        id3v2_read(c, h, end < size ? end : size);
        from = end + (h[3] == 4 && (h[5] & 0x10) ? 10 : 0); /* (a v2.4 footer) */
        v2 = true;
    }
    if (size - from >= 128 && rd(c, size - 128, g, 128) && !memcmp(g, "TAG", 3)) v1 = true;
    int64_t end = size - (v1 ? 128 : 0);
    Mpa m;
    /* frames: after the tag (or anywhere near the start of a file named .mp3), else right at the start, three */
    if (v2 || named) found = mpa_find(c, from, from + MPEG_SCAN, end, 1, &first, &m);
    else found = mpa_at(c, 0, end, 2, &m), first = 0;
    if (v1 && !v2 && found) id3v1_read(c, g);
    if (!found) return v2 && named;
    copy_str(t->codec, sizeof t->codec, m.layer == 3 ? "mp3" : m.layer == 2 ? "mp2" : "mp1");
    t->channels = m.channels, t->sample_rate = m.rate;
    /* the length: the frame count of a Xing/Info or VBRI header in the first frame, less the encoder's delay and
       padding a LAME tag after the Xing one gives; else the bytes over the bitrate */
    uint8_t f[192];
    size_t n = m.len < (int)sizeof f ? (size_t)m.len : sizeof f, x = 4 + (size_t)m.side;
    uint64_t samples = 0, skip = 0;
    if (end - first >= (int64_t)n && rd(c, first, f, n)) {
        uint32_t fl = x + 12 <= n ? g32(f + x + 4) : 0;
        if ((fl & 1) && (is4(f + x, "Xing") || is4(f + x, "Info"))) {
            samples = (uint64_t)g32(f + x + 8) * (uint64_t)m.spf;
            size_t l = x + 12 + (fl & 2 ? 4 : 0) + (fl & 4 ? 100 : 0) + (fl & 8 ? 4 : 0); /* bytes, TOC, quality */
            if (l + 24 <= n && (is4(f + l, "LAME") || is4(f + l, "Lavc") || is4(f + l, "Lavf")))
                skip = ((uint64_t)f[l + 21] << 4 | f[l + 22] >> 4) + ((uint64_t)(f[l + 22] & 15) << 8 | f[l + 23]);
        } else if (54 <= n && is4(f + 36, "VBRI")) samples = (uint64_t)g32(f + 50) * (uint64_t)m.spf;
    }
    if (skip < samples) samples -= skip;
    t->duration_ms = samples ? (int64_t)(samples * 1000 / (uint64_t)m.rate) : (end - first) * 8 / m.kbps;
    return true;
}

/* ---------------------------------------------------------------- the API */

bool audio_tags_read(const AtReader *r, const char *name_hint, AudioTags *out) {
    memset(out, 0, sizeof *out);
    if (!r || !r->read_at || r->size < 4) return false;
    Cx c = {r, out, -1, 0, malloc(TXT_READ + 16), NULL, 0, 0};
    uint8_t h[3];
    bool ok = false;
    if (!c.tmp || !rd(&c, 0, h, 3)) {
        free(c.tmp);
        return false;
    }
    if (memcmp(h, "ID3", 3) && mp4_read(&c)) ok = true, copy_str(out->format, sizeof out->format, "mp4");
    else if (mp3_read(&c, name_hint)) ok = true, copy_str(out->format, sizeof out->format, "mp3");
    chapters_done(&c);
    free(c.tmp);
    return ok;
}

void audio_tags_free(AudioTags *t) {
    if (!t) return;
    free(t->description);
    free(t->chapters);
    t->description = NULL;
    t->chapters = NULL;
    t->nchapters = 0;
}
