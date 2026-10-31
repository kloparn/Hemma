/* modules/av/mkv_mp4.h — the MP4/MOV side of mkv.c (2.5; the owner's DuckTales: 101 MP4s of H.264 + AC-3, silent in
 * Chrome and Firefox, and the converter read only MKV). Included by mkv.c alone, so every build with mkv.c - the
 * program, av.wasm, the fuzz targets - has it, and conv.c reads an MP4 through mkv.h's API unchanged.
 *
 * An ISO BMFF (MP4, M4V) or QuickTime (MOV) file: the moov read whole (at most MP4_MOOV_CAP), each trak's sample
 * table turned into a list of samples (file offset, size, decode time, composition offset, key), and mkv_next()
 * handing out every track's samples in file-offset order - as stored, interleaved - with their presentation time in
 * ns (the composition time less the edit list's media_time, plus an empty edit's delay). Codecs are named as
 * Matroska names them ("V_MPEG4/ISO/AVC" with the avcC as CodecPrivate, "A_AAC" with the AudioSpecificConfig, "A_AC3"
 * ...), so the converter's table and checks work as they are; MkvTrack.fourcc keeps the sample entry. A text track
 * (QuickTime chapters, timecodes) is no track to it (type 0, no samples handed out); tx3g subtitles are S_TEXT/UTF8,
 * their text alone. Fragmented files (mvex, moof) are refused: not yet. Bounded everywhere: box sizes against their
 * parent, sample counts against the file's size and MP4_MAX_SAMPLES, chunks and samples inside the file (a sample
 * past its end ends the track there - a cut file still reads). */

#define MP4_MOOV_CAP (64u << 20)
#define MP4_MAX_SAMPLES 4000000 /* one track's; a 3-hour film has ~260k pictures and ~500k AAC blocks */
#define MP4_MAX_TOP 4096        /* top-level boxes looked at */
#define MP4_MAX_FRAME (64u << 20)

typedef struct {
    int64_t off, dts; /* in the file; in the track's timescale */
    uint32_t size;
    int32_t cto; /* composition offset */
    uint8_t key;
} Mp4Sample;

typedef struct {
    int idx; /* its MkvTrack */
    uint32_t timescale;
    int64_t shift;    /* media time shown at movie time 0 (the edit list's media_time) */
    int64_t delay_ns; /* an empty edit first: it starts this much later */
    bool tx3g;        /* subtitles: a sample is a 16-bit length and the text, then style boxes */
    uint32_t last;    /* the last sample's length (stts): what the next one's start can't say */
    Mp4Sample *s;
    uint32_t n, at; /* samples, the next to hand out */
} Mp4Trk;

struct Mp4r {
    Mp4Trk t[MKV_MAX_TRACKS];
    int nt;
    uint8_t *buf; /* the frame handed out */
    size_t cap;
};

/* ---- big-endian reading of memory ---- */
static uint32_t m4_be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t m4_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t m4_be64(const uint8_t *p) { return (uint64_t)m4_be32(p) << 32 | m4_be32(p + 4); }

typedef struct {
    const uint8_t *p;
    size_t n;
} Span;

/* The next box of s: its type and body; s moves past it. False at the end or on a box that doesn't fit. */
static bool m4_box(Span *s, char type[5], Span *body) {
    if (s->n < 8) return false;
    uint64_t size = m4_be32(s->p);
    size_t head = 8;
    if (size == 1) {
        if (s->n < 16) return false;
        size = m4_be64(s->p + 8);
        head = 16;
    } else if (size == 0) size = s->n; /* (to the end) */
    if (size < head || size > s->n) return false;
    memcpy(type, s->p + 4, 4);
    type[4] = '\0';
    body->p = s->p + head;
    body->n = (size_t)size - head;
    s->p += size;
    s->n -= (size_t)size;
    return true;
}

/* The first child of s of that type (s itself not moved) */
static bool m4_child(Span s, const char *type, Span *out) {
    char t[5];
    Span b;
    for (int k = 0; k < 4096 && m4_box(&s, t, &b); k++)
        if (!memcmp(t, type, 4)) {
            *out = b;
            return true;
        }
    return false;
}

/* children by a path: "mdia/minf/stbl" */
static bool m4_path(Span s, const char *path, Span *out) {
    Span cur = s;
    while (*path) {
        char t[5] = {0};
        memcpy(t, path, 4);
        if (!m4_child(cur, t, &cur)) return false;
        path += 4;
        if (*path == '/') path++;
    }
    *out = cur;
    return true;
}

/* ticks of a timescale -> ns, never overflowing: past ~285 years (damage - conv.c's MAX_TS calls 13 days that) it
   stays at that */
static int64_t m4_ns(int64_t v, uint32_t ts) {
    if (!ts) return 0;
    int64_t s = v / ts;
    if (s > 9000000000LL) return 9000000000LL * 1000000000LL;
    if (s < -9000000000LL) return -9000000000LL * 1000000000LL;
    return s * 1000000000LL + v % ts * 1000000000LL / ts;
}

/* An MPEG-4 descriptor's length (expandable, up to 4 bytes); false past the end */
static bool m4_desc_len(const uint8_t **p, const uint8_t *end, uint32_t *len) {
    *len = 0;
    for (int i = 0; i < 4; i++) {
        if (*p >= end) return false;
        uint8_t b = *(*p)++;
        *len = *len << 7 | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return true;
}

/* esds: the DecoderConfigDescriptor's object type and the DecoderSpecificInfo (the AudioSpecificConfig ...) */
static bool m4_esds(Span e, int *oti, const uint8_t **dsi, size_t *dsi_len) {
    *oti = 0, *dsi = NULL, *dsi_len = 0;
    if (e.n < 4) return false;
    const uint8_t *p = e.p + 4, *end = e.p + e.n;
    uint32_t len;
    if (p >= end || *p++ != 0x03 || !m4_desc_len(&p, end, &len) || end - p < 3) return false;
    uint8_t flags = p[2];
    p += 3;
    if (flags & 0x80) p += 2;
    if (flags & 0x40) {
        if (p >= end) return false;
        p += 1 + *p;
    }
    if (flags & 0x20) p += 2;
    if (p >= end || *p++ != 0x04 || !m4_desc_len(&p, end, &len) || end - p < 13) return false;
    const uint8_t *dcd_end = p + len > end ? end : p + len;
    *oti = p[0];
    p += 13;
    if (p < dcd_end && *p == 0x05) {
        p++;
        if (m4_desc_len(&p, dcd_end, &len) && len <= (uint32_t)(dcd_end - p)) *dsi = p, *dsi_len = len;
    }
    return true;
}

static void m4_set(char *dst, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

static bool m4_priv(MkvTrack *t, const uint8_t *p, size_t n) {
    if (!p || !n || n > MAX_PRIVATE) return true;
    uint8_t *q = malloc(n);
    if (!q) return false;
    memcpy(q, p, n);
    t->priv = q;
    t->priv_len = n;
    return true;
}

/* A name as UTF-8 (a C string, or old QuickTime's Pascal string), handlers' own names left out */
static void m4_name(MkvTrack *t, const uint8_t *p, size_t n) {
    if (!n) return;
    if (p[0] && p[0] == n - 1) p++, n--; /* (Pascal: its length first) */
    char s[sizeof t->name];
    size_t k = 0;
    for (size_t i = 0; i < n && p[i] && k + 1 < sizeof s; i++) s[k++] = (char)(p[i] < 0x20 ? ' ' : p[i]);
    s[k] = '\0';
    while (k && s[k - 1] == ' ') s[--k] = '\0';
    size_t lead = strspn(s, " ");
    if (!s[lead] || (k >= 7 && !memcmp(s + k - 7, "Handler", 7)) || (k >= 7 && !memcmp(s + k - 7, "handler", 7)) ||
        !strcmp(s + lead, "Core Media Audio") || !strcmp(s + lead, "Core Media Video"))
        return;
    m4_set(t->name, sizeof t->name, s + lead);
}

/* The sample entry: what codec, its setup, the picture's size or the sound's rate. false = out of memory. */
static bool m4_entry(MkvTrack *t, Span stsd, uint32_t handler, bool *tx3g) {
    *tx3g = false;
    if (stsd.n < 8) return true;
    Span all = {stsd.p + 8, stsd.n - 8}, body; /* (version/flags, entry count) */
    char fc[5];
    if (!m4_box(&all, fc, &body) || body.n < 8) return true;
    memcpy(t->fourcc, fc, 5);
    bool video = handler == 0x76696465u /* vide */, sound = handler == 0x736F756Eu /* soun */;
    if (video && body.n >= 78) {
        t->type = MKV_VIDEO;
        t->width = (int)m4_be16(body.p + 24);
        t->height = (int)m4_be16(body.p + 26);
        Span kids = {body.p + 78, body.n - 78}, c;
        const char *codec = NULL, *cfg = NULL; /* (Matroska's CodecPrivate for these = the MP4's config record) */
        if (!strcmp(fc, "avc1") || !strcmp(fc, "avc3")) codec = "V_MPEG4/ISO/AVC", cfg = "avcC";
        else if (!strcmp(fc, "hvc1") || !strcmp(fc, "hev1")) codec = "V_MPEGH/ISO/HEVC", cfg = "hvcC";
        else if (!strcmp(fc, "av01")) codec = "V_AV1", cfg = "av1C";
        if (codec) {
            m4_set(t->codec, sizeof t->codec, codec);
            if (m4_child(kids, cfg, &c) && !m4_priv(t, c.p, c.n)) return false;
        } else if (!strcmp(fc, "mp4v")) { /* MPEG-4 Part 2 (DivX/Xvid), or MPEG-2/-1 by its object type */
            int oti;
            const uint8_t *dsi;
            size_t dl;
            if (m4_child(kids, "esds", &c) && m4_esds(c, &oti, &dsi, &dl)) {
                if (oti >= 0x60 && oti <= 0x65) m4_set(t->codec, sizeof t->codec, "V_MPEG2");
                else if (oti == 0x6A) m4_set(t->codec, sizeof t->codec, "V_MPEG1");
                else m4_set(t->codec, sizeof t->codec, "V_MPEG4/ISO/ASP");
                if (dl && !m4_priv(t, dsi, dl)) return false;
            } else m4_set(t->codec, sizeof t->codec, "V_MPEG4/ISO/ASP");
        } else {
            char id[16];
            snprintf(id, sizeof id, "V_MP4/%s", fc);
            m4_set(t->codec, sizeof t->codec, id);
        }
        if (m4_child(kids, "pasp", &c) && c.n >= 8) { /* the pixels' shape: a display size as Matroska says it */
            uint32_t h = m4_be32(c.p), v = m4_be32(c.p + 4);
            if (h && v && h != v && t->width > 0 && t->height > 0) {
                uint64_t w = (uint64_t)t->width * h / v;
                if (w > 0 && w < 65536) t->disp_width = (int)w, t->disp_height = t->height;
            }
        }
        return true;
    }
    if (sound && body.n >= 28) {
        t->type = MKV_AUDIO;
        uint32_t ver = m4_be16(body.p + 8);
        t->channels = (int)m4_be16(body.p + 16);
        t->bits = (int)m4_be16(body.p + 18);
        t->rate = m4_be32(body.p + 24) >> 16;
        size_t kid_at = 28;
        if (ver == 1) kid_at = 44;
        else if (ver == 2 && body.n >= 64) { /* QuickTime v2: the rate as a double, the channels as 32 bits */
            uint64_t bits = m4_be64(body.p + 32);
            double r;
            memcpy(&r, &bits, sizeof r);
            if (r > 0 && r < 1e6) t->rate = r;
            uint32_t ch = m4_be32(body.p + 40);
            if (ch && ch < 64) t->channels = (int)ch;
            kid_at = 64;
        }
        Span kids = {body.p + (kid_at < body.n ? kid_at : body.n), kid_at < body.n ? body.n - kid_at : 0}, c, wave;
        const char *codec = NULL;
        if (!strcmp(fc, "mp4a")) {
            int oti = 0;
            const uint8_t *dsi = NULL;
            size_t dl = 0;
            if ((m4_child(kids, "esds", &c) || (m4_child(kids, "wave", &wave) && m4_child(wave, "esds", &c))) &&
                m4_esds(c, &oti, &dsi, &dl)) {
                if (oti == 0x40 || (oti >= 0x66 && oti <= 0x68)) codec = "A_AAC";
                else if (oti == 0x6B || oti == 0x69) codec = "A_MPEG/L3";
                else if (oti == 0xA5) codec = "A_AC3";
                else if (oti == 0xA6) codec = "A_EAC3";
                else if (oti == 0xA9) codec = "A_DTS";
                if (codec && !strcmp(codec, "A_AAC") && dl && !m4_priv(t, dsi, dl)) return false;
            }
            if (!codec) codec = "A_MP4/mp4a";
        } else if (!strcmp(fc, ".mp3")) codec = "A_MPEG/L3";
        else if (!strcmp(fc, "ac-3")) codec = "A_AC3";
        else if (!strcmp(fc, "ec-3")) codec = "A_EAC3";
        else if (!strcmp(fc, "dtsc") || !strcmp(fc, "dtsh") || !strcmp(fc, "dtsl") || !strcmp(fc, "dtse")) codec = "A_DTS";
        else if (!strcmp(fc, "alac")) codec = "A_ALAC";
        else if (!strcmp(fc, "fLaC")) codec = "A_FLAC";
        else if (!strcmp(fc, "Opus")) { /* (2.5: its dOps as the OpusHead Matroska keeps - RFC 7845, little-endian) */
            codec = "A_OPUS";
            if (m4_child(kids, "dOps", &c) && c.n >= 11 && c.p[0] == 0) {
                uint8_t h[32];
                size_t ch = c.p[1], n = c.p[10] && c.n >= 13 + ch && ch <= 8 ? 21 + ch : 19;
                memcpy(h, "OpusHead", 8);
                h[8] = 1, h[9] = (uint8_t)ch;
                h[10] = c.p[3], h[11] = c.p[2];                               /* pre-skip */
                h[12] = c.p[7], h[13] = c.p[6], h[14] = c.p[5], h[15] = c.p[4]; /* input rate */
                h[16] = c.p[9], h[17] = c.p[8], h[18] = c.p[10];              /* gain, mapping family */
                if (n > 19) h[19] = c.p[11], h[20] = c.p[12], memcpy(h + 21, c.p + 13, ch);
                if (!m4_priv(t, h, n)) return false;
            }
        }
        else if (!strcmp(fc, "lpcm") || !strcmp(fc, "sowt") || !strcmp(fc, "twos") || !strcmp(fc, "in24")) codec = "A_PCM/INT/LIT";
        char id[16];
        if (!codec) snprintf(id, sizeof id, "A_MP4/%s", fc), codec = id;
        m4_set(t->codec, sizeof t->codec, codec);
        return true;
    }
    if (!strcmp(fc, "tx3g") && handler != 0x74657874u /* text: QuickTime's chapters */) {
        t->type = MKV_SUBTITLE;
        m4_set(t->codec, sizeof t->codec, "S_TEXT/UTF8");
        *tx3g = true;
    }
    return true; /* (anything else: no track to us) */
}

/* The trak's sample table -> its samples. false = out of memory (a damaged table just ends the track early). */
static bool m4_samples(Mkv *m, Mp4Trk *k, Span stbl) {
    Span stsz, stco, stsc, stts, ctts, stss;
    bool z2 = false, c64 = false;
    if (!m4_child(stbl, "stsz", &stsz) && !(z2 = m4_child(stbl, "stz2", &stsz))) return true;
    if (!m4_child(stbl, "stco", &stco) && !(c64 = m4_child(stbl, "co64", &stco))) return true;
    if (!m4_child(stbl, "stsc", &stsc) || !m4_child(stbl, "stts", &stts)) return true;
    bool has_ctts = m4_child(stbl, "ctts", &ctts), has_stss = m4_child(stbl, "stss", &stss);
    if (stsz.n < 12 || stco.n < 8 || stsc.n < 8 || stts.n < 8) return true;
    uint32_t fixed = z2 ? 0 : m4_be32(stsz.p + 4), count = m4_be32(stsz.p + 8), field = z2 ? stsz.p[7] : 32;
    if (z2 && field != 4 && field != 8 && field != 16) return true;
    if (!fixed && (uint64_t)count * field / 8 > stsz.n - 12) count = (uint32_t)((uint64_t)(stsz.n - 12) * 8 / field);
    if (count > MP4_MAX_SAMPLES) count = MP4_MAX_SAMPLES;
    if ((int64_t)count > m->r.size) count = (uint32_t)m->r.size; /* (each sample at least a byte) */
    if (!count) return true;
    Mp4Sample *s = calloc(count, sizeof *s);
    if (!s) return false;
    k->s = s;
    /* sizes */
    for (uint32_t i = 0; i < count; i++) {
        if (fixed) s[i].size = fixed;
        else if (field == 32) s[i].size = m4_be32(stsz.p + 12 + (size_t)i * 4);
        else if (field == 16) s[i].size = m4_be16(stsz.p + 12 + (size_t)i * 2);
        else if (field == 8) s[i].size = stsz.p[12 + i];
        else s[i].size = (uint32_t)(stsz.p[12 + i / 2] >> (i % 2 ? 0 : 4) & 15);
    }
    /* offsets: chunks by stsc's runs */
    uint32_t nchunks = m4_be32(stco.p + 4), nrun = m4_be32(stsc.p + 4);
    size_t esz = c64 ? 8 : 4;
    if ((uint64_t)nchunks * esz > stco.n - 8) nchunks = (uint32_t)((stco.n - 8) / esz);
    if ((uint64_t)nrun * 12 > stsc.n - 8) nrun = (uint32_t)((stsc.n - 8) / 12);
    uint32_t got = 0;
    for (uint32_t r = 0; r < nrun && got < count; r++) {
        const uint8_t *e = stsc.p + 8 + (size_t)r * 12;
        uint32_t first = m4_be32(e), spc = m4_be32(e + 4);
        uint32_t last = r + 1 < nrun ? m4_be32(e + 12) - 1 : nchunks;
        if (!first || first > nchunks || last > nchunks || !spc || spc > count) break;
        for (uint32_t ch = first; ch <= last && got < count; ch++) {
            const uint8_t *o = stco.p + 8 + (size_t)(ch - 1) * esz;
            uint64_t off = c64 ? m4_be64(o) : m4_be32(o);
            for (uint32_t j = 0; j < spc && got < count; j++) {
                if (off > (uint64_t)m->r.size || s[got].size > (uint64_t)m->r.size - off || s[got].size > MP4_MAX_FRAME) {
                    count = got; /* (past the file's end: a cut file - the track ends here) */
                    break;
                }
                s[got].off = (int64_t)off;
                off += s[got].size;
                got++;
            }
        }
    }
    if (got < count) count = got;
    /* decode times */
    uint32_t nt = m4_be32(stts.p + 4), i = 0;
    if ((uint64_t)nt * 8 > stts.n - 8) nt = (uint32_t)((stts.n - 8) / 8);
    int64_t dts = 0;
    uint32_t delta = 0;
    for (uint32_t r = 0; r < nt && i < count; r++) {
        uint32_t c = m4_be32(stts.p + 8 + (size_t)r * 8);
        delta = m4_be32(stts.p + 12 + (size_t)r * 8);
        for (uint32_t j = 0; j < c && i < count; j++, i++) {
            s[i].dts = dts;
            if (dts < ((int64_t)1 << 52)) dts += delta;
        }
    }
    for (; i < count; i++) s[i].dts = dts, dts += delta; /* (fewer times than samples: the last length goes on) */
    k->last = delta;
    /* composition offsets */
    if (has_ctts && ctts.n >= 8) {
        uint32_t nc = m4_be32(ctts.p + 4);
        if ((uint64_t)nc * 8 > ctts.n - 8) nc = (uint32_t)((ctts.n - 8) / 8);
        i = 0;
        for (uint32_t r = 0; r < nc && i < count; r++) {
            uint32_t c = m4_be32(ctts.p + 8 + (size_t)r * 8);
            int32_t off = (int32_t)m4_be32(ctts.p + 12 + (size_t)r * 8); /* (v0's are meant unsigned, never that big) */
            for (uint32_t j = 0; j < c && i < count; j++, i++) s[i].cto = off;
        }
    }
    /* keyframes: none listed = every one */
    if (has_stss && stss.n >= 8) {
        uint32_t nk = m4_be32(stss.p + 4);
        if ((uint64_t)nk * 4 > stss.n - 8) nk = (uint32_t)((stss.n - 8) / 4);
        for (uint32_t r = 0; r < nk; r++) {
            uint32_t at = m4_be32(stss.p + 8 + (size_t)r * 4);
            if (at && at <= count) s[at - 1].key = 1;
        }
    } else {
        for (i = 0; i < count; i++) s[i].key = 1;
    }
    k->n = count;
    return true;
}

static const char *m4_trak(Mkv *m, Span trak, uint32_t movie_ts, const uint32_t *chap, int nchap) {
    Span tkhd, mdhd, hdlr, stbl, stsd, mdia;
    if (!m4_child(trak, "tkhd", &tkhd) || tkhd.n < 24 || !m4_child(trak, "mdia", &mdia) ||
        !m4_child(mdia, "mdhd", &mdhd) || mdhd.n < 24 || !m4_child(mdia, "hdlr", &hdlr) || hdlr.n < 12 ||
        !m4_path(mdia, "minf/stbl", &stbl) || !m4_child(stbl, "stsd", &stsd))
        return NULL; /* (a trak without what we need: not a track to us) */
    bool v1 = tkhd.p[0] == 1;
    uint32_t id = m4_be32(tkhd.p + (v1 ? 20 : 12)), flags = m4_be32(tkhd.p) & 0xFFFFFF;
    for (int i = 0; i < nchap; i++)
        if (chap[i] == id) return NULL; /* a chapter track */
    bool mv1 = mdhd.p[0] == 1;
    if (mv1 && mdhd.n < 36) return NULL;
    uint32_t ts = m4_be32(mdhd.p + (mv1 ? 20 : 12)), lang = m4_be16(mdhd.p + (mv1 ? 32 : 20));
    if (!ts || m->ntracks >= MKV_MAX_TRACKS) return NULL;
    uint32_t handler = m4_be32(hdlr.p + 8);
    MkvTrack *t = &m->tracks[m->ntracks];
    memset(t, 0, sizeof *t);
    t->number = m->ntracks + 1;
    t->uid = id;
    t->enabled = t->is_default = (flags & 1) != 0;
    char l[4] = "und";
    if (lang && lang != 0x7FFF && !(lang & 0x8000)) {
        for (int i = 0; i < 3; i++) l[i] = (char)(((lang >> (10 - 5 * i)) & 31) + 0x60);
        for (int i = 0; i < 3; i++)
            if (l[i] < 'a' || l[i] > 'z') memcpy(l, "und", 4);
    }
    m4_set(t->lang, sizeof t->lang, l);
    bool tx3g = false;
    if (!m4_entry(t, stsd, handler, &tx3g)) return "out of memory";
    if (!t->type) { /* (chapters, timecodes, hints ...) */
        free((void *)t->priv);
        memset(t, 0, sizeof *t);
        return NULL;
    }
    Span udta, nm;
    if (m4_child(trak, "udta", &udta) && m4_child(udta, "name", &nm)) m4_name(t, nm.p, nm.n);
    else if (hdlr.n > 24) m4_name(t, hdlr.p + 24, hdlr.n - 24);
    Mp4Trk *k = &m->mp4->t[m->mp4->nt];
    memset(k, 0, sizeof *k);
    k->idx = m->ntracks;
    k->timescale = ts;
    k->tx3g = tx3g;
    Span edts, elst;
    if (m4_child(trak, "edts", &edts) && m4_child(edts, "elst", &elst) && elst.n >= 8) {
        bool e1 = elst.p[0] == 1;
        size_t esz = e1 ? 20 : 12;
        uint32_t ne = m4_be32(elst.p + 4);
        for (uint32_t i = 0; i < ne && 8 + (i + 1) * esz <= elst.n; i++) {
            const uint8_t *e = elst.p + 8 + i * esz;
            int64_t dur = e1 ? (int64_t)(m4_be64(e) & 0x7FFFFFFFFFFFFFFFull) : (int64_t)m4_be32(e);
            int64_t mt = e1 ? (int64_t)m4_be64(e + 8) : (int64_t)(int32_t)m4_be32(e + 4);
            if (mt == -1) { /* an empty edit: nothing shown for this long first (at most 2^50 ns, ~13 days, in all) */
                int64_t d = movie_ts ? m4_ns(dur, movie_ts) : 0, cap = (int64_t)1 << 50;
                k->delay_ns = d > cap - k->delay_ns ? cap : k->delay_ns + d;
                continue;
            }
            if (mt > 0 && mt < ((int64_t)1 << 50)) k->shift = mt;
            break;
        }
    }
    if (!m4_samples(m, k, stbl)) return "out of memory";
    if (k->n > 1 && k->s[1].dts > k->s[0].dts) t->default_duration = m4_ns(k->s[1].dts - k->s[0].dts, ts);
    m->ntracks++;
    m->mp4->nt++;
    return NULL;
}

static bool mp4_looks(Mkv *m) {
    uint8_t h[8];
    if (m->r.size < 8 || !rd(m, 0, h, 8)) return false;
    static const char *const FIRST[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "pnot", "styp"};
    for (size_t i = 0; i < sizeof FIRST / sizeof FIRST[0]; i++)
        if (!memcmp(h + 4, FIRST[i], 4)) {
            uint32_t size = m4_be32(h);
            return size == 0 || size == 1 || (size >= 8 && size <= (uint64_t)m->r.size);
        }
    return false;
}

/* The file's top-level boxes: the moov read whole, then every trak. false + err */
static bool mp4_open(Mkv *m, char *err, size_t errcap) {
    memcpy(m->doctype, "mp4", 4);
    m->mp4 = calloc(1, sizeof *m->mp4);
    if (!m->mp4) return fail(err, errcap, "out of memory"), false;
    int64_t pos = 0, moov_at = -1, moov_len = 0;
    bool moof = false, quicktime = true;
    for (int k = 0; k < MP4_MAX_TOP && pos + 8 <= m->r.size; k++) {
        uint8_t h[16];
        if (!rd(m, pos, h, 8)) return fail(err, errcap, "read error"), false;
        uint64_t size = m4_be32(h);
        int64_t head = 8;
        if (size == 1) {
            if (pos + 16 > m->r.size || !rd(m, pos, h, 16)) break;
            size = m4_be64(h + 8);
            head = 16;
        } else if (size == 0) size = (uint64_t)(m->r.size - pos);
        if (size < (uint64_t)head || size > (uint64_t)(m->r.size - pos)) break; /* (a cut file: what's there) */
        if (!memcmp(h + 4, "ftyp", 4)) {
            uint8_t brand[4];
            if (size >= 12 && rd(m, pos + head, brand, 4)) quicktime = !memcmp(brand, "qt  ", 4);
        } else if (!memcmp(h + 4, "moov", 4) && moov_at < 0) {
            moov_at = pos + head, moov_len = (int64_t)size - head;
        } else if (!memcmp(h + 4, "moof", 4)) moof = true;
        pos += (int64_t)size;
    }
    if (quicktime) memcpy(m->doctype, "mov", 4);
    if (moov_at < 0) return fail(err, errcap, "no moov (the MP4's index)"), false;
    if (moof) return fail(err, errcap, "a fragmented MP4 - not yet"), false;
    if (moov_len > MP4_MOOV_CAP) return fail(err, errcap, "its index (moov) is far too big"), false;
    uint8_t *moov = malloc((size_t)moov_len + 1);
    if (!moov) return fail(err, errcap, "out of memory"), false;
    if (!rd(m, moov_at, moov, (size_t)moov_len)) {
        free(moov);
        return fail(err, errcap, "read error"), false;
    }
    Span mv = {moov, (size_t)moov_len}, b;
    if (m4_child(mv, "mvex", &b)) {
        free(moov);
        return fail(err, errcap, "a fragmented MP4 - not yet"), false;
    }
    uint32_t movie_ts = 0;
    if (m4_child(mv, "mvhd", &b) && b.n >= 20) {
        bool v1 = b.p[0] == 1;
        if (!v1 || b.n >= 32) {
            movie_ts = m4_be32(b.p + (v1 ? 20 : 12));
            uint64_t d = v1 ? m4_be64(b.p + 24) : m4_be32(b.p + 16);
            if (movie_ts && d < ((uint64_t)1 << 50)) m->duration = (double)m4_ns((int64_t)d, movie_ts);
        }
    }
    m->scale = 1; /* (mkv_duration: duration x scale, in ns) */
    /* chapter tracks (tref 'chap' of any trak): not tracks to us */
    uint32_t chap[MKV_MAX_TRACKS];
    int nchap = 0;
    Span all = mv, trak;
    char ty[5];
    for (int k = 0; k < 4096 && m4_box(&all, ty, &trak); k++) {
        Span tref, c;
        if (strcmp(ty, "trak") || !m4_child(trak, "tref", &tref) || !m4_child(tref, "chap", &c)) continue;
        for (size_t i = 0; i + 4 <= c.n && nchap < MKV_MAX_TRACKS; i += 4) chap[nchap++] = m4_be32(c.p + i);
    }
    all = mv;
    const char *bad = NULL;
    for (int k = 0; k < 4096 && !bad && m4_box(&all, ty, &trak); k++)
        if (!strcmp(ty, "trak")) bad = m4_trak(m, trak, movie_ts, chap, nchap);
    free(moov);
    if (bad) return fail(err, errcap, bad), false;
    if (!m->ntracks) return fail(err, errcap, "no picture or sound tracks in it"), false;
    if (m->duration <= 0) /* (no movie length said: the longest track's) */
        for (int i = 0; i < m->mp4->nt; i++) {
            const Mp4Trk *k = &m->mp4->t[i];
            if (!k->n) continue;
            const Mp4Sample *l = &k->s[k->n - 1];
            double d = (double)m4_ns(l->dts + l->cto - k->shift, k->timescale) + (double)k->delay_ns;
            if (d > m->duration) m->duration = d;
        }
    return true;
}

static void mp4_free(struct Mp4r *p) {
    if (!p) return;
    for (int i = 0; i < p->nt; i++) free(p->t[i].s);
    free(p->buf);
    free(p);
}

static int64_t m4_pts(const Mp4Trk *k, const Mp4Sample *s) {
    return m4_ns(s->dts + s->cto - k->shift, k->timescale) + k->delay_ns;
}

static int mp4_next(Mkv *m, MkvFrame *f) {
    struct Mp4r *p = m->mp4;
    int best = -1;
    int64_t at = INT64_MAX;
    for (int i = 0; i < p->nt; i++) {
        const Mp4Trk *k = &p->t[i];
        if (k->at < k->n && k->s[k->at].off < at) at = k->s[k->at].off, best = i;
    }
    if (best < 0) return 0;
    Mp4Trk *k = &p->t[best];
    const Mp4Sample *s = &k->s[k->at++];
    if (!p->buf || s->size > p->cap) {
        size_t cap = s->size > 65536 ? s->size : 65536;
        uint8_t *b = realloc(p->buf, cap);
        if (!b) return -1;
        p->buf = b, p->cap = cap;
    }
    if (!rd(m, s->off, p->buf, s->size)) return -1;
    memset(f, 0, sizeof *f);
    f->track = m->tracks[k->idx].number;
    f->pts = m4_pts(k, s);
    f->duration = k->at < k->n && k->s[k->at].dts > s->dts ? m4_ns(k->s[k->at].dts - s->dts, k->timescale)
                                                             : m4_ns(k->last, k->timescale);
    f->key = s->key != 0;
    f->data = p->buf;
    f->size = s->size;
    f->pos = s->off;
    if (k->tx3g) { /* the text alone: its 16-bit length first, style boxes after */
        size_t len = s->size >= 2 ? m4_be16(p->buf) : 0;
        if (s->size < 2 || len > (size_t)s->size - 2) len = s->size >= 2 ? (size_t)s->size - 2 : 0;
        f->data = p->buf + 2; /* (the buffer is at least 64 KB) */
        f->size = len;
    }
    return 1;
}

static void mp4_rewind(Mkv *m) {
    for (int i = 0; i < m->mp4->nt; i++) m->mp4->t[i].at = 0;
}

/* That track's keyframe at or before t, then every track on from its place in the file */
static bool mp4_seek(Mkv *m, int64_t t_ns, int track) {
    struct Mp4r *p = m->mp4;
    const Mp4Trk *k = NULL;
    for (int i = 0; i < p->nt && !k; i++) {
        const MkvTrack *t = &m->tracks[p->t[i].idx];
        if (track ? t->number == track : t->type == MKV_VIDEO) k = &p->t[i];
    }
    if (!k || !k->n) {
        mp4_rewind(m);
        return k != NULL;
    }
    uint32_t pick = 0;
    for (uint32_t i = 0; i < k->n; i++) {
        if (m4_pts(k, &k->s[i]) > t_ns && i > 0) break;
        if (k->s[i].key) pick = i;
    }
    int64_t from = k->s[pick].off;
    for (int i = 0; i < p->nt; i++) {
        Mp4Trk *o = &p->t[i];
        o->at = 0;
        while (o->at < o->n && o->s[o->at].off < from) o->at++;
    }
    return true;
}
