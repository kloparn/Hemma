/* Managing the library (manage.c, remove.c's media_purge_*): removing an episode, a season and a series,
   fixing a film's title and an episode's number on the disk, what takes room - admins only, in three
   languages. (Paths stay short: the Windows suite runs in long scratch folders.) */
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "media.h"
#include "remove.h"

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
static bool make_link(const char *target, const char *at) { return symlink(target, at) == 0; }
#else
static bool make_link(const char *target, const char *at) { /* (Windows wants a privilege for links: not checked there) */
    (void)target;
    (void)at;
    return false;
}
#endif

static char g_root[512];

static void put_data(const char *rel, const void *data, size_t n) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, data, n);
}

static void put(const char *rel, const char *text) { put_data(rel, text, strlen(text)); }

static bool there(const char *rel) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    return path_stat(path, NULL, NULL, NULL);
}

static const MediaFile *find(const char *rel) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nfiles; i++)
        if (str_eq(ix->files[i].rel, rel)) return &ix->files[i];
    return NULL;
}

static const MediaSeries *series_named(const char *title) {
    const MediaIndex *ix = media_lib();
    for (int i = 0; i < ix->nseries; i++)
        if (str_eq(ix->series[i].title, title)) return &ix->series[i];
    return NULL;
}

static const char *url(const char *fmt, uint64_t id) {
    static char u[300];
    char hex[17];
    media_id_hex(id, hex);
    snprintf(u, sizeof u, fmt, hex, hex);
    return u;
}

static const char *form(const char *fmt, uint64_t id) {
    static char f[300];
    char hex[17];
    media_id_hex(id, hex);
    snprintf(f, sizeof f, fmt, hex);
    return f;
}

static int with_role(const char *name, const char *user, int role) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = role;
    str_copy(p.username, sizeof p.username, user);
    str_copy(p.pass, sizeof p.pass, "hash");
    tbl_put(&T_person, &p);
    return id;
}

static const char *SRT = "1\n00:00:01,000 --> 00:00:02,000\nHej\n";

static const char *last_activity(void) {
    return tbl_count(&T_activity) ? ((const Activity *)tbl_at(&T_activity, tbl_count(&T_activity) - 1))->text : "";
}

static void boot(int *adam, int *eva) {
    th_boot();
    *adam = with_role("Adam", "adam", ROLE_ADMIN);
    *eva = with_role("Eva", "eva", ROLE_MEMBER);
    str_copy(g_root, sizeof g_root, test_tmpdir());
}

TEST(manage_removes_episodes_seasons_and_series) {
    int adam, eva;
    boot(&adam, &eva);
    put("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4", "e1");
    put("Series/Garden/Season 1/Garden.S01E01.Seeds.web.mp4", "e1 copy");
    put("Series/Garden/Season 1/Garden.S01E01.Seeds.sv.srt", SRT);
    put("Series/Garden/Season 1/Garden.S01E01.Seeds.jpg", "picture");
    put("Series/Garden/Season 1/Garden.S01E02.Roots.mp4", "e2");
    put("Series/Garden/Season 2/Garden.S02E01.mkv", "e3");
    put("Series/Garden/Season 2/notes.nfo", "notes");
    put("Series/Garden/poster.jpg", "poster");
    put("Series/Garden/Extras/Behind.mkv", "an extra: left out");
    put("Series/Garden/Some Film (2001)/Some Film (2001).mkv", "a film, not the show's");
    put("Series/Orbit/Season 1/Orbit.S01E01.mkv", "o1");
    put("Series/Orbit/Season 1/Orbit.S01E02.mkv", "o2");
    put("Series/Orbit/folder.jpg", "art");
    put("TV/Lonely.S01E01.mkv", "l1");
    put("Solo/Season 1/Solo.S01E01.mkv", "s1");
    put("Movies/Flower (2020)/Flower.mp4", "film");
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaFile *e1 = find("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4");
    const MediaSeries *garden = series_named("Garden"), *orbit = series_named("Orbit");
    CHECK(e1 && garden && orbit && series_named("Lonely") && series_named("Solo"));
    if (!e1 || !garden || !orbit) return;
    uint64_t e1_id = e1->id, garden_id = garden->id, orbit_id = orbit->id;
    CHECK(e1->web != 0);

    /* the Manage view is the admins': members get the ordinary page, and no removing */
    CHECK_HAS(th_page(adam, url("/media/series/%s", garden_id)), "?manage=1");
    CHECK(!strstr(th_page(eva, url("/media/series/%s", garden_id)), "manage=1"));
    const char *page = th_page(eva, url("/media/series/%s?manage=1", garden_id));
    CHECK(!strstr(page, "Fix number") && !strstr(page, "media.manage_remove") && !strstr(page, "rm=season"));
    CHECK(!th_act(eva, "media.manage_remove", form("what=episode&id=%s", e1_id)).ok);
    CHECK(!th_act(eva, "media.manage_remove", form("what=series&id=%s", garden_id)).ok);
    CHECK(there("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4"));

    /* the admin's view: per episode its files, versions and size; per season and series what removing takes */
    page = th_page(adam, url("/media/series/%s?manage=1&s=1", garden_id));
    CHECK_HAS(page, "<code class=\"media-code\">Garden.S01E01.Seeds.web.mp4</code> <span class=\"badge");
    CHECK_HAS(page, ">Original</span>");
    CHECK_HAS(page, ">Converted copy</span>");
    CHECK_HAS(page, "Fix number<span class=\"sr-only\"> Garden S01E01</span>");
    CHECK_HAS(page, "rm=season\">");
    CHECK_HAS(page, "Remove season 1 (");
    CHECK_HAS(page, "Remove the series (");
    char u[300];
    snprintf(u, sizeof u, "/media/series/%016llx?manage=1&s=1&rm=%016llx", (unsigned long long)garden_id, (unsigned long long)e1_id);
    const char *dlg = th_page(adam, u);
    CHECK_HAS(dlg, "Remove the episode");
    CHECK_HAS(dlg, "<b>Garden S01E01</b> is deleted from the disk");
    CHECK_HAS(dlg, "4 files, ");
    CHECK_HAS(dlg, "<code class=\"media-code\">Season 1/Garden.S01E01.Seeds.sv.srt</code>");
    CHECK_HAS(dlg, "<code class=\"media-code\">Season 1/Garden.S01E01.Seeds.jpg</code>");
    CHECK_HAS(dlg, "data-a=\"media.manage_remove\"");

    /* an episode: its video, copy, subtitles and own picture - and everyone's progress in it */
    media_watch_report(eva, e1_id, 300, 1200, false, now_ms());
    ThResult r = th_act(adam, "media.manage_remove", form("what=episode&id=%s", e1_id));
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Removed Garden S01E01 - ");
    CHECK_HAS(r.redirect, "?manage=1&s=1");
    CHECK(!there("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4"));
    CHECK(!there("Series/Garden/Season 1/Garden.S01E01.Seeds.web.mp4"));
    CHECK(!there("Series/Garden/Season 1/Garden.S01E01.Seeds.sv.srt"));
    CHECK(!there("Series/Garden/Season 1/Garden.S01E01.Seeds.jpg"));
    CHECK(there("Series/Garden/Season 1/Garden.S01E02.Roots.mp4"));
    CHECK(there("Series/Garden/poster.jpg"));
    CHECK(!media_watch_get(eva, e1_id));
    CHECK(!media_file(e1_id)); /* (rescanned) */
    CHECK_HAS(last_activity(), "removed Garden S01E01 from the library");

    /* a season: its folder goes whole (notes and all) when nothing else is in it */
    snprintf(u, sizeof u, "/media/series/%016llx?manage=1&s=2&rm=season", (unsigned long long)garden_id);
    dlg = th_page(adam, u);
    CHECK_HAS(dlg, "Season 2 of <b>Garden</b> is deleted from the disk");
    CHECK_HAS(dlg, "Its folder <code class=\"media-code\">Series/Garden/Season 2</code> goes too");
    snprintf(u, sizeof u, "what=season&s=2&id=%016llx", (unsigned long long)garden_id);
    r = th_act(adam, "media.manage_remove", u);
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Removed Garden S02");
    CHECK(!there("Series/Garden/Season 2"));
    CHECK(there("Series/Garden/Season 1/Garden.S01E02.Roots.mp4"));

    /* a series: a film in its folder (in a folder of its own: a loose one would be a special - 1.3) keeps the folder (and what's left in it); the episodes, their emptied
       folders and the series' picture go */
    snprintf(u, sizeof u, "/media/series/%016llx?manage=1&rm=series", (unsigned long long)garden_id);
    CHECK_HAS(th_page(adam, u), "Remove the series");
    r = th_act(adam, "media.manage_remove", form("what=series&id=%s", garden_id));
    CHECK(r.ok);
    CHECK(!there("Series/Garden/Season 1"));
    CHECK(!there("Series/Garden/poster.jpg"));
    CHECK(there("Series/Garden/Some Film (2001)/Some Film (2001).mkv"));
    CHECK(there("Series/Garden/Extras/Behind.mkv"));
    CHECK(!series_named("Garden"));
    CHECK_STR(r.redirect, "/media/series");

    /* a show folder of its own goes whole - never the "Series" folder above it, though */
    char outside[700], target[800], link[1100];
    snprintf(outside, sizeof outside, "%s/keep", test_tmpdir());
    dir_make(outside);
    snprintf(target, sizeof target, "%s/precious.txt", outside);
    file_write_atomic(target, "keep me", 7);
    snprintf(link, sizeof link, "%s/Series/Orbit/elsewhere", g_root);
    bool linked = make_link(outside, link);
    CHECK(th_act(adam, "media.manage_remove", form("what=series&id=%s", orbit_id)).ok);
    CHECK(!there("Series/Orbit/Season 1"));
    CHECK(path_stat(target, NULL, NULL, NULL)); /* a link inside: never followed */
    if (linked) CHECK(there("Series/Orbit")); /* (and the folder holding it stays) */
    else CHECK(!there("Series/Orbit"));
    CHECK(there("Series"));

    /* right in a "TV" folder: the episode goes, the folder stays even when empty; a show's own folder right in
       the library folder goes as the film's own folder does */
    const MediaSeries *lonely = series_named("Lonely"), *solo = series_named("Solo");
    CHECK(lonely && solo);
    if (!lonely || !solo) return;
    uint64_t solo_id = solo->id;
    CHECK(th_act(adam, "media.manage_remove", form("what=series&id=%s", lonely->id)).ok);
    CHECK(!there("TV/Lonely.S01E01.mkv"));
    CHECK(there("TV"));
    CHECK(th_act(adam, "media.manage_remove", form("what=series&id=%s", solo_id)).ok);
    CHECK(!there("Solo"));
    CHECK(there("Movies/Flower (2020)/Flower.mp4"));
    CHECK(path_is_dir(g_root));

    /* gone already: says so */
    CHECK(!th_act(adam, "media.manage_remove", form("what=series&id=%s", solo_id)).ok);
    th_halt();
}

TEST(manage_fixes_a_films_title_and_year) {
    int adam, eva;
    boot(&adam, &eva);
    put("Movies/Hocus Pokus 1993/Hocus.Pokus.1993.mkv", "film");
    put("Movies/Hocus Pokus 1993/Hocus.Pokus.1993.web.mp4", "copy");
    put("Movies/Hocus Pokus 1993/Hocus.Pokus.1993.en.srt", SRT);
    put("Movies/Hocus Pokus 1993/Hocus.Pokus.1993.jpg", "picture");
    put("Movies/Hocus Pokus 1993/release.nfo", "nfo");
    put("Movies/Dune (2021).mkv", "dune");
    put("Movies/Dune 2021 1080p.mkv", "dune again");
    put("Kino/Flower (2020)/Flower.mp4", "flower");
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaFile *hp = find("Movies/Hocus Pokus 1993/Hocus.Pokus.1993.mkv"), *dune = find("Movies/Dune 2021 1080p.mkv");
    const MediaFile *flower = find("Kino/Flower (2020)/Flower.mp4");
    CHECK(hp && dune && flower);
    if (!hp || !dune || !flower) return;
    CHECK_STR(hp->name.title, "Hocus Pokus");
    uint64_t hp_id = hp->id, dune_id = dune->id, flower_id = flower->id;
    CHECK(th_act(adam, "media.kids", "p=0:Movies/Hocus%20Pokus%201993&on=1").ok);
    CHECK(media_kids_marked(g_root, "Movies/Hocus Pokus 1993"));
    media_watch_report(eva, hp_id, 300, 1200, false, now_ms());

    /* the button and the dialog are the admins' */
    CHECK_HAS(th_page(adam, url("/media/watch/%s", hp_id)), "?fix=1");
    CHECK(!strstr(th_page(eva, url("/media/watch/%s", hp_id)), "?fix=1"));
    const char *dlg = th_page(adam, url("/media/watch/%s?fix=1", hp_id));
    CHECK_HAS(dlg, "Fix the title");
    CHECK_HAS(dlg, "data-a=\"media.fix_title\"");
    CHECK_HAS(dlg, "name=\"title\" maxlength=\"120\" required value=\"Hocus Pokus\"");
    CHECK(!strstr(th_page(eva, url("/media/watch/%s?fix=1", hp_id)), "media.fix_title"));
    CHECK(!th_act(eva, "media.fix_title", form("id=%s&title=Hocus%%20Pocus&year=1993", hp_id)).ok);

    /* names a disk can't store, years that aren't, a name Hemma would read otherwise */
    ThResult r = th_act(adam, "media.fix_title", form("id=%s&title=&year=1993", hp_id));
    CHECK(!r.ok);
    CHECK_STR(r.field, "title");
    r = th_act(adam, "media.fix_title", form("id=%s&title=Bad%%0AName&year=1993", hp_id));
    CHECK(!r.ok);
    CHECK_STR(r.field, "title");
    r = th_act(adam, "media.fix_title", form("id=%s&title=CON&year=", hp_id));
    CHECK(!r.ok);
    r = th_act(adam, "media.fix_title", form("id=%s&title=Hocus%%20Pocus&year=19x3", hp_id));
    CHECK(!r.ok);
    CHECK_STR(r.field, "year");
    r = th_act(adam, "media.fix_title", form("id=%s&title=Hocus%%20Pocus&year=3000", hp_id));
    CHECK_STR(r.field, "year");
    r = th_act(adam, "media.fix_title", form("id=%s&title=Hocus%%20Pocus%%201080p&year=1993", hp_id));
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "Hemma would read that name as \"Hocus Pocus (1993)\"");
    CHECK(there("Movies/Hocus Pokus 1993/Hocus.Pokus.1993.mkv"));

    /* the fix: the video, its copy, subtitles and picture, and its own folder */
    r = th_act(adam, "media.fix_title", form("id=%s&title=Hocus%%20Pocus&year=1993", hp_id));
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Renamed it Hocus Pocus (1993)");
    CHECK(there("Movies/Hocus Pocus (1993)/Hocus Pocus (1993).mkv"));
    CHECK(there("Movies/Hocus Pocus (1993)/Hocus Pocus (1993).web.mp4"));
    CHECK(there("Movies/Hocus Pocus (1993)/Hocus Pocus (1993).en.srt"));
    CHECK(there("Movies/Hocus Pocus (1993)/Hocus Pocus (1993).jpg"));
    CHECK(there("Movies/Hocus Pocus (1993)/release.nfo"));
    CHECK(!there("Movies/Hocus Pokus 1993"));
    uint64_t new_id = media_make_id(g_root, "Movies/Hocus Pocus (1993)/Hocus Pocus (1993).mkv");
    char want[80];
    snprintf(want, sizeof want, "/media/watch/%016llx", (unsigned long long)new_id);
    CHECK_STR(r.redirect, want);
    const MediaFile *now = media_file(new_id); /* (rescanned) */
    CHECK(now != NULL);
    if (now) {
        CHECK_STR(now->name.title, "Hocus Pocus");
        CHECK_INT(now->name.year, 1993);
        CHECK(now->web != 0 && now->poster != 0);
    }
    /* everyone's progress went along, and so did the "Not for children" mark */
    const MediaWatch *w = media_watch_get(eva, new_id);
    CHECK(w && w->pos == 300);
    CHECK(!media_watch_get(eva, hp_id));
    CHECK(media_kids_marked(g_root, "Movies/Hocus Pocus (1993)"));
    CHECK(!media_kids_marked(g_root, "Movies/Hocus Pokus 1993"));
    CHECK_HAS(last_activity(), "renamed Hocus Pokus (1993) to Hocus Pocus (1993)");
    r = th_act(adam, "media.fix_title", form("id=%s&title=Hocus%%20Pocus&year=1993", new_id));
    CHECK_HAS(r.toast, "That's its name already");

    /* never over a file that's there */
    r = th_act(adam, "media.fix_title", form("id=%s&title=Dune&year=2021", dune_id));
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "There's already a file called Dune (2021).mkv there");
    CHECK(there("Movies/Dune 2021 1080p.mkv"));
    CHECK(there("Movies/Dune (2021).mkv"));

    /* a slash or two in a title stay in its folder */
    CHECK(th_act(adam, "media.fix_title", form("id=%s&title=..%%2F..%%2FDune%%20Two&year=2024", dune_id)).ok);
    CHECK(!there("Movies/Dune 2021 1080p.mkv"));
    CHECK(there("Movies/-..-Dune Two (2024).mkv") || there("Movies/..-..-Dune Two (2024).mkv") ||
          there("Movies/-Dune Two (2024).mkv"));

    /* a folder by that name is there already; a change of case only (one file, on a disk that ignores case) */
    put("Movies/Blob 1958/Blob.1958.mkv", "blob");
    put("Movies/The Blob (1958)/keep.txt", "someone's");
    put("Movies/flower power (1969).mkv", "power");
    media_scan_sync();
    const MediaFile *blob = find("Movies/Blob 1958/Blob.1958.mkv"), *power = find("Movies/flower power (1969).mkv");
    CHECK(blob && power);
    if (!blob || !power) return;
    uint64_t power_id = power->id;
    r = th_act(adam, "media.fix_title", form("id=%s&title=The%%20Blob&year=1958", blob->id));
    CHECK_HAS(r.toast, "There's already a folder called The Blob (1958) there");
    CHECK(there("Movies/Blob 1958/Blob.1958.mkv"));
    r = th_act(adam, "media.fix_title", form("id=%s&title=Flower%%20Power&year=1969", power_id));
    CHECK(r.ok);
    char movies[700];
    snprintf(movies, sizeof movies, "%s/Movies", g_root);
    DirIter *it = dir_open(movies);
    bool cased = false, old = false;
    DirEntry de;
    while (it && dir_next(it, &de)) cased |= str_eq(de.name, "Flower Power (1969).mkv"), old |= str_eq(de.name, "flower power (1969).mkv");
    if (it) dir_close(it);
    CHECK(cased && !old);

    /* without a year, in a folder that doesn't say "movies", a film is just a video: refused */
    r = th_act(adam, "media.fix_title", form("id=%s&title=Flower&year=", flower_id));
    CHECK(!r.ok);
    CHECK_STR(r.field, "year");
    CHECK(there("Kino/Flower (2020)/Flower.mp4"));
    th_halt();
}

TEST(manage_fixes_an_episodes_number) {
    int adam, eva;
    boot(&adam, &eva);
    put("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4", "e1");
    put("Series/Garden/Season 1/Garden.S01E05.mp4", "e5");
    put("Series/Garden/Season 1/Garden.S01E05.web.mp4", "e5 copy");
    put("Series/Garden/Season 1/Garden.S01E05.en.srt", SRT);
    put("Series/Garden/Season 2/Garden.S02E01.mkv", "e21");
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaFile *e5 = find("Series/Garden/Season 1/Garden.S01E05.mp4"), *e21 = find("Series/Garden/Season 2/Garden.S02E01.mkv");
    const MediaFile *e1 = find("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4");
    const MediaSeries *garden = series_named("Garden");
    CHECK(e5 && e21 && e1 && garden);
    if (!e5 || !e21 || !e1 || !garden) return;
    uint64_t e5_id = e5->id, e21_id = e21->id, e1_id = e1->id, garden_id = garden->id;
    CHECK(th_act(adam, "media.kids", "p=0:Series/Garden/Season%201&on=1").ok);
    CHECK(media_kids_marked(g_root, "Series/Garden/Season 1"));
    media_watch_report(eva, e5_id, 300, 1200, false, now_ms());

    /* the dialog is the admins' */
    char u[300];
    snprintf(u, sizeof u, "/media/series/%016llx?manage=1&s=1&fix=%016llx", (unsigned long long)garden_id, (unsigned long long)e5_id);
    const char *dlg = th_page(adam, u);
    CHECK_HAS(dlg, "Fix the episode number");
    CHECK_HAS(dlg, "<code class=\"media-code\">Garden S01E05.mp4</code>");
    CHECK_HAS(dlg, "data-a=\"media.fix_episode\"");
    CHECK(!strstr(th_page(eva, u), "media.fix_episode"));
    CHECK(!th_act(eva, "media.fix_episode", form("id=%s&season=2&episode=3", e5_id)).ok);

    /* numbers that aren't, titles a disk can't store */
    ThResult r = th_act(adam, "media.fix_episode", form("id=%s&season=2&episode=0", e5_id));
    CHECK_STR(r.field, "episode");
    r = th_act(adam, "media.fix_episode", form("id=%s&season=x&episode=3", e5_id));
    CHECK_STR(r.field, "season");
    r = th_act(adam, "media.fix_episode", form("id=%s&season=2&episode=3&last=2", e5_id));
    CHECK_STR(r.field, "last");
    r = th_act(adam, "media.fix_episode", form("id=%s&season=2&episode=3&title=A%%0AB", e5_id));
    CHECK_STR(r.field, "title");
    CHECK(there("Series/Garden/Season 1/Garden.S01E05.mp4"));

    /* the fix: renamed, and moved into the season's folder - its copy and subtitles too */
    r = th_act(adam, "media.fix_episode", form("id=%s&season=2&episode=3&title=Weeds", e5_id));
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Renamed it Garden S02E03");
    CHECK(there("Series/Garden/Season 2/Garden S02E03 - Weeds.mp4"));
    CHECK(there("Series/Garden/Season 2/Garden S02E03 - Weeds.web.mp4"));
    CHECK(there("Series/Garden/Season 2/Garden S02E03 - Weeds.en.srt"));
    CHECK(!there("Series/Garden/Season 1/Garden.S01E05.mp4"));
    CHECK(!there("Series/Garden/Season 1/Garden.S01E05.en.srt"));
    uint64_t new_id = media_make_id(g_root, "Series/Garden/Season 2/Garden S02E03 - Weeds.mp4");
    const MediaFile *now = media_file(new_id); /* (rescanned) */
    CHECK(now != NULL);
    if (now) {
        CHECK_INT(now->name.season, 2);
        CHECK_INT(now->name.episode, 3);
        CHECK_STR(now->name.ep_title, "Weeds");
        CHECK(now->series == garden_id && now->web != 0);
    }
    const MediaWatch *w = media_watch_get(eva, new_id);
    CHECK(w && w->pos == 300);
    CHECK(media_kids_marked(g_root, "Series/Garden/Season 2")); /* (it left a folder not for children) */
    CHECK_HAS(last_activity(), "renamed Garden S01E05 to Garden S02E03");

    /* never over a file that's there */
    r = th_act(adam, "media.fix_episode", form("id=%s&season=2&episode=3&title=Weeds", e1_id));
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "There's already a file called Garden S02E03 - Weeds.mp4 there");
    CHECK(there("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4"));

    /* a season folder is made when there's none; two episodes in one file */
    CHECK(th_act(adam, "media.fix_episode", form("id=%s&season=3&episode=1&last=2", e21_id)).ok);
    CHECK(there("Series/Garden/Season 3/Garden S03E01-E02.mkv"));
    now = media_file(media_make_id(g_root, "Series/Garden/Season 3/Garden S03E01-E02.mkv"));
    CHECK(now && now->name.episode == 1 && now->name.episode_end == 2);
    CHECK(there("Series/Garden/Season 2")); /* (still holds S02E03) */
    th_halt();
}

/* Renaming carries every subtitle along - a DVD VobSub's .idx and its .sub as a pair, a Blu-ray .sup,
   numbered ones - and a .sub with no .idx (MicroDVD text, not the library's) stays where it was. */
TEST(manage_renames_carry_every_subtitle_along) {
    int adam, eva;
    boot(&adam, &eva);
    (void)eva;
    put("Movies/Nightmare 1993/Nightmare.1993.mkv", "film");
    put("Movies/Nightmare 1993/Nightmare.1993.en.idx", "idx");
    put("Movies/Nightmare 1993/Nightmare.1993.en.sub", "pictures");
    put("Movies/Nightmare 1993/Nightmare.1993.en.2.srt", SRT);
    put("Movies/Nightmare 1993/Nightmare.1993.sv.sup", "PG");
    put("Series/Garden/Season 1/Garden.S01E05.mp4", "e5");
    put("Series/Garden/Season 1/Garden.S01E05.sv.idx", "idx");
    put("Series/Garden/Season 1/Garden.S01E05.sv.sub", "pictures");
    put("Series/Garden/Season 1/Garden.S01E05.de.sub", "{1}{2}MicroDVD");
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaFile *film = find("Movies/Nightmare 1993/Nightmare.1993.mkv"), *e5 = find("Series/Garden/Season 1/Garden.S01E05.mp4");
    CHECK(film && e5);
    if (!film || !e5) {
        th_halt();
        return;
    }
    uint64_t e5_id = e5->id;
    CHECK(th_act(adam, "media.fix_title", form("id=%s&title=The%%20Nightmare&year=1993", film->id)).ok);
    static const char *const moved[] = {"The Nightmare (1993).mkv", "The Nightmare (1993).en.idx", "The Nightmare (1993).en.sub",
                                        "The Nightmare (1993).en.2.srt", "The Nightmare (1993).sv.sup"};
    for (int i = 0; i < countof(moved); i++) {
        char rel[200];
        snprintf(rel, sizeof rel, "Movies/The Nightmare (1993)/%s", moved[i]);
        if (!there(rel)) test_fail(__FILE__, __LINE__, "%s isn't there", rel);
    }
    CHECK(!there("Movies/Nightmare 1993"));
    CHECK(th_act(adam, "media.fix_episode", form("id=%s&season=1&episode=6", e5_id)).ok);
    CHECK(there("Series/Garden/Season 1/Garden S01E06.mp4"));
    CHECK(there("Series/Garden/Season 1/Garden S01E06.sv.idx"));
    CHECK(there("Series/Garden/Season 1/Garden S01E06.sv.sub"));
    CHECK(!there("Series/Garden/Season 1/Garden.S01E05.sv.sub"));
    CHECK(there("Series/Garden/Season 1/Garden.S01E05.de.sub")); /* (not the episode's) */
    th_halt();
}

TEST(manage_shows_what_takes_room_biggest_first) {
    int adam, eva;
    boot(&adam, &eva);
    put("Movies/Wee (2003)/Wee (2003).mkv", "too small to list");
    setting_set("media.roots", g_root);
    media_scan_sync();
    CHECK(!strstr(th_page(adam, "/media/settings"), "What takes room")); /* (nothing takes room yet) */
    enum { MB = 1 << 20 };
    char *big = xmalloc(4 * MB);
    memset(big, 'x', 4 * MB);
    put_data("Series/Garden/Season 1/Garden.S01E01.mkv", big, 3 * MB / 2);
    put_data("Series/Garden/Season 1/Garden.S01E02.mkv", big, 3 * MB / 2);
    put_data("Movies/Bigfoot (2001)/Bigfoot (2001).mkv", big, 5 * MB / 2);
    put_data("Movies/Tiny (2002)/Tiny (2002).mkv", big, 100);
    put_data("Movies/Tiny (2002)/Tiny (2002).web.mp4", big, 7 * MB / 2);
    free(big);
    media_scan_sync();
    const char *page = th_page(adam, "/media/settings");
    CHECK_HAS(page, "What takes room");
    const char *tiny = strstr(page, "<b>Tiny</b>"), *garden = strstr(page, "<b>Garden</b>"), *bigfoot = strstr(page, "<b>Bigfoot</b>");
    CHECK(tiny && garden && bigfoot);
    CHECK(tiny < garden && garden < bigfoot); /* 3.5 MB with its copy, 3 MB, 2.5 MB */
    CHECK(!strstr(page, "<b>Wee</b>"));
    const MediaSeries *s = series_named("Garden");
    if (s) CHECK_HAS(page, url("/media/series/%s?manage=1\"", s->id));
    CHECK(!strstr(th_page(eva, "/media/settings"), "What takes room"));
    th_halt();
}

/* ------------------------------------------------------------ languages */

static size_t ebml(uint8_t *out, uint32_t id, const void *data, size_t len) {
    size_t n = 0;
    for (int s = 24; s >= 0; s -= 8)
        if (id >> s) out[n++] = (uint8_t)(id >> s);
    out[n++] = (uint8_t)(0x80 | len); /* every element here is < 127 bytes */
    memcpy(out + n, data, len);
    return n + len;
}

/* A tiny Matroska file the probe reads a length from: 720x480 MPEG-2 (a DVD rip) */
static void put_mkv(const char *rel, double minutes) {
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
    put_data(rel, out, n);
}

/* 1.3 (owner, 2026-10-24: "make it so the admin can rename the series"): its folders get the new name, and it
   keeps its id - what TV channels, screen rows and subtitle picks hold - through rescans */
TEST(manage_renames_a_series) {
    int adam, eva;
    boot(&adam, &eva);
    put("Series/Garden Show/Season 1/Garden.Show.S01E01.mkv", "e1");
    put("Series/Garden Show/Season 1/Garden.Show.S01E02.mkv", "e2");
    put("Series/Garden Show/Season 1/Garden.Show.S01E02.web.mp4", "e2 copy");
    put("Series/Garden Show/Season 1/Garden.Show.S01E02.en.srt", SRT);
    put("Series/Garden Show/poster.jpg", "jpeg");
    put("Series/Garden Show/Season 2/Garden.Show.S02E01.mkv", "e3");
    put("Kids/Garden Show/Season 3/Garden.Show.S03E01.mkv", "e4"); /* (a second folder of the same show) */
    put("Series/Orbit/Season 1/Orbit.S01E01.mkv", "o1");
    put("Series/Loose/Loose.S01E01.mkv", "l1");                       /* (no season folder: named by its file too) */
    put("Series/Mixed/Season 1/Mixed.S01E01.mkv", "m1");
    put("Series/Mixed/Some Film (2001)/Some Film (2001).mkv", "film");                 /* (another video in its folder) */
    put("Solo.S01E01.mkv", "s1");                                     /* (no folder of its own) */
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaSeries *garden = series_named("Garden Show"), *loose = series_named("Loose"), *mixed = series_named("Mixed");
    const MediaSeries *solo = series_named("Solo");
    const MediaFile *e2 = find("Series/Garden Show/Season 1/Garden.Show.S01E02.mkv");
    CHECK(garden && loose && mixed && solo && e2 && series_named("Orbit"));
    if (!garden || !loose || !mixed || !solo || !e2) return;
    uint64_t garden_id = garden->id, loose_id = loose->id, mixed_id = mixed->id, solo_id = solo->id, e2_id = e2->id;
    CHECK_INT(garden->count, 4);
    CHECK(th_act(adam, "media.kids", "p=0:Series/Garden%20Show/Season%202&on=1").ok);
    media_watch_report(eva, e2_id, 300, 1200, false, now_ms());

    /* the button and the dialog are the admins' */
    const char *mg = th_page(adam, url("/media/series/%s?manage=1", garden_id));
    CHECK_HAS(mg, "&amp;rename=1");
    /* (owner, 2026-10-25: no scrolling past a season to rename or remove) the whole series first, then the season
       tabs, the season's Remove, and only then its episodes */
    const char *whole = strstr(mg, "The whole series"), *tabs = strstr(mg, "media-seasons"), *rm_season = strstr(mg, "rm=season"),
               *first_ep = strstr(mg, "<li class=\"media-ep\"");
    CHECK(whole && tabs && rm_season && first_ep && whole < tabs && tabs < rm_season && rm_season < first_ep);
    const char *dlg = th_page(adam, url("/media/series/%s?manage=1&rename=1", garden_id));
    CHECK_HAS(dlg, "Rename the series");
    CHECK_HAS(dlg, "its 2 folders get the new name");
    CHECK_HAS(dlg, "data-a=\"media.rename_series\"");
    CHECK_HAS(dlg, "value=\"Garden Show\"");
    CHECK(!strstr(th_page(eva, url("/media/series/%s?manage=1&rename=1", garden_id)), "media.rename_series"));
    CHECK(!th_act(eva, "media.rename_series", form("id=%s&title=Garden%%20Party", garden_id)).ok);
    CHECK_HAS(th_page(adam, url("/media/series/%s?manage=1&rename=1", solo_id)), "Some of its episodes aren&#39;t in a folder");

    /* names that aren't, or that Hemma would read otherwise, or another series has */
    ThResult r = th_act(adam, "media.rename_series", form("id=%s&title=", garden_id));
    CHECK_STR(r.field, "title");
    r = th_act(adam, "media.rename_series", form("id=%s&title=A%%0AB", garden_id));
    CHECK_STR(r.field, "title");
    r = th_act(adam, "media.rename_series", form("id=%s&title=Garden&year=19x3", garden_id));
    CHECK_STR(r.field, "year");
    r = th_act(adam, "media.rename_series", form("id=%s&title=Garden%%20S01-S03", garden_id));
    CHECK_HAS(r.toast, "Hemma would read that name as \"Garden\"");
    r = th_act(adam, "media.rename_series", form("id=%s&title=orbit", garden_id));
    CHECK_HAS(r.toast, "There's already a series called Orbit");
    r = th_act(adam, "media.rename_series", form("id=%s&title=Tight", loose_id));
    CHECK_HAS(r.toast, "Hemma would still read Loose S01E01 as \"Loose\" by its own file name");
    r = th_act(adam, "media.rename_series", form("id=%s&title=Muddle", mixed_id));
    CHECK_HAS(r.toast, "Its folder holds other videos too");
    r = th_act(adam, "media.rename_series", form("id=%s&title=Duo", solo_id));
    CHECK_HAS(r.toast, "Some of its episodes aren't in a folder");
    CHECK(there("Series/Garden Show/Season 1/Garden.Show.S01E01.mkv") && there("Series/Loose/Loose.S01E01.mkv"));
    r = th_act(adam, "media.rename_series", form("id=%s&title=Garden%%20Show", garden_id));
    CHECK_HAS(r.toast, "That's its name already");

    /* the rename: both folders, everything in them; the same id, its history, its marks */
    r = th_act(adam, "media.rename_series", form("id=%s&title=Garden%%20Party&year=", garden_id));
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Renamed it Garden Party");
    CHECK_STR(r.redirect, url("/media/series/%s?manage=1", garden_id));
    CHECK(there("Series/Garden Party/Season 1/Garden.Show.S01E02.web.mp4"));
    CHECK(there("Series/Garden Party/Season 1/Garden.Show.S01E02.en.srt"));
    CHECK(there("Series/Garden Party/poster.jpg"));
    CHECK(there("Kids/Garden Party/Season 3/Garden.Show.S03E01.mkv"));
    CHECK(!there("Series/Garden Show") && !there("Kids/Garden Show"));
    for (int pass = 0; pass < 2; pass++) { /* (rescanned by the action; and once more, as after a restart) */
        const MediaSeries *now = media_series(garden_id);
        CHECK(now != NULL);
        if (!now) return;
        CHECK_STR(now->title, "Garden Party");
        CHECK_INT(now->count, 4);
        CHECK(now->poster != 0);
        CHECK(series_named("Garden Show") == NULL);
        media_scan_sync();
    }
    uint64_t e2_now = media_make_id(g_root, "Series/Garden Party/Season 1/Garden.Show.S01E02.mkv");
    const MediaFile *moved = media_file(e2_now);
    CHECK(moved && moved->series == garden_id && moved->web != 0);
    const MediaWatch *w = media_watch_get(eva, e2_now);
    CHECK(w && w->pos == 300);
    CHECK(!media_watch_get(eva, e2_id));
    CHECK(media_kids_marked(g_root, "Series/Garden Party/Season 2"));
    CHECK(!media_kids_marked(g_root, "Series/Garden Show/Season 2"));
    CHECK_HAS(last_activity(), "renamed Garden Show to Garden Party");
    CHECK_INT(tbl_count(&T_media_series_name), 1);

    /* and back: its own name again needs nothing remembered */
    CHECK(th_act(adam, "media.rename_series", form("id=%s&title=Garden%%20Show", garden_id)).ok);
    CHECK(there("Series/Garden Show/Season 1/Garden.Show.S01E01.mkv"));
    CHECK(media_series(garden_id) && str_eq(media_series(garden_id)->title, "Garden Show"));
    CHECK_INT(tbl_count(&T_media_series_name), 0);
    th_halt();
}

TEST(manage_speaks_swedish_and_spanish) {
    int adam, eva;
    boot(&adam, &eva);
    (void)eva;
    put("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4", "e1");
    put("Series/Garden/Season 1/Garden.S01E01.Seeds.web.mp4", "e1 copy");
    put("Series/Garden/Season 1/Hospital.mp4", "unnumbered"); /* (numbered by the library) */
    put("Series/Garden/Season 2/Garden.S02E01.mkv", "e3");
    put("Series/Garden/Specials/Garden.S00E01.mkv", "special");
    char *mb = xcalloc(1, 1100000); /* (big enough for What takes room) */
    put_data("Movies/Flower (2020)/Flower.mp4", mb, 1100000);
    free(mb);
    /* a DVD season: six episodes, the disc's play-all and a bonus clip */
    static const struct {
        const char *name;
        double min;
    } disc[] = {{"B1_t01.mkv", 22.6}, {"B2_t02.mkv", 22.6}, {"B3_t03.mkv", 22.6}, {"B4_t04.mkv", 22.6},
                {"B5_t05.mkv", 22.6}, {"B6_t06.mkv", 22.6}, {"B7_t00.mkv", 135.5}, {"C2_t08.mkv", 3.8}};
    for (int i = 0; i < countof(disc); i++) {
        char rel[200];
        snprintf(rel, sizeof rel, "Shows/Rip/Season 1/Disc - 1/%s", disc[i].name);
        put_mkv(rel, disc[i].min);
    }
    setting_set("media.roots", g_root);
    media_scan_sync();
    const MediaSeries *garden = series_named("Garden"), *rip = series_named("Rip");
    const MediaFile *e1 = find("Series/Garden/Season 1/Garden.S01E01.Seeds.mp4"), *flower = find("Movies/Flower (2020)/Flower.mp4");
    const MediaFile *hospital = find("Series/Garden/Season 1/Hospital.mp4");
    CHECK(garden && rip && e1 && flower && hospital);
    if (!garden || !rip || !e1 || !flower || !hospital) return;
    const MediaIndex *ix = media_lib();
    const MediaFile *bonus = NULL;
    for (int i = 0; i < ix->nextras; i++)
        if (ix->files[ix->extras[i]].role == MF_BONUS) bonus = &ix->files[ix->extras[i]];
    CHECK(bonus != NULL);
    CHECK_HAS(th_page(adam, "/media/settings"), "What takes room");
    uint64_t e1_id = e1->id, flower_id = flower->id;
    static char urls[16][300];
    int n = 0;
    uint64_t g = garden->id;
    snprintf(urls[n++], 300, "/media/series/%016llx", (unsigned long long)g);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1", (unsigned long long)g);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=0", (unsigned long long)g);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=1&rm=%016llx", (unsigned long long)g, (unsigned long long)e1->id);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=1&rm=season", (unsigned long long)g);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=0&rm=season", (unsigned long long)g);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&rm=series", (unsigned long long)g);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&rename=1", (unsigned long long)g);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=1&fix=%016llx", (unsigned long long)g, (unsigned long long)hospital->id);
    snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=1", (unsigned long long)rip->id);
    if (bonus) {
        snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=1&fix=%016llx", (unsigned long long)rip->id,
                 (unsigned long long)bonus->id);
        snprintf(urls[n++], 300, "/media/series/%016llx?manage=1&s=1&rm=%016llx", (unsigned long long)rip->id,
                 (unsigned long long)bonus->id);
    }
    snprintf(urls[n++], 300, "/media/watch/%016llx", (unsigned long long)flower->id);
    snprintf(urls[n++], 300, "/media/watch/%016llx?fix=1", (unsigned long long)flower->id);
    str_copy(urls[n++], 300, "/media/settings");
    /* data: people, the shows, films and episodes, and the library folder */
    /* (an extra's file name is its name: "Rip S01 (C2_t08.mkv)"; Library settings names the subtitle languages, each
       in its own words; "Film" and "Series" read the same in Swedish or Spanish) */
    static const char *allow[] = {"Adam",   "A",       "Eva",     "Garden",  "Seeds",    "Hospital", "Flower",
                                  "Rip",    "B",       "C",       "t",       "mkv",      "Film",     "Series",
                                  "series", "Extras",  "Extra",   "Video",   "System",   "Svenska",  "English",
                                  "Norsk",  "Dansk",   "Suomi",   "Deutsch", "Français", "Español",  "TVmaze",
                                  "OMDb",   "omdbapi.com", "Wikipedia", NULL, NULL}; /* (+ the services categories are looked up at) */
    allow[countof(allow) - 2] = g_root;
    Buf rep = {0};
    int bad = 0;
    for (int i = 0; i < n; i++) bad += th_untranslated(adam, urls[i], allow, &rep);
    if (bad) printf("%s", buf_cstr(&rep));
    CHECK_INT(bad, 0);
    buf_free(&rep);

    /* the toasts, in the doer's language */
    pref_set(adam, PREF_LANG, "sv");
    ThResult r = th_act(adam, "media.fix_title", form("id=%s&title=Flower&year=2021", flower_id));
    CHECK(r.ok);
    CHECK_HAS(r.toast, "Bytte namn till Flower (2021)");
    r = th_act(adam, "media.manage_remove", form("what=episode&id=%s", e1_id));
    CHECK_HAS(r.toast, "Tog bort Garden S01E01 - ");
    pref_set(adam, PREF_LANG, "");
    th_halt();
}
