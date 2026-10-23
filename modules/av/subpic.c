/* modules/av/subpic.c — picture subtitles (subpic.h): Blu-ray PGS from a .sup, DVD VobSub from an
 * .idx + .sub, indexed into cues and drawn one cue at a time.
 *
 * PGS (a .sup): segments of "PG", PTS (u32 BE, 90 kHz), DTS, type, u16 size, payload. A display set is
 * PCS (0x16: frame size, composition state, palette id, the composition objects with their places and
 * optional cropping), then WDS (0x17, windows - not needed here), PDS (0x14: palette entries id/Y/Cr/Cb/
 * alpha), ODS (0x15: an object's RLE picture, maybe over several segments) and END (0x80). Objects and
 * palettes live for an epoch (composition state 0x80 starts one): a display set may only update the
 * palette or show objects defined earlier in the epoch, so indexing keeps the epoch's state and records
 * for each cue exactly where its objects' data and its palette's segments lie - rendering a cue reads
 * just those. A cue = a display set showing at least one object; it ends at the next display set's PTS
 * (usually a clearing one), the last one LAST_MS after it starts.
 *
 * VobSub: the .idx gives the frame size, the 16-colour palette and, for the first track, a timestamp +
 * file position per subpicture; at each position the .sub holds MPEG-PS packs whose private stream 1
 * packets (substream 0x20-0x3F) carry the subpicture (SPU): u16 size, u16 control offset, the two
 * fields' RLE, then control sequences (start/stop dates, colours, contrast, area, field offsets).
 * Indexing gathers each SPU's packets (headers only) and reads its control part; rendering reads the RLE.
 *
 * Damage: a .sup segment is believed only when its size lands on the next "PG" (or the file's end);
 * otherwise the next believable segment is searched for (RESYNC_MAX bytes). Every count is capped
 * (cues, objects, fragments, palette chains, packets, control sequences), every offset checked. */
#include "subpic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAST_MS 5000          /* the last cue (or one nothing ends) shows this long */
#define MAX_FRAGS 400000      /* pieces of cue data in the file, all cues together */
#define MAX_PDS 200000        /* PGS palette segments */
#define MAX_OBJS 200000       /* PGS object definitions */
#define MAX_COMPS 200000      /* PGS objects shown, all cues together */
#define PGS_COMPS 64          /* composition objects kept per display set (the format allows 2) */
#define PGS_SLOTS 256         /* objects alive in one epoch (the format allows 64) */
#define OBJ_FRAGS 4096        /* ODS segments one object may span */
#define OBJ_BYTES (1u << 24)  /* RLE bytes of one object (its length is a u24) */
#define PAL_CHAIN 256         /* palette segments one cue's palette is gathered from */
#define PCS_READ (11 + 255 * 16) /* the most a PCS can say */
#define RESYNC_MAX (1 << 20)  /* bytes searched for the next believable segment after damage */
#define SPU_PACKETS 128       /* packets one VobSub subpicture may span */
#define SPU_SCAN (1 << 20)    /* bytes of the .sub searched for them */
#define DCSQ_MAX 64           /* control sequences read per subpicture */

typedef struct {
    int64_t off; /* in the file */
    uint32_t len;
} Frag;

typedef struct {
    int64_t off; /* a PDS payload */
    uint32_t len;
    int prev; /* the same palette's segment before it in this epoch, -1 none */
} Pds;

typedef struct {
    int w, h;
    uint32_t bytes;  /* RLE bytes its first segment announced */
    int frag, nfrag; /* its RLE: frags[frag .. frag+nfrag) */
} Obj;

typedef struct {
    int obj;            /* objs[] */
    int x, y;           /* where the shown part's top left goes on the frame */
    int cx, cy, cw, ch; /* the shown part of the object: all of it unless cropped */
} Comp;

typedef struct {
    SubPicCue c;
    int order;  /* as found (ties in start keep it) */
    bool ended; /* end_ms known: the next display set (PGS) or a stop command (VobSub) */
    /* PGS */
    int pal; /* newest palette segment, -1 none */
    int comp, ncomp;
    /* VobSub */
    int frag, nfrag;
    int ax, ay, aw, ah; /* the SPU's area on the frame (it may reach past it) */
    uint32_t size, top, bottom;
    uint8_t col[4], alpha[4];
} Cue;

struct SubPic {
    MkvReader r;
    bool vobsub, hd;
    int fw, fh;
    Cue *cues;
    int ncues, capcues;
    Frag *frags;
    int nfrags, capfrags;
    Pds *pds;
    int npds, cappds;
    Obj *objs;
    int nobjs, capobjs;
    Comp *comps;
    int ncomps, capcomps;
    uint32_t vpal[16]; /* VobSub: the idx palette, 0xRRGGBB */
    bool custom;       /* custom colors: ON - cust[] replace the palette, tridx bits make slots clear */
    uint32_t cust[4];
    uint8_t tridx;
};

/* ------------------------------------------------------------ helpers */

static void say(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

/* Room for one more of n in *p (cap entries, at most max): the array, maybe moved; *ok false when not */
static void *grow(void *p, int *cap, int n, size_t size, int max, bool *ok) {
    *ok = true;
    if (n < *cap) return p;
    if (n >= max) {
        *ok = false;
        return p;
    }
    int c = *cap ? *cap * 2 : 64;
    if (c > max) c = max;
    void *q = realloc(p, (size_t)c * size);
    if (!q) {
        *ok = false;
        return p;
    }
    *cap = c;
    return q;
}

static bool rd(const SubPic *s, int64_t off, void *dst, size_t n) {
    if (off < 0 || off > s->r.size || (uint64_t)n > (uint64_t)(s->r.size - off)) return false;
    return n == 0 || s->r.read_at(s->r.ctx, off, dst, n);
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static bool frag_add(SubPic *s, int64_t off, uint32_t len) {
    bool ok;
    s->frags = grow(s->frags, &s->capfrags, s->nfrags, sizeof *s->frags, MAX_FRAGS, &ok);
    if (ok) s->frags[s->nfrags++] = (Frag){off, len};
    return ok;
}

/* n bytes from offset at of the data in frags[first .. first+nfrag) */
static bool frag_read(const SubPic *s, int first, int nfrag, uint64_t at, uint8_t *dst, size_t n) {
    for (int i = 0; i < nfrag && n; i++) {
        const Frag *f = &s->frags[first + i];
        if (at >= f->len) {
            at -= f->len;
            continue;
        }
        size_t k = (size_t)(f->len - at);
        if (k > n) k = n;
        if (!rd(s, f->off + (int64_t)at, dst, k)) return false;
        dst += k, n -= k, at = 0;
    }
    return n == 0;
}

static Cue *cue_new(SubPic *s) {
    bool ok;
    s->cues = grow(s->cues, &s->capcues, s->ncues, sizeof *s->cues, SUBPIC_MAX_CUES, &ok);
    if (!ok) return NULL;
    Cue *c = &s->cues[s->ncues];
    memset(c, 0, sizeof *c);
    c->order = s->ncues;
    c->pal = -1;
    return c; /* (counted once the caller keeps it) */
}

/* n pixels of colour col from (fx, fy) on the frame, clipped to the cue's rectangle */
static void fill(uint8_t *px, const SubPicCue *r, int fx, int fy, int n, int col) {
    int y = fy - r->y;
    if (y < 0 || y >= r->h || n <= 0) return;
    int x0 = fx - r->x, x1 = x0 + n;
    if (x0 < 0) x0 = 0;
    if (x1 > r->w) x1 = r->w;
    if (x0 < x1) memset(px + (size_t)y * (size_t)r->w + (size_t)x0, col, (size_t)(x1 - x0));
}

static int cue_cmp(const void *a, const void *b) {
    const Cue *x = a, *y = b;
    if (x->c.start_ms != y->c.start_ms) return x->c.start_ms < y->c.start_ms ? -1 : 1;
    return x->order - y->order;
}

/* In order of start; ends settled (a cue nothing ended lasts until the next one starts - VobSub - or
   LAST_MS; none outlasts the next one's start); the ones that never show dropped. */
static void finalize(SubPic *s) {
    if (s->ncues > 1) qsort(s->cues, (size_t)s->ncues, sizeof *s->cues, cue_cmp);
    int64_t next = -1; /* the start of the first cue starting after cue i */
    for (int i = s->ncues - 1; i >= 0; i--) {
        Cue *c = &s->cues[i];
        if (i + 1 < s->ncues && s->cues[i + 1].c.start_ms > c->c.start_ms) next = s->cues[i + 1].c.start_ms;
        if (!c->ended) c->c.end_ms = s->vobsub && next >= 0 ? next : c->c.start_ms + LAST_MS;
        else if (next > c->c.start_ms && next < c->c.end_ms) c->c.end_ms = next;
    }
    int n = 0;
    for (int i = 0; i < s->ncues; i++) {
        Cue c = s->cues[i];
        if (c.c.start_ms < 0) c.c.start_ms = 0;
        if (c.c.end_ms <= c.c.start_ms || c.c.w <= 0 || c.c.h <= 0) continue;
        s->cues[n++] = c;
    }
    s->ncues = n;
}

static SubPic *sp_new(const MkvReader *r, char *err, size_t cap) {
    if (!r || !r->read_at || r->size < 0) {
        say(err, cap, "no file to read");
        return NULL;
    }
    SubPic *s = calloc(1, sizeof *s);
    if (!s) {
        say(err, cap, "out of memory");
        return NULL;
    }
    s->r = *r;
    return s;
}

void subpic_close(SubPic *s) {
    if (!s) return;
    free(s->cues);
    free(s->frags);
    free(s->pds);
    free(s->objs);
    free(s->comps);
    free(s);
}

int subpic_count(const SubPic *s) { return s ? s->ncues : 0; }

const SubPicCue *subpic_cue(const SubPic *s, int i) {
    return s && i >= 0 && i < s->ncues ? &s->cues[i].c : NULL;
}

void subpic_frame(const SubPic *s, int *w, int *h) {
    if (w) *w = s ? s->fw : 0;
    if (h) *h = s ? s->fh : 0;
}

/* ------------------------------------------------------------ PGS: indexing */

enum { SEG_PDS = 0x14, SEG_ODS = 0x15, SEG_PCS = 0x16, SEG_WDS = 0x17, SEG_END = 0x80 };

typedef struct {
    int64_t pay, next; /* its payload, the segment after it */
    uint32_t pts, len;
    int type;
} Seg;

/* A believable segment at `at`: "PG", a known type, and its size lands on the file's end or another "PG" */
static bool seg_at(const SubPic *s, int64_t at, Seg *g) {
    uint8_t h[13], m[2];
    if (!rd(s, at, h, sizeof h) || h[0] != 'P' || h[1] != 'G') return false;
    Seg t = {at + 13, at + 13 + be16(h + 11), be32(h + 2), be16(h + 11), h[10]};
    if (t.type != SEG_PDS && t.type != SEG_ODS && t.type != SEG_PCS && t.type != SEG_WDS && t.type != SEG_END)
        return false;
    if (t.next != s->r.size && (t.next > s->r.size || !rd(s, t.next, m, 2) || m[0] != 'P' || m[1] != 'G'))
        return false;
    *g = t; /* (g untouched unless it's one) */
    return true;
}

/* The next believable segment from `from` on, searching at most RESYNC_MAX bytes */
static bool seg_find(const SubPic *s, int64_t from, Seg *g) {
    uint8_t buf[4096];
    for (int64_t at = from; at >= 0 && at + 13 <= s->r.size && at - from < RESYNC_MAX;) {
        int64_t left = s->r.size - at;
        size_t n = left < (int64_t)sizeof buf ? (size_t)left : sizeof buf;
        if (!rd(s, at, buf, n)) return false;
        for (size_t i = 0; i + 1 < n; i++)
            if (buf[i] == 'P' && buf[i + 1] == 'G' && seg_at(s, at + (int64_t)i, g)) return true;
        at += (int64_t)n - 1; /* (a "PG" across the edge is seen next time) */
    }
    return false;
}

typedef struct {
    bool open; /* a display set's PCS was read, its END not yet */
    bool frame_ok;
    int64_t ms;
    int pal_id, ncomp;
    struct {
        int id, x, y, cx, cy, cw, ch;
        bool crop;
    } comp[PGS_COMPS];
} Ds;

typedef struct {
    Ds ds;
    int slot_id[PGS_SLOTS], slot_obj[PGS_SLOTS], nslots; /* the epoch's objects by id */
    int pal_head[256];                                    /* the epoch's newest segment of each palette */
    int pend, pend_id;                                    /* the object whose ODS are coming, -1 none */
    uint32_t pend_got;
    int last; /* the cue the last display set made, waiting for the next one to end it; -1 none */
} PgsIx;

static void epoch_reset(PgsIx *x) {
    x->nslots = 0;
    for (int i = 0; i < 256; i++) x->pal_head[i] = -1;
}

/* The object being gathered is complete (or cut short: what came is what it is): the epoch's of its id */
static void obj_done(PgsIx *x) {
    if (x->pend < 0) return;
    int i = 0;
    while (i < x->nslots && x->slot_id[i] != x->pend_id) i++;
    if (i == x->nslots && x->nslots < PGS_SLOTS) x->slot_id[x->nslots++] = x->pend_id;
    if (i < x->nslots) x->slot_obj[i] = x->pend;
    x->pend = -1;
}

static int obj_of(const PgsIx *x, int id) {
    for (int i = 0; i < x->nslots; i++)
        if (x->slot_id[i] == id) return x->slot_obj[i];
    return -1;
}

/* The display set read is complete: a cue when it shows anything */
static void ds_end(SubPic *s, PgsIx *x) {
    obj_done(x);
    Ds *d = &x->ds;
    if (!d->open) return;
    d->open = false;
    if (!d->frame_ok || !d->ncomp || s->ncues >= SUBPIC_MAX_CUES) return;
    int first = s->ncomps, n = 0, l = s->fw, t = s->fh, r = 0, b = 0;
    for (int k = 0; k < d->ncomp; k++) {
        int o = obj_of(x, d->comp[k].id);
        if (o < 0) continue; /* (not defined in this epoch: damaged) */
        const Obj *ob = &s->objs[o];
        int cx = 0, cy = 0, cw = ob->w, ch = ob->h;
        if (d->comp[k].crop) {
            cx = imax(d->comp[k].cx, 0), cy = imax(d->comp[k].cy, 0);
            cw = imin(d->comp[k].cx + d->comp[k].cw, ob->w) - cx, ch = imin(d->comp[k].cy + d->comp[k].ch, ob->h) - cy;
        }
        if (cw <= 0 || ch <= 0) continue;
        int X = d->comp[k].x, Y = d->comp[k].y;
        int l1 = imax(X, 0), t1 = imax(Y, 0), r1 = imin(X + cw, s->fw), b1 = imin(Y + ch, s->fh);
        if (r1 <= l1 || b1 <= t1) continue; /* (off the frame) */
        bool ok;
        s->comps = grow(s->comps, &s->capcomps, s->ncomps, sizeof *s->comps, MAX_COMPS, &ok);
        if (!ok) break;
        s->comps[s->ncomps++] = (Comp){o, X, Y, cx, cy, cw, ch};
        n++;
        l = imin(l, l1), t = imin(t, t1), r = imax(r, r1), b = imax(b, b1);
    }
    Cue *c = n ? cue_new(s) : NULL;
    if (!c) {
        s->ncomps = first;
        return;
    }
    c->c = (SubPicCue){d->ms, 0, l, t, r - l, b - t};
    c->comp = first, c->ncomp = n;
    c->pal = x->pal_head[d->pal_id];
    x->last = s->ncues++;
}

static void pgs_pcs(SubPic *s, PgsIx *x, const Seg *g) {
    ds_end(s, x); /* (one without its END) */
    int64_t ms = g->pts / 90;
    if (x->last >= 0) { /* the cue before ends where this display set begins */
        s->cues[x->last].c.end_ms = ms;
        s->cues[x->last].ended = true;
        x->last = -1;
    }
    uint8_t p[PCS_READ];
    size_t n = g->len < sizeof p ? g->len : sizeof p;
    if (n < 11 || !rd(s, g->pay, p, n)) return;
    int w = (int)be16(p), h = (int)be16(p + 2), nobj = p[10];
    if (p[7] & 0x80) epoch_reset(x); /* composition state: epoch start */
    bool frame_ok = w >= 1 && h >= 1 && w <= SUBPIC_MAX_SIDE && h <= SUBPIC_MAX_SIDE;
    if (frame_ok && !s->fw) s->fw = w, s->fh = h;
    Ds *d = &x->ds;
    d->open = true;
    d->frame_ok = frame_ok && w == s->fw && h == s->fh;
    d->ms = ms;
    d->pal_id = p[9];
    d->ncomp = 0;
    size_t at = 11;
    for (int k = 0; k < nobj && at + 8 <= n; k++) {
        const uint8_t *q = p + at;
        bool crop = q[3] & 0x80;
        at += 8;
        if (crop && at + 8 > n) break;
        if (d->ncomp < PGS_COMPS) {
            d->comp[d->ncomp].id = (int)be16(q), d->comp[d->ncomp].crop = crop;
            d->comp[d->ncomp].x = (int)be16(q + 4), d->comp[d->ncomp].y = (int)be16(q + 6);
            if (crop) {
                d->comp[d->ncomp].cx = (int)be16(q + 8), d->comp[d->ncomp].cy = (int)be16(q + 10);
                d->comp[d->ncomp].cw = (int)be16(q + 12), d->comp[d->ncomp].ch = (int)be16(q + 14);
            }
            d->ncomp++;
        }
        if (crop) at += 8;
    }
}

static void pgs_pds(SubPic *s, PgsIx *x, const Seg *g) {
    uint8_t id;
    if (g->len < 2 || !rd(s, g->pay, &id, 1)) return;
    bool ok;
    s->pds = grow(s->pds, &s->cappds, s->npds, sizeof *s->pds, MAX_PDS, &ok);
    if (!ok) return;
    s->pds[s->npds] = (Pds){g->pay, g->len, x->pal_head[id]};
    x->pal_head[id] = s->npds++;
}

/* RLE bytes of the object being gathered */
static void obj_data(SubPic *s, PgsIx *x, int64_t off, uint32_t len) {
    Obj *o = &s->objs[x->pend];
    if (len > OBJ_BYTES - x->pend_got) len = OBJ_BYTES - x->pend_got;
    if (!len || o->nfrag >= OBJ_FRAGS || o->frag + o->nfrag != s->nfrags || !frag_add(s, off, len)) return;
    o->nfrag++;
    x->pend_got += len;
}

static void pgs_ods(SubPic *s, PgsIx *x, const Seg *g) {
    uint8_t p[11];
    if (g->len < 4 || !rd(s, g->pay, p, g->len < 11 ? g->len : 11)) return;
    int id = (int)be16(p), seq = p[3];
    if (seq & 0x80) { /* the first of its segments: a new object */
        obj_done(x);
        if (g->len < 11) return;
        uint32_t len = (uint32_t)p[4] << 16 | (uint32_t)p[5] << 8 | p[6];
        int w = (int)be16(p + 7), h = (int)be16(p + 9);
        if (w < 1 || h < 1 || w > SUBPIC_MAX_SIDE || h > SUBPIC_MAX_SIDE) return;
        bool ok;
        s->objs = grow(s->objs, &s->capobjs, s->nobjs, sizeof *s->objs, MAX_OBJS, &ok);
        if (!ok) return;
        s->objs[s->nobjs] = (Obj){w, h, len >= 4 ? len - 4 : 0, s->nfrags, 0};
        x->pend = s->nobjs++, x->pend_id = id, x->pend_got = 0;
        obj_data(s, x, g->pay + 11, g->len - 11);
    } else if (x->pend >= 0 && x->pend_id == id) {
        obj_data(s, x, g->pay + 4, g->len - 4);
    } else return; /* (a piece of an object whose start is lost) */
    if (seq & 0x40) obj_done(x); /* the last of its segments */
}

SubPic *subpic_open_sup(const MkvReader *sup, char *err, size_t cap) {
    SubPic *s = sp_new(sup, err, cap);
    if (!s) return NULL;
    PgsIx x;
    memset(&x, 0, sizeof x);
    x.pend = x.last = -1;
    epoch_reset(&x);
    Seg g;
    bool any = false, have = seg_at(s, 0, &g) || seg_find(s, 1, &g);
    while (have) {
        any = true;
        switch (g.type) {
        case SEG_PCS: pgs_pcs(s, &x, &g); break;
        case SEG_PDS: pgs_pds(s, &x, &g); break;
        case SEG_ODS: pgs_ods(s, &x, &g); break;
        case SEG_END: ds_end(s, &x); break;
        default: break; /* WDS: windows only bound where objects may go */
        }
        if (s->ncues >= SUBPIC_MAX_CUES && x.last < 0) break; /* (full, and the last cue has its end) */
        int64_t next = g.next;
        if (next >= s->r.size) break;
        have = seg_at(s, next, &g) || seg_find(s, next + 1, &g);
    }
    ds_end(s, &x);
    if (!any) {
        subpic_close(s);
        say(err, cap, "no PGS segments (not a .sup file)");
        return NULL;
    }
    if (!s->fw) s->fw = 1920, s->fh = 1080;
    s->hd = s->fh > 576; /* BT.709 colours for HD, BT.601 for SD */
    finalize(s);
    return s;
}

/* ------------------------------------------------------------ PGS: drawing */

static uint8_t clamp8(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5); }

/* A PGS palette entry (limited-range Y'CbCr) as 0xRRGGBBAA */
static uint32_t ycc_rgba(int y, int cr, int cb, int a, bool hd) {
    double Y = (y - 16) * (255.0 / 219.0), Cb = (cb - 128) * (255.0 / 224.0), Cr = (cr - 128) * (255.0 / 224.0);
    double r, g, b;
    if (hd) r = Y + 1.5748 * Cr, g = Y - 0.187324 * Cb - 0.468124 * Cr, b = Y + 1.8556 * Cb;
    else r = Y + 1.402 * Cr, g = Y - 0.344136 * Cb - 0.714136 * Cr, b = Y + 1.772 * Cb;
    return (uint32_t)clamp8(r) << 24 | (uint32_t)clamp8(g) << 16 | (uint32_t)clamp8(b) << 8 | (uint32_t)a;
}

/* One shown object into the cue's picture. PGS RLE: a non-zero byte = one pixel of it; 0x00 then: 0x00 =
   the line's end, 00LLLLLL = L of colour 0, 01LLLLLL LLLLLLLL = L (14 bits) of 0, 10LLLLLL C = L of C,
   11LLLLLL LLLLLLLL C = L (14 bits) of C. A run past the line's end is cut there. */
static bool draw_obj(const SubPic *s, const Cue *c, const Comp *m, uint8_t *px) {
    const Obj *o = &s->objs[m->obj];
    uint64_t total = 0;
    for (int i = 0; i < o->nfrag; i++) total += s->frags[o->frag + i].len;
    if (total > o->bytes) total = o->bytes;
    if (!total) return true;
    uint8_t *d = malloc((size_t)total);
    if (!d) return false;
    if (!frag_read(s, o->frag, o->nfrag, 0, d, (size_t)total)) {
        free(d);
        return false;
    }
    int ox = 0, oy = 0;
    for (size_t i = 0; i < total && oy < o->h;) {
        int col = d[i++], run = 1;
        if (!col) {
            if (i >= total) break;
            int f = d[i++];
            if (!f) {
                oy++, ox = 0;
                continue;
            }
            run = f & 0x3F;
            if (f & 0x40) {
                if (i >= total) break;
                run = run << 8 | d[i++];
            }
            if (f & 0x80) {
                if (i >= total) break;
                col = d[i++];
            }
        }
        if (oy >= m->cy && oy < m->cy + m->ch && ox < o->w) {
            int l = imax(ox, m->cx), r = imin(imin(ox + run, o->w), m->cx + m->cw);
            if (l < r) fill(px, &c->c, m->x + l - m->cx, m->y + oy - m->cy, r - l, col);
        }
        ox = imin(ox + run, o->w);
    }
    free(d);
    return true;
}

static bool render_pgs(const SubPic *s, const Cue *c, uint8_t *px, uint32_t pal[256]) {
    /* the palette: its newest segment first, older ones of the epoch fill in what it doesn't say */
    bool set[256] = {false};
    uint8_t buf[2 + 256 * 5];
    int k = 0;
    for (int p = c->pal; p >= 0 && k < PAL_CHAIN; p = s->pds[p].prev, k++) {
        const Pds *d = &s->pds[p];
        size_t n = d->len < sizeof buf ? d->len : sizeof buf;
        if (!rd(s, d->off, buf, n)) return false;
        for (size_t e = (n - 2) / 5; e-- > 0;) { /* (later entries in a segment win) */
            const uint8_t *q = buf + 2 + e * 5;
            if (set[q[0]]) continue;
            set[q[0]] = true;
            pal[q[0]] = ycc_rgba(q[1], q[2], q[3], q[4], s->hd);
        }
    }
    /* what no object covers: the clearest entry (one never defined is fully clear) */
    int bg = 0;
    for (int i = 1; i < 256; i++)
        if ((pal[i] & 0xFF) < (pal[bg] & 0xFF)) bg = i;
    memset(px, bg, (size_t)c->c.w * (size_t)c->c.h);
    for (int i = 0; i < c->ncomp; i++)
        if (!draw_obj(s, c, &s->comps[c->comp + i], px)) return false;
    return true;
}

/* ------------------------------------------------------------ VobSub: indexing */

/* Gather the SPU at pos: its private stream 1 packets' payloads (one substream), appended to frags.
   Returns its size, or -1 (frags past their count then are the caller's to drop). */
static int spu_gather(SubPic *s, int64_t pos) {
    int sid = -1, f0 = s->nfrags;
    uint32_t have = 0, need = 0;
    int64_t start = pos;
    uint8_t h[40];
    for (int k = 0; k < SPU_PACKETS * 4 && pos - start < SPU_SCAN; k++) {
        if (!rd(s, pos, h, 4) || h[0] || h[1] || h[2] != 1) return -1;
        int code = h[3];
        if (code == 0xBA) { /* pack header: MPEG-2 (14 bytes + stuffing) or MPEG-1 (12) */
            if (!rd(s, pos + 4, h, 10)) return -1;
            if ((h[0] & 0xC0) == 0x40) pos += 14 + (h[9] & 7);
            else if ((h[0] & 0xF0) == 0x20) pos += 12;
            else return -1;
            continue;
        }
        if (code < 0xBB || !rd(s, pos + 4, h, 2)) return -1; /* (0xB9 = the end) */
        uint32_t len = be16(h);
        int64_t body = pos + 6;
        pos = body + len;
        if (pos > s->r.size) return -1;
        if (code != 0xBD) continue; /* (padding, other streams) */
        size_t hn = len < sizeof h ? len : sizeof h, at = 0;
        if (!rd(s, body, h, hn)) return -1;
        if (hn >= 3 && (h[0] & 0xC0) == 0x80) at = 3 + (size_t)h[2]; /* MPEG-2 PES header */
        else {                                                        /* MPEG-1 */
            while (at < hn && at < 16 && h[at] == 0xFF) at++;
            if (at < hn && (h[at] & 0xC0) == 0x40) at += 2;
            if (at >= hn) continue;
            if ((h[at] & 0xF0) == 0x20) at += 5;
            else if ((h[at] & 0xF0) == 0x30) at += 10;
            else if (h[at] == 0x0F) at += 1;
            else continue;
        }
        if (at >= len) continue;
        uint8_t id;
        if (!rd(s, body + (int64_t)at, &id, 1)) return -1;
        if (sid < 0) {
            if (id < 0x20 || id > 0x3F) return -1;
            sid = id;
        } else if (id != sid) continue;
        uint32_t n = len - (uint32_t)at - 1;
        if (!n) continue;
        if (s->nfrags - f0 >= SPU_PACKETS || !frag_add(s, body + (int64_t)at + 1, n)) return -1;
        have += n;
        if (!need && have >= 2) {
            uint8_t sz[2];
            if (!frag_read(s, f0, s->nfrags - f0, 0, sz, 2)) return -1;
            need = be16(sz);
            if (need < 4) return -1;
        }
        if (need && have >= need) {
            s->frags[s->nfrags - 1].len -= have - need; /* (what follows the SPU in its last packet) */
            return (int)need;
        }
    }
    return -1;
}

/* The SPU's control sequences: its start/stop dates, colours, contrast, area and field offsets. False
   when it can't be drawn. */
static bool spu_control(SubPic *s, Cue *c, int64_t ms, uint8_t *buf) {
    uint8_t hd[4];
    if (!frag_read(s, c->frag, c->nfrag, 0, hd, 4)) return false;
    uint32_t ctrl = be16(hd + 2), size = c->size;
    if (ctrl < 4 || ctrl + 4 > size) return false;
    if (!frag_read(s, c->frag, c->nfrag, ctrl, buf, size - ctrl)) return false;
    bool started = false, stopped = false, area = false, fields = false;
    unsigned start = 0, stop = 0;
    for (int k = 0; k < 4; k++) c->col[k] = (uint8_t)k, c->alpha[k] = k ? 15 : 0;
    uint32_t off = ctrl;
    for (int k = 0; k < DCSQ_MAX && off + 4 <= size; k++) {
        const uint8_t *q = buf + (off - ctrl);
        unsigned date = be16(q), next = be16(q + 2);
        for (uint32_t p = off + 4; p < size;) {
            const uint8_t *a = buf + (p - ctrl) + 1;
            uint32_t left = size - p - 1; /* argument bytes there */
            int cmd = buf[p - ctrl];
            if (cmd == 0x00 || cmd == 0x01) { /* (forced) start */
                if (!started) started = true, start = date;
                p += 1;
            } else if (cmd == 0x02) { /* stop */
                if (!stopped) stopped = true, stop = date;
                p += 1;
            } else if (cmd == 0x03 && left >= 2) { /* colours of pixel values 3, 2, 1, 0 */
                c->col[3] = a[0] >> 4, c->col[2] = a[0] & 15, c->col[1] = a[1] >> 4, c->col[0] = a[1] & 15;
                p += 3;
            } else if (cmd == 0x04 && left >= 2) { /* their contrast (alpha, 0-15) */
                c->alpha[3] = a[0] >> 4, c->alpha[2] = a[0] & 15, c->alpha[1] = a[1] >> 4, c->alpha[0] = a[1] & 15;
                p += 3;
            } else if (cmd == 0x05 && left >= 6) { /* area: x1, x2, y1, y2 (12 bits each, inclusive) */
                int x1 = a[0] << 4 | a[1] >> 4, x2 = (a[1] & 15) << 8 | a[2];
                int y1 = a[3] << 4 | a[4] >> 4, y2 = (a[4] & 15) << 8 | a[5];
                c->ax = x1, c->ay = y1, c->aw = x2 - x1 + 1, c->ah = y2 - y1 + 1;
                area = c->aw > 0 && c->ah > 0;
                p += 7;
            } else if (cmd == 0x06 && left >= 4) { /* the top and bottom field's RLE */
                c->top = be16(a), c->bottom = be16(a + 2);
                fields = true;
                p += 5;
            } else if (cmd == 0x07 && left >= 2 && be16(a) >= 2) { /* colour/contrast changes: skipped */
                p += 1 + be16(a);
            } else break; /* 0xFF the end, or something unknown or cut short */
        }
        if (next <= off) break; /* (the last one points at itself) */
        off = next;
    }
    if (!area || !fields || c->top < 4 || c->top >= size || c->bottom < 4 || c->bottom >= size) return false;
    c->c.start_ms = ms + (int64_t)start * 1024 / 90; /* dates count 1024/90000 s */
    if (stopped && stop > start) c->c.end_ms = ms + (int64_t)stop * 1024 / 90, c->ended = true;
    return true;
}

static void vob_cue(SubPic *s, int64_t ms, int64_t pos, uint8_t *buf) {
    if (pos < 0 || pos >= s->r.size) return;
    Cue *c = cue_new(s);
    if (!c) return;
    int f0 = s->nfrags, size = spu_gather(s, pos);
    c->frag = f0, c->nfrag = s->nfrags - f0, c->size = size > 0 ? (uint32_t)size : 0;
    if (size < 0 || !spu_control(s, c, ms, buf)) {
        s->nfrags = f0;
        return;
    }
    s->ncues++;
}

static const char *skip_sp(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static bool key(const char *line, const char *k, const char **rest) {
    size_t n = strlen(k);
    if (strncmp(line, k, n)) return false;
    *rest = skip_sp(line + n);
    return true;
}

static bool dec(const char **pp, int maxd, int64_t *v) {
    const char *p = *pp;
    int64_t x = 0;
    int n = 0;
    while (n < maxd && *p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0'), n++;
    *pp = p, *v = x;
    return n > 0;
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool hex(const char **pp, int maxd, uint64_t *v) {
    const char *p = *pp;
    uint64_t x = 0;
    int n = 0;
    while (n < maxd && hexval(*p) >= 0) x = x << 4 | (uint64_t)hexval(*p++), n++;
    *pp = p, *v = x;
    return n > 0;
}

/* [-]HH:MM:SS:mmm (or .mmm) as ms */
static bool clock_ms(const char **pp, int64_t *ms) {
    const char *p = *pp;
    bool neg = *p == '-';
    if (neg) p++;
    int64_t hh, mm, ss, mmm;
    if (!dec(&p, 6, &hh) || *p++ != ':' || !dec(&p, 2, &mm) || *p++ != ':' || !dec(&p, 2, &ss) ||
        (*p != ':' && *p != '.') || (p++, !dec(&p, 3, &mmm)))
        return false;
    *ms = ((hh * 60 + mm) * 60 + ss) * 1000 + mmm;
    if (neg) *ms = -*ms;
    *pp = p;
    return true;
}

/* "palette: rrggbb, rrggbb, ..." / "colors: ..." */
static void colours(const char *p, uint32_t *out, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t v;
        p = skip_sp(p);
        if (!hex(&p, 8, &v)) return;
        out[i] = (uint32_t)(v & 0xFFFFFF);
        p = skip_sp(p);
        if (*p == ',') p++;
    }
}

SubPic *subpic_open_vobsub(const char *idx, size_t idx_len, const MkvReader *sub, char *err, size_t cap) {
    SubPic *s = sp_new(sub, err, cap);
    if (!s) return NULL;
    s->vobsub = true;
    for (int i = 0; i < 16; i++) s->vpal[i] = (uint32_t)(i * 17) * 0x010101u; /* (no palette: greys) */
    uint8_t *buf = malloc(65536);                                           /* an SPU's control part */
    if (!buf) {
        subpic_close(s);
        say(err, cap, "out of memory");
        return NULL;
    }
    bool known = false, track = false;
    int64_t delay = 0;
    const char *p = idx ? idx : "", *end = p + (idx ? idx_len : 0);
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        char line[1024];
        size_t n = (size_t)(e - p) < sizeof line - 1 ? (size_t)(e - p) : sizeof line - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        p = e < end ? e + 1 : end;
        const char *l = skip_sp(line), *r;
        int64_t a, b;
        uint64_t u;
        if (key(l, "size:", &r)) {
            if (dec(&r, 5, &a) && *r++ == 'x' && dec(&r, 5, &b) && a >= 1 && b >= 1 && a <= SUBPIC_MAX_SIDE &&
                b <= SUBPIC_MAX_SIDE)
                s->fw = (int)a, s->fh = (int)b;
            known = true;
        } else if (key(l, "palette:", &r)) {
            colours(r, s->vpal, 16);
            known = true;
        } else if (key(l, "custom colors:", &r)) {
            s->custom = !strncmp(r, "ON", 2);
            const char *t = strstr(r, "tridx:"), *k = strstr(r, "colors:");
            if (t)
                for (int i = 0, j = 0; i < 4; i++) { /* the first digit is pixel value 0's */
                    t = skip_sp(j ? t : t + 6), j = 1;
                    if (*t != '0' && *t != '1') break;
                    if (*t++ == '1') s->tridx |= (uint8_t)(1 << i);
                }
            if (k) colours(k + 7, s->cust, 4);
        } else if (key(l, "id:", &r)) {
            if (track) break; /* (the first track only) */
            track = known = true;
            delay = 0;
        } else if (key(l, "delay:", &r)) {
            int64_t ms;
            if (clock_ms(&r, &ms)) delay = ms;
        } else if (key(l, "timestamp:", &r)) {
            known = true;
            int64_t ms;
            const char *f;
            if (s->ncues < SUBPIC_MAX_CUES && clock_ms(&r, &ms) && (f = strstr(r, "filepos:")) &&
                (f = skip_sp(f + 8), hex(&f, 16, &u)) && u < (uint64_t)INT64_MAX)
                vob_cue(s, ms + delay, (int64_t)u, buf);
        }
    }
    free(buf);
    if (!known) {
        subpic_close(s);
        say(err, cap, "not a VobSub index (no size, palette or timestamp lines)");
        return NULL;
    }
    if (!s->fw) { /* no size line: NTSC, or PAL when a picture reaches below 480 lines */
        s->fw = 720, s->fh = 480;
        for (int i = 0; i < s->ncues; i++)
            if (s->cues[i].ay + s->cues[i].ah > 480) s->fh = 576;
    }
    for (int i = 0; i < s->ncues; i++) { /* the rectangle: the area, on the frame */
        Cue *c = &s->cues[i];
        int l = imax(c->ax, 0), t = imax(c->ay, 0), r = imin(c->ax + c->aw, s->fw), b = imin(c->ay + c->ah, s->fh);
        c->c.x = l, c->c.y = t, c->c.w = r > l ? r - l : 0, c->c.h = b > t ? b - t : 0;
    }
    finalize(s);
    return s;
}

/* ------------------------------------------------------------ VobSub: drawing */

/* Both fields of the SPU's area: nibble codes - 1 nibble when >= 0x4, 2 when >= 0x10, 3 when >= 0x40,
   else 4; value >> 2 = the run (0: to the line's end), & 3 = the pixel value; each line byte-aligned. The
   top field holds the even lines, the bottom field the odd ones. */
static bool render_vob(const SubPic *s, const Cue *c, uint8_t *px, uint32_t pal[256]) {
    uint8_t *d = malloc(c->size);
    if (!d) return false;
    if (!frag_read(s, c->frag, c->nfrag, 0, d, c->size)) {
        free(d);
        return false;
    }
    memset(px, 4, (size_t)c->c.w * (size_t)c->c.h); /* 4: not drawn (data cut short) - clear */
    uint32_t endn = c->size * 2;
    for (int field = 0; field < 2; field++) {
        uint32_t nib = (field ? c->bottom : c->top) * 2;
        for (int row = field; row < c->ah; row += 2) {
            for (int x = 0; x < c->aw;) {
                unsigned v = 0;
                for (int k = 0; k < 4 && (k == 0 || v < (k == 1 ? 0x4u : k == 2 ? 0x10u : 0x40u)); k++) {
                    if (nib >= endn) goto next_field;
                    v = v << 4 | (unsigned)(nib & 1 ? d[nib / 2] & 15 : d[nib / 2] >> 4);
                    nib++;
                }
                int run = (int)(v >> 2);
                if (run == 0 || run > c->aw - x) run = c->aw - x;
                fill(px, &c->c, c->ax + x, c->ay + row, run, (int)(v & 3));
                x += run;
            }
            nib = (nib + 1) & ~1u;
        }
    next_field:;
    }
    free(d);
    for (int k = 0; k < 4; k++) {
        uint32_t rgb = s->custom ? s->cust[k] : s->vpal[c->col[k] & 15];
        uint32_t a = s->custom && s->tridx >> k & 1 ? 0 : c->alpha[k] * 17u;
        pal[k] = rgb << 8 | a;
    }
    pal[4] = 0;
    return true;
}

bool subpic_render(SubPic *s, int i, uint8_t *px, size_t cap, uint32_t pal[256], int *ncolors) {
    if (!s || i < 0 || i >= s->ncues || !px || !pal || !ncolors) return false;
    const Cue *c = &s->cues[i];
    size_t n = (size_t)c->c.w * (size_t)c->c.h;
    if (!n || cap < n) return false;
    memset(pal, 0, 256 * sizeof *pal);
    if (!(s->vobsub ? render_vob(s, c, px, pal) : render_pgs(s, c, px, pal))) return false;
    int top = 0;
    for (size_t k = 0; k < n; k++)
        if (px[k] > top) top = px[k];
    *ncolors = top + 1;
    return true;
}
