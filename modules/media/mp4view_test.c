/* Sound languages (2.2): an MP4's sound tracks as the probe lists them, the one-sound view of it (mp4view.c) - every
 * sample of what's kept found again at its new place, moov first or last, co64 in and out, 4 GB passed - what can't
 * have a view refused without a crash, and /media/file/<id>?a=<n> serving it over a real socket. The MP4s are written
 * by hand here (modules/av's writer is another worker's), samples of known bytes so any misplaced offset shows. */
#include "../../core/auth.h"
#include "../../core/plat.h"
#include "../../core/server.h"
#include "../../tests/harness.h"
#include "library.h"
#include "media.h"
#include "mediainfo.h"

int media_view_test_builds(void); /* (stream.c: views made since media_stream_resume) */

/* ------------------------------------------------------------- writing MP4s */

typedef struct {
    bool video;
    uint32_t id;
    const char *lang;      /* three letters, NULL = 0 (none) */
    const char *hdlr_name; /* NULL = "SoundHandler" / "VideoHandler" */
    bool pstring;          /* hdlr name as a QuickTime Pascal string */
    const char *udta_name; /* trak/udta/name: what ffmpeg writes a title as */
    bool on;               /* tkhd enabled */
    uint32_t tref_to;      /* a tref 'sync' naming this track_ID (0 = none) */
} TT;

typedef struct {
    TT t[8];
    int n;
    int rounds;     /* video: one sample a chunk, `rounds` chunks; sound: two samples a chunk */
    bool moov_last; /* ftyp, mdat, moov */
    bool co64;      /* chunk offsets as co64 */
    bool mvex;      /* a fragmented file's moov */
    bool free_box;  /* a 'free' box between moov and mdat (Hemma's own copies have one) */
} TM;

static int spc(const TT *t) { return t->video ? 1 : 2; }
static uint32_t sample_size(const TT *t, int i) { return t->video ? 40u + (uint32_t)(i % 3) : 12u + (uint32_t)(i % 2); }
static uint8_t pattern(uint32_t id, int i, size_t j) { return (uint8_t)(id * 31u + (unsigned)i * 7u + (unsigned)j); }

static void p8(Buf *b, uint32_t v) {
    uint8_t c = (uint8_t)v;
    buf_put(b, &c, 1);
}
static void p16(Buf *b, uint32_t v) { p8(b, v >> 8), p8(b, v); }
static void p32(Buf *b, uint32_t v) { p16(b, v >> 16), p16(b, v); }
static void p64(Buf *b, uint64_t v) { p32(b, (uint32_t)(v >> 32)), p32(b, (uint32_t)v); }
static void pz(Buf *b, size_t n) {
    while (n--) p8(b, 0);
}
static size_t bo(Buf *b, const char *type) {
    size_t at = b->len;
    p32(b, 0);
    buf_put(b, type, 4);
    return at;
}
static size_t fbo(Buf *b, const char *type, int version, uint32_t flags) {
    size_t at = bo(b, type);
    p8(b, (uint32_t)version);
    p8(b, flags >> 16), p16(b, flags);
    return at;
}
static void bc(Buf *b, size_t at) {
    uint32_t n = (uint32_t)(b->len - at);
    uint8_t *p = (uint8_t *)b->p + at;
    p[0] = (uint8_t)(n >> 24), p[1] = (uint8_t)(n >> 16), p[2] = (uint8_t)(n >> 8), p[3] = (uint8_t)n;
}

static uint16_t lang_code(const char *l) {
    if (!l) return 0;
    return (uint16_t)(((l[0] - 0x60) & 31) << 10 | ((l[1] - 0x60) & 31) << 5 | ((l[2] - 0x60) & 31));
}

/* The moov, chunk offsets given (absolute, per track, `rounds` each). */
static void moov(Buf *b, const TM *m, uint64_t *const offs[8]) {
    size_t mv = bo(b, "moov");
    size_t h = fbo(b, "mvhd", 0, 0);
    pz(b, 8), p32(b, 1000), p32(b, 10000), p32(b, 0x00010000), p16(b, 0x0100), pz(b, 10);
    pz(b, 36), pz(b, 24), p32(b, 99);
    bc(b, h);
    for (int k = 0; k < m->n; k++) {
        const TT *t = &m->t[k];
        size_t tr = bo(b, "trak");
        h = fbo(b, "tkhd", 0, t->on ? 3 : 2);
        pz(b, 8), p32(b, t->id), pz(b, 4), p32(b, 10000), pz(b, 8), p16(b, 0), p16(b, t->video ? 0 : 1);
        p16(b, t->video ? 0 : 0x0100), p16(b, 0), pz(b, 36), p32(b, t->video ? 64u << 16 : 0), p32(b, t->video ? 48u << 16 : 0);
        bc(b, h);
        if (t->tref_to) {
            size_t tf = bo(b, "tref"), s = bo(b, "sync");
            p32(b, t->tref_to);
            bc(b, s), bc(b, tf);
        }
        if (t->udta_name) {
            size_t u = bo(b, "udta"), nm = bo(b, "name");
            buf_put(b, t->udta_name, strlen(t->udta_name));
            bc(b, nm), bc(b, u);
        }
        size_t md = bo(b, "mdia");
        h = fbo(b, "mdhd", 0, 0);
        pz(b, 8), p32(b, t->video ? 1000 : 48000), p32(b, t->video ? 10000 : 480000), p16(b, lang_code(t->lang)), p16(b, 0);
        bc(b, h);
        h = fbo(b, "hdlr", 0, 0);
        p32(b, 0), buf_put(b, t->video ? "vide" : "soun", 4), pz(b, 12);
        const char *name = t->hdlr_name ? t->hdlr_name : t->video ? "VideoHandler" : "SoundHandler";
        if (t->pstring) p8(b, (uint32_t)strlen(name)), buf_put(b, name, strlen(name));
        else buf_put(b, name, strlen(name) + 1);
        bc(b, h);
        size_t mi = bo(b, "minf");
        h = t->video ? fbo(b, "vmhd", 0, 1) : fbo(b, "smhd", 0, 0);
        pz(b, t->video ? 8 : 4);
        bc(b, h);
        size_t di = bo(b, "dinf"), dr = fbo(b, "dref", 0, 0);
        p32(b, 1);
        bc(b, fbo(b, "url ", 0, 1));
        bc(b, dr), bc(b, di);
        size_t st = bo(b, "stbl");
        size_t sd = fbo(b, "stsd", 0, 0);
        p32(b, 1);
        if (t->video) {
            size_t e = bo(b, "avc1");
            pz(b, 6), p16(b, 1), pz(b, 16), p16(b, 64), p16(b, 48), pz(b, 78 - 28);
            bc(b, e);
        } else {
            size_t e = bo(b, "mp4a");
            pz(b, 6), p16(b, 1), pz(b, 8), p16(b, 2), p16(b, 16), pz(b, 4), p32(b, 48000u << 16);
            bc(b, e);
        }
        bc(b, sd);
        int ns = m->rounds * spc(t);
        h = fbo(b, "stts", 0, 0);
        p32(b, 1), p32(b, (uint32_t)ns), p32(b, t->video ? 100 : 1024);
        bc(b, h);
        h = fbo(b, "stsc", 0, 0);
        p32(b, 1), p32(b, 1), p32(b, (uint32_t)spc(t)), p32(b, 1);
        bc(b, h);
        h = fbo(b, "stsz", 0, 0);
        p32(b, 0), p32(b, (uint32_t)ns);
        for (int i = 0; i < ns; i++) p32(b, sample_size(t, i));
        bc(b, h);
        h = fbo(b, m->co64 ? "co64" : "stco", 0, 0);
        p32(b, (uint32_t)m->rounds);
        for (int r = 0; r < m->rounds; r++) {
            if (m->co64) p64(b, offs[k][r]);
            else p32(b, (uint32_t)offs[k][r]);
        }
        bc(b, h);
        bc(b, st), bc(b, mi), bc(b, md), bc(b, tr);
    }
    if (m->mvex) {
        size_t x = bo(b, "mvex"), t = fbo(b, "trex", 0, 0);
        p32(b, 1), p32(b, 1), pz(b, 12);
        bc(b, t), bc(b, x);
    }
    bc(b, mv);
}

/* The chunks of every track, round by round (video, then each sound), as one mdat payload; rel offsets out. */
static void media_bytes(Buf *b, const TM *m, uint64_t *rel[8]) {
    for (int r = 0; r < m->rounds; r++)
        for (int k = 0; k < m->n; k++) {
            const TT *t = &m->t[k];
            rel[k][r] = b->len;
            for (int s = 0; s < spc(t); s++) {
                int i = r * spc(t) + s;
                for (size_t j = 0; j < sample_size(t, i); j++) p8(b, pattern(t->id, i, j));
            }
        }
}

static void ftyp(Buf *b) {
    size_t f = bo(b, "ftyp");
    buf_put(b, "isom", 4), p32(b, 512), buf_put(b, "isomiso2mp41", 12);
    bc(b, f);
}

static void make_mp4(Buf *out, const TM *m) {
    uint64_t *rel[8], *abs_[8];
    for (int k = 0; k < 8; k++) rel[k] = xcalloc((size_t)m->rounds, 8), abs_[k] = xcalloc((size_t)m->rounds, 8);
    Buf mdat = {0}, mv = {0};
    media_bytes(&mdat, m, rel);
    buf_reset(out);
    ftyp(out);
    uint64_t base;
    if (m->moov_last) {
        base = out->len + 8;
    } else {
        for (int k = 0; k < m->n; k++)
            for (int r = 0; r < m->rounds; r++) abs_[k][r] = 0;
        moov(&mv, m, abs_);
        base = out->len + mv.len + (m->free_box ? 24 : 0) + 8;
    }
    for (int k = 0; k < m->n; k++)
        for (int r = 0; r < m->rounds; r++) abs_[k][r] = base + rel[k][r];
    buf_reset(&mv);
    moov(&mv, m, abs_);
    if (!m->moov_last) {
        buf_put(out, mv.p, mv.len);
        if (m->free_box) {
            size_t f = bo(out, "free");
            pz(out, 16);
            bc(out, f);
        }
    }
    p32(out, (uint32_t)(mdat.len + 8)), buf_put(out, "mdat", 4), buf_put(out, mdat.p, mdat.len);
    if (m->moov_last) buf_put(out, mv.p, mv.len);
    buf_free(&mdat);
    buf_free(&mv);
    for (int k = 0; k < 8; k++) free(rel[k]), free(abs_[k]);
}

/* the household's kind of file: a picture, then English (the film's own, not the default), German, Swedish */
static TM three_sounds(void) {
    TM m = {.n = 4, .rounds = 6};
    m.t[0] = (TT){.video = true, .id = 1, .lang = "und", .on = true};
    m.t[1] = (TT){.id = 2, .lang = "eng", .hdlr_name = "Original"};
    m.t[2] = (TT){.id = 3, .lang = "ger", .udta_name = "Synchro, Kino", .on = true, .tref_to = 1};
    m.t[3] = (TT){.id = 4, .lang = "swe", .tref_to = 2};
    return m;
}

/* ------------------------------------------------------------- reading */

typedef struct {
    const uint8_t *p;
    int64_t n;
} Mem;

static bool mem_at(void *ctx, int64_t off, void *dst, size_t n) {
    const Mem *m = ctx;
    if (off < 0 || off > m->n || (int64_t)n > m->n - off) return false;
    memcpy(dst, m->p + off, n);
    return true;
}

static MediaReader mem_reader(Mem *m, const Buf *b) {
    *m = (Mem){(const uint8_t *)b->p, (int64_t)b->len};
    return (MediaReader){m, m->n, mem_at};
}

/* A view as a reader: its parts, the file's bytes read from the original's reader */
typedef struct {
    const MediaView *v;
    const MediaReader *orig;
} ViewR;

static bool view_at(void *ctx, int64_t off, void *dst, size_t n) {
    const ViewR *w = ctx;
    uint8_t *d = dst;
    while (n) {
        MediaViewPart pt;
        if (!media_view_part(w->v, off, &pt) || pt.len <= 0) return false;
        size_t k = (int64_t)n < pt.len ? n : (size_t)pt.len;
        if (pt.mem) memcpy(d, pt.mem, k);
        else if (!w->orig->read_at(w->orig->ctx, pt.file_off, d, k)) return false;
        d += k, off += (int64_t)k, n -= k;
    }
    return true;
}

static uint32_t rbe32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rbe64(const uint8_t *p) { return (uint64_t)rbe32(p) << 32 | rbe32(p + 4); }

typedef struct {
    const uint8_t *p;
    size_t n;
} Sp;

static bool kid(Sp in, const char *type, Sp *out) {
    for (size_t pos = 0; pos + 8 <= in.n;) {
        uint32_t sz = rbe32(in.p + pos);
        if (sz < 8 || sz > in.n - pos) return false;
        if (!memcmp(in.p + pos + 4, type, 4)) {
            *out = (Sp){in.p + pos + 8, sz - 8};
            return true;
        }
        pos += sz;
    }
    return false;
}

/* What a file (or view) holds, checked: every trak's samples found by stsc + stsz + stco/co64 with the bytes they
   were written with. ids[]/on[]/co64[] per trak out, in moov order. -1 = something didn't check out. */
static int traks_ok(const MediaReader *r, uint32_t ids[8], bool on[8], bool co64[8]) {
    int64_t off = 0, moov_at = -1, moov_n = 0;
    while (off + 8 <= r->size) {
        uint8_t h[16];
        if (!r->read_at(r->ctx, off, h, 8)) return -1;
        uint64_t sz = rbe32(h);
        int hl = 8;
        if (sz == 1) {
            if (!r->read_at(r->ctx, off + 8, h + 8, 8)) return -1;
            sz = rbe64(h + 8), hl = 16;
        }
        if (sz < (uint64_t)hl) return -1;
        if (!memcmp(h + 4, "moov", 4)) moov_at = off + hl, moov_n = (int64_t)sz - hl;
        off += (int64_t)sz;
    }
    if (moov_at < 0) return -1;
    uint8_t *mv = malloc((size_t)moov_n);
    if (!mv || !r->read_at(r->ctx, moov_at, mv, (size_t)moov_n)) return free(mv), -1;
    int n = 0;
    bool good = true;
    for (size_t pos = 0; good && pos + 8 <= (size_t)moov_n;) {
        uint32_t sz = rbe32(mv + pos);
        if (sz < 8) break;
        if (memcmp(mv + pos + 4, "trak", 4)) {
            pos += sz;
            continue;
        }
        Sp tr = {mv + pos + 8, sz - 8}, tkhd, mdia, minf, stbl, stsc, stsz, co;
        pos += sz;
        if (n == 8 || !kid(tr, "tkhd", &tkhd) || !kid(tr, "mdia", &mdia) || !kid(mdia, "minf", &minf) ||
            !kid(minf, "stbl", &stbl) || !kid(stbl, "stsc", &stsc) || !kid(stbl, "stsz", &stsz)) {
            good = false;
            break;
        }
        bool c64 = kid(stbl, "co64", &co);
        if (!c64 && !kid(stbl, "stco", &co)) {
            good = false;
            break;
        }
        ids[n] = rbe32(tkhd.p + 12), on[n] = tkhd.p[3] & 1, co64[n] = c64;
        uint32_t per = rbe32(stsc.p + 12), nsamp = rbe32(stsz.p + 8), nch = rbe32(co.p + 4);
        for (uint32_t c = 0, i = 0; good && c < nch; c++) {
            uint64_t at = c64 ? rbe64(co.p + 8 + c * 8) : rbe32(co.p + 8 + c * 4);
            for (uint32_t s = 0; s < per && i < nsamp; s++, i++) {
                uint32_t len = rbe32(stsz.p + 12 + i * 4);
                uint8_t buf[64];
                if (len > sizeof buf || !r->read_at(r->ctx, (int64_t)at, buf, len)) {
                    good = false;
                    break;
                }
                for (uint32_t j = 0; j < len; j++)
                    if (buf[j] != pattern(ids[n], (int)i, j)) good = false;
                at += len;
            }
        }
        n++;
    }
    free(mv);
    return good ? n : -1;
}

/* ------------------------------------------------------------- the tests */

TEST(mp4view_lists_the_sound_tracks) {
    TM m = three_sounds();
    Buf b = {0};
    make_mp4(&b, &m);
    Mem mm;
    MediaReader r = mem_reader(&mm, &b);
    MediaSound s[MEDIA_MAX_SOUNDS];
    CHECK_INT(media_mp4_sounds(&r, s, MEDIA_MAX_SOUNDS), 3);
    CHECK_STR(s[0].lang, "en");
    CHECK_STR(s[0].name, "Original");
    CHECK(!s[0].on);
    CHECK_STR(s[1].lang, "de"); /* ("ger": a B code) */
    CHECK_STR(s[1].name, "Synchro, Kino"); /* (udta's name before the hdlr's "SoundHandler") */
    CHECK(s[1].on);
    CHECK_STR(s[2].lang, "sv");
    CHECK_STR(s[2].name, ""); /* (a muxer's handler name isn't the track's) */
    CHECK(!s[2].on);
    CHECK_STR(s[0].codec, "aac");
    CHECK_INT(s[0].channels, 2);
    CHECK_INT(media_mp4_sounds(&r, s, 2), 3); /* (how many in all, two listed) */
    MediaProbe p;
    CHECK(media_probe_mem(b.p, b.len, "x.mp4", &p));
    CHECK_INT(p.nsounds, 3);
    CHECK_STR(p.acodec, "aac");
    CHECK_STR(p.vcodec, "h264");

    /* none enabled: the first is the default; T codes, "und", a Pascal hdlr name, handler names left out */
    m.t[1].on = m.t[2].on = false;
    m.t[1].lang = "deu", m.t[1].hdlr_name = "Core Media Audio";
    m.t[2].lang = "und", m.t[2].udta_name = NULL, m.t[2].hdlr_name = "Kommentar", m.t[2].pstring = true;
    m.t[3].lang = NULL, m.t[3].hdlr_name = "GPAC ISO Audio Handler";
    make_mp4(&b, &m);
    r = mem_reader(&mm, &b);
    CHECK_INT(media_mp4_sounds(&r, s, MEDIA_MAX_SOUNDS), 3);
    CHECK(s[0].on && !s[1].on && !s[2].on);
    CHECK_STR(s[0].lang, "de");
    CHECK_STR(s[0].name, "");
    CHECK_STR(s[1].lang, "");
    CHECK_STR(s[1].name, "Kommentar");
    CHECK_STR(s[2].lang, "");
    CHECK_STR(s[2].name, "");

    /* not an MP4: -1; an MKV's audio tracks count in the probe too (probe_test has its MKVs) */
    const char junk[] = "this is not a video at all, not even close";
    MediaReader jr = {&mm, 0, mem_at};
    mm = (Mem){(const uint8_t *)junk, (int64_t)sizeof junk};
    jr.size = mm.n;
    CHECK_INT(media_mp4_sounds(&jr, s, MEDIA_MAX_SOUNDS), -1);
    buf_free(&b);
}

/* One view per sound, the file laid out each way: exactly that sound left, enabled, and every kept sample where its
   bytes are */
static bool views_check(const TM *m, int *fails) {
    Buf b = {0};
    make_mp4(&b, m);
    Mem mm;
    MediaReader r = mem_reader(&mm, &b);
    uint32_t ids[8];
    bool on[8], co[8];
    if (traks_ok(&r, ids, on, co) != m->n) (*fails)++;
    int want_ids[] = {3, 2, 3, 4, 3}; /* sound -1, 0, 1, 2, 7 (out of range = the default) */
    int sounds[] = {-1, 0, 1, 2, 7};
    for (int k = 0; k < 5; k++) {
        char err[120];
        MediaView *v = media_view_new(&r, sounds[k], err, sizeof err);
        if (!v) {
            fprintf(stderr, "  no view (%s)\n", err);
            (*fails)++;
            continue;
        }
        ViewR w = {v, &r};
        MediaReader vr = {&w, media_view_size(v), view_at};
        int n = traks_ok(&vr, ids, on, co);
        if (n != 2 || ids[0] != 1 || ids[1] != (uint32_t)want_ids[k] || !on[1] || co[1] != m->co64) (*fails)++;
        MediaSound s[4];
        if (media_mp4_sounds(&vr, s, 4) != 1 || !s[0].on) (*fails)++;
        /* the size: the original's less its moov, plus the new one (the dropped sounds' bytes stay in the mdat) */
        int64_t moov_old = 0;
        for (int64_t off = 0; off + 8 <= (int64_t)b.len;) {
            uint32_t sz = rbe32((const uint8_t *)b.p + off);
            if (!memcmp(b.p + off + 4, "moov", 4)) moov_old = sz;
            off += sz;
        }
        Mem vm;
        Buf whole = {0};
        int64_t vs = media_view_size(v);
        uint8_t *all = malloc((size_t)vs);
        bool read = all && vr.read_at(vr.ctx, 0, all, (size_t)vs);
        /* one byte at a time across the whole view: the same bytes as one read */
        for (int64_t i = 0; read && i < vs; i++) {
            uint8_t one;
            if (!vr.read_at(vr.ctx, i, &one, 1) || one != all[i]) read = false;
        }
        if (!read) (*fails)++;
        buf_put(&whole, all, (size_t)vs);
        MediaReader again = mem_reader(&vm, &whole);
        if (traks_ok(&again, ids, on, co) != 2) (*fails)++;
        Sp top = {all, (size_t)vs}, mv;
        if (!kid(top, "moov", &mv) || vs != (int64_t)b.len - moov_old + (int64_t)mv.n + 8) (*fails)++;
        MediaViewPart pt;
        if (media_view_part(v, vs, &pt) || media_view_part(v, -1, &pt)) (*fails)++;
        free(all);
        buf_free(&whole);
        media_view_free(v);
    }
    buf_free(&b);
    return true;
}

TEST(mp4view_keeps_one_sound_in_every_layout) {
    int fails = 0;
    TM m = three_sounds();
    views_check(&m, &fails);
    CHECK_INT(fails, 0);
    m.moov_last = true; /* (a phone's recording, a download not made "fast start") */
    views_check(&m, &fails);
    CHECK_INT(fails, 0);
    m.moov_last = false, m.co64 = true, m.free_box = true; /* (Hemma's own copies: a free box after the moov) */
    views_check(&m, &fails);
    CHECK_INT(fails, 0);
    m.moov_last = true;
    views_check(&m, &fails);
    CHECK_INT(fails, 0);
}

/* A file far past 4 GB with its moov at the end: offsets that fitted 32 bits no longer do once the moov goes in
   front - the kept traks' stco become co64. Served by a sparse reader: no disk. */
typedef struct {
    int64_t size;
    struct {
        int64_t off;
        const uint8_t *p;
        size_t n;
    } piece[64];
    int n;
} Sparse;

static bool sparse_at(void *ctx, int64_t off, void *dst, size_t n) {
    const Sparse *s = ctx;
    if (off < 0 || off > s->size || (int64_t)n > s->size - off) return false;
    memset(dst, 0, n);
    for (int i = 0; i < s->n; i++) {
        int64_t a = off > s->piece[i].off ? off : s->piece[i].off;
        int64_t z = off + (int64_t)n < s->piece[i].off + (int64_t)s->piece[i].n ? off + (int64_t)n
                                                                                 : s->piece[i].off + (int64_t)s->piece[i].n;
        if (a < z) memcpy((uint8_t *)dst + (a - off), s->piece[i].p + (a - s->piece[i].off), (size_t)(z - a));
    }
    return true;
}

TEST(mp4view_moves_offsets_past_4_gb) {
    TM m = three_sounds();
    m.rounds = 3;
    m.moov_last = true;
    /* each round's chunks a few bytes below 4 GB, so the moved ones pass it */
    Buf chunks = {0}, head = {0}, mv = {0};
    uint64_t store[8][64], *rel[8], *abs_[8], store2[8][64];
    for (int k = 0; k < 8; k++) rel[k] = store[k], abs_[k] = store2[k];
    media_bytes(&chunks, &m, rel);
    const int64_t at = 0xFFFFFF00LL - (int64_t)chunks.len; /* the chunks end 256 bytes below 4 GB */
    for (int k = 0; k < m.n; k++)
        for (int r = 0; r < m.rounds; r++) abs_[k][r] = (uint64_t)at + rel[k][r];
    moov(&mv, &m, abs_);
    ftyp(&head);
    int64_t mdat_off = (int64_t)head.len, moov_off = at + (int64_t)chunks.len + 4096, size = moov_off + (int64_t)mv.len;
    p32(&head, 1), buf_put(&head, "mdat", 4), p64(&head, (uint64_t)(moov_off - mdat_off));
    Sparse sp = {size, {{0, (const uint8_t *)head.p, head.len}, {at, (const uint8_t *)chunks.p, chunks.len},
                        {moov_off, (const uint8_t *)mv.p, mv.len}}, 3};
    MediaReader r = {&sp, size, sparse_at};
    uint32_t ids[8];
    bool on[8], co[8];
    CHECK_INT(traks_ok(&r, ids, on, co), 4);
    CHECK(!co[0] && !co[2]);
    char err[120] = "";
    MediaView *v = media_view_new(&r, 2, err, sizeof err);
    if (!v) fprintf(stderr, "  no view (%s)\n", err);
    CHECK(v != NULL);
    ViewR w = {v, &r};
    MediaReader vr = {&w, media_view_size(v), view_at};
    CHECK_INT(traks_ok(&vr, ids, on, co), 2);
    CHECK(ids[0] == 1 && ids[1] == 4 && on[1]);
    CHECK(co[0] && co[1]); /* (both kept traks moved past 4 GB) */
    MediaViewPart pt;
    CHECK(media_view_part(v, 0, &pt) && pt.mem);
    int64_t ftyp_len = rbe32((const uint8_t *)head.p);
    CHECK_INT(media_view_size(v), pt.len + (moov_off - ftyp_len)); /* (ftyp + the new moov, then the whole mdat) */
    CHECK(media_view_part(v, media_view_size(v) - 1, &pt) && !pt.mem && pt.file_off == moov_off - 1 && pt.len == 1);
    media_view_free(v);
    buf_free(&chunks), buf_free(&head), buf_free(&mv);
}

static uint64_t g_rng = 88172645463325252ull;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return (uint32_t)g_rng;
}

/* What gets no view: a fragmented file, a cut moov, lying sizes - and thousands of damaged files, none crashing.
   A view that is made reads to its end in parts that fit together. */
TEST(mp4view_refuses_what_it_cannot_show) {
    TM m = three_sounds();
    Buf b = {0};
    Mem mm;
    MediaReader r;
    char err[120];
    m.mvex = true;
    make_mp4(&b, &m);
    r = mem_reader(&mm, &b);
    CHECK(!media_view_new(&r, 0, err, sizeof err));
    CHECK_HAS(err, "fragmented");
    m.mvex = false;
    make_mp4(&b, &m);
    Buf moof = {0};
    buf_put(&moof, b.p, b.len);
    size_t f = bo(&moof, "moof");
    pz(&moof, 8);
    bc(&moof, f);
    r = mem_reader(&mm, &moof);
    CHECK(!media_view_new(&r, 0, err, sizeof err));
    CHECK_HAS(err, "fragmented");
    buf_free(&moof);
    m.moov_last = true; /* a moov cut short at the end (an unfinished download) */
    make_mp4(&b, &m);
    r = mem_reader(&mm, &b);
    r.size -= 10, mm.n -= 10;
    CHECK(!media_view_new(&r, 0, err, sizeof err));
    m.moov_last = false;
    make_mp4(&b, &m);
    /* a chunk pointing into the moov itself */
    Buf bad = {0};
    buf_put(&bad, b.p, b.len);
    for (size_t i = 0; i + 8 < bad.len; i++)
        if (!memcmp(bad.p + i, "stco", 4)) {
            uint8_t *e = (uint8_t *)bad.p + i + 12;
            e[0] = e[1] = e[2] = 0, e[3] = 40;
            break;
        }
    r = mem_reader(&mm, &bad);
    CHECK(!media_view_new(&r, -1, err, sizeof err));
    CHECK_HAS(err, "outside");
    buf_free(&bad);

    int made = 0;
    for (int round = 0; round < 3000; round++) {
        Buf x = {0};
        buf_put(&x, b.p, b.len);
        int hits = 1 + (int)(rnd() % 6);
        for (int h = 0; h < hits; h++) {
            size_t at = rnd() % x.len;
            switch (rnd() % 4) {
            case 0: ((uint8_t *)x.p)[at] = (uint8_t)rnd(); break;
            case 1: ((uint8_t *)x.p)[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 2: /* a size or count made huge */
                if (at + 4 <= x.len) ((uint8_t *)x.p)[at] = 0xFF, ((uint8_t *)x.p)[at + 1] = 0xFF;
                break;
            default: x.len = at + 1; break; /* cut */
            }
        }
        Mem xm;
        MediaReader xr = mem_reader(&xm, &x);
        MediaSound s[MEDIA_MAX_SOUNDS];
        media_mp4_sounds(&xr, s, MEDIA_MAX_SOUNDS);
        MediaProbe p;
        media_probe_mem(x.p, x.len, "x.mp4", &p);
        MediaView *v = media_view_new(&xr, (int)(rnd() % 5) - 1, err, sizeof err);
        if (v) {
            made++;
            int64_t off = 0, sum = 0;
            MediaViewPart pt;
            while (media_view_part(v, off, &pt)) {
                if (pt.len <= 0 || (!pt.mem && (pt.file_off < 0 || pt.file_off + pt.len > (int64_t)x.len))) break;
                sum += pt.len, off += pt.len;
            }
            CHECK_INT(sum, media_view_size(v));
            media_view_free(v);
        }
        buf_free(&x);
    }
    CHECK(made > 100); /* (most single hits land in sample bytes: still a good file) */
    buf_free(&b);
}

/* What a view of a film costs (not part of the suite): HEMMA_TEST_VIEW_BIG=1 ./build/test mp4view_big - a 1.5 h
   film's worth of samples, a picture and six sounds, every chunk its own (worse than Hemma's copies, which write a
   second of sound a chunk): the time to make one view, and its header. */
TEST(mp4view_big_moov_when_asked) {
    if (!getenv("HEMMA_TEST_VIEW_BIG")) return;
    TM m = {.n = 7, .rounds = 90 * 60 * 24};
    m.t[0] = (TT){.video = true, .id = 1, .on = true};
    for (int k = 1; k < 7; k++) m.t[k] = (TT){.id = (uint32_t)k + 1, .lang = k == 6 ? "eng" : "rus", .on = k == 1};
    Buf b = {0};
    make_mp4(&b, &m);
    Mem mm;
    MediaReader r = mem_reader(&mm, &b);
    int64_t t0 = now_ms();
    MediaView *v = NULL;
    for (int i = 0; i < 5; i++) {
        media_view_free(v);
        v = media_view_new(&r, 5, NULL, 0);
    }
    int64_t each = (now_ms() - t0) / 5;
    CHECK(v != NULL);
    MediaViewPart pt;
    CHECK(media_view_part(v, 0, &pt));
    fprintf(stderr, "  %zu-byte file, its moov %lld bytes: a view in %lld ms, its header %lld bytes\n", b.len,
            (long long)(b.len - (size_t)media_view_size(v) + (size_t)pt.len), (long long)each, (long long)pt.len);
    media_view_free(v);
    buf_free(&b);
}

/* ------------------------------------------------------------- over sockets */

static volatile int g_done;
static int g_port;
static void serve(void *arg) {
    (void)arg;
    app_serve();
    g_done = 1;
}

static sock_t dial(void) {
    sock_t s = tcp_connect_nb(0x7F000001, g_port);
    pollfd_t pf = {.fd = s, .events = POLLOUT};
    if (s == SOCK_BAD || plat_poll(&pf, 1, 2000) != 1 || !tcp_connect_done(s)) return SOCK_BAD;
    return s;
}

static void send_all(sock_t s, const void *p, size_t n) {
    const char *c = p;
    for (int64_t end = now_ms() + 5000; n && now_ms() < end;) {
        long k = sock_send(s, c, n);
        if (k > 0) c += k, n -= (size_t)k;
        else plat_sleep_ms(1);
    }
}

static void exchange(const char *req, Buf *resp) {
    buf_reset(resp);
    sock_t s = dial();
    if (s == SOCK_BAD) return;
    send_all(s, req, strlen(req));
    for (int64_t end = now_ms() + 10000; now_ms() < end;) {
        pollfd_t pf = {.fd = s, .events = POLLIN};
        if (plat_poll(&pf, 1, 50) != 1) continue;
        char tmp[16384];
        long k = sock_recv(s, tmp, sizeof tmp);
        if (k > 0) buf_put(resp, tmp, (size_t)k);
        else if (k == 0 || !sock_again(sock_errno())) break;
    }
    sock_close(s);
}

static void request(const char *method, const char *path, const char *cookie, const char *extra, Buf *resp) {
    char head[1024];
    snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: t\r\nCookie: hemma=%s\r\n%sConnection: close\r\n\r\n", method, path,
             cookie, extra ? extra : "");
    exchange(head, resp);
}

static const uint8_t *body_of(Buf *r, size_t *n) {
    const char *e = r->p ? strstr(buf_cstr(r), "\r\n\r\n") : NULL;
    *n = e ? r->len - (size_t)(e + 4 - r->p) : 0;
    return e ? (const uint8_t *)e + 4 : (const uint8_t *)"";
}

static void sign_in(const char *user, const char *pass, char *cookie, size_t cap) {
    char req[600], form[200];
    snprintf(form, sizeof form, "username=%s&password=%s&next=%%2F", user, pass);
    snprintf(req, sizeof req,
             "POST /login HTTP/1.1\r\nHost: t\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n%s",
             strlen(form), form);
    Buf r = {0};
    exchange(req, &r);
    const char *c = strstr(buf_cstr(&r), "Set-Cookie: hemma=");
    cookie[0] = '\0';
    if (c) str_ncopy(cookie, cap, c + 18, strcspn(c + 18, ";\r\n"));
    buf_free(&r);
}

/* the view of `sound` made here, to compare what the server sends with */
static bool view_bytes(const Buf *file, int sound, Buf *out) {
    Mem mm;
    MediaReader r = mem_reader(&mm, file);
    MediaView *v = media_view_new(&r, sound, NULL, 0);
    if (!v) return false;
    ViewR w = {v, &r};
    buf_reset(out);
    int64_t n = media_view_size(v);
    uint8_t *p = malloc((size_t)n);
    bool ok = p && view_at(&w, 0, p, (size_t)n);
    if (ok) buf_put(out, p, (size_t)n);
    free(p);
    media_view_free(v);
    return ok;
}

TEST(mp4view_served_one_sound_at_a_time) {
    th_boot();
    auth_configure("view-salt", 1000);
    int eva = th_person("Eva");
    Person p = *person_get(eva);
    str_copy(p.username, sizeof p.username, "eva");
    auth_hash("pw-eva-1234", p.pass, sizeof p.pass);
    p.role = ROLE_MEMBER;
    tbl_put(&T_person, &p);
    char root[512], dir[600], path[700];
    str_copy(root, sizeof root, test_tmpdir());
    TM m = three_sounds();
    m.free_box = true;
    Buf file = {0}, want = {0}, r = {0};
    make_mp4(&file, &m);
    snprintf(dir, sizeof dir, "%s/Movies/Two Voices (2024)", root);
    dir_make(dir);
    snprintf(path, sizeof path, "%s/Two Voices (2024).mp4", dir);
    CHECK(file_write_atomic(path, file.p, file.len));
    setting_set("media.roots", root);
    media_scan_sync();
    uint64_t id = media_make_id(root, "Movies/Two Voices (2024)/Two Voices (2024).mp4");
    const MediaFile *f = media_file(id);
    CHECK(f != NULL);
    CHECK_INT(f->probe.nsounds, 3);
    char hex[17], url[200];
    media_id_hex(id, hex);
    media_stream_resume(); /* (an earlier test's th_halt() ran the module's stop hook) */
    CHECK(server_listen("127.0.0.1", 0));
    g_port = server_port();
    g_done = 0;
    CHECK(thread_spawn(serve, NULL));
    char ck[64];
    sign_in("eva", "pw-eva-1234", ck, sizeof ck);
    CHECK(strlen(ck) == 32);
    size_t n = 0;
    const uint8_t *body;

    /* ?a=1 (beside a key, in either order): German alone */
    CHECK(view_bytes(&file, 1, &want));
    snprintf(url, sizeof url, "/media/file/%s?k=x&a=1", hex);
    request("GET", url, ck, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), "Content-Type: video/mp4");
    body = body_of(&r, &n);
    CHECK(n == want.len && !memcmp(body, want.p, n));
    Mem bm;
    Buf got = {0};
    buf_put(&got, body, n);
    MediaReader br = mem_reader(&bm, &got);
    MediaSound s[4];
    CHECK_INT(media_mp4_sounds(&br, s, 4), 1);
    CHECK_STR(s[0].lang, "de");
    /* a range across the end of the new header into the file's own bytes */
    Mem wm;
    MediaReader wr = mem_reader(&wm, &want);
    MediaView *v = media_view_new(&wr, 1, NULL, 0); /* (a view of the view: the same header size) */
    CHECK(v != NULL);
    MediaViewPart pt;
    CHECK(media_view_part(v, 0, &pt) && pt.mem);
    int64_t edge = pt.len;
    media_view_free(v);
    char range[80];
    snprintf(range, sizeof range, "Range: bytes=%lld-%lld\r\n", (long long)(edge - 20), (long long)(edge + 99));
    snprintf(url, sizeof url, "/media/file/%s?a=1", hex);
    request("GET", url, ck, range, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 206 "));
    char cr[120];
    snprintf(cr, sizeof cr, "Content-Range: bytes %lld-%lld/%zu", (long long)(edge - 20), (long long)(edge + 99), want.len);
    CHECK_HAS(buf_cstr(&r), cr);
    body = body_of(&r, &n);
    CHECK(n == 120 && !memcmp(body, want.p + edge - 20, 120));
    snprintf(range, sizeof range, "Range: bytes=%zu-\r\n", want.len + 5);
    request("GET", url, ck, range, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 416 "));
    /* HEAD: the view's length, no body */
    request("HEAD", url, ck, NULL, &r);
    char cl[80];
    snprintf(cl, sizeof cl, "Content-Length: %zu\r\n", want.len);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    CHECK_HAS(buf_cstr(&r), cl);
    body_of(&r, &n);
    CHECK_INT((int)n, 0);
    /* no a= (or a bad one): the file's default sound - German is the enabled one here */
    snprintf(url, sizeof url, "/media/file/%s", hex);
    request("GET", url, ck, NULL, &r);
    body = body_of(&r, &n);
    CHECK(n == want.len && !memcmp(body, want.p, n));
    snprintf(url, sizeof url, "/media/file/%s?a=x9", hex);
    request("GET", url, ck, NULL, &r);
    body = body_of(&r, &n);
    CHECK(n == want.len && !memcmp(body, want.p, n));
    CHECK_INT(media_view_test_builds(), 2); /* (?a=1 once for its five requests, the default once for its two) */
    /* ?a=2: Swedish */
    CHECK(view_bytes(&file, 2, &want));
    snprintf(url, sizeof url, "/media/file/%s?a=2", hex);
    request("GET", url, ck, NULL, &r);
    body = body_of(&r, &n);
    CHECK(n == want.len && !memcmp(body, want.p, n));
    /* a download: the whole file, every sound */
    snprintf(url, sizeof url, "/media/file/%s?download=1&a=2", hex);
    request("GET", url, ck, NULL, &r);
    CHECK(str_starts(buf_cstr(&r), "HTTP/1.1 200 "));
    body = body_of(&r, &n);
    CHECK(n == file.len && !memcmp(body, file.p, n));
    /* the file changed on the disk: a new view, not the one kept */
    m.rounds = 4;
    Buf file2 = {0};
    make_mp4(&file2, &m);
    CHECK(file_write_atomic(path, file2.p, file2.len));
    media_scan_sync();
    CHECK(view_bytes(&file2, 0, &want));
    snprintf(url, sizeof url, "/media/file/%s?a=0", hex);
    request("GET", url, ck, NULL, &r);
    body = body_of(&r, &n);
    CHECK(n == want.len && !memcmp(body, want.p, n));
    for (int i = 0; i < 300 && media_streams_active(); i++) plat_sleep_ms(10);

    server_stop();
    for (int i = 0; i < 300 && !g_done; i++) plat_sleep_ms(10);
    CHECK(g_done);
    server_shutdown();
    buf_free(&file), buf_free(&file2), buf_free(&want), buf_free(&r), buf_free(&got);
    th_halt();
}
