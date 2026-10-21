/* modules/tv/tv.c — "90's TV": channels that never stop, on a shared clock.
 *
 * Pick some series (or everything), and the channel airs their episodes in a
 * loop that every screen agrees on: tune in and you land in the middle of
 * whatever is on, like broadcast TV. Plays through the media module's
 * /media/file/<id> stream; the schedule math lives in tv_plan.c. */
#include "tv.h"
#include "../../core/ui.h"
#include "../media/media.h"

static const FieldDef channel_fields[] = {
    FIELD(Channel, id, F_INT),          FIELD(Channel, number, F_INT),   FIELD(Channel, name, F_STR),
    FIELD(Channel, hue, F_INT),         FIELD(Channel, all_series, F_BOOL), FIELD(Channel, movies, F_BOOL),
    FIELD(Channel, shows, F_STR),       FIELD(Channel, order, F_INT),    FIELD(Channel, align, F_INT),
    FIELD(Channel, epoch, F_I64),       FIELD(Channel, seed, F_INT),     FIELD(Channel, created_by, F_INT),
    FIELD(Channel, created, F_I64),
};
Table T_tv_channel = TABLE("tv_channel", Channel, channel_fields);

#define MAX_CHANNELS 99

/* ================================================================ channels */

static int by_number(const void *a, const void *b) {
    return (*(Channel *const *)a)->number - (*(Channel *const *)b)->number;
}

/* A child - or a TV with a child picked - gets only channels with nothing from a folder marked "Not for
   children" on them (issue #44): one clock for everyone, so a channel can't skip a programme for them. */
static bool for_kid(const Ctx *c, const Channel *ch) {
    if (!media_kid(c)) return true;
    const TvPlan *p = tv_channel_plan(ch);
    for (int i = 0; i < p->n; i++)
        if (!media_kids_ok(c, media_file(p->items[i].file))) return false;
    return true;
}

static int channel_list(const Ctx *c, Channel **out, int max) {
    int n = 0;
    for (int i = 0; i < tbl_count(&T_tv_channel) && n < max; i++)
        if (for_kid(c, tbl_at(&T_tv_channel, i))) out[n++] = tbl_at(&T_tv_channel, i);
    qsort(out, (size_t)n, sizeof *out, by_number);
    return n;
}

Channel *tv_channel_by_number(int number) {
    for (int i = 0; i < tbl_count(&T_tv_channel); i++) {
        Channel *ch = tbl_at(&T_tv_channel, i);
        if (ch->number == number) return ch;
    }
    return NULL;
}

static int next_free_number(void) {
    for (int n = 1; n <= MAX_CHANNELS; n++)
        if (!tv_channel_by_number(n)) return n;
    return 0;
}

static bool can_edit(const Ctx *c) { return c->me && c->me->role != ROLE_DISPLAY; }

/* ========================================================= library -> plan */

static bool has_show(const Channel *ch, uint64_t id) {
    char hex[17];
    media_id_hex(id, hex);
    for (const char *p = ch->shows; (p = strstr(p, hex)) != NULL; p += 16)
        if ((p == ch->shows || p[-1] == ',') && (p[16] == ',' || p[16] == '\0')) return true;
    return false;
}

/* Can this video go on air? It needs a known length and a browser that plays it. */
static bool airable(const MediaFile *f, int *dur_s) {
    if (!f || f->type != MF_VIDEO) return false;
    const MediaFile *p = media_playable(f); /* its play-everywhere copy, when converted: that is what airs */
    if (p->probe.duration_ms < 1000) return false;
    if (p->probe.playable != PLAY_YES && p->probe.playable != PLAY_MAYBE) return false;
    *dur_s = (int)((p->probe.duration_ms + 999) / 1000);
    return true;
}

static void build_plan(const Channel *ch, TvPlan *out) {
    const MediaIndex *ix = media_lib();
    int cap = ix->nepisodes + ix->nmovies + 1;
    TvItem *all = xmalloc((size_t)cap * sizeof *all), *list = xmalloc((size_t)cap * sizeof *list);
    TvStream *streams = xmalloc((size_t)(ix->nseries + 1) * sizeof *streams);
    int ns = 0, n = 0, skipped = 0, d = 0;
    for (int i = 0; i < ix->nseries; i++) {
        const MediaSeries *s = &ix->series[i];
        if (!ch->all_series && !has_show(ch, s->id)) continue;
        int first = n;
        for (int e = 0; e < s->count; e++) {
            const MediaFile *f = media_episode(s, e);
            if (airable(f, &d)) all[n++] = (TvItem){f->id, d};
            else skipped++;
        }
        if (n > first) streams[ns++] = (TvStream){all + first, n - first};
    }
    if (ch->movies) {
        int first = n;
        for (int i = 0; i < ix->nmovies; i++) {
            const MediaFile *f = &ix->files[ix->movies[i]];
            if (airable(f, &d)) all[n++] = (TvItem){f->id, d};
            else skipped++;
        }
        if (n > first) streams[ns++] = (TvStream){all + first, n - first};
    }
    int m = n;
    if (ch->order == TV_SHUFFLE) {
        memcpy(list, all, (size_t)n * sizeof *all);
        tv_shuffle(list, n, (uint32_t)ch->seed);
    } else {
        m = tv_rotate(streams, ns, list, cap);
    }
    tv_plan_init(out, list, m, ch->align == TV_ALIGN_HALF_HOUR);
    out->skipped = skipped;
    free(all);
    free(list);
    free(streams);
}

/* Plans are rebuilt only when the channel or the library changes: one slot per
   possible channel, so the 1 s tick and every render never evict each other. */
#define PLAN_CACHE MAX_CHANNELS
static struct {
    bool used;
    int channel;
    uint64_t key;
    TvPlan plan;
} g_plans[PLAN_CACHE];
static int g_plan_next;

void tv_plans_reset(void) {
    for (int i = 0; i < PLAN_CACHE; i++)
        if (g_plans[i].used) tv_plan_free(&g_plans[i].plan);
    memset(g_plans, 0, sizeof g_plans);
    g_plan_next = 0;
}

static uint64_t plan_key(const Channel *ch) {
    const MediaIndex *ix = media_lib();
    uint64_t v[8] = {ch->all_series, ch->movies, (uint64_t)ch->order, (uint64_t)ch->align, (uint32_t)ch->seed,
                     (uint64_t)(uintptr_t)ix, (uint64_t)ix->nfiles, (uint64_t)ix->scanned_at};
    return hash64(ch->shows, strlen(ch->shows)) ^ (hash64(v, sizeof v) * 31);
}

const TvPlan *tv_channel_plan(const Channel *ch) {
    static TvPlan empty;
    if (!ch) return &empty;
    uint64_t key = plan_key(ch);
    int slot = -1;
    for (int i = 0; i < PLAN_CACHE && slot < 0; i++)
        if (g_plans[i].used && g_plans[i].channel == ch->id) slot = i;
    if (slot >= 0 && g_plans[slot].key == key) return &g_plans[slot].plan;
    if (slot < 0) /* a free slot, or one whose channel was deleted */
        for (int i = 0; i < PLAN_CACHE && slot < 0; i++)
            if (!g_plans[i].used || !tbl_get(&T_tv_channel, g_plans[i].channel)) slot = i;
    if (slot < 0) slot = g_plan_next++ % PLAN_CACHE;
    if (g_plans[slot].used) tv_plan_free(&g_plans[slot].plan);
    g_plans[slot].used = true;
    g_plans[slot].channel = ch->id;
    g_plans[slot].key = key;
    build_plan(ch, &g_plans[slot].plan);
    return &g_plans[slot].plan;
}

/* ================================================================== labels */

static void ep_code(Buf *b, const MediaFile *f) {
    if (!f->name.season && !f->name.episode) return;
    H(b, "S%02dE%02d", f->name.season, f->name.episode);
    if (f->name.episode_end > f->name.episode) H(b, "-E%02d", f->name.episode_end);
}

static const char *prog_name(const MediaFile *f) { return f ? f->name.title : T("Unknown programme"); }

static void prog_title(Buf *b, const MediaFile *f) { html_esc(b, prog_name(f)); }

static void prog_sub(Buf *b, const MediaFile *f) {
    if (!f) return;
    if (f->name.kind == MEDIA_EPISODE) {
        ep_code(b, f);
        if (f->name.ep_title[0]) H(b, " · %s", f->name.ep_title);
    } else if (f->name.year) {
        H(b, "%d", f->name.year);
    }
}

static int bp(int64_t part, int64_t whole) { /* basis points, 0..10000 */
    if (whole <= 0) return 0;
    return (int)CLAMP(part * 10000 / whole, 0, 10000);
}

static void pct(Buf *b, int basis) { H(b, "%d.%02d%%", basis / 100, basis % 100); }

static void left(Buf *b, int64_t seconds) {
    if (seconds < 60) {
        buf_puts(b, T("less than a minute left"));
        return;
    }
    Buf d = {0};
    ui_duration(&d, seconds);
    H(b, T("%s left"), buf_cstr(&d));
    buf_free(&d);
}

/* "20:30" for a sentence's %s */
static const char *hm_of(int64_t t, char out[6]) {
    fmt_hm(t, out);
    return out;
}

/* "Now: …" line + progress for a channel, used by the list and widgets */
static bool channel_now(const Channel *ch, int64_t now, TvSlot *s, const MediaFile **f) {
    if (!tv_now(tv_channel_plan(ch), ch->epoch, now, s)) return false;
    *f = media_file(s->file);
    return true;
}

/* ===================================================================== pages */

static void no_library(Ctx *c, Buf *b) {
    if (!is_admin(c)) { /* Library settings are an admin's (issue #58) */
        ui_empty(b, "📼", T("Nothing to broadcast yet"), T("TV channels air the series and movies from your library."));
        Buf who = {0};
        ui_admins(&who); /* (HTML: names escaped) */
        buf_puts(b, "<p class=\"center muted\">");
        H(b, T("Ask %r to add the folders with your movies and series."), buf_cstr(&who));
        buf_puts(b, "</p>");
        buf_free(&who);
        return;
    }
    ui_empty(b, "📼", T("Nothing to broadcast yet"),
             T("TV channels air the series and movies from your library. Point Hemma at your drive first."));
    H(b, "<p class=\"center\"><a class=\"btn primary\" href=\"/media/settings\">%s</a></p>", T("Set up the library"));
}

static void channel_form(Ctx *c, Buf *b, const Channel *ch) {
    const MediaIndex *ix = media_lib();
    ui_modal(b, ch ? T("Edit channel") : T("New channel"), ch ? "/tv" : "/tv");
    ui_form(b, "tv.save", "stack");
    if (ch) ui_hidden_int(b, "id", ch->id);
    H(b,
      "<div class=\"field-row\"><label class=\"field tv-f-num\"><span>%s</span><input type=\"number\" "
      "name=\"number\" min=\"1\" max=\"99\" required value=\"%d\"></label><label class=\"field tv-f-name\">"
      "<span>%s</span><input name=\"name\" maxlength=\"36\" value=\"%s\" placeholder=\"%s\"%r></label></div>",
      T("Channel"), ch ? ch->number : next_free_number(), T("Name"), ch ? ch->name : "",
      T("Sitcoms, Kids, Late night…"), ch ? "" : " autofocus");
    H(b, "<div class=\"field\"><span>%s</span><label class=\"tv-check\"><input type=\"checkbox\" name=\"all\" "
         "value=\"1\"%r> <b>%s</b> <small class=\"muted\">%s</small></label>",
      T("What's on"), ch && ch->all_series ? " checked" : "", T("Every series"), T("new ones join automatically"));
    if (ix->nseries) {
        buf_puts(b, "<div class=\"tv-shows\">");
        for (int i = 0; i < ix->nseries; i++) {
            const MediaSeries *s = &ix->series[i];
            int ok = 0, d;
            for (int e = 0; e < s->count; e++) ok += airable(media_episode(s, e), &d);
            H(b, "<label class=\"tv-check\"><input type=\"checkbox\" name=\"show\" value=\"%X\"%r> %s <small "
                 "class=\"muted\">",
              s->id, ch && has_show(ch, s->id) ? " checked" : "", s->title);
            H(b, TN(ok, "%d episode", "%d episodes"), ok);
            buf_puts(b, "</small></label>");
        }
        buf_puts(b, "</div>");
    }
    H(b, "<label class=\"tv-check\"><input type=\"checkbox\" name=\"movies\" value=\"1\"%r> %s</label></div>",
      ch && ch->movies ? " checked" : "", T("Movies too"));
    bool shuffle = ch && ch->order == TV_SHUFFLE, half = !ch || ch->align == TV_ALIGN_HALF_HOUR;
    H(b,
      "<div class=\"field\"><span>%s</span><div class=\"tv-opts\"><label class=\"tv-opt\"><input type=\"radio\" "
      "name=\"order\" value=\"rotate\"%r><span><b>%s</b><small>%s</small></span></label>"
      "<label class=\"tv-opt\"><input type=\"radio\" name=\"order\" value=\"shuffle\"%r><span><b>%s</b><small>%s"
      "</small></span></label></div></div>",
      T("Order"), shuffle ? "" : " checked", T("Take turns"), T("Show A, show B, show A …"), shuffle ? " checked" : "",
      T("Shuffle"), T("A surprise every time"));
    H(b,
      "<div class=\"field\"><span>%s</span><div class=\"tv-opts\"><label class=\"tv-opt\"><input type=\"radio\" "
      "name=\"align\" value=\"half\"%r><span><b>%s</b><small>%s</small></span></label><label class=\"tv-opt\"><input "
      "type=\"radio\" name=\"align\" value=\"none\"%r><span><b>%s</b><small>%s</small></span></label></div></div>",
      T("Timing"), half ? " checked" : "", T("On the half hour"), T("Shows start at :00 and :30, like real TV"),
      half ? "" : " checked", T("Back to back"), T("No breaks between shows"));
    H(b, "<div class=\"field\"><span>%s</span>", T("Colour"));
    ui_hue_picker(b, "hue", ch ? ch->hue : 150);
    H(b, "</div><div class=\"modal-f\"><button class=\"btn primary\">%s</button></div>", T("Save"));
    ui_form_end(b);
    if (ch) {
        Buf q = {0};
        buf_printf(&q, T("Take channel %d off the air?"), ch->number);
        ui_form_confirm(b, "tv.delete", "danger-zone", buf_cstr(&q));
        ui_hidden_int(b, "id", ch->id);
        H(b, "<button class=\"btn danger\">%s</button>", T("Delete channel"));
        ui_form_end(b);
        buf_free(&q);
    }
    ui_modal_end(b);
}

static void guide(Ctx *c, Buf *b, Channel **chs, int n) {
    int64_t from = c->now / TV_SLOT_S * TV_SLOT_S, to = from + 3 * 3600, span = to - from;
    H(b, "<section class=\"card tv-guide\"><header class=\"card-h\"><h2>%s</h2></header>"
         "<div class=\"tv-g-scroll\"><div class=\"tv-g\"><div class=\"tv-g-row tv-g-head\"><span></span>"
         "<div class=\"tv-g-times\">",
      T("TV guide"));
    for (int64_t t = from; t < to; t += TV_SLOT_S) {
        H(b, "<span style=\"--x:");
        pct(b, bp(t - from, span));
        buf_puts(b, "\">");
        ui_hm(b, t);
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</div></div>");
    for (int i = 0; i < n; i++) {
        Channel *ch = chs[i];
        H(b, "<a class=\"tv-g-row\" href=\"/tv/%d\" style=\"--h:%d\"><span class=\"tv-g-ch\"><span class=\"tv-num\">%02d"
             "</span><span>%s</span></span><span class=\"tv-g-lane\">",
          ch->number, ch->hue, ch->number, ch->name);
        TvSlot s[64];
        int k = tv_window(tv_channel_plan(ch), ch->epoch, from, to, s, countof(s));
        for (int j = 0; j < k; j++) {
            int64_t a = MAX(s[j].start, from), z = MIN(s[j].end, to);
            if (z > a) {
                const MediaFile *f = media_file(s[j].file);
                bool on = s[j].start <= c->now && c->now < s[j].end;
                H(b, "<span class=\"tv-g-p%r\" style=\"--x:", on ? " now" : "");
                pct(b, bp(a - from, span));
                buf_puts(b, ";--w:");
                pct(b, bp(z - a, span));
                buf_puts(b, "\"><b>");
                prog_title(b, f);
                buf_puts(b, "</b><small>");
                ui_hm(b, s[j].start);
                buf_puts(b, " ");
                prog_sub(b, f);
                buf_puts(b, "</small></span>");
            }
            int64_t ga = MAX(s[j].end, from), gz = MIN(s[j].next, to);
            if (gz > ga) {
                buf_puts(b, "<span class=\"tv-g-gap\" style=\"--x:");
                pct(b, bp(ga - from, span));
                buf_puts(b, ";--w:");
                pct(b, bp(gz - ga, span));
                buf_puts(b, "\"></span>");
            }
        }
        if (!k) H(b, "<span class=\"tv-g-off\">%s</span>", T("No signal"));
        buf_puts(b, "<i class=\"tv-g-nowline\" style=\"--x:");
        pct(b, bp(c->now - from, span));
        buf_puts(b, "\"></i></span></a>");
    }
    buf_puts(b, "</div></div></section>");
}

static void overview(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Live TV"));
    Channel *chs[MAX_CHANNELS];
    int n = channel_list(c, chs, MAX_CHANNELS);
    const MediaIndex *ix = media_lib();
    bool library = ix->nseries || ix->nmovies;
    Buf add = {0};
    if (can_edit(c) && n && library && next_free_number()) {
        buf_puts(&add, "<a class=\"btn\" href=\"/tv?new=1\">");
        ui_icon(&add, "plus");
        H(&add, "<span>%s</span></a>", T("New channel"));
    }
    media_lib_head(c, b, "tv", T("Live TV"), T("Always on — tune in like it's 1996"), add.len ? buf_cstr(&add) : NULL);
    buf_free(&add);
    if (!library && !n) {
        no_library(c, b);
        return;
    }
    if (!n) {
        ui_empty(b, "📺", T("Start your first channel"),
                 T("A channel plays your series in a loop that never stops. Everyone who tunes in sees the same "
                   "moment — like TV used to be."));
        if (can_edit(c)) {
            buf_puts(b, "<div class=\"tv-start\">");
            ui_form(b, "tv.quick", "inline");
            H(b, "<button class=\"btn primary big\">%s</button>", T("Channel 1 · Everything"));
            ui_form_end(b);
            H(b, "<a class=\"btn big\" href=\"/tv?new=1\">%s</a></div>", T("Choose shows…"));
        }
    } else {
        buf_puts(b, "<div class=\"tv-list\">");
        for (int i = 0; i < n; i++) {
            Channel *ch = chs[i];
            TvSlot s;
            const MediaFile *f = NULL;
            bool on = channel_now(ch, c->now, &s, &f);
            char key[32] = ""; /* 1-9 on the keyboard tune straight in, like a remote */
            if (ch->number <= 9) snprintf(key, sizeof key, " data-key=\"%d\"", ch->number);
            H(b, "<div class=\"tv-ch\" style=\"--h:%d\"><a class=\"tv-ch-link\" href=\"/tv/%d\"%r><span "
                 "class=\"tv-num\">%02d</span><span class=\"tv-ch-main\"><b>%s</b>",
              ch->hue, ch->number, key, ch->number, ch->name);
            if (!on) {
                H(b, "<span class=\"tv-now muted\">%s</span>", T("No signal"));
            } else if (s.gap) {
                const MediaFile *nf = NULL;
                TvSlot nx;
                if (channel_now(ch, s.next, &nx, &nf)) {
                    char hm[6];
                    buf_puts(b, "<span class=\"tv-now\">");
                    H(b, T("Intermission · next %s at %s"), prog_name(nf), hm_of(s.next, hm));
                    buf_puts(b, "</span>");
                }
            } else {
                buf_puts(b, "<span class=\"tv-now\">");
                H(b, T("Now: <b>%s</b>"), prog_name(f));
                buf_puts(b, " ");
                prog_sub(b, f);
                buf_puts(b, "</span><span class=\"tv-bar\"><i style=\"width:");
                pct(b, bp(c->now - s.start, s.end - s.start));
                buf_puts(b, "\"></i></span><small class=\"muted\">");
                left(b, s.end - c->now);
                buf_puts(b, "</small>");
            }
            buf_puts(b, "</span></a>");
            if (can_edit(c)) {
                char label[64];
                snprintf(label, sizeof label, T("Edit channel %d"), ch->number);
                H(b, "<a class=\"icon-btn\" href=\"/tv?edit=%d\" aria-label=\"%s\">", ch->id, label);
                ui_icon(b, "edit");
                buf_puts(b, "</a>");
            }
            buf_puts(b, "</div>");
        }
        buf_puts(b, "</div>");
        guide(c, b, chs, n);
    }
    if (!can_edit(c)) return;
    if (str_eq(ctx_q(c, "new"), "1") && next_free_number()) channel_form(c, b, NULL);
    long long edit = ctx_qint(c, "edit", 0);
    if (edit) {
        Channel *ch = tbl_get(&T_tv_channel, (int)edit);
        if (ch) channel_form(c, b, ch);
    }
}

/* tvq: what the channel's links carry - "" on the ordinary page, "?tv=1&amp;back=..." in TV mode
   (already escaped for an attribute). */
static void screen(Ctx *c, Buf *b, const Channel *ch, const TvPlan *p, const char *tvq) {
    TvSlot s;
    bool on = tv_now(p, ch->epoch, c->now, &s);
    int64_t mark = on ? s.start : 0; /* fresh ids per airing: the static and the OSD play again */
    /* full screen is the screen (the player's bar stays over the picture); the video is one
       element for good (data-static="<programme>"): the next programme or channel morphs into it,
       so full screen stays */
    buf_puts(b, "<div class=\"tv-screen\" id=\"tv-screen\">");
    if (on && !s.gap) {
        /* live (owner): it can't be paused or wound (wire.js data-onair keeps it at "now"); the
           player's live bar has the sound and full screen only. TV mode: OK on the remote = sound. */
        char key[48];
        snprintf(key, sizeof key, "%016llx-%lld", (unsigned long long)s.file, (long long)mark);
        char src[100];
        media_file_src(c, s.file, src, sizeof src);
        H(b,
          "<video id=\"tv-video\" class=\"tv-video\" src=\"%s\" autoplay playsinline preload=\"auto\" "
          "data-static=\"%s\" data-onair data-resume=\"%L\" data-next=\"/tv/%d%r\"></video>",
          src, key, (long long)s.offset, ch->number, tvq);
        char hm[6], trouble[160];
        fmt_hm(s.end, hm);
        snprintf(trouble, sizeof trouble, T("This programme won't play on this screen. The next one starts at %s."),
                 hm);
        UiPlayer pl = {.box = "tv-screen", .video = "tv-video", .key = key, .live = true, .focus_sound = *tvq != 0,
                       .tv = *tvq != 0, .trouble = trouble};
        ui_player(b, &pl);
    } else if (on) {
        TvSlot nx;
        const MediaFile *nf = tv_now(p, ch->epoch, s.next, &nx) ? media_file(nx.file) : NULL;
        char hm[6];
        H(b, "<div class=\"tv-card tv-bars\" id=\"tv-i-%d-%L\"><div class=\"tv-card-box\"><small>", ch->number,
          (long long)s.next);
        H(b, T("Up next at %s"), hm_of(s.next, hm));
        buf_puts(b, "</small><b>");
        prog_title(b, nf);
        buf_puts(b, "</b><span>");
        prog_sub(b, nf);
        buf_puts(b, "</span></div></div>");
    } else {
        H(b, "<div class=\"tv-card tv-snow\"><div class=\"tv-card-box\"><b>%s</b><span>", T("No signal"));
        if (p->skipped)
            buf_printf(b,
                       TN(p->skipped, "%d video here can't be aired (unknown length or won't play in browsers).",
                          "%d videos here can't be aired (unknown length or won't play in browsers)."),
                       p->skipped);
        else buf_puts(b, T("This channel has nothing to air yet."));
        buf_puts(b, "</span></div></div>");
    }
    /* (no scan lines over the picture any more - owner, 2026-10-14: they cost the show its quality;
       the static between channels and the channel's number fade within seconds) */
    H(b, "<div class=\"tv-static\" id=\"tv-s-%d-%L\"></div>", ch->number, (long long)mark);
    H(b, "<div class=\"tv-osd\" id=\"tv-o-%d-%L\"><span class=\"tv-osd-n\">CH %02d</span>", ch->number,
      (long long)mark, ch->number);
    if (on && !s.gap) {
        buf_puts(b, "<span class=\"tv-osd-t\">");
        prog_title(b, media_file(s.file));
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</div></div>");
}

static void channel_page(Ctx *c, Buf *b, Channel *ch) {
    ctx_title(c, "CH %02d · %s", ch->number, ch->name);
    const TvPlan *p = tv_channel_plan(ch);
    Channel *chs[MAX_CHANNELS];
    int n = channel_list(c, chs, MAX_CHANNELS), at = 0;
    for (int i = 0; i < n; i++)
        if (chs[i]->id == ch->id) at = i;
    const Channel *down = chs[(at + n - 1) % n], *up = chs[(at + 1) % n];

    char sub[64];
    snprintf(sub, sizeof sub, T("Channel %d"), ch->number);
    media_lib_head(c, b, "tv", ch->name, sub, NULL);
    H(b, "<div class=\"tv-room\" style=\"--h:%d\"><div class=\"tv-set\">", ch->hue);
    screen(c, b, ch, p, "");
    H(b, "<div class=\"tv-knobs\"><a class=\"tv-knob\" href=\"/tv/%d\" data-key=\"ArrowDown\" aria-label=\"%s\">",
      down->number, T("Channel down"));
    ui_icon(b, "left");
    H(b, "</a><span class=\"tv-brand\"><span class=\"tv-num\">%02d</span> %s</span><a class=\"tv-knob\" href=\"/tv/%d\" "
         "data-key=\"ArrowUp\" aria-label=\"%s\">",
      ch->number, ch->name, up->number, T("Channel up"));
    ui_icon(b, "right");
    buf_puts(b, "</a></div></div>");

    /* now + coming up */
    buf_puts(b, "<aside class=\"card tv-info\">");
    TvSlot s;
    if (tv_now(p, ch->epoch, c->now, &s)) {
        const MediaFile *f = media_file(s.file);
        if (!s.gap) {
            H(b, "<p class=\"tv-label\">%s</p><h2>", T("On now"));
            prog_title(b, f);
            buf_puts(b, "</h2><p class=\"muted\">");
            prog_sub(b, f);
            buf_puts(b, "</p><span class=\"tv-bar\"><i style=\"width:");
            pct(b, bp(c->now - s.start, s.end - s.start));
            buf_puts(b, "\"></i></span><p class=\"muted small\">");
            ui_span(b, s.start, s.end);
            buf_puts(b, " · ");
            left(b, s.end - c->now);
            buf_puts(b, "</p>");
        } else {
            char hm[6];
            H(b, "<p class=\"tv-label\">%s</p><p class=\"muted\">", T("Intermission"));
            H(b, T("Back at %s"), hm_of(s.next, hm));
            buf_puts(b, "</p>");
        }
        TvSlot up_next[8];
        int k = tv_window(p, ch->epoch, c->now, c->now + 6 * 3600, up_next, countof(up_next));
        H(b, "<p class=\"tv-label\">%s</p><ul class=\"tv-up\">", TC("tv", "Coming up"));
        int shown = 0;
        for (int i = 0; i < k && shown < 5; i++) {
            if (up_next[i].start <= c->now) continue;
            const MediaFile *nf = media_file(up_next[i].file);
            buf_puts(b, "<li><time>");
            ui_hm(b, up_next[i].start);
            buf_puts(b, "</time><span><b>");
            prog_title(b, nf);
            buf_puts(b, "</b> <small class=\"muted\">");
            prog_sub(b, nf);
            buf_puts(b, "</small></span></li>");
            shown++;
        }
        if (!shown) H(b, "<li class=\"muted\">%s</li>", T("More of the same."));
        buf_puts(b, "</ul>");
    } else {
        H(b, "<p class=\"muted\">%s</p>", T("Nothing to air on this channel yet."));
    }
    if (p->skipped) {
        buf_puts(b, "<p class=\"muted small\">");
        H(b, TN(p->skipped, "%d video left out: unknown length, or browsers can't play them.",
                "%d videos left out: unknown length, or browsers can't play them."),
          p->skipped);
        buf_puts(b, "</p>");
    }
    buf_puts(b, "<div class=\"tv-actions\"><a class=\"btn\" href=\"/tv\">");
    ui_icon(b, "list");
    H(b, "<span>%s</span></a>", T("Guide"));
    if (can_edit(c)) {
        H(b, "<a class=\"btn ghost\" href=\"/tv?edit=%d\">", ch->id);
        ui_icon(b, "edit");
        H(b, "<span>%s</span></a>", T("Edit"));
    }
    buf_puts(b, "</div>");
    /* the remote: 1-9 tune straight in */
    H(b, "<nav class=\"tv-remote\" aria-label=\"%s\">", T("Channels"));
    for (int i = 0; i < n; i++) {
        H(b, "<a class=\"tv-num%r\" href=\"/tv/%d\"", chs[i]->id == ch->id ? " on" : "", chs[i]->number);
        if (chs[i]->number <= 9) H(b, " data-key=\"%d\"", chs[i]->number);
        H(b, " title=\"%s\">%02d</a>", chs[i]->name, chs[i]->number);
    }
    buf_puts(b, "</nav></aside></div>");
}

/* A channel on the TV (?tv=1, opened from a TV screen's "On TV" row): the picture fills the
   screen, tuned in at the shared clock; Up and Down on the remote change channel, 1-9 tune
   straight in, Back returns to the screen. */
static void channel_tv(Ctx *c, Buf *b, Channel *ch) {
    ctx_title(c, "CH %02d · %s", ch->number, ch->name);
    const TvPlan *p = tv_channel_plan(ch);
    Channel *chs[MAX_CHANNELS];
    int n = channel_list(c, chs, MAX_CHANNELS), at = 0;
    for (int i = 0; i < n; i++)
        if (chs[i]->id == ch->id) at = i;
    const Channel *down = chs[(at + n - 1) % n], *up = chs[(at + 1) % n];
    char back[400];
    media_tv_back(c, back, sizeof back);
    Buf q = {0};
    H(&q, "?tv=1&amp;back=%u", back);
    const char *tvq = buf_cstr(&q);

    media_tv_open(c, b, back, "tv-full");
    H(b, "<div class=\"tv-full-screen\" style=\"--h:%d\">", ch->hue);
    screen(c, b, ch, p, tvq);
    buf_puts(b, "</div>");

    H(b, "<div class=\"tv-full-bar\" style=\"--h:%d\"><a class=\"tv-knob\" href=\"/tv/%d%r\" data-key=\"ArrowDown\" "
         "aria-label=\"%s\">",
      ch->hue, down->number, tvq, T("Channel down"));
    ui_icon(b, "left");
    H(b, "</a><div class=\"tv-full-now\"><span class=\"tv-num\" style=\"--h:%d\">%02d</span><div><b>%s</b>", ch->hue,
      ch->number, ch->name);
    TvSlot s;
    if (tv_now(p, ch->epoch, c->now, &s)) {
        const MediaFile *f = media_file(s.file);
        if (!s.gap) {
            buf_puts(b, "<span>");
            prog_title(b, f);
            buf_puts(b, " <small>");
            prog_sub(b, f);
            buf_puts(b, " · ");
            left(b, s.end - c->now);
            buf_puts(b, "</small></span><span class=\"tv-bar\"><i style=\"width:");
            pct(b, bp(c->now - s.start, s.end - s.start));
            buf_puts(b, "\"></i></span>");
        } else {
            char hm[6];
            buf_puts(b, "<span>");
            H(b, T("Intermission · back at %s"), hm_of(s.next, hm));
            buf_puts(b, "</span>");
        }
    } else {
        H(b, "<span>%s</span>", T("No signal"));
    }
    H(b, "</div></div><a class=\"tv-knob\" href=\"/tv/%d%r\" data-key=\"ArrowUp\" aria-label=\"%s\">", up->number, tvq,
      T("Channel up"));
    ui_icon(b, "right");
    H(b, "</a></div><nav class=\"tv-full-remote\" aria-label=\"%s\">", T("Channels"));
    for (int i = 0; i < n; i++) {
        H(b, "<a class=\"tv-num%r\" href=\"/tv/%d%r\" style=\"--h:%d\"", chs[i]->id == ch->id ? " on" : "", chs[i]->number,
          tvq, chs[i]->hue);
        if (chs[i]->number <= 9) H(b, " data-key=\"%d\"", chs[i]->number);
        H(b, " title=\"%s\">%02d</a>", chs[i]->name, chs[i]->number);
    }
    buf_puts(b, "</nav></div>");
    buf_free(&q);
}

static void page(Ctx *c, Buf *b) {
    if (str_starts(c->path, "/tv/")) {
        long long n = str_int_or(c->path + 4, 0);
        Channel *ch = n > 0 ? tv_channel_by_number((int)n) : NULL;
        if (ch && !for_kid(c, ch)) ch = NULL; /* (as if it weren't on the air) */
        if (ch && media_tv_mode(c)) channel_tv(c, b, ch);
        if (ch && !media_tv_mode(c)) channel_page(c, b, ch);
        if (ch) return;
        ctx_title(c, "%s", T("Live TV"));
        if (media_tv_mode(c)) media_tv_open(c, b, NULL, NULL);
        else media_lib_head(c, b, "tv", T("Live TV"), NULL, NULL);
        ui_empty(b, "📡", T("No such channel"), T("That channel isn't on the air."));
        if (c->bare) buf_puts(b, "</div>");
        else H(b, "<p class=\"center\"><a class=\"btn\" href=\"/tv\">%s</a></p>", T("All channels"));
        return;
    }
    overview(c, b);
}

/* =================================================================== actions */

static void act_save(Ctx *c, Fields *f) {
    int id = (int)fields_int(f, "id", 0);
    Channel ch = {0};
    if (id) {
        Channel *old = tbl_get(&T_tv_channel, id);
        if (!old) {
            ctx_fail(c, "%s", T("That channel is gone"));
            return;
        }
        ch = *old;
    }
    long long number = fields_int(f, "number", 0);
    if (number < 1 || number > MAX_CHANNELS) {
        ctx_fail_at(c, "number", T("Channel numbers go from 1 to %d"), MAX_CHANNELS);
        return;
    }
    Channel *other = tv_channel_by_number((int)number);
    if (other && other->id != ch.id) {
        ctx_fail_at(c, "number", T("Channel %lld is already %s"), number, other->name);
        return;
    }
    char name[64];
    str_copy(name, sizeof name, fields_str(f, "name"));
    str_trim(name);
    /* (a name made for whoever made the channel, in their words - as if they had typed it: it's the channel's
       name from then on) */
    if (!name[0]) snprintf(name, sizeof name, T("Channel %lld"), number);
    Buf shows = {0};
    for (int i = 0; i < f->n; i++) {
        uint64_t sid;
        if (!str_eq(f->k[i], "show") || !media_parse_id(f->v[i], &sid)) continue;
        char hex[17];
        media_id_hex(sid, hex);
        if (shows.len && strstr(buf_cstr(&shows), hex)) continue;
        if (shows.len + 18 > sizeof ch.shows) break;
        if (shows.len) buf_putc(&shows, ',');
        buf_puts(&shows, hex);
    }
    bool all = fields_get(f, "all") != NULL, movies = fields_get(f, "movies") != NULL;
    if (!all && !movies && !shows.len) {
        buf_free(&shows);
        ctx_fail(c, "%s", T("Pick at least one show — or every series"));
        return;
    }
    ch.number = (int)number;
    str_copy(ch.name, sizeof ch.name, name);
    ch.hue = (int)CLAMP(fields_int(f, "hue", 150), 0, 359);
    ch.all_series = all;
    ch.movies = movies;
    str_copy(ch.shows, sizeof ch.shows, shows.len ? buf_cstr(&shows) : "");
    buf_free(&shows);
    ch.order = str_eq(fields_str(f, "order"), "shuffle") ? TV_SHUFFLE : TV_ROTATE;
    ch.align = str_eq(fields_str(f, "align"), "none") ? TV_ALIGN_NONE : TV_ALIGN_HALF_HOUR;
    if (!id) {
        ch.epoch = c->now;
        uint32_t seed;
        rand_bytes(&seed, sizeof seed);
        ch.seed = (int)(seed & 0x7fffffff);
        ch.created_by = c->me ? c->me->id : 0;
        ch.created = c->now;
    }
    tbl_put(&T_tv_channel, &ch);
    if (!id) {
        activity_add(ch.created_by, "📺", "/tv", N_("%s started channel %d · %s"), c->me ? c->me->name : "Someone",
                     ch.number, ch.name);
        ctx_ok(c, T("Channel %d is on the air"), ch.number);
        ctx_redirect(c, "/tv/%d", ch.number);
    } else {
        ctx_ok(c, T("Saved channel %d"), ch.number);
        ctx_redirect(c, "/tv");
    }
}

static void act_delete(Ctx *c, Fields *f) {
    Channel *ch = tbl_get(&T_tv_channel, (int)fields_int(f, "id", 0));
    if (!ch) {
        ctx_fail(c, "%s", T("That channel is gone"));
        return;
    }
    int number = ch->number;
    tbl_del(&T_tv_channel, ch->id);
    ctx_ok(c, T("Channel %d is off the air"), number);
    ctx_redirect(c, "/tv");
}

static void act_quick(Ctx *c, Fields *f) {
    int number = next_free_number();
    if (!number) {
        ctx_fail(c, "%s", T("Every channel number is taken"));
        return;
    }
    Fields g = {0};
    char num[8];
    snprintf(num, sizeof num, "%d", number);
    fields_add(&g, "number", num);
    fields_add(&g, "name", T("Everything")); /* (the channel's name from then on: in its maker's words) */
    fields_add(&g, "all", "1");
    fields_add(&g, "movies", "1");
    fields_add(&g, "order", "rotate");
    fields_add(&g, "align", "half");
    fields_add(&g, "hue", "150");
    act_save(c, &g);
    fields_free(&g);
}

/* =================================================================== widget */

static void widget(Ctx *c, Buf *b, int where) {
    Channel *chs[MAX_CHANNELS];
    int n = channel_list(c, chs, MAX_CHANNELS);
    if (!n) {
        const MediaIndex *ix = media_lib();
        if (where == WIDGET_HOME && ix->nseries && c->me && c->me->role != ROLE_DISPLAY) { /* (a hint: people only) */
            ui_card(b, T("Live TV"), "/tv", T("Start"));
            H(b, "<p class=\"muted\">%r</p>",
              T("Turn your series into channels that never stop — <a href=\"/tv\">start one</a>."));
            ui_card_end(b);
        }
        return;
    }
    ui_card(b, T("On TV now"), "/tv", T("Guide"));
    buf_puts(b, "<ul class=\"tv-w\">");
    for (int i = 0; i < n && i < 4; i++) {
        Channel *ch = chs[i];
        TvSlot s;
        const MediaFile *f = NULL;
        bool on = channel_now(ch, c->now, &s, &f);
        H(b, "<li><a href=\"/tv/%d\" style=\"--h:%d\"><span class=\"tv-num\">%02d</span><span class=\"tv-w-t\">",
          ch->number, ch->hue, ch->number);
        if (!on) {
            H(b, "<span class=\"muted\">%s</span>", T("No signal"));
        } else if (s.gap) {
            TvSlot nx;
            const MediaFile *nf = NULL;
            char hm[6];
            if (channel_now(ch, s.next, &nx, &nf))
                H(b, T("<span class=\"muted\">Next: </span><b>%s</b> <small>at %s</small>"), prog_name(nf),
                  hm_of(s.next, hm));
            else H(b, "<span class=\"muted\">%s</span>", T("Up next"));
        } else {
            char hm[6];
            buf_puts(b, "<b>");
            prog_title(b, f);
            buf_puts(b, "</b> <small>");
            prog_sub(b, f);
            buf_puts(b, " · ");
            H(b, TC("time", "until %s"), hm_of(s.end, hm));
            buf_puts(b, "</small>");
        }
        buf_puts(b, "</span></a></li>");
    }
    buf_puts(b, "</ul>");
    ui_card_end(b);
}

/* ==================================================================== blocks */

/* "On TV": every channel's programme right now as a wide tile - the channel's number, what's on,
   how long it has left - for a screen's row (TV mode: tune in full screen, Back returns to the
   screen) and for Watch's home (kind 0: the ordinary channel page). Channels playing come first,
   those between programmes after them (issue #101: the row was "On now" and mixed both). Channels
   with nothing to air are left out; none on the air: no row. */
static bool row_onnow(Ctx *c, Buf *b, int kind, const char *arg) {
    (void)arg;
    Channel *chs[MAX_CHANNELS];
    int n = channel_list(c, chs, MAX_CHANNELS), shown = 0;
    char back[400];
    media_row_back(c, back, sizeof back);
    Buf row = {0};
    for (int k = 0; k < 2 * n; k++) { /* (playing ones, then those between programmes) */
        const Channel *ch = chs[k % n];
        TvSlot s, nx;
        const MediaFile *f = NULL;
        if (!channel_now(ch, c->now, &s, &f)) continue; /* no signal */
        if (s.gap != (k >= n)) continue;
        if (s.gap && !channel_now(ch, s.next, &nx, &f)) continue;
        char path[16];
        snprintf(path, sizeof path, "/tv/%d", ch->number);
        bool wall = kind == SCREEN_WALL; /* a wall only shows (issue #80): no Live TV starting on the tablet */
        if (wall) {
            buf_puts(&row, "<div class=\"tile wide\">");
        } else {
            buf_puts(&row, "<a class=\"tile wide\" href=\"");
            media_tv_href(&row, path, kind, back);
            buf_puts(&row, "\">");
        }
        /* between programmes the tile is the next one's, dimmed and labelled with when it starts (issue #56: it
           read as playing under "On now") */
        H(&row, "<span class=\"tile-art tv-tile%r\" style=\"--h:%d\">", s.gap ? " tv-tile-off" : "", ch->hue);
        uint64_t img = media_picture(f);
        if (img) {
            buf_puts(&row, "<img src=\"");
            media_img_src(&row, img);
            buf_puts(&row, "\" alt=\"\" loading=\"lazy\" decoding=\"async\">");
        } else {
            buf_puts(&row, "<span class=\"tv-tile-t\">");
            prog_title(&row, f);
            buf_puts(&row, "</span>");
        }
        H(&row, "<span class=\"tv-num tv-tile-n\" style=\"--h:%d\">%02d</span>", ch->hue, ch->number);
        if (s.gap) {
            char hm[6];
            buf_puts(&row, "<span class=\"tv-tile-up\">");
            H(&row, T("Up next · %s"), hm_of(s.next, hm));
            buf_puts(&row, "</span>");
        } else { /* how far into the programme, along the foot of the picture */
            H(&row, "<span class=\"tile-bar\"><i style=\"width:%d%%\"></i></span>",
              bp(c->now - s.start, s.end - s.start) / 100);
        }
        buf_puts(&row, "</span><b class=\"tile-t\">");
        prog_title(&row, f);
        H(&row, "</b><span class=\"tile-s\">%s · ", ch->name);
        if (s.gap) {
            buf_puts(&row, T("Intermission"));
        } else {
            prog_sub(&row, f);
            if (f && (f->name.kind == MEDIA_EPISODE || f->name.year)) buf_puts(&row, " · ");
            left(&row, s.end - c->now);
        }
        buf_puts(&row, wall ? "</span></div>" : "</span></a>");
        shown++;
    }
    if (shown) {
        buf_puts(b, "<div class=\"tiles\">");
        buf_put(b, row.p, row.len);
        buf_puts(b, "</div>");
    }
    buf_free(&row);
    return shown > 0;
}

static const Block blocks[] = {
    {"tv.onnow", N_("On TV"), SCREEN_TV | SCREEN_WALL, row_onnow, NULL},
    {0},
};

/* ===================================================================== tick */

/* When a channel moves to its next programme, every screen should flip at
 * once (the guide, the widgets, and viewers whose video already ended). */
static struct {
    int channel;
    uint64_t key;
} g_air[MAX_CHANNELS + 1];
static int g_nair;
static int64_t g_last_check;

static void tick(int64_t now_ms) {
    if (now_ms - g_last_check < 1000) return;
    g_last_check = now_ms;
    int64_t now = now_sec();
    bool changed = false;
    int n = 0;
    for (int i = 0; i < tbl_count(&T_tv_channel) && n < MAX_CHANNELS; i++) {
        Channel *ch = tbl_at(&T_tv_channel, i);
        TvSlot s;
        uint64_t key = tv_now(tv_channel_plan(ch), ch->epoch, now, &s)
                           ? (uint64_t)s.start * 2654435761ULL ^ s.file ^ (s.gap ? 0x5bd1e995ULL : 0)
                           : 1;
        if (n >= g_nair || g_air[n].channel != ch->id || g_air[n].key != key) changed = true;
        g_air[n].channel = ch->id;
        g_air[n].key = key;
        n++;
    }
    if (n != g_nair) changed = true;
    g_nair = n;
    if (changed) app_changed();
}

/* =================================================================== module */

static void init(void) {
    T_tv_channel.what = "TV channels";
    store_register(&T_tv_channel);
}

static void start(void) {
    tv_plans_reset();
    g_nair = 0;
    g_last_check = 0;
}

static void stop(void) { tv_plans_reset(); }

static const Action actions[] = {
    {"tv.save", act_save},
    {"tv.delete", act_delete},
    {"tv.quick", act_quick},
    {0},
};

/* Live TV is part of Watch: not in the navigation itself (nav 0), its pages light up Watch's
   entry (parent) and draw Watch's tabs (media_lib_head). */
static const Module tv = {
    .id = "tv",
    .title = N_("Live TV"),
    .icon = "tv",
    .nav = 0,
    .parent = "media",
    .blocks = blocks,
    .init = init,
    .start = start,
    .stop = stop,
    .tick = tick,
    .page = page,
    .actions = actions,
    .widget = widget,
    .widget_order = 55,
};
MODULE(tv)
