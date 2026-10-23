/* Watch in Swedish and Spanish (issue #46): every page of the library, its dialogs, the TV-mode pages, the
   home card and the rows for screens - with a library that has a bit of everything (films, a series, a ripped
   DVD season with its play-all and bonus clips, home videos, subtitles, a left-out trailer, an unfinished
   upload, watch history whose video is gone, a folder not for children). */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "media.h"
#include "room.h"

static void put(const char *root, const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

static void put_text(const char *root, const char *rel, const char *text) { put(root, rel, text, strlen(text)); }

static size_t ebml(uint8_t *out, uint32_t id, const void *data, size_t len) {
    size_t n = 0;
    for (int s = 24; s >= 0; s -= 8)
        if (id >> s) out[n++] = (uint8_t)(id >> s);
    out[n++] = (uint8_t)(0x80 | len); /* every element here is < 127 bytes */
    memcpy(out + n, data, len);
    return n + len;
}

/* A tiny Matroska file the probe reads a length from: 720x480 MPEG-2 (a DVD rip) */
static void put_mkv(const char *root, const char *rel, double minutes) {
    uint8_t a[64], b[64], info[64], tr[64], te[64], seg[192], out[256];
    size_t n;
    double ms = minutes * 60000.0;
    uint64_t bits;
    memcpy(&bits, &ms, 8);
    uint8_t be[8];
    for (int i = 0; i < 8; i++) be[i] = (uint8_t)(bits >> (56 - 8 * i));
    n = ebml(a, 0x2AD7B1, "\x0F\x42\x40", 3);
    n += ebml(a + n, 0x4489, be, 8);
    size_t ni = ebml(info, 0x1549A966, a, n);
    n = ebml(b, 0xB0, "\x02\xD0", 2);
    n += ebml(b + n, 0xBA, "\x01\xE0", 2);
    size_t nv = ebml(tr, 0xE0, b, n);
    n = ebml(a, 0x83, "\x01", 1);
    n += ebml(a + n, 0x86, "V_MPEG2", 7);
    memcpy(a + n, tr, nv);
    size_t nt = ebml(te, 0xAE, a, n + nv);
    size_t ntr = ebml(tr, 0x1654AE6B, te, nt);
    memcpy(seg, info, ni);
    memcpy(seg + ni, tr, ntr);
    n = ebml(out, 0x1A45DFA3, "\x42\x82\x88matroska", 11);
    n += ebml(out + n, 0x18538067, seg, ni + ntr);
    put(root, rel, out, n);
}

static const MediaFile *find(const char *rel_suffix) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_ends(ix->files[i].rel, rel_suffix)) return &ix->files[i];
    return NULL;
}

static const MediaSeries *series_named(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nseries; i++)
        if (str_eq(ix->series[i].title, title)) return &ix->series[i];
    return NULL;
}

static int admin(const char *name, const char *user) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    str_copy(p.username, sizeof p.username, user);
    str_copy(p.pass, sizeof p.pass, "hash");
    tbl_put(&T_person, &p);
    return id;
}

static int member(const char *name, const char *user) {
    int id = th_person(name);
    Person p = *person_get(id);
    str_copy(p.username, sizeof p.username, user);
    str_copy(p.pass, sizeof p.pass, "hash");
    tbl_put(&T_person, &p);
    return id;
}

static char g_urls[40][200];
static int g_nurls;

static void add_url(const char *fmt, uint64_t id) {
    char hex[17];
    media_id_hex(id, hex);
    snprintf(g_urls[g_nurls++], sizeof g_urls[0], fmt, hex);
}

/* Data, not words: people's names (and their avatars' initials), films', series' and folders' names, the
   library folder - and what reads the same in the other language: "Series", "series", "Extras" and "Extra" in
   Spanish, "Video"/"video" and the System section in Swedish, "A–Z" in Spanish, the subtitle languages' names
   (each in its own). */
static const char *g_allow[40] = {"Anna", "Eva", "A", "E", "Garden Show", "Garden", "Show", "Seeds", "Roots",
                                  "Inception", "Flower", "Midsommar", "IMG", "70's Show", "70's show", "Kino", "Shows",
                                  "Klipp", "Series", "series", "Extras", "Extra", "Video", "video", "A–Z", "Svenska",
                                  "English", "Norsk", "Dansk", "Suomi", "Deutsch", "Français", "Español", "System", NULL};

static void allow_also(const char *s) {
    int n = 0;
    while (g_allow[n]) n++;
    if (n + 1 < countof(g_allow)) g_allow[n] = s, g_allow[n + 1] = NULL;
}

static int untranslated(int person, Buf *report) {
    int n = 0;
    for (int i = 0; i < g_nurls; i++) n += th_untranslated(person, g_urls[i], g_allow, report);
    return n;
}

TEST(media_speaks_swedish_and_spanish) {
    th_boot();
    int anna = admin("Anna", "anna"), eva = member("Eva", "eva");
    static char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    allow_also(root); /* (the library folder, in Library settings) */
    put_text(root, "Kino/Inception (2010)/Inception.2010.1080p.BluRay.x264.mkv", "not really a movie");
    put_mkv(root, "Kino/Inception (2010)/Inception-trailer.mkv", 2); /* left out: named like one, and short */
    put_text(root, "Kino/Flower (2020)/Flower.mp4", "m2");
    put_text(root, "Kino/Flower (2020)/Flower.sv.srt", "1\n00:00:01,000 --> 00:00:02,000\nHej\n");
    put_text(root, "Kino/Garden (2019)/Garden (2019).mp4", "gone soon");
    put_text(root, "Shows/Garden Show/Season 1/Garden.Show.S01E01.Seeds.mp4", "e1");
    put_text(root, "Shows/Garden Show/Season 1/Garden.Show.S01E02.Roots.mp4", "e2");
    put_text(root, "Shows/Garden Show/Season 2/Garden.Show.S02E01.mkv", "e3");
    put_text(root, "Klipp/Midsommar 2024/IMG_1234.mov", "home");
    put_text(root, "Kino/Unfinished (2021)/Unfinished (2021).mkv.hemma-part", "half a film");
    /* a DVD season: six episodes, the disc's play-all and three bonus clips */
    static const struct {
        const char *name;
        double min;
    } disc[] = {{"B1_t01.mkv", 22.6}, {"B2_t02.mkv", 22.6}, {"B3_t03.mkv", 22.6}, {"B4_t04.mkv", 22.6},
                {"B5_t05.mkv", 22.6}, {"B6_t06.mkv", 22.6}, {"B7_t00.mkv", 135.5}, {"C1_t07.mkv", 18.1},
                {"C2_t08.mkv", 3.8},  {"C3_t09.mkv", 2.4}};
    for (int i = 0; i < countof(disc); i++) {
        char rel[200];
        snprintf(rel, sizeof rel, "Shows/70's show/season 1 70's show/Disc - 4/%s", disc[i].name);
        put_mkv(root, rel, disc[i].min);
    }
    setting_set("media.roots", root);
    media_scan_sync();

    const MediaFile *flower = find("Flower.mp4"), *inception = find("Inception.2010.1080p.BluRay.x264.mkv");
    const MediaFile *ep1 = find("S01E01.Seeds.mp4"), *garden = find("Garden (2019).mp4");
    const MediaSeries *show = series_named("Garden Show"), *rips = series_named("70's Show");
    CHECK(flower && inception && ep1 && garden && show && rips);
    if (!flower || !inception || !ep1 || !garden || !show || !rips) {
        th_halt();
        return;
    }
    const MediaIndex *ix = media_lib();
    const MediaFile *playall = NULL, *bonus = NULL;
    for (int i = 0; i < ix->nextras; i++) {
        const MediaFile *x = &ix->files[ix->extras[i]];
        if (x->role == MF_PLAYALL && !playall) playall = x;
        if (x->role == MF_BONUS && !bonus) bonus = x;
    }
    CHECK(playall && bonus);

    /* someone's watching: Continue watching, ticks, watch history whose video goes */
    char form[200];
    snprintf(form, sizeof form, "id=%016llx&pos=120&dur=1200", (unsigned long long)flower->id);
    CHECK(th_act(anna, "media.progress", form).ok);
    snprintf(form, sizeof form, "id=%016llx&done=1", (unsigned long long)ep1->id);
    CHECK(th_act(anna, "media.watched", form).ok);
    snprintf(form, sizeof form, "id=%016llx&pos=300&dur=1200", (unsigned long long)garden->id);
    CHECK(th_act(anna, "media.progress", form).ok);
    if (bonus) {
        snprintf(form, sizeof form, "id=%016llx&pos=60&dur=200", (unsigned long long)bonus->id);
        CHECK(th_act(anna, "media.progress", form).ok);
    }
    char path[800];
    snprintf(path, sizeof path, "%s/Kino/Garden (2019)/Garden (2019).mp4", root);
    CHECK(file_remove(path));
    CHECK(th_act(anna, "media.kids", "p=0:Klipp&on=1").ok);
    g_fake_now = now_sec() + 8 * 86400; /* a week later: the unfinished upload is listed */
    media_scan_sync();
    ix = media_lib();
    CHECK_INT(ix->nparts_all, 1);
    CHECK(ix->nleft_all >= 1);
    flower = find("Flower.mp4"), inception = find("Inception.2010.1080p.BluRay.x264.mkv"), ep1 = find("S01E01.Seeds.mp4");
    show = series_named("Garden Show"), rips = series_named("70's Show");
    playall = bonus = NULL;
    for (int i = 0; i < ix->nextras; i++) {
        const MediaFile *x = &ix->files[ix->extras[i]];
        if (x->role == MF_PLAYALL && !playall) playall = x;
        if (x->role == MF_BONUS && !bonus) bonus = x;
    }

    g_nurls = 0;
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media?q=zzzz");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/movies");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/movies?sort=new");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/series");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/browse");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/browse?p=0:Kino");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/browse?p=0:Klipp");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/browse?p=0:Klipp/Midsommar%202024");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/settings");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/nope");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/watch/0000000000000001");
    str_copy(g_urls[g_nurls++], sizeof g_urls[0], "/media/series/00000000000000aa");
    add_url("/media/series/%s", show->id);
    add_url("/media/series/%s?pic=1", show->id);
    add_url("/media/series/%s?s=1", rips->id);
    add_url("/media/series/%s?tv=1&back=%%2F", show->id);
    add_url("/media/watch/%s", flower->id);
    add_url("/media/watch/%s?pic=1", flower->id);
    add_url("/media/watch/%s?remove=1", flower->id);
    add_url("/media/watch/%s", inception->id);
    add_url("/media/watch/%s", ep1->id);
    add_url("/media/watch/%s?tv=1&back=%%2F", ep1->id);
    add_url("/media/watch/%s?tv=1&back=%%2F", flower->id);
    if (playall) add_url("/media/watch/%s", playall->id);
    if (bonus) add_url("/media/watch/%s", bonus->id);
    if (find("B1_t01.mkv")) add_url("/media/watch/%s", find("B1_t01.mkv")->id); /* (an MPEG-2 rip: needs converting) */

    Buf r = {0};
    int n = untranslated(anna, &r); /* an admin: Library settings' every card, Remove, Picture */
    str_copy(g_urls[0], sizeof g_urls[0], "/media/settings"); /* a member: who to ask */
    g_nurls = 1;
    add_url("/media/watch/%s", inception->id);
    n += untranslated(eva, &r);
    if (n) printf("%s", buf_cstr(&r));
    CHECK_INT(n, 0);
    buf_free(&r);

    /* the disc's extras in the reader's language, kept in English as what the video is */
    if (playall) {
        char t[300];
        pref_set(anna, PREF_LANG, "sv");
        Ctx c;
        app_ctx(&c, anna, "/media");
        media_title(playall, t, sizeof t);
        CHECK_STR(t, "70's Show – säsong 1, skiva 4: alla avsnitt");
        app_ctx_free(&c);
        pref_set(anna, PREF_LANG, "");
        CHECK_STR(playall->name.title, "70's Show – season 1, disc 4: all episodes");
    }

    /* the home card and the rows a TV screen shows */
    const Module *m = module_find("media");
    static const char *const langs[] = {"sv", "es"};
    static const char *const want[][3] = {{"Fortsätt titta", "nya avsnitt", "Härnäst"},
                                          {"Seguir viendo", "episodios nuevos", "Siguiente"}};
    for (int l = 0; l < 2; l++) {
        pref_set(anna, PREF_LANG, langs[l]);
        Ctx c;
        Buf b = {0};
        app_ctx(&c, anna, "/");
        m->widget(&c, &b, WIDGET_HOME);
        CHECK_HAS(buf_cstr(&b), want[l][0]);
        CHECK(!strstr(buf_cstr(&b), "Continue watching"));
        app_ctx_free(&c);
        char hex[17];
        media_id_hex(show->id, hex);
        static const char *const rows[] = {"media.continue", "media.recent", "media.series", "media.movies",
                                           "media.series_one"};
        for (int k = 0; k < countof(rows); k++) {
            const Block *bl = block_find(rows[k], NULL);
            CHECK(bl != NULL);
            if (!bl) continue;
            app_ctx(&c, anna, "/screens/1");
            bl->render(&c, &b, SCREEN_TV, hex);
            app_ctx_free(&c);
        }
        const char *h = buf_cstr(&b);
        CHECK_HAS(h, want[l][1]);
        CHECK_HAS(h, want[l][2]);
        CHECK(!strstr(h, " episodes") && !strstr(h, "Next up") && !strstr(h, " left<") && !strstr(h, "Watched"));
        buf_reset(&b);
        app_ctx(&c, anna, "/media");
        media_lib_head(&c, &b, "home", "x", NULL, NULL);
        app_ctx_free(&c);
        CHECK(!strstr(buf_cstr(&b), ">Movies<") && !strstr(buf_cstr(&b), "Library settings"));
        buf_free(&b);
    }
    pref_set(anna, PREF_LANG, "");
    g_fake_now = 0;
    th_halt();
}

TEST(media_speaks_swedish_and_spanish_before_there_is_a_library) {
    th_boot();
    int anna = admin("Anna", "anna"), eva = member("Eva", "eva");
    setting_set("media.roots", ""); /* (no folders - whatever an earlier test in this process left) */
    for (int i = tbl_count(&T_media_watch) - 1; i >= 0; i--) tbl_del(&T_media_watch, ((MediaWatch *)tbl_at(&T_media_watch, i))->id);
    media_scan_sync();
    static const char *const allow[] = {"Anna", "Eva", "A", "E", "Series", "series", "Svenska", "English", "Norsk",
                                        "Dansk", "Suomi", "Deutsch", "Français", "Español", "System", NULL};
    Buf r = {0};
    int n = th_untranslated(anna, "/media", allow, &r) + th_untranslated(eva, "/media", allow, &r) +
            th_untranslated(eva, "/media/settings", allow, &r) + th_untranslated(anna, "/media/movies", allow, &r);
    /* the disks this machine has, offered with one tap (issue #27) */
    char disk[600];
    snprintf(disk, sizeof disk, "%s/usb", test_tmpdir());
    dir_make(disk);
    media_test_mounts(disk);
    n += th_untranslated(anna, "/media", allow, &r);
    n += th_untranslated(anna, "/media/settings", allow, &r);
    if (n) printf("%s", buf_cstr(&r));
    CHECK_INT(n, 0);
    buf_free(&r);
    th_halt();
}

TEST(media_toasts_and_checks_in_the_actors_language) {
    th_boot();
    int anna = admin("Anna", "anna");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_text(root, "Kino/Flower (2020)/Flower.mp4", "m2");
    setting_set("media.roots", root);
    media_scan_sync();
    pref_set(anna, PREF_LANG, "sv");
    ThResult t = th_act(anna, "media.rescan", NULL);
    CHECK_STR(t.toast, "Biblioteket genomsökt");
    char form[400];
    snprintf(form, sizeof form, "roots=%s", root);
    t = th_act(anna, "media.roots", form);
    CHECK_HAS(t.toast, "Sparat — söker igenom 1 mapp");
    t = th_act(anna, "media.roots", "roots=/no/such/folder/here");
    CHECK_HAS(t.toast, "Hittar inte mappen");
    pref_set(anna, PREF_LANG, "es");
    t = th_act(anna, "media.prefs", "sublang=");
    CHECK_STR(t.toast, "Subtítulos desactivados por defecto");
    /* the System page's words: the library's checks in Spanish */
    if (module_find("system")) CHECK_HAS(th_page(anna, "/system"), "Películas y series");
    /* sizes with a decimal comma */
    char s[24];
    i18n_use(LANG_SV);
    media_size_text(1288490189LL, s, sizeof s);
    CHECK_STR(s, "1,2 GB");
    i18n_use(LANG_EN);
    media_size_text(1288490189LL, s, sizeof s);
    CHECK_STR(s, "1.2 GB");
    /* a probe's verdict, kept in English, said in the reader's language */
    MediaProbe p = {0};
    str_copy(p.container, sizeof p.container, "mkv");
    str_copy(p.vcodec, sizeof p.vcodec, "mpeg2");
    media_probe_judge(&p);
    CHECK_STR(p.why, "MPEG-2 video needs conversion");
    char why[200];
    i18n_use(LANG_SV);
    media_probe_why(&p, why, sizeof why);
    CHECK_STR(why, "MPEG-2-video behöver konverteras");
    str_copy(p.vcodec, sizeof p.vcodec, "h264");
    str_copy(p.acodec, sizeof p.acodec, "eac3");
    media_probe_judge(&p);
    media_probe_why(&p, why, sizeof why);
    CHECK_STR(why, "Dolby-ljud: bara Safari/Edge - tyst i Chrome och Firefox");
    i18n_use(LANG_EN);
    pref_set(anna, PREF_LANG, "");
    th_halt();
}

/* Two subtitles in one language (English and English SDH, two plain English ones, a forced one): the
   player's choices read in the reader's words - the language's own name and the rest translated. */
TEST(media_subtitle_choices_speak_swedish_and_spanish) {
    th_boot();
    int anna = admin("Anna", "anna");
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_text(root, "Kino/Nightmare (1993)/Nightmare (1993).mp4", "film");
    static const char *const subs[] = {"en", "en.2", "en.sdh", "en.forced", "en.sdh.forced", "sv", "und.2"};
    for (int i = 0; i < countof(subs); i++) {
        char rel[200];
        snprintf(rel, sizeof rel, "Kino/Nightmare (1993)/Nightmare (1993).%s.srt", subs[i]);
        put_text(root, rel, "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
    }
    setting_set("media.roots", root);
    media_scan_sync();
    const MediaFile *f = find("Nightmare (1993).mp4");
    CHECK(f);
    if (!f) {
        th_halt();
        return;
    }
    g_nurls = 0;
    add_url("/media/watch/%s", f->id);
    add_url("/media/watch/%s?tv=1&back=%%2F", f->id);
    allow_also("Nightmare");
    Buf r = {0};
    int n = untranslated(anna, &r);
    if (n) printf("%s", buf_cstr(&r));
    CHECK_INT(n, 0);
    buf_free(&r);
    th_halt();
}
