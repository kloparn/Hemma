/* modules/av/mkv.c — see mkv.h */
#include "mkv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Element ids, written with their length-marker bits as they appear in files. */
enum {
    ID_EBML = 0x1A45DFA3,
    ID_DOCTYPE = 0x4282,
    ID_SEGMENT = 0x18538067,
    ID_SEEKHEAD = 0x114D9B74,
    ID_SEEK = 0x4DBB,
    ID_SEEKID = 0x53AB,
    ID_SEEKPOSITION = 0x53AC,
    ID_INFO = 0x1549A966,
    ID_TIMESTAMPSCALE = 0x2AD7B1,
    ID_DURATION = 0x4489,
    ID_TITLE = 0x7BA9,
    ID_WRITINGAPP = 0x5741,
    ID_TRACKS = 0x1654AE6B,
    ID_TRACKENTRY = 0xAE,
    ID_TRACKNUMBER = 0xD7,
    ID_TRACKUID = 0x73C5,
    ID_TRACKTYPE = 0x83,
    ID_FLAGENABLED = 0xB9,
    ID_FLAGDEFAULT = 0x88,
    ID_FLAGFORCED = 0x55AA,
    ID_DEFAULTDURATION = 0x23E383,
    ID_NAME = 0x536E,
    ID_LANGUAGE = 0x22B59C,
    ID_LANGUAGEIETF = 0x22B59D,
    ID_CODECID = 0x86,
    ID_CODECPRIVATE = 0x63A2,
    ID_CODECDELAY = 0x56AA,
    ID_VIDEO = 0xE0,
    ID_PIXELWIDTH = 0xB0,
    ID_PIXELHEIGHT = 0xBA,
    ID_DISPLAYWIDTH = 0x54B0,
    ID_DISPLAYHEIGHT = 0x54BA,
    ID_DISPLAYUNIT = 0x54B2,
    ID_FLAGINTERLACED = 0x9A,
    ID_AUDIO = 0xE1,
    ID_SAMPLINGFREQUENCY = 0xB5,
    ID_CHANNELS = 0x9F,
    ID_BITDEPTH = 0x6264,
    ID_CONTENTENCODINGS = 0x6D80,
    ID_CONTENTENCODING = 0x6240,
    ID_CONTENTENCODINGSCOPE = 0x5032,
    ID_CONTENTENCODINGTYPE = 0x5033,
    ID_CONTENTCOMPRESSION = 0x5034,
    ID_CONTENTCOMPALGO = 0x4254,
    ID_CONTENTCOMPSETTINGS = 0x4255,
    ID_CUES = 0x1C53BB6B,
    ID_CUEPOINT = 0xBB,
    ID_CUETIME = 0xB3,
    ID_CUETRACKPOSITIONS = 0xB7,
    ID_CUETRACK = 0xF7,
    ID_CUECLUSTERPOSITION = 0xF1,
    ID_CLUSTER = 0x1F43B675,
    ID_TIMESTAMP = 0xE7,
    ID_SILENTTRACKS = 0x5854,
    ID_POSITION = 0xA7,
    ID_PREVSIZE = 0xAB,
    ID_SIMPLEBLOCK = 0xA3,
    ID_BLOCKGROUP = 0xA0,
    ID_BLOCK = 0xA1,
    ID_BLOCKDURATION = 0x9B,
    ID_REFERENCEBLOCK = 0xFB,
    ID_ENCRYPTEDBLOCK = 0xAF,
    ID_CHAPTERS = 0x1043A770,
    ID_TAGS = 0x1254C367,
    ID_ATTACHMENTS = 0x1941A469,
    ID_VOID = 0xEC,
    ID_CRC32 = 0xBF,
};

#define MAX_PRIVATE (16u << 20) /* CodecPrivate */
#define MAX_BLOCK (128u << 20)  /* one (Simple)Block */
#define MAX_CUES 2000000
#define MAX_LACE 256
#define MAX_TOP_ELEMENTS 100000 /* level-1 elements looked at before the first cluster */
#define CACHE_SIZE (1 << 16)

typedef struct {
    uint32_t id;
    int64_t size;  /* -1 = unknown */
    int64_t start; /* offset of the id */
    int64_t data;  /* offset of the payload */
} Elem;

typedef struct {
    int64_t time; /* TimestampScale units */
    int64_t pos;  /* absolute offset of the cluster */
    int track;
} Cue;

struct Mkv {
    MkvReader r;
    bool read_failed;
    char doctype[16], title[256], app[128];
    int64_t scale;   /* ns per timestamp unit */
    double duration; /* timestamp units */
    int64_t seg_data, seg_end, first_cluster;
    MkvTrack tracks[MKV_MAX_TRACKS];
    int ntracks;
    Cue *cues;
    int ncues, capcues;
    /* read cache */
    uint8_t *cache;
    int64_t cache_off;
    size_t cache_len;
    /* iteration */
    int64_t pos;
    bool in_cluster, cluster_unknown;
    int64_t cluster_end, cluster_ts;
    /* the block whose frames are being handed out */
    uint8_t *blk, *out;
    size_t blk_cap, out_cap;
    size_t lace_off[MAX_LACE], lace_size[MAX_LACE];
    int lace_n, lace_i;
    MkvFrame lace;
    const MkvTrack *lace_track;
    struct Mp4r *mp4; /* 2.5: an MP4/MOV, read by mkv_mp4.h - every call below goes there (NULL: Matroska) */
};

/* ------------------------------------------------------------- reading */

static bool rd(Mkv *m, int64_t off, void *dst, size_t n) {
    if (off < 0 || (uint64_t)n > (uint64_t)m->r.size || off > m->r.size - (int64_t)n) return false;
    if (!n) return true;
    if (m->cache_len && off >= m->cache_off && off + (int64_t)n <= m->cache_off + (int64_t)m->cache_len) {
        memcpy(dst, m->cache + (off - m->cache_off), n);
        return true;
    }
    if (n >= CACHE_SIZE / 2) {
        if (m->r.read_at(m->r.ctx, off, dst, n)) return true;
        m->read_failed = true;
        return false;
    }
    size_t want = CACHE_SIZE;
    if (m->r.size - off < (int64_t)want) want = (size_t)(m->r.size - off);
    if (!m->r.read_at(m->r.ctx, off, m->cache, want)) {
        m->cache_len = 0;
        m->read_failed = true;
        return false;
    }
    m->cache_off = off;
    m->cache_len = want;
    memcpy(dst, m->cache, n);
    return true;
}

static int vint_len(uint8_t b) {
    int n = 1;
    while (n <= 8 && !(b & (0x80 >> (n - 1)))) n++;
    return n; /* 9 = invalid (b == 0) */
}

/* Element header at off (must start before limit). The payload may still run
   past limit: callers check e->data + e->size themselves. */
static bool read_elem(Mkv *m, int64_t off, int64_t limit, Elem *e) {
    uint8_t h[12];
    if (off < 0 || off >= limit) return false;
    size_t n = limit - off < 12 ? (size_t)(limit - off) : 12;
    if (!rd(m, off, h, n)) return false;
    int il = vint_len(h[0]);
    if (il > 4 || (size_t)il >= n) return false;
    uint32_t id = 0;
    for (int i = 0; i < il; i++) id = id << 8 | h[i];
    int sl = vint_len(h[il]);
    if (sl > 8 || (size_t)(il + sl) > n) return false;
    uint64_t v = h[il] & (0xFFu >> sl);
    bool ones = v == (0xFFu >> sl);
    for (int i = 1; i < sl; i++) {
        v = v << 8 | h[il + i];
        if (h[il + i] != 0xFF) ones = false;
    }
    e->id = id;
    e->start = off;
    e->data = off + il + sl;
    e->size = ones ? -1 : (int64_t)v; /* v < 2^56 */
    return true;
}

static bool fits(const Elem *e, int64_t end) { return e->size >= 0 && e->size <= end - e->data; }

static uint64_t get_uint(Mkv *m, const Elem *e) {
    uint8_t b[8];
    if (e->size < 0 || e->size > 8 || !rd(m, e->data, b, (size_t)e->size)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < e->size; i++) v = v << 8 | b[i];
    return v;
}

static double get_float(Mkv *m, const Elem *e) {
    uint8_t b[8];
    if (e->size == 4 && rd(m, e->data, b, 4)) {
        uint32_t u = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        float f;
        memcpy(&f, &u, 4);
        return f == f ? f : 0; /* NaN -> 0 */
    }
    if (e->size == 8 && rd(m, e->data, b, 8)) {
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) u = u << 8 | b[i];
        double d;
        memcpy(&d, &u, 8);
        return d == d ? d : 0;
    }
    return 0;
}

/* String payload, NUL-terminated; a UTF-8 sequence cut by the cap is dropped. */
static void get_str(Mkv *m, const Elem *e, char *out, size_t cap) {
    out[0] = '\0';
    if (e->size <= 0) return;
    size_t n = e->size < (int64_t)cap ? (size_t)e->size : cap - 1;
    if (!rd(m, e->data, out, n)) {
        out[0] = '\0';
        return;
    }
    out[n] = '\0';
    n = strlen(out);
    size_t k = n;
    while (k > 0 && ((uint8_t)out[k - 1] & 0xC0) == 0x80) k--; /* back to a lead byte */
    if (k > 0 && ((uint8_t)out[k - 1] & 0x80)) {
        uint8_t lead = (uint8_t)out[k - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (n - (k - 1) < need) out[k - 1] = '\0';
    }
    for (char *p = out; *p; p++)
        if ((uint8_t)*p < 0x20) *p = ' ';
}

static uint8_t *get_bin(Mkv *m, const Elem *e, size_t max, size_t *len) {
    *len = 0;
    if (e->size <= 0 || (uint64_t)e->size > max) return NULL;
    uint8_t *p = malloc((size_t)e->size);
    if (!p) return NULL;
    if (!rd(m, e->data, p, (size_t)e->size)) {
        free(p);
        return NULL;
    }
    *len = (size_t)e->size;
    return p;
}

static bool grow(uint8_t **p, size_t *cap, size_t need) {
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    uint8_t *q = realloc(*p, c);
    if (!q) return false;
    *p = q;
    *cap = c;
    return true;
}

/* Iterate the children of a master element: for (CHILDREN(m, parent, e)) {...} */
#define CHILDREN(m, parent, e)                                                                                         \
    int64_t p_ = (parent)->data, end_ = (parent)->data + (parent)->size;                                               \
    p_ < end_ && read_elem((m), p_, end_, &(e)) && fits(&(e), end_);                                                   \
    p_ = (e).data + (e).size

/* -------------------------------------------------------------- headers */

static void parse_info(Mkv *m, const Elem *info) {
    Elem e;
    for (CHILDREN(m, info, e)) {
        switch (e.id) {
        case ID_TIMESTAMPSCALE: {
            uint64_t v = get_uint(m, &e);
            if (v >= 1 && v <= 1000000000000ull) m->scale = (int64_t)v;
            break;
        }
        case ID_DURATION: m->duration = get_float(m, &e); break;
        case ID_TITLE: get_str(m, &e, m->title, sizeof m->title); break;
        case ID_WRITINGAPP: get_str(m, &e, m->app, sizeof m->app); break;
        }
    }
}

static int clamp_int(uint64_t v, int max) { return v > (uint64_t)max ? max : (int)v; }

static void parse_video(Mkv *m, const Elem *v, MkvTrack *t) {
    Elem e;
    for (CHILDREN(m, v, e)) {
        uint64_t u = get_uint(m, &e);
        switch (e.id) {
        case ID_PIXELWIDTH: t->width = clamp_int(u, 65535); break;
        case ID_PIXELHEIGHT: t->height = clamp_int(u, 65535); break;
        case ID_DISPLAYWIDTH: t->disp_width = clamp_int(u, 65535); break;
        case ID_DISPLAYHEIGHT: t->disp_height = clamp_int(u, 65535); break;
        case ID_DISPLAYUNIT: t->disp_unit = clamp_int(u, 4); break;
        case ID_FLAGINTERLACED: t->interlaced = clamp_int(u, 2); break;
        }
    }
}

static void parse_audio(Mkv *m, const Elem *a, MkvTrack *t) {
    Elem e;
    for (CHILDREN(m, a, e)) {
        switch (e.id) {
        case ID_SAMPLINGFREQUENCY: {
            double r = get_float(m, &e);
            if (r > 0 && r < 10e6) t->rate = r;
            break;
        }
        case ID_CHANNELS: t->channels = clamp_int(get_uint(m, &e), 255); break;
        case ID_BITDEPTH: t->bits = clamp_int(get_uint(m, &e), 255); break;
        }
    }
}

/* ContentEncodings: header stripping is undone for the caller; anything else
   (zlib, bzlib, lzo, encryption) marks the track as unusable. */
static void parse_encodings(Mkv *m, const Elem *encs, MkvTrack *t, uint8_t **strip, size_t *strip_len) {
    Elem enc;
    for (CHILDREN(m, encs, enc)) {
        if (enc.id != ID_CONTENTENCODING) continue;
        uint64_t scope = 1, type = 0, algo = 0;
        uint8_t *settings = NULL;
        size_t settings_len = 0;
        Elem e;
        for (CHILDREN(m, &enc, e)) {
            if (e.id == ID_CONTENTENCODINGSCOPE) scope = get_uint(m, &e);
            else if (e.id == ID_CONTENTENCODINGTYPE) type = get_uint(m, &e);
            else if (e.id == ID_CONTENTCOMPRESSION) {
                Elem c;
                for (CHILDREN(m, &e, c)) {
                    if (c.id == ID_CONTENTCOMPALGO) algo = get_uint(m, &c);
                    else if (c.id == ID_CONTENTCOMPSETTINGS && !settings) settings = get_bin(m, &c, 4096, &settings_len);
                }
            }
        }
        if (!(scope & 1)) { /* only the codec private data or later encodings */
            free(settings);
            continue;
        }
        if (type == 0 && algo == 3 && !*strip && settings) {
            *strip = settings;
            *strip_len = settings_len;
        } else {
            if (!(type == 0 && algo == 3)) t->compressed = true;
            free(settings);
        }
    }
}

static void parse_track(Mkv *m, const Elem *te) {
    MkvTrack t;
    memset(&t, 0, sizeof t);
    t.enabled = t.is_default = true;
    t.rate = 8000;
    t.channels = 1;
    char lang[24] = "", ietf[24] = "";
    uint8_t *priv = NULL, *strip = NULL;
    size_t priv_len = 0, strip_len = 0;
    Elem e;
    for (CHILDREN(m, te, e)) {
        switch (e.id) {
        case ID_TRACKNUMBER: t.number = clamp_int(get_uint(m, &e), 1 << 30); break;
        case ID_TRACKUID: t.uid = get_uint(m, &e); break;
        case ID_TRACKTYPE: t.type = clamp_int(get_uint(m, &e), 255); break;
        case ID_FLAGENABLED: t.enabled = get_uint(m, &e) != 0; break;
        case ID_FLAGDEFAULT: t.is_default = get_uint(m, &e) != 0; break;
        case ID_FLAGFORCED: t.forced = get_uint(m, &e) != 0; break;
        case ID_DEFAULTDURATION: {
            uint64_t d = get_uint(m, &e);
            t.default_duration = d < 3600000000000ull ? (int64_t)d : 0;
            break;
        }
        case ID_CODECDELAY: {
            uint64_t d = get_uint(m, &e);
            t.codec_delay = d < 3600000000000ull ? (int64_t)d : 0;
            break;
        }
        case ID_NAME: get_str(m, &e, t.name, sizeof t.name); break;
        case ID_LANGUAGE: get_str(m, &e, lang, sizeof lang); break;
        case ID_LANGUAGEIETF: get_str(m, &e, ietf, sizeof ietf); break;
        case ID_CODECID: get_str(m, &e, t.codec, sizeof t.codec); break;
        case ID_CODECPRIVATE:
            if (!priv) priv = get_bin(m, &e, MAX_PRIVATE, &priv_len);
            break;
        case ID_VIDEO: parse_video(m, &e, &t); break;
        case ID_AUDIO: parse_audio(m, &e, &t); break;
        case ID_CONTENTENCODINGS: parse_encodings(m, &e, &t, &strip, &strip_len); break;
        }
    }
    bool dup = false;
    for (int i = 0; i < m->ntracks; i++) dup |= m->tracks[i].number == t.number;
    if (t.number <= 0 || dup || m->ntracks == MKV_MAX_TRACKS) {
        free(priv);
        free(strip);
        return;
    }
    const char *l = ietf[0] ? ietf : lang[0] ? lang : "eng";
    memcpy(t.lang, l, strlen(l) + 1);
    t.priv = priv;
    t.priv_len = priv_len;
    t.strip = strip;
    t.strip_len = strip_len;
    m->tracks[m->ntracks++] = t;
}

static void parse_tracks(Mkv *m, const Elem *tracks) {
    Elem e;
    for (CHILDREN(m, tracks, e))
        if (e.id == ID_TRACKENTRY) parse_track(m, &e);
}

static void add_cue(Mkv *m, int64_t time, int64_t pos, int track) {
    if (m->ncues == MAX_CUES) return;
    if (m->ncues == m->capcues) {
        int cap = m->capcues ? m->capcues * 2 : 256;
        Cue *c = realloc(m->cues, (size_t)cap * sizeof *c);
        if (!c) return;
        m->cues = c;
        m->capcues = cap;
    }
    m->cues[m->ncues++] = (Cue){time, pos, track};
}

static int cue_cmp(const void *a, const void *b) {
    const Cue *x = a, *y = b;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

static void parse_cues(Mkv *m, const Elem *cues) {
    Elem cp;
    for (CHILDREN(m, cues, cp)) {
        if (cp.id != ID_CUEPOINT) continue;
        int64_t time = -1;
        Elem e;
        for (CHILDREN(m, &cp, e)) {
            if (e.id == ID_CUETIME) {
                uint64_t t = get_uint(m, &e);
                time = t < (1ull << 53) ? (int64_t)t : -1;
            }
        }
        if (time < 0) continue;
        for (CHILDREN(m, &cp, e)) {
            if (e.id != ID_CUETRACKPOSITIONS) continue;
            int track = 0;
            int64_t pos = -1;
            Elem c;
            for (CHILDREN(m, &e, c)) {
                if (c.id == ID_CUETRACK) track = clamp_int(get_uint(m, &c), 1 << 30);
                else if (c.id == ID_CUECLUSTERPOSITION) {
                    uint64_t p = get_uint(m, &c);
                    if (p < (uint64_t)(m->seg_end - m->seg_data)) pos = m->seg_data + (int64_t)p;
                }
            }
            if (pos >= 0) add_cue(m, time, pos, track);
        }
    }
    if (m->ncues > 1) qsort(m->cues, (size_t)m->ncues, sizeof *m->cues, cue_cmp);
}

typedef struct {
    int64_t info, tracks, cues, seekhead;
} SeekTargets;

static void parse_seekhead(Mkv *m, const Elem *sh, SeekTargets *st) {
    Elem s;
    for (CHILDREN(m, sh, s)) {
        if (s.id != ID_SEEK) continue;
        uint64_t id = 0, pos = UINT64_MAX;
        Elem e;
        for (CHILDREN(m, &s, e)) {
            if (e.id == ID_SEEKID) id = get_uint(m, &e);
            else if (e.id == ID_SEEKPOSITION) pos = get_uint(m, &e);
        }
        if (pos >= (uint64_t)(m->seg_end - m->seg_data)) continue;
        int64_t at = m->seg_data + (int64_t)pos;
        if (id == ID_INFO && st->info < 0) st->info = at;
        else if (id == ID_TRACKS && st->tracks < 0) st->tracks = at;
        else if (id == ID_CUES && st->cues < 0) st->cues = at;
        else if (id == ID_SEEKHEAD && st->seekhead < 0 && at != sh->start) st->seekhead = at;
    }
}

/* The level-1 element `id` at `at`, fully inside the segment. */
static bool elem_at(Mkv *m, int64_t at, uint32_t id, Elem *e) {
    return at >= 0 && read_elem(m, at, m->seg_end, e) && e->id == id && fits(e, m->seg_end);
}

static void fail(char *err, size_t cap, const char *msg) {
    if (!err || !cap) return;
    size_t n = strlen(msg);
    if (n >= cap) n = cap - 1;
    memcpy(err, msg, n);
    err[n] = '\0';
}

#include "mkv_mp4.h" /* (the MP4/MOV side: its boxes, after rd() and fail()) */

Mkv *mkv_open(const MkvReader *r, char *err, size_t errcap) {
    fail(err, errcap, "");
    Mkv *m = calloc(1, sizeof *m);
    if (!m || !(m->cache = malloc(CACHE_SIZE))) {
        free(m);
        fail(err, errcap, "out of memory");
        return NULL;
    }
    m->r = *r;
    m->scale = 1000000;
    m->first_cluster = -1;
    memcpy(m->doctype, "matroska", 9);
    if (mp4_looks(m)) { /* 2.5: an MP4 or a QuickTime file (mkv_mp4.h) - doctype "mp4" or "mov" */
        if (mp4_open(m, err, errcap)) return m;
        mkv_close(m);
        return NULL;
    }

    Elem e;
    if (r->size < 8 || !read_elem(m, 0, r->size, &e) || e.id != ID_EBML || !fits(&e, r->size)) {
        fail(err, errcap, m->read_failed ? "read error" : "not a Matroska/WebM file");
        mkv_close(m);
        return NULL;
    }
    Elem c;
    for (CHILDREN(m, &e, c))
        if (c.id == ID_DOCTYPE) get_str(m, &c, m->doctype, sizeof m->doctype);
    if (strcmp(m->doctype, "matroska") != 0 && strcmp(m->doctype, "webm") != 0) {
        fail(err, errcap, "unsupported EBML document type");
        mkv_close(m);
        return NULL;
    }

    /* the Segment (skipping Void and friends between it and the EBML header) */
    int64_t p = e.data + e.size;
    for (int k = 0;; k++) {
        if (k == 64 || !read_elem(m, p, r->size, &e) || (e.id != ID_SEGMENT && !fits(&e, r->size))) {
            fail(err, errcap, "no Segment");
            mkv_close(m);
            return NULL;
        }
        if (e.id == ID_SEGMENT) break;
        p = e.data + e.size;
    }
    m->seg_data = e.data;
    m->seg_end = !fits(&e, r->size) ? r->size : e.data + e.size; /* unknown size or truncated file */

    /* level-1 elements up to the first cluster */
    SeekTargets st = {-1, -1, -1, -1};
    bool got_info = false, got_tracks = false, got_cues = false;
    p = m->seg_data;
    for (int k = 0; k < MAX_TOP_ELEMENTS && p < m->seg_end; k++) {
        if (!read_elem(m, p, m->seg_end, &e)) break;
        if (e.id == ID_CLUSTER) {
            m->first_cluster = p;
            break;
        }
        if (!fits(&e, m->seg_end)) break;
        if (e.id == ID_SEEKHEAD) parse_seekhead(m, &e, &st);
        else if (e.id == ID_INFO && !got_info) parse_info(m, &e), got_info = true;
        else if (e.id == ID_TRACKS && !got_tracks) parse_tracks(m, &e), got_tracks = true;
        else if (e.id == ID_CUES && !got_cues) parse_cues(m, &e), got_cues = true;
        p = e.data + e.size;
    }
    /* what lives elsewhere (typically Cues at the end), via the SeekHead(s) */
    if (st.seekhead >= 0 && elem_at(m, st.seekhead, ID_SEEKHEAD, &e)) parse_seekhead(m, &e, &st);
    if (!got_info && elem_at(m, st.info, ID_INFO, &e)) parse_info(m, &e);
    if (!got_tracks && elem_at(m, st.tracks, ID_TRACKS, &e)) parse_tracks(m, &e);
    if (!got_cues && elem_at(m, st.cues, ID_CUES, &e)) parse_cues(m, &e);

    if (!m->ntracks) {
        fail(err, errcap, m->read_failed ? "read error" : "no tracks");
        mkv_close(m);
        return NULL;
    }
    mkv_rewind(m);
    return m;
}

void mkv_close(Mkv *m) {
    if (!m) return;
    for (int i = 0; i < m->ntracks; i++) {
        free((void *)m->tracks[i].priv);
        free((void *)m->tracks[i].strip);
    }
    free(m->cues);
    free(m->cache);
    free(m->blk);
    free(m->out);
    mp4_free(m->mp4);
    free(m);
}

const char *mkv_doctype(const Mkv *m) { return m->doctype; }
const char *mkv_title(const Mkv *m) { return m->title; }
const char *mkv_writing_app(const Mkv *m) { return m->app; }
int mkv_track_count(const Mkv *m) { return m->ntracks; }
const MkvTrack *mkv_track(const Mkv *m, int i) { return i >= 0 && i < m->ntracks ? &m->tracks[i] : NULL; }
int mkv_cue_count(const Mkv *m) { return m->ncues; }

int64_t mkv_duration(const Mkv *m) {
    double ns = m->duration * (double)m->scale;
    return ns > 0 && ns < 9e18 ? (int64_t)ns : 0;
}

const MkvTrack *mkv_track_by_number(const Mkv *m, int number) {
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i].number == number) return &m->tracks[i];
    return NULL;
}

const MkvTrack *mkv_find_track(const Mkv *m, int type) {
    const MkvTrack *first = NULL;
    for (int i = 0; i < m->ntracks; i++) {
        const MkvTrack *t = &m->tracks[i];
        if (t->type != type) continue;
        if (t->is_default && t->enabled) return t;
        if (!first) first = t;
    }
    return first;
}

/* --------------------------------------------------------------- frames */

static bool is_level1(uint32_t id) {
    switch (id) {
    case ID_SEEKHEAD:
    case ID_INFO:
    case ID_TRACKS:
    case ID_CUES:
    case ID_CHAPTERS:
    case ID_TAGS:
    case ID_ATTACHMENTS:
    case ID_CLUSTER:
    case ID_VOID:
    case ID_CRC32: return true;
    }
    return false;
}

static bool is_cluster_child(uint32_t id) {
    switch (id) {
    case ID_TIMESTAMP:
    case ID_SILENTTRACKS:
    case ID_POSITION:
    case ID_PREVSIZE:
    case ID_SIMPLEBLOCK:
    case ID_BLOCKGROUP:
    case ID_ENCRYPTEDBLOCK:
    case ID_VOID:
    case ID_CRC32: return true;
    }
    return false;
}

static int64_t units_to_ns(const Mkv *m, int64_t units) {
    int64_t ns;
    if (__builtin_mul_overflow(units, m->scale, &ns)) return MKV_NOPTS;
    return ns;
}

/* Parse a Block/SimpleBlock payload into m->blk + lace tables. False = skip it. */
static bool load_block(Mkv *m, const Elem *b, bool simple, bool group_key, int64_t group_dur) {
    if (b->size < 4 || (uint64_t)b->size > MAX_BLOCK) return false;
    size_t n = (size_t)b->size;
    if (!grow(&m->blk, &m->blk_cap, n) || !rd(m, b->data, m->blk, n)) return false;
    const uint8_t *p = m->blk, *end = m->blk + n;
    int tl = vint_len(p[0]);
    if (tl > 8 || (size_t)tl + 3 > n) return false;
    uint64_t track = p[0] & (0xFFu >> tl);
    for (int i = 1; i < tl; i++) track = track << 8 | p[i];
    p += tl;
    int16_t rel = (int16_t)(uint16_t)(p[0] << 8 | p[1]);
    uint8_t flags = p[2];
    p += 3;
    const MkvTrack *t = track <= (1u << 30) ? mkv_track_by_number(m, (int)track) : NULL;
    if (!t) return false;

    int nf = 1;
    int lacing = (flags >> 1) & 3;
    if (lacing) {
        if (p >= end) return false;
        nf = *p++ + 1;
    }
    size_t total = (size_t)(end - p), sum = 0;
    if (lacing == 1) { /* Xiph: sizes as runs of 255 */
        for (int i = 0; i < nf - 1; i++) {
            size_t s = 0;
            uint8_t v;
            do {
                if (p >= end) return false;
                v = *p++;
                s += v;
            } while (v == 255 && s <= n);
            m->lace_size[i] = s;
            sum += s;
            if (sum > n) return false;
        }
    } else if (lacing == 3) { /* EBML: first size, then signed differences */
        int64_t prev = 0;
        for (int i = 0; i < nf - 1; i++) {
            if (p >= end) return false;
            int l = vint_len(*p);
            if (l > 8 || l > end - p) return false;
            uint64_t v = *p & (0xFFu >> l);
            for (int k = 1; k < l; k++) v = v << 8 | p[k];
            p += l;
            int64_t s;
            if (i == 0) s = (int64_t)v;
            else s = prev + ((int64_t)v - (((int64_t)1 << (7 * l - 1)) - 1));
            if (s < 0 || s > (int64_t)n) return false;
            m->lace_size[i] = (size_t)s;
            sum += (size_t)s;
            if (sum > n) return false;
            prev = s;
        }
    } else if (lacing == 2) { /* fixed */
        if (total % (size_t)nf) return false;
        for (int i = 0; i < nf - 1; i++) m->lace_size[i] = total / (size_t)nf;
        sum = total / (size_t)nf * (size_t)(nf - 1);
    }
    total = (size_t)(end - p);
    if (sum > total) return false;
    m->lace_size[nf - 1] = total - sum;
    size_t off = (size_t)(p - m->blk);
    for (int i = 0; i < nf; i++) {
        m->lace_off[i] = off;
        off += m->lace_size[i];
    }

    int64_t pts = MKV_NOPTS, units;
    if (!__builtin_add_overflow(m->cluster_ts, (int64_t)rel, &units)) pts = units_to_ns(m, units);
    int64_t dur = group_dur > 0 ? units_to_ns(m, group_dur) : 0;
    if (dur == MKV_NOPTS) dur = 0;
    if (nf > 1) dur = t->default_duration ? t->default_duration : dur / nf;
    else if (!dur) dur = t->default_duration;
    m->lace = (MkvFrame){.track = t->number,
                         .pts = pts,
                         .duration = dur,
                         .key = simple ? (flags & 0x80) != 0 : group_key,
                         .invisible = (flags & 0x08) != 0,
                         .discardable = simple && (flags & 0x01),
                         .pos = b->start};
    m->lace_track = t;
    m->lace_n = nf;
    m->lace_i = 0;
    return true;
}

static void load_group(Mkv *m, const Elem *g) {
    Elem e, blk = {0};
    bool have = false, ref = false;
    int64_t dur = 0;
    for (CHILDREN(m, g, e)) {
        if (e.id == ID_BLOCK && !have) blk = e, have = true;
        else if (e.id == ID_REFERENCEBLOCK) ref = true;
        else if (e.id == ID_BLOCKDURATION) {
            uint64_t d = get_uint(m, &e);
            dur = d < (1ull << 53) ? (int64_t)d : 0;
        }
    }
    if (have) load_block(m, &blk, false, !ref, dur);
}

static void emit(Mkv *m, MkvFrame *f) {
    int i = m->lace_i++;
    const MkvTrack *t = m->lace_track;
    *f = m->lace;
    if (i > 0) {
        int64_t step, pts;
        if (m->lace.pts == MKV_NOPTS || !t->default_duration ||
            __builtin_mul_overflow(t->default_duration, (int64_t)i, &step) ||
            __builtin_add_overflow(m->lace.pts, step, &pts))
            pts = MKV_NOPTS;
        f->pts = pts;
    }
    const uint8_t *d = m->blk + m->lace_off[i];
    size_t n = m->lace_size[i];
    if (t->strip_len) {
        if (grow(&m->out, &m->out_cap, t->strip_len + n)) {
            memcpy(m->out, t->strip, t->strip_len);
            memcpy(m->out + t->strip_len, d, n);
            d = m->out;
            n += t->strip_len;
        }
    }
    f->data = d;
    f->size = n;
}

/* The cluster whose id starts at q looks real: sane size and a cluster child first. */
static bool cluster_ok(Mkv *m, int64_t q, Elem *e) {
    Elem c;
    return read_elem(m, q, m->seg_end, e) && e->id == ID_CLUSTER && (e->size < 0 || fits(e, m->seg_end)) &&
           read_elem(m, e->data, m->seg_end, &c) &&
           (c.id == ID_TIMESTAMP || c.id == ID_CRC32 || c.id == ID_POSITION || c.id == ID_PREVSIZE || c.id == ID_VOID);
}

/* First plausible cluster starting in [from, limit). */
static bool find_cluster(Mkv *m, int64_t from, int64_t limit, int64_t *at) {
    uint8_t buf[4096];
    if (limit > m->seg_end) limit = m->seg_end;
    for (int64_t p = from; p + 4 <= limit;) {
        size_t n = limit - p < (int64_t)sizeof buf ? (size_t)(limit - p) : sizeof buf;
        if (!rd(m, p, buf, n)) return false;
        for (size_t i = 0; i + 4 <= n; i++) {
            if (buf[i] != 0x1F || buf[i + 1] != 0x43 || buf[i + 2] != 0xB6 || buf[i + 3] != 0x75) continue;
            Elem e;
            if (cluster_ok(m, p + (int64_t)i, &e)) {
                *at = p + (int64_t)i;
                return true;
            }
        }
        if (n < 4) break;
        p += (int64_t)(n - 3);
    }
    return false;
}

/* Damaged data: carry on at the next cluster after m->pos (or stop at the end). */
static void resync(Mkv *m) {
    int64_t at;
    m->in_cluster = false;
    if (find_cluster(m, m->pos + 1, m->seg_end, &at)) m->pos = at;
    else m->pos = m->seg_end;
}

int mkv_next(Mkv *m, MkvFrame *f) {
    if (m->mp4) return mp4_next(m, f);
    for (;;) {
        if (m->lace_i < m->lace_n) {
            emit(m, f);
            return 1;
        }
        m->lace_n = m->lace_i = 0;
        if (m->read_failed) return -1;
        if (m->pos < 0 || m->pos >= m->seg_end) return 0;
        if (m->in_cluster && m->pos >= m->cluster_end) m->in_cluster = false;
        Elem e;
        if (!read_elem(m, m->pos, m->in_cluster ? m->cluster_end : m->seg_end, &e)) {
            if (m->read_failed) return -1;
            resync(m);
            continue;
        }
        if (!m->in_cluster) {
            if (e.id == ID_CLUSTER) {
                m->in_cluster = true;
                m->cluster_unknown = e.size < 0;
                m->cluster_end = fits(&e, m->seg_end) ? e.data + e.size : m->seg_end;
                m->cluster_ts = 0;
                m->pos = e.data;
            } else if (is_level1(e.id) && fits(&e, m->seg_end)) {
                m->pos = e.data + e.size;
            } else {
                resync(m);
            }
            continue;
        }
        if (!is_cluster_child(e.id)) {
            if (m->cluster_unknown && is_level1(e.id)) m->in_cluster = false; /* end of a live cluster */
            else resync(m);
            continue;
        }
        if (!fits(&e, m->cluster_end)) {
            resync(m);
            continue;
        }
        if (e.id == ID_TIMESTAMP) {
            uint64_t ts = get_uint(m, &e);
            m->cluster_ts = ts < (1ull << 53) ? (int64_t)ts : 0;
        } else if (e.id == ID_SIMPLEBLOCK) {
            load_block(m, &e, true, false, 0);
        } else if (e.id == ID_BLOCKGROUP) {
            load_group(m, &e);
        }
        m->pos = e.data + e.size;
    }
}

void mkv_rewind(Mkv *m) {
    if (m->mp4) {
        mp4_rewind(m);
        return;
    }
    m->pos = m->first_cluster >= 0 ? m->first_cluster : m->seg_end;
    m->in_cluster = false;
    m->lace_n = m->lace_i = 0;
    m->read_failed = false;
}

/* Timestamp (units) of the cluster at q, -1 when it has none near its start. */
static int64_t cluster_time(Mkv *m, int64_t q) {
    Elem cl, e;
    if (!read_elem(m, q, m->seg_end, &cl) || cl.id != ID_CLUSTER) return -1;
    int64_t end = fits(&cl, m->seg_end) ? cl.data + cl.size : m->seg_end;
    int64_t p = cl.data;
    for (int k = 0; k < 8 && read_elem(m, p, end, &e) && fits(&e, end); k++) {
        if (e.id == ID_TIMESTAMP) {
            uint64_t ts = get_uint(m, &e);
            return ts < (1ull << 53) ? (int64_t)ts : -1;
        }
        if (e.id == ID_SIMPLEBLOCK || e.id == ID_BLOCKGROUP) break;
        p = e.data + e.size;
    }
    return -1;
}

/* The cluster after the one at q: its declared end, else a scan. */
static bool next_cluster(Mkv *m, int64_t q, int64_t *next) {
    Elem cl, e;
    if (read_elem(m, q, m->seg_end, &cl) && fits(&cl, m->seg_end)) {
        int64_t p = cl.data + cl.size;
        for (int k = 0; k < 16 && read_elem(m, p, m->seg_end, &e); k++) {
            if (e.id == ID_CLUSTER && cluster_ok(m, p, &e)) {
                *next = p;
                return true;
            }
            if (!is_level1(e.id) || !fits(&e, m->seg_end)) break;
            p = e.data + e.size;
        }
    }
    return find_cluster(m, q + 1, m->seg_end, next);
}

/* Last cluster starting at or before time t (units), by bisecting the file. */
static int64_t bisect(Mkv *m, int64_t t) {
    int64_t lo = m->first_cluster, hi = m->seg_end, best = m->first_cluster;
    for (int k = 0; k < 64 && hi - lo > (1 << 20); k++) {
        int64_t mid = lo + (hi - lo) / 2, at, ts;
        if (!find_cluster(m, mid, hi, &at) || (ts = cluster_time(m, at)) < 0) {
            hi = mid;
            continue;
        }
        if (ts <= t) {
            best = at;
            lo = at + 1;
        } else {
            hi = mid;
        }
    }
    for (int k = 0; k < 100000; k++) {
        int64_t at, ts;
        if (!next_cluster(m, best, &at) || (ts = cluster_time(m, at)) < 0 || ts > t) break;
        best = at;
    }
    return best;
}

bool mkv_seek(Mkv *m, int64_t t_ns, int track) {
    if (m->mp4) return mp4_seek(m, t_ns, track);
    mkv_rewind(m);
    if (m->first_cluster < 0) return false;
    if (!track) {
        const MkvTrack *v = mkv_find_track(m, MKV_VIDEO);
        track = v ? v->number : 0;
    }
    int64_t t = t_ns <= 0 ? 0 : t_ns / m->scale;
    if (m->ncues) {
        /* binary search: first cue later than t */
        int lo = 0, hi = m->ncues;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (m->cues[mid].time <= t) lo = mid + 1;
            else hi = mid;
        }
        bool track_has_cues = false;
        for (int i = 0; i < m->ncues && !track_has_cues; i++) track_has_cues = m->cues[i].track == track;
        for (int i = lo - 1; i >= 0; i--) {
            if (track_has_cues && m->cues[i].track != track) continue;
            Elem e;
            if (cluster_ok(m, m->cues[i].pos, &e)) {
                m->pos = m->cues[i].pos;
                return true;
            }
        }
        return true; /* before the first cue: from the start */
    }
    m->pos = bisect(m, t);
    return true;
}
