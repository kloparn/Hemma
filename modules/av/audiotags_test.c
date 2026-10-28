/* modules/av/audiotags_test.c — audiotags.c read back from audiobooks made here (the builders of
 * audiotags_testmk.h are defined below): every field of an MP4 (moov first or last, a QuickTime chapter track or
 * Nero's chpl, a cover, a real AAC sound) and of MP3s (ID3v2.2/2.3/2.4 in every text encoding, unsynchronised,
 * CHAP, APIC, Xing / VBRI / constant bitrate, ID3v1), then damaged ones: cut short anywhere, absurd counts and
 * sizes, boxes in boxes. Every result, damaged or not, must keep audiotags.h's promises (sane()).
 *
 *   HEMMA_TEST_TAGS=<file> ./build/test audiotags_real           what a real file says (vs ffprobe by eye)
 *   HEMMA_FUZZ_RAW=<dir> ./build/test audiotags_fuzz_seeds        seeds for tools/avfuzz.c -DAVFUZZ_TAGS
 *   HEMMA_TEST_TAGS_OUT=<file.m4b> ./build/test audiotags_m4b_with_real_aac   the real-AAC audiobook, kept */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "aac.h"
#include "audiotags.h"
#include "audiotags_testmk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- writing */

static void p8(Buf *b, uint32_t v) {
    char x = (char)(v & 0xFF);
    buf_put(b, &x, 1);
}
static void p16(Buf *b, uint32_t v) { p8(b, v >> 8), p8(b, v); }
static void p32(Buf *b, uint32_t v) { p16(b, v >> 16), p16(b, v); }
static void p64(Buf *b, uint64_t v) { p32(b, (uint32_t)(v >> 32)), p32(b, (uint32_t)v); }
static void pz(Buf *b, size_t n) {
    while (n--) p8(b, 0);
}
static void ps(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static void set32(Buf *b, size_t at, uint32_t v) {
    uint8_t *p = (uint8_t *)b->p + at;
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static size_t bx(Buf *b, const char *type) {
    size_t at = b->len;
    p32(b, 0);
    buf_put(b, type, 4);
    return at;
}
static size_t fbx(Buf *b, const char *type, uint32_t version_flags) {
    size_t at = bx(b, type);
    p32(b, version_flags);
    return at;
}
static void bend(Buf *b, size_t at) { set32(b, at, (uint32_t)(b->len - at)); }

/* ------------------------------------------------------------- the m4b */

typedef struct {
    int rate, ch, ns;
    uint32_t delta;     /* each sound sample's ticks */
    uint64_t total;     /* the sound's ticks */
    int64_t ms;         /* the movie's length */
    int64_t snd_at, text_at;
    const uint32_t *tsize; /* the chapter samples' sizes */
} Lay;

static void matrix(Buf *b) {
    static const uint32_t M[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
    for (int i = 0; i < 9; i++) p32(b, M[i]);
}

static void tkhd(Buf *b, uint32_t flags, uint32_t id, int64_t ms, bool sound) {
    size_t a = fbx(b, "tkhd", flags);
    p32(b, 0), p32(b, 0), p32(b, id), p32(b, 0), p32(b, (uint32_t)ms);
    pz(b, 8);
    p16(b, 0), p16(b, sound ? 1 : 0), p16(b, sound ? 0x100 : 0), p16(b, 0);
    matrix(b);
    p32(b, 0), p32(b, 0);
    bend(b, a);
}

static void mdhd_hdlr(Buf *b, uint32_t scale, uint64_t dur, const char *handler, const char *name) {
    size_t a = fbx(b, "mdhd", 0);
    p32(b, 0), p32(b, 0), p32(b, scale), p32(b, (uint32_t)dur), p16(b, 0x55C4), p16(b, 0); /* "und" */
    bend(b, a);
    a = fbx(b, "hdlr", 0);
    p32(b, 0), ps(b, handler), pz(b, 12), ps(b, name), p8(b, 0);
    bend(b, a);
}

static void dinf(Buf *b) {
    size_t a = bx(b, "dinf"), d = fbx(b, "dref", 0);
    p32(b, 1);
    bend(b, fbx(b, "url ", 1)); /* the media is in this file */
    bend(b, d), bend(b, a);
}

/* stts with one run, stsc with one chunk, stsz, stco */
static void tables(Buf *b, int n, const uint32_t *runs /* n counts + deltas, or NULL */, uint32_t delta,
                   const uint32_t *sizes, uint32_t size, int64_t at) {
    size_t a = fbx(b, "stts", 0);
    if (runs) {
        p32(b, (uint32_t)n);
        for (int i = 0; i < n; i++) p32(b, 1), p32(b, runs[i]);
    } else p32(b, 1), p32(b, (uint32_t)n), p32(b, delta);
    bend(b, a);
    a = fbx(b, "stsc", 0);
    p32(b, 1), p32(b, 1), p32(b, (uint32_t)n), p32(b, 1);
    bend(b, a);
    a = fbx(b, "stsz", 0);
    p32(b, 0), p32(b, (uint32_t)n);
    for (int i = 0; i < n; i++) p32(b, sizes ? sizes[i] : size);
    bend(b, a);
    a = fbx(b, "stco", 0);
    p32(b, 1), p32(b, (uint32_t)at);
    bend(b, a);
}

static void item(Buf *b, const char *id, uint32_t type, const void *p, size_t n) {
    size_t a = bx(b, id), d = bx(b, "data");
    p32(b, type), p32(b, 0), buf_put(b, p, n);
    bend(b, d), bend(b, a);
}

static void item_text(Buf *b, const char *id, const char *s) {
    if (s) item(b, id, 1, s, strlen(s));
}

static void ilst(Buf *b, const AtTestM4b *m) {
    size_t a = bx(b, "ilst");
    item_text(b, "\251nam", m->title);
    size_t x = bx(b, "----"), y = fbx(b, "mean", 0); /* a custom one, passed over */
    ps(b, "com.apple.iTunes"), bend(b, y);
    y = fbx(b, "name", 0), ps(b, "SUBTITLE"), bend(b, y);
    item(b, "data", 1, "not the title", 13); /* (an item named 'data' holding a data box: close enough) */
    bend(b, x);
    item_text(b, "\251ART", m->artist);
    item_text(b, "aART", m->album_artist);
    item_text(b, "\251alb", m->album);
    item_text(b, "\251gen", m->genre);
    if (m->gnre) {
        uint8_t g[2] = {(uint8_t)(m->gnre >> 8), (uint8_t)m->gnre};
        item(b, "gnre", 0, g, 2);
    }
    if (m->year) {
        char y4[32];
        snprintf(y4, sizeof y4, "%d-11-04T00:00:00Z", m->year);
        item_text(b, "\251day", y4);
    }
    item_text(b, "\251nrt", m->narrator);
    if (m->track) {
        uint8_t t[8] = {0, 0, (uint8_t)(m->track >> 8), (uint8_t)m->track, (uint8_t)(m->tracks >> 8), (uint8_t)m->tracks, 0, 0};
        item(b, "trkn", 0, t, 8);
    }
    if (m->disc) {
        uint8_t d[6] = {0, 0, (uint8_t)(m->disc >> 8), (uint8_t)m->disc, (uint8_t)(m->discs >> 8), (uint8_t)m->discs};
        item(b, "disk", 0, d, 6);
    }
    item_text(b, "\251cmt", m->cmt); /* (the weakest first: the order mustn't matter) */
    item_text(b, "desc", m->desc);
    item_text(b, "\251des", m->des);
    item_text(b, "ldes", m->ldes);
    if (m->cover_len) item(b, "covr", m->cover[0] == 0xFF ? 13 : 14, m->cover, m->cover_len);
    bend(b, a);
}

static void moov(Buf *b, const AtTestM4b *m, const Lay *l) {
    int nc = m->nchapters;
    bool qt = nc && (m->qt || !m->chpl), late = nc && m->chapters[0].start_ms > 0;
    size_t mv = bx(b, "moov"), a, k, md, mi, st, e;
    a = fbx(b, "mvhd", 0);
    p32(b, 0), p32(b, 0), p32(b, 1000), p32(b, (uint32_t)l->ms), p32(b, 0x10000), p16(b, 0x100), pz(b, 10);
    matrix(b);
    pz(b, 24), p32(b, 3);
    bend(b, a);
    /* the sound */
    k = bx(b, "trak");
    tkhd(b, 3, 1, l->ms, true);
    if (m->priming) {
        a = bx(b, "edts"), e = fbx(b, "elst", 0);
        p32(b, 1), p32(b, (uint32_t)l->ms), p32(b, (uint32_t)m->priming), p32(b, 0x10000);
        bend(b, e), bend(b, a);
    }
    if (qt) {
        a = bx(b, "tref"), e = bx(b, "chap");
        p32(b, 2);
        bend(b, e), bend(b, a);
    }
    md = bx(b, "mdia");
    mdhd_hdlr(b, (uint32_t)l->rate, l->total, "soun", "SoundHandler");
    mi = bx(b, "minf");
    a = fbx(b, "smhd", 0), p32(b, 0), bend(b, a);
    dinf(b);
    st = bx(b, "stbl");
    a = fbx(b, "stsd", 0);
    p32(b, 1);
    e = bx(b, "mp4a");
    pz(b, 6), p16(b, 1), pz(b, 8), p16(b, (uint32_t)l->ch), p16(b, 16), p32(b, 0), p32(b, (uint32_t)l->rate << 16);
    uint8_t made[2] = {0};
    const uint8_t *asc = m->asc;
    size_t an = m->asc_len;
    if (!asc) { /* AAC-LC, the rate's index, the channels */
        static const int R[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
        int fi = 4;
        for (int i = 0; i < 12; i++)
            if (R[i] == l->rate) fi = i;
        made[0] = (uint8_t)(2 << 3 | fi >> 1), made[1] = (uint8_t)((fi & 1) << 7 | l->ch << 3);
        asc = made, an = 2;
    }
    size_t es = fbx(b, "esds", 0);
    p8(b, 0x03), p8(b, (uint32_t)(23 + an)), p16(b, 1), p8(b, 0);                    /* ES_Descriptor */
    p8(b, 0x04), p8(b, (uint32_t)(15 + an)), p8(b, 0x40), p8(b, 0x15), p8(b, 0), p16(b, 0); /* MPEG-4 audio */
    p32(b, 0), p32(b, 0);
    p8(b, 0x05), p8(b, (uint32_t)an), buf_put(b, asc, an); /* DecoderSpecificInfo: the AudioSpecificConfig */
    p8(b, 0x06), p8(b, 1), p8(b, 2);                       /* SLConfigDescriptor */
    bend(b, es), bend(b, e), bend(b, a);
    uint32_t *sizes = malloc(sizeof(uint32_t) * (size_t)l->ns);
    for (int i = 0; i < l->ns; i++) sizes[i] = m->aac_frames > 0 ? (uint32_t)m->aac_len[i] : 6;
    tables(b, l->ns, NULL, l->delta, sizes, 0, l->snd_at);
    free(sizes);
    bend(b, st), bend(b, mi), bend(b, md), bend(b, k);
    /* the chapter track: a QuickTime text track, times in the sound's timescale */
    if (qt) {
        k = bx(b, "trak");
        tkhd(b, 2, 2, l->ms, false);
        int64_t s0 = m->chapters[0].start_ms;
        if (late) { /* an empty edit, then the track */
            a = bx(b, "edts"), e = fbx(b, "elst", 0);
            p32(b, 2), p32(b, (uint32_t)s0), p32(b, 0xFFFFFFFFu), p32(b, 0x10000);
            p32(b, (uint32_t)(l->ms - s0)), p32(b, 0), p32(b, 0x10000);
            bend(b, e), bend(b, a);
        }
        uint32_t *runs = malloc(sizeof(uint32_t) * (size_t)nc);
        uint64_t sum = 0;
        for (int i = 0; i < nc; i++) {
            int64_t end = i + 1 < nc ? m->chapters[i + 1].start_ms : l->ms;
            int64_t d = (end - s0) * l->rate / 1000 - (m->chapters[i].start_ms - s0) * l->rate / 1000;
            runs[i] = d > 0 ? (uint32_t)d : 1;
            sum += runs[i];
        }
        md = bx(b, "mdia");
        mdhd_hdlr(b, (uint32_t)l->rate, sum, "text", "TextHandler");
        mi = bx(b, "minf");
        a = bx(b, "gmhd"), e = fbx(b, "gmin", 0);
        p16(b, 0x40), p16(b, 0x8000), p16(b, 0x8000), p16(b, 0x8000), p16(b, 0), p16(b, 0);
        bend(b, e);
        e = bx(b, "text"), matrix(b), bend(b, e);
        bend(b, a);
        dinf(b);
        st = bx(b, "stbl");
        a = fbx(b, "stsd", 0);
        p32(b, 1);
        e = bx(b, "text"); /* as FFmpeg writes a chapter track's */
        pz(b, 6), p16(b, 1), pz(b, 7), p8(b, 1), pz(b, 35);
        bend(b, e), bend(b, a);
        tables(b, nc, runs, 0, l->tsize, 0, l->text_at);
        free(runs);
        bend(b, st), bend(b, mi), bend(b, md), bend(b, k);
    }
    /* tags */
    size_t u = bx(b, "udta");
    if (m->chpl && nc) {
        a = fbx(b, "chpl", 0x01000000);
        p32(b, 0), p8(b, (uint32_t)nc);
        for (int i = 0; i < nc; i++) {
            size_t tl = strlen(m->chapters[i].title);
            p64(b, (uint64_t)m->chapters[i].start_ms * 10000), p8(b, (uint32_t)tl), buf_put(b, m->chapters[i].title, tl);
        }
        bend(b, a);
    }
    a = m->meta_qt ? bx(b, "meta") : fbx(b, "meta", 0);
    e = fbx(b, "hdlr", 0);
    p32(b, 0), ps(b, "mdirappl"), pz(b, 8), p16(b, 0);
    bend(b, e);
    ilst(b, m);
    bend(b, a);
    bend(b, bx(b, "free"));
    bend(b, u);
    bend(b, mv);
}

void at_test_m4b(Buf *out, const AtTestM4b *m) {
    Lay l = {.rate = m->sample_rate ? m->sample_rate : 44100, .ch = m->channels ? m->channels : 2};
    bool real = m->aac_frames > 0;
    l.ns = real ? m->aac_frames : m->fake_frames > 0 ? m->fake_frames : 8;
    int64_t want = m->duration_ms ? m->duration_ms : 60000;
    l.delta = real ? AAC_FRAME : (uint32_t)(want * l.rate / 1000 / l.ns);
    if (!l.delta) l.delta = 1;
    l.total = (uint64_t)l.ns * l.delta;
    l.ms = (int64_t)((l.total - (uint64_t)m->priming) * 1000 / (uint64_t)l.rate);
    /* the media: the sound, then a sample per chapter (a length, the title, FFmpeg's 'encd': UTF-8) */
    Buf data = {0};
    for (int i = 0, at = 0; i < l.ns; i++) {
        if (real) buf_put(&data, m->aac + at, (size_t)m->aac_len[i]), at += m->aac_len[i];
        else pz(&data, 6);
    }
    size_t snd_len = data.len;
    uint32_t *tsize = calloc((size_t)(m->nchapters > 0 ? m->nchapters : 1), sizeof *tsize);
    for (int i = 0; i < m->nchapters; i++) {
        size_t at = data.len, tl = strlen(m->chapters[i].title);
        static const uint8_t encd[12] = {0, 0, 0, 12, 'e', 'n', 'c', 'd', 0, 0, 1, 0};
        p16(&data, (uint32_t)tl), buf_put(&data, m->chapters[i].title, tl), buf_put(&data, encd, 12);
        tsize[i] = (uint32_t)(data.len - at);
    }
    l.tsize = tsize;
    Buf ftyp = {0}, mv = {0};
    size_t a = bx(&ftyp, "ftyp");
    ps(&ftyp, "M4A "), p32(&ftyp, 0), ps(&ftyp, "M4A M4B mp42isom");
    bend(&ftyp, a);
    moov(&mv, m, &l); /* (its size doesn't depend on where the media is) */
    l.snd_at = (int64_t)ftyp.len + 8 + (m->moov_last ? 0 : (int64_t)mv.len);
    l.text_at = l.snd_at + (int64_t)snd_len;
    buf_free(&mv);
    moov(&mv, m, &l);
    buf_put(out, ftyp.p, ftyp.len);
    if (!m->moov_last) buf_put(out, mv.p, mv.len);
    p32(out, (uint32_t)(data.len + 8)), ps(out, "mdat"), buf_put(out, data.p, data.len);
    if (m->moov_last) buf_put(out, mv.p, mv.len);
    buf_free(&ftyp), buf_free(&mv), buf_free(&data);
    free(tsize);
}

/* ------------------------------------------------------------- the mp3 */

static const uint16_t CP1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                    0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

static uint32_t next_cp(const char **s) { /* (valid UTF-8 in) */
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t c = p[0];
    int k = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    if (k) c &= 0x3Fu >> k;
    for (int i = 1; i <= k; i++) c = c << 6 | (p[i] & 0x3Fu);
    *s += k + 1;
    return c;
}

/* A string in an ID3 encoding, its terminator after it when term */
static void id3_str(Buf *b, int enc, const char *s, bool term) {
    if (enc == 3) buf_put(b, s, strlen(s));
    if (enc == 1) p8(b, 0xFF), p8(b, 0xFE);
    while (enc != 3 && *s) {
        uint32_t c = next_cp(&s);
        if (!enc) {
            int byte = c < 0x100 && (c < 0x80 || c >= 0xA0) ? (int)c : '?';
            for (int i = 0; i < 32; i++)
                if (CP1252[i] && CP1252[i] == c) byte = 0x80 + i;
            p8(b, (uint32_t)byte);
            continue;
        }
        uint32_t u[2] = {c, 0};
        int n = 1;
        if (c >= 0x10000) u[0] = 0xD800 + ((c - 0x10000) >> 10), u[1] = 0xDC00 + ((c - 0x10000) & 0x3FF), n = 2;
        for (int i = 0; i < n; i++)
            if (enc == 1) p8(b, u[i]), p8(b, u[i] >> 8);
            else p16(b, u[i]);
    }
    if (term && (enc == 1 || enc == 2)) p16(b, 0);
    else if (term) p8(b, 0);
}

static int wenc(const AtTestMp3 *m) { return m->raw_utf8 && !m->enc ? 3 : m->enc; } /* the bytes' encoding */

static void unsync(Buf *out, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p8(out, p[i]);
        if (p[i] == 0xFF && (i + 1 == n || !p[i + 1] || p[i + 1] >= 0xE0)) p8(out, 0);
    }
}

static void safe32(Buf *b, uint32_t v) { p8(b, v >> 21 & 0x7F), p8(b, v >> 14 & 0x7F), p8(b, v >> 7 & 0x7F), p8(b, v & 0x7F); }

/* A frame: v2.2's three-letter ids, v2.3's plain sizes, v2.4's synchsafe ones (unsynchronised each when asked) */
static void frame(Buf *b, const AtTestMp3 *m, bool sub, const char *id, const Buf *d) {
    static const char *const V22[][2] = {{"TIT2", "TT2"}, {"TPE1", "TP1"}, {"TPE2", "TP2"}, {"TALB", "TAL"},
                                         {"TYER", "TYE"}, {"TRCK", "TRK"}, {"TPOS", "TPA"}, {"TCON", "TCO"},
                                         {"COMM", "COM"}, {"APIC", "PIC"}, {"TXXX", "TXX"}};
    if (m->id3 == 2) {
        for (size_t i = 0; i < sizeof V22 / sizeof V22[0]; i++)
            if (!strcmp(id, V22[i][0])) ps(b, V22[i][1]);
        p8(b, (uint32_t)d->len >> 16), p16(b, (uint32_t)d->len);
        buf_put(b, d->p, d->len);
        return;
    }
    Buf x = {0};
    bool un = m->id3 == 4 && m->unsync && !sub;
    if (un) {
        Buf y = {0};
        safe32(&y, (uint32_t)d->len); /* the data length */
        buf_put(&y, d->p, d->len);
        unsync(&x, (const uint8_t *)y.p, y.len);
        buf_free(&y);
    } else buf_put(&x, d->p, d->len);
    ps(b, id);
    if (m->id3 == 4 && !m->plain_sizes) safe32(b, (uint32_t)x.len);
    else p32(b, (uint32_t)x.len);
    p8(b, 0), p8(b, un ? 0x03 : 0);
    buf_put(b, x.p, x.len);
    buf_free(&x);
}

static void text_frame(Buf *b, const AtTestMp3 *m, const char *id, const char *s) {
    if (!s) return;
    Buf d = {0};
    p8(&d, (uint32_t)m->enc), id3_str(&d, wenc(m), s, false);
    frame(b, m, false, id, &d);
    buf_free(&d);
}

static void picture(Buf *b, const AtTestMp3 *m, int type, const uint8_t *p, size_t n) {
    Buf d = {0};
    p8(&d, (uint32_t)m->enc);
    if (m->id3 == 2) ps(&d, p[0] == 0xFF ? "JPG" : "PNG");
    else ps(&d, p[0] == 0xFF ? "image/jpeg" : "image/png"), p8(&d, 0);
    p8(&d, (uint32_t)type), id3_str(&d, wenc(m), type == 3 ? "Omslag" : "Back", true);
    buf_put(&d, p, n);
    frame(b, m, false, "APIC", &d);
    buf_free(&d);
}

static int mpa_len(const AtTestMp3 *m, int *rest) { /* 144 kbps / rate a frame, padded to keep the average */
    int kbps = m->kbps ? m->kbps : 128, rate = m->rate ? m->rate : 44100;
    *rest += 144000 * kbps % rate;
    bool pad = *rest >= rate;
    if (pad) *rest -= rate;
    return 144000 * kbps / rate + pad;
}

void at_test_mp3(Buf *out, const AtTestMp3 *m) {
    int kbps = m->kbps ? m->kbps : 128, rate = m->rate ? m->rate : 44100;
    static const int KBPS[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    int bi = 9, ri = rate == 48000 ? 1 : rate == 32000 ? 2 : 0, rest = 0;
    for (int i = 1; i < 15; i++)
        if (KBPS[i] == kbps) bi = i;
    int64_t length_ms = (int64_t)m->frames * 1152 * 1000 / rate;
    if (m->id3) {
        Buf f = {0}, d = {0};
        char s[64];
        if (m->ext_header && m->id3 == 3) p32(&f, 6), p16(&f, 0), p32(&f, 0);
        if (m->ext_header && m->id3 == 4) safe32(&f, 6), p8(&f, 1), p8(&f, 0);
        text_frame(&f, m, "TIT2", m->title);
        text_frame(&f, m, "TPE1", m->artist);
        text_frame(&f, m, "TPE2", m->album_artist);
        text_frame(&f, m, "TALB", m->album);
        text_frame(&f, m, "TCON", m->genre);
        if (m->year) {
            snprintf(s, sizeof s, m->id3 == 4 ? "%d-01-02" : "%d", m->year);
            text_frame(&f, m, m->id3 == 4 ? "TDRC" : "TYER", s);
        }
        for (int k = 0; k < 2; k++) {
            int a = k ? m->disc : m->track, of = k ? m->discs : m->tracks;
            if (!a) continue;
            if (of) snprintf(s, sizeof s, "%d/%d", a, of);
            else snprintf(s, sizeof s, "%d", a);
            text_frame(&f, m, k ? "TPOS" : "TRCK", s);
        }
        if (m->narrator) {
            p8(&d, (uint32_t)m->enc), id3_str(&d, wenc(m), "Narrator", true), id3_str(&d, wenc(m), m->narrator, false);
            frame(&f, m, false, "TXXX", &d);
            buf_reset(&d);
        }
        if (m->comment) { /* iTunes' own first: no comment */
            p8(&d, (uint32_t)m->enc), ps(&d, "eng"), id3_str(&d, wenc(m), "iTunNORM", true);
            id3_str(&d, wenc(m), " 0000031C 0000033B", false);
            frame(&f, m, false, "COMM", &d);
            buf_reset(&d);
            p8(&d, (uint32_t)m->enc), ps(&d, "eng"), id3_str(&d, wenc(m), "", true), id3_str(&d, wenc(m), m->comment, false);
            frame(&f, m, false, "COMM", &d);
            buf_reset(&d);
        }
        if (m->cover_len) {
            static const uint8_t back[4] = {0x89, 'P', 'N', 'G'};
            if (m->back_first) picture(&f, m, 4, back, 4);
            picture(&f, m, 3, m->cover, m->cover_len);
        }
        if (m->nchapters && m->id3 >= 3) {
            p8(&d, 't'), p8(&d, 0), p8(&d, 3), p8(&d, (uint32_t)m->nchapters); /* CTOC: top level, ordered */
            for (int i = 0; i < m->nchapters; i++) snprintf(s, sizeof s, "ch%d", i), ps(&d, s), p8(&d, 0);
            frame(&f, m, false, "CTOC", &d);
            buf_reset(&d);
        }
        for (int i = 0; m->id3 >= 3 && i < m->nchapters; i++) {
            Buf sub = {0}, t = {0};
            int64_t end = length_ms;
            for (int k = 0; k < m->nchapters; k++)
                if (m->chapters[k].start_ms > m->chapters[i].start_ms && m->chapters[k].start_ms < end)
                    end = m->chapters[k].start_ms;
            if (end < m->chapters[i].start_ms) end = m->chapters[i].start_ms; /* (past the sound's end) */
            snprintf(s, sizeof s, "ch%d", i), ps(&d, s), p8(&d, 0);
            p32(&d, (uint32_t)m->chapters[i].start_ms), p32(&d, (uint32_t)end), p32(&d, 0xFFFFFFFFu), p32(&d, 0xFFFFFFFFu);
            p8(&t, (uint32_t)m->enc), id3_str(&t, wenc(m), m->chapters[i].title, false);
            frame(&sub, m, true, "TIT2", &t);
            buf_put(&d, sub.p, sub.len);
            frame(&f, m, false, "CHAP", &d);
            buf_reset(&d), buf_free(&sub), buf_free(&t);
        }
        pz(&f, (size_t)m->padding);
        Buf body = {0};
        if (m->unsync && m->id3 < 4) unsync(&body, (const uint8_t *)f.p, f.len);
        else buf_put(&body, f.p, f.len);
        ps(out, "ID3"), p8(out, (uint32_t)m->id3), p8(out, 0);
        p8(out, (m->unsync ? 0x80u : 0) | (m->ext_header && m->id3 > 2 ? 0x40u : 0));
        safe32(out, (uint32_t)body.len);
        buf_put(out, body.p, body.len);
        buf_free(&f), buf_free(&d), buf_free(&body);
    }
    for (int i = 0; i < m->frames; i++) {
        int len = mpa_len(m, &rest);
        size_t at = out->len;
        p8(out, 0xFF), p8(out, 0xFB), p8(out, (uint32_t)(bi << 4 | ri << 2 | (len > 144000 * kbps / rate) << 1));
        p8(out, m->mono ? 0xC0 : 0x40);
        pz(out, (size_t)len - 4);
        uint32_t said = m->vbr_frames ? m->vbr_frames : (uint32_t)m->frames - 1;
        uint8_t *f = (uint8_t *)out->p + at;
        if (!i && (m->vbr == 1 || m->vbr == 2)) {
            size_t x = 4 + (m->mono ? 17 : 32);
            memcpy(f + x, m->vbr == 1 ? "Xing" : "Info", 4);
            f[x + 7] = m->lame_delay || m->lame_padding ? 11 : 3; /* frames and bytes (and a quality, then LAME's tag) */
            for (int k = 0; k < 4; k++) f[x + 8 + k] = (uint8_t)(said >> (24 - 8 * k));
            if (m->lame_delay || m->lame_padding) {
                uint8_t *l = f + x + 20;
                memcpy(l, "LAME3.100", 9);
                l[21] = (uint8_t)(m->lame_delay >> 4); /* 12 bits each */
                l[22] = (uint8_t)((m->lame_delay & 15) << 4 | m->lame_padding >> 8);
                l[23] = (uint8_t)m->lame_padding;
            }
        } else if (!i && m->vbr == 3) {
            memcpy(f + 36, "VBRI", 4);
            f[41] = 1; /* version 1 */
            for (int k = 0; k < 4; k++) f[50 + k] = (uint8_t)(said >> (24 - 8 * k));
        }
    }
    if (m->id3v1) {
        Buf v = {0};
        ps(&v, "TAG");
        const char *fields[3] = {m->title, m->artist, m->album};
        for (int i = 0; i < 3; i++) {
            Buf s = {0};
            if (fields[i]) id3_str(&s, m->raw_utf8 ? 3 : 0, fields[i], false);
            buf_put(&v, s.p, s.len < 30 ? s.len : 30), pz(&v, s.len < 30 ? 30 - s.len : 0);
            buf_free(&s);
        }
        char y[8];
        snprintf(y, sizeof y, "%04d", m->year % 10000);
        buf_put(&v, y, 4);
        Buf s = {0};
        if (m->comment) id3_str(&s, m->raw_utf8 ? 3 : 0, m->comment, false);
        buf_put(&v, s.p, s.len < 28 ? s.len : 28), pz(&v, s.len < 28 ? 28 - s.len : 0);
        p8(&v, 0), p8(&v, (uint32_t)m->track), p8(&v, 183);
        buf_put(out, v.p, v.len);
        buf_free(&v), buf_free(&s);
    }
}

/* ------------------------------------------------------------- reading */

typedef struct {
    const uint8_t *p;
    size_t n;
    int64_t reads, bytes;
} Mem;

static bool mem_read(void *ctx, int64_t off, void *dst, size_t n) {
    Mem *m = ctx;
    if (off < 0 || (uint64_t)off > m->n || n > m->n - (size_t)off) return false;
    memcpy(dst, m->p + off, n);
    m->reads++, m->bytes += (int64_t)n;
    return true;
}

static bool tags_of(const void *p, size_t n, const char *hint, AudioTags *t) {
    Mem m = {p, n, 0, 0};
    AtReader r = {&m, (int64_t)n, mem_read};
    return audio_tags_read(&r, hint, t);
}

static char *find_in(char *p, size_t n, const char *s, size_t k) {
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(p + i, s, k)) return p + i;
    return NULL;
}

/* Valid UTF-8, no control characters (a description's line breaks aside), nothing at either end, NUL inside */
static bool clean(const char *s, size_t cap, bool lines) {
    size_t n = 0;
    while (n < cap && s[n]) n++;
    if (n == cap) return false;
    if (n && (s[0] == ' ' || s[0] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\n')) return false;
    for (size_t i = 0; i < n;) {
        uint8_t b = (uint8_t)s[i];
        if ((b < 0x20 && !(lines && b == '\n')) || b == 0x7F) return false;
        size_t k = b < 0x80 ? 0 : b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 : b >= 0xF0 && b <= 0xF4 ? 3 : 9;
        if (k == 9 || n - i <= k) return false;
        uint32_t cp = k ? b & (0x3Fu >> k) : b;
        for (size_t j = 1; j <= k; j++) {
            uint8_t x = (uint8_t)s[i + j];
            if ((x & 0xC0) != 0x80) return false;
            cp = cp << 6 | (x & 0x3Fu);
        }
        if ((k == 2 && cp < 0x800) || (k == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000) ||
            (cp >= 0x80 && cp < 0xA0))
            return false;
        i += k + 1;
    }
    return true;
}

/* audiotags.h's promises, kept for any file */
static bool sane(const AudioTags *t, const uint8_t *file, int64_t size, bool ok) {
    if (ok != (t->format[0] != 0) || !clean(t->format, sizeof t->format, false) || !clean(t->codec, sizeof t->codec, false) ||
        !clean(t->title, AT_TEXT, false) || !clean(t->artist, AT_TEXT, false) || !clean(t->album_artist, AT_TEXT, false) ||
        !clean(t->album, AT_TEXT, false) || !clean(t->narrator, AT_TEXT, false) || !clean(t->genre, sizeof t->genre, false) ||
        !clean(t->cover_type, sizeof t->cover_type, false))
        return false;
    if (t->description && (!t->description[0] || !clean(t->description, AT_DESC_MAX, true))) return false;
    if (t->channels < 0 || t->sample_rate < 0 || t->duration_ms < 0 || t->year < 0 || t->year > 9999 || t->track < 0 ||
        t->tracks < 0 || t->disc < 0 || t->discs < 0)
        return false;
    if (t->nchapters < 0 || t->nchapters > AT_MAX_CHAPTERS || !t->nchapters != !t->chapters) return false;
    for (int i = 0; i < t->nchapters; i++)
        if (t->chapters[i].start_ms < 0 || (i && t->chapters[i].start_ms < t->chapters[i - 1].start_ms) ||
            !clean(t->chapters[i].title, AT_CHAPTER_TEXT, false))
            return false;
    if (!t->cover_len) return !t->cover_type[0] && !t->cover_off;
    if (t->cover_len < 0 || t->cover_off < 0 || t->cover_off > size - t->cover_len) return false;
    if (!file) return !strcmp(t->cover_type, "image/jpeg") || !strcmp(t->cover_type, "image/png");
    const uint8_t *c = file + t->cover_off;
    if (!strcmp(t->cover_type, "image/jpeg")) return t->cover_len >= 2 && c[0] == 0xFF && c[1] == 0xD8;
    return !strcmp(t->cover_type, "image/png") && t->cover_len >= 4 && c[0] == 0x89 && c[1] == 'P';
}

/* every way of cutting a file short (each 7th length): never a crash, always sane */
static bool cuts_sane(const Buf *b, const char *hint) {
    for (size_t n = 0; n < b->len; n += 7) {
        AudioTags t;
        bool ok = tags_of(b->p, n, hint, &t);
        bool fine = sane(&t, (const uint8_t *)b->p, (int64_t)n, ok);
        audio_tags_free(&t);
        if (!fine) {
            printf("  not sane when cut at %zu of %zu bytes\n", n, b->len);
            return false;
        }
    }
    return true;
}

/* test pictures: a JPEG's first bytes (FF D8 FF E0: unsynchronisation changes those) or a PNG's (no FF at all) */
static uint8_t *picture_bytes(size_t n, bool png) {
    uint8_t *p = xmalloc(n);
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 37 + 11) & 0x7F);
    if (png) memcpy(p, "\x89PNG\r\n\x1a\n", 8);
    else p[0] = 0xFF, p[1] = 0xD8, p[2] = 0xFF, p[3] = 0xE0, p[n - 2] = 0xFF, p[n - 1] = 0xD9;
    return p;
}

static const AtTestChapter CH4[] = {
    {0, "Opening Credits"},
    {63530, "Chapter 1 - The Boy Who Lived"},
    {1970530, "Chapter 2 - The Vanishing Glass"},
    {2400000, "End Credits"}};

/* ------------------------------------------------------------- MP4 */

TEST(audiotags_m4b_reads_every_tag) {
    uint8_t *cover = picture_bytes(700, false);
    AtTestM4b m = {.title = "Harry Potter and the Philosopher\xE2\x80\x99s Stone",
                   .artist = "J.K. Rowling",
                   .album_artist = "J.K. Rowling",
                   .album = "Harry Potter (Unabridged)",
                   .narrator = "Full Cast",
                   .genre = "Fantas\xC3\xAD"
                            "a y magia",
                   .cmt = "a comment",
                   .des = "the \302\251des",
                   .desc = "short",
                   .ldes = "The long one.\nWith two lines.",
                   .year = 2025, .track = 1, .tracks = 7, .disc = 2, .discs = 3,
                   .cover = cover, .cover_len = 700,
                   .chapters = CH4, .nchapters = 4,
                   .duration_ms = 2500000, .fake_frames = 100};
    Buf b = {0};
    at_test_m4b(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.format, "mp4");
    CHECK_STR(t.codec, "aac");
    CHECK_INT(t.channels, 2);
    CHECK_INT(t.sample_rate, 44100);
    CHECK_INT(t.duration_ms, 2500000);
    CHECK_STR(t.title, m.title);
    CHECK_STR(t.artist, "J.K. Rowling");
    CHECK_STR(t.album_artist, "J.K. Rowling");
    CHECK_STR(t.album, "Harry Potter (Unabridged)");
    CHECK_STR(t.narrator, "Full Cast");
    CHECK_STR(t.genre, m.genre);
    CHECK_INT(t.year, 2025);
    CHECK_INT(t.track, 1); CHECK_INT(t.tracks, 7); CHECK_INT(t.disc, 2); CHECK_INT(t.discs, 3);
    CHECK_STR(t.description, "The long one.\nWith two lines."); /* ldes before desc, ©des and ©cmt */
    CHECK_STR(t.cover_type, "image/jpeg");
    CHECK_INT(t.cover_len, 700);
    CHECK(!memcmp(b.p + t.cover_off, cover, 700));
    CHECK_INT(t.nchapters, 4);
    for (int i = 0; i < 4; i++) {
        CHECK_INT(t.chapters[i].start_ms, CH4[i].start_ms);
        CHECK_STR(t.chapters[i].title, CH4[i].title);
    }
    audio_tags_free(&t);
    CHECK(!t.chapters && !t.description);
    CHECK(cuts_sane(&b, NULL));
    buf_free(&b);
    free(cover);
}

TEST(audiotags_m4b_moov_last_nero_chapters) {
    static const AtTestChapter ch[] = {{1000, "Two"}, {0, "One"}, {9000, "Three \xF0\x9F\x98\x80"}}; /* (out of order) */
    uint8_t *cover = picture_bytes(300, true);
    AtTestM4b m = {.title = "Book", .des = "From \302\251des", .cmt = "From \302\251cmt", .gnre = 184, .meta_qt = true,
                   .moov_last = true, .chapters = ch, .nchapters = 3, .chpl = true, .cover = cover, .cover_len = 300,
                   .sample_rate = 22050, .channels = 1, .duration_ms = 12000};
    Buf b = {0};
    at_test_m4b(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, "x.m4b", &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.title, "Book");
    CHECK_STR(t.description, "From \302\251des");
    CHECK_STR(t.genre, "Audiobook"); /* gnre 184 = ID3v1's 183 */
    CHECK_INT(t.sample_rate, 22050);
    CHECK_INT(t.channels, 1);
    CHECK_INT(t.duration_ms, 12000);
    CHECK_STR(t.cover_type, "image/png");
    CHECK(!memcmp(b.p + t.cover_off, cover, 300));
    CHECK_INT(t.nchapters, 3);
    CHECK_STR(t.chapters[0].title, "One"); CHECK_INT(t.chapters[0].start_ms, 0);
    CHECK_STR(t.chapters[1].title, "Two"); CHECK_INT(t.chapters[1].start_ms, 1000);
    CHECK_STR(t.chapters[2].title, "Three \xF0\x9F\x98\x80"); CHECK_INT(t.chapters[2].start_ms, 9000);
    audio_tags_free(&t);
    CHECK(cuts_sane(&b, "x.m4b"));
    buf_free(&b);
    free(cover);
}

TEST(audiotags_m4b_prefers_the_chapter_track_and_its_edit) {
    static const AtTestChapter ch[] = {{5000, "Late start"}, {7000, "Next"}};
    AtTestM4b m = {.title = "Both", .chapters = ch, .nchapters = 2, .qt = true, .chpl = true, .duration_ms = 10000};
    Buf b = {0};
    at_test_m4b(&b, &m);
    /* the chpl says the same times: tell them apart by changing its first title's bytes */
    char *p = find_in(b.p, b.len, "\012Late start", 11);
    CHECK(p);
    memcpy(p + 1, "Nero!", 5);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK_INT(t.nchapters, 2);
    CHECK_STR(t.chapters[0].title, "Late start"); /* the track's, not Nero's */
    CHECK_INT(t.chapters[0].start_ms, 5000);      /* through its empty edit */
    CHECK_INT(t.chapters[1].start_ms, 7000);
    audio_tags_free(&t);
    buf_free(&b);
}

/* What a browser plays: real AAC from aac.c, its AudioSpecificConfig, priming cut by an edit */
TEST(audiotags_m4b_with_real_aac) {
    AacConfig ac = {44100, 2, 64000, 0, 0};
    AacEnc *e = aac_open(&ac, NULL, 0);
    CHECK(e);
    uint8_t asc[2], au[AAC_MAX_AU];
    aac_specific_config(e, asc);
    static float pcm[AAC_FRAME * 2];
    Buf aus = {0};
    int lens[200], n = 0, blocks = 2 * 44100 / AAC_FRAME + 1;
    for (int k = 0; k <= blocks + 2 && n < 200; k++) {
        for (int i = 0; i < AAC_FRAME; i++) pcm[2 * i] = pcm[2 * i + 1] = 0.2f * sinf((float)(k * AAC_FRAME + i) * 0.0627f);
        int got = k < blocks ? aac_encode(e, pcm, au, AAC_MAX_AU) : aac_flush(e, au, AAC_MAX_AU);
        if (got > 0) buf_put(&aus, au, (size_t)got), lens[n++] = got;
        if (k >= blocks && got == 0) break;
    }
    aac_close(e);
    CHECK_INT(n, blocks + 1);
    AtTestM4b m = {.title = "Tone", .chapters = CH4, .nchapters = 2, .aac = (const uint8_t *)aus.p, .aac_len = lens,
                   .aac_frames = n, .asc = asc, .asc_len = 2, .priming = AAC_PRIMING};
    Buf b = {0};
    at_test_m4b(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK_STR(t.codec, "aac");
    CHECK_INT(t.channels, 2);
    CHECK_INT(t.sample_rate, 44100);
    CHECK_INT(t.duration_ms, (int64_t)(n - 1) * AAC_FRAME * 1000 / 44100);
    CHECK_INT(t.nchapters, 2);
    /* the access units sit in the mdat, back to back, as stco says */
    const char *md = find_in(b.p, b.len, "mdat", 4);
    CHECK(md && !memcmp(md + 4, aus.p, aus.len));
    const char *out = getenv("HEMMA_TEST_TAGS_OUT"); /* (to hold it against a player or ffprobe) */
    if (out && *out) CHECK(file_write_atomic(out, b.p, b.len));
    audio_tags_free(&t);
    buf_free(&b), buf_free(&aus);
}

/* An audiobook's moov is nearly all its sample tables: they're never read */
TEST(audiotags_m4b_reads_little) {
    AtTestM4b m = {.title = "Big", .chapters = CH4, .nchapters = 4, .duration_ms = 36000000, .fake_frames = 200000,
                   .moov_last = true};
    Buf b = {0};
    at_test_m4b(&b, &m);
    CHECK(b.len > 2000000);
    Mem mem = {(const uint8_t *)b.p, b.len, 0, 0};
    AtReader r = {&mem, (int64_t)b.len, mem_read};
    AudioTags t;
    CHECK(audio_tags_read(&r, NULL, &t));
    CHECK_INT(t.nchapters, 4);
    CHECK_STR(t.title, "Big");
    CHECK(mem.bytes < 64 * 1024);
    CHECK(mem.reads < 300);
    audio_tags_free(&t);
    buf_free(&b);
}

TEST(audiotags_text_is_cleaned_and_cut) {
    char long_title[700] = "", long_desc[6100] = "";
    for (int i = 0; i < 300; i++) strcat(long_title, "\xC3\xA9"); /* é: 600 bytes */
    for (int i = 0; i < 1500; i++) strcat(long_desc, "ab\xC3\xA9");  /* 6000 bytes */
    AtTestM4b m = {.title = " \tA\r\n  B\001C\177 ", .artist = "bad \xC3\x28 and \xED\xA0\x80 and \xF0\x9F",
                   .album = long_title, .ldes = "  Line 1\r\n\r\n\r\nLine 2\tx  \n", .desc = long_desc};
    Buf b = {0};
    at_test_m4b(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.title, "A BC");
    CHECK_STR(t.artist, "bad \xEF\xBF\xBD( and \xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD and \xEF\xBF\xBD");
    CHECK_INT(strlen(t.album), 254); /* 127 whole é in 255 bytes */
    CHECK_STR(t.description, "Line 1\n\nLine 2 x");
    audio_tags_free(&t);
    buf_free(&b);
    m = (AtTestM4b){.desc = long_desc};
    at_test_m4b(&b, &m);
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK(strlen(t.description) >= AT_DESC_MAX - 4 && strlen(t.description) < AT_DESC_MAX);
    audio_tags_free(&t);
    buf_free(&b);
}

/* ------------------------------------------------------------- MP3 */

static AtTestChapter shuffled[3] = {{2000, "Kapitel 2"}, {0, "Kapitel 1"}, {4000, "Kapitel 3 \xC3\xA5\xC3\xA4\xC3\xB6"}};

static void check_chapters(const AudioTags *t, int *bad) {
    *bad = t->nchapters != 3 || strcmp(t->chapters[0].title, "Kapitel 1") || t->chapters[0].start_ms ||
           strcmp(t->chapters[1].title, "Kapitel 2") || t->chapters[1].start_ms != 2000 ||
           strcmp(t->chapters[2].title, "Kapitel 3 \xC3\xA5\xC3\xA4\xC3\xB6") || t->chapters[2].start_ms != 4000;
}

static int64_t id3_size(const Buf *b) {
    const uint8_t *p = (const uint8_t *)b->p;
    return 10 + ((int64_t)p[6] << 21 | (int64_t)p[7] << 14 | (int64_t)p[8] << 7 | p[9]);
}

TEST(audiotags_mp3_v23_latin1_xing) {
    uint8_t *cover = picture_bytes(500, false);
    AtTestMp3 m = {.id3 = 3, .enc = 0, .ext_header = true, .title = "Caf\xC3\xA9 \xE2\x80\x99s", .artist = "Astrid",
                   .album_artist = "Astrid L", .album = "Sagor", .genre = "(101)", .comment = "Ett \xE2\x82\xAC.",
                   .narrator = "Nils", .year = 1945, .track = 3, .tracks = 12, .disc = 1,
                   .cover = cover, .cover_len = 500, .back_first = true, .chapters = shuffled, .nchapters = 3,
                   .padding = 300, .frames = 6, .vbr = 1, .vbr_frames = 100000};
    Buf b = {0};
    at_test_mp3(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.format, "mp3");
    CHECK_STR(t.codec, "mp3");
    CHECK_INT(t.channels, 2); CHECK_INT(t.sample_rate, 44100);
    CHECK_INT(t.duration_ms, (int64_t)100000 * 1152 * 1000 / 44100); /* the Xing header's count */
    CHECK_STR(t.title, "Caf\xC3\xA9 \xE2\x80\x99s"); /* (’ by Windows-1252) */
    CHECK_STR(t.artist, "Astrid"); CHECK_STR(t.album_artist, "Astrid L"); CHECK_STR(t.album, "Sagor");
    CHECK_STR(t.genre, "Speech");
    CHECK_STR(t.description, "Ett \xE2\x82\xAC.");
    CHECK_STR(t.narrator, "Nils");
    CHECK_INT(t.year, 1945); CHECK_INT(t.track, 3); CHECK_INT(t.tracks, 12); CHECK_INT(t.disc, 1); CHECK_INT(t.discs, 0);
    CHECK_STR(t.cover_type, "image/jpeg"); /* the front cover, not the back one before it */
    CHECK_INT(t.cover_len, 500);
    CHECK(!memcmp(b.p + t.cover_off, cover, 500));
    int bad;
    check_chapters(&t, &bad);
    CHECK(!bad);
    audio_tags_free(&t);
    CHECK(cuts_sane(&b, NULL));
    buf_free(&b);
    free(cover);
}

TEST(audiotags_mp3_unsynchronised_vbri) {
    uint8_t *jpeg = picture_bytes(400, false), *png = picture_bytes(300, true);
    AtTestMp3 m = {.id3 = 4, .enc = 1, .unsync = true, .ext_header = true, .title = "\xC3\xBF\xC3\xBF \xF0\x9F\x98\x80",
                   .artist = "A", .genre = "Audiobook", .year = 2001, .track = 2, .cover = png, .cover_len = 300,
                   .chapters = shuffled, .nchapters = 3, .frames = 4, .vbr = 3, .vbr_frames = 5000};
    Buf b = {0};
    at_test_mp3(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.title, "\xC3\xBF\xC3\xBF \xF0\x9F\x98\x80");
    CHECK_STR(t.genre, "Audiobook");
    CHECK_INT(t.year, 2001); CHECK_INT(t.track, 2);
    CHECK_INT(t.duration_ms, (int64_t)5000 * 1152 * 1000 / 44100); /* VBRI */
    CHECK_STR(t.cover_type, "image/png"); /* no FF in it: unsynchronisation left it as it was */
    CHECK(!memcmp(b.p + t.cover_off, png, 300));
    int bad;
    check_chapters(&t, &bad);
    CHECK(!bad);
    audio_tags_free(&t);
    CHECK(cuts_sane(&b, "x.mp3"));
    buf_free(&b);
    /* a JPEG's FF E0 gets a 0 after it: its bytes aren't in the file as they are, so no place for it */
    m.cover = jpeg, m.cover_len = 400;
    at_test_mp3(&b, &m);
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK_STR(t.title, "\xC3\xBF\xC3\xBF \xF0\x9F\x98\x80");
    CHECK_INT(t.cover_len, 0);
    check_chapters(&t, &bad);
    CHECK(!bad);
    audio_tags_free(&t);
    buf_free(&b);
    /* v2.3: the whole tag unsynchronised */
    m.id3 = 3, m.enc = 2, m.cover = png, m.cover_len = 300;
    at_test_mp3(&b, &m);
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.title, "\xC3\xBF\xC3\xBF \xF0\x9F\x98\x80");
    CHECK_INT(t.track, 2);
    CHECK_STR(t.cover_type, "image/png");
    CHECK(!memcmp(b.p + t.cover_off, png, 300));
    check_chapters(&t, &bad);
    CHECK(!bad);
    audio_tags_free(&t);
    CHECK(cuts_sane(&b, NULL));
    buf_free(&b);
    free(jpeg), free(png);
}

TEST(audiotags_mp3_v24_text_plain_sizes_cbr) {
    char comment[400] = "";
    for (int i = 0; i < 30; i++) strcat(comment, "Ord och ord ");
    for (int enc = 1; enc <= 3; enc++) {
        AtTestMp3 m = {.id3 = 4, .enc = enc, .plain_sizes = true, .title = "\xF0\x9F\x93\x96 Bok", .artist = "Selma",
                       .comment = comment, .narrator = "Her", .disc = 2, .discs = 2, .frames = 9, .kbps = 64, .mono = true};
        Buf b = {0};
        at_test_mp3(&b, &m);
        AudioTags t;
        CHECK(tags_of(b.p, b.len, NULL, &t));
        CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
        CHECK_STR(t.title, "\xF0\x9F\x93\x96 Bok");
        CHECK_STR(t.artist, "Selma");
        CHECK_STR(t.narrator, "Her");
        CHECK_INT(t.disc, 2); CHECK_INT(t.discs, 2);
        CHECK(t.description && !strncmp(t.description, "Ord och ord Ord", 15));
        CHECK_INT(t.channels, 1);
        CHECK_INT(t.duration_ms, ((int64_t)b.len - id3_size(&b)) * 8 / 64); /* constant bitrate: bytes over it */
        audio_tags_free(&t);
        buf_free(&b);
    }
}

TEST(audiotags_mp3_v22_and_genres) {
    uint8_t *cover = picture_bytes(200, false);
    AtTestMp3 m = {.id3 = 2, .enc = 1, .title = "Tv\xC3\xA5", .album = "Album", .genre = "(183)Ljudbok", .year = 1999,
                   .track = 7, .comment = "Hej", .cover = cover, .cover_len = 200, .frames = 5};
    Buf b = {0};
    at_test_mp3(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.title, "Tv\xC3\xA5");
    CHECK_STR(t.genre, "Ljudbok");
    CHECK_INT(t.year, 1999); CHECK_INT(t.track, 7);
    CHECK_STR(t.description, "Hej");
    CHECK_STR(t.cover_type, "image/jpeg");
    CHECK(!memcmp(b.p + t.cover_off, cover, 200));
    CHECK_INT(t.nchapters, 0);
    audio_tags_free(&t);
    CHECK(cuts_sane(&b, NULL));
    buf_free(&b);
    static const char *const genres[][2] = {{"(183)", "Audiobook"}, {"183", "Audiobook"}, {"(12)(13)", "Other"},
                                            {"Prat", "Prat"},        {"(999)", ""},         {"(133)", ""},
                                            {"(17) Rock och mer", "Rock och mer"}, /* (the fuzzer: " Rock") */
                                            {"(17)   (18)", "Rock"}};
    for (size_t i = 0; i < sizeof genres / sizeof genres[0]; i++) {
        m = (AtTestMp3){.id3 = 3, .genre = genres[i][0], .frames = 3};
        at_test_mp3(&b, &m);
        CHECK(tags_of(b.p, b.len, NULL, &t));
        CHECK_STR(t.genre, genres[i][1]);
        audio_tags_free(&t);
        buf_free(&b);
    }
    free(cover);
}

/* LAME's tag after an Info header: its delay and padding aren't the book's (FFmpeg's files say 60.000 s that way);
   UTF-8 bytes where Latin-1 belongs (FFmpeg's ID3v1 writes them) read as UTF-8 */
TEST(audiotags_mp3_lame_tag_and_utf8_as_latin1) {
    AtTestMp3 m = {.id3 = 3, .enc = 0, .raw_utf8 = true, .title = "Bok \xC3\x85\xC3\x84\xC3\x96", .comment = "p\xC3\xA5 tv\xC3\xA5",
                   .frames = 10, .vbr = 2, .vbr_frames = 2298, .lame_delay = 576, .lame_padding = 720};
    Buf b = {0};
    at_test_mp3(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK_INT(t.duration_ms, 60000); /* (2298 x 1152 - 576 - 720 samples at 44.1 kHz) */
    CHECK_STR(t.title, "Bok \xC3\x85\xC3\x84\xC3\x96");
    CHECK_STR(t.description, "p\xC3\xA5 tv\xC3\xA5");
    audio_tags_free(&t);
    buf_free(&b);
    m = (AtTestMp3){.raw_utf8 = true, .title = "Bok \xC3\x85\xC3\x84\xC3\x96", .id3v1 = true, .frames = 5};
    at_test_mp3(&b, &m);
    CHECK(tags_of(b.p, b.len, NULL, &t));
    CHECK_STR(t.title, "Bok \xC3\x85\xC3\x84\xC3\x96");
    audio_tags_free(&t);
    buf_free(&b);
}

static void hand_frame(Buf *b, const char *id, int ver, uint32_t flags, const void *d, size_t n) {
    ps(b, id);
    if (ver == 4) safe32(b, (uint32_t)n);
    else p32(b, (uint32_t)n);
    p16(b, flags), buf_put(b, d, n);
}

/* v2.4's NUL-separated values are joined, v2.3's text ends at its NUL; a CHAP's own TIT2 unsynchronised by itself
   (FF 00 00: only the first 0 is the stuffing) */
TEST(audiotags_mp3_values_and_a_chapters_own_frame) {
    Buf audio = {0}, f = {0}, tag = {0};
    AtTestMp3 a = {.frames = 3};
    at_test_mp3(&audio, &a);
    for (int ver = 3; ver <= 4; ver++) {
        buf_reset(&f), buf_reset(&tag);
        hand_frame(&f, "TPE1", ver, 0, "\003A\0B", 4);
        hand_frame(&f, "TIT2", ver, 0, "\003Title\0junk", 11);
        if (ver == 4) {
            static const uint8_t ya[] = {1, 0xFF, 0xFE, 0xFF, 0x00, 0x00, 'A', 0}; /* "ÿA" in UTF-16LE, unsynchronised */
            Buf s = {0}, d = {0};
            hand_frame(&s, "TIT2", 4, 0x02, ya, sizeof ya);
            buf_put(&d, "c1", 3), p32(&d, 1000), p32(&d, 2000), p32(&d, 0xFFFFFFFFu), p32(&d, 0xFFFFFFFFu);
            buf_put(&d, s.p, s.len);
            hand_frame(&f, "CHAP", 4, 0, d.p, d.len);
            buf_free(&s), buf_free(&d);
        }
        ps(&tag, "ID3"), p8(&tag, (uint32_t)ver), p8(&tag, 0), p8(&tag, 0), safe32(&tag, (uint32_t)f.len);
        buf_put(&tag, f.p, f.len), buf_put(&tag, audio.p, audio.len);
        AudioTags t;
        CHECK(tags_of(tag.p, tag.len, NULL, &t));
        CHECK_STR(t.artist, ver == 4 ? "A, B" : "A");
        CHECK_STR(t.title, ver == 4 ? "Title, junk" : "Title");
        if (ver == 4) {
            CHECK_INT(t.nchapters, 1);
            CHECK_INT(t.chapters[0].start_ms, 1000);
            CHECK_STR(t.chapters[0].title, "\xC3\xBF" "A");
        }
        audio_tags_free(&t);
    }
    buf_free(&audio), buf_free(&f), buf_free(&tag);
}

TEST(audiotags_mp3_id3v1_only_and_none) {
    AtTestMp3 m = {.title = "Gammal \xE2\x80\x99titel", .artist = "Ingen", .album = "Skiva", .year = 1987,
                   .comment = "En kommentar", .track = 4, .id3v1 = true, .frames = 10, .kbps = 32, .rate = 32000};
    Buf b = {0};
    at_test_mp3(&b, &m);
    AudioTags t;
    CHECK(tags_of(b.p, b.len, NULL, &t)); /* frames from the first byte: an MP3 without its name */
    CHECK(sane(&t, (const uint8_t *)b.p, (int64_t)b.len, true));
    CHECK_STR(t.title, "Gammal \xE2\x80\x99titel");
    CHECK_STR(t.artist, "Ingen"); CHECK_STR(t.album, "Skiva");
    CHECK_INT(t.year, 1987); CHECK_INT(t.track, 4);
    CHECK_STR(t.description, "En kommentar");
    CHECK_STR(t.genre, "Audiobook");
    CHECK_INT(t.sample_rate, 32000);
    CHECK_INT(t.duration_ms, ((int64_t)b.len - 128) * 8 / 32);
    audio_tags_free(&t);
    CHECK(cuts_sane(&b, NULL));
    /* garbage first: found only for a file named .mp3 */
    Buf g = {0};
    for (int i = 0; i < 1000; i++) p8(&g, (uint32_t)(i * 7 + 3) & 0x7F);
    buf_put(&g, b.p, b.len);
    CHECK(!tags_of(g.p, g.len, NULL, &t));
    CHECK_STR(t.format, "");
    audio_tags_free(&t);
    CHECK(tags_of(g.p, g.len, "D:/Ljud/bok.MP3", &t));
    CHECK_STR(t.title, "Gammal \xE2\x80\x99titel");
    audio_tags_free(&t);
    /* no frames at all */
    CHECK(!tags_of(g.p, 1000, "bok.mp3", &t));
    audio_tags_free(&t);
    buf_free(&g);
    buf_free(&b);
    /* an ID3 tag with nothing after it: an MP3 only by its name */
    m = (AtTestMp3){.id3 = 3, .title = "Bara taggen"};
    at_test_mp3(&b, &m);
    CHECK(!tags_of(b.p, b.len, NULL, &t));
    audio_tags_free(&t);
    CHECK(tags_of(b.p, b.len, "a.mp3", &t));
    CHECK_STR(t.title, "Bara taggen");
    CHECK_STR(t.codec, "");
    audio_tags_free(&t);
    buf_free(&b);
}

/* ------------------------------------------------------------- damage */

static size_t find4(const Buf *b, const char *type, int nth) { /* the nth box of a type: its header's offset */
    for (size_t i = 4; i + 4 <= b->len; i++)
        if (!memcmp(b->p + i, type, 4) && !nth--) return i - 4;
    return 0;
}

static bool read_sane(const Buf *b, const char *hint, AudioTags *t) {
    bool ok = tags_of(b->p, b->len, hint, t);
    return sane(t, (const uint8_t *)b->p, (int64_t)b->len, ok);
}

TEST(audiotags_m4b_absurd_counts_and_sizes) {
    AtTestM4b m = {.title = "T", .chapters = CH4, .nchapters = 4, .chpl = true, .qt = true};
    Buf b = {0}, x = {0};
    at_test_m4b(&b, &m);
    AudioTags t;
    /* the chapter track's tables (the second of each) saying 2^31 and more */
    static const char *const tabs[] = {"stsz", "stts", "stsc", "stco"};
    for (int k = 0; k < 4; k++)
        for (int v = 0; v < 3; v++) {
            static const uint32_t counts[3] = {0x80000000u, 0xFFFFFFFFu, 0};
            buf_reset(&x), buf_put(&x, b.p, b.len);
            size_t at = find4(&x, tabs[k], 1);
            CHECK(at);
            set32(&x, at + 12 + (k == 0 ? 4 : 0), counts[v]);
            CHECK(read_sane(&x, NULL, &t));
            CHECK_STR(t.title, "T");
            CHECK(t.nchapters <= 4);
            audio_tags_free(&t);
        }
    /* every sample in a chunk of 2^32 - 1, sizes of 2^32 - 1, offsets past the end */
    buf_reset(&x), buf_put(&x, b.p, b.len);
    set32(&x, find4(&x, "stsc", 1) + 20, 0xFFFFFFFFu);
    set32(&x, find4(&x, "stsz", 1) + 20, 0xFFFFFFFFu);
    set32(&x, find4(&x, "stco", 1) + 16, 0xFFFFFFF0u);
    CHECK(read_sane(&x, NULL, &t));
    CHECK_INT(t.nchapters, 4);
    audio_tags_free(&t);
    /* the chapter track's timescale 1: hours become years, nothing overflows */
    buf_reset(&x), buf_put(&x, b.p, b.len);
    size_t mdhd = find4(&x, "mdhd", 1);
    set32(&x, mdhd + 20, 1);
    CHECK(read_sane(&x, NULL, &t));
    CHECK(t.nchapters == 4 && t.chapters[3].start_ms > 1000000000);
    audio_tags_free(&t);
    /* boxes claiming more than their parent or the file; a box of size 0 = to the end; sizes under 8 */
    static const char *const boxes[] = {"moov", "trak", "mdia", "udta", "meta", "ilst",
                                        "\251nam", "data", "stsd", "esds", "chpl", "tref"};
    static const uint32_t sizes[] = {0x7FFFFFFFu, 0, 3, 9, 1};
    for (size_t k = 0; k < sizeof boxes / sizeof boxes[0]; k++)
        for (size_t v = 0; v < sizeof sizes / sizeof sizes[0]; v++) {
            buf_reset(&x), buf_put(&x, b.p, b.len);
            size_t at = find4(&x, boxes[k], 0);
            CHECK(at || k == 0);
            set32(&x, at, sizes[v]);
            CHECK(read_sane(&x, NULL, &t));
            audio_tags_free(&t);
        }
    /* a 64-bit size: right, and absurd */
    for (int v = 0; v < 2; v++) {
        buf_reset(&x);
        size_t ftyp = find4(&b, "moov", 0);
        buf_put(&x, b.p, ftyp);
        p32(&x, 1), ps(&x, "free"), p64(&x, v ? 0x7FFFFFFFFFFFFFF0ull : 24), p64(&x, 0);
        buf_put(&x, b.p + ftyp, b.len - ftyp);
        CHECK(read_sane(&x, NULL, &t));
        CHECK_STR(t.title, v ? "" : "T");
        audio_tags_free(&t);
    }
    /* deep: boxes in boxes in boxes, a meta in a meta */
    buf_reset(&x);
    p32(&x, 16), ps(&x, "ftypM4A "), p32(&x, 0);
    size_t open[2000];
    open[0] = bx(&x, "moov");
    for (int i = 1; i < 2000; i++) open[i] = bx(&x, i % 2 ? "udta" : "meta");
    for (int i = 1999; i >= 0; i--) bend(&x, open[i]);
    CHECK(read_sane(&x, NULL, &t));
    CHECK_STR(t.format, "mp4");
    audio_tags_free(&t);
    /* Nero's count says 255; four are there */
    buf_reset(&x), buf_put(&x, b.p, b.len);
    size_t chpl = find4(&x, "chpl", 0);
    x.p[chpl + 16] = (char)0xFF;
    char *q = find_in(x.p, x.len, "tref", 4);
    CHECK(q);
    memcpy(q, "fere", 4); /* (no chapter track: Nero's are read) */
    CHECK(read_sane(&x, NULL, &t));
    CHECK_INT(t.nchapters, 4);
    audio_tags_free(&t);
    buf_free(&b), buf_free(&x);
}

TEST(audiotags_mp3_absurd_sizes) {
    AtTestMp3 m = {.id3 = 3, .title = "T", .comment = "C", .chapters = shuffled, .nchapters = 3, .frames = 4, .vbr = 1};
    Buf b = {0}, x = {0};
    at_test_mp3(&b, &m);
    AudioTags t;
    /* frames claiming 2^32 - 1 bytes, the tag claiming 256 MB */
    static const char *const ids[] = {"TIT2", "COMM", "CHAP", "CTOC", "TPE1"};
    for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++) {
        buf_reset(&x), buf_put(&x, b.p, b.len);
        char *p = find_in(x.p, x.len, ids[k], 4);
        if (!p) continue;
        set32(&x, (size_t)(p - x.p) + 4, 0xFFFFFFFFu);
        CHECK(read_sane(&x, NULL, &t));
        audio_tags_free(&t);
    }
    buf_reset(&x), buf_put(&x, b.p, b.len);
    memset(x.p + 6, 0x7F, 4);
    CHECK(read_sane(&x, NULL, &t));
    CHECK_STR(t.title, "T");
    audio_tags_free(&t);
    /* a CHAP whose own frame claims too much; one without a TIT2 */
    buf_reset(&x), buf_put(&x, b.p, b.len);
    char *p = find_in(x.p, x.len, "CHAP", 4);
    char *s = find_in(p, x.len - (size_t)(p - x.p), "TIT2", 4);
    set32(&x, (size_t)(s - x.p) + 4, 0x7FFFFFFFu);
    CHECK(read_sane(&x, NULL, &t));
    CHECK_INT(t.nchapters, 3);
    audio_tags_free(&t);
    memcpy(s, "TIT3", 4);
    CHECK(read_sane(&x, NULL, &t));
    CHECK_INT(t.nchapters, 3);
    audio_tags_free(&t);
    /* more chapters than are kept */
    AtTestChapter *many = xmalloc(sizeof *many * 2100);
    char (*names)[16] = xmalloc(16 * 2100);
    for (int i = 0; i < 2100; i++) {
        snprintf(names[i], 16, "N%d", 2099 - i);
        many[i] = (AtTestChapter){(2099 - i) * 1000, names[i]};
    }
    m = (AtTestMp3){.id3 = 4, .chapters = many, .nchapters = 2100, .frames = 3};
    buf_free(&b);
    at_test_mp3(&b, &m);
    CHECK(read_sane(&b, NULL, &t));
    CHECK_INT(t.nchapters, AT_MAX_CHAPTERS);
    CHECK_STR(t.chapters[0].title, "N100"); /* the first 2000 in the file, then in order */
    audio_tags_free(&t);
    AtTestM4b mm = {.chapters = NULL, .nchapters = 2100, .duration_ms = 2200000};
    for (int i = 0; i < 2100; i++) many[i].start_ms = i * 1000, snprintf(names[i], 16, "N%d", i);
    mm.chapters = many;
    buf_free(&b);
    at_test_m4b(&b, &mm);
    CHECK(read_sane(&b, NULL, &t));
    CHECK_INT(t.nchapters, AT_MAX_CHAPTERS);
    CHECK_STR(t.chapters[1999].title, "N1999");
    CHECK_INT(t.chapters[1999].start_ms, 1999000);
    audio_tags_free(&t);
    free(many), free(names);
    buf_free(&b), buf_free(&x);
}

TEST(audiotags_not_audio) {
    AudioTags t;
    uint8_t junk[3000];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 131 + 7);
    for (size_t n = 0; n < sizeof junk; n += 97) {
        CHECK(!tags_of(junk, n, NULL, &t));
        CHECK(sane(&t, junk, (int64_t)n, false));
        audio_tags_free(&t);
    }
    AtReader none = {NULL, 0, mem_read};
    CHECK(!audio_tags_read(&none, NULL, &t));
    audio_tags_free(&t);
    audio_tags_free(NULL);
}

/* ------------------------------------------------------------- seeds, a real file */

TEST(audiotags_fuzz_seeds) {
    const char *raw = getenv("HEMMA_FUZZ_RAW");
    char path[1024];
    if (raw && *raw) {
        snprintf(path, sizeof path, "%s/tags", raw);
        CHECK(dir_make(path));
    }
    static const AtTestChapter ch[] = {{0, "Intro"}, {1500, "Del 1"}, {4000, "Slut"}};
    uint8_t *jpeg = picture_bytes(48, false), *png = picture_bytes(40, true);
    for (int i = 0; i < 7; i++) {
        static const char *const names[] = {"m4b-qt",  "m4b-chpl-last",  "m4b-both", "mp3-v23",
                                            "mp3-v24-unsync", "mp3-v22", "mp3-v1"};
        Buf b = {0};
        p8(&b, i < 3 ? 2 : i == 6 ? 0 : 1); /* the fuzz target's first byte: no name, "x.mp3", "x.m4b" */
        if (i < 3) {
            AtTestM4b m = {.title = "Bok", .artist = "Jag", .narrator = "Du", .album = "Album", .ldes = "Lang\ntext",
                           .cmt = "c", .year = 2024, .track = 1, .tracks = 2, .gnre = 102,
                           .cover = i == 1 ? png : jpeg, .cover_len = i == 1 ? 40 : 48,
                           .chapters = ch, .nchapters = 3, .chpl = i > 0, .qt = i != 1, .moov_last = i == 1, .meta_qt = i == 1,
                           .duration_ms = 6000, .fake_frames = 4};
            at_test_m4b(&b, &m);
        } else {
            AtTestMp3 m = {.id3 = i == 3 ? 3 : i == 4 ? 4 : i == 5 ? 2 : 0, .unsync = i == 4, .ext_header = i == 4,
                           .enc = i == 4 ? 3 : i == 5 ? 1 : 0, .title = "Bok \xC3\xA5", .artist = "Jag", .album = "A",
                           .genre = "(183)", .comment = "c", .narrator = "Du", .year = 2020, .track = 3, .tracks = 9,
                           .cover = i == 4 ? png : jpeg, .cover_len = i == 4 ? 40 : 48, .chapters = ch, .nchapters = 3,
                           .id3v1 = i == 6, .raw_utf8 = i == 6, .frames = 3, .kbps = 32, .rate = 48000,
                           .vbr = i == 3 ? 1 : i == 4 ? 3 : 0, .lame_delay = i == 3 ? 576 : 0, .lame_padding = i == 3 ? 100 : 0};
            at_test_mp3(&b, &m);
        }
        CHECK(b.len < 4096);
        AudioTags t;
        CHECK(tags_of(b.p + 1, b.len - 1, i < 3 ? "x.m4b" : NULL, &t));
        CHECK_STR(t.title, i < 3 ? "Bok" : "Bok \xC3\xA5");
        audio_tags_free(&t);
        if (raw && *raw) {
            snprintf(path, sizeof path, "%s/tags/%s", raw, names[i]);
            CHECK(file_write_atomic(path, b.p, b.len));
        }
        buf_free(&b);
    }
    free(jpeg), free(png);
}

static bool file_read_cb(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

typedef struct {
    FILE *f;
    int64_t reads, bytes;
} Counted;

static bool counted_read(void *ctx, int64_t off, void *dst, size_t n) {
    Counted *c = ctx;
    c->reads++, c->bytes += (int64_t)n;
    return file_read_cb(c->f, off, dst, n);
}

/* By hand: HEMMA_TEST_TAGS=<file> - what it says, to hold against ffprobe -show_chapters -show_format */
TEST(audiotags_real) {
    const char *path = plat_getenv("HEMMA_TEST_TAGS");
    if (!path || !*path) return;
    int64_t size = 0;
    Counted c = {file_open(path, "rb"), 0, 0};
    CHECK(c.f && path_stat(path, &size, NULL, NULL));
    AtReader r = {&c, size, counted_read};
    AudioTags t;
    bool ok = audio_tags_read(&r, path, &t);
    fclose(c.f);
    printf("  %s: %s %s, %d ch, %d Hz, %lld ms (%lld reads, %lld bytes read of %lld)\n", path, t.format, t.codec,
           t.channels, t.sample_rate, (long long)t.duration_ms, (long long)c.reads, (long long)c.bytes, (long long)size);
    printf("  title: %s\n  artist: %s\n  album artist: %s\n  album: %s\n  narrator: %s\n  genre: %s\n  year: %d\n"
           "  track: %d/%d  disc: %d/%d\n",
           t.title, t.artist, t.album_artist, t.album, t.narrator, t.genre, t.year, t.track, t.tracks, t.disc, t.discs);
    printf("  cover: %s, %lld bytes at %lld\n  description: %.120s%s\n  %d chapters:\n", t.cover_type,
           (long long)t.cover_len, (long long)t.cover_off, t.description ? t.description : "(none)",
           t.description && strlen(t.description) > 120 ? "..." : "", t.nchapters);
    for (int i = 0; i < t.nchapters; i++) {
        int64_t s = t.chapters[i].start_ms / 1000;
        printf("    %3lld:%02lld.%03lld  %s\n", (long long)(s / 60), (long long)(s % 60), (long long)(t.chapters[i].start_ms % 1000),
               t.chapters[i].title);
    }
    CHECK(ok);
    CHECK(sane(&t, NULL, size, ok)); /* (the cover's bytes aren't looked at) */
    audio_tags_free(&t);
}
