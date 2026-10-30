/* modules/media/download.c — taking videos home (see download.h): who may, what things are called, what a
 * season's zip holds and how big it is, and the buttons. The sending is stream.c's. */
#include "download.h"
#include "media.h"
#include "room.h"
#include "zip.h"

bool media_grown_up(const Ctx *c) { return c->me && (c->me->role == ROLE_MEMBER || c->me->role == ROLE_ADMIN); }

/* ------------------------------------------------------------------ names */

void media_safe_name(const char *in, char *out, size_t cap) {
    Buf b = {0};
    char last = ' '; /* (as if a space came first: no leading " -") */
    for (const char *p = in ? in : ""; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20 || ch == 0x7F) continue;
        switch (ch) {
        case ':': /* "Star Wars: A New Hope" -> "Star Wars - A New Hope", "10:30" -> "10-30" */
            buf_puts(&b, p[1] == ' ' && last != ' ' ? " -" : "-");
            break;
        case '/': case '\\': case '|': buf_putc(&b, '-'); break;
        case '"': buf_putc(&b, '\''); break;
        case '*': case '?': case '<': case '>': break;
        default: buf_putc(&b, (char)ch);
        }
        if (b.len) last = b.p[b.len - 1];
    }
    const char *s = buf_cstr(&b);
    while (*s == ' ' || *s == '.') s++; /* (a leading dot hides a file) */
    str_copy(out, cap, s);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = '\0'; /* (Windows drops them) */
    if (!n) str_copy(out, cap, "video");
    /* Windows' device names ("Con", "nul.x", "COM1") can't be a file or a folder: "Con_" */
    static const char *const DEVICES[] = {"con", "prn", "aux", "nul", "com", "lpt"};
    size_t k = strcspn(out, ".");
    for (int i = 0; i < countof(DEVICES) && (k == 3 || k == 4); i++) {
        bool hit = k == 3 ? i < 4 : i >= 4 && out[3] >= '1' && out[3] <= '9'; /* (COM1-9, LPT1-9) */
        if (hit && str_istarts(out, DEVICES[i]) && strlen(out) + 2 <= cap) {
            memmove(out + k + 1, out + k, strlen(out + k) + 1);
            out[k] = '_';
            break;
        }
    }
    buf_free(&b);
}

static const char *base_name(const char *rel) {
    const char *s = strrchr(rel, '/');
    return s ? s + 1 : rel;
}

/* The extension of what goes out: ".mp4" for a copy ("x.web.mp4"), else the file's own. */
static const char *ext_of(const MediaFile *pf) {
    if (media_is_web_copy(pf->rel)) return ".mp4";
    const char *b = base_name(pf->rel), *dot = strrchr(b, '.');
    return dot && dot != b ? dot : "";
}

/* Its own name without the extension ("x" of "Show/x.mkv" and "Show/x.web.mp4"). */
static void own_stem(const MediaFile *f, char *out, size_t cap) {
    const char *b = base_name(f->rel);
    size_t stem = media_stem_len(f->rel), at = (size_t)(b - f->rel);
    str_ncopy(out, cap, b, stem > at ? stem - at : strlen(b));
}

/* The name without its extension: "Show S01E03-E04 - Title", "Film (2020)", or its own. */
static void download_stem(const MediaFile *f, char *out, size_t cap) {
    char raw[600];
    const MediaName *nm = &f->name;
    if (f->type == MF_VIDEO && f->role == MF_MAIN && nm->kind == MEDIA_EPISODE && nm->episode > 0) {
        const MediaSeries *s = media_series(f->series);
        int k = snprintf(raw, sizeof raw, "%s S%02dE%02d", s ? s->title : nm->title, nm->season, nm->episode);
        if (k > 0 && (size_t)k < sizeof raw && nm->episode_end > nm->episode)
            k += snprintf(raw + k, sizeof raw - (size_t)k, "-E%02d", nm->episode_end);
        if (k > 0 && (size_t)k < sizeof raw && nm->ep_title[0]) snprintf(raw + k, sizeof raw - (size_t)k, " - %s", nm->ep_title);
    } else if (f->type == MF_VIDEO && f->role == MF_MAIN && nm->kind == MEDIA_MOVIE && nm->title[0]) {
        int k = nm->year ? snprintf(raw, sizeof raw, "%s (%d)", nm->title, nm->year) : snprintf(raw, sizeof raw, "%s", nm->title);
        int part = media_name_part(f->rel);
        if (part && k > 0 && (size_t)k < sizeof raw) snprintf(raw + k, sizeof raw - (size_t)k, " - Part %d", part);
    } else {
        own_stem(f, raw, sizeof raw);
    }
    media_safe_name(raw, out, cap);
}

void media_download_name(const MediaFile *f, char *out, size_t cap) {
    char stem[400];
    download_stem(f, stem, sizeof stem);
    snprintf(out, cap, "%s%s", stem, ext_of(media_playable(f)));
}

void media_content_disposition(Buf *out, const char *name) {
    /* À..ÿ as plain letters for the old-fashioned filename="..." (every browser of today reads filename*) */
    static const char LATIN1[] = "AAAAAAAC" "EEEEIIII" "DNOOOOOx" "OUUUUYTs" "aaaaaaac" "eeeeiiii" "dnooooo-" "ouuuuyty";
    static const char HEX[] = "0123456789ABCDEF";
    buf_puts(out, "Content-Disposition: attachment; filename=\"");
    for (size_t i = 0, n = strlen(name); i < n;) {
        uint32_t cp = 0;
        int k = utf8_decode(name + i, n - i, &cp);
        i += (size_t)(k > 0 ? k : 1);
        if (cp >= 0x20 && cp < 0x7F && cp != '"' && cp != '\\' && cp != '%') buf_putc(out, (char)cp);
        else if (cp >= 0xC0 && cp <= 0xFF) buf_putc(out, LATIN1[cp - 0xC0]);
        else buf_putc(out, '_');
    }
    buf_puts(out, "\"; filename*=UTF-8''");
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
                     *p == '.' || *p == '_' || *p == '~';
        if (plain) buf_putc(out, (char)*p);
        else {
            buf_putc(out, '%');
            buf_putc(out, HEX[*p >> 4]);
            buf_putc(out, HEX[*p & 15]);
        }
    }
    buf_puts(out, "\r\n");
}

/* ------------------------------------------------------------------- zips */

static uint64_t name_key(const char *name) { /* the same name to a file system that ignores case */
    char low[MEDIA_ZIP_NAME];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof low; i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = '\0';
    return hash64_str(low);
}

typedef struct {
    MediaZipItem *items;
    uint64_t *keys;
    int n, cap;
} ZipList;

static bool taken(const ZipList *l, uint64_t key) {
    for (int i = 0; i < l->n; i++)
        if (l->keys[i] == key) return true;
    return false;
}

static void add_item(ZipList *l, const MediaFile *f, const char *name, uint64_t key) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = xrealloc(l->items, (size_t)l->cap * sizeof *l->items);
        l->keys = xrealloc(l->keys, (size_t)l->cap * sizeof *l->keys);
    }
    MediaZipItem *it = &l->items[l->n];
    it->file = f->id;
    it->size = f->size;
    str_copy(it->name, sizeof it->name, name);
    l->keys[l->n++] = key;
}

/* What follows the episode's name in a subtitle's: ".sv.forced.srt" of "Show.S01E01.sv.forced.srt" next to
   "Show.S01E01.mkv" (its tags joined by dots), else ".<language>.srt", else ".srt". */
static void sub_suffix(const MediaFile *video, const MediaFile *sub, char *out, size_t cap) {
    char vstem[400], sstem[400], tags[64] = "";
    own_stem(video, vstem, sizeof vstem);
    const char *sb = base_name(sub->rel), *dot = strrchr(sb, '.');
    const char *ext = dot && dot != sb ? dot : ".srt";
    str_ncopy(sstem, sizeof sstem, sb, dot && dot != sb ? (size_t)(dot - sb) : strlen(sb));
    if (str_istarts(sstem, vstem)) {
        Buf t = {0};
        for (const char *p = sstem + strlen(vstem); *p;) {
            size_t w = strcspn(p, "._- ");
            if (w) {
                buf_putc(&t, '.');
                buf_put(&t, p, w);
            }
            p += w;
            if (*p) p++;
        }
        if (t.len < sizeof tags) str_copy(tags, sizeof tags, buf_cstr(&t));
        else if (sub->lang[0]) snprintf(tags, sizeof tags, ".%s", sub->lang);
        buf_free(&t);
    } else if (sub->lang[0]) {
        snprintf(tags, sizeof tags, ".%s", sub->lang);
    }
    char raw[100];
    snprintf(raw, sizeof raw, "%s%s", tags, ext);
    /* (safe as a name, but its leading dot stays: it follows the episode's name) */
    Buf s = {0};
    for (const char *p = raw; *p; p++)
        if (!strchr("\\/:*?\"<>|", *p) && (unsigned char)*p >= 0x20) buf_putc(&s, *p);
    str_copy(out, cap, buf_cstr(&s));
    buf_free(&s);
}

/* A DVD VobSub .idx's pictures: the .sub of the same name beside it (library.c indexes one only so) */
static const MediaFile *vobsub_data(const MediaIndex *ix, const int *subs, int nsubs, const MediaFile *idx) {
    size_t stem = strlen(idx->rel) - 4;
    for (int j = 0; j < nsubs; j++) {
        const MediaFile *s = &ix->files[subs[j]];
        if (s->root == idx->root && media_is_vobsub_data(s->rel) && strlen(s->rel) == stem + 4 &&
            strncmp(s->rel, idx->rel, stem) == 0)
            return s;
    }
    return NULL;
}

MediaZipItem *media_zip_items(const MediaSeries *s, int season, int *n) {
    *n = 0;
    if (!s) return NULL;
    const MediaIndex *ix = media_lib();
    int *subs = xmalloc((size_t)(ix->nfiles ? ix->nfiles : 1) * sizeof *subs), nsubs = 0;
    for (int i = 0; i < ix->nfiles; i++)
        if (ix->files[i].type == MF_SUB && ix->files[i].video) subs[nsubs++] = i;
    char show[200];
    media_safe_name(s->title, show, sizeof show);
    ZipList l = {0};
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        if (!e || e->type != MF_VIDEO || e->role != MF_MAIN || (season >= 0 && e->name.season != season)) continue;
        const MediaFile *pf = media_playable(e);
        char folder[260], stem[400], name[MEDIA_ZIP_NAME];
        if (e->name.season == 0) snprintf(folder, sizeof folder, "%s/Specials", show);
        else snprintf(folder, sizeof folder, "%s/Season %d", show, e->name.season);
        download_stem(e, stem, sizeof stem);
        /* two files that would be called the same ("S01E01" twice, in two qualities): "(2)" for the second */
        char unique[420];
        str_copy(unique, sizeof unique, stem);
        snprintf(name, sizeof name, "%s/%s%s", folder, unique, ext_of(pf));
        for (int k = 2; taken(&l, name_key(name)) && k < 1000; k++) {
            snprintf(unique, sizeof unique, "%s (%d)", stem, k);
            snprintf(name, sizeof name, "%s/%s%s", folder, unique, ext_of(pf));
        }
        add_item(&l, pf, name, name_key(name));
        for (int j = 0; j < nsubs; j++) {
            const MediaFile *sub = &ix->files[subs[j]];
            if (sub->video != e->id && sub->video != pf->id) continue;
            if (media_is_vobsub_data(sub->rel)) continue; /* (a VobSub's pictures go with its .idx, below) */
            char suffix[100];
            sub_suffix(e, sub, suffix, sizeof suffix);
            snprintf(name, sizeof name, "%s/%s%s", folder, unique, suffix);
            uint64_t key = name_key(name);
            if (taken(&l, key)) continue; /* ("x.en.srt" and "x.EN.srt": one is enough) */
            const MediaFile *pics = NULL;
            if (str_iends(sub->rel, ".idx") && (!str_iends(name, ".idx") || !(pics = vobsub_data(ix, subs, nsubs, sub))))
                continue; /* (an .idx is nothing without its pictures) */
            add_item(&l, sub, name, key);
            if (pics) { /* the pair, under one name: "<episode>.en.idx" + "<episode>.en.sub" */
                memcpy(name + strlen(name) - 3, "sub", 3);
                add_item(&l, pics, name, name_key(name));
            }
        }
    }
    free(subs);
    free(l.keys);
    *n = l.n;
    return l.items;
}

void media_zip_name(const MediaSeries *s, int season, char *out, size_t cap) {
    char show[200];
    media_safe_name(s->title, show, sizeof show);
    if (season < 0) snprintf(out, cap, "%s.zip", show);
    else if (season == 0) snprintf(out, cap, "%s - Specials.zip", show);
    else snprintf(out, cap, "%s - Season %d.zip", show, season);
}

uint64_t media_zip_bytes(const MediaSeries *s, int season) {
    int n = 0;
    MediaZipItem *it = media_zip_items(s, season, &n);
    ZipEntry *e = xcalloc((size_t)(n ? n : 1), sizeof *e);
    for (int i = 0; i < n; i++) e[i] = (ZipEntry){.name = it[i].name, .size = (uint64_t)MAX(it[i].size, 0)};
    uint64_t total = n ? zip_size(e, n) : 0;
    free(e);
    free(it);
    return total;
}

/* ---------------------------------------------------------------- buttons */

void media_download_link(Ctx *c, Buf *b, const MediaFile *f) {
    if (!media_grown_up(c) || media_tv_mode(c) || !f || f->type != MF_VIDEO) return;
    char size[24];
    media_size_text(MAX(media_playable(f)->size, 0), size, sizeof size);
    H(b, "<a class=\"btn\" id=\"media-dl\" href=\"/media/file/%X?download=1\" download data-native title=\"%s\">", f->id,
      T("Downloads slow down while someone is watching."));
    H(b, T("Download (%s)"), size);
    buf_puts(b, "</a>");
}

void media_download_series(Ctx *c, Buf *b, const MediaSeries *s, int season) {
    if (media_tv_mode(c) || !s) return;
    uint64_t all = media_grown_up(c) ? media_zip_bytes(s, -1) : 0;
    Buf save = {0};
    media_offline_season(c, &save, s, season); /* (in the app: "Save season for offline" - offline.c, 2.3; children too) */
    if (!all && !save.len) {
        buf_free(&save);
        return;
    }
    char size[24];
    buf_puts(b, "<div class=\"toolbar\" id=\"media-dl\">");
    uint64_t one = all && s->nseasons > 1 ? media_zip_bytes(s, season) : 0; /* (one season: "all" is that season) */
    if (one) {
        media_size_text((int64_t)one, size, sizeof size);
        H(b, "<a class=\"btn\" href=\"/media/zip/%X?s=%d\" download data-native>", s->id, season);
        if (season == 0) H(b, T("Download the specials (%s)"), size);
        else H(b, T("Download season %d (%s)"), season, size);
        buf_puts(b, "</a>");
    }
    if (all) {
        media_size_text((int64_t)all, size, sizeof size);
        H(b, "<a class=\"btn\" href=\"/media/zip/%X\" download data-native>", s->id);
        H(b, T("Download all (%s)"), size);
        buf_puts(b, "</a>");
    }
    buf_put(b, save.p, save.len);
    if (all) H(b, "<span class=\"muted small\">%s</span>", T("Downloads slow down while someone is watching."));
    buf_puts(b, "</div>");
    buf_free(&save);
}
