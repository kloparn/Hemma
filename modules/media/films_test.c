/* FILMS AND THEIR YEARS (2.4; owner, 2026-10-30: a folder "Disney" of films named without years read as 54 other
   videos - "I would like the year of a movie to be almost mandatory? Otherwise it's hard to identify stuff"): a
   feature-length video is a film wherever it lies, Library settings lists films without a year, and an upload's
   preview asks for each film's year and lets a folder of videos be films. */
#include "../../core/codec.h"
#include "../../core/plat.h"
#include "../../tests/harness.h"
#include "../av/mp4.h"
#include "library.h"
#include "upload.h"

static char g_root[512];

static bool f_put(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)ctx) == n; }
static bool f_put_at(void *ctx, int64_t off, const void *p, size_t n) {
    FILE *f = ctx;
    fflush(f);
    bool ok = file_seek(f, off) && fwrite(p, 1, n, f) == n;
    fflush(f);
    return fseek(f, 0, SEEK_END) == 0 && ok;
}

/* A tiny MP4 the probe reads as `seconds` long */
static void put_mp4(const char *rel, int seconds) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    FILE *f = file_open(path, "wb");
    Mp4Out out = {f, f_put, f_put_at};
    Mp4 *m = mp4_new(&out, 0, 4096);
    static const uint8_t cfg[] = {1, 0x42, 0, 0x1E, 0xFF, 0xE0, 0}, frame[16] = {0, 0, 0, 12};
    Mp4Track t = {.codec = MP4_H264, .timescale = 1000, .config = cfg, .config_len = sizeof cfg, .width = 64, .height = 48};
    mp4_add_track(m, &t);
    mp4_begin(m);
    mp4_sample(m, 0, frame, sizeof frame, (uint32_t)seconds * 1000, 0, true);
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

static int admin(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

TEST(media_a_feature_length_video_is_a_film_wherever_it_lies) {
    char data[512];
    str_copy(data, sizeof data, test_tmpdir());
    th_boot_in(data);
    int adam = admin("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    put_mp4("Disney/Up.mp4", 96 * 60);
    put_mp4("Disney/Aladdin/1  Aladdin.mp4", 90 * 60);
    put_mp4("Disney/Aladdin/2  The Return Of Jafar.mp4", 69 * 60);
    put_mp4("Disney/Short Clip.mp4", 7 * 60);                  /* (short: another video) */
    put_mp4("Home videos/Wedding 2019.mp4", 2 * 3600);         /* (a home video, however long) */
    put_mp4("Movies/Inception (2010)/Inception (2010).mp4", 148 * 60);
    setting_set("media.roots", g_root);
    media_scan_sync();
    for (int pass = 0; pass < 2; pass++) { /* (the scan, then a restart: the index from the cache) */
        const MediaFile *up = find("Disney/Up.mp4"), *al = find("1  Aladdin.mp4"), *ja = find("2  The Return Of Jafar.mp4"),
                        *clip = find("Short Clip.mp4"), *wed = find("Wedding 2019.mp4");
        CHECK(up && al && ja && clip && wed);
        CHECK_INT(up->name.kind, MEDIA_MOVIE);
        CHECK_STR(up->name.title, "Up");
        CHECK_INT(al->name.kind, MEDIA_MOVIE);
        CHECK_STR(al->name.title, "Aladdin");
        CHECK_INT(al->name.order, 10);
        CHECK_STR(ja->name.title, "The Return Of Jafar");
        CHECK_INT(clip->name.kind, MEDIA_OTHER);
        CHECK_INT(wed->name.kind, MEDIA_OTHER);
        CHECK_INT(media_lib()->nmovies, 4);
        if (!pass) {
            th_halt();
            th_boot_in(data);
        }
    }
    /* Library settings: the films without a year, each with Fix title - Inception has its year */
    const char *html = th_page(adam, "/media/settings/manage");
    CHECK_HAS(html, "Films without a year");
    CHECK_HAS(html, "3 films have no year in their names.");
    char want[120];
    snprintf(want, sizeof want, "href=\"/media/watch/%016llx?fix=1\"", (unsigned long long)find("Disney/Up.mp4")->id);
    CHECK_HAS(html, want);
    const MediaFile *inception = find("Movies/Inception (2010)/Inception (2010).mp4");
    CHECK(inception != NULL);
    snprintf(want, sizeof want, "id=\"media-noyear-%016llx\"", inception ? (unsigned long long)inception->id : 0ULL);
    CHECK(!strstr(html, want)); /* (the tab lists every film too: Inception isn't among those without a year) */
    th_halt();
}

/* rels sorted into the library folder by the upload's rules, with the preview's years and "These are films" */
static UpSorted *sort_these(const char *const *rels, int n, const UpYear *years, int nyears, const uint64_t *films, int nfilms) {
    UpSorted *out = xcalloc((size_t)n, sizeof *out);
    UpSortOpts o = {.root = g_root, .years = years, .nyears = nyears, .films = films, .nfilms = nfilms};
    upload_sort(&o, rels, n, out);
    return out;
}

TEST(upload_puts_a_films_year_on_its_folder_and_takes_a_folder_as_films) {
    th_boot();
    str_copy(g_root, sizeof g_root, test_tmpdir());
    setting_set("media.roots", g_root);
    media_scan_sync();
    static const char *const rels[] = {
        "Toy Story/Toy Story 1.mkv",   /* 0: under no Movies folder: another video, until "These are films" */
        "Disney/Bambi.mkv",            /* 1 */
        "Disney/1  Aladdin.mkv",       /* 2 */
        "Arrival.1080p.BluRay.x264.mkv", /* 3: a film by its name, without a year */
        "Inception (2010).mkv",        /* 4: a film with its year */
    };
    int n = (int)countof(rels);
    UpSorted *s = sort_these(rels, n, NULL, 0, NULL, 0);
    CHECK_INT(s[0].kind, UPK_OTHER);
    CHECK_INT(s[1].kind, UPK_OTHER);
    CHECK(s[1].group && s[1].group == s[2].group); /* (the same top folder: one "These are films") */
    CHECK(s[0].group != s[1].group);
    CHECK_INT(s[3].kind, UPK_FILM);
    CHECK(s[3].no_year);
    CHECK_STR(s[3].dest, "Movies/Arrival/Arrival.1080p.BluRay.x264.mkv");
    CHECK(!s[4].no_year);
    CHECK_STR(s[4].dest, "Movies/Inception (2010)/Inception (2010).mkv");
    uint64_t arrival = s[3].film, disney = s[1].group;
    free(s);
    /* the year typed: the film's folder carries it (its file keeps its name) - and the library reads it from there */
    UpYear years[] = {{arrival, 2016}, {upload_show_key("Bambi"), 1942}};
    uint64_t films[] = {disney};
    s = sort_these(rels, n, years, 2, films, 1);
    CHECK_STR(s[3].dest, "Movies/Arrival (2016)/Arrival.1080p.BluRay.x264.mkv");
    CHECK_INT(s[3].year, 2016);
    CHECK(s[3].no_year); /* (its name has none: the preview keeps its box) */
    CHECK_INT(s[1].kind, UPK_FILM);
    CHECK(s[1].as_film);
    CHECK_STR(s[1].dest, "Movies/Bambi (1942)/Bambi.mkv");
    CHECK_INT(s[2].kind, UPK_FILM);
    CHECK_STR(s[2].title, "Aladdin");
    CHECK_STR(s[2].dest, "Movies/Aladdin/1  Aladdin.mkv");
    CHECK_INT(s[0].kind, UPK_OTHER); /* (Toy Story's folder wasn't ticked) */
    MediaName nm;
    media_parse_name("Movies/Bambi (1942)/Bambi.mkv", &nm);
    CHECK_INT(nm.kind, MEDIA_MOVIE);
    CHECK_STR(nm.title, "Bambi");
    CHECK_INT(nm.year, 1942);
    media_parse_name("Movies/Aladdin (1992)/1  Aladdin.mkv", &nm);
    CHECK_STR(nm.title, "Aladdin");
    CHECK_INT(nm.year, 1992);
    free(s);
    th_halt();
}

/* The preview: a box for each film without a year, Start asking first while some are empty, a folder's
   "These are films" - read from the preview's fields and from Start's plan alike */
TEST(upload_preview_asks_for_films_years) {
    th_boot();
    int adam = admin("Adam");
    str_copy(g_root, sizeof g_root, test_tmpdir());
    setting_set("media.roots", g_root);
    media_scan_sync();
    upload_test_free_space(1LL << 40);
    const char *files = "[[\"Arrival.1080p.BluRay.x264.mkv\",900],[\"Inception (2010).mkv\",800],"
                        "[\"Disney/Bambi.mkv\",700],[\"Disney/Dumbo.mkv\",600]]";
    Buf form = {0};
    buf_puts(&form, "sort=1&root=0&files=");
    url_encode(&form, files, false);
    upload_test_preview(adam, buf_cstr(&form));
    const char *page = th_page(adam, "/media/upload");
    char key[17], want[160];
    media_id_hex(upload_show_key("Arrival"), key);
    snprintf(want, sizeof want, "<input name=\"y.%s\" value=\"\"", key);
    CHECK_HAS(page, want);
    CHECK_HAS(page, "1 film has no year");
    CHECK_HAS(page, "needs its year");
    CHECK_HAS(page, "data-confirm=\"1 film has no year. Upload it anyway?\"");
    CHECK_HAS(page, "The videos in Disney are films");
    UpSorted probe[1];
    static const char *const one[] = {"Disney/Bambi.mkv"};
    UpSortOpts o = {.root = g_root};
    upload_sort(&o, one, 1, probe);
    char gkey[17];
    media_id_hex(probe[0].group, gkey);
    snprintf(want, sizeof want, "name=\"films.%s\" value=\"1\">", gkey);
    CHECK_HAS(page, want);
    /* the year typed and Disney ticked: Bambi and Dumbo ask for theirs now */
    buf_printf(&form, "&y.%s=2016&films.%s=1", key, gkey);
    upload_test_preview(adam, buf_cstr(&form));
    page = th_page(adam, "/media/upload");
    snprintf(want, sizeof want, "<input name=\"y.%s\" value=\"2016\"", key);
    CHECK_HAS(page, want);
    CHECK_HAS(page, "2 films have no year");
    CHECK_HAS(page, "data-confirm=\"2 films have no year. Upload them anyway?\"");
    snprintf(want, sizeof want, "name=\"films.%s\" value=\"1\" checked>", gkey);
    CHECK_HAS(page, want);
    CHECK_HAS(page, "Movies <i>/</i> Arrival (2016)");
    /* every year given: no question; a year that can't be one counts as none */
    char bambi[17], dumbo[17];
    media_id_hex(upload_show_key("Bambi"), bambi);
    media_id_hex(upload_show_key("Dumbo"), dumbo);
    buf_printf(&form, "&y.%s=1942&y.%s=19411", bambi, dumbo);
    upload_test_preview(adam, buf_cstr(&form));
    CHECK_HAS(th_page(adam, "/media/upload"), "data-confirm=\"1 film has no year. Upload it anyway?\"");
    buf_printf(&form, "&y.%s=1941", dumbo);
    upload_test_preview(adam, buf_cstr(&form));
    page = th_page(adam, "/media/upload");
    CHECK(!strstr(page, "data-confirm="));
    CHECK_HAS(page, "The films&#39; years");
    CHECK_HAS(page, "Movies <i>/</i> Dumbo (1941)");
    buf_free(&form);
    upload_test_preview(adam, NULL);
    th_halt();
}
