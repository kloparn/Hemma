/* SOUND LANGUAGES (2.2): a video in several languages - what the library keeps of its sound tracks, the question,
   the viewer's language, the player's Language menu and its pick, and the address the player gets. */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../av/mp4.h"
#include "library.h"
#include "media.h"

static char g_root[512];

static bool snd_put(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)ctx) == n; }
static bool snd_put_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    fflush(f);
    bool ok = file_seek(f, off) && fwrite(p, 1, n, f) == n;
    fflush(f);
    return fseek(f, 0, SEEK_END) == 0 && ok;
}

/* A tiny MP4 the probe reads: a picture, then a sound track per language (ISO 639-2), each a few AAC blocks */
static void put_film(const char *rel, const char *const *langs, int n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    FILE *f = file_open(path, "wb");
    Mp4Out out = {f, snd_put, snd_put_at};
    Mp4 *m = mp4_new(&out, 0, 8192);
    static const uint8_t avcc[] = {1, 0x42, 0, 0x1E, 0xFF, 0xE0, 0}, asc[] = {0x11, 0x90};
    Mp4Track v = {.codec = MP4_H264, .timescale = 1000, .config = avcc, .config_len = sizeof avcc, .width = 64, .height = 48};
    mp4_add_track(m, &v);
    for (int i = 0; i < n; i++) {
        Mp4Track a = {.codec = MP4_AAC, .timescale = 48000, .config = asc, .config_len = sizeof asc, .channels = 2,
                      .sample_rate = 48000};
        snprintf(a.lang, sizeof a.lang, "%s", langs[i]);
        mp4_add_track(m, &a);
    }
    mp4_begin(m);
    static const uint8_t frame[16] = {0, 0, 0, 12}, block[8] = {0x21, 0x10};
    mp4_sample(m, 0, frame, sizeof frame, 10000, 0, true);
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < n; i++) mp4_sample(m, 1 + i, block, sizeof block, 1024, 0, true);
    mp4_finish(m);
    mp4_free(m);
    fclose(f);
}

static const MediaFile *find(const char *rel_suffix) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_ends(ix->files[i].rel, rel_suffix)) return &ix->files[i];
    return NULL;
}

TEST(media_a_film_in_several_languages_asks_once_and_plays_yours) {
    th_boot();
    int eva = th_person("Eva"), leo = th_person("Leo");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    static const char *const three[] = {"rus", "ukr", "eng"}, *const one[] = {"swe"};
    put_film("Movies/Idiocracy (2006)/Idiocracy (2006).mp4", three, 3);
    put_film("Movies/Flower (2020)/Flower (2020).mp4", one, 1);
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaFile *f = find("Idiocracy (2006).mp4"), *flower = find("Flower (2020).mp4");
    CHECK(f && flower);
    uint64_t id = f->id;
    CHECK_INT(f->probe.nsounds, 3);
    const MediaSounds *s = media_sounds(id);
    CHECK(s && s->n == 3);
    CHECK_STR(s->s[0].lang, "ru");
    CHECK_STR(s->s[1].lang, "uk");
    CHECK_STR(s->s[2].lang, "en");
    CHECK(s->s[0].on); /* (every track enabled: the first is the default) */
    CHECK(!media_sounds(flower->id)); /* one sound: nothing kept, nothing asked, a plain address */

    char page[64], want[160];
    snprintf(page, sizeof page, "/media/watch/%016llx", (unsigned long long)id);
    const char *html = th_page(eva, page);
    CHECK_HAS(html, "Which language do you want to watch in?");
    CHECK_HAS(html, "data-a=\"media.soundlang\"");
    CHECK_HAS(html, ">Русский</button>");
    CHECK_HAS(html, ">Українська</button>");
    CHECK_HAS(html, ">English</button>");
    CHECK_HAS(html, "As each video comes");
    snprintf(want, sizeof want, "src=\"/media/file/%016llx?a=0\"", (unsigned long long)id);
    CHECK_HAS(html, want); /* (the file's default until then) */
    snprintf(want, sizeof want, "data-static=\"%016llx-a0\"", (unsigned long long)id);
    CHECK_HAS(html, want);
    CHECK_HAS(html, "data-p=\"snd\"");
    CHECK_HAS(html, "data-snd=\"2\"");
    snprintf(want, sizeof want, "%s?asked=1", page);
    CHECK(!strstr(th_page(eva, want), "Which language")); /* closed: not on this page again */
    snprintf(want, sizeof want, "/media/watch/%016llx", (unsigned long long)flower->id);
    html = th_page(eva, want);
    CHECK(!strstr(html, "Which language"));
    CHECK(!strstr(html, "data-snd"));
    snprintf(want, sizeof want, "src=\"/media/file/%016llx\"", (unsigned long long)flower->id);
    CHECK_HAS(html, want);

    /* English it is: no more questions, English plays, the menu says so */
    CHECK(!th_act(eva, "media.soundlang", "lang=english").ok);
    ThResult r = th_act(eva, "media.soundlang", "lang=en");
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Sound: English");
    html = th_page(eva, page);
    CHECK(!strstr(html, "Which language"));
    snprintf(want, sizeof want, "src=\"/media/file/%016llx?a=2\"", (unsigned long long)id);
    CHECK_HAS(html, want);
    CHECK_HAS(html, "data-snd=\"2\" data-src=");
    const char *on = strstr(html, "data-snd=\"2\"");
    CHECK(on && strstr(on, "aria-checked=\"true\">English</button>"));
    CHECK(strstr(th_page(leo, page), "Which language")); /* (Leo hasn't picked: each their own) */

    /* the player's Language menu: Ukrainian for this film, by Eva - her language stays English elsewhere */
    char form[80];
    snprintf(form, sizeof form, "id=%016llx&n=1", (unsigned long long)id);
    th_act(eva, "media.sound", form);
    snprintf(want, sizeof want, "src=\"/media/file/%016llx?a=1\"", (unsigned long long)id);
    CHECK_HAS(th_page(eva, page), want);
    snprintf(form, sizeof form, "id=%016llx&n=9", (unsigned long long)id); /* (no such track: nothing changes) */
    th_act(eva, "media.sound", form);
    CHECK_HAS(th_page(eva, page), want);

    /* a language the film lacks: its own default; "As each video comes" the same */
    CHECK(th_act(leo, "media.soundlang", "lang=sv").ok);
    snprintf(want, sizeof want, "src=\"/media/file/%016llx?a=0\"", (unsigned long long)id);
    CHECK_HAS(th_page(leo, page), want);
    CHECK(th_act(leo, "media.soundlang", "lang=-").ok);
    html = th_page(leo, page);
    CHECK_HAS(html, want);
    CHECK(!strstr(html, "Which language"));
    CHECK(th_act(leo, "media.soundlang", "lang=").ok); /* (Ask me: asked again) */
    CHECK(strstr(th_page(leo, page), "Which language"));

    /* Library settings: the choice, as picked */
    html = th_page(eva, "/media/settings/devices");
    CHECK_HAS(html, "Sound language");
    CHECK_HAS(html, "<option value=\"en\" selected>English</option>");
    /* (what /media/file/<id>?a=2 sends - that sound alone - is mp4view_test.c's, over a real socket) */
    th_halt();
}

/* The cache keeps a video's sound tracks: a restart (from the cache, no scan) still knows them; a cache row from before
   2.2 (no sounds in it) is an MP4 read again at the next scan. */
TEST(media_sound_tracks_survive_a_restart) {
    char dir[512];
    str_copy(dir, sizeof dir, test_tmpdir());
    th_boot_in(dir);
    str_copy(g_root, sizeof g_root, test_tmpdir());
    static const char *const two[] = {"eng", "swe"};
    put_film("Movies/Two Voices (2024)/Two Voices (2024).mp4", two, 2);
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaFile *f = find("Two Voices (2024).mp4");
    CHECK(f && media_sounds(f->id));
    uint64_t id = f->id;
    th_halt();
    th_boot_in(dir); /* (Hemma restarted: the index from the cache) */
    f = media_file(id);
    CHECK(f && f->probe.nsounds == 2);
    const MediaSounds *s = media_sounds(id);
    CHECK(s && s->n == 2);
    CHECK_STR(s->s[1].lang, "sv");
    th_halt();
}

/* Four Russian tracks and an English one (Idiocracy's kind): the menu's pick is that very track, kept for the film -
   not the first Russian one again. */
TEST(media_the_language_menu_keeps_the_very_track_picked) {
    th_boot();
    int eva = th_person("Eva");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    static const char *const five[] = {"rus", "rus", "rus", "eng"};
    put_film("Movies/Idiocracy (2006)/Idiocracy (2006).mp4", five, 4);
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaFile *f = find("Idiocracy (2006).mp4");
    CHECK(f && media_sounds(f->id));
    uint64_t id = f->id;
    CHECK(th_act(eva, "media.soundlang", "lang=ru").ok);
    char page[64], want[96], form[64];
    snprintf(page, sizeof page, "/media/watch/%016llx", (unsigned long long)id);
    snprintf(want, sizeof want, "src=\"/media/file/%016llx?a=0\"", (unsigned long long)id);
    CHECK_HAS(th_page(eva, page), want); /* (Russian: the first, the default) */
    const char *html = th_page(eva, page);
    CHECK_HAS(html, ">Русский</button>");
    CHECK_HAS(html, ">Русский 2</button>");
    CHECK_HAS(html, ">Русский 3</button>");
    snprintf(form, sizeof form, "id=%016llx&n=2", (unsigned long long)id);
    th_act(eva, "media.sound", form);
    snprintf(want, sizeof want, "data-static=\"%016llx-a2\"", (unsigned long long)id);
    CHECK_HAS(th_page(eva, page), want);
    th_halt();
}
