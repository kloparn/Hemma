/* modules/media/probe.c — read container headers for duration, resolution
 * and codecs, and judge whether a browser can play the file.
 *
 * MP4/MOV (ISO BMFF boxes, moov at the start or the end), Matroska/WebM
 * (EBML: Info + Tracks, via SeekHead when they sit after the clusters) and
 * AVI (RIFF hdrl). TS/WMV/MPEG-PS are only recognised. Every read is
 * bounds-checked: a truncated or hostile file yields "unknown", never a crash. */
#define _FILE_OFFSET_BITS 64 /* 64-bit fseeko/off_t on 32-bit glibc and mingw */
#include "mediainfo.h"
#include "../../core/i18n.h"
#include "../../core/plat.h"

#include <ctype.h>
#include <sys/types.h>

#define MOOV_CAP (64u << 20) /* (2.2: a long film's copy with eight sound tracks has a moov of tens of MB) */
#define MKV_EL_CAP (4u << 20)
#define AVI_HEAD_CAP (512u << 10)

typedef struct {
    const uint8_t *p;
    size_t n;
} Span;

/* ------------------------------------------------------------ byte order */

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---------------------------------------------------------------- reading */

static bool rd(const MediaReader *r, int64_t off, void *dst, size_t n) {
    if (off < 0 || !n || (uint64_t)n > (uint64_t)r->size || off > r->size - (int64_t)n) return false;
    return r->read_at(r->ctx, off, dst, n);
}

/* Read n bytes into a fresh buffer; NULL when out of range, over cap or failing. */
static uint8_t *rd_alloc(const MediaReader *r, int64_t off, uint64_t n, uint64_t cap) {
    if (!n || n > cap || n > (uint64_t)SIZE_MAX) return NULL;
    if (off < 0 || off > r->size || n > (uint64_t)(r->size - off)) return NULL; /* before any big malloc */
    uint8_t *p = malloc((size_t)n);
    if (!p) return NULL;
    if (!rd(r, off, p, (size_t)n)) {
        free(p);
        return NULL;
    }
    return p;
}

static int64_t mul_div(uint64_t a, uint64_t b, uint64_t c) { /* a*b/c without overflowing for b <= 1000 */
    if (!c) return 0;
    uint64_t v = (a / c) * b + (a % c) * b / c;
    return v > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)v;
}

static void set_str(char *dst, size_t cap, const char *src) { str_copy(dst, cap, src ? src : ""); }

/* ===================================================================== MP4 */

typedef struct {
    char kind; /* 'v' 'a' or 0 */
    bool enabled;
    int w, h;
    char codec[12];
    int channels;
    int64_t media_ms;
    uint16_t lang;         /* mdhd's packed ISO 639-2 (0 = none) */
    char name[sizeof ((MediaSound *)0)->name]; /* the track's own name (sound_name): "" = none */
} Track;

static bool box_next(const Span *parent, size_t *pos, char type[5], Span *payload) {
    if (*pos > parent->n || parent->n - *pos < 8) return false;
    const uint8_t *b = parent->p + *pos;
    size_t left = parent->n - *pos;
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
    *pos += (size_t)size;
    return true;
}

static bool find_box(Span parent, const char *type, Span *out) {
    size_t pos = 0;
    char t[5];
    Span pl;
    while (box_next(&parent, &pos, t, &pl))
        if (!memcmp(t, type, 4)) {
            *out = pl;
            return true;
        }
    return false;
}

static bool desc_header(const uint8_t *p, size_t n, size_t *i, int *tag) {
    if (*i >= n) return false;
    *tag = p[(*i)++];
    for (int k = 0; k < 4; k++) { /* expandable length: up to 4 bytes with continuation bits */
        if (*i >= n) return false;
        if (!(p[(*i)++] & 0x80)) break;
    }
    return true;
}

/* objectTypeIndication from an esds box (-1 when absent). */
static int esds_oti(Span esds) {
    const uint8_t *p = esds.p;
    size_t n = esds.n, i = 4; /* skip version/flags */
    int tag;
    if (n < 5 || !desc_header(p, n, &i, &tag) || tag != 0x03 || n - i < 3) return -1;
    i += 2; /* ES_ID */
    uint8_t fl = p[i++];
    if (fl & 0x80) i += 2; /* dependsOn_ES_ID */
    if (fl & 0x40) {       /* URL */
        if (i >= n) return -1;
        i += 1 + (size_t)p[i];
    }
    if (fl & 0x20) i += 2; /* OCR_ES_Id */
    if (i >= n || !desc_header(p, n, &i, &tag) || tag != 0x04 || i >= n) return -1;
    return p[i];
}

static const char *mp4_video(const char *f) {
    static const struct {
        const char *fourcc, *name;
    } map[] = {
        {"avc1", "h264"}, {"avc2", "h264"}, {"avc3", "h264"}, {"avc4", "h264"}, {"dvav", "h264"},
        {"dva1", "h264"}, {"hvc1", "hevc"}, {"hev1", "hevc"}, {"dvh1", "hevc"}, {"dvhe", "hevc"},
        {"av01", "av1"},  {"vp09", "vp9"},  {"vp08", "vp8"},  {"mp4v", "mpeg4"}, {"s263", "h263"},
        {"h263", "h263"}, {"jpeg", "mjpeg"}, {"mjpa", "mjpeg"}, {"mjpb", "mjpeg"}, {"apch", "prores"},
        {"apcn", "prores"}, {"apcs", "prores"}, {"apco", "prores"}, {"ap4h", "prores"}, {"ap4x", "prores"},
        {"m2v1", "mpeg2"}, {"mp2v", "mpeg2"}, {"xvid", "mpeg4"}, {"divx", "mpeg4"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!memcmp(f, map[i].fourcc, 4)) return map[i].name;
    return NULL;
}

static const char *mp4_audio(const char *f) {
    static const struct {
        const char *fourcc, *name;
    } map[] = {
        {"mp4a", "aac"},  {"ac-3", "ac3"},  {"ec-3", "eac3"}, {"ac-4", "ac4"},  {"Opus", "opus"}, {"opus", "opus"},
        {"fLaC", "flac"}, {"alac", "alac"}, {"lpcm", "pcm"},  {"sowt", "pcm"},  {"twos", "pcm"},  {"in24", "pcm"},
        {"in32", "pcm"},  {"fl32", "pcm"},  {"fl64", "pcm"},  {"raw ", "pcm"},  {"NONE", "pcm"},  {"ulaw", "pcm"},
        {"alaw", "pcm"},  {".mp3", "mp3"},  {"dtsc", "dts"},  {"dtsh", "dts"},  {"dtsl", "dts"},  {"dtse", "dts"},
        {"dtsx", "dts"},  {"mlpa", "truehd"}, {"samr", "amr"}, {"sawb", "amr"}, {"mha1", "mpegh"}, {"mhm1", "mpegh"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!memcmp(f, map[i].fourcc, 4)) return map[i].name;
    return NULL;
}

static void fourcc_name(const char *f, char *out, size_t cap) {
    char tmp[5];
    size_t n = 0;
    for (int i = 0; i < 4; i++)
        if (isalnum((unsigned char)f[i])) tmp[n++] = (char)tolower((unsigned char)f[i]);
    tmp[n] = 0;
    str_copy(out, cap, n ? tmp : "unknown");
}

/* Encrypted entries (encv/enca) carry the real format in sinf/frma. */
static bool original_format(Span children, char out[5]) {
    Span sinf, frma;
    if (!find_box(children, "sinf", &sinf) || !find_box(sinf, "frma", &frma) || frma.n < 4) return false;
    memcpy(out, frma.p, 4);
    out[4] = 0;
    return true;
}

static void sample_entry(Track *t, const char *fourcc_in, Span e) {
    char fourcc[5];
    memcpy(fourcc, fourcc_in, 5);
    Span kids = {NULL, 0};
    const char *name = NULL;
    if (t->kind == 'v') {
        if (e.n >= 28 && (!t->w || !t->h)) {
            t->w = be16(e.p + 24);
            t->h = be16(e.p + 26);
        }
        if (e.n > 78) kids = (Span){e.p + 78, e.n - 78};
        if (!memcmp(fourcc, "encv", 4) && kids.n) original_format(kids, fourcc);
        name = mp4_video(fourcc);
        Span esds;
        if (!memcmp(fourcc, "mp4v", 4) && kids.n && find_box(kids, "esds", &esds)) {
            int oti = esds_oti(esds);
            if (oti >= 0x60 && oti <= 0x65) name = "mpeg2";
            else if (oti == 0x6A) name = "mpeg2"; /* MPEG-1 video */
            else if (oti == 0x6C) name = "mjpeg";
        }
    } else {
        int ver = e.n >= 10 ? be16(e.p + 8) : 0;
        size_t child_off = ver == 1 ? 44 : ver == 2 ? 64 : 28;
        if (ver == 2 && e.n >= 44) t->channels = (int)be32(e.p + 40);
        else if (e.n >= 18) t->channels = be16(e.p + 16);
        if (t->channels < 0 || t->channels > 64) t->channels = 0;
        if (e.n > child_off) kids = (Span){e.p + child_off, e.n - child_off};
        if (!memcmp(fourcc, "enca", 4) && kids.n) original_format(kids, fourcc);
        name = mp4_audio(fourcc);
        if (!memcmp(fourcc, "mp4a", 4) && kids.n) {
            Span esds, wave;
            bool have = find_box(kids, "esds", &esds) || (find_box(kids, "wave", &wave) && find_box(wave, "esds", &esds));
            int oti = have ? esds_oti(esds) : -1;
            if (oti == 0x69 || oti == 0x6B) name = "mp3";
            else if (oti == 0xA5) name = "ac3";
            else if (oti == 0xA6) name = "eac3";
            else if (oti == 0xA9) name = "dts";
            else if (oti == 0xAD) name = "opus";
        }
    }
    if (name) set_str(t->codec, sizeof t->codec, name);
    else fourcc_name(fourcc, t->codec, sizeof t->codec);
}

/* A track name as people may read it: UTF-8 (bytes that aren't are Latin-1), no control characters, trimmed, cut
   whole letters short; "" for a muxer's handler name ("SoundHandler", "Core Media Audio", "GPAC ISO Audio Handler"
   ...) - those name the program, not the track. */
static void sound_name(const uint8_t *p, size_t n, char *out, size_t cap) {
    char tmp[256];
    size_t k = 0;
    bool utf8 = utf8_valid((const char *)p, n);
    for (size_t i = 0; i < n && k + 4 < sizeof tmp;) {
        uint32_t cp = p[i];
        int used = 1;
        if (!cp) break;
        if (utf8 && cp >= 0x80) used = utf8_decode((const char *)p + i, n - i, &cp);
        i += (size_t)(used > 0 ? used : 1);
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) cp = ' ';
        k += utf8_encode(cp, tmp + k);
    }
    tmp[k] = '\0';
    char *s = tmp;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len && s[len - 1] == ' ') s[--len] = '\0';
    static const char *const generic[] = {"Core Media Audio", "Apple Sound Media Handler", "Sound Media Handler",
                                          "Mainconcept MP4 Sound Media Handler", "Bento4 Sound Handler", NULL};
    bool is_generic = !len || str_iends(s, "handler");
    for (int i = 0; generic[i] && !is_generic; i++) is_generic = str_ieq(s, generic[i]);
    if (is_generic || !cap) {
        if (cap) out[0] = '\0';
        return;
    }
    str_ncopy(out, cap, s, utf8_clip(s, cap - 1));
}

/* A sound trak's name: its udta 'name' (what ffmpeg writes a title as) or QuickTime's '©nam', else its hdlr name
   (a C string, or a Pascal one in old QuickTime files) - sound_name. */
static void trak_name(Span trak, Span hdlr, char *out, size_t cap) {
    out[0] = '\0';
    Span udta, nm;
    if (find_box(trak, "udta", &udta)) {
        if (find_box(udta, "name", &nm) && nm.n) sound_name(nm.p, nm.n, out, cap);
        else if (find_box(udta, "\xA9nam", &nm) && nm.n > 4 && be16(nm.p) <= nm.n - 4) sound_name(nm.p + 4, be16(nm.p), out, cap);
        if (out[0]) return;
    }
    if (hdlr.n <= 24) return;
    const uint8_t *s = hdlr.p + 24;
    size_t n = hdlr.n - 24;
    if (n >= 2 && ((size_t)s[0] + 1 == n || ((size_t)s[0] + 2 == n && !s[n - 1])) && s[0]) sound_name(s + 1, s[0], out, cap);
    else sound_name(s, n, out, cap);
}

static void parse_trak(Span trak, Track *t) {
    Span tkhd, mdia, mdhd, hdlr = {NULL, 0}, minf, stbl, stsd;
    if (find_box(trak, "tkhd", &tkhd) && tkhd.n >= 4) {
        size_t wo = tkhd.p[0] == 1 ? 88 : 76;
        t->enabled = (tkhd.p[3] & 1) != 0;
        if (tkhd.n >= wo + 8) {
            t->w = (int)(be32(tkhd.p + wo) >> 16);
            t->h = (int)(be32(tkhd.p + wo + 4) >> 16);
        }
    }
    if (!find_box(trak, "mdia", &mdia)) return;
    if (find_box(mdia, "mdhd", &mdhd) && mdhd.n >= 20) {
        uint64_t ts = 0, dur = 0;
        if (mdhd.p[0] == 1 && mdhd.n >= 32) {
            ts = be32(mdhd.p + 20);
            dur = be64(mdhd.p + 24);
            if (dur == UINT64_MAX) dur = 0;
            if (mdhd.n >= 34) t->lang = be16(mdhd.p + 32);
        } else if (mdhd.p[0] == 0) {
            ts = be32(mdhd.p + 12);
            dur = be32(mdhd.p + 16);
            if (dur == 0xFFFFFFFFu) dur = 0;
            if (mdhd.n >= 22) t->lang = be16(mdhd.p + 20);
        }
        t->media_ms = mul_div(dur, 1000, ts);
    }
    if (find_box(mdia, "hdlr", &hdlr) && hdlr.n >= 12) {
        if (!memcmp(hdlr.p + 8, "vide", 4)) t->kind = 'v';
        else if (!memcmp(hdlr.p + 8, "soun", 4)) t->kind = 'a';
    }
    if (!t->kind) return;
    if (t->kind == 'a') trak_name(trak, hdlr, t->name, sizeof t->name);
    if (!find_box(mdia, "minf", &minf) || !find_box(minf, "stbl", &stbl) || !find_box(stbl, "stsd", &stsd) ||
        stsd.n < 8)
        return;
    Span entries = {stsd.p + 8, stsd.n - 8};
    size_t pos = 0;
    char fourcc[5];
    Span e;
    if (box_next(&entries, &pos, fourcc, &e)) sample_entry(t, fourcc, e);
}

static void take_track(MediaProbe *o, const Track *t) {
    if (t->kind == 'v') {
        set_str(o->vcodec, sizeof o->vcodec, t->codec);
        if (t->w > 0 && t->h > 0 && t->w < 100000 && t->h < 100000) o->width = t->w, o->height = t->h;
    } else {
        set_str(o->acodec, sizeof o->acodec, t->codec);
        o->audio_channels = t->channels;
    }
}

static bool parse_moov(Span moov, MediaProbe *o) {
    Span b;
    uint64_t ts = 0, dur = 0;
    if (find_box(moov, "mvhd", &b) && b.n >= 20) {
        if (b.p[0] == 1 && b.n >= 32) {
            ts = be32(b.p + 20);
            dur = be64(b.p + 24);
            if (dur == UINT64_MAX) dur = 0;
        } else if (b.p[0] == 0) {
            ts = be32(b.p + 12);
            dur = be32(b.p + 16);
            if (dur == 0xFFFFFFFFu) dur = 0;
        }
    }
    Span mvex, mehd; /* fragmented MP4: the movie header often says 0 */
    if (!dur && find_box(moov, "mvex", &mvex) && find_box(mvex, "mehd", &mehd) && mehd.n >= 8)
        dur = mehd.p[0] == 1 && mehd.n >= 12 ? be64(mehd.p + 4) : be32(mehd.p + 4);

    Track best_v = {0}, best_a = {0};
    int64_t longest = 0;
    size_t pos = 0;
    char type[5];
    Span trak;
    while (box_next(&moov, &pos, type, &trak)) {
        if (memcmp(type, "trak", 4)) continue;
        Track t = {0};
        parse_trak(trak, &t);
        if (t.media_ms > longest) longest = t.media_ms;
        if (t.kind == 'a' && o->nsounds < 1000) o->nsounds++;
        Track *slot = t.kind == 'v' ? &best_v : t.kind == 'a' ? &best_a : NULL;
        if (slot && (!slot->kind || (!slot->enabled && t.enabled))) *slot = t;
    }
    if (best_v.kind) take_track(o, &best_v);
    if (best_a.kind) take_track(o, &best_a);
    o->duration_ms = (ts && dur) ? mul_div(dur, 1000, ts) : longest;
    return true;
}

static bool probe_mp4(const MediaReader *r, MediaProbe *o) {
    if (!o->container[0] || (strcmp(o->container, "mov") && strcmp(o->container, "mp4")))
        set_str(o->container, sizeof o->container, "mp4");
    int64_t off = 0;
    for (int guard = 0; guard < 100000 && off <= r->size - 8; guard++) {
        uint8_t h[16];
        if (!rd(r, off, h, 8)) break;
        uint64_t size = be32(h);
        int hl = 8;
        if (size == 1) {
            if (!rd(r, off + 8, h + 8, 8)) break;
            size = be64(h + 8);
            hl = 16;
        } else if (size == 0) {
            size = (uint64_t)(r->size - off);
        }
        for (int i = 4; i < 8; i++)
            if (h[i] < 0x20 || h[i] > 0x7E) return false; /* not a box: corrupt */
        if (size < (uint64_t)hl) return false;
        bool moov = !memcmp(h + 4, "moov", 4);
        if (size > (uint64_t)(r->size - off)) {
            if (!moov) return false; /* truncated (e.g. an unfinished download) */
            size = (uint64_t)(r->size - off);
        }
        if (!memcmp(h + 4, "ftyp", 4) && size >= (uint64_t)hl + 4) {
            uint8_t brand[4];
            if (rd(r, off + hl, brand, 4) && !memcmp(brand, "qt  ", 4)) set_str(o->container, sizeof o->container, "mov");
        } else if (moov) {
            uint8_t *m = rd_alloc(r, off + hl, size - (uint64_t)hl, MOOV_CAP);
            if (!m) return false;
            bool ok = parse_moov((Span){m, (size_t)(size - (uint64_t)hl)}, o);
            free(m);
            return ok;
        }
        off += (int64_t)size;
    }
    return false;
}

static bool looks_like_bmff(const uint8_t *h, size_t n);

/* The moov's payload, read whole (the walk as probe_mp4's); NULL when there's none or it can't be read. */
static uint8_t *mp4_moov(const MediaReader *r, size_t *n) {
    int64_t off = 0;
    for (int guard = 0; guard < 100000 && r->size >= 8 && off <= r->size - 8; guard++) {
        uint8_t h[16];
        if (!rd(r, off, h, 8)) break;
        uint64_t size = be32(h);
        int hl = 8;
        if (size == 1) {
            if (!rd(r, off + 8, h + 8, 8)) break;
            size = be64(h + 8);
            hl = 16;
        } else if (size == 0) {
            size = (uint64_t)(r->size - off);
        }
        for (int i = 4; i < 8; i++)
            if (h[i] < 0x20 || h[i] > 0x7E) return NULL;
        if (size < (uint64_t)hl) return NULL;
        bool moov = !memcmp(h + 4, "moov", 4);
        if (size > (uint64_t)(r->size - off)) {
            if (!moov) return NULL;
            size = (uint64_t)(r->size - off);
        }
        if (moov) {
            *n = (size_t)(size - (uint64_t)hl);
            return rd_alloc(r, off + hl, size - (uint64_t)hl, MOOV_CAP);
        }
        off += (int64_t)size;
    }
    return NULL;
}

/* mdhd's packed language (three letters of 5 bits, + 0x60 each) as two letters; "" for "und", none or a
   QuickTime Macintosh language code (under 0x400) */
static void lang_of(uint16_t code, char *out, size_t cap) {
    out[0] = '\0';
    if (code < 0x400 || code == 0x7FFF) return;
    char l3[4] = {(char)(((code >> 10) & 31) + 0x60), (char)(((code >> 5) & 31) + 0x60), (char)((code & 31) + 0x60), 0};
    for (int i = 0; i < 3; i++)
        if (l3[i] < 'a' || l3[i] > 'z') return;
    if (!strcmp(l3, "hin")) { /* (subtitles read "hi" as hearing impaired; a sound track speaks Hindi) */
        str_copy(out, cap, "hi");
        return;
    }
    MediaSubTags t;
    media_sub_track_tags(l3, NULL, false, &t);
    str_copy(out, cap, t.lang);
}

int media_mp4_sounds(const MediaReader *r, MediaSound *out, int max) {
    if (!r || !r->read_at || r->size < 8) return -1;
    uint8_t head[16];
    if (!rd(r, 0, head, 16 <= r->size ? 16 : 8) || !looks_like_bmff(head, 8)) return -1;
    size_t n = 0;
    uint8_t *m = mp4_moov(r, &n);
    if (!m) return -1;
    Span moov = {m, n}, trak;
    size_t pos = 0;
    char type[5];
    int count = 0, first_on = -1;
    while (box_next(&moov, &pos, type, &trak)) {
        if (memcmp(type, "trak", 4)) continue;
        Track t = {0};
        parse_trak(trak, &t);
        if (t.kind != 'a') continue;
        if (count < max && out) {
            MediaSound *s = &out[count];
            memset(s, 0, sizeof *s);
            lang_of(t.lang, s->lang, sizeof s->lang);
            str_copy(s->name, sizeof s->name, t.name);
            str_copy(s->codec, sizeof s->codec, t.codec);
            s->channels = t.channels;
            if (t.enabled && first_on < 0) first_on = count;
        }
        count++;
    }
    free(m);
    int shown = count < max ? count : max;
    for (int i = 0; out && i < shown; i++) out[i].on = i == (first_on >= 0 ? first_on : 0);
    return count;
}

static bool looks_like_bmff(const uint8_t *h, size_t n) {
    static const char *const types[] = {"ftyp", "moov", "mdat", "wide", "free", "skip", "pnot", "styp", "sidx", NULL};
    if (n < 8) return false;
    uint32_t size = be32(h);
    if (size != 0 && size != 1 && size < 8) return false;
    for (int i = 0; types[i]; i++)
        if (!memcmp(h + 4, types[i], 4)) return true;
    return false;
}

/* ================================================================ Matroska */

enum {
    MKV_EBML = 0x1A45DFA3,
    MKV_DOCTYPE = 0x4282,
    MKV_SEGMENT = 0x18538067,
    MKV_SEEKHEAD = 0x114D9B74,
    MKV_SEEK = 0x4DBB,
    MKV_SEEKID = 0x53AB,
    MKV_SEEKPOS = 0x53AC,
    MKV_INFO = 0x1549A966,
    MKV_TIMESCALE = 0x2AD7B1,
    MKV_DURATION = 0x4489,
    MKV_TRACKS = 0x1654AE6B,
    MKV_TRACKENTRY = 0xAE,
    MKV_TRACKTYPE = 0x83,
    MKV_CODECID = 0x86,
    MKV_CODECPRIV = 0x63A2,
    MKV_FLAGDEFAULT = 0x88,
    MKV_FLAGENABLED = 0xB9,
    MKV_VIDEO = 0xE0,
    MKV_PIXELW = 0xB0,
    MKV_PIXELH = 0xBA,
    MKV_AUDIO = 0xE1,
    MKV_CHANNELS = 0x9F,
    MKV_CLUSTER = 0x1F43B675,
};

static int vint_len(uint8_t b) {
    for (int i = 0; i < 8; i++)
        if (b & (0x80 >> i)) return i + 1;
    return 0;
}

/* Element header: id (1-4 bytes, marker kept) + size (1-8 bytes, all ones = unknown). */
static bool ebml_header(const uint8_t *p, size_t n, uint32_t *id, uint64_t *size, bool *unknown, int *hlen) {
    if (!n) return false;
    int il = vint_len(p[0]);
    if (il < 1 || il > 4 || (size_t)il >= n) return false;
    uint32_t v = 0;
    for (int i = 0; i < il; i++) v = v << 8 | p[i];
    int sl = vint_len(p[il]);
    if (sl < 1 || (size_t)(il + sl) > n) return false;
    uint64_t s = p[il] & (0xFF >> sl);
    bool ones = s == (uint64_t)(0xFF >> sl);
    for (int i = 1; i < sl; i++) {
        s = s << 8 | p[il + i];
        if (p[il + i] != 0xFF) ones = false;
    }
    *id = v, *size = s, *unknown = ones, *hlen = il + sl;
    return true;
}

typedef struct {
    uint32_t id;
    Span data;
} Ebml;

static bool ebml_next(const Span *s, size_t *pos, Ebml *e) {
    if (*pos >= s->n) return false;
    const uint8_t *p = s->p + *pos;
    size_t left = s->n - *pos;
    uint64_t size;
    bool unknown;
    int hl;
    if (!ebml_header(p, left, &e->id, &size, &unknown, &hl)) return false;
    size_t room = left - (size_t)hl;
    if (unknown || size > room) size = room; /* clamp to the parent */
    e->data = (Span){p + hl, (size_t)size};
    *pos += (size_t)hl + (size_t)size;
    return true;
}

static uint64_t ebml_uint(Span d) {
    if (d.n > 8) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < d.n; i++) v = v << 8 | d.p[i];
    return v;
}

static double ebml_float(Span d) {
    if (d.n == 4) {
        uint32_t u = be32(d.p);
        float f;
        memcpy(&f, &u, sizeof f);
        return f;
    }
    if (d.n == 8) {
        uint64_t u = be64(d.p);
        double f;
        memcpy(&f, &u, sizeof f);
        return f;
    }
    return 0;
}

static void ebml_string(Span d, char *out, size_t cap) {
    size_t n = d.n < cap - 1 ? d.n : cap - 1;
    memcpy(out, d.p, n);
    out[n] = 0;
}

static const char *vfw_fourcc(const uint8_t *f) {
    static const struct {
        const char *fourcc, *name;
    } map[] = {
        {"h264", "h264"}, {"x264", "h264"}, {"avc1", "h264"}, {"davc", "h264"}, {"vssh", "h264"}, {"hevc", "hevc"},
        {"h265", "hevc"}, {"hvc1", "hevc"}, {"x265", "hevc"}, {"xvid", "mpeg4"}, {"divx", "mpeg4"}, {"dx50", "mpeg4"},
        {"fmp4", "mpeg4"}, {"mp4v", "mpeg4"}, {"div3", "mpeg4"}, {"div4", "mpeg4"}, {"mp42", "mpeg4"},
        {"mp43", "mpeg4"}, {"3iv2", "mpeg4"}, {"xvix", "mpeg4"}, {"dm4v", "mpeg4"}, {"mjpg", "mjpeg"},
        {"jpeg", "mjpeg"}, {"mpg1", "mpeg2"}, {"mpg2", "mpeg2"}, {"mpeg", "mpeg2"}, {"mp2v", "mpeg2"},
        {"wmv1", "wmv"},  {"wmv2", "wmv"},  {"wmv3", "wmv"},  {"wvc1", "vc1"},   {"vp80", "vp8"},   {"vp90", "vp9"},
        {"av01", "av1"},  {"iv50", "indeo"}, {"iv41", "indeo"}, {"cvid", "cinepak"},
    };
    char lo[5];
    for (int i = 0; i < 4; i++) lo[i] = (char)tolower(f[i]);
    lo[4] = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!memcmp(lo, map[i].fourcc, 4)) return map[i].name;
    return NULL;
}

static const char *wave_tag(unsigned tag) {
    switch (tag) {
    case 0x0001:
    case 0x0003: return "pcm";
    case 0x0050: return "mp2";
    case 0x0055: return "mp3";
    case 0x00FF:
    case 0x1600:
    case 0x1610:
    case 0x706D: return "aac";
    case 0x2000: return "ac3";
    case 0x2001: return "dts";
    case 0x0161:
    case 0x0162:
    case 0x0163: return "wma";
    case 0x0011: return "adpcm";
    case 0x6771: return "vorbis";
    case 0xF1AC: return "flac";
    default: return NULL;
    }
}

static const char *mkv_codec(const char *id, Span priv) {
    static const struct {
        const char *id, *name;
        bool prefix;
    } map[] = {
        {"V_MPEG4/ISO/AVC", "h264", false},  {"V_MPEGH/ISO/HEVC", "hevc", false}, {"V_AV1", "av1", false},
        {"V_VP9", "vp9", false},             {"V_VP8", "vp8", false},             {"V_MPEG4/ISO/", "mpeg4", true},
        {"V_MPEG4/MS/V3", "mpeg4", false},   {"V_MPEG2", "mpeg2", false},         {"V_MPEG1", "mpeg2", false},
        {"V_THEORA", "theora", false},       {"V_PRORES", "prores", false},       {"V_REAL/", "real", true},
        {"V_MJPEG", "mjpeg", false},         {"V_FFV1", "ffv1", false},           {"A_AAC", "aac", true},
        {"A_EAC3", "eac3", false},           {"A_AC3", "ac3", true},              {"A_DTS", "dts", true},
        {"A_TRUEHD", "truehd", false},       {"A_MLP", "truehd", false},          {"A_OPUS", "opus", false},
        {"A_VORBIS", "vorbis", false},       {"A_FLAC", "flac", false},           {"A_MPEG/L3", "mp3", false},
        {"A_MPEG/L2", "mp2", false},         {"A_MPEG/L1", "mp2", false},         {"A_PCM/", "pcm", true},
        {"A_ALAC", "alac", false},           {"A_REAL/", "real", true},           {"A_TTA1", "tta", false},
        {"A_WAVPACK4", "wavpack", false},
    };
    if (!strcmp(id, "V_MS/VFW/FOURCC")) return priv.n >= 20 ? vfw_fourcc(priv.p + 16) : NULL;
    if (!strcmp(id, "A_MS/ACM")) return priv.n >= 2 ? wave_tag(le16(priv.p)) : NULL;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t k = strlen(map[i].id);
        if (map[i].prefix ? !strncmp(id, map[i].id, k) : !strcmp(id, map[i].id)) return map[i].name;
    }
    return NULL;
}

typedef struct {
    bool have_info, have_tracks;
    uint64_t seek_info, seek_tracks; /* relative to the segment data start; 0 = none */
    uint64_t timescale;
    double duration;
    bool have_v, have_a, a_default;
} MkvState;

static void mkv_seekhead(Span s, MkvState *st) {
    size_t pos = 0;
    Ebml seek;
    while (ebml_next(&s, &pos, &seek)) {
        if (seek.id != MKV_SEEK) continue;
        size_t p2 = 0;
        Ebml e;
        uint32_t id = 0;
        uint64_t at = 0;
        while (ebml_next(&seek.data, &p2, &e)) {
            if (e.id == MKV_SEEKID && e.data.n <= 4) id = (uint32_t)ebml_uint(e.data);
            else if (e.id == MKV_SEEKPOS) at = ebml_uint(e.data);
        }
        if (id == MKV_INFO && at) st->seek_info = at;
        if (id == MKV_TRACKS && at) st->seek_tracks = at;
    }
}

static void mkv_info(Span s, MkvState *st) {
    size_t pos = 0;
    Ebml e;
    while (ebml_next(&s, &pos, &e)) {
        if (e.id == MKV_TIMESCALE) {
            uint64_t v = ebml_uint(e.data);
            if (v) st->timescale = v;
        } else if (e.id == MKV_DURATION) {
            st->duration = ebml_float(e.data);
        }
    }
    st->have_info = true;
}

static void mkv_tracks(Span s, MkvState *st, MediaProbe *o) {
    if (st->have_tracks) return; /* (a second Tracks element: the first counts) */
    size_t pos = 0;
    Ebml te;
    while (ebml_next(&s, &pos, &te)) {
        if (te.id != MKV_TRACKENTRY) continue;
        uint64_t type = 0, def = 1, enabled = 1, w = 0, h = 0, ch = 1;
        char codec[48] = "";
        Span priv = {NULL, 0};
        size_t p2 = 0;
        Ebml e;
        while (ebml_next(&te.data, &p2, &e)) {
            switch (e.id) {
            case MKV_TRACKTYPE: type = ebml_uint(e.data); break;
            case MKV_CODECID: ebml_string(e.data, codec, sizeof codec); break;
            case MKV_CODECPRIV: priv = e.data; break;
            case MKV_FLAGDEFAULT: def = ebml_uint(e.data); break;
            case MKV_FLAGENABLED: enabled = ebml_uint(e.data); break;
            case MKV_VIDEO:
            case MKV_AUDIO: {
                size_t p3 = 0;
                Ebml v;
                while (ebml_next(&e.data, &p3, &v)) {
                    if (v.id == MKV_PIXELW) w = ebml_uint(v.data);
                    else if (v.id == MKV_PIXELH) h = ebml_uint(v.data);
                    else if (v.id == MKV_CHANNELS) ch = ebml_uint(v.data);
                }
                break;
            }
            default: break;
            }
        }
        if (!enabled) continue;
        if (type == 2 && o->nsounds < 1000) o->nsounds++;
        const char *name = mkv_codec(codec, priv);
        char fallback[12];
        if (!name) {
            fold_copy(fallback, sizeof fallback, codec[0] && codec[1] == '_' ? codec + 2 : codec);
            name = fallback[0] ? fallback : "unknown";
        }
        if (type == 1 && !st->have_v) {
            st->have_v = true;
            set_str(o->vcodec, sizeof o->vcodec, name);
            if (w && h && w < 100000 && h < 100000) o->width = (int)w, o->height = (int)h;
        } else if (type == 2 && (!st->have_a || (!st->a_default && def))) {
            st->have_a = true;
            st->a_default = def != 0;
            set_str(o->acodec, sizeof o->acodec, name);
            o->audio_channels = ch < 64 ? (int)ch : 0;
        }
    }
    st->have_tracks = true;
}

/* Read the element header at `off` via the reader. */
static bool mkv_peek(const MediaReader *r, int64_t off, uint32_t *id, uint64_t *size, bool *unknown, int *hl) {
    uint8_t h[12];
    if (off < 0 || off >= r->size) return false;
    size_t n = (size_t)MIN((int64_t)sizeof h, r->size - off);
    if (!rd(r, off, h, n)) return false;
    return ebml_header(h, n, id, size, unknown, hl);
}

static bool mkv_load(const MediaReader *r, int64_t off, uint32_t want, MkvState *st, MediaProbe *o) {
    uint32_t id;
    uint64_t size;
    bool unknown;
    int hl;
    if (!mkv_peek(r, off, &id, &size, &unknown, &hl) || id != want || unknown) return false;
    uint8_t *buf = rd_alloc(r, off + hl, size, MKV_EL_CAP);
    if (!buf) return false;
    Span s = {buf, (size_t)size};
    if (want == MKV_INFO) mkv_info(s, st);
    else if (want == MKV_TRACKS) mkv_tracks(s, st, o);
    else if (want == MKV_SEEKHEAD) mkv_seekhead(s, st);
    free(buf);
    return true;
}

static bool probe_mkv(const MediaReader *r, MediaProbe *o) {
    uint32_t id;
    uint64_t size;
    bool unknown;
    int hl;
    if (!mkv_peek(r, 0, &id, &size, &unknown, &hl) || id != MKV_EBML || unknown) return false;
    set_str(o->container, sizeof o->container, "mkv");
    uint8_t *hdr = rd_alloc(r, hl, size, 4096);
    if (hdr) {
        Span s = {hdr, (size_t)size};
        size_t pos = 0;
        Ebml e;
        while (ebml_next(&s, &pos, &e))
            if (e.id == MKV_DOCTYPE && e.data.n == 4 && !memcmp(e.data.p, "webm", 4))
                set_str(o->container, sizeof o->container, "webm");
        free(hdr);
    }
    /* find the Segment among the top-level elements */
    int64_t off = hl + (int64_t)size, seg = -1, seg_end = r->size;
    for (int i = 0; i < 16 && off < r->size; i++) {
        if (!mkv_peek(r, off, &id, &size, &unknown, &hl)) return false;
        if (id == MKV_SEGMENT) {
            seg = off + hl;
            if (!unknown && size < (uint64_t)(r->size - seg)) seg_end = seg + (int64_t)size;
            break;
        }
        if (unknown || size > (uint64_t)(r->size - off - hl)) return false;
        off += hl + (int64_t)size;
    }
    if (seg < 0) return false;

    MkvState st = {0};
    st.timescale = 1000000;
    off = seg;
    for (int i = 0; i < 1024 && off < seg_end && !(st.have_info && st.have_tracks); i++) {
        if (!mkv_peek(r, off, &id, &size, &unknown, &hl)) break;
        if (id == MKV_CLUSTER || unknown) break; /* media data from here on */
        int64_t room = seg_end - off - hl;
        if (room < 0) break;
        if (id == MKV_SEEKHEAD || id == MKV_INFO || id == MKV_TRACKS) mkv_load(r, off, id, &st, o);
        if (size > (uint64_t)room) break;
        off += hl + (int64_t)size;
    }
    if (!st.have_info && st.seek_info && st.seek_info < (uint64_t)(seg_end - seg))
        mkv_load(r, seg + (int64_t)st.seek_info, MKV_INFO, &st, o);
    if (!st.have_tracks && st.seek_tracks && st.seek_tracks < (uint64_t)(seg_end - seg))
        mkv_load(r, seg + (int64_t)st.seek_tracks, MKV_TRACKS, &st, o);

    double ms = st.duration * (double)st.timescale / 1e6;
    if (ms > 0 && ms < 1e11) o->duration_ms = (int64_t)(ms + 0.5);
    return st.have_info || st.have_tracks;
}

/* ===================================================================== AVI */

typedef struct {
    uint32_t us_per_frame, total_frames, dml_frames;
    int w, h;
    uint32_t v_scale, v_rate, v_length;
    bool have_v, have_a;
} AviState;

static bool riff_next(const Span *s, size_t *pos, char id[5], Span *data) {
    if (*pos > s->n || s->n - *pos < 8) return false;
    const uint8_t *p = s->p + *pos;
    memcpy(id, p, 4);
    id[4] = 0;
    uint32_t sz = le32(p + 4);
    size_t avail = s->n - *pos - 8;
    data->p = p + 8;
    data->n = sz <= avail ? sz : avail;
    uint64_t next = (uint64_t)*pos + 8 + sz + (sz & 1);
    *pos = next > s->n ? s->n : (size_t)next;
    return true;
}

static void avi_strl(Span s, AviState *st, MediaProbe *o) {
    size_t pos = 0;
    char id[5];
    Span d;
    char type = 0;
    while (riff_next(&s, &pos, id, &d)) {
        if (!strcmp(id, "strh") && d.n >= 36) {
            type = !memcmp(d.p, "vids", 4) ? 'v' : !memcmp(d.p, "auds", 4) ? 'a' : 0;
            if (type == 'v' && !st->have_v) {
                st->v_scale = le32(d.p + 20);
                st->v_rate = le32(d.p + 24);
                st->v_length = le32(d.p + 32);
                const char *n = vfw_fourcc(d.p + 4);
                if (n) set_str(o->vcodec, sizeof o->vcodec, n);
            }
        } else if (!strcmp(id, "strf") && type == 'v' && !st->have_v) {
            if (d.n >= 20) {
                const char *n = vfw_fourcc(d.p + 16);
                if (n) set_str(o->vcodec, sizeof o->vcodec, n);
                else if (!o->vcodec[0]) fourcc_name((const char *)d.p + 16, o->vcodec, sizeof o->vcodec);
            }
            st->have_v = true;
        } else if (!strcmp(id, "strf") && type == 'a' && !st->have_a) {
            if (d.n >= 4) {
                const char *n = wave_tag(le16(d.p));
                set_str(o->acodec, sizeof o->acodec, n ? n : "unknown");
                o->audio_channels = le16(d.p + 2);
            }
            st->have_a = true;
        }
    }
}

static void avi_hdrl(Span s, AviState *st, MediaProbe *o) {
    size_t pos = 0;
    char id[5];
    Span d;
    while (riff_next(&s, &pos, id, &d)) {
        if (!strcmp(id, "avih") && d.n >= 40) {
            st->us_per_frame = le32(d.p);
            st->total_frames = le32(d.p + 16);
            st->w = (int)le32(d.p + 32);
            st->h = (int)le32(d.p + 36);
        } else if (!strcmp(id, "LIST") && d.n >= 4) {
            Span inner = {d.p + 4, d.n - 4};
            if (!memcmp(d.p, "strl", 4)) avi_strl(inner, st, o);
            else if (!memcmp(d.p, "odml", 4)) {
                size_t p2 = 0;
                char id2[5];
                Span d2;
                while (riff_next(&inner, &p2, id2, &d2))
                    if (!strcmp(id2, "dmlh") && d2.n >= 4) st->dml_frames = le32(d2.p);
            }
        }
    }
}

static bool probe_avi(const MediaReader *r, MediaProbe *o) {
    set_str(o->container, sizeof o->container, "avi");
    size_t n = (size_t)MIN(r->size, (int64_t)AVI_HEAD_CAP);
    uint8_t *buf = rd_alloc(r, 0, n, AVI_HEAD_CAP);
    if (!buf) return false;
    AviState st = {0};
    bool found = false;
    Span all = {buf + 12, n - 12};
    size_t pos = 0;
    char id[5];
    Span d;
    while (riff_next(&all, &pos, id, &d)) {
        if (strcmp(id, "LIST") || d.n < 4) continue;
        if (!memcmp(d.p, "hdrl", 4)) {
            uint32_t declared = le32(d.p - 4);
            if (declared > d.n && declared <= 4u << 20) { /* header list bigger than our first read */
                int64_t at = (int64_t)(d.p - buf);
                uint8_t *full = rd_alloc(r, at, declared, 4u << 20);
                if (full) {
                    avi_hdrl((Span){full + 4, declared - 4}, &st, o);
                    free(full);
                }
            } else {
                avi_hdrl((Span){d.p + 4, d.n - 4}, &st, o);
            }
            found = true;
            break;
        }
        if (!memcmp(d.p, "movi", 4)) break;
    }
    free(buf);
    if (!found) return false;
    double ms = 0;
    if (st.dml_frames && st.us_per_frame) ms = (double)st.dml_frames * st.us_per_frame / 1000.0;
    else if (st.v_rate && st.v_length) ms = (double)st.v_length * st.v_scale * 1000.0 / st.v_rate;
    else if (st.total_frames && st.us_per_frame) ms = (double)st.total_frames * st.us_per_frame / 1000.0;
    if (ms > 0 && ms < 1e11) o->duration_ms = (int64_t)(ms + 0.5);
    if (st.w > 0 && st.h > 0 && st.w < 100000 && st.h < 100000) o->width = st.w, o->height = st.h;
    return true;
}

/* =========================================================== playability */

static const char *video_label(const char *c) {
    if (!strcmp(c, "mpeg4")) return "MPEG-4 Part 2 (XviD/DivX)";
    if (!strcmp(c, "mpeg2")) return "MPEG-2";
    if (!strcmp(c, "vc1")) return "VC-1";
    if (!strcmp(c, "mjpeg")) return "Motion JPEG";
    if (!strcmp(c, "prores")) return "ProRes";
    if (!strcmp(c, "wmv")) return "WMV";
    return c;
}

static int severity(Playable p) { return p == PLAY_NO ? 3 : p == PLAY_MAYBE ? 2 : p == PLAY_UNKNOWN ? 1 : 0; }

/* The verdict's words: kept in English in the probe (the scan's thread, the cache), said again in the
   reader's language by media_probe_why() - tr = in the language in use. */
static const char *W(bool tr, const char *en) { return tr ? T(en) : en; }

static void judge_in(MediaProbe *o, bool parsed, bool tr) {
    const char *ct = o->container, *vc = o->vcodec, *ac = o->acodec;
    o->playable = PLAY_YES;
    o->why[0] = 0;
    if (!parsed) {
        o->playable = PLAY_UNKNOWN;
        set_str(o->why, sizeof o->why, ct[0] ? W(tr, N_("couldn't read the file header (incomplete or damaged?)"))
                                              : W(tr, N_("unrecognised file format")));
        return;
    }
    bool webm = !strcmp(ct, "webm"), mkv = !strcmp(ct, "mkv");
    Playable pv = PLAY_YES, pa = PLAY_YES, pc = PLAY_YES;
    char wv[96] = "", wa[96] = "", wc[96] = "";

    if (!vc[0] || !strcmp(vc, "h264")) {
    } else if (!strcmp(vc, "hevc")) {
        pv = PLAY_MAYBE;
        set_str(wv, sizeof wv, W(tr, N_("HEVC needs hardware support (Safari/Edge, some Chrome)")));
    } else if (!strcmp(vc, "vp9") || !strcmp(vc, "av1") || !strcmp(vc, "vp8")) {
        if (!webm && !mkv) {
            pv = PLAY_MAYBE;
            const char *name = !strcmp(vc, "av1") ? "AV1" : !strcmp(vc, "vp9") ? "VP9" : "VP8";
            if (!strcmp(vc, "av1") && !strcmp(ct, "mp4")) /* (what the converter makes of an AV1 MKV, 2.1) */
                set_str(wv, sizeof wv, W(tr, N_("AV1 needs a newer device (Chrome, Edge, Firefox; the newest Apple devices)")));
            else if (ct[0]) snprintf(wv, sizeof wv, W(tr, N_("%s in %s doesn't play everywhere")), name, ct);
            else snprintf(wv, sizeof wv, W(tr, N_("%s in this container doesn't play everywhere")), name);
        }
    } else {
        pv = PLAY_NO;
        snprintf(wv, sizeof wv, W(tr, N_("%s video needs conversion")), video_label(vc));
    }

    if (!ac[0] || !strcmp(ac, "aac") || !strcmp(ac, "mp3") || !strcmp(ac, "opus") || !strcmp(ac, "flac")) {
    } else if (!strcmp(ac, "vorbis")) {
        if (!webm && !mkv) {
            pa = PLAY_MAYBE;
            set_str(wa, sizeof wa, W(tr, N_("Vorbis audio outside WebM doesn't play everywhere")));
        }
    } else if (!strcmp(ac, "ac3") || !strcmp(ac, "eac3")) {
        pa = PLAY_MAYBE;
        set_str(wa, sizeof wa, W(tr, N_("Dolby sound: Safari/Edge only - silent in Chrome and Firefox")));
    } else if (!strcmp(ac, "dts")) {
        pa = PLAY_NO;
        set_str(wa, sizeof wa, W(tr, N_("DTS audio isn't supported by browsers")));
    } else if (!strcmp(ac, "truehd")) {
        pa = PLAY_NO;
        set_str(wa, sizeof wa, W(tr, N_("TrueHD audio isn't supported by browsers")));
    } else if (!strcmp(ac, "alac")) {
        pa = PLAY_MAYBE;
        set_str(wa, sizeof wa, W(tr, N_("ALAC audio: Safari only")));
    } else if (!strcmp(ac, "pcm")) {
        pa = PLAY_MAYBE;
        set_str(wa, sizeof wa, W(tr, N_("uncompressed PCM audio may not play")));
    } else {
        pa = PLAY_NO;
        snprintf(wa, sizeof wa, W(tr, N_("%s audio isn't supported by browsers")), ac);
    }

    if (!strcmp(ct, "mp4") || !strcmp(ct, "mov") || webm) {
    } else if (mkv) {
        pc = PLAY_MAYBE;
        set_str(wc, sizeof wc, W(tr, N_("MKV plays in Chrome/Edge, not Safari/iOS")));
    } else if (!strcmp(ct, "avi")) {
        pc = PLAY_NO;
        set_str(wc, sizeof wc, W(tr, N_("AVI needs conversion")));
    } else if (!strcmp(ct, "ts")) {
        pc = PLAY_NO;
        set_str(wc, sizeof wc, W(tr, N_("MPEG-TS needs conversion")));
    } else if (!strcmp(ct, "wmv")) {
        pc = PLAY_NO;
        set_str(wc, sizeof wc, W(tr, N_("WMV needs conversion")));
    } else if (!strcmp(ct, "mpg")) {
        pc = PLAY_NO;
        set_str(wc, sizeof wc, W(tr, N_("MPEG-PS needs conversion")));
    } else {
        pc = PLAY_UNKNOWN;
        set_str(wc, sizeof wc, W(tr, N_("unknown container")));
    }

    /* most restrictive verdict wins; on ties codecs explain better than containers, and sound that
       plays silently is the bigger surprise than a picture needing new hardware */
    Playable best = pv;
    const char *why = wv;
    if (severity(pa) > severity(best) || (pa == PLAY_MAYBE && pv == PLAY_MAYBE)) best = pa, why = wa;
    if (severity(pc) > severity(best)) best = pc, why = wc;
    o->playable = best;
    set_str(o->why, sizeof o->why, best == PLAY_YES ? "" : why);
}

static void judge(MediaProbe *o, bool parsed) { judge_in(o, parsed, false); }

void media_probe_judge(MediaProbe *p) { judge(p, true); }

/* p->why in the language in use: the same verdict judged again in its words (a file whose header
   couldn't be read: its English sentence looked up). Anything else stays as it was. */
void media_probe_why(const MediaProbe *p, char *out, size_t cap) {
    if (i18n_find(p->why)) {
        str_copy(out, cap, T(p->why));
        return;
    }
    MediaProbe again = *p;
    judge_in(&again, true, true);
    str_copy(out, cap, again.playable == p->playable && again.why[0] ? again.why : p->why);
}

/* ===================================================================== API */

static const char *ext_of(const char *name) {
    if (!name) return "";
    const char *slash = strrchr(name, '/'), *bslash = strrchr(name, '\\');
    if (bslash > slash) slash = bslash;
    const char *dot = strrchr(name, '.');
    return dot && (!slash || dot > slash) ? dot + 1 : "";
}

static bool ext_in(const char *name, const char *const *list) {
    const char *e = ext_of(name);
    for (; *list; list++)
        if (str_ieq(e, *list)) return true;
    return false;
}

bool media_is_video_ext(const char *name) {
    static const char *const v[] = {"mp4", "m4v", "mkv", "webm", "mov", "avi", "ts", "m2ts", "mts", "wmv", "mpg", "mpeg", NULL};
    return ext_in(name, v);
}

bool media_is_sub_ext(const char *name) {
    static const char *const v[] = {"srt", "vtt", "sup", "idx", NULL}; /* (+ PGS and VobSub pictures) */
    return ext_in(name, v);
}

bool media_is_image_ext(const char *name) {
    static const char *const v[] = {"jpg", "jpeg", "png", "webp", NULL};
    return ext_in(name, v);
}

bool media_is_audio_ext(const char *name) {
    static const char *const v[] = {"m4b", "m4a", "mp3", NULL};
    return ext_in(name, v);
}

bool media_is_books_folder(const char *name) {
    static const char *const words[] = {"audiobooks", "audiobook", "audio books", "audio book", "ljudböcker", "ljudbocker",
                                        "ljudbok", "audiolibros", "audiolibro", "hörbücher", "hörbuch", "lydbøger",
                                        "lydbøker", "äänikirjat", NULL};
    char f[96];
    fold_copy(f, sizeof f, name ? name : "");
    for (char *p = f; *p; p++)
        if (*p == '_' || *p == '.' || *p == '-') *p = ' ';
    str_trim(f);
    for (int i = 0; words[i]; i++)
        if (str_eq(f, words[i])) return true;
    return false;
}

bool media_is_book_file(const char *root_name, const char *rel) {
    if (!media_is_audio_ext(rel)) return false;
    if (str_iends(rel, ".m4b")) return true; /* (an audiobook by its very kind) */
    if (root_name && media_is_books_folder(root_name)) return true;
    for (const char *s = rel, *slash; (slash = strchr(s, '/')); s = slash + 1) { /* a folder above it says so */
        char seg[96];
        str_ncopy(seg, sizeof seg, s, (size_t)(slash - s));
        if (media_is_books_folder(seg)) return true;
    }
    return false;
}

static const char *container_from_ext(const char *name) {
    const char *e = ext_of(name);
    static const struct {
        const char *ext, *c;
    } map[] = {{"mp4", "mp4"}, {"m4v", "mp4"}, {"3gp", "mp4"}, {"mov", "mov"},  {"mkv", "mkv"},
               {"webm", "webm"}, {"avi", "avi"}, {"ts", "ts"},   {"m2ts", "ts"}, {"mts", "ts"},
               {"wmv", "wmv"}, {"asf", "wmv"}, {"mpg", "mpg"}, {"mpeg", "mpg"}, {"vob", "mpg"}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (str_ieq(e, map[i].ext)) return map[i].c;
    return "";
}

const char *media_mime(const char *c) {
    if (!c) return "application/octet-stream";
    if (!strcmp(c, "mp4")) return "video/mp4";
    if (!strcmp(c, "mov")) return "video/quicktime";
    if (!strcmp(c, "webm")) return "video/webm";
    if (!strcmp(c, "mkv")) return "video/x-matroska";
    if (!strcmp(c, "avi")) return "video/x-msvideo";
    if (!strcmp(c, "ts")) return "video/mp2t";
    if (!strcmp(c, "wmv")) return "video/x-ms-wmv";
    if (!strcmp(c, "mpg")) return "video/mpeg";
    return "application/octet-stream";
}

const char *media_playable_label(Playable p) {
    switch (p) {
    case PLAY_YES: return N_("Plays in browsers");
    case PLAY_MAYBE: return N_("May not play everywhere");
    case PLAY_NO: return N_("Needs conversion");
    default: return N_("Unknown");
    }
}

void media_fmt_duration(int64_t ms, char *out, size_t cap) {
    if (!cap) return;
    if (ms <= 0) {
        out[0] = 0;
        return;
    }
    int64_t s = (ms + 500) / 1000;
    if (s < 60) snprintf(out, cap, "%lld s", (long long)s);
    else if (s < 3600) snprintf(out, cap, "%lld min", (long long)((s + 30) / 60));
    else {
        int64_t m = (s + 30) / 60;
        snprintf(out, cap, "%lld h %lld min", (long long)(m / 60), (long long)(m % 60));
    }
}

static bool is_ts(const MediaReader *r, const uint8_t *h, size_t hn) {
    uint8_t b[2];
    if (hn >= 1 && h[0] == 0x47 && r->size > 376 && rd(r, 188, b, 1) && rd(r, 376, b + 1, 1))
        return b[0] == 0x47 && b[1] == 0x47;
    if (hn >= 5 && h[4] == 0x47 && r->size > 388 && rd(r, 196, b, 1) && rd(r, 388, b + 1, 1)) /* m2ts: 192-byte packets */
        return b[0] == 0x47 && b[1] == 0x47;
    return false;
}

bool media_probe(const MediaReader *r, const char *filename_hint, MediaProbe *o) {
    static const uint8_t asf_guid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                         0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
    memset(o, 0, sizeof *o);
    set_str(o->container, sizeof o->container, container_from_ext(filename_hint));
    bool ok = false;
    uint8_t head[64];
    size_t hn = (r && r->read_at && r->size > 0) ? (size_t)MIN(r->size, (int64_t)sizeof head) : 0;
    if (hn && rd(r, 0, head, hn)) {
        if (looks_like_bmff(head, hn)) {
            ok = probe_mp4(r, o);
        } else if (hn >= 4 && be32(head) == MKV_EBML) {
            ok = probe_mkv(r, o);
        } else if (hn >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "AVI ", 4)) {
            ok = probe_avi(r, o);
        } else if (is_ts(r, head, hn)) {
            set_str(o->container, sizeof o->container, "ts");
            ok = true;
        } else if (hn >= 16 && !memcmp(head, asf_guid, 16)) {
            set_str(o->container, sizeof o->container, "wmv");
            ok = true;
        } else if (hn >= 4 && be32(head) == 0x000001BA) {
            set_str(o->container, sizeof o->container, "mpg");
            ok = true;
        }
    }
    judge(o, ok);
    return ok;
}

typedef struct {
    const uint8_t *p;
    size_t n;
} MemCtx;

static bool mem_read_at(void *ctx, int64_t off, void *dst, size_t n) {
    const MemCtx *m = ctx;
    if (off < 0 || (uint64_t)off > m->n || n > m->n - (size_t)off) return false;
    memcpy(dst, m->p + off, n);
    return true;
}

bool media_probe_mem(const void *data, size_t n, const char *filename_hint, MediaProbe *out) {
    MemCtx m = {data, data ? n : 0};
    MediaReader r = {&m, (int64_t)m.n, mem_read_at};
    return media_probe(&r, filename_hint, out);
}

static bool file_read_at(void *ctx, int64_t off, void *dst, size_t n) {
    FILE *f = ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return false;
    return fread(dst, 1, n, f) == n;
}

bool media_probe_file(const char *path, MediaProbe *out) {
    int64_t size = 0;
    bool is_dir = false;
    FILE *f = NULL;
    if (path_stat(path, &size, NULL, &is_dir) && !is_dir) f = file_open(path, "rb");
    if (!f) {
        MediaReader none = {NULL, 0, NULL};
        media_probe(&none, path, out);
        return false;
    }
    setvbuf(f, NULL, _IOFBF, 64 * 1024);
    MediaReader r = {f, size, file_read_at};
    bool ok = media_probe(&r, path, out);
    fclose(f);
    return ok;
}
