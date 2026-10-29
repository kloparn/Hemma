/* modules/av/mp4.c — see mp4.h. Box layouts follow ISO/IEC 14496-12 (ISO
 * BMFF), 14496-15 (avcC, hvcC), 14496-14 (esds), "Encapsulation of FLAC in ISO-BMFF"
 * (dfLa) and ETSI TS 102 366 Annex F (dac3). */
#include "mp4.h"

#include <stdlib.h>
#include <string.h>

#define MOVIE_TIMESCALE 1000

/* -------------------------------------------------------- growable bytes */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool oom;
} Bb;

static bool need(Bb *b, size_t n) {
    if (b->oom) return false;
    if (n > SIZE_MAX / 4 - b->len) return !(b->oom = true); /* the sum or the doubling below would wrap (32-bit builds) */
    if (b->len + n <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < b->len + n) cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) return !(b->oom = true);
    b->p = p, b->cap = cap;
    return true;
}

static void put_bytes(Bb *b, const void *p, size_t n) {
    if (n && need(b, n)) memcpy(b->p + b->len, p, n), b->len += n;
}
static void put8(Bb *b, uint32_t v) {
    uint8_t x = (uint8_t)v;
    put_bytes(b, &x, 1);
}
static void put16(Bb *b, uint32_t v) {
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put_bytes(b, x, 2);
}
static void put24(Bb *b, uint32_t v) {
    uint8_t x[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put_bytes(b, x, 3);
}
static void put32(Bb *b, uint32_t v) {
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put_bytes(b, x, 4);
}
static void put64(Bb *b, uint64_t v) {
    put32(b, (uint32_t)(v >> 32));
    put32(b, (uint32_t)v);
}
static void put_zero(Bb *b, size_t n) {
    if (need(b, n)) memset(b->p + b->len, 0, n), b->len += n;
}
static void set32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

static size_t box(Bb *b, const char *type) { /* returns where it starts; close with end() */
    size_t at = b->len;
    put32(b, 0);
    put_bytes(b, type, 4);
    return at;
}
static size_t fullbox(Bb *b, const char *type, int version, uint32_t flags) {
    size_t at = box(b, type);
    put8(b, (uint32_t)version);
    put24(b, flags);
    return at;
}
static void end(Bb *b, size_t at) {
    if (!b->oom) set32(b->p + at, (uint32_t)(b->len - at));
}

/* ---------------------------------------------------------------- tables */

typedef struct {
    uint32_t count, value;
} Run;

typedef struct {
    uint32_t first_chunk, samples;
} StscRun;

typedef struct { /* a sample waiting in the current fragment */
    uint32_t size, duration, flags;
    int32_t cts;
} FragSample;

/* Growable array of T; push returns false when out of memory. */
#define VEC(T, name)                                                                                                   \
    T *name;                                                                                                           \
    uint32_t n_##name, cap_##name
#define PUSH(t, name, val)                                                                                             \
    ((t)->n_##name < (t)->cap_##name || grow((void **)&(t)->name, &(t)->cap_##name, sizeof *(t)->name)                 \
         ? ((t)->name[(t)->n_##name++] = (val), true)                                                                  \
         : false)

static bool grow(void **p, uint32_t *cap, size_t elem) {
    if (*cap > UINT32_MAX / 2 || (size_t)*cap * 2 > SIZE_MAX / elem) return false; /* the count or the bytes would wrap */
    uint32_t c = *cap ? *cap * 2 : 256;
    void *q = realloc(*p, (size_t)c * elem);
    if (!q) return false;
    *p = q, *cap = c;
    return true;
}

typedef struct {
    Mp4Track t;
    uint8_t *cfg;
    size_t cfg_len;
    uint32_t id;
    VEC(uint32_t, sizes);
    VEC(Run, stts);
    VEC(Run, ctts);
    VEC(uint32_t, sync); /* 1-based numbers of key samples */
    VEC(int64_t, chunks);
    VEC(StscRun, stsc);
    VEC(FragSample, frag);
    Bb data;                /* fragmented: this track's bytes in the current fragment */
    uint32_t chunk_samples; /* samples in the chunk being written */
    uint64_t duration;      /* ticks: sum of sample durations */
    uint64_t frag_dts;      /* fragmented: decode time where the current fragment starts */
    bool any_cts, neg_cts, all_key;
} Trk;

struct Mp4 {
    Mp4Out out;
    int flags;
    int64_t reserve, pos, ftyp_end, mdat_at;
    int last; /* track of the previous sample: a change starts a new chunk */
    Trk trk[MP4_MAX_TRACKS];
    int ntrk;
    bool begun, finished, failed, fast;
    uint32_t seq;
    uint64_t co64_above;
};

/* ------------------------------------------------------------------ output */

static bool emit(Mp4 *m, const void *p, size_t n) {
    if (m->failed) return false;
    if (n && !m->out.write(m->out.ctx, p, n)) return !(m->failed = true);
    m->pos += (int64_t)n;
    return true;
}

static bool emit_at(Mp4 *m, int64_t off, const void *p, size_t n) {
    if (m->failed || !m->out.write_at || !m->out.write_at(m->out.ctx, off, p, n)) return !(m->failed = true);
    return true;
}

/* ------------------------------------------------------------------- API */

Mp4 *mp4_new(const Mp4Out *out, int flags, int64_t reserve) {
    if (!out || !out->write) return NULL;
    if (!(flags & MP4_FRAGMENTED) && !out->write_at) return NULL; /* progressive needs fix-ups */
    Mp4 *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->out = *out;
    m->flags = flags;
    m->reserve = flags & MP4_FRAGMENTED || reserve < 0 ? 0 : reserve > 0x7FFFFFFF ? 0x7FFFFFFF : reserve;
    if (m->reserve && m->reserve < 8) m->reserve = 8;
    m->last = -1;
    m->seq = 1;
    m->co64_above = 0xFFFFFFFFu;
    return m;
}

void mp4_free(Mp4 *m) {
    if (!m) return;
    for (int i = 0; i < m->ntrk; i++) {
        Trk *t = &m->trk[i];
        free(t->cfg);
        free(t->sizes);
        free(t->stts);
        free(t->ctts);
        free(t->sync);
        free(t->chunks);
        free(t->stsc);
        free(t->frag);
        free(t->data.p);
    }
    free(m);
}

void mp4_set_co64_threshold(Mp4 *m, uint64_t offset) { m->co64_above = offset; }
int64_t mp4_bytes(const Mp4 *m) { return m->pos; }
bool mp4_faststart(const Mp4 *m) { return m->fast; }

int64_t mp4_moov_bound(int64_t samples, int64_t chunks) {
    /* worst cases per sample: stsz 4 + stts 8 + ctts 8 + stss 4; per chunk: stsc 12 + co64 8 */
    return 8192 + samples * 24 + chunks * 20;
}

static bool is_video(int codec) { return codec == MP4_H264 || codec == MP4_HEVC || codec == MP4_AV1; }

/* 'hvc1' says every parameter set is in the hvcC (14496-15 8.4.1): its VPS/SPS/PPS arrays get
   array_completeness = 1, as Apple's players expect. A record that doesn't parse stays as it is. */
static void hvcc_complete(uint8_t *p, size_t n) {
    size_t at[64], k = 0, i = 23;
    if (n < 23) return;
    for (int a = 0; a < p[22]; a++) {
        if (i + 3 > n || k == 64) return;
        at[k++] = i;
        unsigned count = (unsigned)p[i + 1] << 8 | p[i + 2];
        i += 3;
        for (unsigned u = 0; u < count; u++) {
            if (i + 2 > n) return;
            i += 2 + ((size_t)p[i] << 8 | p[i + 1]);
            if (i > n) return;
        }
    }
    for (size_t a = 0; a < k; a++) {
        int type = p[at[a]] & 0x3F;
        if (type >= 32 && type <= 34) p[at[a]] |= 0x80;
    }
}

bool mp4_set_config(Mp4 *m, int track, const uint8_t *config, size_t n) {
    if (track < 0 || track >= m->ntrk || (m->begun && (m->flags & MP4_FRAGMENTED))) return false;
    Trk *t = &m->trk[track];
    uint8_t *c = n ? malloc(n) : NULL;
    if (n && !c) return false;
    if (n) memcpy(c, config, n);
    if (n && t->t.codec == MP4_HEVC) hvcc_complete(c, n);
    free(t->cfg);
    t->cfg = c, t->cfg_len = n;
    return true;
}

int mp4_add_track(Mp4 *m, const Mp4Track *tr) {
    if (m->begun || m->ntrk == MP4_MAX_TRACKS || !tr->timescale || tr->codec < MP4_H264 || tr->codec > MP4_AV1) return -1;
    int i = m->ntrk;
    Trk *t = &m->trk[i];
    memset(t, 0, sizeof *t);
    t->t = *tr;
    t->t.config = NULL;
    t->id = (uint32_t)i + 1;
    t->all_key = true;
    m->ntrk++;
    if (tr->config_len && !mp4_set_config(m, i, tr->config, tr->config_len)) {
        m->ntrk--;
        return -1;
    }
    return i;
}

static void ftyp(const Mp4 *m, Bb *b) {
    size_t at = box(b, "ftyp");
    put_bytes(b, "isom", 4);
    put32(b, 0x200);
    put_bytes(b, "isom", 4);
    put_bytes(b, m->flags & MP4_FRAGMENTED ? "iso6" : "iso2", 4);
    bool av1 = false;
    for (int i = 0; i < m->ntrk; i++) av1 |= m->trk[i].t.codec == MP4_AV1;
    put_bytes(b, av1 ? "av01" : "avc1", 4); /* (AV1 in ISOBMFF: a file with AV1 lists the 'av01' brand) */
    put_bytes(b, "mp41", 4);
    end(b, at);
}

static void moov(Mp4 *m, Bb *b, bool co64);

bool mp4_begin(Mp4 *m) {
    if (m->begun || !m->ntrk) return false;
    m->begun = true;
    Bb b = {0};
    ftyp(m, &b);
    if (m->flags & MP4_FRAGMENTED) {
        moov(m, &b, false);
        bool ok = !b.oom && emit(m, b.p, b.len);
        free(b.p);
        return ok;
    }
    m->ftyp_end = (int64_t)b.len;
    if (m->reserve) { /* a 'free' box the moov will overwrite */
        put32(&b, (uint32_t)m->reserve);
        put_bytes(&b, "free", 4);
    }
    bool ok = !b.oom && emit(m, b.p, b.len);
    free(b.p);
    static const uint8_t zero[4096] = {0};
    for (int64_t left = m->reserve - 8; ok && left > 0; left -= (int64_t)sizeof zero)
        ok = emit(m, zero, left < (int64_t)sizeof zero ? (size_t)left : sizeof zero);
    m->mdat_at = m->pos;
    static const uint8_t mdat[16] = {0, 0, 0, 1, 'm', 'd', 'a', 't'}; /* 64-bit size, filled in at the end */
    return ok && emit(m, mdat, sizeof mdat);
}

bool mp4_sample(Mp4 *m, int track, const void *data, size_t n, uint32_t duration, int32_t cts, bool key) {
    if (!m->begun || m->finished || m->failed || track < 0 || track >= m->ntrk || n > 0xFFFFFFFFu) return false;
    Trk *t = &m->trk[track];
    if (m->flags & MP4_FRAGMENTED) {
        uint32_t flags = key ? 0x02000000 : 0x01010000; /* depends on nothing / depends on others, not sync */
        FragSample fs = {(uint32_t)n, duration, flags, cts};
        put_bytes(&t->data, data, n);
        if (t->data.oom || !PUSH(t, frag, fs)) return !(m->failed = true);
        if (cts) t->any_cts = true;
        if (cts < 0) t->neg_cts = true;
        return true;
    }
    /* chunking: a run of samples of one track is one chunk */
    if (m->last != track || !t->n_chunks) {
        if (t->chunk_samples && (!t->n_stsc || t->stsc[t->n_stsc - 1].samples != t->chunk_samples)) {
            StscRun r = {t->n_chunks, t->chunk_samples};
            if (!PUSH(t, stsc, r)) return !(m->failed = true);
        }
        if (!PUSH(t, chunks, m->pos)) return !(m->failed = true);
        t->chunk_samples = 0;
        m->last = track;
    }
    if (!emit(m, data, n)) return false;
    t->chunk_samples++;
    bool ok = PUSH(t, sizes, (uint32_t)n);
    if (ok && t->n_stts && t->stts[t->n_stts - 1].value == duration) t->stts[t->n_stts - 1].count++;
    else if (ok) {
        Run r = {1, duration};
        ok = PUSH(t, stts, r);
    }
    if (ok && t->n_ctts && t->ctts[t->n_ctts - 1].value == (uint32_t)cts) t->ctts[t->n_ctts - 1].count++;
    else if (ok) {
        Run r = {1, (uint32_t)cts};
        ok = PUSH(t, ctts, r);
    }
    if (cts) t->any_cts = true;
    if (cts < 0) t->neg_cts = true;
    if (ok && key) ok = PUSH(t, sync, t->n_sizes);
    if (!key) t->all_key = false;
    t->duration += duration;
    return ok || !(m->failed = true);
}

/* ------------------------------------------------------------------ moov */

static uint64_t to_movie(uint64_t ticks, uint32_t timescale) {
    return (ticks * MOVIE_TIMESCALE + timescale / 2) / timescale;
}

static uint64_t track_movie_duration(const Trk *t) { /* what tkhd/edit lists show */
    uint64_t media = t->duration > (uint64_t)t->t.start ? t->duration - (uint64_t)t->t.start : 0;
    return to_movie(media + (uint64_t)t->t.delay, t->t.timescale);
}

static void matrix(Bb *b) {
    static const uint32_t unity[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (int i = 0; i < 9; i++) put32(b, unity[i]);
}

static void lang(Bb *b, const char *l) {
    const char *s = l && strlen(l) == 3 ? l : "und";
    put16(b, (uint32_t)((s[0] - 0x60) & 31) << 10 | (uint32_t)((s[1] - 0x60) & 31) << 5 | (uint32_t)((s[2] - 0x60) & 31));
}

static size_t desc_size(size_t n) { return n < 0x80 ? 1 : 4; }

static void desc_len(Bb *b, size_t n) { /* MPEG-4 descriptor size (expandable) */
    if (n < 0x80) put8(b, (uint32_t)n);
    else {
        put8(b, 0x80 | (uint32_t)(n >> 21 & 0x7F));
        put8(b, 0x80 | (uint32_t)(n >> 14 & 0x7F));
        put8(b, 0x80 | (uint32_t)(n >> 7 & 0x7F));
        put8(b, (uint32_t)(n & 0x7F));
    }
}

static void audio_entry(Bb *b, const Trk *t, uint32_t bits) {
    put_zero(b, 6);
    put16(b, 1); /* data_reference_index */
    put_zero(b, 8);
    put16(b, (uint32_t)(t->t.channels ? t->t.channels : 2));
    put16(b, bits);
    put32(b, 0);
    uint32_t rate = (uint32_t)(t->t.sample_rate ? t->t.sample_rate : (int)t->t.timescale);
    put32(b, rate < 65536 ? rate << 16 : 0);
}

static void sample_entry(Bb *b, const Trk *t) {
    size_t e;
    switch (t->t.codec) {
    case MP4_H264:
    case MP4_HEVC:
    case MP4_AV1: {
        int k = t->t.codec == MP4_HEVC ? 1 : t->t.codec == MP4_AV1 ? 2 : 0;
        e = box(b, (const char *[]){"avc1", "hvc1", "av01"}[k]);
        put_zero(b, 6);
        put16(b, 1);
        put_zero(b, 16);
        put16(b, (uint32_t)t->t.width);
        put16(b, (uint32_t)t->t.height);
        put32(b, 0x00480000); /* 72 dpi */
        put32(b, 0x00480000);
        put32(b, 0);
        put16(b, 1); /* frame_count */
        uint8_t name[32] = {5, 'h', 'e', 'm', 'm', 'a'};
        put_bytes(b, name, 32);
        put16(b, 0x0018);
        put16(b, 0xFFFF);
        size_t c = box(b, (const char *[]){"avcC", "hvcC", "av1C"}[k]);
        put_bytes(b, t->cfg, t->cfg_len);
        end(b, c);
        if (t->t.sar_num > 0 && t->t.sar_den > 0) {
            c = box(b, "pasp");
            put32(b, (uint32_t)t->t.sar_num);
            put32(b, (uint32_t)t->t.sar_den);
            end(b, c);
        }
        if (t->t.primaries || t->t.transfer || t->t.matrix) {
            c = box(b, "colr");
            put_bytes(b, "nclx", 4);
            put16(b, (uint32_t)(t->t.primaries ? t->t.primaries : 2)); /* 2 = unspecified */
            put16(b, (uint32_t)(t->t.transfer ? t->t.transfer : 2));
            put16(b, (uint32_t)(t->t.matrix ? t->t.matrix : 2));
            put8(b, t->t.full_range ? 0x80 : 0);
            end(b, c);
        }
        end(b, e);
        break;
    }
    case MP4_AAC: {
        e = box(b, "mp4a");
        audio_entry(b, t, 16);
        size_t c = fullbox(b, "esds", 0, 0);
        size_t dsi = 1 + desc_size(t->cfg_len) + t->cfg_len; /* whole DecoderSpecificInfo */
        size_t dcd = 13 + dsi;                                /* DecoderConfigDescriptor contents */
        size_t es = 3 + 1 + desc_size(dcd) + dcd + 3;         /* ES_Descriptor contents */
        put8(b, 0x03); /* ES_Descriptor */
        desc_len(b, es);
        put16(b, t->id);
        put8(b, 0);
        put8(b, 0x04); /* DecoderConfigDescriptor */
        desc_len(b, dcd);
        put8(b, 0x40); /* Audio ISO/IEC 14496-3 */
        put8(b, 0x15); /* audio stream */
        put24(b, 0);
        put32(b, t->t.max_bitrate);
        put32(b, t->t.avg_bitrate);
        put8(b, 0x05); /* DecoderSpecificInfo: the AudioSpecificConfig */
        desc_len(b, t->cfg_len);
        put_bytes(b, t->cfg, t->cfg_len);
        put8(b, 0x06); /* SLConfigDescriptor */
        put8(b, 1);
        put8(b, 2);
        end(b, c);
        end(b, e);
        break;
    }
    case MP4_FLAC: {
        e = box(b, "fLaC");
        audio_entry(b, t, (uint32_t)(t->t.sample_bits ? t->t.sample_bits : 16));
        size_t c = fullbox(b, "dfLa", 0, 0);
        put8(b, 0x80); /* last metadata block, STREAMINFO */
        put24(b, (uint32_t)t->cfg_len);
        put_bytes(b, t->cfg, t->cfg_len);
        end(b, c);
        end(b, e);
        break;
    }
    default: { /* AC-3 */
        e = box(b, "ac-3");
        audio_entry(b, t, 16);
        size_t c = box(b, "dac3");
        put_bytes(b, t->cfg, t->cfg_len);
        end(b, c);
        end(b, e);
        break;
    }
    }
}

static void stbl(Mp4 *m, Bb *b, const Trk *t, bool co64) {
    bool frag = m->flags & MP4_FRAGMENTED;
    size_t s = box(b, "stbl");
    size_t d = fullbox(b, "stsd", 0, 0);
    put32(b, 1);
    sample_entry(b, t);
    end(b, d);
    d = fullbox(b, "stts", 0, 0);
    put32(b, frag ? 0 : t->n_stts);
    for (uint32_t i = 0; !frag && i < t->n_stts; i++) put32(b, t->stts[i].count), put32(b, t->stts[i].value);
    end(b, d);
    if (!frag && t->any_cts) {
        d = fullbox(b, "ctts", t->neg_cts ? 1 : 0, 0);
        put32(b, t->n_ctts);
        for (uint32_t i = 0; i < t->n_ctts; i++) put32(b, t->ctts[i].count), put32(b, t->ctts[i].value);
        end(b, d);
    }
    if (!frag && !t->all_key) {
        d = fullbox(b, "stss", 0, 0);
        put32(b, t->n_sync);
        for (uint32_t i = 0; i < t->n_sync; i++) put32(b, t->sync[i]);
        end(b, d);
    }
    d = fullbox(b, "stsz", 0, 0);
    put32(b, 0);
    put32(b, frag ? 0 : t->n_sizes);
    for (uint32_t i = 0; !frag && i < t->n_sizes; i++) put32(b, t->sizes[i]);
    end(b, d);
    d = fullbox(b, "stsc", 0, 0);
    put32(b, frag ? 0 : t->n_stsc);
    for (uint32_t i = 0; !frag && i < t->n_stsc; i++) put32(b, t->stsc[i].first_chunk), put32(b, t->stsc[i].samples), put32(b, 1);
    end(b, d);
    d = fullbox(b, co64 ? "co64" : "stco", 0, 0);
    put32(b, frag ? 0 : t->n_chunks);
    for (uint32_t i = 0; !frag && i < t->n_chunks; i++) {
        if (co64) put64(b, (uint64_t)t->chunks[i]);
        else put32(b, (uint32_t)t->chunks[i]);
    }
    end(b, d);
    end(b, s);
}

static void trak(Mp4 *m, Bb *b, const Trk *t, bool co64) {
    bool video = is_video(t->t.codec);
    uint64_t mdur = track_movie_duration(t);
    size_t k = box(b, "trak");
    bool v1 = mdur > 0xFFFFFFFFu;
    size_t h = fullbox(b, "tkhd", v1, 3); /* enabled, in movie */
    if (v1) put64(b, 0), put64(b, 0), put32(b, t->id), put32(b, 0), put64(b, mdur);
    else put32(b, 0), put32(b, 0), put32(b, t->id), put32(b, 0), put32(b, (uint32_t)mdur);
    put_zero(b, 8);
    put16(b, 0);                 /* layer */
    put16(b, video ? 0 : 1);     /* alternate group */
    put16(b, video ? 0 : 0x0100); /* volume */
    put16(b, 0);
    matrix(b);
    uint32_t w = (uint32_t)t->t.width, hh = (uint32_t)t->t.height;
    if (video && t->t.sar_num > 0 && t->t.sar_den > 0) w = (uint32_t)((uint64_t)w * (uint64_t)t->t.sar_num / (uint64_t)t->t.sar_den);
    put32(b, video ? w << 16 : 0);
    put32(b, video ? hh << 16 : 0);
    end(b, h);
    if (!(m->flags & MP4_FRAGMENTED) && (t->t.start || t->t.delay)) {
        size_t e = box(b, "edts");
        size_t l = fullbox(b, "elst", 1, 0);
        put32(b, t->t.delay ? 2 : 1);
        if (t->t.delay) {
            put64(b, to_movie((uint64_t)t->t.delay, t->t.timescale));
            put64(b, (uint64_t)-1); /* empty edit */
            put32(b, 0x00010000);
        }
        uint64_t media = t->duration > (uint64_t)t->t.start ? t->duration - (uint64_t)t->t.start : 0;
        put64(b, to_movie(media, t->t.timescale));
        put64(b, (uint64_t)t->t.start);
        put32(b, 0x00010000);
        end(b, l);
        end(b, e);
    }
    size_t md = box(b, "mdia");
    uint64_t dur = m->flags & MP4_FRAGMENTED ? 0 : t->duration;
    bool mv1 = dur > 0xFFFFFFFFu;
    size_t d = fullbox(b, "mdhd", mv1, 0);
    if (mv1) put64(b, 0), put64(b, 0), put32(b, t->t.timescale), put64(b, dur);
    else put32(b, 0), put32(b, 0), put32(b, t->t.timescale), put32(b, (uint32_t)dur);
    lang(b, t->t.lang);
    put16(b, 0);
    end(b, d);
    d = fullbox(b, "hdlr", 0, 0);
    put32(b, 0);
    put_bytes(b, video ? "vide" : "soun", 4);
    put_zero(b, 12);
    const char *name = video ? "VideoHandler" : "SoundHandler";
    put_bytes(b, name, strlen(name) + 1);
    end(b, d);
    size_t mi = box(b, "minf");
    if (video) {
        d = fullbox(b, "vmhd", 0, 1);
        put_zero(b, 8);
    } else {
        d = fullbox(b, "smhd", 0, 0);
        put32(b, 0);
    }
    end(b, d);
    size_t di = box(b, "dinf");
    d = fullbox(b, "dref", 0, 0);
    put32(b, 1);
    size_t u = fullbox(b, "url ", 0, 1); /* media in this file */
    end(b, u);
    end(b, d);
    end(b, di);
    stbl(m, b, t, co64);
    end(b, mi);
    end(b, md);
    end(b, k);
}

static void moov(Mp4 *m, Bb *b, bool co64) {
    bool frag = m->flags & MP4_FRAGMENTED;
    uint64_t dur = 0;
    for (int i = 0; i < m->ntrk && !frag; i++) {
        uint64_t d = track_movie_duration(&m->trk[i]);
        if (d > dur) dur = d;
    }
    size_t mv = box(b, "moov");
    bool v1 = dur > 0xFFFFFFFFu;
    size_t h = fullbox(b, "mvhd", v1, 0);
    if (v1) put64(b, 0), put64(b, 0), put32(b, MOVIE_TIMESCALE), put64(b, dur);
    else put32(b, 0), put32(b, 0), put32(b, MOVIE_TIMESCALE), put32(b, (uint32_t)dur);
    put32(b, 0x00010000); /* rate 1.0 */
    put16(b, 0x0100);     /* volume 1.0 */
    put_zero(b, 10);
    matrix(b);
    put_zero(b, 24);
    put32(b, (uint32_t)m->ntrk + 1);
    end(b, h);
    for (int i = 0; i < m->ntrk; i++) trak(m, b, &m->trk[i], co64);
    if (frag) {
        size_t x = box(b, "mvex");
        for (int i = 0; i < m->ntrk; i++) {
            size_t t = fullbox(b, "trex", 0, 0);
            put32(b, m->trk[i].id);
            put32(b, 1);
            put32(b, 0), put32(b, 0), put32(b, 0);
            end(b, t);
        }
        end(b, x);
    }
    end(b, mv);
}

/* ------------------------------------------------------------- fragments */

bool mp4_fragment(Mp4 *m) {
    if (!(m->flags & MP4_FRAGMENTED) || !m->begun || m->failed) return false;
    int pending = 0;
    for (int i = 0; i < m->ntrk; i++) pending += m->trk[i].n_frag > 0;
    if (!pending) return true;
    Bb b = {0};
    size_t mf = box(&b, "moof");
    size_t h = fullbox(&b, "mfhd", 0, 0);
    put32(&b, m->seq);
    end(&b, h);
    size_t offset_at[MP4_MAX_TRACKS] = {0};
    for (int i = 0; i < m->ntrk; i++) {
        Trk *t = &m->trk[i];
        if (!t->n_frag) continue;
        size_t tf = box(&b, "traf");
        h = fullbox(&b, "tfhd", 0, 0x020000); /* default-base-is-moof */
        put32(&b, t->id);
        end(&b, h);
        h = fullbox(&b, "tfdt", 1, 0);
        put64(&b, t->frag_dts);
        end(&b, h);
        uint32_t flags = 0x000001 | 0x000100 | 0x000200 | 0x000400 | (t->any_cts ? 0x000800 : 0);
        h = fullbox(&b, "trun", t->neg_cts ? 1 : 0, flags);
        put32(&b, t->n_frag);
        offset_at[i] = b.len;
        put32(&b, 0); /* data_offset: patched below */
        for (uint32_t s = 0; s < t->n_frag; s++) {
            const FragSample *fs = &t->frag[s];
            put32(&b, fs->duration);
            put32(&b, fs->size);
            put32(&b, is_video(t->t.codec) ? fs->flags : 0x02000000);
            if (t->any_cts) put32(&b, (uint32_t)fs->cts);
        }
        end(&b, h);
        end(&b, tf);
    }
    end(&b, mf);
    size_t moof_len = b.len, data = 0;
    for (int i = 0; i < m->ntrk; i++) data += m->trk[i].data.len;
    size_t off = moof_len + 8;
    for (int i = 0; i < m->ntrk; i++) {
        Trk *t = &m->trk[i];
        if (!t->n_frag) continue;
        if (!b.oom) set32(b.p + offset_at[i], (uint32_t)off);
        off += t->data.len;
    }
    put32(&b, (uint32_t)(8 + data));
    put_bytes(&b, "mdat", 4);
    bool ok = !b.oom && data + 8 <= 0xFFFFFFFFu && emit(m, b.p, b.len);
    free(b.p);
    for (int i = 0; i < m->ntrk && ok; i++) {
        Trk *t = &m->trk[i];
        ok = emit(m, t->data.p, t->data.len);
        for (uint32_t s = 0; s < t->n_frag; s++) t->frag_dts += t->frag[s].duration;
        t->duration = t->frag_dts;
        t->n_frag = 0;
        t->data.len = 0;
        t->any_cts = t->neg_cts = false;
    }
    m->seq++;
    return ok;
}

/* ---------------------------------------------------------------- finish */

bool mp4_finish(Mp4 *m) {
    if (!m->begun || m->finished || m->failed) return false;
    m->finished = true;
    if (m->flags & MP4_FRAGMENTED) return mp4_fragment(m);
    bool co64 = false;
    for (int i = 0; i < m->ntrk; i++) {
        Trk *t = &m->trk[i];
        if (t->chunk_samples && (!t->n_stsc || t->stsc[t->n_stsc - 1].samples != t->chunk_samples)) {
            StscRun r = {t->n_chunks, t->chunk_samples};
            if (!PUSH(t, stsc, r)) return !(m->failed = true);
        }
        t->chunk_samples = 0;
        for (uint32_t c = 0; c < t->n_chunks; c++)
            if ((uint64_t)t->chunks[c] > m->co64_above) co64 = true;
    }
    uint8_t size[8];
    uint64_t mdat = (uint64_t)(m->pos - m->mdat_at);
    for (int i = 0; i < 8; i++) size[i] = (uint8_t)(mdat >> (56 - 8 * i));
    if (!emit_at(m, m->mdat_at + 8, size, 8)) return false;
    Bb b = {0};
    moov(m, &b, co64);
    if (b.oom) {
        free(b.p);
        return !(m->failed = true);
    }
    bool ok;
    if (m->reserve && ((int64_t)b.len == m->reserve || (int64_t)b.len + 8 <= m->reserve)) {
        ok = emit_at(m, m->ftyp_end, b.p, b.len);
        if (ok && (int64_t)b.len < m->reserve) { /* what is left of the reservation stays a 'free' box */
            uint8_t fb[8];
            set32(fb, (uint32_t)(m->reserve - (int64_t)b.len));
            memcpy(fb + 4, "free", 4);
            ok = emit_at(m, m->ftyp_end + (int64_t)b.len, fb, 8);
        }
        m->fast = ok;
    } else {
        ok = emit(m, b.p, b.len); /* didn't fit: moov at the end (still valid, just not faststart) */
    }
    free(b.p);
    return ok;
}

/* ---------------------------------------------------------------- helpers */

size_t mp4_avcc(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len, uint8_t *out, size_t cap) {
    if (sps_len < 4 || !pps_len || sps_len > 0xFFFF || pps_len > 0xFFFF) return 0;
    int profile = sps[1];
    bool high = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
                profile == 86 || profile == 118 || profile == 128;
    size_t n = 6 + 2 + sps_len + 1 + 2 + pps_len + (high ? 4 : 0);
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = 1;
    *p++ = sps[1], *p++ = sps[2], *p++ = sps[3];
    *p++ = 0xFF; /* 4-byte NAL lengths */
    *p++ = 0xE1; /* one SPS */
    *p++ = (uint8_t)(sps_len >> 8), *p++ = (uint8_t)sps_len;
    memcpy(p, sps, sps_len), p += sps_len;
    *p++ = 1;
    *p++ = (uint8_t)(pps_len >> 8), *p++ = (uint8_t)pps_len;
    memcpy(p, pps, pps_len), p += pps_len;
    if (high) {
        *p++ = 0xFC | 1; /* chroma_format_idc 1 (4:2:0) */
        *p++ = 0xF8;     /* bit_depth_luma - 8 */
        *p++ = 0xF8;     /* bit_depth_chroma - 8 */
        *p++ = 0;        /* no SPS extensions */
    }
    return n;
}

static size_t start_code(const uint8_t *p, size_t n, size_t i) { /* index of the next 00 00 01, or n */
    for (; i + 2 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
    return n;
}

size_t mp4_annexb_to_lengths(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0, i = start_code(in, n, 0);
    while (i < n) {
        size_t s = i + 3, e = start_code(in, n, s), z = e;
        while (z > s && in[z - 1] == 0) z--; /* trailing zeros belong to the next start code (00 00 00 01) */
        size_t len = z - s;
        if (len) {
            if (o + 4 + len > cap) return 0;
            out[o] = (uint8_t)(len >> 24), out[o + 1] = (uint8_t)(len >> 16), out[o + 2] = (uint8_t)(len >> 8), out[o + 3] = (uint8_t)len;
            memcpy(out + o + 4, in + s, len);
            o += 4 + len;
        }
        i = e;
    }
    return o;
}

bool mp4_dac3(const uint8_t *f, size_t n, uint8_t out[3]) {
    if (n < 8 || f[0] != 0x0B || f[1] != 0x77) return false;
    uint32_t fscod = f[4] >> 6, frmsizecod = f[4] & 0x3F, bsid = f[5] >> 3, bsmod = f[5] & 7, acmod = f[6] >> 5;
    if (fscod == 3 || frmsizecod > 37 || bsid > 8) return false;
    /* lfeon follows the optional mix-level fields */
    int bit = 3; /* bits of f[6] used by acmod */
    if ((acmod & 1) && acmod != 1) bit += 2; /* cmixlev */
    if (acmod & 4) bit += 2;                 /* surmixlev */
    if (acmod == 2) bit += 2;                /* dsurmod */
    uint32_t lfeon = (uint32_t)(((f[6] << 8 | f[7]) >> (15 - bit)) & 1);
    uint32_t v = fscod << 22 | bsid << 17 | bsmod << 14 | acmod << 11 | lfeon << 10 | (frmsizecod >> 1) << 5;
    out[0] = (uint8_t)(v >> 16), out[1] = (uint8_t)(v >> 8), out[2] = (uint8_t)v;
    return true;
}
