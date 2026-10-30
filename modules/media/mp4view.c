/* modules/media/mp4view.c — a multi-sound MP4 seen with ONE sound track (2.2; mediainfo.h, "sound tracks").
 *
 * Browsers play one sound track of an MP4 (only Safari lets a page switch), so the server hands each viewer a file
 * holding just theirs: the file's ftyp, a new moov - every trak but the other sound traks, the chosen one enabled,
 * chunk offsets moved to where the bytes now are - then the file's other top-level boxes (mdat, free ...) as they
 * are. The other sounds' samples stay in the mdat, referenced by nothing: allowed (ISO/IEC 14496-12), and only a
 * few hundred kbit/s each. Only the new header is held in memory; everything else is the file's own bytes, read
 * where they lie (stream.c sends them with sendfile).
 *
 * Every size and count is checked against what holds it: a damaged or hostile file gets no view (the whole file
 * is served as it is), never a crash. */
#include "mediainfo.h"

#define VIEW_MOOV_CAP (64u << 20) /* (as probe.c's MOOV_CAP) */
#define VIEW_FTYP_CAP (64u << 10)
#define VIEW_BOXES 4096 /* top-level boxes: a real file has a handful */
#define VIEW_TRAKS 64

typedef struct {
    const uint8_t *p;
    size_t n;
} Span;

typedef struct {
    char type[5];
    int64_t off, size; /* the whole box, header included (clamped at the end of the file) */
    int64_t new_off;   /* where it goes in the view; -1 = not copied (the moov, sidx) or kept in memory (ftyp) */
} Top;

typedef struct {
    int64_t old_off, new_off, len;
} Seg;

struct MediaView {
    uint8_t *head; /* ftyp + the new moov */
    size_t nhead;
    Seg *segs;
    int nsegs;
    int64_t size;
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static bool rd(const MediaReader *r, int64_t off, void *dst, size_t n) {
    if (off < 0 || !n || (uint64_t)n > (uint64_t)r->size || off > r->size - (int64_t)n) return false;
    return r->read_at(r->ctx, off, dst, n);
}

static bool fail(char *err, size_t cap, const char *why) {
    if (err && cap) str_copy(err, cap, why);
    return false;
}

/* One box of a parent's payload: its type, payload and the whole box (header too). Size 0 = to the parent's end. */
static bool box_next(Span parent, size_t *pos, char type[5], Span *payload, Span *whole) {
    if (*pos > parent.n || parent.n - *pos < 8) return false;
    const uint8_t *b = parent.p + *pos;
    size_t left = parent.n - *pos;
    uint64_t size = be32(b);
    size_t hl = 8;
    if (size == 1) {
        if (left < 16) return false;
        size = be64(b + 8);
        hl = 16;
    } else if (size == 0) {
        size = left;
    }
    if (size < hl || size > left) return false;
    memcpy(type, b + 4, 4);
    type[4] = 0;
    payload->p = b + hl;
    payload->n = (size_t)size - hl;
    if (whole) *whole = (Span){b, (size_t)size};
    *pos += (size_t)size;
    return true;
}

/* Every box of a payload, to its very end - or false (a moov that doesn't parse whole gets no view). */
static bool boxes_whole(Span parent) {
    size_t pos = 0;
    char t[5];
    Span pl;
    while (box_next(parent, &pos, t, &pl, NULL)) {
    }
    return pos == parent.n;
}

static bool find_box(Span parent, const char *type, Span *out) {
    size_t pos = 0;
    char t[5];
    Span pl;
    while (box_next(parent, &pos, t, &pl, NULL))
        if (!memcmp(t, type, 4)) {
            *out = pl;
            return true;
        }
    return false;
}

/* ------------------------------------------------------------ the traks */

typedef struct {
    Span whole, payload;
    bool sound, on; /* a 'soun' handler; tkhd enabled */
    uint32_t id;    /* tkhd track_ID */
    bool keep, chosen, co64;
} Trak;

static void read_trak(Trak *t) {
    Span tkhd, mdia, hdlr;
    if (find_box(t->payload, "tkhd", &tkhd) && tkhd.n >= 4) {
        t->on = (tkhd.p[3] & 1) != 0;
        size_t at = tkhd.p[0] == 1 ? 20 : 12;
        if (tkhd.n >= at + 4) t->id = be32(tkhd.p + at);
    }
    t->sound = find_box(t->payload, "mdia", &mdia) && find_box(mdia, "hdlr", &hdlr) && hdlr.n >= 12 &&
               !memcmp(hdlr.p + 8, "soun", 4);
}

/* ------------------------------------------------------------ the layout */

typedef struct {
    Top *top;
    int ntop;
    int64_t file_size;
} Layout;

/* Where an old offset into the file lies in the view: in a copied box, moved as that box moved. -1 = in none of
   them (in the moov, the ftyp, a box left out, past the end) - a chunk there can't be served. */
static int64_t moved(const Layout *l, uint64_t x) {
    int lo = 0, hi = l->ntop - 1;
    while (lo <= hi) { /* (boxes in file order: offsets ascending) */
        int mid = (lo + hi) / 2;
        const Top *b = &l->top[mid];
        if (x < (uint64_t)b->off) hi = mid - 1;
        else if (x >= (uint64_t)(b->off + b->size)) lo = mid + 1;
        else return b->new_off >= 0 && memcmp(b->type, "ftyp", 4) ? (int64_t)x + (b->new_off - b->off) : -1;
    }
    return -1;
}

/* ------------------------------------------------------------ writing */

static void put32(Buf *b, uint32_t v) {
    uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    buf_put(b, p, 4);
}
static void put64(Buf *b, uint64_t v) {
    put32(b, (uint32_t)(v >> 32));
    put32(b, (uint32_t)v);
}
static size_t open_box(Buf *b, const char *type) {
    size_t at = b->len;
    put32(b, 0);
    buf_put(b, type, 4);
    return at;
}
static void close_box(Buf *b, size_t at) {
    uint32_t n = (uint32_t)(b->len - at);
    uint8_t *p = (uint8_t *)b->p + at;
    p[0] = (uint8_t)(n >> 24), p[1] = (uint8_t)(n >> 16), p[2] = (uint8_t)(n >> 8), p[3] = (uint8_t)n;
}

typedef struct {
    const Layout *lay; /* NULL: sizes only (offsets written as 0) */
    const uint32_t *gone; /* track_IDs left out */
    int ngone;
    bool bad;          /* an offset outside the copied boxes */
    bool wants_co64;   /* an stco entry past 4 GB in the view */
} Build;

/* stco / co64 again, each offset moved; co64: write 64-bit ones. */
static void chunk_offsets(Build *bd, Buf *b, const char *type, Span pl, bool co64) {
    bool in64 = !memcmp(type, "co64", 4);
    size_t w = in64 ? 8 : 4;
    uint32_t n = pl.n >= 8 ? be32(pl.p + 4) : 0;
    if (pl.n < 8 || (uint64_t)n * w > pl.n - 8) {
        bd->bad = true;
        return;
    }
    size_t at = open_box(b, co64 ? "co64" : "stco");
    buf_put(b, pl.p, 4); /* version + flags */
    put32(b, n);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = pl.p + 8 + (size_t)i * w;
        uint64_t x = in64 ? be64(e) : be32(e);
        int64_t y = 0;
        if (bd->lay && (y = moved(bd->lay, x)) < 0) bd->bad = true;
        if (y < 0) y = 0;
        if (!co64 && (uint64_t)y > 0xFFFFFFFFu) bd->wants_co64 = true;
        if (co64) put64(b, (uint64_t)y);
        else put32(b, (uint32_t)y);
    }
    close_box(b, at);
}

static bool gone(const Build *bd, uint32_t id) {
    for (int i = 0; i < bd->ngone; i++)
        if (bd->gone[i] == id) return true;
    return false;
}

/* A tref whose references name a trak left out goes (a 'chap' or 'sync' pointing at nothing would mislead) */
static bool tref_points_away(const Build *bd, Span tref) {
    size_t pos = 0;
    char t[5];
    Span pl;
    while (box_next(tref, &pos, t, &pl, NULL))
        for (size_t i = 0; i + 4 <= pl.n; i += 4)
            if (gone(bd, be32(pl.p + i))) return true;
    return false;
}

/* A container's children again: path = the containers to go into on the way to stbl (trak > mdia > minf > stbl),
   where stco/co64 are written anew; tkhd of the chosen sound made enabled. */
static void rewrite(Build *bd, Buf *b, Span pl, int depth, const Trak *t) {
    static const char *const path[] = {"mdia", "minf", "stbl"};
    size_t pos = 0;
    char type[5];
    Span kid, whole;
    while (box_next(pl, &pos, type, &kid, &whole)) {
        if (depth < 3 && !memcmp(type, path[depth], 4)) {
            size_t at = open_box(b, type);
            rewrite(bd, b, kid, depth + 1, t);
            close_box(b, at);
        } else if (depth == 3 && (!memcmp(type, "stco", 4) || !memcmp(type, "co64", 4))) {
            chunk_offsets(bd, b, type, kid, t->co64);
        } else if (depth == 0 && !memcmp(type, "tkhd", 4) && t->chosen && kid.n >= 4) {
            size_t at = b->len;
            buf_put(b, whole.p, whole.n);
            ((uint8_t *)b->p)[at + (whole.n - kid.n) + 3] |= 3; /* enabled, in the movie */
        } else if (depth == 0 && !memcmp(type, "tref", 4) && tref_points_away(bd, kid)) {
            continue;
        } else {
            buf_put(b, whole.p, whole.n);
        }
    }
}

static void build_moov(Build *bd, Buf *b, Span moov, const Trak *traks, int ntraks) {
    buf_reset(b);
    size_t at = open_box(b, "moov");
    size_t pos = 0;
    char type[5];
    Span kid, whole;
    int k = 0;
    while (box_next(moov, &pos, type, &kid, &whole)) {
        if (!memcmp(type, "trak", 4)) {
            const Trak *t = k < ntraks ? &traks[k] : NULL;
            k++;
            if (!t || !t->keep) continue;
            size_t tk = open_box(b, "trak");
            rewrite(bd, b, kid, 0, t);
            close_box(b, tk);
        } else {
            buf_put(b, whole.p, whole.n);
        }
    }
    close_box(b, at);
}

/* ------------------------------------------------------------ the view */

void media_view_free(MediaView *v) {
    if (!v) return;
    free(v->head);
    free(v->segs);
    free(v);
}

int64_t media_view_size(const MediaView *v) { return v ? v->size : 0; }

bool media_view_part(const MediaView *v, int64_t off, MediaViewPart *out) {
    if (!v || !out || off < 0 || off >= v->size) return false;
    if (off < (int64_t)v->nhead) {
        *out = (MediaViewPart){v->head + off, -1, (int64_t)v->nhead - off};
        return true;
    }
    int lo = 0, hi = v->nsegs - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (v->segs[mid].new_off <= off) at = mid, lo = mid + 1;
        else hi = mid - 1;
    }
    if (at < 0 || off >= v->segs[at].new_off + v->segs[at].len) return false;
    int64_t d = off - v->segs[at].new_off;
    *out = (MediaViewPart){NULL, v->segs[at].old_off + d, v->segs[at].len - d};
    return true;
}

/* The file's top-level boxes; false + err when it isn't an MP4 we can show this way. */
static bool walk(const MediaReader *r, Layout *l, int *moov_i, int *ftyp_i, char *err, size_t cap) {
    *moov_i = *ftyp_i = -1;
    int64_t off = 0;
    while (off < r->size) {
        uint8_t h[16];
        if (r->size - off < 8 || !rd(r, off, h, 8)) return fail(err, cap, "a box cut short");
        uint64_t size = be32(h);
        int hl = 8;
        if (size == 1) {
            if (!rd(r, off + 8, h + 8, 8)) return fail(err, cap, "a box cut short");
            size = be64(h + 8);
            hl = 16;
        } else if (size == 0) {
            size = (uint64_t)(r->size - off);
        }
        for (int i = 4; i < 8; i++)
            if (h[i] < 0x20 || h[i] > 0x7E) return fail(err, cap, "not an MP4 box");
        if (size < (uint64_t)hl) return fail(err, cap, "a box smaller than its header");
        bool cut = size > (uint64_t)(r->size - off);
        if (cut) size = (uint64_t)(r->size - off); /* (an mdat cut short at the end: what's there is served) */
        if (l->ntop == VIEW_BOXES) return fail(err, cap, "too many boxes");
        Top *t = &l->top[l->ntop++];
        memcpy(t->type, h + 4, 4);
        t->type[4] = 0;
        t->off = off;
        t->size = (int64_t)size;
        t->new_off = -1;
        if (!memcmp(t->type, "moof", 4)) return fail(err, cap, "a fragmented MP4");
        if (!memcmp(t->type, "moov", 4)) {
            if (*moov_i >= 0) return fail(err, cap, "two moov boxes");
            if (cut) return fail(err, cap, "its moov is cut short");
            *moov_i = l->ntop - 1;
        }
        if (!memcmp(t->type, "ftyp", 4) && *ftyp_i < 0) *ftyp_i = l->ntop - 1;
        off += (int64_t)size;
    }
    return *moov_i >= 0 ? true : fail(err, cap, "no moov");
}

MediaView *media_view_new(const MediaReader *r, int sound, char *err, size_t cap) {
    if (err && cap) err[0] = '\0';
    if (!r || !r->read_at || r->size < 16) return fail(err, cap, "not an MP4"), NULL;
    Layout l = {xcalloc(VIEW_BOXES, sizeof(Top)), 0, r->size};
    MediaView *v = NULL;
    uint8_t *moov = NULL, *ftyp = NULL;
    Trak *traks = NULL;
    uint32_t *gone_ids = NULL;
    Buf b = {0};
    int moov_i, ftyp_i;
    if (!walk(r, &l, &moov_i, &ftyp_i, err, cap)) goto out;

    const Top *mt = &l.top[moov_i];
    uint8_t h[16];
    int hl = rd(r, mt->off, h, 8) && be32(h) == 1 ? 16 : 8;
    int64_t mn = mt->size - hl;
    if (mn <= 0 || mn > (int64_t)VIEW_MOOV_CAP) {
        fail(err, cap, mn > 0 ? "its moov is too big" : "an empty moov");
        goto out;
    }
    moov = malloc((size_t)mn);
    if (!moov || !rd(r, mt->off + hl, moov, (size_t)mn)) {
        fail(err, cap, "its moov can't be read");
        goto out;
    }
    Span ms = {moov, (size_t)mn}, kid;
    if (!boxes_whole(ms)) {
        fail(err, cap, "its moov doesn't parse");
        goto out;
    }
    if (find_box(ms, "mvex", &kid)) {
        fail(err, cap, "a fragmented MP4");
        goto out;
    }
    int ntraks = 0, nsounds = 0, chosen = -1, first_on = -1;
    traks = xcalloc(VIEW_TRAKS, sizeof *traks);
    size_t pos = 0;
    char type[5];
    Span whole;
    while (box_next(ms, &pos, type, &kid, &whole)) {
        if (memcmp(type, "trak", 4)) continue;
        if (ntraks == VIEW_TRAKS) {
            fail(err, cap, "too many tracks");
            goto out;
        }
        Trak *t = &traks[ntraks++];
        t->whole = whole;
        t->payload = kid;
        read_trak(t);
        t->keep = !t->sound;
        if (t->sound) {
            if (nsounds == sound) chosen = ntraks - 1;
            if (t->on && first_on < 0) first_on = ntraks - 1;
            nsounds++;
        }
    }
    if (!nsounds) {
        fail(err, cap, "no sound track");
        goto out;
    }
    if (chosen < 0) { /* -1 or out of range: the file's default (the first sound when none is enabled) */
        chosen = first_on;
        for (int i = 0; i < ntraks && chosen < 0; i++)
            if (traks[i].sound) chosen = i;
    }
    traks[chosen].keep = traks[chosen].chosen = true;
    gone_ids = xcalloc((size_t)ntraks, sizeof *gone_ids);
    int ngone = 0;
    for (int i = 0; i < ntraks; i++) {
        Span stbl, co;
        Span mdia, minf;
        if (!traks[i].keep && traks[i].id) gone_ids[ngone++] = traks[i].id;
        traks[i].co64 = find_box(traks[i].payload, "mdia", &mdia) && find_box(mdia, "minf", &minf) &&
                        find_box(minf, "stbl", &stbl) && find_box(stbl, "co64", &co);
    }

    /* ftyp first (as it is), then the new moov, then the rest in file order (no sidx: it maps fragments) */
    size_t nftyp = 0;
    if (ftyp_i >= 0) {
        if (l.top[ftyp_i].size > (int64_t)VIEW_FTYP_CAP) {
            fail(err, cap, "its ftyp is too big");
            goto out;
        }
        nftyp = (size_t)l.top[ftyp_i].size;
        ftyp = malloc(nftyp);
        if (!ftyp || !rd(r, l.top[ftyp_i].off, ftyp, nftyp)) {
            fail(err, cap, "its ftyp can't be read");
            goto out;
        }
    }
    Build bd = {NULL, gone_ids, ngone, false, false};
    for (int round = 0;; round++) {
        bd.lay = NULL;
        bd.bad = bd.wants_co64 = false;
        build_moov(&bd, &b, ms, traks, ntraks); /* sizes: what the moov takes doesn't hang on the offsets */
        int64_t at = (int64_t)(nftyp + b.len);
        for (int i = 0; i < l.ntop; i++) {
            Top *t = &l.top[i];
            t->new_off = -1;
            if (i == moov_i || !memcmp(t->type, "sidx", 4)) continue;
            if (i == ftyp_i) {
                t->new_off = 0;
                continue;
            }
            t->new_off = at;
            at += t->size;
        }
        bd.lay = &l;
        build_moov(&bd, &b, ms, traks, ntraks);
        if (bd.bad) {
            fail(err, cap, "a chunk lies outside the file's boxes");
            goto out;
        }
        if (!bd.wants_co64) break;
        if (round == VIEW_TRAKS) {
            fail(err, cap, "its offsets don't settle");
            goto out;
        }
        /* an offset passed 4 GB in a 32-bit table: every kept trak with one goes 64-bit, and the sizes again */
        for (int i = 0; i < ntraks; i++) {
            if (!traks[i].keep || traks[i].co64) continue;
            Build one = {&l, gone_ids, ngone, false, false};
            Buf tmp = {0};
            rewrite(&one, &tmp, traks[i].payload, 0, &traks[i]);
            traks[i].co64 = one.wants_co64;
            buf_free(&tmp);
        }
    }

    v = xcalloc(1, sizeof *v);
    v->nhead = nftyp + b.len;
    v->head = malloc(v->nhead);
    v->segs = calloc((size_t)l.ntop, sizeof *v->segs);
    if (!v->head || !v->segs) {
        media_view_free(v);
        v = NULL;
        fail(err, cap, "out of memory");
        goto out;
    }
    if (nftyp) memcpy(v->head, ftyp, nftyp);
    memcpy(v->head + nftyp, b.p, b.len);
    v->size = (int64_t)v->nhead;
    for (int i = 0; i < l.ntop; i++) { /* (in file order = view order: new offsets ascend) */
        const Top *t = &l.top[i];
        if (t->new_off < 0 || i == ftyp_i) continue;
        Seg *last = v->nsegs ? &v->segs[v->nsegs - 1] : NULL;
        if (last && last->old_off + last->len == t->off && last->new_off + last->len == t->new_off) last->len += t->size;
        else v->segs[v->nsegs++] = (Seg){t->off, t->new_off, t->size};
        v->size = t->new_off + t->size;
    }
out:
    free(l.top);
    free(moov);
    free(ftyp);
    free(traks);
    free(gone_ids);
    buf_free(&b);
    return v;
}
