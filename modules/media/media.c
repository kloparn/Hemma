/* modules/media — the home cinema.
 *
 * Browse the household's movies, series and other videos from a local drive
 * and watch them in any browser on the network. The library is scanned by a
 * background thread (library.c), files stream with Range support from worker
 * threads (stream.c) and every person's progress is remembered, so "continue
 * watching" works across the phone, the laptop and the TV's browser. */
#include "../../core/env.h"
#include "../../core/ui.h"
#include "media.h"
#include "convert.h"
#include "remove.h"
#include "room.h"
#include "upload.h"
#include "health.h"
#include "drives.h"
#include "download.h"
#include "manage.h"
#include "subtake.h"

static int64_t g_disks_at; /* when the disks under the library were last measured (see "disk space") */

/* ---------------------------------------------------------------- helpers */

/* Languages: each named in itself (the menus), and in English (what a file's track names often say: "Swedish") */
static const struct {
    const char *code, *name, *english;
} LANGS[] = {{"sv", "Svenska", "Swedish"},       {"en", "English", "English"},      {"no", "Norsk", "Norwegian"},
             {"nb", "Norsk", "Norwegian"},       {"da", "Dansk", "Danish"},         {"fi", "Suomi", "Finnish"},
             {"de", "Deutsch", "German"},        {"fr", "Français", "French"},      {"es", "Español", "Spanish"},
             {"it", "Italiano", "Italian"},      {"nl", "Nederlands", "Dutch"},     {"pl", "Polski", "Polish"},
             {"pt", "Português", "Portuguese"},  {"ja", "日本語", "Japanese"},     {"is", "Íslenska", "Icelandic"},
             {"ru", "Русский", "Russian"},       {"uk", "Українська", "Ukrainian"}, {"cs", "Čeština", "Czech"},
             {"sk", "Slovenčina", "Slovak"},     {"hu", "Magyar", "Hungarian"},     {"ro", "Română", "Romanian"},
             {"et", "Eesti", "Estonian"},        {"lv", "Latviešu", "Latvian"},     {"lt", "Lietuvių", "Lithuanian"},
             {"hr", "Hrvatski", "Croatian"},     {"sr", "Српски", "Serbian"},      {"bg", "Български", "Bulgarian"},
             {"el", "Ελληνικά", "Greek"},        {"tr", "Türkçe", "Turkish"},       {"ar", "العربية", "Arabic"},
             {"he", "עברית", "Hebrew"},          {"hi", "हिन्दी", "Hindi"},          {"th", "ไทย", "Thai"},
             {"ko", "한국어", "Korean"},          {"zh", "中文", "Chinese"}};

static int lang_at(const char *code) {
    for (int i = 0; code && i < countof(LANGS); i++)
        if (str_ieq(code, LANGS[i].code)) return i;
    return -1;
}

static const char *lang_name(const char *code) {
    int i = lang_at(code);
    if (i >= 0) return LANGS[i].name;
    return code && *code ? code : T("Subtitles");
}

static void keyed_src(const Ctx *c, const char *path, char *out, size_t cap) {
    char k[CTX_KEY_LEN];
    ctx_url_key(c, path, k);
    snprintf(out, cap, "%s%s%s", path, k[0] ? "?k=" : "", k);
}

/* ---------------------------------------------------- sound languages (2.2) */

#define PREF_SOUNDLANG "media.soundlang" /* "" = not picked yet (asked), "-" = each video's own, "en", "sv" ... */

/* The pref keeping a viewer's last Language pick: "media.snd.<series id>" for an episode (the series goes on in it),
   "media.snd.<file id>" for a film or another video - its language. */
typedef struct {
    char k[32];
} SndMemory;

static SndMemory snd_memory_key(const MediaFile *f) {
    SndMemory m;
    snprintf(m.k, sizeof m.k, "media.snd.%016llx",
             (unsigned long long)(f->name.kind == MEDIA_EPISODE && f->series ? f->series : f->id));
    return m;
}

/* The sound tracks of the file a video streams (its copy when it has one): NULL = one or none */
static const MediaSounds *sounds_of(const MediaFile *f) {
    const MediaFile *pf = f ? media_playable(f) : NULL;
    return pf && pf->type == MF_VIDEO ? media_sounds(pf->id) : NULL;
}

static int sound_default(const MediaSounds *s) {
    for (int i = 0; i < s->n; i++)
        if (s->s[i].on) return i;
    return 0;
}

/* The first track in lang - the file's default when it's one of them; -1 = none */
static int sound_in(const MediaSounds *s, const char *lang) {
    if (!lang || strlen(lang) != 2) return -1;
    int d = sound_default(s);
    if (str_eq(s->s[d].lang, lang)) return d;
    for (int i = 0; i < s->n; i++)
        if (str_eq(s->s[i].lang, lang)) return i;
    return -1;
}

static int sound_pick(const Ctx *c, const MediaFile *f, const MediaSounds *s) {
    int v = ctx_viewer(c), k = -1;
    /* the menu's pick: "<lang>#<n>" - that very track when this file's n-th is in that language (the same film, or an
       episode alike), else the language's first */
    const char *mem = v ? pref(v, snd_memory_key(f).k, "") : "", *hash = strchr(mem, '#');
    char lang[8] = "";
    str_ncopy(lang, sizeof lang, mem, hash ? (size_t)(hash - mem) : strlen(mem));
    int n = hash ? atoi(hash + 1) : -1;
    if (n >= 0 && n < s->n && str_eq(s->s[n].lang, lang)) k = n;
    else k = sound_in(s, lang);
    if (v && k < 0) k = sound_in(s, pref(v, PREF_SOUNDLANG, ""));
    return k >= 0 ? k : sound_default(s);
}

/* "/media/file/<id>?a=<n>&k=..." (the key covers the path only: app.c reads k= among the rest) */
static void sound_src(const Ctx *c, uint64_t id, int n, char *out, size_t cap) {
    char path[40], k[CTX_KEY_LEN];
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)id);
    ctx_url_key(c, path, k);
    snprintf(out, cap, "%s?a=%d%s%s", path, n, k[0] ? "&k=" : "", k);
}

void media_video_key(const Ctx *c, const MediaFile *f, char *out, size_t cap) {
    const MediaSounds *s = sounds_of(f);
    if (s) snprintf(out, cap, "%016llx-a%d", (unsigned long long)f->id, sound_pick(c, f, s));
    else snprintf(out, cap, "%016llx", (unsigned long long)f->id);
}

/* The menu's words for track i: its language in itself, then its own name ("Русский · MVO, Fox, iTunes"); a second
   one of a language without a name "Русский 2"; no language: its name, else "Sound 3". */
static void sound_label(const MediaSounds *s, int i, char *out, size_t cap) {
    const MediaSound *t = &s->s[i];
    int nth = 1, l = lang_at(t->lang);
    for (int j = 0; j < i; j++) nth += str_eq(s->s[j].lang, t->lang);
    bool says = t->name[0] && !(l >= 0 && (str_ieq(t->name, LANGS[l].name) || str_ieq(t->name, LANGS[l].english)));
    if (!t->lang[0]) {
        if (t->name[0]) str_copy(out, cap, t->name);
        else snprintf(out, cap, T("Sound %d"), i + 1);
    } else if (says) snprintf(out, cap, "%s · %s", lang_name(t->lang), t->name); /* ("English" on an English one: no) */
    else if (nth > 1) snprintf(out, cap, "%s %d", lang_name(t->lang), nth);
    else str_copy(out, cap, lang_name(t->lang));
}

bool media_sound_question(const Ctx *c, Buf *b, const MediaFile *f, const char *close) {
    const MediaSounds *s = sounds_of(f);
    int v = ctx_viewer(c);
    if (!s || !v || *pref(v, PREF_SOUNDLANG, "") || str_eq(ctx_q(c, "asked"), "1")) return false;
    const char *langs[MEDIA_MAX_SOUNDS];
    int n = 0;
    for (int i = 0; i < s->n; i++) {
        bool seen = !s->s[i].lang[0];
        for (int j = 0; j < n && !seen; j++) seen = str_eq(langs[j], s->s[i].lang);
        if (!seen) langs[n++] = s->s[i].lang;
    }
    if (n < 2) return false; /* (one language in several versions: the Language menu is enough) */
    ui_modal(b, T("Which language do you want to watch in?"), close);
    H(b, "<p class=\"muted\">%s</p><div class=\"media-snd-q\" data-arrows>",
      T("This video has sound in several languages. Hemma remembers your choice for every video - the player's "
        "Language menu changes it for one."));
    for (int i = 0; i < n; i++) {
        ui_form(b, "media.soundlang", "inline");
        H(b, "<input type=\"hidden\" name=\"lang\" value=\"%s\"><button class=\"btn%r\"%r>%s</button>", langs[i],
          i == 0 ? " primary" : "", i == 0 ? " autofocus" : "", lang_name(langs[i]));
        ui_form_end(b);
    }
    ui_form(b, "media.soundlang", "inline");
    H(b, "<input type=\"hidden\" name=\"lang\" value=\"-\"><button class=\"btn\">%s</button>", T("As each video comes"));
    ui_form_end(b);
    buf_puts(b, "</div>");
    ui_modal_end(b);
    return true;
}

void media_file_src(const Ctx *c, uint64_t id, char *out, size_t cap) {
    const MediaFile *f = media_file(id);
    const MediaSounds *s = sounds_of(f);
    if (s) {
        sound_src(c, id, sound_pick(c, f, s), out, cap);
        return;
    }
    char path[40];
    snprintf(path, sizeof path, "/media/file/%016llx", (unsigned long long)id);
    keyed_src(c, path, out, cap);
}

void media_file_src_at(const Ctx *c, uint64_t id, int64_t at, char *out, size_t cap) {
    media_file_src(c, id, out, cap);
    size_t n = strlen(out);
    if (at > 0 && n < cap) snprintf(out + n, cap - n, "#t=%lld", (long long)at);
}

int media_resume_at(const MediaWatch *w) {
    if (!w || w->done || w->pos <= 10 || (w->dur > 0 && w->pos >= w->dur - 5)) return 0;
    return w->pos;
}

void media_sub_src(const Ctx *c, uint64_t id, char *out, size_t cap) {
    char path[40];
    snprintf(path, sizeof path, "/media/sub/%016llx.vtt", (unsigned long long)id);
    keyed_src(c, path, out, cap);
}

/* ------------------------------------------------------------- subtitles */

#define MEDIA_MAX_SUBS 48 /* subtitles one video offers (a file's 13 tracks + the household's own) */

/* One of a video's subtitles as the player offers it */
typedef struct {
    const MediaFile *f;
    MediaSubTags t;
    char key[40];    /* unique among the video's: "en", "en.sdh", "en.2" (media_sub_key) */
    char label[160]; /* in the reader's words: "English", "English (for the hard of hearing)", "English 2" */
    bool named;      /* its name is the one Hemma gives such a subtitle ("x.en.2.srt"): it keeps its key */
    bool on;         /* shown from the start: the viewer's pick in this series, else their language */
    bool pref;       /* what the C key turns on when none shows (the same, even when the pick was "off") */
    bool pic;        /* pictures (.sup, a VobSub's .idx): drawn by the player from its cue list, no <track> */
    char cues[240];  /* ... that list's address (C5: keyed, ?v=<size>-<mtime>) */
} SubChoice;

static int sub_order(const void *pa, const void *pb) {
    const SubChoice *a = pa, *b = pb;
    if (!a->t.lang[0] != !b->t.lang[0]) return a->t.lang[0] ? -1 : 1; /* (a language not known: last) */
    int k = strcmp(a->t.lang, b->t.lang);
    if (k) return k;
    if (a->t.forced != b->t.forced) return a->t.forced ? 1 : -1; /* the plain one first, then SDH, then forced */
    if (a->t.sdh != b->t.sdh) return a->t.sdh ? 1 : -1;
    if (a->t.n != b->t.n) return a->t.n < b->t.n ? -1 : 1;
    if (a->named != b->named) return a->named ? -1 : 1; /* ("x.en.srt" before "x.English.srt") */
    return strcmp(a->f->rel, b->f->rel);
}

static bool key_taken(const SubChoice *s, int n, const char *key, int except) {
    for (int i = 0; i < n; i++)
        if (i != except && str_eq(s[i].key, key)) return true;
    return false;
}

/* The words for a subtitle: its language in itself ("Svenska", "English"), then which one in the
   reader's words. Two that would read the same differ by their number ("English 2"). */
static void sub_label(const MediaSubTags *t, char *out, size_t cap) {
    char name[96];
    if (t->n >= 2) snprintf(name, sizeof name, "%s %d", lang_name(t->lang), t->n);
    else str_copy(name, sizeof name, lang_name(t->lang));
    if (t->sdh && t->forced) snprintf(out, cap, T("%s (for the hard of hearing, foreign speech only)"), name);
    else if (t->sdh) snprintf(out, cap, T("%s (for the hard of hearing)"), name);
    else if (t->forced) snprintf(out, cap, T("%s (foreign speech only)"), name);
    else str_copy(out, cap, name);
}

/* The pref holding a viewer's last subtitles pick in a series (1.3; media.sub): "media.sub.<series id>" =
   "<generation> <the pick's key>" ("3 en", "3 sv.forced"), the key "-" when they turned subtitles off. A new
   "Subtitles on by default" (media.prefs) starts a new generation ("media.sub.gen"): every series then
   starts from that language again. */
typedef struct {
    char k[32];
} SubMemory;

static SubMemory sub_memory_key(uint64_t series) {
    SubMemory m;
    char hex[17];
    media_id_hex(series, hex);
    snprintf(m.k, sizeof m.k, "media.sub.%s", hex);
    return m;
}

static int sub_generation(int viewer) { return (int)str_int_or(pref(viewer, "media.sub.gen", "0"), 0); }

/* the viewer's pick in a series, this generation's: a key, "-" (off), or "" (none) */
static void sub_memory(int viewer, uint64_t series, char *out, size_t cap) {
    const char *m = pref(viewer, sub_memory_key(series).k, "");
    char *sp = NULL;
    long gen = strtol(m, &sp, 10);
    out[0] = '\0';
    if (sp && sp > m && *sp == ' ' && gen == sub_generation(viewer)) str_copy(out, cap, sp + 1);
}

/* A video's subtitles as the player offers them (C4): by language, the plain one before SDH and forced
   ones, then the second (".2") ... - each with a key of its own (two files that would share one: the
   later gets the next free number). Picture subtitles (.sup, a VobSub's .idx - never its .sub) are in the
   same list by the same rules (C5: x.en.srt + x.en.2.sup); the player draws them from their cue list. The
   viewer's language (Library settings) turns on its first text one - a picture one only when it has no
   text one (pictures need wire.js; a <track> shows without it). */
static int sub_choices(const Ctx *c, const MediaFile *f, SubChoice *out, int max) {
    const MediaIndex *ix = media_lib();
    int n = 0;
    for (int i = 0; i < ix->nfiles && n < max; i++) {
        const MediaFile *sub = &ix->files[i];
        if (sub->type != MF_SUB || sub->video != f->id || media_is_vobsub_data(sub->rel)) continue;
        SubChoice *s = &out[n];
        memset(s, 0, sizeof *s);
        s->f = sub;
        s->pic = media_is_sub_pic(sub->rel);
        if (s->pic) { /* "/media/subpic/<id>.json?v=<size>-<mtime>&k=...": a file made again is another address,
                         and the key (as keyed_src's) lets a screen without the page's cookie in too */
            char path[60], k[CTX_KEY_LEN];
            snprintf(path, sizeof path, "/media/subpic/%016llx.json", (unsigned long long)sub->id);
            ctx_url_key(c, path, k);
            snprintf(s->cues, sizeof s->cues, "%s?v=%lld-%lld%s%s", path, (long long)sub->size, (long long)sub->mtime,
                     k[0] ? "&k=" : "", k);
        }
        const char *vb = strrchr(f->rel, '/'), *sb = strrchr(sub->rel, '/'), *ext = strrchr(sub->rel, '.');
        vb = vb ? vb + 1 : f->rel;
        sb = sb ? sb + 1 : sub->rel;
        media_sub_tags(vb, sb, &s->t);
        if (!s->t.lang[0]) str_copy(s->t.lang, sizeof s->t.lang, sub->lang); /* (as the library read it) */
        char own[400];
        s->named = ext && media_sub_name(vb, &s->t, ext + 1, own, sizeof own) && str_ieq(own, sb);
        n++;
    }
    qsort(out, (size_t)n, sizeof *out, sub_order);
    /* keys: each its own first (in this order), then those whose key was taken get the next free number */
    for (int i = 0; i < n; i++) {
        media_sub_key(&out[i].t, out[i].key, sizeof out[i].key);
        if (key_taken(out, i, out[i].key, -1)) out[i].key[0] = '\0';
    }
    bool moved = false;
    for (int i = 0; i < n; i++) {
        if (out[i].key[0]) continue;
        for (out[i].t.n = MAX(out[i].t.n, 1) + 1;; out[i].t.n++) {
            media_sub_key(&out[i].t, out[i].key, sizeof out[i].key);
            if (!key_taken(out, n, out[i].key, i)) break;
        }
        moved = true;
    }
    if (moved) qsort(out, (size_t)n, sizeof *out, sub_order);
    int v = ctx_viewer(c);
    const char *want = v ? pref(v, "media.sublang", "") : "";
    int lang = -1; /* the viewer's language (Library settings): text first, then pictures */
    for (int pass = 0; pass < 2 && lang < 0 && *want; pass++)
        for (int i = 0; i < n && lang < 0; i++)
            if (out[i].pic == (pass == 1) && str_ieq(out[i].t.lang, want)) lang = i;
    /* the viewer's last pick in this series (1.3; owner: "if enabled on an episode, carry on to the next episode
       with the same language if it exists") - the same key, else the first of its language (the menu's order, as
       the player picks), else none; "-" = turned off - wins over the language */
    char mem[40] = "";
    if (v && f->series) sub_memory(v, f->series, mem, sizeof mem);
    int last = -1;
    if (mem[0] && !str_eq(mem, "-")) {
        size_t ml = strcspn(mem, ".");
        for (int i = 0; i < n && last < 0; i++)
            if (str_eq(out[i].key, mem)) last = i;
        for (int i = 0; i < n && last < 0; i++)
            if (strcspn(out[i].key, ".") == ml && !strncmp(out[i].key, mem, ml)) last = i;
    }
    int on = mem[0] ? last : lang, pick = last >= 0 ? last : lang;
    for (int i = 0; i < n; i++) {
        sub_label(&out[i].t, out[i].label, sizeof out[i].label);
        out[i].on = i == on;
        out[i].pref = i == pick;
    }
    return n;
}

/* The video's text subtitles as <track>s (id "sub-<key>": wire.js picks by it); the viewer's language
   (Library settings) is on by default. (Picture ones have none: the player draws them.) */
void media_tracks(const Ctx *c, Buf *b, const MediaFile *f) {
    SubChoice *s = xmalloc(MEDIA_MAX_SUBS * sizeof *s);
    int n = sub_choices(c, f, s, MEDIA_MAX_SUBS);
    for (int i = 0; i < n; i++) {
        if (s[i].pic) continue;
        char src[100];
        media_sub_src(c, s[i].f->id, src, sizeof src);
        H(b, "<track id=\"sub-%s\" kind=\"subtitles\" src=\"%s\" srclang=\"%s\" label=\"%s\"%r>", s[i].key, src,
          s[i].t.lang[0] ? s[i].t.lang : "und", s[i].label, s[i].on ? " default" : "");
    }
    free(s);
}

void media_controls(const Ctx *c, Buf *b, const MediaFile *f, bool tv) {
    SubChoice *choice = xmalloc(MEDIA_MAX_SUBS * sizeof *choice); /* the same as the <track>s: same order, same one on */
    UiSub subs[MEDIA_MAX_SUBS];
    char key[24];
    media_video_key(c, f, key, sizeof key);
    const MediaFile *pf = media_playable(f);
    int64_t ms = f->probe.duration_ms ? f->probe.duration_ms : pf->probe.duration_ms;
    UiPlayer p = {.box = "media-box", .video = "media-video", .key = key, .seconds = (int)(ms / 1000), .subs = subs, .tv = tv};
    if (str_eq(pf->probe.vcodec, "hevc")) /* (an HEVC copy plays where the device decodes HEVC - not every TV's browser) */
        p.trouble = T("This video is HEVC (x265), which this browser can't play. Most phones, computers and iPads can.");
    else if (str_eq(pf->probe.vcodec, "av1")) /* (and an AV1 one where it decodes AV1: Apple's only on the newest) */
        p.trouble = T("This video is AV1, which this browser can't play. Chrome, Edge, Firefox and Android phones can, and "
                      "the newest iPhones and Macs.");
    p.nsubs = sub_choices(c, f, choice, MEDIA_MAX_SUBS);
    for (int i = 0; i < p.nsubs; i++) {
        const SubChoice *s = &choice[i];
        subs[i] = (UiSub){.key = s->key, .lang = s->t.lang[0] ? s->t.lang : "und", .label = s->label,
                          .pic = s->pic ? s->cues : NULL, .on = s->on, .pref = s->pref};
    }
    /* its sound tracks (2.2): each the same video's address with that one */
    const MediaSounds *snds = sounds_of(f);
    UiSound snd[MEDIA_MAX_SOUNDS];
    char snd_src[MEDIA_MAX_SOUNDS][120], snd_label[MEDIA_MAX_SOUNDS][112];
    int on = snds ? sound_pick(c, f, snds) : -1;
    for (int i = 0; snds && i < snds->n; i++) {
        sound_src(c, f->id, i, snd_src[i], sizeof snd_src[i]);
        sound_label(snds, i, snd_label[i], sizeof snd_label[i]);
        snd[i] = (UiSound){.n = i, .src = snd_src[i], .label = snd_label[i], .on = i == on};
    }
    p.sounds = snd;
    p.nsounds = snds ? snds->n : 0;
    ui_player(b, &p);
    free(choice);
}

static const char *query(const Ctx *c) {
    const char *q = ctx_live(c, "media.q");
    return *q ? q : ctx_q(c, "q");
}

int media_percent(const MediaWatch *w) {
    if (!w || w->dur <= 0 || w->pos <= 0) return 0;
    return CLAMP(w->pos * 100 / w->dur, 1, 100);
}

/* Progress, "continue watching" and watched ticks are the viewer's: the signed-in person, or
   whoever was picked on a display account (the living-room TV) - nobody's until someone is. */
const MediaWatch *media_watch_of(const Ctx *c, const MediaFile *f) {
    int v = ctx_viewer(c);
    return v && f ? media_watch_get(v, f->id) : NULL;
}

/* Episodes in a series, counting both halves of a two-episode file ("S01E01-E02"). */
int media_episodes_in(const MediaSeries *s) {
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        n += e && e->name.episode_end > e->name.episode ? e->name.episode_end - e->name.episode + 1 : 1;
    }
    return n;
}

/* The number in an episode row: "5", or "1–2" for two episodes in one file. */
static const char *ep_n(const MediaFile *f) {
    static char n[24];
    if (f->name.episode_end > f->name.episode) snprintf(n, sizeof n, "%d–%d", f->name.episode, f->name.episode_end);
    else snprintf(n, sizeof n, "%d", f->name.episode);
    return n;
}

void media_ep_code(Buf *b, const MediaFile *f) {
    H(b, "S%02dE%02d", f->name.season, f->name.episode);
    if (f->name.episode_end > f->name.episode) H(b, "–E%02d", f->name.episode_end);
}

/* Bold line of an episode row: its title, or "Episode 3". */
void media_ep_title(Buf *b, const MediaFile *f) {
    if (f->name.ep_title[0]) html_esc(b, f->name.ep_title);
    else H(b, T("Episode %d"), f->name.episode);
}

static void quality(Buf *b, const MediaFile *f) {
    int h = f->probe.height, w = f->probe.width;
    if (h >= 2000 || w >= 3800) buf_puts(b, "4K");
    else if (h >= 1000 || w >= 1900) buf_puts(b, "1080p");
    else if (h >= 700 || w >= 1270) buf_puts(b, "720p");
    else if (h > 0) buf_puts(b, "SD");
}

/* A codec or container by the name people know it by ("h264" -> "H.264"); others in capitals. */
static void tech_name(Buf *b, const char *id) {
    static const char *const names[][2] = {
        {"h264", "H.264"}, {"hevc", "HEVC (H.265)"}, {"mpeg2", "MPEG-2"}, {"mpeg4", "MPEG-4 (XviD/DivX)"},
        {"ac3", "AC-3 (Dolby Digital)"}, {"eac3", "E-AC-3"}, {"opus", "Opus"}, {"vorbis", "Vorbis"},
        {"truehd", "TrueHD"}, {"pcm", "PCM"}};
    for (int i = 0; i < countof(names); i++)
        if (str_eq(id, names[i][0])) {
            buf_puts(b, names[i][1]);
            return;
        }
    for (const char *p = id; *p; p++) buf_putc(b, (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p));
}

/* "MP4 file · H.264 video · AAC sound (5.1)" - folded under the plain badge (issue #62). Escaped. */
static void tech_fact(Buf *t, const char *fmt, const char *id) { /* fmt: "%s file" in the language in use */
    Buf n = {0};
    tech_name(&n, id);
    buf_puts(t, t->len ? " · " : "");
    buf_printf(t, fmt, buf_cstr(&n));
    buf_free(&n);
}

static void tech_facts(Buf *b, const MediaFile *f) {
    Buf t = {0};
    if (f->probe.container[0]) tech_fact(&t, T("%s file"), f->probe.container);
    if (f->probe.vcodec[0]) tech_fact(&t, T("%s video"), f->probe.vcodec);
    if (f->probe.acodec[0]) {
        tech_fact(&t, T("%s sound"), f->probe.acodec);
        if (f->probe.audio_channels > 2) buf_printf(&t, " (%d.1)", f->probe.audio_channels - 1);
    }
    html_esc(b, buf_cstr(&t));
    buf_free(&t);
}

static void duration(Buf *b, const MediaFile *f) {
    char d[32];
    media_fmt_duration(f->probe.duration_ms, d, sizeof d);
    html_esc(b, d);
}

/* /media/img/<id>?v=<version>: a replaced picture keeps its name (and id), and browsers keep
   images for a day - the version makes them fetch the new one at once. */
void media_img_src(Buf *b, uint64_t img) {
    const MediaFile *f = media_file(img);
    H(b, "/media/img/%X?v=%X", img, f ? (uint64_t)f->mtime * 1000003u + (uint64_t)f->size : (uint64_t)0);
}

uint64_t media_picture(const MediaFile *f) {
    if (!f) return 0;
    if (f->poster) return f->poster;
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    return s ? s->poster : 0;
}

/* The colours of a drawn poster, from its title (each series its own, the same everywhere): the
   hue and two neighbours worked out here, so the CSS needs no calc() inside hsl() - older TV
   browsers drop a whole background over that. */
void media_art_style(Buf *b, const char *title) {
    int h = media_hue(title);
    H(b, " style=\"--h:%d;--h2:%d;--h3:%d\"", h, (h + 40) % 360, (h + 25) % 360);
}

/* The art of a poster: an image when the library has one, otherwise a
 * typographic card in the title's colour (`shown`: the words on it, NULL = the title - a disc
 * extra's in the reader's language keeps its colour from its own title). */
static void poster_art_as(Buf *b, const char *title, const char *shown, int year, const char *label, uint64_t img, int pct,
                          bool done) {
    H(b, "<span class=\"media-art\"");
    media_art_style(b, title);
    buf_putc(b, '>');
    if (img) {
        buf_puts(b, "<img src=\"");
        media_img_src(b, img);
        buf_puts(b, "\" alt=\"\" loading=\"lazy\" decoding=\"async\">");
    } else {
        if (label) H(b, "<span class=\"media-art-k\">%s</span>", label);
        H(b, "<span class=\"media-art-t\">%s</span>", shown ? shown : title);
        if (year) H(b, "<span class=\"media-art-y\">%d</span>", year);
    }
    if (done) {
        H(b, "<span class=\"media-art-done\" title=\"%s\">", T("Watched"));
        ui_icon(b, "check");
        buf_puts(b, "</span>");
    } else if (pct > 0) {
        H(b, "<span class=\"media-art-bar\"><i style=\"width:%d%%\"></i></span>", pct);
    }
    buf_puts(b, "</span>");
}

static void poster_art(Buf *b, const char *title, int year, const char *label, uint64_t img, int pct, bool done) {
    poster_art_as(b, title, NULL, year, label, img, pct, done);
}

/* A video's title as people read it. The library names a ripped disc's "play all" title and bonus clips
   itself, in English ("70's Show – season 1, disc 4: all episodes" - kept so: it's part of what the video
   is, library.c name_extra); here they're said again in the reader's language from their parts. */
const char *media_title(const MediaFile *f, char *out, size_t cap) {
    const MediaSeries *s = f->name.extra && (f->role == MF_PLAYALL || f->role == MF_BONUS) ? media_series(f->series) : NULL;
    if (!s) {
        str_copy(out, cap, f->name.title);
        return out;
    }
    char where[96];
    if (f->name.season && f->name.disc) snprintf(where, sizeof where, T("season %d, disc %d"), f->name.season, f->name.disc);
    else if (f->name.season) snprintf(where, sizeof where, T("season %d"), f->name.season);
    else if (f->name.disc) snprintf(where, sizeof where, T("specials, disc %d"), f->name.disc);
    else str_copy(where, sizeof where, T("specials"));
    if (f->role == MF_PLAYALL) snprintf(out, cap, T("%s – %s: all episodes"), s->title, where);
    else if (f->name.ep_title[0]) snprintf(out, cap, "%s – %s: %s", s->title, where, f->name.ep_title);
    else if (f->name.track >= 0) snprintf(out, cap, T("%s – %s: extra (title %d)"), s->title, where, f->name.track);
    else snprintf(out, cap, T("%s – %s: extra"), s->title, where);
    return out;
}

static void video_card(Ctx *c, Buf *b, const MediaFile *f) {
    const MediaWatch *w = media_watch_of(c, f);
    H(b, "<a class=\"media-poster\" href=\"/media/watch/%X\">", f->id);
    if (f->name.kind == MEDIA_EPISODE) {
        const MediaSeries *s = media_series(f->series);
        poster_art(b, f->name.title, 0, NULL, f->poster ? f->poster : s ? s->poster : 0, media_percent(w), w && w->done);
        H(b, "<span class=\"media-cap\">%s<small>", f->name.title);
        media_ep_code(b, f);
        if (f->name.ep_title[0]) H(b, " · %s", f->name.ep_title);
    } else {
        char title[300];
        media_title(f, title, sizeof title);
        poster_art_as(b, f->name.title, title, f->name.year, f->name.kind == MEDIA_OTHER ? T("Video") : NULL, f->poster,
                      media_percent(w), w && w->done);
        H(b, "<span class=\"media-cap\">%s<small>", title);
        if (f->name.year) H(b, "%d", f->name.year);
        if (f->name.year && f->probe.duration_ms) buf_puts(b, " · ");
        duration(b, f);
    }
    buf_puts(b, "</small></span></a>");
}

static void going_card(Ctx *c, Buf *b, const MediaFile *f, bool small); /* (below, next to mini) */

/* --------------------------------------------------------------- pictures */

/* Admins pick a series' or film's picture: one image from this device, sent like an upload
   (upload.c, form[data-upload]) and saved where the scan looks first (media_art_target). */
static bool can_set_art(const Ctx *c, uint64_t id) {
    int root;
    char dir[400], stem[400];
    return is_admin(c) && media_art_target(id, &root, dir, sizeof dir, stem, sizeof stem);
}

static void art_modal(Ctx *c, Buf *b, uint64_t id, const char *title, int year, const char *label, uint64_t img,
                      const char *close) {
    char hex[17], abs[1024], stem[400];
    media_id_hex(id, hex);
    if (!media_art_folder(id, abs, sizeof abs, stem, sizeof stem)) return;
    ui_modal(b, T("Picture"), close);
    buf_puts(b, "<div class=\"media-pic\"><div class=\"media-pic-art\">");
    poster_art(b, title, year, label, img, 0, false);
    /* the form first, so status lines appearing below never rebuild it (and drop the chosen file) */
    H(b,
      "</div><div class=\"stack\"><form class=\"stack\" id=\"media-pic-f\" data-upload=\"/media/upload\" "
      "data-empty=\"%s\"><input type=\"hidden\" name=\"art\" value=\"%s\">"
      "<label class=\"field\"><span>%s</span><input type=\"file\" name=\"files\" "
      "accept=\"image/jpeg,image/png,image/webp\"></label>"
      "<p class=\"muted media-small\">",
      T("Choose a picture first"), hex, T("A picture from this device"));
    H(b,
      T("JPEG, PNG or WebP - a tall poster (2:3) fits best. It's saved in the library folder as "
        "<code class=\"media-code\">%s.jpg</code> (or .png/.webp), where other players find it too."),
      stem);
    H(b, "</p><div class=\"modal-f\"><button class=\"btn primary\">%s</button></div></form>", T("Use this picture"));
    upload_art_status(c, b, id);
    if (media_art_current(id)) {
        ui_form_confirm(b, "media.art_remove", "inline", T("Remove this picture? Its file is deleted from the library folder."));
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%s\"><button class=\"btn\">%s</button>", hex, T("Remove picture"));
        ui_form_end(b);
    }
    buf_puts(b, "</div></div>");
    ui_modal_end(b);
}

static int watched_count(const Ctx *c, const MediaSeries *s) {
    int n = 0, v = ctx_viewer(c);
    for (int i = 0; v && i < s->count; i++) {
        MediaWatch *w = media_watch_get(v, media_episode(s, i)->id);
        n += w && w->done;
    }
    return n;
}

static void series_card(Ctx *c, Buf *b, const MediaSeries *s) {
    H(b, "<a class=\"media-poster\" href=\"/media/series/%X\">", s->id);
    int seen = watched_count(c, s);
    poster_art(b, s->title, s->year, T("Series"), s->poster, seen ? seen * 100 / s->count : 0, seen == s->count && seen);
    int eps = media_episodes_in(s);
    H(b, "<span class=\"media-cap\">%s<small>", s->title);
    H(b, TN(eps, "%d episode", "%d episodes"), eps);
    if (media_seasons(s) > 1) {
        buf_puts(b, " · ");
        H(b, T("%d seasons"), media_seasons(s));
    }
    buf_puts(b, "</small></span></a>");
}

/* The viewer's unfinished videos, most recently watched first. */
int media_in_progress(const Ctx *c, const MediaFile **out, int max) {
    const MediaWatch *keep[24];
    int n = 0, v = ctx_viewer(c);
    max = MIN(max, countof(keep));
    for (int i = 0; v && max > 0 && i < tbl_count(&T_media_watch); i++) {
        const MediaWatch *w = tbl_at(&T_media_watch, i);
        uint64_t id = 0;
        if (w->person != v || w->done || w->pos < 10 || !media_parse_id(w->item, &id) || !media_file(id) ||
            media_file(id)->type != MF_VIDEO || /* (an audiobook's: Continue listening - 1.6) */
            !media_kids_ok(c, media_file(id))) /* (a child's: not what's marked - issue #44) */
            continue;
        if (n == max && keep[n - 1]->updated >= w->updated) continue;
        int k = n < max ? n++ : n - 1; /* full: the oldest falls off the end */
        while (k > 0 && keep[k - 1]->updated < w->updated) keep[k] = keep[k - 1], k--;
        keep[k] = w;
    }
    for (int i = 0; i < n; i++) {
        uint64_t id = 0;
        media_parse_id(keep[i]->item, &id);
        out[i] = media_file(id);
    }
    return n;
}

/* What to play next in a series for the viewer: the one they're in the middle of, else the episode after the
 * last one they finished. The specials (last, 1.3) are apart: a special watched doesn't move it into them, and
 * after the last season it starts over - unless the series is nothing but specials. */
const MediaFile *media_next_up(const Ctx *c, const MediaSeries *s) {
    int regular = s->count, last_done = -1, v = ctx_viewer(c); /* episodes [0, regular) are the seasons' */
    while (regular > 0 && media_episode(s, regular - 1)->name.season == 0) regular--;
    int upto = regular ? regular : s->count;
    for (int i = 0; v && i < s->count; i++) {
        MediaWatch *w = media_watch_get(v, media_episode(s, i)->id);
        if (w && w->done) last_done = i < upto ? i : last_done;
        else if (w && w->pos > 0) return media_episode(s, i);
    }
    return media_episode(s, last_done + 1 < upto ? last_done + 1 : 0);
}

/* ------------------------------------------------------------------ chrome */

/* One Watch section: the library's tabs, and tv's channels as "Live TV" (tv's pages draw this
   header too, through media.h). */
void media_lib_head(Ctx *c, Buf *b, const char *active, const char *title, const char *sub, const char *buttons) {
    ui_head(b, title, sub);
    if (media_scanning()) {
        int seen = media_scan_seen();
        buf_puts(b, "<span class=\"badge media-scanning\"><span class=\"media-spin\"></span>");
        H(b, TN(seen, "Scanning · %d file", "Scanning · %d files"), seen);
        buf_puts(b, "</span>");
    }
    if (buttons) buf_puts(b, buttons);
    /* adding videos, where an admin looks for it (1.1: it hid behind Library settings' corner) - once the library
       has a folder to put them in (the empty page offers the folders first) */
    char roots[MEDIA_MAX_ROOTS][300];
    if (is_admin(c) && !str_eq(active, "tv") && media_roots(roots, MEDIA_MAX_ROOTS) > 0) {
        buf_puts(b, "<a class=\"btn primary media-add\" href=\"/media/upload\">");
        ui_icon(b, "plus");
        H(b, "<span>%s</span></a>", str_eq(active, "books") ? T("Add audiobooks") : T("Add videos"));
    }
    H(b, "<a class=\"icon-btn\" href=\"/media/settings\" title=\"%s\" aria-label=\"%s\">", T("Library settings"),
      T("Library settings"));
    ui_icon(b, "settings");
    buf_puts(b, "</a>");
    ui_head_end(b);
    static const char *const tabs[][3] = {{"home", "/media", N_("Home")},          {"movies", "/media/movies", N_("Movies")},
                                          {"series", "/media/series", N_("Series")}, {"books", "/media/books", N_("Audiobooks")},
                                          {"tv", "/tv", N_("Live TV")},              {"browse", "/media/browse", N_("Folders")}};
    buf_puts(b, "<div class=\"toolbar media-tools\"><nav class=\"seg media-tabs\">");
    for (int i = 0; i < countof(tabs); i++) {
        if (str_eq(tabs[i][0], "tv") && !module_find("tv")) continue; /* built without channels */
        if (str_eq(tabs[i][0], "books") && !media_lib_for(c)->nbooks && !str_eq(active, "books")) continue; /* (none yet) */
        bool on = str_eq(active, tabs[i][0]);
        H(b, "<a href=\"%s\"%r>%s</a>", tabs[i][1], on ? " class=\"on\" aria-current=\"page\"" : "", T(tabs[i][2]));
    }
    /* searching is the library's: from Live TV the box simply opens Watch's results */
    H(b,
      "</nav><form class=\"media-search\" method=\"get\" action=\"/media\" role=\"search\">"
      "<input type=\"search\" name=\"q\" value=\"%s\" placeholder=\"%s\" autocomplete=\"off\"%r "
      "data-key=\"/\" aria-label=\"%s\"></form></div>",
      str_eq(active, "tv") ? "" : query(c), str_eq(active, "books") ? T("Search audiobooks…") : T("Search movies and series…"),
      str_eq(active, "tv") ? "" : " data-live=\"media.search\"",
      T("Search the library"));
}

static void shelf_head(Buf *b, const char *title, const char *more) {
    H(b, "<section class=\"media-row\"><header class=\"media-row-h\"><h2>%s</h2>", title);
    if (more) H(b, "<a href=\"%s\">%s</a>", more, T("See all"));
    buf_puts(b, "</header><div class=\"media-shelf\">");
}

static void shelf_end(Buf *b) { buf_puts(b, "</div></section>"); }

/* "Recently added" with a series' batch of new episodes as one entry (media.h). An episode joins
   its series' latest entry when it came within a week of that entry's oldest episode, so two
   series copied in at once (their files interleaved) still make one entry each; an older batch
   of the same series is an entry of its own. */
#define BATCH_S (7 * 86400)
int media_recent(const Ctx *c, MediaRecent *out, int max) {
    const MediaIndex *ix = media_lib_for(c);
    int n = 0;
    for (int i = 0; i < ix->nrecent && max > 0; i++) {
        const MediaFile *f = &ix->files[ix->recent[i]];
        int k = -1;
        if (f->name.kind == MEDIA_EPISODE && f->series)
            for (int j = n - 1; j >= 0 && k < 0; j--)
                if (out[j].f->name.kind == MEDIA_EPISODE && out[j].f->series == f->series) k = j;
        if (k >= 0 && out[k].oldest - f->mtime <= BATCH_S) {
            out[k].n++;
            out[k].oldest = f->mtime;
            out[k].series = media_series(f->series);
            continue;
        }
        if (n < max) {
            out[n++] = (MediaRecent){f, NULL, 1, f->mtime};
            continue;
        }
        /* full: only open batches can still grow - stop once this is older than all of them */
        bool open = false;
        for (int j = 0; j < n && !open; j++) open = out[j].oldest - f->mtime <= BATCH_S;
        if (!open) break;
    }
    for (int j = 0; j < n; j++)
        if (out[j].n > 1 && !out[j].series) out[j].n = 1; /* (a series that vanished: just the video) */
    return n;
}

/* A shelf card for a batch of new episodes: the series' poster, "8 new episodes". */
static void batch_card(Buf *b, const MediaRecent *r) {
    const MediaSeries *s = r->series;
    H(b, "<a class=\"media-poster\" href=\"/media/series/%X\">", s->id);
    poster_art(b, s->title, s->year, T("Series"), s->poster, 0, false);
    H(b, "<span class=\"media-cap\">%s<small>", s->title);
    H(b, TN(r->n, "%d new episode", "%d new episodes"), r->n);
    buf_puts(b, "</small></span></a>");
}

/* ------------------------------------------------- folders to suggest (issue #27) */

/* What an admin can add with one tap instead of typing a path from memory: the folders the installer was told
   about (HEMMA_MEDIA in the settings file, ";" between them) - until they're in the library - and, while the
   library has no folders yet, the disks this machine has mounted (plat_mounts: under /mnt and /media, a Mac's
   /Volumes, Windows' other drives). Each once, only folders that are there. */
typedef struct {
    char path[300];
    bool installer; /* named by the installer (HEMMA_MEDIA) */
    int64_t free, total;
} Suggest;

static void gb(int64_t n, char *out, size_t cap);

static char g_test_mounts[600]; /* tests: the disks "this machine" has (";" between), instead of plat_mounts() */
void media_test_mounts(const char *list) { str_copy(g_test_mounts, sizeof g_test_mounts, list ? list : ""); }

static void same_slashes(char *p) {
    for (; *p; p++)
        if (*p == '\\') *p = '/';
}

static bool same_folder(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    while (na > 1 && a[na - 1] == '/') na--;
    while (nb > 1 && b[nb - 1] == '/') nb--;
    return na == nb && strncmp(a, b, na) == 0;
}

static int suggestions(Suggest *out, int max) {
    char roots[MEDIA_MAX_ROOTS][300], found[16][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS), n = 0, nfound = 0;
    const char *told = env("HEMMA_MEDIA", "");
    while (*told && nfound < countof(found)) { /* "/mnt/media;/mnt/more" */
        size_t len = strcspn(told, ";");
        str_ncopy(found[nfound], sizeof found[0], told, len);
        str_trim(found[nfound]);
        if (found[nfound][0]) nfound++;
        told += len + (told[len] == ';');
    }
    int ntold = nfound;
    if (!nroots && !app_testing()) nfound += plat_mounts(found + nfound, countof(found) - nfound);
    for (const char *t = g_test_mounts; !nroots && *t && nfound < countof(found);) { /* (tests: this machine's disks) */
        size_t len = strcspn(t, ";");
        str_ncopy(found[nfound++], sizeof found[0], t, len);
        t += len + (t[len] == ';');
    }
    for (int i = 0; i < nfound && n < max; i++) {
        same_slashes(found[i]);
        bool skip = !path_is_dir(found[i]);
        for (int k = 0; k < nroots && !skip; k++) skip = same_folder(roots[k], found[i]);
        for (int k = 0; k < n && !skip; k++) skip = same_folder(out[k].path, found[i]);
        if (skip) continue;
        Suggest *s = &out[n++];
        *s = (Suggest){.installer = i < ntold};
        str_copy(s->path, sizeof s->path, found[i]);
        path_disk_space(s->path, &s->free, &s->total);
    }
    return n;
}

/* One tap adds it (media.roots_add); `big` = the Watch page's first-run buttons, else chips. */
static void suggestion_forms(Buf *b, const Suggest *s, int n, bool big) {
    for (int i = 0; i < n; i++) {
        char size[32] = "";
        if (s[i].total > 0) gb(s[i].total, size, sizeof size);
        ui_form(b, "media.roots_add", "inline");
        ui_hidden(b, "dir", s[i].path);
        if (big) {
            buf_puts(b, "<button class=\"btn primary\">");
            H(b, T("Use <code class=\"media-code\">%s</code>"), s[i].path);
            buf_puts(b, "</button>");
        } else {
            buf_puts(b, "<button class=\"chip\" aria-label=\"");
            H(b, T("Add %s"), s[i].path);
            H(b, "\">+ <code class=\"media-code\">%s</code>%r%s", s[i].path, size[0] ? " <small>" : "", size);
            if (s[i].installer) H(b, " · %s</small>", T("from the installer"));
            else if (size[0]) buf_puts(b, "</small>");
            buf_puts(b, "</button>");
        }
        ui_form_end(b);
    }
}

/* A sentence naming who to ask (ui_admins: "Anna (admin)"), fmt's %r. */
static void ask_admins(Buf *b, const char *fmt) {
    Buf who = {0};
    ui_admins(&who);
    H(b, fmt, buf_cstr(&who));
    buf_free(&who);
}

/* The last row of a cut list: open ("<li class=\"muted\">") + "and 3 more" + "</li>". */
static void more_row(Buf *b, const char *open, int n) {
    buf_puts(b, open);
    H(b, T("and %d more"), n);
    buf_puts(b, "</li>");
}

static bool library_empty_states(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    if (ix->nroots == 0 && !is_admin(c)) {
        ui_empty(b, "🎬", T("No movies yet"), T("Movies and series from the drive at home will show up here."));
        buf_puts(b, "<p class=\"media-center muted\">");
        ask_admins(b, T("Ask %r to add the movie folders."));
        buf_puts(b, "</p>");
        return true;
    }
    if (ix->nroots == 0) {
        ui_empty(b, "🎬", T("Point Hemma at your movies"),
                 T("Tell it which folders hold your movies and series — the drive on the Pi, a mounted share, "
                   "or a folder on this computer."));
        if (media_drives_offer(c, b)) { /* (issue #74: a USB disk just plugged in) */
            H(b, "<p class=\"media-center\"><a href=\"/media/settings\">%s</a></p>", T("Another folder"));
            return true;
        }
        Suggest s[4];
        int ns = suggestions(s, countof(s)); /* (issue #27: one tap, not a path typed from memory) */
        if (ns) {
            buf_puts(b, "<div class=\"media-center media-suggest-big\">");
            suggestion_forms(b, s, ns, true);
            H(b, "</div><p class=\"media-center\"><a href=\"/media/settings\">%s</a></p>", T("Another folder"));
            return true;
        }
        H(b, "<p class=\"media-center\"><a class=\"btn primary\" href=\"/media/settings\">%s</a></p>", T("Choose folders"));
        return true;
    }
    if (ix->nrecent == 0 && ix->nbooks) { /* (1.6: a library of audiobooks only) */
        media_books_shelf(c, b);
        ui_empty(b, "🎧", T("No videos yet"), T("Your audiobooks are under Audiobooks."));
        H(b, "<p class=\"media-center\"><a class=\"btn primary\" href=\"/media/books\">%s</a></p>", T("Audiobooks"));
        return true;
    }
    if (ix->nrecent == 0) {
        if (media_scanning())
            ui_empty(b, "🔎", T("Looking through your library…"), T("Movies and series show up here as soon as the scan "
                                                                    "finishes."));
        else
            ui_empty(b, "🗂️", T("No videos found yet"),
                     T("Hemma looked in your folders but found no video files. Check the folders in Library settings."));
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- search */

/* 1.4: every word typed, in any order, somewhere in what's searched - a title, its year, its categories (in
   the reader's words and English's), an episode's name and number ("simpsons s02e03", "2x03"), a film's folders;
   the results grouped (series, movies, episodes, other videos), the best first: the whole title, a title that
   starts so, words that start the title's words, then the rest. An episode shows only when the words reach
   past its series' name (else "simpsons" would list 683 episodes) or its number was asked for. */
#define SEARCH_WORDS 8
typedef struct {
    char all[160];               /* the whole query, folded */
    char w[SEARCH_WORDS][64];
    int n, season, episode;      /* episode: 0 = no number asked */
} Query;

static bool word_char(unsigned char ch) { return ch >= 0x80 || (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z'); }

/* "s02e03", "s2e3", "2x03": an episode's number */
static bool ep_token(const char *t, int *season, int *episode) {
    int s = 0, e = 0, k = 0;
    if (sscanf(t, "s%de%d%n", &s, &e, &k) == 2 && !t[k]) goto ok;
    if (sscanf(t, "%dx%d%n", &s, &e, &k) == 2 && !t[k]) goto ok;
    return false;
ok:
    *season = s, *episode = e;
    return e > 0;
}

static void query_parse(const char *q, Query *out) {
    memset(out, 0, sizeof *out);
    fold_copy(out->all, sizeof out->all, q);
    str_trim(out->all);
    for (const char *p = out->all; *p && out->n < SEARCH_WORDS;) {
        while (*p && !word_char((unsigned char)*p)) p++;
        size_t len = 0;
        while (p[len] && word_char((unsigned char)p[len])) len++;
        if (!len) break;
        str_ncopy(out->w[out->n], sizeof out->w[0], p, len);
        if (!out->episode && ep_token(out->w[out->n], &out->season, &out->episode)) out->w[out->n][0] = '\0';
        else out->n++;
        p += len;
    }
}

/* every word in hay (folded) */
static bool all_words(const Query *q, const char *hay) {
    for (int i = 0; i < q->n; i++)
        if (!strstr(hay, q->w[i])) return false;
    return true;
}

static bool at_word_start(const char *hay, const char *w) {
    for (const char *p = hay; (p = strstr(p, w)); p++)
        if (p == hay || !word_char((unsigned char)p[-1])) return true;
    return false;
}

/* how well a title matches (folded title): 4 the whole of it, 3 its start, 2 every word starts one of its words,
   1 every word in it, 0 the words were found elsewhere */
static int title_rank(const Query *q, const char *title) {
    if (!q->n) return 0;
    if (str_eq(title, q->all)) return 4;
    if (str_starts(title, q->all)) return 3;
    bool starts = true, in = true;
    for (int i = 0; i < q->n; i++) {
        starts = starts && at_word_start(title, q->w[i]);
        in = in && strstr(title, q->w[i]);
    }
    return starts ? 2 : in ? 1 : 0;
}

typedef struct {
    int rank, i; /* i: a series' index, a file's, or an audiobook's */
    const char *title;
} Hit;

static int hit_order(const void *x, const void *y) {
    const Hit *a = x, *b = y;
    if (a->rank != b->rank) return b->rank - a->rank;
    char fa[160], fb[160];
    fold_copy(fa, sizeof fa, a->title);
    fold_copy(fb, sizeof fb, b->title);
    int k = strcmp(fa, fb);
    return k ? k : a->i - b->i;
}

static void hay_of(Buf *h, const char *a, const char *b2, const char *c2) {
    char f[1200];
    buf_reset(h);
    buf_printf(h, "%s %s %s", a, b2 ? b2 : "", c2 ? c2 : "");
    fold_copy(f, sizeof f, buf_cstr(h));
    buf_reset(h);
    buf_puts(h, f);
}

/* One group of results: a heading with how many, the best `max` of them. */
enum { HITS_VIDEO, HITS_SERIES, HITS_BOOK }; /* (Hit.i: a file's index, a series', an audiobook's) */
static void hits_group(Ctx *c, Buf *b, const MediaIndex *ix, const char *head, Hit *h, int n, int max, int kind) {
    if (!n) return;
    qsort(h, (size_t)n, sizeof *h, hit_order);
    H(b, "<section class=\"media-found\"><h2>%s <small>%d</small></h2><div class=\"media-grid%r\">", head, n,
      kind == HITS_BOOK ? " media-grid-sq" : "");
    for (int i = 0; i < n && i < max; i++) {
        if (kind == HITS_SERIES) series_card(c, b, &ix->series[h[i].i]);
        else if (kind == HITS_BOOK) media_book_card(c, b, &ix->books[h[i].i]);
        else video_card(c, b, &ix->files[h[i].i]);
    }
    buf_puts(b, "</div>");
    if (n > max) {
        buf_puts(b, "<p class=\"muted media-found-more\">");
        H(b, T("and %d more"), n - max);
        H(b, " · %s</p>", T("more words narrow it down"));
    }
    buf_puts(b, "</section>");
}

static void search_results(Ctx *c, Buf *b, const char *qtext) {
    const MediaIndex *ix = media_lib_for(c);
    Query q;
    query_parse(qtext, &q);
    if (!q.n && !q.episode) return;
    Hit *hs = xmalloc((size_t)(ix->nseries + 1) * sizeof *hs), *hf = xmalloc((size_t)(ix->nrecent + 1) * sizeof *hf);
    Hit *he = xmalloc((size_t)(ix->nrecent + 1) * sizeof *he), *ho = xmalloc((size_t)(ix->nrecent + 1) * sizeof *ho);
    int ns = 0, nf = 0, ne = 0, no = 0;
    Buf hay = {0}, extra = {0};
    char t[160], year[8];
    MediaWork w;
    for (int i = 0; i < ix->nseries && !q.episode; i++) {
        const MediaSeries *s = &ix->series[i];
        media_work_series(s, &w);
        buf_reset(&extra);
        media_cats_words(&w, &extra);
        snprintf(year, sizeof year, "%d", s->year);
        hay_of(&hay, s->title, s->year ? year : NULL, buf_cstr(&extra));
        if (!all_words(&q, buf_cstr(&hay))) continue;
        fold_copy(t, sizeof t, s->title);
        hs[ns++] = (Hit){title_rank(&q, t), i, s->title};
    }
    for (int k = 0; k < ix->nrecent; k++) {
        int i = ix->recent[k];
        const MediaFile *f = &ix->files[i];
        snprintf(year, sizeof year, "%d", f->name.year);
        if (f->name.kind == MEDIA_EPISODE) {
            if (q.episode && (f->name.season != q.season || q.episode < f->name.episode ||
                              q.episode > MAX(f->name.episode, f->name.episode_end)))
                continue;
            hay_of(&hay, f->name.title, f->name.ep_title, NULL);
            if (!all_words(&q, buf_cstr(&hay))) continue;
            fold_copy(t, sizeof t, f->name.ep_title);
            bool own = q.episode != 0; /* (the words reach its own name, or its number was asked for) */
            for (int j = 0; j < q.n && !own; j++) own = strstr(t, q.w[j]) != NULL;
            if (own) he[ne++] = (Hit){title_rank(&q, t), i, f->name.ep_title};
        } else if (!q.episode && f->name.kind == MEDIA_MOVIE) {
            media_work_film(f, &w);
            buf_reset(&extra);
            media_cats_words(&w, &extra);
            buf_printf(&extra, " %s", f->rel);
            hay_of(&hay, f->name.title, f->name.year ? year : NULL, buf_cstr(&extra));
            if (!all_words(&q, buf_cstr(&hay))) continue;
            fold_copy(t, sizeof t, f->name.title);
            hf[nf++] = (Hit){title_rank(&q, t), i, f->name.title};
        } else if (!q.episode) {
            hay_of(&hay, f->name.title, f->rel, NULL);
            if (!all_words(&q, buf_cstr(&hay))) continue;
            fold_copy(t, sizeof t, f->name.title);
            ho[no++] = (Hit){title_rank(&q, t), i, f->name.title};
        }
    }
    /* audiobooks (1.6): by title, author, narrator, series */
    Hit *hb = xmalloc((size_t)(ix->nbooks + 1) * sizeof *hb);
    int nb = 0;
    for (int i = 0; i < ix->nbooks && !q.episode; i++) {
        const MediaBook *bk = &ix->books[i];
        buf_reset(&extra);
        buf_printf(&extra, "%s %s", bk->narrator, bk->series);
        hay_of(&hay, bk->title, bk->author, buf_cstr(&extra));
        if (!all_words(&q, buf_cstr(&hay))) continue;
        fold_copy(t, sizeof t, bk->title);
        hb[nb++] = (Hit){title_rank(&q, t), i, bk->title};
    }
    hits_group(c, b, ix, T("Series"), hs, ns, 24, HITS_SERIES);
    hits_group(c, b, ix, T("Movies"), hf, nf, 24, HITS_VIDEO);
    hits_group(c, b, ix, T("Episodes"), he, ne, 30, HITS_VIDEO);
    hits_group(c, b, ix, T("Audiobooks"), hb, nb, 24, HITS_BOOK);
    hits_group(c, b, ix, T("Other videos"), ho, no, 12, HITS_VIDEO);
    if (!ns && !nf && !ne && !no && !nb)
        ui_empty(b, "🤷", T("Nothing matches"),
                 T("Try another word — titles, episode names and numbers, categories and folders are searched."));
    buf_free(&hay);
    buf_free(&extra);
    free(hs), free(hf), free(he), free(ho), free(hb);
}

bool media_search_shown(Ctx *c, Buf *b) {
    const char *q = query(c);
    if (!*q) return false;
    search_results(c, b, q);
    return true;
}

/* ------------------------------------------------- lists: categories, order, what's shown (1.4) */

/* What the viewer has done with a series or film: 0 not started, 1 started, 2 seen to the end; *last = when
   they last watched any of it (0 = never). */
enum { SEEN_NONE, SEEN_GOING, SEEN_ALL };
static int series_seen(const Ctx *c, const MediaSeries *s, int64_t *last) {
    int v = ctx_viewer(c), done = 0, any = 0;
    *last = 0;
    for (int i = 0; v && i < s->count; i++) {
        const MediaWatch *w = media_watch_get(v, media_episode(s, i)->id);
        if (!w || (!w->done && w->pos <= 0)) continue;
        any++;
        done += w->done;
        if (w->updated > *last) *last = w->updated;
    }
    return !any ? SEEN_NONE : done == s->count ? SEEN_ALL : SEEN_GOING;
}

static int film_seen(const Ctx *c, const MediaFile *f, int64_t *last) {
    const MediaWatch *w = media_watch_of(c, f);
    *last = w && (w->done || w->pos > 0) ? w->updated : 0;
    return !*last ? SEEN_NONE : w->done ? SEEN_ALL : SEEN_GOING;
}

/* A list's address with its choices (each "" = its default): base?cat=&sort=&show= */
static void list_href(Buf *b, const char *base, const char *cat, const char *sort, const char *show) {
    const char *const keys[] = {"cat", "sort", "show"}, *vals[] = {cat, sort, show};
    H(b, "%s", base);
    for (int i = 0, n = 0; i < 3; i++)
        if (*vals[i]) H(b, "%r%s=%u", n++ ? "&amp;" : "?", keys[i], vals[i]);
}

typedef struct {
    const char *base, *sort, *show;
    char cat[MEDIA_CAT_LEN];
} ListChoice;

/* The category chips (the ones this list's titles are in, most first), the order and what's shown. */
static void list_controls(Ctx *c, Buf *b, const ListChoice *lc, int kind, const char *const sorts[][2], int nsorts) {
    MediaCatUse used[48];
    int nu = media_cats_used(c, kind, used, countof(used));
    if (nu) {
        H(b, "<nav class=\"chips media-cats\" aria-label=\"%s\"><a class=\"chip%r\" href=\"", T("Categories"),
          lc->cat[0] ? "" : " on");
        list_href(b, lc->base, "", lc->sort, lc->show);
        H(b, "\">%s</a>", T("All"));
        for (int i = 0; i < nu; i++) {
            H(b, "<a class=\"chip%r\" href=\"", str_eq(lc->cat, used[i].cat) ? " on" : "");
            list_href(b, lc->base, used[i].cat, lc->sort, lc->show);
            H(b, "\">%s <small>%d</small></a>", media_cat_label(used[i].cat),
              kind == MEDIA_CATS_SERIES ? used[i].series : used[i].films);
        }
        buf_puts(b, "</nav>");
    }
    buf_puts(b, "<div class=\"media-list-tools\"><nav class=\"seg media-sort\">");
    for (int i = 0; i < nsorts; i++) {
        H(b, "<a href=\"");
        list_href(b, lc->base, lc->cat, i ? sorts[i][0] : "", lc->show);
        H(b, "\"%r>%s</a>", str_eq(lc->sort, sorts[i][0]) ? " class=\"on\"" : "", T(sorts[i][1]));
    }
    buf_puts(b, "</nav>");
    if (ctx_viewer(c)) { /* (a TV with nobody picked has nobody's progress to go by) */
        static const char *const shows[][2] = {{"", N_("Everything")}, {"new", N_("Not started")}, {"going", N_("Started")}};
        buf_puts(b, "<nav class=\"seg media-show\">");
        for (int i = 0; i < countof(shows); i++) {
            H(b, "<a href=\"");
            list_href(b, lc->base, lc->cat, lc->sort, shows[i][0]);
            H(b, "\"%r>%s</a>", str_eq(lc->show, shows[i][0]) ? " class=\"on\"" : "", T(shows[i][1]));
        }
        buf_puts(b, "</nav>");
    }
    buf_puts(b, "</div>");
}

static void list_choice(Ctx *c, ListChoice *lc, const char *base, const char *const sorts[][2], int nsorts) {
    lc->base = base;
    lc->sort = sorts[0][0];
    for (int i = 1; i < nsorts; i++)
        if (str_eq(ctx_q(c, "sort"), sorts[i][0])) lc->sort = sorts[i][0];
    lc->show = str_eq(ctx_q(c, "show"), "new") ? "new" : str_eq(ctx_q(c, "show"), "going") ? "going" : "";
    if (!ctx_viewer(c)) lc->show = "";
    if (!media_cat_parse(ctx_q(c, "cat"), lc->cat)) lc->cat[0] = '\0';
}

static bool show_ok(const ListChoice *lc, int seen) {
    return !*lc->show || (str_eq(lc->show, "new") ? seen == SEEN_NONE : seen == SEEN_GOING);
}

static void list_nothing(Buf *b, const ListChoice *lc) {
    ui_empty(b, "🔎", T("Nothing here with these choices"), NULL);
    H(b, "<p class=\"media-center\"><a class=\"btn\" href=\"%s\">%s</a></p>", lc->base, T("Show everything"));
}

typedef struct {
    int i;
    int64_t key; /* what it's ordered by (bigger first) */
} Ordered;
static int ordered_cmp(const void *x, const void *y) {
    const Ordered *a = x, *b = y;
    if (a->key != b->key) return a->key > b->key ? -1 : 1;
    return a->i - b->i; /* (else as the index has them: A-Z) */
}

/* ----------------------------------------------------------------- pages */

/* "2 movies", "1 series" (the same word for one and many in English: hence "1 series" as its own text) */
static void count_movies(char *out, size_t cap, int n) { snprintf(out, cap, TN(n, "%d movie", "%d movies"), n); }
static void count_series(char *out, size_t cap, int n) { snprintf(out, cap, TN(n, "1 series", "%d series"), n); }

static void category_page(Ctx *c, Buf *b, const char *cat); /* (below, with the lists) */
static void category_rows(Ctx *c, Buf *b);

static void page_home(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib_for(c);
    ctx_title(c, "%s", T("Watch"));
    char sub[160] = "";
    if (ix->nrecent) {
        char m[48], s[48], o[48];
        count_movies(m, sizeof m, ix->nmovies);
        count_series(s, sizeof s, ix->nseries);
        snprintf(o, sizeof o, TN(ix->nothers, "%d other video", "%d other videos"), ix->nothers);
        snprintf(sub, sizeof sub, "%s · %s · %s", m, s, o);
    }
    media_lib_head(c, b, "home", T("Watch"), sub, NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    char cat[MEDIA_CAT_LEN];
    if (media_cat_parse(ctx_q(c, "cat"), cat)) {
        category_page(c, b, cat);
        return;
    }

    const MediaFile *going[12];
    int ngoing = media_in_progress(c, going, countof(going));
    if (ngoing) {
        shelf_head(b, T("Continue watching"), NULL);
        for (int i = 0; i < ngoing; i++) going_card(c, b, going[i], false);
        shelf_end(b);
    }
    media_books_shelf(c, b); /* (1.6: Continue listening) */
    /* what the channels air right now: tv's row, found by id (tv links media, never the other way) */
    const Block *onnow = block_find("tv.onnow", NULL);
    Buf row = {0};
    if (onnow && onnow->render(c, &row, 0, "")) { /* kind 0: an ordinary page - plain links */
        H(b, "<section class=\"media-row media-onnow\"><header class=\"media-row-h\"><h2>%s</h2>"
             "<a href=\"/tv\">%s</a></header>",
          T("On TV"), T("Guide"));
        buf_put(b, row.p, row.len);
        buf_puts(b, "</section>");
    }
    buf_free(&row);
    MediaRecent recent[14];
    int nrecent = media_recent(c, recent, countof(recent));
    shelf_head(b, T("Recently added"), NULL);
    for (int i = 0; i < nrecent; i++) {
        if (recent[i].n > 1) batch_card(b, &recent[i]);
        else video_card(c, b, recent[i].f);
    }
    shelf_end(b);
    if (ix->nseries) {
        shelf_head(b, T("Series"), "/media/series");
        for (int i = 0; i < ix->nseries && i < 14; i++) series_card(c, b, &ix->series[i]);
        shelf_end(b);
    }
    if (ix->nmovies) {
        shelf_head(b, T("Movies"), "/media/movies");
        for (int i = 0; i < ix->nmovies && i < 14; i++) video_card(c, b, &ix->files[ix->movies[i]]);
        shelf_end(b);
    }
    category_rows(c, b);
    if (ix->nothers) {
        shelf_head(b, T("Other videos"), "/media/browse");
        for (int i = 0; i < ix->nothers && i < 14; i++) video_card(c, b, &ix->files[ix->others[i]]);
        shelf_end(b);
    }
}

static void page_movies(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib_for(c);
    ctx_title(c, "%s", T("Movies"));
    char sub[48];
    count_movies(sub, sizeof sub, ix->nmovies);
    media_lib_head(c, b, "movies", T("Movies"), sub, NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    if (!ix->nmovies) {
        ui_empty(b, "🎞️", T("No movies yet"), T("Files with a year in the name — like \"Inception (2010).mkv\" — "
                                                "become movies."));
        return;
    }
    static const char *const sorts[][2] = {{"", N_("A–Z")}, {"new", N_("Newest")}, {"year", N_("Year")},
                                           {"watched", N_("Watched lately")}};
    ListChoice lc;
    list_choice(c, &lc, "/media/movies", sorts, ctx_viewer(c) ? 4 : 3);
    list_controls(c, b, &lc, MEDIA_CATS_FILMS, sorts, ctx_viewer(c) ? 4 : 3);
    Ordered *o = xmalloc((size_t)ix->nmovies * sizeof *o);
    int n = 0;
    for (int i = 0; i < ix->nmovies; i++) {
        const MediaFile *f = &ix->files[ix->movies[i]];
        MediaWork w;
        media_work_film(f, &w);
        int64_t last;
        if (!show_ok(&lc, film_seen(c, f, &last)) || (lc.cat[0] && !media_cat_in(&w, lc.cat))) continue;
        int64_t key = str_eq(lc.sort, "new") ? f->mtime : str_eq(lc.sort, "year") ? f->name.year : str_eq(lc.sort, "watched") ? last : 0;
        o[n++] = (Ordered){i, key};
    }
    qsort(o, (size_t)n, sizeof *o, ordered_cmp);
    buf_puts(b, "<div class=\"media-grid\">");
    for (int i = 0; i < n; i++) video_card(c, b, &ix->files[ix->movies[o[i].i]]);
    buf_puts(b, "</div>");
    if (!n) list_nothing(b, &lc);
    free(o);
}

static void page_series_list(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib_for(c);
    ctx_title(c, "%s", T("Series"));
    char sub[48];
    count_series(sub, sizeof sub, ix->nseries);
    media_lib_head(c, b, "series", T("Series"), sub, NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    if (!ix->nseries) {
        ui_empty(b, "📺", T("No series yet"), T("Episodes named like \"Show.S01E02.mkv\" are grouped into series."));
        return;
    }
    static const char *const sorts[][2] = {{"", N_("A–Z")}, {"new", N_("Newest")}, {"watched", N_("Watched lately")}};
    ListChoice lc;
    list_choice(c, &lc, "/media/series", sorts, ctx_viewer(c) ? 3 : 2);
    list_controls(c, b, &lc, MEDIA_CATS_SERIES, sorts, ctx_viewer(c) ? 3 : 2);
    Ordered *o = xmalloc((size_t)ix->nseries * sizeof *o);
    int n = 0;
    for (int i = 0; i < ix->nseries; i++) {
        const MediaSeries *s = &ix->series[i];
        MediaWork w;
        media_work_series(s, &w);
        int64_t last;
        if (!show_ok(&lc, series_seen(c, s, &last)) || (lc.cat[0] && !media_cat_in(&w, lc.cat))) continue;
        o[n++] = (Ordered){i, str_eq(lc.sort, "new") ? s->newest : str_eq(lc.sort, "watched") ? last : 0};
    }
    qsort(o, (size_t)n, sizeof *o, ordered_cmp);
    buf_puts(b, "<div class=\"media-grid\">");
    for (int i = 0; i < n; i++) series_card(c, b, &ix->series[o[i].i]);
    buf_puts(b, "</div>");
    if (!n) list_nothing(b, &lc);
    free(o);
}

/* A category's series and films (Watch's home rows' "See all": /media?cat=), grouped like search results. */
static void category_page(Ctx *c, Buf *b, const char *cat) {
    const MediaIndex *ix = media_lib_for(c);
    Hit *hs = xmalloc((size_t)(ix->nseries + 1) * sizeof *hs), *hf = xmalloc((size_t)(ix->nmovies + 1) * sizeof *hf);
    int ns = 0, nf = 0;
    MediaWork w;
    for (int i = 0; i < ix->nseries; i++) {
        media_work_series(&ix->series[i], &w);
        if (media_cat_in(&w, cat)) hs[ns++] = (Hit){0, i, ix->series[i].title};
    }
    for (int i = 0; i < ix->nmovies; i++) {
        const MediaFile *f = &ix->files[ix->movies[i]];
        media_work_film(f, &w);
        if (media_cat_in(&w, cat)) hf[nf++] = (Hit){0, ix->movies[i], f->name.title};
    }
    H(b, "<a class=\"media-back\" href=\"/media\">");
    ui_icon(b, "left");
    H(b, "%s</a><h2 class=\"media-cat-h\">%s</h2>", T("Watch"), media_cat_label(cat));
    hits_group(c, b, ix, T("Series"), hs, ns, 1000, HITS_SERIES);
    hits_group(c, b, ix, T("Movies"), hf, nf, 1000, HITS_VIDEO);
    if (!ns && !nf) ui_empty(b, "🏷️", T("Nothing in this category yet"), NULL);
    free(hs), free(hf);
}

/* Watch's home: a row for each category with titles in it (1.4), the fullest first. */
#define CAT_ROWS 6
static void category_rows(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib_for(c);
    MediaCatUse used[48];
    int nu = media_cats_used(c, MEDIA_CATS_SERIES | MEDIA_CATS_FILMS, used, countof(used));
    int rows[CAT_ROWS], nrows = 0;
    for (int i = 0; i < nu && nrows < CAT_ROWS; i++)
        if (used[i].series + used[i].films >= 2) rows[nrows++] = i; /* (one title is no row) */
    if (!nrows) return;
    /* each title's categories once (series, then films), not once a row */
    int nw = ix->nseries + ix->nmovies;
    char (*of)[MEDIA_CAT_MAX][MEDIA_CAT_LEN] = xmalloc((size_t)(nw ? nw : 1) * sizeof *of);
    int *n = xmalloc((size_t)(nw ? nw : 1) * sizeof *n);
    MediaWork w;
    for (int k = 0; k < nw; k++) {
        if (k < ix->nseries) media_work_series(&ix->series[k], &w);
        else media_work_film(&ix->files[ix->movies[k - ix->nseries]], &w);
        n[k] = media_cats_of(&w, of[k], MEDIA_CAT_MAX);
    }
    for (int r = 0; r < nrows; r++) {
        const char *cat = used[rows[r]].cat;
        Buf more = {0};
        H(&more, "/media?cat=%u", cat);
        shelf_head(b, media_cat_label(cat), buf_cstr(&more));
        buf_free(&more);
        for (int k = 0, shown = 0; k < nw && shown < 14; k++) {
            bool in = false;
            for (int j = 0; j < n[k] && !in; j++) in = str_eq(of[k][j], cat);
            if (!in) continue;
            if (k < ix->nseries) series_card(c, b, &ix->series[k]);
            else video_card(c, b, &ix->files[ix->movies[k - ix->nseries]]);
            shown++;
        }
        shelf_end(b);
    }
    free(of);
    free(n);
}

static void tick_form(Ctx *c, Buf *b, const MediaFile *f, bool done) {
    ui_form(b, "media.watched", "inline");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"done\" value=\"%d\">", f->id,
      done ? 0 : 1);
    const char *say = done ? T("Mark as not watched") : T("Mark as watched");
    H(b, "<button class=\"tick%r\" title=\"%s\" aria-label=\"%s\">", done ? " on" : "", say, say);
    ui_icon(b, "check");
    buf_puts(b, "</button>");
    ui_form_end(b);
}

static void page_series(Ctx *c, Buf *b, const char *idhex) {
    uint64_t id = 0;
    const MediaSeries *s = media_parse_id(idhex, &id) ? media_series(id) : NULL;
    if (!media_kids_series_ok(c, s)) s = NULL; /* (not for a child: as if it weren't there - issue #44) */
    if (!s) {
        ctx_title(c, "%s", T("Series"));
        if (media_tv_mode(c)) media_tv_open(c, b, NULL, NULL);
        ui_empty(b, "📺", T("This series isn't in the library"), T("It may have been moved or renamed."));
        if (c->bare) buf_puts(b, "</div>");
        return;
    }
    if (media_tv_mode(c)) { /* opened from a TV screen */
        media_series_tv(c, b, s);
        return;
    }
    ctx_title(c, "%s", s->title);
    buf_puts(b, "<a class=\"media-back\" href=\"/media/series\">");
    ui_icon(b, "left");
    H(b, "%s</a><div class=\"media-sdetail\"><div class=\"media-sdetail-art\">", T("Series"));
    poster_art(b, s->title, s->year, T("Series"), s->poster, 0, false);
    if (can_set_art(c, s->id)) {
        int sq = (int)ctx_qint(c, "s", -1);
        H(b, "<a class=\"media-pic-edit\" href=\"/media/series/%X?pic=1", s->id);
        if (sq >= 0) H(b, "&amp;s=%d", sq);
        H(b, "\" title=\"%s\">", T("Change the picture"));
        ui_icon(b, "edit");
        H(b, "<span>%s</span></a>", T("Picture"));
    }
    H(b, "</div><div class=\"media-sdetail-t\"><h1>%s</h1><p class=\"sub\">", s->title);
    if (media_seasons(s) > 1) {
        H(b, T("%d seasons"), media_seasons(s));
        buf_puts(b, " · ");
    }
    int eps = media_episodes_in(s);
    H(b, TN(eps, "%d episode", "%d episodes"), eps);
    if (s->year) H(b, " · %d", s->year);
    int seen = watched_count(c, s);
    if (seen) {
        buf_puts(b, " · ");
        H(b, T("%d watched"), seen);
    }
    buf_puts(b, "</p>");
    MediaWork work;
    char here[64];
    media_work_series(s, &work);
    snprintf(here, sizeof here, "/media/series/%016llx", (unsigned long long)s->id);
    media_cats_line(c, b, &work, "/media/series", here); /* (1.4) */
    const MediaFile *up = media_next_up(c, s);
    if (up) {
        const MediaWatch *w = media_watch_of(c, up);
        H(b, "<a class=\"btn primary big\" href=\"/media/watch/%X\">", up->id);
        ui_icon(b, "play");
        H(b, "<span>%s ", w && w->pos > 0 && !w->done ? T("Resume") : seen ? T("Next up") : T("Start watching"));
        media_ep_code(b, up);
        buf_puts(b, "</span></a>");
    }
    media_manage_series_link(c, b, s); /* (admins: Manage - manage.c) */
    if (is_admin(c) && !str_eq(ctx_q(c, "manage"), "1")) { /* into this series' folder (upload.c ?series=) */
        H(b, " <a class=\"btn\" href=\"/media/upload?series=%X\">", s->id);
        ui_icon(b, "plus");
        H(b, "<span>%s</span></a>", T("Add episodes"));
    }
    buf_puts(b, "</div></div>");
    if (media_manage_series(c, b, s)) return; /* ?manage=1 */

    /* season tabs */
    int season = (int)ctx_qint(c, "s", up ? up->name.season : media_episode(s, 0)->name.season);
    if (s->nseasons > 1) {
        buf_puts(b, "<nav class=\"seg media-seasons\">");
        int last = -1;
        for (int i = 0; i < s->count; i++) {
            const MediaFile *e = media_episode(s, i);
            if (e->name.season == last) continue;
            last = e->name.season;
            H(b, "<a href=\"/media/series/%X?s=%d\"%r>", s->id, last, last == season ? " class=\"on\"" : "");
            if (last) H(b, T("Season %d"), last);
            else H(b, "%s", T("Specials"));
            buf_puts(b, "</a>");
        }
        buf_puts(b, "</nav>");
    }
    media_download_series(c, b, s, season); /* "Download season 2 (8.2 GB)", "Download all": grown-ups (download.c) */
    buf_puts(b, "<ol class=\"media-eps\">");
    for (int i = 0; i < s->count; i++) {
        const MediaFile *e = media_episode(s, i);
        if (s->nseasons > 1 && e->name.season != season) continue;
        const MediaWatch *w = media_watch_of(c, e);
        bool done = w && w->done;
        H(b,
          "<li class=\"media-ep%r%r\"><a class=\"media-ep-main\" href=\"/media/watch/%X\">"
          "<span class=\"media-ep-n%r\">%s</span>",
          done ? " media-ep-done" : "", e == up ? " media-ep-up" : "", e->id,
          e->name.episode_end > e->name.episode ? " media-ep-nn" : "", ep_n(e));
        buf_puts(b, "<span class=\"media-ep-t\"><b>");
        media_ep_title(b, e);
        buf_puts(b, "</b><small>");
        media_ep_code(b, e);
        if (e->probe.duration_ms) {
            buf_puts(b, " · ");
            duration(b, e);
        }
        if (e->name.unnumbered && e->name.disc) { /* numbered in disc order */
            buf_puts(b, " · ");
            H(b, T("disc %d"), e->name.disc);
        }
        if (media_playable(e)->probe.playable == PLAY_NO)
            H(b, " · <span class=\"media-warn\">%s</span>", T("needs conversion"));
        buf_puts(b, "</small>");
        int p = media_percent(w);
        if (p && !done) H(b, "<span class=\"media-ep-bar\"><i style=\"width:%d%%\"></i></span>", p);
        buf_puts(b, "</span></a>");
        if (ctx_viewer(c)) tick_form(c, b, e, done); /* (a display account with nobody picked: no ticks) */
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ol>");

    /* the season's bonus material from ripped discs: "play all" titles and clips (library.c) */
    const MediaIndex *ix = media_lib_for(c);
    int nx = 0;
    for (int i = 0; i < ix->nextras; i++) {
        const MediaFile *x = &ix->files[ix->extras[i]];
        if (x->series != s->id || (s->nseasons > 1 && x->name.season != season)) continue;
        if (!nx++) H(b, "<h2 class=\"mt\">%s</h2><ol class=\"media-eps media-eps-compact\">", T("Extras"));
        H(b, "<li class=\"media-ep\"><a class=\"media-ep-main\" href=\"/media/watch/%X\"><span class=\"media-ep-n\">",
          x->id);
        ui_icon(b, x->role == MF_PLAYALL ? "play" : "film");
        buf_puts(b, "</span><span class=\"media-ep-t\"><b>");
        if (x->role == MF_PLAYALL) H(b, "%s", T("All episodes in one"));
        else if (x->name.ep_title[0]) html_esc(b, x->name.ep_title);
        else if (x->name.track >= 0) H(b, T("Extra · title %d"), x->name.track);
        else H(b, "%s", T("Extra"));
        buf_puts(b, "</b><small>");
        if (x->name.disc) H(b, T("Disc %d"), x->name.disc);
        if (x->name.disc && x->probe.duration_ms) buf_puts(b, " · ");
        duration(b, x);
        buf_puts(b, "</small></span></a></li>");
    }
    if (nx) buf_puts(b, "</ol>");

    if (str_eq(ctx_q(c, "pic"), "1") && can_set_art(c, s->id)) {
        char close[64];
        int sq = (int)ctx_qint(c, "s", -1);
        if (sq >= 0) snprintf(close, sizeof close, "/media/series/%016llx?s=%d", (unsigned long long)s->id, sq);
        else snprintf(close, sizeof close, "/media/series/%016llx", (unsigned long long)s->id);
        art_modal(c, b, s->id, s->title, s->year, T("Series"), s->poster, close);
    }
    media_cats_modal(c, b, &work, s->title, here); /* ?cats=1 */
}

/* Folder browser over the index: /media/browse?p=<root>:<dir> */
typedef struct {
    char name[200];
    int videos;
} Sub;

static void page_browse(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "%s", T("Folders"));
    media_lib_head(c, b, "browse", T("Folders"), NULL, NULL);
    const char *q = query(c);
    if (*q) {
        search_results(c, b, q);
        return;
    }
    if (library_empty_states(c, b)) return;
    const char *p = ctx_q(c, "p");
    int root = -1;
    char dir[400] = "";
    if (*p) {
        root = (int)strtol(p, NULL, 10);
        const char *colon = strchr(p, ':');
        str_copy(dir, sizeof dir, colon ? colon + 1 : "");
        if (root < 0 || root >= ix->nroots) root = -1;
    }
    if (root < 0 && ix->nroots == 1) root = 0;
    if (root < 0) {
        buf_puts(b, "<ul class=\"media-folders\">");
        for (int r = 0; r < ix->nroots; r++) {
            H(b, "<li><a href=\"/media/browse?p=%d:\">", r);
            ui_icon(b, "folder");
            H(b, "<span>%s</span></a></li>", ix->roots[r]);
        }
        buf_puts(b, "</ul>");
        return;
    }
    /* breadcrumbs */
    H(b, "<nav class=\"media-crumbs\"><a href=\"/media/browse?p=%d:\">%s</a>", root,
      ix->nroots > 1 ? ix->roots[root] : T("Library"));
    char acc[400] = "";
    for (const char *s = dir; *s;) {
        size_t n = strcspn(s, "/");
        char seg[200];
        str_ncopy(seg, sizeof seg, s, n);
        size_t al = strlen(acc);
        snprintf(acc + al, sizeof acc - al, "%s%s", al ? "/" : "", seg);
        H(b, "<span>/</span><a href=\"/media/browse?p=%d:%u\">%s</a>", root, acc, seg);
        s += n;
        if (*s == '/') s++;
    }
    buf_puts(b, "</nav>");
    /* "Not for children" (issue #44): this folder and everything in it - an admin's mark */
    const char *above = media_kids_covered(ix->roots[root], dir);
    if (is_admin(c)) {
        bool on = media_kids_marked(ix->roots[root], dir);
        char p_arg[420];
        snprintf(p_arg, sizeof p_arg, "%d:%s", root, dir);
        ui_form(b, "media.kids", "media-kids");
        ui_hidden(b, "p", p_arg);
        if (above) {
            buf_puts(b, "<p class=\"media-kids-on\">");
            H(b, T("Not for children: <a href=\"/media/browse?p=%d:%u\">%s</a> is marked, and this is in it."), root, above,
              above[0] ? above : ix->roots[root]);
            buf_puts(b, "</p>");
        } else {
            H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"on\" value=\"1\" data-auto%r> %s "
                 "<span class=\"muted media-small\">%s</span></label>",
              on ? " checked" : "", T("Not for children"),
              dir[0] ? T("- this folder and everything in it stays out of a child's Watch")
                     : T("- the whole library folder and everything in it stays out of a child's Watch"));
        }
        ui_form_end(b);
    }

    size_t dl = strlen(dir);
    Sub *subs = NULL;
    int nsubs = 0, capsubs = 0, nvid = 0;
    buf_puts(b, "<div class=\"media-grid\" id=\"media-files\">");
    for (int i = 0; i < ix->nfiles; i++) {
        const MediaFile *f = &ix->files[i];
        /* a play-everywhere copy is shown as its original; a play-all's header isn't a video */
        if (f->root != root || f->type != MF_VIDEO || f->role == MF_WEBCOPY || media_is_playall_head(f->rel)) continue;
        if (!media_kids_ok(c, f)) continue; /* (a child's: not what's marked - issue #44) */
        const char *rest = f->rel;
        if (dl) {
            if (strncmp(f->rel, dir, dl) != 0 || f->rel[dl] != '/') continue;
            rest = f->rel + dl + 1;
        }
        const char *slash = strchr(rest, '/');
        if (!slash) {
            video_card(c, b, f);
            nvid++;
            continue;
        }
        char name[200];
        str_ncopy(name, sizeof name, rest, (size_t)(slash - rest));
        int k = 0;
        while (k < nsubs && !str_eq(subs[k].name, name)) k++;
        if (k == nsubs) {
            if (nsubs == capsubs) subs = xrealloc(subs, (size_t)(capsubs = capsubs ? capsubs * 2 : 16) * sizeof *subs);
            str_copy(subs[nsubs].name, sizeof subs[nsubs].name, name);
            subs[nsubs++].videos = 0;
        }
        subs[k].videos++;
    }
    buf_puts(b, "</div>");
    if (nsubs) {
        buf_puts(b, "<ul class=\"media-folders\">");
        for (int k = 0; k < nsubs; k++) {
            char next[620];
            snprintf(next, sizeof next, "%s%s%s", dir, dl ? "/" : "", subs[k].name);
            H(b, "<li><a href=\"/media/browse?p=%d:%u\">", root, next);
            ui_icon(b, "folder");
            H(b, "<span>%s</span><small>", subs[k].name);
            H(b, TN(subs[k].videos, "%d video", "%d videos"), subs[k].videos);
            if (is_admin(c) && media_kids_marked(ix->roots[root], next)) H(b, " · %s", T("not for children"));
            buf_puts(b, "</small></a></li>");
        }
        buf_puts(b, "</ul>");
    }
    if (!nsubs && !nvid) ui_empty(b, "📂", T("Nothing to play here"), NULL);
    free(subs);
}

/* ----------------------------------------------------------------- removing */

/* Admins take a film (or another video) out of the library for good: remove.c deletes its files
   - and its own folder, when it has one. Episodes belong to their series and aren't removed one by one. */
static bool can_remove(const Ctx *c, const MediaFile *f) {
    return is_admin(c) && f->type == MF_VIDEO && f->role == MF_MAIN && f->name.kind != MEDIA_EPISODE;
}

/* "1.5 GB" -> "1,5 GB" in Swedish and Spanish */
static void decimal_mark(char *s) {
    char d = i18n_decimal();
    for (; d != '.' && *s; s++)
        if (*s == '.') *s = d;
}

static void size_text(int64_t n, char *out, size_t cap) {
    if (n >= 1000LL * 1048576) snprintf(out, cap, "%.1f GB", (double)n / 1073741824.0);
    else if (n >= 1048576) snprintf(out, cap, "%lld MB", (long long)(n / 1048576));
    else snprintf(out, cap, "%lld KB", (long long)((n + 1023) / 1024));
    decimal_mark(out);
}

static void remove_modal(Ctx *c, Buf *b, const MediaFile *f, const char *close) {
    (void)c;
    RemovePlan p;
    if (!media_remove_plan(f, &p)) {
        media_remove_free(&p);
        return;
    }
    char size[24];
    size_text(p.bytes, size, sizeof size);
    ui_modal(b, T("Remove from the library"), close);
    buf_puts(b, "<p>");
    if (f->name.year)
        H(b, T("<b>%s</b> (%d) is deleted from the disk - for everyone in the household, and it can't be undone."),
          f->name.title, f->name.year);
    else H(b, T("<b>%s</b> is deleted from the disk - for everyone in the household, and it can't be undone."), f->name.title);
    buf_puts(b, "</p><p class=\"muted\">");
    if (p.folder[0])
        H(b,
          TN(p.files, "Its folder <code class=\"media-code\">%s</code> goes with everything in it: %d file, %s.",
             "Its folder <code class=\"media-code\">%s</code> goes with everything in it: %d files, %s."),
          p.folder, p.files, size);
    else H(b, TN(p.files, "%d file, %s:", "%d files, %s:"), p.files, size);
    buf_puts(b, "</p>");
    buf_puts(b, "<ul class=\"media-rm-list\">");
    int shown = 0;
    size_t skip = p.folder[0] ? strlen(p.folder) + 1 : 0;
    for (int i = 0; i < p.n && shown < 12; i++) {
        if (p.items[i].dir) continue;
        const char *rel = p.items[i].rel;
        H(b, "<li><code class=\"media-code\">%s</code></li>", strlen(rel) > skip ? rel + skip : rel);
        shown++;
    }
    if (p.files > shown) more_row(b, "<li class=\"muted\">", p.files - shown);
    buf_puts(b, "</ul>");
    ui_form(b, "media.remove", "inline");
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><div class=\"modal-f\"><a class=\"btn\" "
         "href=\"%s\">%s</a><button class=\"btn danger\">%s</button></div>",
      f->id, close, T("Keep it"), T("Delete from the disk"));
    ui_form_end(b);
    ui_modal_end(b);
    media_remove_free(&p);
}

void media_upnext(Buf *b, const MediaFile *next, const char *href, bool tv) {
    H(b, "<div class=\"media-upnext\" id=\"media-upnext\" data-upnext=\"%d\" data-static=\"%X\" hidden%r>"
         "<p class=\"media-upnext-t\"><small>",
      MEDIA_UPNEXT_S, next->id, tv ? " data-arrows" : "");
    H(b, T("Next episode in <span data-count>%d</span> s"), MEDIA_UPNEXT_S);
    buf_puts(b, "</small><b>");
    media_ep_code(b, next);
    buf_puts(b, " · ");
    media_ep_title(b, next);
    H(b, "</b></p><div class=\"media-upnext-a\"><a class=\"btn primary\" href=\"%r\"%r>", href, tv ? " autofocus" : "");
    ui_icon(b, "right");
    H(b, "<span>%s</span></a><button type=\"button\" class=\"btn\" data-hide>%s</button></div></div>", T("Play now"),
      T("Hide"));
}

static void page_watch(Ctx *c, Buf *b, const char *idhex) {
    uint64_t id = 0;
    const MediaFile *f = media_parse_id(idhex, &id) ? media_file(id) : NULL;
    if (!media_kids_ok(c, f)) f = NULL; /* (issue #44) */
    if (!f || f->type != MF_VIDEO) {
        ctx_title(c, "%s", T("Watch"));
        if (media_tv_mode(c)) media_tv_open(c, b, NULL, NULL);
        ui_empty(b, "🎬", T("This video isn't in the library"), T("It may have been moved, renamed or deleted."));
        if (c->bare) buf_puts(b, "</div>");
        return;
    }
    if (media_tv_mode(c)) { /* opened from a TV screen: the player, full screen */
        media_watch_tv(c, b, f);
        return;
    }
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    const MediaFile *prev = media_neighbour(f, -1), *next = media_neighbour(f, +1);
    const MediaWatch *w = media_watch_of(c, f);
    char title[300];
    media_title(f, title, sizeof title);
    if (s) {
        Buf t = {0};
        buf_printf(&t, "%s ", f->name.title);
        media_ep_code(&t, f);
        ctx_title(c, "%s", buf_cstr(&t));
        buf_free(&t);
        H(b, "<a class=\"media-back\" href=\"/media/series/%X?s=%d\">", s->id, f->name.season);
        ui_icon(b, "left");
        H(b, "%s</a>", s->title);
    } else {
        ctx_title(c, "%s", title);
        H(b, "<a class=\"media-back\" href=\"%s\">", f->name.kind == MEDIA_MOVIE ? "/media/movies" : "/media/browse");
        ui_icon(b, "left");
        H(b, "%s</a>", f->name.kind == MEDIA_MOVIE ? T("Movies") : T("Folders"));
    }

    /* the player: data-static="<the video>" keeps live re-renders away from it, and the next
       episode morphs into the same element - full screen (the box: the card shows over it) stays.
       Our own controls (media_controls); the browser's only without wire.js */
    bool autoplay = str_eq(ctx_q(c, "auto"), "1");
    Buf go = {0};
    if (next) H(&go, "/media/watch/%X?auto=1", next->id);
    int at = autoplay ? 0 : media_resume_at(w);
    char src[120], vkey[24], here_q[64];
    media_file_src_at(c, f->id, at, src, sizeof src);
    media_video_key(c, f, vkey, sizeof vkey); /* (with its sound: a new pick morphs it in place - 2.2) */
    snprintf(here_q, sizeof here_q, "/media/watch/%016llx?asked=1", (unsigned long long)f->id);
    if (media_sound_question(c, b, f, here_q)) autoplay = false;
    H(b, "<div class=\"media-player-box\" id=\"media-box\"><video id=\"media-video\" class=\"media-player\" "
         "src=\"%s\" controls preload=\"metadata\" playsinline data-static=\"%s\" data-id=\"%X\" "
         "data-report=\"media.progress\"",
      src, vkey, f->id);
    if (at) H(b, " data-resume=\"%d\"", at);
    if (next) H(b, " data-next=\"%r\"", buf_cstr(&go));
    if (autoplay) buf_puts(b, " autoplay");
    uint64_t poster = f->poster ? f->poster : s ? s->poster : 0;
    if (poster) {
        buf_puts(b, " poster=\"");
        media_img_src(b, poster);
        buf_putc(b, '"');
    }
    buf_putc(b, '>');
    media_tracks(c, b, f);
    buf_puts(b, "</video>");
    if (next) media_upnext(b, next, buf_cstr(&go), false);
    media_controls(c, b, f, false);
    buf_puts(b, "</div>");
    buf_free(&go);

    /* about it */
    buf_puts(b, "<div class=\"media-watch-info\"><div class=\"media-watch-t\">");
    if (s) {
        H(b, "<h1>%s</h1><p class=\"sub\">", f->name.ep_title[0] ? f->name.ep_title : f->name.title);
        media_ep_code(b, f);
        H(b, " · %s", s->title);
    } else {
        H(b, "<h1>%s</h1><p class=\"sub\">", title);
        if (f->name.year) H(b, "%d", f->name.year);
    }
    if (f->probe.duration_ms) {
        buf_puts(b, " · ");
        duration(b, f);
    }
    Buf q = {0};
    quality(&q, f);
    if (q.len) H(b, " · %s", buf_cstr(&q));
    buf_free(&q);
    buf_puts(b, "</p>");
    MediaWork work;
    char here[64];
    snprintf(here, sizeof here, "/media/watch/%016llx", (unsigned long long)f->id);
    if (f->name.kind == MEDIA_MOVIE) { /* its categories (1.4): links to Movies with each, and the dialog's */
        media_work_film(f, &work);
        media_cats_line(c, b, &work, "/media/movies", here);
    }
    buf_puts(b, "</div><div class=\"ph-a\">");
    if (prev) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X\" title=\"%s\">", prev->id, T("Previous episode"));
        ui_icon(b, "left");
        H(b, "<span>%s</span></a>", T("Previous"));
    }
    if (next) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X\" title=\"%s\"><span>%s</span>", next->id, T("Next episode"), T("Next"));
        ui_icon(b, "right");
        buf_puts(b, "</a>");
    }
    if (ctx_viewer(c)) {
        bool done = w && w->done;
        ui_form(b, "media.watched", "inline");
        H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><input type=\"hidden\" name=\"done\" value=\"%d\">", f->id,
          done ? 0 : 1);
        H(b, "<button class=\"btn%r\" aria-pressed=\"%r\">", done ? " media-watched" : "", done ? "true" : "false");
        ui_icon(b, "check");
        H(b, "<span>%s</span></button>", done ? T("Watched") : T("Mark watched"));
        ui_form_end(b);
    }
    if (can_set_art(c, f->id)) { /* films and other videos; episodes use their series' picture */
        H(b, "<a class=\"btn\" href=\"/media/watch/%X?pic=1\" title=\"%s\">", f->id, T("Change the picture"));
        ui_icon(b, "edit");
        H(b, "<span>%s</span></a>", T("Picture"));
    }
    if (can_remove(c, f)) {
        H(b, "<a class=\"btn\" href=\"/media/watch/%X?remove=1\" title=\"%s\">", f->id, T("Delete it from the library"));
        ui_icon(b, "trash");
        H(b, "<span>%s</span></a>", T("Remove"));
    }
    media_manage_watch_button(c, b, f); /* (admins: Fix title - manage.c) */
    buf_puts(b, "</div></div>");

    static const char *const cls[] = {"", "ok", "warn", "bad"};
    const MediaFile *pf = media_playable(f); /* the play-everywhere copy, when there is one */
    Playable pl = pf->probe.playable;
    /* an HEVC or AV1 copy is as far as Hemma goes (no decoder for either): say where it plays, not just "may not" */
    bool newer = pl == PLAY_MAYBE && !str_eq(pf->probe.container, "mkv"), av1 = newer && str_eq(pf->probe.vcodec, "av1");
    newer = av1 || (newer && str_eq(pf->probe.vcodec, "hevc"));
    H(b, "<div class=\"media-playable\"><span class=\"badge %s\">%s</span>", cls[CLAMP((int)pl, 0, 3)],
      newer ? T("Plays on newer devices") : T(media_playable_label(pl)));
    if (newer)
        H(b, "<span class=\"muted\">%s%s%s</span>", pf != f ? T("Converted copy") : "", pf != f ? " · " : "",
          av1 ? T("AV1 video: Chrome, Edge and Firefox on computers and phones, most TVs from the last few years, and "
                  "the newest iPhones, iPads and Macs")
              : T("HEVC video: Safari on iPhone, iPad and Mac, and Chrome or Edge on most computers and phones from the "
                  "last few years"));
    else if (pf != f) H(b, "<span class=\"muted\">%s</span>", T("Converted copy"));
    else if (pl != PLAY_YES && pf->probe.why[0]) {
        char why[200];
        media_probe_why(&pf->probe, why, sizeof why);
        H(b, "<span class=\"muted\">%s</span>", why);
    }
    if (pf->probe.container[0] || pf->probe.vcodec[0]) { /* the codecs folded away: plain words first (issue #62) */
        H(b, "<details class=\"media-tech\"><summary>%s</summary><span>", T("File details"));
        tech_facts(b, pf);
        buf_puts(b, "</span></details>");
    }
    media_download_link(c, b, f); /* "Download (1.2 GB)": grown-ups only (download.c) */
    buf_puts(b, "</div>");
    ConvertNeed need = convert_need(f);
    if (need != CONV_NONE) { /* Hemma makes a copy that plays everywhere, once */
        buf_puts(b, "<div class=\"media-convert\">");
        if (convert_queued(f->id))
            H(b, "<p class=\"muted\">%s</p>", T("Being made playable everywhere - see the library settings."));
        else if (is_admin(c)) {
            bool hevc = str_eq(f->probe.vcodec, "hevc"), dolby = str_eq(f->probe.acodec, "ac3") || str_eq(f->probe.acodec, "eac3");
            bool dts = str_eq(f->probe.acodec, "dts"), av1 = str_eq(f->probe.vcodec, "av1"); /* (both copied: no decoder) */
            H(b, "<p class=\"muted\">%s</p>",
              hevc && dts ? T("Its DTS sound plays in no browser, and Safari can't open MKV files: a copy with AAC sound fixes "
                              "both - quick, the picture isn't re-encoded. HEVC still needs a device that can decode it (most "
                              "from the last few years).")
              : hevc && dolby ? T("Its Dolby sound plays silently in Chrome and Firefox, and Safari can't open MKV files: a copy "
                                "with AAC sound fixes both - quick, the picture isn't re-encoded. HEVC still needs a device "
                                "that can decode it (most from the last few years).")
              : hevc ? T("Safari (iPhone, iPad, Mac) can't open MKV files: a new container fixes that - quick, the picture "
                         "isn't re-encoded. HEVC still needs a device that can decode it (most from the last few years).")
              : av1 && dts ? T("Its DTS sound plays in no browser, and Safari can't open MKV files: a copy with AAC sound fixes "
                               "both - quick, the picture isn't re-encoded. AV1 still needs a device that can decode it "
                               "(Chrome, Edge and Firefox can; of Apple's, only the newest).")
              : av1 && dolby ? T("Its Dolby sound plays silently in Chrome and Firefox, and Safari can't open MKV files: a copy "
                                 "with AAC sound fixes both - quick, the picture isn't re-encoded. AV1 still needs a device "
                                 "that can decode it (Chrome, Edge and Firefox can; of Apple's, only the newest).")
              : av1 ? T("Safari (iPhone, iPad, Mac) can't open MKV files: a new container fixes that - quick, the picture "
                        "isn't re-encoded. AV1 still needs a device that can decode it (Chrome, Edge and Firefox can; of "
                        "Apple's, only the newest).")
              : need == CONV_REMUX ? T("A new container makes this play on iPhones and every TV too - quick, nothing is "
                                       "re-encoded.")
                                   : T("Hemma can convert this once into a copy that plays on every browser, iPhone and TV."));
            char hex[17];
            media_id_hex(f->id, hex);
            ui_act(b, "media.convert", "btn",
                   (hevc || av1) && (dolby || dts) ? T("Fix the sound")
                   : hevc                          ? T("Make it play in Safari too")
                   : av1                           ? T("Make it play on more devices")
                                                   : T("Make it play everywhere"),
                   "id", hex, NULL);
        } else {
            buf_puts(b, "<p class=\"muted\">");
            ask_admins(b, T("Hemma can make a copy of this that plays everywhere: ask %r."));
            buf_puts(b, "</p>");
        }
        buf_puts(b, "</div>");
    } else if (pl == PLAY_NO && pf == f) {
        H(b, "<p class=\"muted media-convert\">%s</p>", T("This format can't be converted by Hemma yet."));
    }

    /* the rest of the season, for binge-friendly navigation */
    if (s && s->count > 1) {
        H(b, "<h2 class=\"mt\">%s</h2><ol class=\"media-eps media-eps-compact\">", T("More episodes"));
        for (int i = 0; i < s->count; i++) {
            const MediaFile *e = media_episode(s, i);
            if (e->name.season != f->name.season) continue;
            const MediaWatch *ew = media_watch_of(c, e);
            H(b,
              "<li class=\"media-ep%r%r\"><a class=\"media-ep-main\" href=\"/media/watch/%X\">"
              "<span class=\"media-ep-n%r\">%s</span>",
              ew && ew->done ? " media-ep-done" : "", e == f ? " media-ep-up" : "", e->id,
              e->name.episode_end > e->name.episode ? " media-ep-nn" : "", ep_n(e));
            buf_puts(b, "<span class=\"media-ep-t\"><b>");
            media_ep_title(b, e);
            buf_puts(b, "</b><small>");
            media_ep_code(b, e);
            if (e->probe.duration_ms) {
                buf_puts(b, " · ");
                duration(b, e);
            }
            buf_puts(b, "</small></span></a></li>");
        }
        buf_puts(b, "</ol>");
    }

    if (str_eq(ctx_q(c, "pic"), "1") && can_set_art(c, f->id)) {
        char close[64];
        snprintf(close, sizeof close, "/media/watch/%016llx", (unsigned long long)f->id);
        art_modal(c, b, f->id, f->name.title, f->name.year, f->name.kind == MEDIA_OTHER ? T("Video") : NULL, f->poster,
                  close);
    }
    if (str_eq(ctx_q(c, "remove"), "1") && can_remove(c, f)) {
        char close[64];
        snprintf(close, sizeof close, "/media/watch/%016llx", (unsigned long long)f->id);
        remove_modal(c, b, f, close);
    }
    media_manage_watch_dialog(c, b, f); /* ?fix=1 */
    if (f->name.kind == MEDIA_MOVIE) media_cats_modal(c, b, &work, title, here); /* ?cats=1 */
}

/* Who may rescan the library: admins, and the members an admin lets (setting "media.rescanners",
   their ids comma-separated - a setting only the admin-only media.rescanners writes, not a pref
   people set for themselves). Display accounts never. An id counts only while it is a member's
   who can sign in: one who left (ids are never handed out again) or changed role drops out. */
#define RESCANNERS "media.rescanners"

static bool can_sign_in_member(const Person *p) { return p && p->role == ROLE_MEMBER && p->username[0]; }

static bool rescan_granted(int person) {
    if (!can_sign_in_member(person_get(person))) return false;
    for (const char *s = setting(RESCANNERS, ""); *s;) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s) end++; /* a separator */
        else if (v == person) return true;
        s = end;
    }
    return false;
}

static bool can_rescan(const Ctx *c) { return is_admin(c) || (c->me && rescan_granted(c->me->id)); }

static void rescanners_form(Buf *b) { /* admins: who else may rescan */
    int members = 0;
    for (int i = 0; i < person_count(); i++) members += can_sign_in_member(person_at(i));
    if (!members) return; /* nobody but admins to choose from */
    ui_form(b, "media.rescanners", "stack");
    H(b, "<p class=\"muted media-small\">%s</p>", T("Admins can always rescan. Who else may:"));
    for (int i = 0; i < person_count(); i++) {
        const Person *p = person_at(i);
        if (!can_sign_in_member(p)) continue;
        H(b, "<label class=\"check\"><input type=\"checkbox\" name=\"p\" value=\"%d\" data-auto%r> %s</label>", p->id,
          rescan_granted(p->id) ? " checked" : "", p->name);
    }
    ui_form_end(b);
}

/* Videos the scan left out (issue #28: a film named "Sample Film (2021)" vanished without a word). */
static void left_card(Ctx *c, Buf *b, const MediaIndex *ix) {
    if (!is_admin(c) || !ix->nleft_all) return;
    ui_card(b, T("Left out"), NULL, NULL);
    buf_puts(b, "<p class=\"muted\">");
    H(b,
      TN(ix->nleft_all,
         "%d video is left out of the library: by its name or folder, it looks like trailers, extras or samples (a short "
         "preview clip). A video named like one stays in when it runs long, or when Hemma can't tell how long it runs. "
         "One that belongs in? Rename it, and rescan.",
         "%d videos are left out of the library: by their name or folder, they look like trailers, extras or samples (a "
         "short preview clip). A video named like one stays in when it runs long, or when Hemma can't tell how long it "
         "runs. One that belongs in? Rename it, and rescan."),
      ix->nleft_all);
    H(b, "</p><details class=\"media-left\"><summary>%s</summary><ul class=\"rows\">", T("Which ones"));
    for (int i = 0; i < ix->nleft; i++) {
        const MediaLeft *l = &ix->left[i];
        H(b, "<li class=\"row\"><div class=\"row-main\"><code class=\"media-code\">%s</code><small class=\"muted\">%s"
             "</small></div></li>",
          l->rel,
          l->why == MEDIA_LEFT_SAMPLE  ? T("starts with \"sample\" and runs under 10 minutes: a preview clip")
          : l->why == MEDIA_LEFT_NAMED ? T("named like a trailer or extra (\"-trailer\", \".sample\") and runs under 40 minutes")
                                       : T("in a folder for extras (Extras, Trailers, Featurettes...)"));
    }
    if (ix->nleft_all > ix->nleft) more_row(b, "<li class=\"row muted\">", ix->nleft_all - ix->nleft);
    buf_puts(b, "</ul></details>");
    ui_card_end(b);
}

/* Uploads that never finished (issue #40): their ".part" files stayed for good, as big as a film,
   and nothing in Hemma showed them. Listed once nobody has touched them for MEDIA_PART_DAYS. */
static void parts_card(Ctx *c, Buf *b, const MediaIndex *ix) {
    if (!is_admin(c) || !ix->nparts_all) return;
    char sz[24];
    media_size_text(ix->parts_bytes, sz, sizeof sz);
    ui_card(b, T("Unfinished uploads"), NULL, NULL);
    buf_puts(b, "<p class=\"muted\">");
    H(b,
      TN(ix->nparts_all, "%d upload never finished, and nobody has touched it for over a week: %s on the disk.",
         "%d uploads never finished, and nobody has touched them for over a week: %s on the disk."),
      ix->nparts_all, sz);
    H(b, "</p><details class=\"media-left\"><summary>%s</summary><ul class=\"rows\">", T("Which ones"));
    for (int i = 0; i < ix->nparts; i++) {
        const MediaLeft *l = &ix->parts[i];
        char size[24], day[11];
        media_size_text(l->size, size, sizeof size);
        fmt_ymd(l->mtime, day);
        H(b, "<li class=\"row\"><div class=\"row-main\"><code class=\"media-code\">%s</code><small class=\"muted\">", l->rel);
        H(b, T("%s, last written %s"), size, day);
        buf_puts(b, "</small></div></li>");
    }
    if (ix->nparts_all > ix->nparts) more_row(b, "<li class=\"row muted\">", ix->nparts_all - ix->nparts);
    buf_puts(b, "</ul></details>");
    ui_form_confirm(b, "media.parts_remove", "inline", T("Remove these unfinished uploads? They can't be resumed after."));
    H(b, "<button class=\"btn danger\">%s</button>", T("Remove them"));
    ui_form_end(b);
    ui_card_end(b);
}

static void act_parts_remove(Ctx *c, Fields *f) {
    (void)f;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    const MediaIndex *ix = media_lib();
    int removed = 0;
    int64_t bytes = 0;
    int more = ix->nparts_all - ix->nparts;
    for (int i = 0; i < ix->nparts; i++) { /* each one again: still there, still untouched, no upload's */
        const MediaLeft *l = &ix->parts[i];
        char path[800], video[800];
        int64_t size = 0, mtime = 0;
        snprintf(path, sizeof path, "%s/%s", ix->roots[l->root], l->rel);
        if (!str_iends(path, MEDIA_PART_SUFFIX)) continue; /* (only Hemma's own: issue #89) */
        /* upload_claims knows the video by its own name, not its half-sent file's (issue #89: it never matched) */
        snprintf(video, sizeof video, "%.*s", (int)(strlen(path) - (sizeof MEDIA_PART_SUFFIX - 1)), path);
        if (!path_stat(path, &size, &mtime, NULL) || c->now - mtime <= MEDIA_PART_DAYS * 86400LL || upload_claims(video))
            continue;
        if (file_remove(path)) removed++, bytes += size;
    }
    char sz[24], logged[24];
    Lang was = i18n_use(LANG_EN); /* (the log speaks English: "1.2 GB") */
    media_size_text(bytes, logged, sizeof logged);
    i18n_use(was);
    media_size_text(bytes, sz, sizeof sz);
    if (removed)
        log_info("media: %s removed %d unfinished upload%s (%s)", c->me->name, removed, removed == 1 ? "" : "s", logged);
    media_scan_start(); /* (the list comes again from the disk) */
    if (more > 0)
        ctx_ok(c,
               TN(removed, "Removed %d unfinished upload (%s) - %d more show here after the scan",
                  "Removed %d unfinished uploads (%s) - %d more show here after the scan"),
               removed, sz, more);
    else
        ctx_ok(c, TN(removed, "Removed %d unfinished upload: %s free again", "Removed %d unfinished uploads: %s free again"),
               removed, sz);
}

/* Watch history that found no video after the last scan (issue #93: it used to stay behind without a
   word) - matched by place, content and name already; what's left is a video deleted, or renamed and
   changed at once. */
static void history_card(Ctx *c, Buf *b) {
    if (!is_admin(c)) return;
    char labels[8][100];
    bool away = false;
    int n = media_history_lost(labels, countof(labels), &away);
    if (!n) return;
    ui_card(b, T("Watch history without its video"), NULL, NULL);
    buf_puts(b, "<p class=\"muted\">");
    H(b,
      TN(n,
         "The household watched %d video that isn't in the library any more, and it couldn't be found under another "
         "name or folder.",
         "The household watched %d videos that aren't in the library any more, and they couldn't be found under another "
         "name or folder."),
      n);
    if (away) H(b, " %s", T("A library folder is missing or empty right now: they may come back with its disk."));
    buf_puts(b, "</p><ul class=\"rows\">");
    int named = 0;
    for (int i = 0; i < n && i < countof(labels); i++)
        if (labels[i][0]) H(b, "<li class=\"row\"><div class=\"row-main\">%s</div></li>", labels[i]), named++;
    if (n > named) more_row(b, "<li class=\"row muted\">", n - named);
    buf_puts(b, "</ul>");
    if (!away) {
        ui_form_confirm(b, "media.history_forget", "inline",
                        T("Forget this watch history? Continue watching and watched ticks for these videos go."));
        H(b, "<button class=\"btn\">%s</button>", T("Forget it"));
        ui_form_end(b);
    }
    ui_card_end(b);
}

static void act_history_forget(Ctx *c, Fields *f) {
    (void)f;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    int n = media_history_forget();
    if (n) log_info("media: %s forgot the watch history of %d video%s no longer in the library", c->me->name, n,
                    n == 1 ? "" : "s");
    ctx_ok(c, TN(n, "Forgot the history of %d video", "Forgot the history of %d videos"), n);
}

/* Library settings: the folders marked "Not for children" (issue #44; marked on the Folders tab) */
static void kids_card(Buf *b) {
    ui_card(b, T("Not for children"), "/media/browse", T("Folders"));
    int n = media_kids_count();
    if (!n) {
        H(b, "<p class=\"muted\">%r</p>",
          T("Children see the whole library. To keep a folder out of their Watch, open it under "
            "<a href=\"/media/browse\">Folders</a> and tick <i>Not for children</i>."));
        ui_card_end(b);
        return;
    }
    H(b, "<p class=\"muted\">%s</p><ul class=\"rows media-kids-list\">",
      T("Left out of a child's Watch - and of the TV while a child is picked there:"));
    for (int i = 0; i < n; i++) {
        char root[300], dir[400];
        if (!media_kids_at(i, root, sizeof root, dir, sizeof dir)) continue;
        H(b, "<li class=\"row\"><div class=\"row-main\"><code class=\"media-code\">%s%r%s</code></div>", root,
          dir[0] ? "/" : "", dir);
        ui_form(b, "media.kids", "inline");
        ui_hidden(b, "root", root);
        ui_hidden(b, "dir", dir);
        buf_puts(b, "<button class=\"btn sm\" aria-label=\"");
        H(b, T("Open %s to children"), dir[0] ? dir : root);
        H(b, "\">%s</button>", T("Open to children"));
        ui_form_end(b);
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ul>");
    ui_card_end(b);
}

static void page_settings(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    ctx_title(c, "%s", T("Library settings"));
    buf_puts(b, "<a class=\"media-back\" href=\"/media\">");
    ui_icon(b, "left");
    H(b, "%s</a>", T("Watch"));
    ui_head(b, T("Library"), T("Where your movies and series live"));
    ui_head_end(b);
    buf_puts(b, "<div class=\"grid2\">");

    ui_card(b, T("Folders"), NULL, NULL);
    if (is_admin(c)) {
        ui_form(b, "media.roots", "stack");
        H(b, "<label class=\"field\"><span>%s</span><textarea name=\"roots\" rows=\"4\" "
             "spellcheck=\"false\" placeholder=\"%r\">",
          T("One folder per line"), T("/mnt/media&#10;D:\\Movies"));
        for (int r = 0; r < ix->nroots; r++) H(b, "%s%s", r ? "\n" : "", ix->roots[r]);
        H(b, "</textarea></label><p class=\"muted media-small\">%r</p><div class=\"modal-f\"><button class=\"btn primary\">"
             "%s</button></div>",
          T("On the Raspberry Pi that's where the drive is mounted, e.g. <code class=\"media-code\">/mnt/media</code>. "
            "A network share works too once it's mounted."),
          T("Save & scan"));
        ui_form_end(b);
        Suggest s[8];
        int ns = suggestions(s, countof(s)); /* (issue #27) */
        if (ns) {
            H(b, "<div class=\"media-suggest\" id=\"media-suggest\"><p class=\"muted media-small\">%s</p><div class=\"chips\">",
              T("Found on this machine - tap one to add it:"));
            suggestion_forms(b, s, ns, false);
            buf_puts(b, "</div></div>");
        }
    } else {
        buf_puts(b, "<ul class=\"rows\">");
        for (int r = 0; r < ix->nroots; r++) H(b, "<li class=\"row\"><code class=\"media-code\">%s</code></li>", ix->roots[r]);
        if (!ix->nroots) H(b, "<li class=\"row muted\">%s</li>", T("No folders yet."));
        buf_puts(b, "</ul><p class=\"muted media-small\">");
        ask_admins(b, T("An admin chooses which folders are in the library: ask %r."));
        buf_puts(b, "</p>");
    }
    ui_card_end(b);
    media_drives_card(c, b); /* (issue #74: USB disks - new ones, and the library's own) */

    if (is_admin(c) && ix->nroots) {
        ui_card(b, T("Add videos"), "/media/upload", T("Upload"));
        H(b, "<p class=\"muted\">%s</p>",
          T("Send movies and series from a laptop or PC to the library - whole folders at a time, picking up where it "
            "left off if the connection drops."));
        upload_summary(c, b);
        ui_card_end(b);
    }
    convert_card(c, b);
    subtake_card(c, b); /* (admins: the subtitles inside the MKVs, taken out by themselves) */
    media_lookup_card(c, b); /* (admins: categories looked up online - lookup.c, 1.4) */

    ui_card(b, T("Status"), NULL, NULL);
    buf_puts(b, "<dl class=\"media-stats\">");
    H(b, "<div><dt>%s</dt><dd>%d</dd></div><div><dt>%s</dt><dd>%d</dd></div>"
         "<div><dt>%s</dt><dd>%d</dd></div><div><dt>%s</dt><dd>%d</dd></div>",
      T("Movies"), ix->nmovies, T("Series"), ix->nseries, T("Episodes"), ix->nepisodes, T("Other videos"), ix->nothers);
    int playable = 0;
    for (int i = 0; i < ix->nrecent; i++) playable += media_playable(&ix->files[ix->recent[i]])->probe.playable == PLAY_YES;
    H(b, "<div><dt>%s</dt><dd>", T("Play everywhere"));
    H(b, T("%d of %d"), playable, ix->nrecent);
    buf_puts(b, "</dd></div></dl><p class=\"muted\">");
    if (media_scanning()) {
        int seen = media_scan_seen();
        H(b, TN(seen, "Scanning now — %d file looked at so far.", "Scanning now — %d files looked at so far."), seen);
    } else if (ix->scanned_at) {
        Buf ago = {0};
        ui_ago(&ago, ix->scanned_at, c->now);
        H(b,
          TN(ix->probed, "Last scan %r, took %d ms (%d new or changed file read). Hemma rescans every 30 minutes.",
             "Last scan %r, took %d ms (%d new or changed files read). Hemma rescans every 30 minutes."),
          buf_cstr(&ago), ix->scan_ms, ix->probed);
        buf_free(&ago);
    } else H(b, "%s", T("Showing the library as it was last time; a fresh scan starts on its own."));
    buf_puts(b, "</p>");
    if (can_rescan(c)) ui_act(b, "media.rescan", "btn", T("Rescan now"), NULL, NULL, NULL);
    if (is_admin(c)) rescanners_form(b);
    ui_card_end(b);
    left_card(c, b, ix);
    parts_card(c, b, ix);
    history_card(c, b);
    media_manage_room_card(c, b); /* (admins: What takes room - manage.c) */

    if (c->me) {
        ui_card(b, T("Playback"), NULL, NULL);
        const char *cur = pref(c->me->id, "media.sublang", "");
        ui_form(b, "media.prefs", "stack");
        H(b, "<label class=\"field\"><span>%s</span><select name=\"sublang\" data-auto>", T("Subtitles on by default"));
        static const char *const langs[] = {"", "sv", "en", "no", "da", "fi", "de", "fr", "es"};
        for (int i = 0; i < countof(langs); i++)
            H(b, "<option value=\"%s\"%r>%s</option>", langs[i], str_eq(cur, langs[i]) ? " selected" : "",
              i == 0 ? T("Off") : lang_name(langs[i]));
        buf_puts(b, "</select></label><p class=\"muted media-small\">");
        H(b,
          T("Just for %s. Subtitle files next to a video (<code class=\"media-code\">Movie.sv.srt</code>) are picked up "
            "automatically."),
          c->me->name);
        H(b, "</p><noscript><button class=\"btn\">%s</button></noscript>", T("Save"));
        ui_form_end(b);
        /* the sound language (2.2): what a video with sound in several languages plays in */
        const char *snd = pref(c->me->id, PREF_SOUNDLANG, "");
        ui_form(b, "media.soundlang", "stack");
        H(b, "<label class=\"field\"><span>%s</span><select name=\"lang\" data-auto>", T("Sound language"));
        H(b, "<option value=\"\"%r>%s</option>", !*snd ? " selected" : "", T("Ask me"));
        H(b, "<option value=\"-\"%r>%s</option>", str_eq(snd, "-") ? " selected" : "", T("As each video comes"));
        for (int i = 1; i < countof(langs); i++)
            H(b, "<option value=\"%s\"%r>%s</option>", langs[i], str_eq(snd, langs[i]) ? " selected" : "", lang_name(langs[i]));
        if (*snd && !str_eq(snd, "-")) { /* (one picked over a video, not in this list: still shown) */
            bool listed = false;
            for (int i = 1; i < countof(langs); i++) listed |= str_eq(snd, langs[i]);
            if (!listed) H(b, "<option value=\"%s\" selected>%s</option>", snd, lang_name(snd));
        }
        H(b, "</select></label><p class=\"muted media-small\">%s</p><noscript><button class=\"btn\">%s</button></noscript>",
          T("For videos with sound in several languages. A video without yours plays in its own."), T("Save"));
        ui_form_end(b);
        ui_card_end(b);
    }

    if (is_admin(c)) kids_card(b);

    ui_card(b, T("What plays where"), NULL, NULL);
    /* plain words first, the formats folded away (issue #62) */
    H(b, "<p class=\"muted media-plays\">%r</p>",
      T("Most videos play straight away. Some - DVD rips especially - need converting first: one button (<i>Play "
        "everywhere</i> above) makes a copy that plays on every phone, computer and TV."));
    /* (each item's words in ONE span: the list items are column flexboxes, so loose text, <i> and <b> each took a
       row of their own - "Plays on newer devices" sat apart from its sentence) */
    H(b, "<ul class=\"media-tips\"><li><span class=\"badge ok\">%s</span> <span>%r</span></li>", T("Plays in browsers"),
      T("On every screen."));
    H(b, "<li><span class=\"badge warn\">%s</span> <span>%r</span></li>", T("May not play everywhere"),
      T("On computers and most phones, but a TV's browser or an iPhone may refuse it. <i>Plays on newer devices</i>: "
        "most from the last few years."));
    H(b, "<li><span class=\"badge bad\">%s</span> <span>%r</span></li></ul>", T("Needs conversion"),
      T("Nowhere until Hemma has made its copy."));
    H(b, "<details class=\"media-formats\"><summary>%s</summary><ul class=\"media-tips\">", T("Which formats are which"));
    H(b, "<li><span>%r</span></li>", T("<b>Plays in browsers:</b> MP4 or WebM with H.264/VP9 video and AAC/MP3/Opus sound."));
    H(b, "<li><span>%r</span></li>",
      T("<b>May not play everywhere:</b> MKV, HEVC, AV1, or Dolby sound (Dolby Digital and Digital Plus: AC-3, E-AC-3) — "
        "fine in Chrome/Edge or Safari, not all of them; Dolby sound plays silently in Chrome and Firefox. An HEVC or AV1 "
        "video converted here (<i>Plays on newer devices</i>) plays on most devices from the last few years."));
    H(b, "<li><span>%r</span></li></ul></details>",
      T("<b>Needs conversion:</b> MPEG-2 (DVD rips), AVI/XviD, DTS sound. Hemma converts MKV files with MPEG-2, H.264, "
        "HEVC or AV1 video and Dolby, DTS or AAC sound itself, and keeps their subtitles next to them."));
    ui_card_end(b);
    buf_puts(b, "</div>");
    media_drives_dialog(c, b); /* (?format=<disk>) */
}

static void page(Ctx *c, Buf *b) {
    const char *p = c->path + 6; /* after "/media" */
    if (!*p || str_eq(p, "/")) page_home(c, b);
    else if (str_eq(p, "/movies")) page_movies(c, b);
    else if (str_eq(p, "/series")) page_series_list(c, b);
    else if (str_starts(p, "/series/")) page_series(c, b, p + 8);
    else if (str_eq(p, "/browse")) page_browse(c, b);
    else if (str_starts(p, "/watch/")) page_watch(c, b, p + 7);
    else if (str_eq(p, "/books")) media_books_page(c, b);
    else if (str_starts(p, "/book/")) media_book_page(c, b, p + 6);
    else if (str_eq(p, "/settings")) page_settings(c, b);
    else if (str_eq(p, "/upload")) upload_page(c, b);
    else {
        ctx_title(c, "%s", T("Watch"));
        ui_empty(b, "🧭", T("Nothing here"), T("That page doesn't exist."));
    }
}

/* --------------------------------------------------------------- widgets */

static void mini(Ctx *c, Buf *b, const MediaFile *f) {
    const MediaWatch *w = media_watch_of(c, f);
    const MediaSeries *s = f->name.kind == MEDIA_EPISODE ? media_series(f->series) : NULL;
    H(b, "<a class=\"media-mini\" href=\"/media/watch/%X\">", f->id);
    /* pictures are for signed-in screens (display accounts too: /media/img only asks for a session);
       a page drawn for nobody (c->me NULL) gets drawn posters */
    uint64_t img = c->me ? media_picture(f) : 0;
    char title[300];
    media_title(f, title, sizeof title);
    poster_art_as(b, f->name.title, title, s ? 0 : f->name.year, NULL, img, media_percent(w), w && w->done);
    H(b, "<span class=\"media-mini-cap\">%s<small>", title);
    if (s) media_ep_code(b, f);
    else if (f->name.year) H(b, "%d", f->name.year);
    buf_puts(b, "</small></span></a>");
}

/* A series' new episodes as one small card ("70's Show · 8 new episodes"), as Watch groups them. */
static void mini_series(Ctx *c, Buf *b, const MediaSeries *s, int new_eps) {
    H(b, "<a class=\"media-mini\" href=\"/media/series/%X\">", s->id);
    poster_art(b, s->title, 0, T("Series"), c->me ? s->poster : 0, 0, false);
    H(b, "<span class=\"media-mini-cap\">%s<small>", s->title);
    H(b, TN(new_eps, "%d new episode", "%d new episodes"), new_eps);
    buf_puts(b, "</small></span></a>");
}

/* A "continue watching" card: the poster, and an x that drops the viewer's progress in it (the
   video then starts from the beginning, and leaves the list). Everyone else's progress stays.
   On a display account the x is there while someone is picked under "Who's watching?". */
static void going_card(Ctx *c, Buf *b, const MediaFile *f, bool small) {
    if (!ctx_viewer(c)) { /* nobody's progress to drop (a wall display only looks) */
        if (small) mini(c, b, f);
        else video_card(c, b, f);
        return;
    }
    H(b, "<div class=\"media-going\" id=\"media-going-%X\">", f->id);
    if (small) mini(c, b, f);
    else video_card(c, b, f);
    ui_form(b, "media.forget", "media-going-x");
    char title[300];
    media_title(f, title, sizeof title);
    H(b, "<input type=\"hidden\" name=\"id\" value=\"%X\"><button title=\"%s\" aria-label=\"", f->id,
      T("Remove from Continue watching"));
    H(b, T("Remove %s from Continue watching"), title);
    buf_puts(b, "\">");
    ui_icon(b, "x");
    buf_puts(b, "</button>");
    ui_form_end(b);
    buf_puts(b, "</div>");
}

static void widget(Ctx *c, Buf *b, int where) {
    const MediaIndex *ix = media_lib_for(c);
    (void)where;
    if (ix->nroots == 0) {
        if (!is_admin(c)) { /* Library settings are an admin's: no "Set up" that leads nowhere (issue #58) */
            ui_card(b, T("Watch"), NULL, NULL);
            H(b, "<p class=\"muted\">%s ", T("Movies and series from the drive at home will show up here."));
            ask_admins(b, T("Ask %r to add the movie folders."));
            buf_puts(b, "</p>");
            ui_card_end(b);
            return;
        }
        ui_card(b, T("Watch"), "/media/settings", T("Set up"));
        H(b, "<p class=\"muted\">%s</p>",
          T("Point Hemma at the drive with your movies and series, and everyone can watch them on any screen at home — "
            "offline."));
        ui_card_end(b);
        return;
    }
    if (!ix->nrecent) return;
    const MediaFile *list[4];
    int n = media_in_progress(c, list, 4);
    bool resuming = n > 0;
    ui_card(b, resuming ? T("Continue watching") : T("Recently added"), "/media", T("Watch"));
    buf_puts(b, "<div class=\"media-minis\">");
    for (int i = 0; i < n; i++) going_card(c, b, list[i], true);
    if (!resuming) { /* a series' new episodes as one card, as Watch has them (issue #59: one per episode) */
        MediaRecent r[4];
        int nr = media_recent(c, r, countof(r));
        for (int i = 0; i < nr; i++) {
            if (r[i].n > 1) mini_series(c, b, r[i].series, r[i].n);
            else mini(c, b, r[i].f);
        }
    }
    buf_puts(b, "</div>");
    ui_card_end(b);
}

/* --------------------------------------------------------------- actions */

static void act_search(Ctx *c, Fields *f) { ctx_set_live(c, "media.q", fields_str(f, "q")); }

/* Watching actions (progress, watched, forget) are the viewer's - also on a display account, for
   whoever was picked there (Action.display); with nobody picked they quietly do nothing. */
static void act_progress(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int v = ctx_viewer(c);
    if (!v || !media_parse_id(fields_str(f, "id"), &id) || !media_file(id)) return;
    bool ended = fields_str(f, "ended")[0] == '1';
    if (media_watch_report(v, id, (int)fields_int(f, "pos", 0), (int)fields_int(f, "dur", 0), ended, c->now))
        app_changed(); /* finished: other screens' "continue watching" changes */
}

/* The viewer picked subtitles in the player (the menu, or the C key): kept for the series (1.3) - its next
   episode, opened anywhere, starts with them. key "" = off (kept as "-"). Films keep nothing: the
   viewer's language (Library settings) stays theirs. */
static void act_sub(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int v = ctx_viewer(c);
    const MediaFile *mf = v && media_parse_id(fields_str(f, "id"), &id) ? media_file(id) : NULL;
    const char *key = fields_str(f, "key");
    if (!mf || !mf->series || strlen(key) > 32 || key[strspn(key, "abcdefghijklmnopqrstuvwxyz0123456789.")]) return;
    char val[48];
    snprintf(val, sizeof val, "%d %s", sub_generation(v), key[0] ? key : "-");
    pref_set(v, sub_memory_key(mf->series).k, val);
}

/* The player's Language menu (2.2, live; wire.js has already switched the sound): the pick's language kept for the
   series (an episode) or the video, by the viewer - as act_sub. */
static void act_sound(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int v = ctx_viewer(c), n = (int)fields_int(f, "n", -1);
    const MediaFile *mf = v && media_parse_id(fields_str(f, "id"), &id) ? media_file(id) : NULL;
    const MediaSounds *s = sounds_of(mf);
    if (!s || n < 0 || n >= s->n) return;
    char val[16];
    snprintf(val, sizeof val, "%s#%d", s->s[n].lang, n); /* (its language, and which: a film may have four Russian) */
    pref_set(v, snd_memory_key(mf).k, val);
}

/* Someone's sound language (2.2): the question over a video with several, or Library settings. "-" = each video's
   own, "" = ask again. The viewer's: on a TV, whoever is watching. */
static void act_soundlang(Ctx *c, Fields *f) {
    int v = ctx_viewer(c);
    const char *lang = fields_str(f, "lang");
    bool code = strlen(lang) == 2 && lang[0] >= 'a' && lang[0] <= 'z' && lang[1] >= 'a' && lang[1] <= 'z';
    if (!v || !(code || !*lang || str_eq(lang, "-"))) {
        ctx_fail(c, "%s", T("Pick a language"));
        return;
    }
    pref_set(v, PREF_SOUNDLANG, lang);
    if (code) ctx_ok(c, T("Sound: %s"), lang_name(lang));
    else if (*lang) ctx_ok(c, "%s", T("Each video plays in its own language"));
    else ctx_ok(c, "%s", T("Hemma asks again at the next video with several languages"));
}

static void act_watched(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int v = ctx_viewer(c);
    if (!v) return;
    const MediaFile *mf = media_parse_id(fields_str(f, "id"), &id) ? media_file(id) : NULL;
    if (!mf) {
        ctx_fail(c, "%s", T("That video isn't in the library any more"));
        return;
    }
    bool done = fields_int(f, "done", 1) != 0;
    media_watch_set_done(v, id, done, c->now);
    if (mf->type == MF_AUDIO) ctx_ok(c, "%s", done ? T("Marked as finished") : T("Marked as not finished")); /* (a book) */
    else ctx_ok(c, "%s", done ? T("Marked as watched") : T("Marked as not watched"));
}

static void act_rescan(Ctx *c, Fields *f) {
    if (!can_rescan(c)) {
        ctx_fail(c, "%s", T("Rescanning is for admins - ask one to rescan, or to let you do it"));
        return;
    }
    if (app_testing()) {
        media_scan_sync();
        ctx_ok(c, "%s", T("Library scanned"));
        return;
    }
    if (media_scanning()) {
        ctx_info(c, "%s", T("Already scanning — hang on"));
        return;
    }
    media_scan_start();
    ctx_info(c, "%s", T("Scanning your library…"));
}

static void act_art_remove(Ctx *c, Fields *f) {
    uint64_t id = 0;
    char dir[1024], stem[400];
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change pictures"));
        return;
    }
    if (!media_parse_id(fields_str(f, "id"), &id) || !media_art_folder(id, dir, sizeof dir, stem, sizeof stem)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    int n = media_art_prune(dir, stem, NULL);
    if (app_testing()) media_scan_sync();
    else media_scan_start();
    if (n) ctx_ok(c, "%s", T("Picture removed"));
    else ctx_info(c, "%s", T("There was no picture to remove"));
}

static void act_remove(Ctx *c, Fields *fl) {
    uint64_t id = 0;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can remove things from the library"));
        return;
    }
    const MediaFile *f = media_parse_id(fields_str(fl, "id"), &id) ? media_file(id) : NULL;
    if (!f) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    if (convert_queued(f->id)) {
        ctx_fail(c, "%s", T("It's being converted right now - stop that first (Library settings)"));
        return;
    }
    RemovePlan p = {0};
    if (!can_remove(c, f) || !media_remove_plan(f, &p)) {
        media_remove_free(&p);
        ctx_fail(c, "%s", T("Only films and other videos can be removed here"));
        return;
    }
    char title[160], hex[17], size[24], video[1024] = "";
    snprintf(title, sizeof title, f->name.year ? "%s (%d)" : "%s", f->name.title, f->name.year);
    media_id_hex(f->id, hex);
    media_full_path(f, video, sizeof video);
    bool movie = f->name.kind == MEDIA_MOVIE;
    size_text(p.bytes, size, sizeof size);
    int failed = 0;
    media_remove_run(&p, &failed);
    media_remove_free(&p);
    if (failed && path_stat(video, NULL, NULL, NULL)) { /* the video itself is still there (playing somewhere?) */
        ctx_fail(c, "%s", T("Couldn't delete it - is it playing somewhere? Try again in a moment."));
    } else {
        for (int i = tbl_count(&T_media_watch) - 1; i >= 0; i--) { /* everyone's progress in it */
            const MediaWatch *w = tbl_at(&T_media_watch, i);
            if (str_eq(w->item, hex)) tbl_del(&T_media_watch, w->id);
        }
        activity_add(c->me->id, "🗑️", movie ? "/media/movies" : "/media/browse", N_("removed %s from the library"), title);
        if (failed)
            ctx_info(c,
                     TN(failed, "Removed %s - %d leftover file couldn't be deleted",
                        "Removed %s - %d leftover files couldn't be deleted"),
                     title, failed);
        else ctx_ok(c, T("Removed %s - %s freed"), title, size);
        ctx_redirect(c, "%s", movie ? "/media/movies" : "/media/browse");
    }
    g_disks_at = 0; /* the sidebar's disk space: look again */
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

/* Videos with two versions (an original and its converted copy): the one that plays in fewer
   places is deleted (remove.c media_lesser_version) - usually the original, whose progress moves
   to the copy. */
static void act_drop_originals(Ctx *c, Fields *fl) {
    (void)fl;
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can delete originals"));
        return;
    }
    const MediaIndex *ix = media_lib();
    int done = 0, failed = 0;
    int64_t bytes = 0;
    for (int i = 0; i < ix->nfiles; i++) { /* (deleting doesn't change the index until the rescan) */
        const MediaFile *better = NULL, *lesser = media_lesser_version(&ix->files[i], &better);
        if (!lesser || convert_queued(ix->files[i].id)) continue;
        if (media_drop_version(lesser, better)) done++, bytes += lesser->size;
        else failed++;
    }
    char size[24], logged[24];
    Lang was = i18n_use(LANG_EN); /* (the log speaks English: "1.2 GB") */
    size_text(bytes, logged, sizeof logged);
    i18n_use(was);
    size_text(bytes, size, sizeof size);
    if (done) {
        log_info("media: %s kept only the most compatible versions: %d files deleted (%s)", c->me->name, done, logged);
        if (failed)
            ctx_info(c, TN(done, "Deleted %d file (%s) - %d couldn't be deleted", "Deleted %d files (%s) - %d couldn't be deleted"),
                     done, size, failed);
        else ctx_ok(c, TN(done, "Deleted %d file - %s freed", "Deleted %d files - %s freed"), done, size);
    } else if (failed) {
        ctx_fail(c, "%s", T("Couldn't delete them (in use, or no permission?)"));
    } else {
        ctx_info(c, "%s", T("Nothing to delete: no video has two versions"));
    }
    g_disks_at = 0; /* the sidebar's disk space: look again */
    if (app_testing()) media_scan_sync();
    else media_scan_start();
}

/* "Remove from Continue watching": the viewer's progress in the video goes (finished ones aren't listed). */
static void act_forget(Ctx *c, Fields *f) {
    uint64_t id = 0;
    int who = ctx_viewer(c);
    if (!who) return;
    if (!media_parse_id(fields_str(f, "id"), &id)) {
        ctx_fail(c, "%s", T("That isn't in the library any more"));
        return;
    }
    const MediaWatch *w = media_watch_get(who, id);
    if (w && !w->done) {
        ctx_undo(c, &T_media_watch, w->id); /* the card's x is instant: Undo brings the progress back (issue #60) */
        tbl_del(&T_media_watch, w->id);
    }
    const MediaFile *v = media_file(id);
    const MediaBook *bk = v && v->type == MF_AUDIO ? media_book(v->series) : NULL; /* (an audiobook: 1.6) */
    char what[300];
    if (v && v->name.kind == MEDIA_EPISODE)
        snprintf(what, sizeof what, "%s S%02dE%02d", v->name.title, v->name.season, v->name.episode);
    else if (v) media_title(v, what, sizeof what);
    if (bk) ctx_ok(c, T("Removed %s from Continue listening"), bk->title);
    else if (v) ctx_ok(c, T("Removed %s from Continue watching"), what);
    else ctx_ok(c, "%s", T("Removed it from Continue watching"));
}

static void act_drop_auto(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change that"));
        return;
    }
    bool on = str_eq(fields_str(f, "on"), "1");
    setting_set_int("media.drop_originals", on);
    ctx_ok(c, "%s",
           on ? T("After each conversion only the most compatible version stays") : T("Both versions stay after converting"));
}

/* media.rescanners: p=<id> for each member who may rescan too (the whole list, every time). */
static void act_rescanners(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can choose who may rescan"));
        return;
    }
    int ids[FIELDS_MAX], n = 0;
    for (int i = 0; i < f->n; i++) {
        long long id = str_eq(f->k[i], "p") ? str_int_or(f->v[i], 0) : 0;
        bool dup = false;
        for (int k = 0; k < n; k++) dup |= ids[k] == id;
        if (!dup && id > 0 && id < INT32_MAX && can_sign_in_member(person_get((int)id))) ids[n++] = (int)id;
    }
    Buf list = {0};
    for (int k = 0; k < n; k++) buf_printf(&list, "%s%d", k ? "," : "", ids[k]);
    setting_set(RESCANNERS, buf_cstr(&list));
    buf_free(&list);
    if (n) ctx_ok(c, TN(n, "%d member may rescan the library too", "%d members may rescan the library too"), n);
    else ctx_ok(c, "%s", T("Only admins may rescan the library"));
}

/* The library's folders, one per line (checked: each must be there): saved, then scanned. */
static void save_roots(Ctx *c, const char *s) {
    Buf keep = {0};
    int n = 0;
    while (*s) {
        size_t len = strcspn(s, "\n");
        char line[300];
        str_ncopy(line, sizeof line, s, len);
        str_trim(line);
        s += len;
        if (*s == '\n') s++;
        if (!line[0]) continue;
        for (char *p = line; *p; p++)
            if (*p == '\\') *p = '/';
        if (!path_is_dir(line)) {
            ctx_fail_at(c, "roots", T("Can't find the folder \"%s\" on the server"), line);
            buf_free(&keep);
            return;
        }
        if (++n > MEDIA_MAX_ROOTS) {
            ctx_fail_at(c, "roots", T("That's more than %d folders — point at a parent folder instead"), MEDIA_MAX_ROOTS);
            buf_free(&keep);
            return;
        }
        if (keep.len) buf_putc(&keep, '\n');
        buf_puts(&keep, line);
    }
    setting_set("media.roots", keep.len ? buf_cstr(&keep) : "");
    g_disks_at = 0; /* the sidebar's disk space: look again */
    media_health_reset(); /* and the System page's checks */
    buf_free(&keep);
    if (app_testing()) media_scan_sync();
    else media_scan_start();
    if (n) ctx_ok(c, TN(n, "Saved — scanning %d folder", "Saved — scanning %d folders"), n);
    else ctx_ok(c, "%s", T("Saved — no folders"));
}

static void act_roots(Ctx *c, Fields *f) {
    if (!is_admin(c)) { /* server-side folders: admins only */
        ctx_fail(c, "%s", T("Only an admin can change the library folders"));
        return;
    }
    save_roots(c, fields_str(f, "roots"));
}

/* A folder more or less from outside an action (a movie disk the disk helper mounted or forgot, drives.c):
   true = the folders changed (and a scan starts). */
bool media_root_add(const char *dir) {
    char roots[MEDIA_MAX_ROOTS][300];
    int n = media_roots(roots, MEDIA_MAX_ROOTS);
    if (n >= MEDIA_MAX_ROOTS || !path_is_dir(dir)) return false;
    Buf all = {0};
    for (int i = 0; i < n; i++) {
        if (same_folder(roots[i], dir)) {
            buf_free(&all);
            return false;
        }
        buf_printf(&all, "%s\n", roots[i]);
    }
    buf_puts(&all, dir);
    setting_set("media.roots", buf_cstr(&all));
    buf_free(&all);
    g_disks_at = 0;
    media_health_reset();
    if (app_testing()) media_scan_sync();
    else media_scan_start();
    return true;
}

bool media_root_drop(const char *dir) {
    char roots[MEDIA_MAX_ROOTS][300];
    int n = media_roots(roots, MEDIA_MAX_ROOTS);
    Buf keep = {0};
    bool dropped = false;
    for (int i = 0; i < n; i++) {
        if (same_folder(roots[i], dir)) {
            dropped = true;
            continue;
        }
        if (keep.len) buf_putc(&keep, '\n');
        buf_puts(&keep, roots[i]);
    }
    if (dropped) {
        setting_set("media.roots", keep.len ? buf_cstr(&keep) : "");
        g_disks_at = 0;
        media_health_reset();
        if (app_testing()) media_scan_sync();
        else media_scan_start();
    }
    buf_free(&keep);
    return dropped;
}

/* One folder more (a suggestion's one tap, issue #27) - the ones there stay. */
static void act_roots_add(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can change the library folders"));
        return;
    }
    char dir[300], roots[MEDIA_MAX_ROOTS][300];
    str_copy(dir, sizeof dir, fields_str(f, "dir"));
    str_trim(dir);
    same_slashes(dir);
    int n = media_roots(roots, MEDIA_MAX_ROOTS);
    Buf all = {0};
    bool have = false;
    for (int i = 0; i < n; i++) {
        buf_printf(&all, "%s\n", roots[i]);
        have |= same_folder(roots[i], dir);
    }
    if (!dir[0] || have) {
        if (dir[0]) ctx_info(c, T("%s is in the library already"), dir);
        else ctx_fail(c, "%s", T("Pick a folder"));
        buf_free(&all);
        return;
    }
    buf_puts(&all, dir);
    save_roots(c, buf_cstr(&all));
    buf_free(&all);
}

/* Mark a folder "Not for children", or open it to them again (issue #44): from the Folders tab (p = "<library
   folder number>:<folder below it>", on), or Library settings' list (root + dir: a folder no longer in the
   library can be cleared too). */
static void act_kids(Ctx *c, Fields *f) {
    if (!is_admin(c)) {
        ctx_fail(c, "%s", T("Only an admin can do that"));
        return;
    }
    const MediaIndex *ix = media_lib();
    char root[300], dir[400];
    bool on = str_eq(fields_str(f, "on"), "1");
    const char *p = fields_str(f, "p");
    if (p[0]) {
        int r = (int)strtol(p, NULL, 10);
        const char *colon = strchr(p, ':');
        if (!colon || r < 0 || r >= ix->nroots) {
            ctx_fail(c, "%s", T("That folder isn't in the library"));
            return;
        }
        str_copy(root, sizeof root, ix->roots[r]);
        str_copy(dir, sizeof dir, colon + 1);
    } else {
        str_copy(root, sizeof root, fields_str(f, "root"));
        str_copy(dir, sizeof dir, fields_str(f, "dir"));
        on = false;
    }
    if (strstr(dir, "..") || dir[0] == '/' || strchr(dir, '\\')) {
        ctx_fail(c, "%s", T("That folder isn't in the library"));
        return;
    }
    media_kids_set(root, dir, on);
    const char *name = dir[0] ? strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir : root;
    ctx_ok(c, on ? T("%s is not for children now") : T("%s is open to children"), name);
}

static void act_prefs(Ctx *c, Fields *f) {
    char lang[16];
    str_copy(lang, sizeof lang, fields_str(f, "sublang"));
    pref_set(c->me->id, "media.sublang", lang);
    char gen[16]; /* (the picks remembered per series give way to the new default) */
    snprintf(gen, sizeof gen, "%d", sub_generation(c->me->id) + 1);
    pref_set(c->me->id, "media.sub.gen", gen);
    if (*lang) ctx_ok(c, T("Subtitles: %s"), lang_name(lang));
    else ctx_ok(c, "%s", T("Subtitles off by default"));
}

/* Watching is open to children and guests too (issue #44): searching, progress, ticks, their own prefs. */
static const Action actions[] = {
    {"media.search", act_search, true, .may = ACT_ALL}, {"media.progress", act_progress, true, true, ACT_ALL},
    {"media.sub", act_sub, true, true, ACT_ALL}, {"media.watched", act_watched, false, true, ACT_ALL},
    {"media.sound", act_sound, true, true, ACT_ALL}, {"media.soundlang", act_soundlang, false, true, ACT_ALL},
    {"media.rescan", act_rescan}, {"media.roots", act_roots}, {"media.roots_add", act_roots_add},       {"media.prefs", act_prefs, .may = ACT_ALL},
    {"media.upload_stop", upload_act_stop}, {"media.upload_preview", upload_act_preview, true}, {"media.art_remove", act_art_remove},
    {"media.convert", convert_act_start},   {"media.convert_stop", convert_act_stop}, {"media.remove", act_remove},
    {"media.forget", act_forget, false, true, ACT_ALL}, {"media.kids", act_kids},
    {"media.drop_originals", act_drop_originals}, {"media.drop_auto", act_drop_auto},
    {"media.rescanners", act_rescanners}, {"media.parts_remove", act_parts_remove},
    {"media.history_forget", act_history_forget},
    {"media.drive_use", media_drives_act_use}, {"media.drive_format", media_drives_act_format},
    {"media.drive_forget", media_drives_act_forget}, {"media.drive_seen", media_drives_act_seen},
    {"media.manage_remove", media_manage_act_remove}, {"media.fix_title", media_manage_act_fix_title},
    {"media.fix_episode", media_manage_act_fix_episode}, {"media.rename_series", media_manage_act_rename_series},
    {"media.take_subs", subtake_act_switch},
    {"media.cat", act_media_cat}, {"media.cat_new", act_media_cat_new}, /* (members and admins: 1.4) */
    {"media.lookup", act_media_lookup}, {"media.lookup_id", act_media_lookup_id},
    {0},
};

/* ------------------------------------------------------------- lifecycle */

static void init(void) {
    T_media_watch.api_owner = "person"; /* /api: watch history is for its person (and admins) */
    T_media_watch.what = "watch history";
    store_register(&T_media_watch);
    T_media_series_name.what = "names of renamed series";
    store_register(&T_media_series_name);
    media_cats_init();
    media_lookup_init();
    media_kids_init();
}

/* Someone leaves (issue #45): moved out, what they watched stays with them; erased, it goes. */
static int forget(Forget *f) {
    int n = 0;
    for (int i = tbl_count(&T_media_watch) - 1; i >= 0 && !f->keep; i--) {
        const MediaWatch *w = tbl_at(&T_media_watch, i);
        if (w->person != f->person) continue;
        n++;
        if (f->apply) tbl_del(&T_media_watch, w->id);
    }
    if (n) forget_note(f, TN(n, "What they watched goes (%d video)", "What they watched goes (%d videos)"), n);
    return n;
}

static void start(void) {
    media_test_mounts(NULL); /* (a test's disks end with its boot) */
    media_stream_resume();
    upload_start();
    media_lib_start();
    subtake_start(); /* (after the library: its table sits beside the library's cache) */
    media_health_reset();
    g_disks_at = 0; /* (a start measures afresh: a test before this one may have measured other folders) */
    media_drives_start(); /* (after the library: a disk the helper just mounted becomes a folder of it) */
    media_lookup_start();
}

static void stop(void) {
    media_lookup_stop();
    media_drives_stop();
    subtake_stop(); /* (the file it was reading goes back on its list) */
    media_stream_stop();
    upload_stop();
    convert_stop();
    media_lib_stop();
    media_kids_stop();
}

/* ------------------------------------------------- disk space (sidebar) */

/* Used and free space of each disk that holds a library folder, for the admins' sidebar:
   checked every 30 s on the tick (and when the folders change), so drawing the sidebar on
   every page never touches a disk. Two folders on one disk give one bar. */
#define DISKS_MAX 4
static struct {
    char name[64];
    int64_t free, total;
} g_disks[DISKS_MAX];
static int g_ndisks;

static void disks_refresh(int64_t now) {
    g_disks_at = now ? now : 1;
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS), n = 0;
    for (int i = 0; i < nroots && n < DISKS_MAX; i++) {
        int64_t fr = 0, tot = 0;
        if (!path_disk_space(roots[i], &fr, &tot) || tot <= 0) continue;
        bool same = false;
        for (int k = 0; k < n && !same; k++) same = g_disks[k].total == tot && llabs(g_disks[k].free - fr) < (64LL << 20);
        if (same) continue;
        const char *slash = strrchr(roots[i], '/');
        str_copy(g_disks[n].name, sizeof g_disks[n].name, slash && slash[1] ? slash + 1 : roots[i]);
        g_disks[n].free = fr;
        g_disks[n].total = tot;
        n++;
    }
    g_ndisks = n;
}

static void gb(int64_t n, char *out, size_t cap) {
    double g = (double)n / 1073741824.0;
    if (g >= 1024) snprintf(out, cap, "%.1f TB", g / 1024);
    else snprintf(out, cap, g >= 10 ? "%.0f GB" : "%.1f GB", g);
    decimal_mark(out);
}

/* (the numbers come from the tick; the words are the reader's, made here) */
static void side(Ctx *c, Buf *b) {
    if (!is_admin(c)) return; /* the people who add videos (and would fill the disk) */
    if (!g_disks_at) disks_refresh(now_ms());
    for (int i = 0; i < g_ndisks; i++) {
        int64_t used = g_disks[i].total - g_disks[i].free;
        int pct = (int)CLAMP(used * 100 / g_disks[i].total, 0, 100);
        const char *level = pct >= 95 || g_disks[i].free < (2LL << 30) ? " bad" : pct >= 85 ? " warn" : "";
        char fr[24], us[24], tot[24], width[8];
        gb(g_disks[i].free, fr, sizeof fr);
        gb(used, us, sizeof us);
        gb(g_disks[i].total, tot, sizeof tot);
        snprintf(width, sizeof width, "%d%%", pct);
        H(b, "<a class=\"media-disk%r\" href=\"/media/settings\" title=\"", level);
        H(b, T("Space on the disk with the library (%s)"), g_disks[i].name);
        H(b, "\"><span class=\"media-disk-l\"><span>%s%s%s</span><b>", T("Storage"), g_ndisks > 1 ? " · " : "",
          g_ndisks > 1 ? g_disks[i].name : "");
        H(b, T("%s free"), fr);
        H(b, "</b></span><span class=\"media-disk-bar\"><i style=\"width:%s\"></i></span><small>", width);
        H(b, T("%s used of %s"), us, tot);
        buf_puts(b, "</small></a>");
    }
}

static void tick(int64_t now) {
    media_lib_tick(now);
    upload_tick(now);
    convert_tick(now);
    subtake_tick(now); /* (after the queue's: a conversion that just started makes it wait) */
    media_health_tick(now);
    media_drives_tick(now);
    media_manage_tick(now); /* (a removal finished on its thread) */
    media_lookup_tick(now);
    if (!g_disks_at || now - g_disks_at >= 30000) disks_refresh(now);
}

static bool http(Ctx *c, Conn *conn, HttpReq *r) { return upload_http(c, conn, r) || media_http(c, conn, r); }

static const Module media = {
    .id = "media",
    .title = N_("Watch"),
    .icon = "play",
    .nav = 4,
    .init = init,
    .start = start,
    .stop = stop,
    .tick = tick,
    .side = side,
    .page = page,
    .actions = actions,
    .http = http,
    .widget = widget,
    .widget_order = 50,
    .health = media_health,
    .blocks = media_blocks,
    .forget = forget,
};
MODULE(media)
