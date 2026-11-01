/* modules/media/devices.c — WHAT YOUR DEVICES PLAY (2.5; owner, 2026-10-30: "check the issue with the videos that are
 * not playable everywhere and how we can make them playable everywhere" - Library settings said "Play everywhere 1456
 * of 2776": the rest mostly HEVC (x265) and AV1 copies, which play on newer devices, not on every one there is;
 * "devices first": what matters is whether YOUR devices play them).
 *
 * Each browser that opens a Watch page answers which kinds of video it plays: the page carries a hidden list of
 * media types (media_caps, [data-caps] - the list and what it means are the server's); wire.js asks the browser
 * (canPlayType) once a load and sends the answers as live media.caps: one letter each, y (probably), m (maybe), n.
 * Kept per device - its person, its browser (the session's agent: "Chrome on Windows"), its answers - in table
 * media_device; a device counts while it was seen in the last DEVICE_DAYS. Library settings then says how many
 * videos play on every one of them, lists the devices with what each can't play, and why the rest of the library
 * doesn't play everywhere (the probes' reasons, counted). Each new device and each changed count is logged. */
#include "../../core/ui.h"
#include "media.h"

#define DEVICE_DAYS 30      /* a device counts while it was seen this recently */
#define DEVICE_KEEP_DAYS 90 /* ... and is forgotten after this */
#define SEEN_EVERY_S 3600   /* its "seen" written at most this often */
#define CAPS_FORMAT 1       /* the list below: a changed one gets a new number (older answers then don't count) */
#define MAX_DEVICES 64      /* shown and counted */
#define MAX_REASONS 16

/* What a browser is asked: append only - a changed meaning or order needs CAPS_FORMAT + 1. HEVC and AV1 asked in
   10 bits at a 1080p level: what x265/AV1 releases are (a device playing those plays the 8-bit ones). */
enum {
    CAP_H264, CAP_HEVC, CAP_HEVC10, CAP_AV1, CAP_AV1_10, CAP_AAC, CAP_MP3, CAP_OPUS, CAP_FLAC, CAP_AC3, CAP_EAC3,
    CAP_WEBM, CAP_WEBM_AV1, CAP_VORBIS, CAP_MKV, NCAPS
};
static const char *const CAP_TYPE[NCAPS] = {
    "video/mp4; codecs=\"avc1.640028\"",     "video/mp4; codecs=\"hvc1.1.6.L120.90\"",
    "video/mp4; codecs=\"hvc1.2.4.L153.90\"", "video/mp4; codecs=\"av01.0.08M.08\"",
    "video/mp4; codecs=\"av01.0.09M.10\"",    "audio/mp4; codecs=\"mp4a.40.2\"",
    "audio/mp4; codecs=\"mp4a.6B\"",          "audio/mp4; codecs=\"opus\"",
    "audio/mp4; codecs=\"flac\"",             "audio/mp4; codecs=\"ac-3\"",
    "audio/mp4; codecs=\"ec-3\"",             "video/webm; codecs=\"vp9, opus\"",
    "video/webm; codecs=\"av01.0.08M.08\"",   "audio/webm; codecs=\"vorbis\"",
    "video/x-matroska; codecs=\"avc1.640028, mp4a.40.2\"",
};

typedef struct {
    int id;
    int person;
    char agent[64]; /* the session's (packed: i18n_unpack shows it) */
    char caps[24];  /* NCAPS letters y / m / n */
    int format;     /* CAPS_FORMAT the answers are to */
    int64_t first, seen;
    int cores; /* (2.7) its logical cores, as the browser said (navigator.hardwareConcurrency); 0 = not said */
} MediaDevice;

static const FieldDef device_fields[] = {
    FIELD(MediaDevice, id, F_INT),     FIELD(MediaDevice, person, F_INT), FIELD(MediaDevice, agent, F_STR),
    FIELD(MediaDevice, caps, F_STR),   FIELD(MediaDevice, format, F_INT), FIELD(MediaDevice, first, F_I64),
    FIELD(MediaDevice, seen, F_I64),   FIELD(MediaDevice, cores, F_INT),
};
static Table T_media_device = TABLE("media_device", MediaDevice, device_fields);

void media_devices_init(void) {
    T_media_device.what = "devices that opened Watch, and what they play";
    T_media_device.api_owner = "person";
    store_register(&T_media_device);
}

/* the hidden list a Watch page carries (wire.js answers it) */
void media_caps(Buf *b) {
    buf_puts(b, "<i hidden id=\"media-caps\" data-caps=\"");
    for (int i = 0; i < NCAPS; i++) {
        if (i) buf_putc(b, '|');
        H(b, "%s", CAP_TYPE[i]);
    }
    buf_puts(b, "\"></i>");
}

static bool cap(const char *caps, int k) { return caps[k] == 'y' || caps[k] == 'm'; }

/* Why a file doesn't play on a device with these answers: a short kind ("HEVC (x265)", "Dolby sound", "MKV"), in
   English (N_: shown with T()), or NULL = it plays. */
static const char *cant(const char *caps, const MediaProbe *p) {
    const char *ct = p->container, *vc = p->vcodec, *ac = p->acodec;
    bool mp4 = str_eq(ct, "mp4") || str_eq(ct, "mov"), webm = str_eq(ct, "webm"), mkv = str_eq(ct, "mkv");
    if (p->playable == PLAY_UNKNOWN) return N_("unreadable files"); /* (its header couldn't be read: nothing to go by) */
    if (!mp4 && !webm && !mkv) return ct[0] ? N_("files browsers don't open") : N_("unreadable files");
    if (mkv && !cap(caps, CAP_MKV)) return N_("MKV");
    if (!vc[0] || str_eq(vc, "h264")) {
        if (!cap(caps, CAP_H264)) return N_("H.264");
    } else if (str_eq(vc, "hevc")) {
        if (!cap(caps, CAP_HEVC10)) return N_("HEVC (x265)");
    } else if (str_eq(vc, "av1")) {
        if (!cap(caps, webm ? CAP_WEBM_AV1 : CAP_AV1_10)) return N_("AV1");
    } else if (str_eq(vc, "vp9") || str_eq(vc, "vp8")) {
        if (mp4 || !cap(caps, CAP_WEBM)) return N_("VP9");
    } else {
        return N_("pictures browsers don't play");
    }
    if (!ac[0] || str_eq(ac, "aac")) return ac[0] && !cap(caps, CAP_AAC) ? N_("AAC sound") : NULL;
    if (str_eq(ac, "mp3")) return cap(caps, CAP_MP3) ? NULL : N_("MP3 sound");
    if (str_eq(ac, "opus")) return cap(caps, mp4 ? CAP_OPUS : CAP_WEBM) ? NULL : N_("Opus sound");
    if (str_eq(ac, "flac")) return cap(caps, CAP_FLAC) ? NULL : N_("FLAC sound");
    if (str_eq(ac, "vorbis")) return cap(caps, CAP_VORBIS) && !mp4 ? NULL : N_("Vorbis sound");
    if (str_eq(ac, "ac3")) return cap(caps, CAP_AC3) ? NULL : N_("Dolby sound");
    if (str_eq(ac, "eac3")) return cap(caps, CAP_EAC3) ? NULL : N_("Dolby sound");
    return str_eq(ac, "dts") ? N_("DTS sound") : str_eq(ac, "truehd") ? N_("TrueHD sound") : N_("sound browsers don't play");
}

bool media_device_plays(const char *caps, const MediaProbe *p) {
    return caps && strlen(caps) == NCAPS && !cant(caps, p);
}

/* the devices that count now: seen lately, by someone who's still here, answers to today's list */
static int devices_now(const MediaDevice **out, int max, int64_t now) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_media_device) && n < max; i++) {
        const MediaDevice *d = tbl_at(&T_media_device, i);
        const Person *p = person_get(d->person);
        if (d->format != CAPS_FORMAT || strlen(d->caps) != NCAPS || now - d->seen > DEVICE_DAYS * 86400LL || !p || p->left)
            continue;
        out[n++] = d;
    }
    return n;
}

/* what a device plays, in words: "H.264, HEVC (x265), AV1, Opus" (English: the log) */
static void plays_list(const char *caps, Buf *out) {
    static const struct {
        int k;
        const char *what;
    } said[] = {{CAP_H264, "H.264"}, {CAP_HEVC10, "HEVC 10-bit"}, {CAP_HEVC, "HEVC"}, {CAP_AV1_10, "AV1 10-bit"},
                {CAP_AV1, "AV1"},   {CAP_MKV, "MKV"},            {CAP_WEBM, "WebM"},  {CAP_OPUS, "Opus"},
                {CAP_FLAC, "FLAC"}, {CAP_AC3, "Dolby Digital"},  {CAP_EAC3, "Dolby Digital Plus"}};
    for (int i = 0; i < countof(said); i++)
        buf_printf(out, "%s%s%s", out->len ? ", " : "", cap(caps, said[i].k) ? "" : "no ", said[i].what);
}

/* media.caps caps: what this browser answered (live, from any signed-in screen - a TV's too) */
void act_media_caps(Ctx *c, Fields *f) {
    const char *caps = fields_str(f, "caps");
    if (!c->sess || strlen(caps) != NCAPS || strspn(caps, "ymn") != NCAPS) return;
    int person = c->sess->person;
    int64_t now = now_sec();
    long cores = strtol(fields_str(f, "cores"), NULL, 10);
    cores = cores > 0 && cores <= 1024 ? cores : 0;
    for (int i = 0; i < tbl_count(&T_media_device); i++) {
        const MediaDevice *d = tbl_at(&T_media_device, i);
        if (d->person != person || d->format != CAPS_FORMAT || !str_eq(d->agent, c->sess->agent) || !str_eq(d->caps, caps))
            continue;
        if (now - d->seen >= SEEN_EVERY_S || (cores && d->cores != cores)) {
            MediaDevice m = *d;
            m.seen = now;
            if (cores) m.cores = (int)cores;
            tbl_put(&T_media_device, &m);
        }
        return;
    }
    MediaDevice m = {.person = person, .format = CAPS_FORMAT, .first = now, .seen = now, .cores = (int)cores};
    str_copy(m.agent, sizeof m.agent, c->sess->agent);
    str_copy(m.caps, sizeof m.caps, caps);
    tbl_put(&T_media_device, &m);
    const Person *p = person_get(person);
    char agent[96];
    Buf what = {0};
    plays_list(caps, &what);
    int was = lang_use(LANG_EN); /* (the log speaks English) */
    i18n_unpack_to(agent, sizeof agent, m.agent);
    lang_use(was);
    log_info("media: a new device: %s's %s plays %s", p ? p->name : "?", agent, buf_cstr(&what));
    buf_free(&what);
    app_changed_where("/media/settings");
}

/* (2.7) this session's device: its cores as its browser last said (0 = not yet), whether it's a phone or tablet (by
   the session's agent: Android, iPhone, iPad) and its name ("Chrome on Windows", packed) */
int media_device_cores(const Ctx *c, bool *phone, char *agent, size_t cap) {
    if (phone) *phone = false;
    if (agent && cap) agent[0] = '\0';
    if (!c->sess) return 0;
    const char *a = c->sess->agent;
    if (phone) *phone = strstr(a, "Android") || strstr(a, "iPhone") || strstr(a, "iPad");
    if (agent) str_copy(agent, cap, a);
    const MediaDevice *best = NULL;
    for (int i = 0; i < tbl_count(&T_media_device); i++) {
        const MediaDevice *d = tbl_at(&T_media_device, i);
        if (d->person == c->sess->person && str_eq(d->agent, a) && d->cores > 0 && (!best || d->seen > best->seen)) best = d;
    }
    return best ? best->cores : 0;
}

/* (2.7) the household's devices that can't play HEVC (10-bit, as x265 releases are): how many (-1 = no device seen yet),
   named in names ("Anna's Chrome on Windows, Living room TV") */
int media_devices_no_hevc(const Ctx *c, char *names, size_t ncap) {
    (void)c;
    if (ncap) names[0] = 0;
    const MediaDevice *dev[64];
    int nd = devices_now(dev, countof(dev), now_sec()), n = 0;
    if (!nd) return -1;
    Buf b = {0};
    for (int d = 0; d < nd; d++) {
        if (cap(dev[d]->caps, CAP_HEVC10)) continue;
        const Person *p = person_get(dev[d]->person);
        Buf agent = {0};
        i18n_unpack(&agent, dev[d]->agent);
        if (n++) buf_puts(&b, ", ");
        if (p && p->role == ROLE_DISPLAY) buf_puts(&b, p->name); /* (a TV: its account's name says it) */
        else buf_printf(&b, "%s (%s)", p ? p->name : "?", buf_cstr(&agent));
        buf_free(&agent);
    }
    if (ncap) str_copy(names, ncap, buf_cstr(&b));
    buf_free(&b);
    return n;
}

/* ---------------------------------------------------------------- counting */

typedef struct {
    char why[96];     /* a probe's reason (English), or a kind from cant() */
    int n;
    const MediaProbe *one; /* one of them: its reason said again in the reader's words (media_probe_why) */
} Reason;

static void count_reason(Reason *r, int *nr, const char *why, const MediaProbe *one) {
    for (int i = 0; i < *nr; i++)
        if (str_eq(r[i].why, why)) {
            r[i].n++;
            return;
        }
    if (*nr < MAX_REASONS) {
        str_copy(r[*nr].why, sizeof r[*nr].why, why);
        r[*nr].one = one;
        r[(*nr)++].n = 1;
    }
}

static int by_count(const void *a, const void *b) { return ((const Reason *)b)->n - ((const Reason *)a)->n; }

/* The library's videos (the "Play everywhere" count's): how many play everywhere, how many on every device that
   counts, and the reasons the rest don't play everywhere. */
typedef struct {
    int all, everywhere, on_all;
    Reason why[MAX_REASONS];
    int nwhy;
} Tally;

static void tally(const MediaIndex *ix, const MediaDevice *const *dev, int ndev, Tally *t) {
    memset(t, 0, sizeof *t);
    for (int i = 0; i < ix->nrecent; i++) {
        const MediaProbe *p = &media_playable(&ix->files[ix->recent[i]])->probe;
        t->all++;
        bool every = p->playable == PLAY_YES, on_all = ndev > 0;
        for (int d = 0; d < ndev && on_all; d++) on_all = media_device_plays(dev[d]->caps, p);
        t->everywhere += every;
        t->on_all += on_all;
        if (!every) count_reason(t->why, &t->nwhy, p->why[0] ? p->why : "unknown", p);
    }
    qsort(t->why, (size_t)t->nwhy, sizeof t->why[0], by_count);
}

int media_devices_on_all(const Ctx *c, int *all) {
    const MediaDevice *dev[MAX_DEVICES];
    int ndev = devices_now(dev, MAX_DEVICES, c->now);
    Tally t;
    tally(media_lib(), dev, ndev, &t);
    *all = t.all;
    return ndev ? t.on_all : -1;
}

/* Library settings, under Status: why the rest doesn't play everywhere (everyone who sees the page), and - admins -
   the devices and what each can't play. */
void media_devices_card(Ctx *c, Buf *b) {
    const MediaIndex *ix = media_lib();
    const MediaDevice *dev[MAX_DEVICES];
    int ndev = devices_now(dev, MAX_DEVICES, c->now);
    Tally t;
    tally(ix, dev, ndev, &t);
    if (!t.all) return;
    ui_card(b, T("What your devices play"), NULL, NULL);
    if (t.nwhy) {
        H(b, "<p class=\"muted\">%s</p><ul class=\"media-why-list\" id=\"media-why-list\">",
          T("The videos that don't play on every device there is, and why:"));
        for (int i = 0; i < t.nwhy; i++) {
            char said[128];
            media_probe_why(t.why[i].one, said, sizeof said);
            H(b, "<li><b>%d</b> · %s</li>", t.why[i].n, said);
        }
        buf_puts(b, "</ul>");
    } else {
        H(b, "<p class=\"muted\">%s</p>", T("Every video plays on every device there is."));
    }
    if (!is_admin(c)) {
        ui_card_end(b);
        return;
    }
    H(b, "<h3>%s</h3>", T("The household's devices"));
    if (!ndev) {
        H(b, "<p class=\"muted\">%s</p>", T("None yet: each phone, computer and TV says what it plays once it opens Watch."));
        ui_card_end(b);
        return;
    }
    buf_puts(b, "<p class=\"muted\">");
    H(b, TN(DEVICE_DAYS, "Each one that opened Watch in the last %d day:", "Each one that opened Watch in the last %d days:"),
      DEVICE_DAYS);
    buf_puts(b, "</p><ul class=\"media-devices\" id=\"media-devices\">");
    for (int d = 0; d < ndev; d++) {
        Reason miss[MAX_REASONS];
        int nmiss = 0, plays = 0;
        for (int i = 0; i < ix->nrecent; i++) {
            const char *k = cant(dev[d]->caps, &media_playable(&ix->files[ix->recent[i]])->probe);
            if (k) count_reason(miss, &nmiss, k, NULL);
            else plays++;
        }
        qsort(miss, (size_t)nmiss, sizeof miss[0], by_count);
        const Person *p = person_get(dev[d]->person);
        Buf agent = {0};
        i18n_unpack(&agent, dev[d]->agent);
        H(b, "<li id=\"media-dev-%d\"><b>%s</b> · %s — ", dev[d]->id, p ? p->name : "?", buf_cstr(&agent));
        buf_free(&agent);
        if (plays == t.all) {
            H(b, "<span class=\"media-ok\">%s</span>", T("plays them all"));
        } else {
            H(b, T("plays %d of %d"), plays, t.all);
            buf_puts(b, "<span class=\"muted\"> · ");
            for (int i = 0; i < nmiss && i < 3; i++) {
                if (i) buf_puts(b, ", ");
                H(b, T("not %d %s"), miss[i].n, T(miss[i].why));
            }
            buf_puts(b, "</span>");
        }
        buf_puts(b, "</li>");
    }
    buf_puts(b, "</ul><p>");
    if (ndev == 1) H(b, T("%d of %d videos play on this device."), t.on_all, t.all);
    else H(b, T("%d of %d videos play on every one of these %d devices."), t.on_all, t.all, ndev);
    buf_puts(b, "</p>");
    ui_card_end(b);
}

/* The watch page, for a video that doesn't play everywhere: whether it plays on the household's devices - nothing while
   none has said. */
void media_devices_line(const Ctx *c, Buf *b, const MediaProbe *p) {
    const MediaDevice *dev[MAX_DEVICES];
    int ndev = devices_now(dev, MAX_DEVICES, c->now), nmiss = 0;
    if (!ndev) return;
    Buf miss = {0};
    for (int d = 0; d < ndev; d++) {
        if (media_device_plays(dev[d]->caps, p)) continue;
        const Person *who = person_get(dev[d]->person);
        Buf agent = {0};
        i18n_unpack(&agent, dev[d]->agent);
        if (nmiss++) buf_puts(&miss, ", ");
        buf_puts(&miss, who ? who->name : "?");
        if (agent.len) buf_printf(&miss, " (%s)", buf_cstr(&agent));
        buf_free(&agent);
    }
    if (!nmiss) {
        H(b, "<span class=\"media-devs ok\">%s</span>",
          ndev == 1 ? T("Plays on the one device of yours that has opened Watch") : T("Plays on every device of yours"));
    } else {
        buf_puts(b, "<span class=\"media-devs\">");
        H(b, T("Doesn't play on: %s"), buf_cstr(&miss));
        buf_puts(b, "</span>");
    }
    buf_free(&miss);
}

/* ---------------------------------------------------------------- the log, keeping */

static char g_said[600]; /* the last count logged: said again only when it changes */
static unsigned g_said_gen;
static int64_t g_pruned;

void media_devices_start(void) {
    g_said[0] = '\0';
    g_said_gen = 0;
    g_pruned = 0;
}

/* Once a minute: the count logged when the library changed it (a scan); devices unseen for DEVICE_KEEP_DAYS go. */
void media_devices_tick(int64_t now_s) {
    if (now_s - g_pruned >= 3600) {
        g_pruned = now_s;
        for (int i = tbl_count(&T_media_device) - 1; i >= 0; i--) {
            const MediaDevice *d = tbl_at(&T_media_device, i);
            if (now_s - d->seen > DEVICE_KEEP_DAYS * 86400LL || d->format != CAPS_FORMAT) tbl_del(&T_media_device, d->id);
        }
    }
    unsigned gen = media_lib_gen();
    if (gen == g_said_gen) return;
    g_said_gen = gen;
    const MediaDevice *dev[MAX_DEVICES];
    int ndev = devices_now(dev, MAX_DEVICES, now_s);
    Tally t;
    tally(media_lib(), dev, ndev, &t);
    if (!t.all) return;
    Buf s = {0};
    buf_printf(&s, "%d of %d videos play everywhere", t.everywhere, t.all);
    if (ndev) buf_printf(&s, ", %d on all %d devices seen", t.on_all, ndev);
    for (int i = 0; i < t.nwhy; i++) buf_printf(&s, "%s%d %s", i ? "; " : " - the rest: ", t.why[i].n, t.why[i].why);
    if (!str_eq(buf_cstr(&s), g_said)) {
        str_copy(g_said, sizeof g_said, buf_cstr(&s));
        log_info("media: %s", buf_cstr(&s));
    }
    buf_free(&s);
}

/* Someone leaves: their devices go with them (both ways - a moved-out person's list of browsers says nothing). */
int media_devices_forget(Forget *f) {
    int n = 0;
    for (int i = tbl_count(&T_media_device) - 1; i >= 0; i--) {
        const MediaDevice *d = tbl_at(&T_media_device, i);
        if (d->person != f->person) continue;
        n++;
        if (f->apply) tbl_del(&T_media_device, d->id);
    }
    return n;
}

/* tests */
int media_devices_test_ncaps(void) { return NCAPS; }
