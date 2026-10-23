/* modules/av/subpic_test.c — picture subtitles written here: .sup files (display sets, epochs, palette-only
 * updates, two objects and cropping, objects over several segments, acquisition points) and .idx/.sub pairs
 * (both fields, packs of either MPEG, other substreams between them, custom colours, delays), opened,
 * indexed and drawn pixel for pixel; PNGs decoded back with an inflate of the test's own; then damaged
 * copies (cut short, lying sizes, flipped bytes, too big, too many).
 * HEMMA_TEST_SUP=<file.sup> or HEMMA_TEST_IDX=<file.idx> (its .sub beside it) opens a real one, and with
 * HEMMA_TEST_PNG=<folder> writes every cue there as a PNG to look at. HEMMA_FUZZ_RAW=<folder> writes
 * tools/avfuzz.c's seeds to <folder>/pgs and <folder>/vobsub. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "subpic.h"

#include <stdlib.h>

/* ------------------------------------------------------------ bytes, memory files, pictures */

static void b8(Buf *b, int v) { buf_putc(b, (char)(uint8_t)v); }
static void b16(Buf *b, unsigned v) {
    b8(b, (int)(v >> 8 & 255));
    b8(b, (int)(v & 255));
}
static void b24(Buf *b, uint32_t v) {
    b8(b, (int)(v >> 16 & 255));
    b16(b, v & 0xFFFF);
}
static void b32(Buf *b, uint32_t v) {
    b16(b, v >> 16);
    b16(b, v & 0xFFFF);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

typedef struct {
    const uint8_t *p;
    size_t n;
} Mem;

static bool mem_read(void *ctx, int64_t off, void *dst, size_t n) {
    const Mem *m = ctx;
    if (off < 0 || (uint64_t)off > m->n || n > m->n - (size_t)off) return false;
    if (n) memcpy(dst, m->p + off, n);
    return true;
}

static MkvReader mem(Mem *m, const void *p, size_t n) {
    m->p = p, m->n = n;
    return (MkvReader){m, (int64_t)n, mem_read};
}

static bool file_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

typedef struct {
    int w, h;
    uint8_t *px;
} Pic;

/* Test pictures: 0 = text-like (a clear band, a full band, runs of every length), 1 = noisy with 11 colours,
   2 = diagonal stripes of 1..3, 3 = noisy in 4 colours (DVD) */
static Pic pic(int w, int h, int kind) {
    Pic p = {w, h, xmalloc((size_t)w * (size_t)h)};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v;
            if (kind == 0) v = y < 3 ? 0 : y < 5 ? 1 : ((x + y * 7) / (1 + y % 7 * 11) + y / 4) % 4;
            else if (kind == 1) v = (x * 7 + y * 13 + (x * y) % 5 + (x / 9) * (y / 7)) % 11;
            else if (kind == 2) v = (x / 3 + y) % 3 + 1;
            else v = (x * 5 + y * 3 + (x * x + y) % 7) % 4;
            p.px[y * w + x] = (uint8_t)v;
        }
    return p;
}

/* Where a picture's part (cx, cy, cw, ch) shown at (x, y) on the frame lands in a cue's picture */
static void place(uint8_t *ex, const SubPicCue *c, const Pic *p, int x, int y, int cx, int cy, int cw, int ch) {
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
            int X = x + i - c->x, Y = y + j - c->y;
            if (X >= 0 && X < c->w && Y >= 0 && Y < c->h) ex[Y * c->w + X] = p->px[(cy + j) * p->w + cx + i];
        }
}

/* Cue i drawn: px (malloc'd, the caller frees), palette and colour count; NULL when it can't be */
static uint8_t *draw(SubPic *s, int i, uint32_t pal[256], int *nc) {
    const SubPicCue *c = subpic_cue(s, i);
    if (!c) return NULL;
    size_t n = (size_t)c->w * (size_t)c->h;
    uint8_t *px = xmalloc(n);
    if (!subpic_render(s, i, px, n, pal, nc)) {
        free(px);
        return NULL;
    }
    return px;
}

/* An RGBA entry within 2 of r/g/b (colour conversion rounding), alpha exactly */
static bool close_to(uint32_t got, int r, int g, int b, int a) {
    int R = (int)(got >> 24), G = (int)(got >> 16 & 255), B = (int)(got >> 8 & 255), A = (int)(got & 255);
    return abs(R - r) <= 2 && abs(G - g) <= 2 && abs(B - b) <= 2 && A == a;
}

/* Every cue in order, on the frame, drawable with every value under its colour count */
static bool sane(SubPic *s) {
    int fw, fh;
    subpic_frame(s, &fw, &fh);
    if (fw < 1 || fh < 1 || fw > SUBPIC_MAX_SIDE || fh > SUBPIC_MAX_SIDE) return false;
    int64_t prev = INT64_MIN;
    for (int i = 0; i < subpic_count(s); i++) {
        const SubPicCue *c = subpic_cue(s, i);
        if (!c || c->start_ms < prev || c->start_ms < 0 || c->end_ms <= c->start_ms || c->x < 0 || c->y < 0 ||
            c->w < 1 || c->h < 1 || c->x + c->w > fw || c->y + c->h > fh)
            return false;
        prev = c->start_ms;
        uint32_t pal[256];
        int nc = 0;
        uint8_t *px = draw(s, i, pal, &nc);
        if (px) {
            bool ok = nc >= 1 && nc <= 256;
            for (size_t k = 0; ok && k < (size_t)c->w * (size_t)c->h; k++) ok = px[k] < nc;
            free(px);
            if (!ok) return false;
        }
    }
    return subpic_cue(s, subpic_count(s)) == NULL && subpic_cue(s, -1) == NULL;
}

/* ------------------------------------------------------------ a .sup writer */

static void seg(Buf *b, uint32_t pts, int type, const Buf *pay) {
    buf_put(b, "PG", 2);
    b32(b, pts);
    b32(b, 0);
    b8(b, type);
    b16(b, (unsigned)pay->len);
    if (pay->len) buf_put(b, pay->p, pay->len);
}

typedef struct {
    int obj, x, y;
    bool crop;
    int cx, cy, cw, ch;
} TC;

/* A PCS: state 0x80 epoch start, 0x40 acquisition point, 0 normal */
static void pcs(Buf *b, uint32_t pts, int fw, int fh, int state, bool pal_only, int pal_id, const TC *c, int n) {
    Buf p = {0};
    b16(&p, (unsigned)fw), b16(&p, (unsigned)fh), b8(&p, 0x10), b16(&p, 0), b8(&p, state), b8(&p, pal_only ? 0x80 : 0);
    b8(&p, pal_id), b8(&p, n);
    for (int i = 0; i < n; i++) {
        b16(&p, (unsigned)c[i].obj), b8(&p, 0), b8(&p, c[i].crop ? 0x80 : 0), b16(&p, (unsigned)c[i].x), b16(&p, (unsigned)c[i].y);
        if (c[i].crop) b16(&p, (unsigned)c[i].cx), b16(&p, (unsigned)c[i].cy), b16(&p, (unsigned)c[i].cw), b16(&p, (unsigned)c[i].ch);
    }
    seg(b, pts, 0x16, &p);
    buf_free(&p);
}

static void wds(Buf *b, uint32_t pts, int fw, int fh) {
    Buf p = {0};
    b8(&p, 1), b8(&p, 0), b16(&p, 0), b16(&p, 0), b16(&p, (unsigned)fw), b16(&p, (unsigned)fh);
    seg(b, pts, 0x17, &p);
    buf_free(&p);
}

typedef struct {
    int id, y, cr, cb, a;
} TE;

static void pds(Buf *b, uint32_t pts, int pal_id, int ver, const TE *e, int n) {
    Buf p = {0};
    b8(&p, pal_id), b8(&p, ver);
    for (int i = 0; i < n; i++) b8(&p, e[i].id), b8(&p, e[i].y), b8(&p, e[i].cr), b8(&p, e[i].cb), b8(&p, e[i].a);
    seg(b, pts, 0x14, &p);
    buf_free(&p);
}

/* PGS RLE: lone pixels of colours 1-255 as themselves, the rest as 0x00-led codes (6- or 14-bit lengths) */
static void pgs_run(Buf *o, int c, int len) {
    while (len > 0) {
        int n = len > 16383 ? 16383 : len;
        if (c && n <= 2)
            for (int i = 0; i < n; i++) b8(o, c);
        else if (n < 64) {
            b8(o, 0), b8(o, (c ? 0x80 : 0) | n);
            if (c) b8(o, c);
        } else {
            b8(o, 0), b8(o, (c ? 0xC0 : 0x40) | n >> 8), b8(o, n & 255);
            if (c) b8(o, c);
        }
        len -= n;
    }
}

static void pgs_rle(Buf *o, const Pic *p) {
    for (int y = 0; y < p->h; y++) {
        const uint8_t *row = p->px + (size_t)y * (size_t)p->w;
        for (int x = 0; x < p->w;) {
            int k = 1;
            while (x + k < p->w && row[x + k] == row[x]) k++;
            pgs_run(o, row[x], k);
            x += k;
        }
        b8(o, 0), b8(o, 0);
    }
}

/* An object in `pieces` ODS segments (the first with the length and size) */
static void ods(Buf *b, uint32_t pts, int id, const Pic *p, int pieces) {
    Buf r = {0};
    pgs_rle(&r, p);
    size_t at = 0;
    for (int k = 0; k < pieces; k++) {
        size_t end = k == pieces - 1 ? r.len : r.len * (size_t)(k + 1) / (size_t)pieces;
        Buf s = {0};
        b16(&s, (unsigned)id), b8(&s, 0), b8(&s, (k == 0 ? 0x80 : 0) | (k == pieces - 1 ? 0x40 : 0));
        if (k == 0) b24(&s, (uint32_t)r.len + 4), b16(&s, (unsigned)p->w), b16(&s, (unsigned)p->h);
        buf_put(&s, r.p + at, end - at);
        at = end;
        seg(b, pts, 0x15, &s);
        buf_free(&s);
    }
    buf_free(&r);
}

static void end_seg(Buf *b, uint32_t pts) {
    Buf p = {0};
    seg(b, pts, 0x80, &p);
}

/* Offset of the k-th (0-based) segment of a type in a well-formed .sup, -1 none */
static int64_t seg_find_k(const Buf *b, int type, int k) {
    const uint8_t *p = (const uint8_t *)b->p;
    for (size_t at = 0; at + 13 <= b->len; at += 13 + (size_t)(p[at + 11] << 8 | p[at + 12]))
        if (p[at + 10] == type && k-- == 0) return (int64_t)at;
    return -1;
}

#define SEC(s) ((uint32_t)((s) * 90000))

/* Palette and epochs (720x480, BT.601): five cues - see subpic_pgs_palettes_and_epochs */
static void sup_epochs(Buf *b, Pic *A, Pic *B, Pic *C) {
    *A = pic(64, 16, 0), *B = pic(32, 8, 2), *C = pic(20, 10, 0);
    static const TE v0[] = {{0, 16, 128, 128, 0}, {1, 235, 128, 128, 255}, {2, 16, 128, 128, 255}, {3, 126, 128, 128, 200}};
    static const TE v1[] = {{1, 81, 240, 90, 255}}; /* BT.601 red */
    static const TE e2[] = {{0, 16, 128, 128, 0}, {1, 16, 128, 128, 255}, {2, 235, 128, 128, 255}};
    static const TE other[] = {{1, 145, 54, 34, 255}};
    TC a = {0, 300, 400, false, 0, 0, 0, 0}, ab[] = {{0, 300, 400, false, 0, 0, 0, 0}, {1, 100, 420, false, 0, 0, 0, 0}};
    TC c0[] = {{0, 10, 10, false, 0, 0, 0, 0}, {1, 50, 50, false, 0, 0, 0, 0}}, c1 = {1, 50, 50, false, 0, 0, 0, 0};
    /* 0.5 s: epoch start, palette 0, object 0 = A */
    pcs(b, SEC(0.5), 720, 480, 0x80, false, 0, &a, 1), wds(b, SEC(0.5), 720, 480), pds(b, SEC(0.5), 0, 0, v0, 4);
    ods(b, SEC(0.5), 0, A, 1), end_seg(b, SEC(0.5));
    /* 1 s: a palette-only update of entry 1 */
    pcs(b, SEC(1), 720, 480, 0, true, 0, &a, 1), pds(b, SEC(1), 0, 1, v1, 1), end_seg(b, SEC(1));
    /* 1.5 s: object 1 = B defined, shown with object 0 from the first display set; another palette */
    pcs(b, SEC(1.5), 720, 480, 0, false, 0, ab, 2), wds(b, SEC(1.5), 720, 480), pds(b, SEC(1.5), 1, 0, other, 1);
    ods(b, SEC(1.5), 1, B, 1), end_seg(b, SEC(1.5));
    /* 2 s: cleared */
    pcs(b, SEC(2), 720, 480, 0, false, 0, NULL, 0), wds(b, SEC(2), 720, 480), end_seg(b, SEC(2));
    /* 3 s: a new epoch: palette 0 anew (entry 3 gone), object 0 = C; object 1 is the old epoch's */
    pcs(b, SEC(3), 720, 480, 0x80, false, 0, c0, 2), wds(b, SEC(3), 720, 480), pds(b, SEC(3), 0, 0, e2, 3);
    ods(b, SEC(3), 0, C, 1), end_seg(b, SEC(3));
    /* 4 s: only object 1 - nothing this epoch knows: no cue, but it ends the one before */
    pcs(b, SEC(4), 720, 480, 0, false, 0, &c1, 1), end_seg(b, SEC(4));
    /* 6 s: object 0 (C) again, never cleared */
    pcs(b, SEC(6), 720, 480, 0, false, 0, c0, 1), end_seg(b, SEC(6));
}

/* One cue (1920x1080): 300x40 picture of the text kind at (100, 900), 1 s to 3.5 s */
static void sup_one(Buf *b, Pic *A) {
    *A = pic(300, 40, 0);
    static const TE pal[] = {{0, 16, 128, 128, 0}, {1, 235, 128, 128, 255}, {2, 16, 128, 128, 255}, {3, 63, 240, 102, 128}};
    TC a = {0, 100, 900, false, 0, 0, 0, 0};
    pcs(b, SEC(1), 1920, 1080, 0x80, false, 0, &a, 1), wds(b, SEC(1), 1920, 1080), pds(b, SEC(1), 0, 0, pal, 4);
    ods(b, SEC(1), 0, A, 1), end_seg(b, SEC(1));
    pcs(b, SEC(3.5), 1920, 1080, 0, false, 0, NULL, 0), wds(b, SEC(3.5), 1920, 1080), end_seg(b, SEC(3.5));
}

static const TE NOISE_PAL[] = {{0, 16, 128, 128, 0},     {1, 235, 128, 128, 255}, {2, 16, 128, 128, 255},
                               {3, 63, 240, 102, 255},   {4, 219, 138, 16, 255},   {5, 32, 118, 240, 255},
                               {6, 188, 154, 16, 128},   {7, 78, 214, 230, 64},   {8, 200, 100, 100, 32},
                               {9, 100, 200, 50, 255},   {10, 128, 128, 128, 255}};

/* An object over several ODS segments (its RLE well past 64 KB), then an acquisition point redefining it */
static void sup_big(Buf *b, Pic *N, Pic *D) {
    *N = pic(500, 200, 1), *D = pic(40, 10, 2);
    TC a = {0, 700, 800, false, 0, 0, 0, 0};
    Buf r = {0};
    pgs_rle(&r, N);
    int pieces = (int)(r.len / 40000) + 1;
    buf_free(&r);
    pcs(b, SEC(10), 1920, 1080, 0x80, false, 0, &a, 1), wds(b, SEC(10), 1920, 1080), pds(b, SEC(10), 0, 0, NOISE_PAL, 11);
    ods(b, SEC(10), 0, N, pieces), end_seg(b, SEC(10));
    pcs(b, SEC(12), 1920, 1080, 0x40, false, 0, &a, 1), wds(b, SEC(12), 1920, 1080), pds(b, SEC(12), 0, 0, NOISE_PAL, 11);
    ods(b, SEC(12), 0, D, 1), end_seg(b, SEC(12));
    pcs(b, SEC(14), 1920, 1080, 0, false, 0, &a, 1), end_seg(b, SEC(14));
}

/* ------------------------------------------------------------ PGS */

TEST(subpic_pgs_draws_a_cue) {
    Buf b = {0};
    Pic A;
    sup_one(&b, &A);
    Mem m;
    MkvReader r = mem(&m, b.p, b.len);
    char err[100];
    SubPic *s = subpic_open_sup(&r, err, sizeof err);
    CHECK(s);
    int fw, fh;
    subpic_frame(s, &fw, &fh);
    CHECK_INT(fw, 1920); CHECK_INT(fh, 1080);
    CHECK_INT(subpic_count(s), 1);
    const SubPicCue *c = subpic_cue(s, 0);
    CHECK_INT(c->start_ms, 1000); CHECK_INT(c->end_ms, 3500);
    CHECK_INT(c->x, 100); CHECK_INT(c->y, 900); CHECK_INT(c->w, 300); CHECK_INT(c->h, 40);
    uint32_t pal[256];
    int nc = 0;
    uint8_t *px = draw(s, 0, pal, &nc);
    CHECK(px);
    CHECK(!memcmp(px, A.px, 300 * 40));
    CHECK_INT(nc, 4);
    CHECK_INT(pal[0] & 255, 0);
    CHECK(pal[1] == 0xFFFFFFFFu);
    CHECK(pal[2] == 0x000000FFu);
    CHECK(close_to(pal[3], 255, 0, 0, 128)); /* BT.709 red: HD */
    uint8_t small[10];
    CHECK(!subpic_render(s, 0, small, sizeof small, pal, &nc)); /* (too little room) */
    CHECK(!subpic_render(s, 1, px, 300 * 40, pal, &nc));
    free(px), free(A.px), buf_free(&b);
    subpic_close(s);
}

TEST(subpic_pgs_two_objects_and_cropping) {
    Buf b = {0};
    Pic P = pic(50, 20, 0), Q = pic(40, 20, 2);
    static const TE pal[] = {{0, 16, 128, 128, 255}, {1, 235, 128, 128, 200}, {2, 16, 128, 128, 255}, {3, 63, 240, 102, 100}};
    TC two[] = {{1, 10, 600, false, 0, 0, 0, 0}, {2, 200, 640, true, 5, 3, 20, 10}};
    pcs(&b, SEC(2), 1280, 720, 0x80, false, 3, two, 2), wds(&b, SEC(2), 1280, 720), pds(&b, SEC(2), 3, 0, pal, 4);
    ods(&b, SEC(2), 1, &P, 1), ods(&b, SEC(2), 2, &Q, 1), end_seg(&b, SEC(2));
    /* a crop running past the object's edge shows what's there; one wholly outside shows nothing */
    TC edge[] = {{2, 1270, 700, true, 30, 10, 50, 50}, {1, 0, 0, true, 60, 0, 10, 10}};
    pcs(&b, SEC(3), 1280, 720, 0, false, 3, edge, 2), end_seg(&b, SEC(3));
    Mem m;
    MkvReader r = mem(&m, b.p, b.len);
    SubPic *s = subpic_open_sup(&r, NULL, 0);
    CHECK(s);
    CHECK_INT(subpic_count(s), 2);
    const SubPicCue *c = subpic_cue(s, 0);
    CHECK_INT(c->start_ms, 2000); CHECK_INT(c->end_ms, 3000);
    CHECK_INT(c->x, 10); CHECK_INT(c->y, 600); CHECK_INT(c->w, 210); CHECK_INT(c->h, 50);
    uint8_t *ex = xmalloc(210 * 50);
    memset(ex, 4, 210 * 50); /* uncovered: the first entry that's clear - 4, never defined */
    place(ex, c, &P, 10, 600, 0, 0, 50, 20);
    place(ex, c, &Q, 200, 640, 5, 3, 20, 10);
    uint32_t pl[256];
    int nc = 0;
    uint8_t *px = draw(s, 0, pl, &nc);
    CHECK(px);
    CHECK(!memcmp(px, ex, 210 * 50));
    CHECK_INT(nc, 5);
    CHECK(pl[4] == 0);
    CHECK(close_to(pl[3], 255, 0, 0, 100)); /* 720 lines: HD */
    free(px), free(ex);
    c = subpic_cue(s, 1); /* the crop (30..40, 10..20) at (1270, 700), cut by the frame to 10 x 10 */
    CHECK_INT(c->x, 1270); CHECK_INT(c->y, 700); CHECK_INT(c->w, 10); CHECK_INT(c->h, 10);
    ex = xmalloc(100);
    place(ex, c, &Q, 1270, 700, 30, 10, 10, 10);
    px = draw(s, 1, pl, &nc);
    CHECK(px);
    CHECK(!memcmp(px, ex, 100));
    free(px), free(ex), free(P.px), free(Q.px), buf_free(&b);
    subpic_close(s);
}

TEST(subpic_pgs_palettes_and_epochs) {
    Buf b = {0};
    Pic A, B, C;
    sup_epochs(&b, &A, &B, &C);
    Mem m;
    MkvReader r = mem(&m, b.p, b.len);
    SubPic *s = subpic_open_sup(&r, NULL, 0);
    CHECK(s);
    CHECK(sane(s));
    CHECK_INT(subpic_count(s), 4 + 1);
    static const int64_t times[][2] = {{500, 1000}, {1000, 1500}, {1500, 2000}, {3000, 4000}, {6000, 11000}};
    for (int i = 0; i < 5; i++) {
        CHECK_INT(subpic_cue(s, i)->start_ms, times[i][0]);
        CHECK_INT(subpic_cue(s, i)->end_ms, times[i][1]);
    }
    uint32_t pal[256];
    int nc;
    /* 0: A with the first palette (SD: BT.601) */
    uint8_t *px = draw(s, 0, pal, &nc);
    CHECK(px && !memcmp(px, A.px, 64 * 16));
    CHECK(pal[1] == 0xFFFFFFFFu && pal[2] == 0x000000FFu && close_to(pal[3], 128, 128, 128, 200) && (pal[0] & 255) == 0);
    free(px);
    /* 1: the same picture, entry 1 now red, the rest as they were */
    px = draw(s, 1, pal, &nc);
    CHECK(px && !memcmp(px, A.px, 64 * 16));
    CHECK(close_to(pal[1], 255, 0, 0, 255) && pal[2] == 0x000000FFu && close_to(pal[3], 128, 128, 128, 200));
    free(px);
    /* 2: A (never sent again) and B together */
    const SubPicCue *c = subpic_cue(s, 2);
    CHECK_INT(c->x, 100); CHECK_INT(c->y, 400); CHECK_INT(c->w, 264); CHECK_INT(c->h, 28);
    uint8_t *ex = xcalloc(264 * 28, 1);
    place(ex, c, &A, 300, 400, 0, 0, 64, 16), place(ex, c, &B, 100, 420, 0, 0, 32, 8);
    px = draw(s, 2, pal, &nc);
    CHECK(px && !memcmp(px, ex, 264 * 28));
    CHECK(close_to(pal[1], 255, 0, 0, 255));
    free(px), free(ex);
    /* 3: the new epoch's C alone (object 1 died with its epoch), its own palette: entry 3 undefined */
    c = subpic_cue(s, 3);
    CHECK_INT(c->x, 10); CHECK_INT(c->y, 10); CHECK_INT(c->w, 20); CHECK_INT(c->h, 10);
    px = draw(s, 3, pal, &nc);
    CHECK(px && !memcmp(px, C.px, 20 * 10));
    CHECK(pal[1] == 0x000000FFu && pal[2] == 0xFFFFFFFFu && pal[3] == 0);
    free(px);
    px = draw(s, 4, pal, &nc);
    CHECK(px && !memcmp(px, C.px, 20 * 10));
    free(px);
    subpic_close(s);
    free(A.px), free(B.px), free(C.px), buf_free(&b);
}

TEST(subpic_pgs_objects_over_several_segments) {
    Buf b = {0};
    Pic N, D;
    sup_big(&b, &N, &D);
    int64_t third = seg_find_k(&b, 0x15, 2); /* (the first object took three segments or more) */
    CHECK(third > 0 && !((uint8_t)b.p[third + 13 + 3] & 0x80));
    Mem m;
    MkvReader r = mem(&m, b.p, b.len);
    SubPic *s = subpic_open_sup(&r, NULL, 0);
    CHECK(s);
    CHECK_INT(subpic_count(s), 3);
    CHECK_INT(subpic_cue(s, 0)->w, 500); CHECK_INT(subpic_cue(s, 0)->h, 200);
    CHECK_INT(subpic_cue(s, 2)->start_ms, 14000); CHECK_INT(subpic_cue(s, 2)->end_ms, 19000);
    uint32_t pal[256];
    int nc;
    uint8_t *px = draw(s, 0, pal, &nc);
    CHECK(px && !memcmp(px, N.px, 500 * 200));
    CHECK_INT(nc, 11);
    CHECK(close_to(pal[4], 255, 255, 0, 255)); /* (BT.709 yellow) */
    free(px);
    for (int i = 1; i < 3; i++) { /* the acquisition point's object 0 replaced it */
        px = draw(s, i, pal, &nc);
        CHECK(px && subpic_cue(s, i)->w == 40 && !memcmp(px, D.px, 40 * 10));
        free(px);
    }
    subpic_close(s);
    free(N.px), free(D.px), buf_free(&b);
}

TEST(subpic_pgs_damaged) {
    Buf b = {0};
    Pic A, B, C;
    sup_epochs(&b, &A, &B, &C);
    Mem m;
    MkvReader r;
    /* cut short anywhere */
    for (size_t n = 0; n <= b.len; n++) {
        r = mem(&m, b.p, n);
        SubPic *s = subpic_open_sup(&r, NULL, 0);
        if (n < 13) CHECK(!s);
        if (!s) continue;
        CHECK(sane(s));
        CHECK(subpic_count(s) <= 5);
        subpic_close(s);
    }
    uint32_t pal[256];
    int nc;
    /* a palette segment saying it's longer than it is: it's dropped, the rest stands */
    Buf d = {0};
    buf_put(&d, b.p, b.len);
    int64_t at = seg_find_k(&d, 0x14, 1);
    CHECK(at > 0);
    d.p[at + 11] = (char)0xFF, d.p[at + 12] = (char)0xF0;
    r = mem(&m, d.p, d.len);
    SubPic *s = subpic_open_sup(&r, NULL, 0);
    CHECK(s && sane(s));
    CHECK_INT(subpic_count(s), 5);
    uint8_t *px = draw(s, 1, pal, &nc);
    CHECK(px && pal[1] == 0xFFFFFFFFu); /* (no red: the update was lost) */
    free(px);
    subpic_close(s);
    /* an object segment saying it's shorter: object B is lost, cue 2 shows A alone */
    buf_reset(&d);
    buf_put(&d, b.p, b.len);
    at = seg_find_k(&d, 0x15, 1);
    CHECK(at > 0);
    unsigned len = (unsigned)((uint8_t)d.p[at + 11] << 8 | (uint8_t)d.p[at + 12]) - 10;
    d.p[at + 11] = (char)(len >> 8), d.p[at + 12] = (char)len;
    r = mem(&m, d.p, d.len);
    s = subpic_open_sup(&r, NULL, 0);
    CHECK(s && sane(s));
    CHECK_INT(subpic_count(s), 5);
    CHECK_INT(subpic_cue(s, 2)->x, 300); CHECK_INT(subpic_cue(s, 2)->w, 64);
    px = draw(s, 2, pal, &nc);
    CHECK(px && !memcmp(px, A.px, 64 * 16));
    free(px);
    subpic_close(s);
    /* junk before it: found anyway; nothing PGS at all: refused */
    buf_reset(&d);
    for (int i = 0; i < 300; i++) b8(&d, i % 7 == 0 ? 'P' : i);
    buf_put(&d, b.p, b.len);
    r = mem(&m, d.p, d.len);
    s = subpic_open_sup(&r, NULL, 0);
    CHECK(s && subpic_count(s) == 5 && sane(s));
    subpic_close(s);
    char err[100] = "";
    memset(d.p, 0, d.len);
    r = mem(&m, d.p, d.len);
    CHECK(!subpic_open_sup(&r, err, sizeof err));
    CHECK_HAS(err, "PGS");
    /* flipped bytes */
    uint32_t seed = 12345;
    for (int round = 0; round < 400; round++) {
        buf_reset(&d);
        buf_put(&d, b.p, b.len);
        for (int k = 0; k <= round % 8; k++) {
            seed = seed * 1103515245u + 12345u;
            size_t i = (seed >> 8) % d.len;
            seed = seed * 1103515245u + 12345u;
            d.p[i] = (char)(seed >> 16);
        }
        r = mem(&m, d.p, d.len);
        s = subpic_open_sup(&r, NULL, 0);
        CHECK(s); /* (the first segments may break; later ones are found) */
        CHECK(sane(s));
        subpic_close(s);
    }
    buf_free(&d);
    free(A.px), free(B.px), free(C.px), buf_free(&b);
}

TEST(subpic_pgs_bounds) {
    Buf b = {0};
    Pic big = {4097, 1, xcalloc(4097, 1)}, ok = pic(8, 4, 2);
    static const TE pal[] = {{1, 235, 128, 128, 255}};
    TC a = {0, 0, 0, false, 0, 0, 0, 0};
    /* an object too wide, a frame too big: no cues */
    pcs(&b, SEC(1), 1920, 1080, 0x80, false, 0, &a, 1), pds(&b, SEC(1), 0, 0, pal, 1), ods(&b, SEC(1), 0, &big, 1), end_seg(&b, SEC(1));
    pcs(&b, SEC(2), 5000, 5000, 0x80, false, 0, &a, 1), pds(&b, SEC(2), 0, 0, pal, 1), ods(&b, SEC(2), 0, &ok, 1), end_seg(&b, SEC(2));
    Mem m;
    MkvReader r = mem(&m, b.p, b.len);
    SubPic *s = subpic_open_sup(&r, NULL, 0);
    CHECK(s);
    CHECK_INT(subpic_count(s), 0);
    subpic_close(s);
    /* at most SUBPIC_MAX_CUES cues: the last kept ends where the next display set starts */
    buf_reset(&b);
    pcs(&b, 0, 720, 480, 0x80, false, 0, &a, 1), pds(&b, 0, 0, 0, pal, 1), ods(&b, 0, 0, &ok, 1), end_seg(&b, 0);
    for (int i = 1; i < SUBPIC_MAX_CUES + 10; i++) pcs(&b, (uint32_t)i * 900, 720, 480, 0, false, 0, &a, 1), end_seg(&b, (uint32_t)i * 900);
    r = mem(&m, b.p, b.len);
    s = subpic_open_sup(&r, NULL, 0);
    CHECK(s);
    CHECK_INT(subpic_count(s), SUBPIC_MAX_CUES);
    const SubPicCue *c = subpic_cue(s, SUBPIC_MAX_CUES - 1);
    CHECK_INT(c->start_ms, (SUBPIC_MAX_CUES - 1) * 10);
    CHECK_INT(c->end_ms, SUBPIC_MAX_CUES * 10);
    subpic_close(s);
    free(big.px), free(ok.px), buf_free(&b);
}

/* ------------------------------------------------------------ a VobSub writer */

typedef struct {
    Buf *b;
    int cur;
    bool half;
} Nib;

static void nib(Nib *n, int v) {
    if (!n->half) n->cur = v << 4, n->half = true;
    else b8(n->b, n->cur | v), n->half = false;
}

/* One code: len 1..255 of pixel value c, or len 0 = to the line's end */
static void vob_code(Nib *n, int len, int c) {
    int v = len << 2 | c;
    if (len == 0) nib(n, 0), nib(n, 0), nib(n, 0), nib(n, c);
    else if (len < 4) nib(n, v);
    else if (len < 16) nib(n, v >> 4), nib(n, v & 15);
    else if (len < 64) nib(n, v >> 8), nib(n, v >> 4 & 15), nib(n, v & 15);
    else nib(n, v >> 12), nib(n, v >> 8 & 15), nib(n, v >> 4 & 15), nib(n, v & 15);
}

static void vob_field(Buf *o, const Pic *p, int field, bool to_end) {
    Nib n = {o, 0, false};
    for (int y = field; y < p->h; y += 2) {
        const uint8_t *row = p->px + (size_t)y * (size_t)p->w;
        for (int x = 0; x < p->w;) {
            int k = 1;
            while (x + k < p->w && row[x + k] == row[x]) k++;
            if (to_end && x + k == p->w) vob_code(&n, 0, row[x]);
            else
                for (int left = k; left > 0; left -= left > 255 ? 255 : left) vob_code(&n, left > 255 ? 255 : left, row[x]);
            x += k;
        }
        if (n.half) nib(&n, 0); /* (each line byte-aligned) */
    }
}

typedef struct {
    int x, y;
    uint8_t col[4], alpha[4];
    int start, stop; /* dates (1024/90000 s); stop < 0: none */
    bool to_end;
} TSpu;

static void spu_make(Buf *o, const Pic *p, const TSpu *t) {
    Buf top = {0}, bot = {0};
    vob_field(&top, p, 0, t->to_end), vob_field(&bot, p, 1, t->to_end);
    unsigned ctrl = 4 + (unsigned)top.len + (unsigned)bot.len, d2 = ctrl + 24, size = d2 + (t->stop >= 0 ? 6 : 0);
    b16(o, size), b16(o, ctrl);
    buf_put(o, top.p, top.len), buf_put(o, bot.p, bot.len);
    b16(o, (unsigned)t->start), b16(o, t->stop >= 0 ? d2 : ctrl);
    b8(o, 0x03), b8(o, t->col[3] << 4 | t->col[2]), b8(o, t->col[1] << 4 | t->col[0]);
    b8(o, 0x04), b8(o, t->alpha[3] << 4 | t->alpha[2]), b8(o, t->alpha[1] << 4 | t->alpha[0]);
    int x2 = t->x + p->w - 1, y2 = t->y + p->h - 1;
    b8(o, 0x05), b8(o, t->x >> 4), b8(o, (t->x & 15) << 4 | x2 >> 8), b8(o, x2 & 255);
    b8(o, t->y >> 4), b8(o, (t->y & 15) << 4 | y2 >> 8), b8(o, y2 & 255);
    b8(o, 0x06), b16(o, 4), b16(o, 4 + (unsigned)top.len);
    b8(o, 0x01), b8(o, 0xFF);
    if (t->stop >= 0) b16(o, (unsigned)t->stop), b16(o, d2), b8(o, 0x02), b8(o, 0xFF);
    buf_free(&top), buf_free(&bot);
}

/* The SPU into the .sub in packs (2048 bytes each with pad; MPEG-1 or -2 pack and PES headers, the PTS on
   the first), with another substream's packet before ours in every pack but the first (others). Returns
   where it starts. */
static int64_t sub_put(Buf *sub, const Buf *spu, int sid, bool mpeg1, bool pad, bool others) {
    int64_t pos = (int64_t)sub->len;
    size_t at = 0;
    for (int k = 0; at < spu->len; k++) {
        size_t pack = sub->len;
        b32(sub, 0x1BA);
        static const uint8_t m1[] = {0x21, 0x00, 0x01, 0x00, 0x01, 0x80, 0x00, 0x01};
        static const uint8_t m2[] = {0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x01, 0x89, 0xC3, 0xF8};
        if (mpeg1) buf_put(sub, m1, sizeof m1);
        else buf_put(sub, m2, sizeof m2);
        if (others && k > 0) {
            b32(sub, 0x1BD), b16(sub, 3 + 1 + 20), b8(sub, 0x81), b8(sub, 0), b8(sub, 0), b8(sub, sid + 1);
            for (int i = 0; i < 20; i++) b8(sub, 0xEE);
        }
        size_t hdr = mpeg1 ? (k == 0 ? 5 : 1) : (k == 0 ? 8 : 3);
        size_t room = 2048 - (sub->len - pack) - 6 - hdr - 1, n = spu->len - at < room ? spu->len - at : room;
        b32(sub, 0x1BD), b16(sub, (unsigned)(hdr + 1 + n));
        if (mpeg1 && k == 0) b8(sub, 0x21), b8(sub, 0), b8(sub, 1), b8(sub, 0), b8(sub, 1);
        else if (mpeg1) b8(sub, 0x0F);
        else {
            b8(sub, 0x81), b8(sub, k == 0 ? 0x80 : 0), b8(sub, k == 0 ? 5 : 0);
            if (k == 0) b8(sub, 0x21), b8(sub, 0), b8(sub, 1), b8(sub, 0), b8(sub, 1);
        }
        b8(sub, sid);
        buf_put(sub, spu->p + at, n);
        at += n;
        size_t left = 2048 - (sub->len - pack);
        if (pad && left >= 6) {
            b32(sub, 0x1BE), b16(sub, (unsigned)(left - 6));
            for (size_t i = 6; i < left; i++) b8(sub, 0xFF);
        }
    }
    return pos;
}

static const uint32_t VPAL[16] = {0x000000, 0xffffff, 0x808080, 0xff0000, 0x00ff00, 0x0000ff, 0x123456, 0xabcdef,
                                  0x102030, 0x405060, 0x708090, 0xa0b0c0, 0xd0e0f0, 0x111111, 0x222222, 0xfedcba};

static void idx_ts(Buf *x, int64_t ms, int64_t pos) {
    buf_printf(x, "timestamp: %02d:%02d:%02d:%03d, filepos: %09llx\n", (int)(ms / 3600000), (int)(ms / 60000 % 60),
               (int)(ms / 1000 % 60), (int)(ms % 1000), (unsigned long long)pos);
}

typedef struct {
    Buf idx, sub;
    Pic pic[3];
    TSpu spu[3];
    int64_t pos[3];
} Vob;

/* Three subpictures (720x480): 0 at 2.5 s with a stop, 1 at 5 s (start delayed, no stop, over several
   packs with another substream between them), 2 at 60 s in MPEG-1 packs, reaching past the frame. The
   idx's second track points at 0 again (left alone). custom: custom colors ON + a delay; no_size: no
   size line. */
static void vob_build(Vob *v, bool custom, bool no_size) {
    memset(v, 0, sizeof *v);
    v->pic[0] = pic(180, 37, 3), v->pic[1] = pic(720, 100, 3), v->pic[2] = pic(64, 20, 0);
    v->spu[0] = (TSpu){270, 400, {0, 1, 3, 6}, {0, 15, 15, 9}, 0, 176, false};
    v->spu[1] = (TSpu){0, 340, {2, 1, 0, 15}, {3, 15, 15, 15}, 9, -1, true};
    v->spu[2] = (TSpu){600, 470, {0, 5, 7, 1}, {0, 15, 8, 15}, 0, -1, false};
    Buf decoy = {0}; /* (another substream's subpicture first in the file) */
    spu_make(&decoy, &v->pic[2], &v->spu[2]);
    sub_put(&v->sub, &decoy, 0x21, false, true, false);
    buf_free(&decoy);
    for (int i = 0; i < 3; i++) {
        Buf s = {0};
        spu_make(&s, &v->pic[i], &v->spu[i]);
        v->pos[i] = sub_put(&v->sub, &s, 0x20, i == 2, i < 2, i == 1);
        buf_free(&s);
    }
    Buf *x = &v->idx;
    buf_puts(x, "# VobSub index file, v7 (do not modify this line!)\n#\n");
    if (!no_size) buf_puts(x, "size: 720x480\r\n");
    buf_puts(x, "org: 0, 0\nscale: 100%, 100%\nalpha: 100%\nsmooth: OFF\nfadein/out: 50, 50\nalign: OFF at LEFT TOP\n");
    buf_puts(x, "time offset: 0\nforced subs: OFF\npalette: ");
    for (int i = 0; i < 16; i++) buf_printf(x, "%s%06x", i ? ", " : "", VPAL[i]);
    buf_puts(x, custom ? "\ncustom colors: ON, tridx: 1000, colors: 0a0b0c, 112233, 445566, 778899\n"
                       : "\ncustom colors: OFF, tridx: 0000, colors: 000000, 000000, 000000, 000000\n");
    buf_puts(x, "langidx: 0\n\n# English\nid: en, index: 0\n");
    if (custom) buf_puts(x, "delay: 00:00:01:000\n");
    idx_ts(x, 2500, v->pos[0]), idx_ts(x, 5000, v->pos[1]), idx_ts(x, 60000, v->pos[2]);
    buf_puts(x, "\n# Swedish\nid: sv, index: 1\n");
    idx_ts(x, 1000, v->pos[0]);
}

static void vob_free(Vob *v) {
    buf_free(&v->idx), buf_free(&v->sub);
    for (int i = 0; i < 3; i++) free(v->pic[i].px);
}

/* Cue k of the Vob drawn as expected: its picture where the rectangle cuts it; its 4 colours */
static bool vob_same(SubPic *s, const Vob *v, int k, bool custom) {
    const SubPicCue *c = subpic_cue(s, k);
    const TSpu *t = &v->spu[k];
    uint8_t *ex = xmalloc((size_t)c->w * (size_t)c->h);
    memset(ex, 9, (size_t)c->w * (size_t)c->h);
    place(ex, c, &v->pic[k], t->x, t->y, 0, 0, v->pic[k].w, v->pic[k].h);
    uint32_t pal[256];
    int nc = 0;
    uint8_t *px = draw(s, k, pal, &nc);
    bool ok = px && !memcmp(px, ex, (size_t)c->w * (size_t)c->h) && nc <= 4;
    static const uint32_t cust[4] = {0x0a0b0c, 0x112233, 0x445566, 0x778899};
    for (int i = 0; i < 4 && ok; i++) {
        uint32_t want = (custom ? cust[i] : VPAL[t->col[i]]) << 8 | (custom && i == 0 ? 0 : t->alpha[i] * 17u);
        ok = pal[i] == want;
    }
    free(px), free(ex);
    return ok;
}

/* ------------------------------------------------------------ VobSub */

TEST(subpic_vobsub_draws_both_fields) {
    Vob v;
    vob_build(&v, false, false);
    CHECK(v.pos[1] - v.pos[0] >= 2048 && v.sub.len - (size_t)v.pos[1] > 3 * 2048); /* (1 spans several packs) */
    Mem m;
    MkvReader r = mem(&m, v.sub.p, v.sub.len);
    char err[100];
    SubPic *s = subpic_open_vobsub(v.idx.p, v.idx.len, &r, err, sizeof err);
    CHECK(s);
    CHECK(sane(s));
    int fw, fh;
    subpic_frame(s, &fw, &fh);
    CHECK_INT(fw, 720); CHECK_INT(fh, 480);
    CHECK_INT(subpic_count(s), 3);
    const SubPicCue *c = subpic_cue(s, 0);
    CHECK_INT(c->start_ms, 2500); CHECK_INT(c->end_ms, 2500 + 176 * 1024 / 90);
    CHECK_INT(c->x, 270); CHECK_INT(c->y, 400); CHECK_INT(c->w, 180); CHECK_INT(c->h, 37);
    c = subpic_cue(s, 1);
    CHECK_INT(c->start_ms, 5000 + 9 * 1024 / 90); CHECK_INT(c->end_ms, 60000); /* (no stop: until the next) */
    CHECK_INT(c->x, 0); CHECK_INT(c->y, 340); CHECK_INT(c->w, 720); CHECK_INT(c->h, 100);
    c = subpic_cue(s, 2);
    CHECK_INT(c->start_ms, 60000); CHECK_INT(c->end_ms, 65000);
    CHECK_INT(c->x, 600); CHECK_INT(c->y, 470); CHECK_INT(c->w, 64); CHECK_INT(c->h, 10); /* (cut by the frame) */
    for (int k = 0; k < 3; k++) CHECK(vob_same(s, &v, k, false));
    subpic_close(s);
    vob_free(&v);
}

TEST(subpic_vobsub_custom_colours_delay_and_no_size) {
    Vob v;
    vob_build(&v, true, true);
    Mem m;
    MkvReader r = mem(&m, v.sub.p, v.sub.len);
    SubPic *s = subpic_open_vobsub(v.idx.p, v.idx.len, &r, NULL, 0);
    CHECK(s);
    int fw, fh;
    subpic_frame(s, &fw, &fh);
    CHECK_INT(fw, 720); CHECK_INT(fh, 576); /* (no size line, a picture below line 480: PAL) */
    CHECK_INT(subpic_count(s), 3);
    CHECK_INT(subpic_cue(s, 0)->start_ms, 3500);
    CHECK_INT(subpic_cue(s, 2)->start_ms, 61000); CHECK_INT(subpic_cue(s, 2)->h, 20);
    for (int k = 0; k < 3; k++) CHECK(vob_same(s, &v, k, true));
    subpic_close(s);
    vob_free(&v);
}

TEST(subpic_vobsub_damaged) {
    Vob v;
    vob_build(&v, false, false);
    Mem m;
    MkvReader r;
    char err[100] = "";
    /* nothing VobSub in the idx: refused; an idx with nothing for its .sub: no cues */
    r = mem(&m, v.sub.p, v.sub.len);
    CHECK(!subpic_open_vobsub("hello\nworld\n", 12, &r, err, sizeof err));
    CHECK_HAS(err, "VobSub");
    SubPic *s = subpic_open_vobsub("size: 720x576\ntimestamp: 00:00:01:000, filepos: 7fffffff\n", 57, &r, NULL, 0);
    CHECK(s && subpic_count(s) == 0);
    subpic_close(s);
    /* the .sub cut short anywhere */
    for (size_t n = 0; n <= v.sub.len; n += n < 4200 ? 1 : 37) {
        r = mem(&m, v.sub.p, n);
        s = subpic_open_vobsub(v.idx.p, v.idx.len, &r, NULL, 0);
        CHECK(s && sane(s));
        CHECK(subpic_count(s) <= 3);
        subpic_close(s);
    }
    /* the idx cut short anywhere */
    r = mem(&m, v.sub.p, v.sub.len);
    for (size_t n = 0; n <= v.idx.len; n++) {
        s = subpic_open_vobsub(v.idx.p, n, &r, NULL, 0);
        if (s) CHECK(sane(s));
        subpic_close(s);
    }
    /* a control offset past the subpicture's end: that one's left out */
    Buf d = {0};
    buf_put(&d, v.sub.p, v.sub.len);
    size_t at = (size_t)v.pos[0] + 14 + 6 + 8 + 1; /* (pack header, PES start + length, its header with the PTS, substream) */
    d.p[at + 2] = d.p[at], d.p[at + 3] = d.p[at + 1];
    r = mem(&m, d.p, d.len);
    s = subpic_open_vobsub(v.idx.p, v.idx.len, &r, NULL, 0);
    CHECK(s && subpic_count(s) == 2 && subpic_cue(s, 0)->start_ms > 5000);
    subpic_close(s);
    /* flipped bytes in the .sub and the idx */
    uint32_t seed = 777;
    Buf x = {0};
    for (int round = 0; round < 300; round++) {
        buf_reset(&d), buf_reset(&x);
        buf_put(&d, v.sub.p, v.sub.len), buf_put(&x, v.idx.p, v.idx.len);
        for (int k = 0; k <= round % 6; k++) {
            seed = seed * 1103515245u + 12345u;
            Buf *t = round % 3 == 0 ? &x : &d;
            size_t i = (seed >> 8) % t->len;
            seed = seed * 1103515245u + 12345u;
            /* (in the .sub near the subpictures' starts: where the headers are) */
            if (t == &d && round % 2) i = ((size_t)v.pos[(seed >> 4) % 3] + (seed >> 8) % 64) % d.len;
            t->p[i] = (char)(seed >> 16);
        }
        r = mem(&m, d.p, d.len);
        s = subpic_open_vobsub(x.p, x.len, &r, NULL, 0);
        if (s) CHECK(sane(s));
        subpic_close(s);
    }
    buf_free(&x);
    /* at most SUBPIC_MAX_CUES */
    buf_reset(&d);
    buf_puts(&d, "size: 720x480\nid: en, index: 0\n");
    for (int i = 0; i < SUBPIC_MAX_CUES + 10; i++) idx_ts(&d, (int64_t)i * 100, v.pos[0]);
    r = mem(&m, v.sub.p, v.sub.len);
    s = subpic_open_vobsub(d.p, d.len, &r, NULL, 0);
    CHECK(s);
    CHECK_INT(subpic_count(s), SUBPIC_MAX_CUES);
    CHECK_INT(subpic_cue(s, 5)->end_ms, 600); /* (its stop would be later: the next one starts) */
    subpic_close(s);
    buf_free(&d);
    vob_free(&v);
}

/* ------------------------------------------------------------ PNG, read back by the test's own decoder */

typedef struct {
    const uint8_t *p;
    size_t n, pos;
    int nb;
    bool bad;
} Bin;

static int bit1(Bin *in) {
    if (in->pos >= in->n) {
        in->bad = true;
        return 0;
    }
    int b = in->p[in->pos] >> in->nb & 1;
    if (++in->nb == 8) in->nb = 0, in->pos++;
    return b;
}

static unsigned bitsn(Bin *in, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= (unsigned)bit1(in) << i;
    return v;
}

/* A literal/length symbol in the fixed code (RFC 1951 3.2.6) */
static int fixed_sym(Bin *in) {
    unsigned code = 0;
    for (int len = 1; len <= 9 && !in->bad; len++) {
        code = code << 1 | (unsigned)bit1(in);
        if (len == 7 && code <= 23) return 256 + (int)code;
        if (len == 8 && code >= 0x30 && code <= 0xBF) return (int)code - 0x30;
        if (len == 8 && code >= 0xC0 && code <= 0xC7) return 280 + (int)code - 0xC0;
        if (len == 9 && code >= 0x190) return 144 + (int)code - 0x190;
    }
    in->bad = true;
    return -1;
}

/* zlib around deflate's stored and fixed-Huffman blocks (anything else fails), its Adler-32 checked */
static bool inflate_test(const uint8_t *z, size_t n, Buf *out) {
    static const int lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    if (n < 6 || (z[0] & 15) != 8 || (z[0] * 256 + z[1]) % 31 || z[1] & 0x20) return false;
    Bin in = {z, n - 4, 2, 0, false};
    for (bool last = false; !last;) {
        last = bit1(&in);
        unsigned type = bitsn(&in, 2);
        if (in.bad) return false;
        if (type == 0) {
            if (in.nb) in.nb = 0, in.pos++;
            if (in.pos + 4 > in.n) return false;
            unsigned len = z[in.pos] | (unsigned)z[in.pos + 1] << 8, nlen = z[in.pos + 2] | (unsigned)z[in.pos + 3] << 8;
            in.pos += 4;
            if ((len ^ 0xFFFF) != nlen || in.pos + len > in.n) return false;
            buf_put(out, z + in.pos, len);
            in.pos += len;
        } else if (type == 1) {
            for (;;) {
                int sym = fixed_sym(&in);
                if (in.bad) return false;
                if (sym < 256) {
                    b8(out, sym);
                    continue;
                }
                if (sym == 256) break;
                if ((sym -= 257) >= 29) return false;
                unsigned len = (unsigned)lbase[sym] + bitsn(&in, lext[sym]), dc = 0;
                for (int i = 0; i < 5; i++) dc = dc << 1 | (unsigned)bit1(&in);
                if (dc >= 30) return false;
                unsigned dist = (unsigned)dbase[dc] + bitsn(&in, dext[dc]);
                if (in.bad || dist > out->len) return false;
                for (unsigned i = 0; i < len; i++) b8(out, (uint8_t)out->p[out->len - dist]);
            }
        } else return false;
    }
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < out->len; i++) s1 = (s1 + (uint8_t)out->p[i]) % 65521, s2 = (s2 + s1) % 65521;
    return (s2 << 16 | s1) == rd32(z + n - 4);
}

static uint32_t crc_bits(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

typedef struct {
    int w, h, n;
    bool trns;
    uint32_t pal[256];
    Buf px;
} Png;

/* A PNG of colour type 3, every chunk's CRC checked, IEND last; px without the filter bytes (all 0) */
static bool png_read(const uint8_t *p, size_t n, Png *o) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memset(o, 0, sizeof *o);
    if (n < 8 || memcmp(p, sig, 8)) return false;
    Buf z = {0}, raw = {0};
    bool end = false, ok = true;
    size_t at = 8;
    while (ok && !end && at + 12 <= n) {
        uint32_t len = rd32(p + at);
        const uint8_t *t = p + at + 4, *d = p + at + 8;
        if (len > n - at - 12 || crc_bits(t, len + 4) != rd32(d + len)) return false;
        if (!memcmp(t, "IHDR", 4)) {
            ok = at == 8 && len == 13 && d[8] == 8 && d[9] == 3 && !d[10] && !d[11] && !d[12];
            o->w = (int)rd32(d), o->h = (int)rd32(d + 4);
        } else if (!memcmp(t, "PLTE", 4)) {
            ok = len % 3 == 0 && len && len <= 768;
            o->n = (int)len / 3;
            for (int i = 0; i < o->n; i++) o->pal[i] = (uint32_t)d[3 * i] << 24 | (uint32_t)d[3 * i + 1] << 16 | (uint32_t)d[3 * i + 2] << 8 | 255;
        } else if (!memcmp(t, "tRNS", 4)) {
            ok = len <= (uint32_t)o->n && len;
            o->trns = true;
            for (uint32_t i = 0; i < len; i++) o->pal[i] = (o->pal[i] & 0xFFFFFF00u) | d[i];
        } else if (!memcmp(t, "IDAT", 4)) buf_put(&z, d, len);
        else if (!memcmp(t, "IEND", 4)) end = len == 0;
        else ok = false;
        at += 12 + len;
    }
    ok = ok && end && at == n && o->w > 0 && o->h > 0 && o->n > 0 && inflate_test((const uint8_t *)z.p, z.len, &raw) &&
         raw.len == (size_t)o->h * ((size_t)o->w + 1);
    for (int y = 0; ok && y < o->h; y++) {
        const uint8_t *row = (const uint8_t *)raw.p + (size_t)y * ((size_t)o->w + 1);
        ok = row[0] == 0;
        buf_put(&o->px, row + 1, (size_t)o->w);
    }
    for (size_t i = 0; ok && i < o->px.len; i++) ok = (uint8_t)o->px.p[i] < o->n;
    buf_free(&z), buf_free(&raw);
    return ok;
}

TEST(subpic_inflate_reads_real_zlib) { /* (the test's decoder against zlib's own output for "hello") */
    static const uint8_t fixed[] = {0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x15};
    static const uint8_t stored[] = {0x78, 0x01, 0x01, 0x05, 0x00, 0xfa, 0xff, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x06, 0x2c, 0x02, 0x15};
    Buf o = {0};
    CHECK(inflate_test(fixed, sizeof fixed, &o));
    CHECK_STR(buf_cstr(&o), "hello");
    buf_reset(&o);
    CHECK(inflate_test(stored, sizeof stored, &o));
    CHECK_STR(buf_cstr(&o), "hello");
    buf_reset(&o);
    uint8_t bad[sizeof fixed];
    memcpy(bad, fixed, sizeof bad);
    bad[sizeof bad - 1] ^= 1;
    CHECK(!inflate_test(bad, sizeof bad, &o));
    buf_free(&o);
}

/* png_indexed(px) read back: the same pixels and colours */
static bool png_round_trip(const uint8_t *px, int w, int h, const uint32_t *pal, int nc, bool want_trns) {
    size_t len = 0;
    uint8_t *png = png_indexed(px, w, h, pal, nc, &len);
    Png p;
    bool ok = png && png_read(png, len, &p) && p.w == w && p.h == h && p.trns == want_trns &&
              !memcmp(p.px.p, px, (size_t)w * (size_t)h);
    for (int i = 0; ok && i < p.n; i++) ok = p.pal[i] == (pal && i < nc ? pal[i] : 0);
    free(png), buf_free(&p.px);
    return ok;
}

TEST(subpic_png_indexed) {
    /* a cue from a .sup: clear entries, so a tRNS */
    Buf b = {0};
    Pic N, D;
    sup_big(&b, &N, &D);
    Mem m;
    MkvReader r = mem(&m, b.p, b.len);
    SubPic *s = subpic_open_sup(&r, NULL, 0);
    CHECK(s);
    uint32_t pal[256];
    int nc;
    uint8_t *px = draw(s, 0, pal, &nc);
    CHECK(px);
    CHECK(png_round_trip(px, 500, 200, pal, nc, true));
    free(px);
    subpic_close(s);
    /* runs longer than a match (258), across rows; every value; all opaque: no tRNS */
    int w = 4096, h = 3;
    uint8_t *q = xcalloc((size_t)w * (size_t)h, 1);
    for (int i = 0; i < w; i++) q[2 * w + i] = (uint8_t)(i / 16);
    uint32_t op[256];
    for (int i = 0; i < 256; i++) op[i] = (uint32_t)i << 24 | (uint32_t)(255 - i) << 16 | 0x7700u | 255;
    CHECK(png_round_trip(q, w, h, op, 256, false));
    /* one pixel; values past ncolors (transparent black entries added); no palette */
    uint8_t one = 0;
    CHECK(png_round_trip(&one, 1, 1, op, 1, false));
    uint8_t two[2] = {0, 5};
    size_t len;
    uint8_t *png = png_indexed(two, 2, 1, op, 2, &len);
    Png p;
    CHECK(png && png_read(png, len, &p));
    CHECK_INT(p.n, 6);
    CHECK(p.trns && p.pal[5] == 0 && p.pal[1] == op[1]);
    free(png), buf_free(&p.px);
    png = png_indexed(two, 2, 1, NULL, 0, &len);
    CHECK(png && png_read(png, len, &p) && p.n == 6 && p.pal[0] == 0);
    free(png), buf_free(&p.px);
    CHECK(!png_indexed(q, 0, 1, op, 1, &len) && !png_indexed(q, 1, 16385, op, 1, &len));
    free(q), free(N.px), free(D.px), buf_free(&b);
}

/* ------------------------------------------------------------ fuzz seeds, real files */

/* tools/avfuzz.c's seeds (HEMMA_FUZZ_RAW=<folder>): .sup files in pgs/, idx + NUL + sub in vobsub/ */
TEST(subpic_fuzz_seeds) {
    const char *raw = getenv("HEMMA_FUZZ_RAW");
    char path[1024];
    if (raw && *raw) {
        snprintf(path, sizeof path, "%s/pgs", raw);
        CHECK(dir_make(path));
        snprintf(path, sizeof path, "%s/vobsub", raw);
        CHECK(dir_make(path));
    }
    for (int i = 0; i < 5; i++) {
        static const char *const names[] = {"pgs/one.sup", "pgs/epochs.sup", "pgs/two-objects-big.sup", "vobsub/three",
                                            "vobsub/custom-no-size"};
        Buf b = {0};
        Pic x, y, z;
        if (i == 0) sup_one(&b, &x), free(x.px);
        else if (i == 1) sup_epochs(&b, &x, &y, &z), free(x.px), free(y.px), free(z.px);
        else if (i == 2) { /* (smaller than sup_big, under the fuzzer's 64 KB) */
            TC two[] = {{1, 10, 600, false, 0, 0, 0, 0}, {2, 200, 640, true, 5, 3, 20, 10}};
            x = pic(120, 30, 1), y = pic(40, 20, 2);
            pcs(&b, SEC(2), 1280, 720, 0x80, false, 0, two, 2), wds(&b, SEC(2), 1280, 720), pds(&b, SEC(2), 0, 0, NOISE_PAL, 11);
            ods(&b, SEC(2), 1, &x, 3), ods(&b, SEC(2), 2, &y, 1), end_seg(&b, SEC(2));
            pcs(&b, SEC(3), 1280, 720, 0x40, true, 0, two, 2), pds(&b, SEC(3), 0, 1, NOISE_PAL + 3, 2), end_seg(&b, SEC(3));
            free(x.px), free(y.px);
        } else {
            Vob v;
            vob_build(&v, i == 4, i == 4);
            /* (the noisy 720x100 subpicture makes it big: the fuzzer gets the first two) */
            size_t keep = (size_t)v.pos[2];
            buf_put(&b, v.idx.p, v.idx.len);
            b8(&b, 0);
            buf_put(&b, v.sub.p, i == 3 ? v.sub.len : keep);
            vob_free(&v);
        }
        CHECK(b.len > 100);
        if (i < 3) { /* (each seed opens and has cues) */
            Mem m;
            MkvReader r = mem(&m, b.p, b.len);
            SubPic *s = subpic_open_sup(&r, NULL, 0);
            CHECK(s && subpic_count(s) > 0);
            subpic_close(s);
        }
        if (raw && *raw) {
            snprintf(path, sizeof path, "%s/%s", raw, names[i]);
            CHECK(file_write_atomic(path, b.p, b.len));
        }
        buf_free(&b);
    }
}

/* By hand: HEMMA_TEST_SUP=<file.sup> | HEMMA_TEST_IDX=<file.idx> [HEMMA_TEST_PNG=<folder>] */
TEST(subpic_real) {
    const char *sup = plat_getenv("HEMMA_TEST_SUP"), *idx = plat_getenv("HEMMA_TEST_IDX"), *out = plat_getenv("HEMMA_TEST_PNG");
    if ((!sup || !*sup) && (!idx || !*idx)) return;
    char path[1024];
    Buf text = {0};
    if (idx && *idx) {
        CHECK(file_read_all(idx, &text, 1 << 24));
        snprintf(path, sizeof path, "%.*s.sub", (int)strlen(idx) - 4, idx);
    } else snprintf(path, sizeof path, "%s", sup);
    int64_t size = 0;
    FILE *fp = file_open(path, "rb");
    CHECK(fp && path_stat(path, &size, NULL, NULL));
    MkvReader r = {fp, size, file_cb};
    char err[100] = "";
    SubPic *s = idx && *idx ? subpic_open_vobsub(text.p, text.len, &r, err, sizeof err) : subpic_open_sup(&r, err, sizeof err);
    if (!s) printf("  %s: %s\n", path, err);
    CHECK(s);
    int fw, fh, drawn = 0;
    subpic_frame(s, &fw, &fh);
    printf("  %s: %dx%d, %d cues\n", path, fw, fh, subpic_count(s));
    CHECK(sane(s));
    if (out && *out) CHECK(dir_make(out));
    for (int i = 0; i < subpic_count(s); i++) {
        const SubPicCue *c = subpic_cue(s, i);
        uint32_t pal[256];
        int nc;
        uint8_t *px = draw(s, i, pal, &nc);
        if (!px) continue;
        drawn++;
        if (i < 5 || i == subpic_count(s) - 1)
            printf("  %d: %lld-%lld ms at %d,%d %dx%d, %d colours\n", i, (long long)c->start_ms, (long long)c->end_ms, c->x,
                   c->y, c->w, c->h, nc);
        if (out && *out) {
            size_t len;
            uint8_t *png = png_indexed(px, c->w, c->h, pal, nc, &len);
            char name[1100];
            snprintf(name, sizeof name, "%s/%05d.png", out, i);
            CHECK(png && file_write_atomic(name, png, len));
            free(png);
        }
        free(px);
    }
    CHECK_INT(drawn, subpic_count(s));
    subpic_close(s);
    fclose(fp);
    buf_free(&text);
}
