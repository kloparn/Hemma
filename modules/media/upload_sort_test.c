/* Where an upload's files go (upload_sort.c): episodes into <Series>/<Show>/Season N, films into
 * <Movies>/<Title (Year)>, next to what the library already has; subtitles and pictures with their
 * video; disc rips keep their folders; the rest stays as chosen. */
#include "../../tests/harness.h"
#include "upload.h"

static void put_file(const char *root, const char *rel) {
    char path[1024], dir[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    str_copy(dir, sizeof dir, path);
    *strrchr(dir, '/') = '\0';
    dir_make(dir);
    file_write_atomic(path, "x", 1);
}

/* rels sorted into root: out[i].dest */
static UpSorted *sort_these(const char *root, const char *const *rels, int n, uint64_t series, const UpName *names,
                            int nnames) {
    UpSorted *out = xcalloc((size_t)n, sizeof *out);
    UpSortOpts o = {.root = root, .series = series, .names = names, .nnames = nnames};
    upload_sort(&o, rels, n, out);
    return out;
}

TEST(upload_sorts_episodes_and_films_by_their_names) {
    th_boot();
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    /* the library: a show in "Serier" with its season folder, a film in "Filmer", an empty "TV Shows" (Serier wins) */
    put_file(root, "Serier/Friends/Säsong 01/Friends.S01E01.mkv");
    put_file(root, "Filmer/Inception (2010)/Inception (2010).mkv");
    put_file(root, "TV Shows/.keep");
    setting_set("media.roots", root);
    media_scan_sync();
    CHECK_INT(media_lib()->nseries, 1);

    static const char *const rels[] = {
        "Friends.S01E02.The.One.With.the.Sonogram.mkv",          /* 0: next to the old ones, in their season folder */
        "Friends.S02E01.mkv",                                    /* 1: a new season folder there */
        "New Show/Season 1/New.Show.S01E03.mkv",                 /* 2: a new show */
        "New Show/Season 1/New.Show.S01E03.en.srt",              /* 3: with its video */
        "New Show/poster.jpg",                                   /* 4: the show's own folder goes along */
        "70's show/season 1 70's show/Disc - 2/B3_t03.mkv",      /* 5: a disc rip keeps its folders */
        "Inception.2010.1080p.BluRay.x264.mkv",                  /* 6: the film's folder in the library */
        "Downloads/Arrival (2016)/Arrival.2016.1080p.mkv",       /* 7: a new film */
        "Downloads/Arrival (2016)/Arrival.2016.1080p.sv.srt",    /* 8: with it */
        "Downloads/Arrival (2016)/Extras/Making of.mkv",         /* 9: its folder goes along */
        "Holiday/IMG_1234.mov",                                  /* 10: nothing to go by: as chosen */
        "Season 1/01 - Pilot.mkv",                               /* 11: an episode of no named series */
        "notes.txt",                                             /* 12: not for the library */
        "Friends.S02E01.web.mp4",                                /* 13: a copy, where its original goes */
        NULL,                                                    /* 14: a bad name */
    };
    int n = (int)countof(rels);
    UpSorted *s = sort_these(root, rels, n, 0, NULL, 0);
    CHECK_STR(s[0].dest, "Serier/Friends/Säsong 01/Friends.S01E02.The.One.With.the.Sonogram.mkv");
    CHECK_INT(s[0].kind, UPK_EPISODE);
    CHECK(s[0].known);
    CHECK_STR(s[0].title, "Friends");
    CHECK_INT(s[0].season, 1);
    CHECK_INT(s[0].episode, 2);
    CHECK_STR(s[1].dest, "Serier/Friends/Season 2/Friends.S02E01.mkv");
    CHECK_STR(s[2].dest, "Serier/New Show/Season 1/New.Show.S01E03.mkv");
    CHECK(!s[2].known);
    CHECK_STR(s[3].dest, "Serier/New Show/Season 1/New.Show.S01E03.en.srt");
    CHECK_INT(s[3].kind, UPK_SUB);
    CHECK_STR(s[3].title, "New Show");
    CHECK_STR(s[4].dest, "Serier/New Show/poster.jpg");
    CHECK_INT(s[4].kind, UPK_PIC);
    CHECK_INT(s[5].kind, UPK_DISC);
    CHECK_STR(s[5].dest, "Serier/70's Show/season 1 70's show/Disc - 2/B3_t03.mkv");
    CHECK_STR(s[6].dest, "Filmer/Inception (2010)/Inception.2010.1080p.BluRay.x264.mkv");
    CHECK_INT(s[6].kind, UPK_FILM);
    CHECK(s[6].known);
    CHECK_INT(s[6].year, 2010);
    CHECK_STR(s[7].dest, "Filmer/Arrival (2016)/Arrival.2016.1080p.mkv");
    CHECK_STR(s[8].dest, "Filmer/Arrival (2016)/Arrival.2016.1080p.sv.srt");
    CHECK_STR(s[9].dest, "Filmer/Arrival (2016)/Extras/Making of.mkv");
    CHECK_STR(s[10].dest, "Holiday/IMG_1234.mov");
    CHECK(s[10].kept);
    CHECK_INT(s[11].kind, UPK_ASK);
    CHECK_STR(s[11].dest, "Season 1/01 - Pilot.mkv");
    CHECK(s[11].kept);
    CHECK_INT(s[12].kind, UPK_JUNK);
    CHECK_STR(s[13].dest, "Serier/Friends/Season 2/Friends.S02E01.web.mp4");
    CHECK_INT(s[14].kind, UPK_JUNK);
    free(s);

    /* the preview named the unknown series - and renamed the new show to the one the library has */
    UpName names[] = {{upload_show_key("Unknown series"), "New Show"}, {upload_show_key("New Show"), "friends"}};
    s = sort_these(root, rels, n, 0, names, 2);
    CHECK_STR(s[11].dest, "Serier/New Show/Season 1/01 - Pilot.mkv"); /* (a name typed is the name: no chains) */
    CHECK_INT(s[11].kind, UPK_EPISODE);
    CHECK(!s[11].kept);
    CHECK_STR(s[2].dest, "Serier/Friends/Säsong 01/New.Show.S01E03.mkv");
    CHECK_STR(s[2].found, "New Show");
    CHECK_STR(s[2].title, "Friends"); /* (the library's spelling) */
    CHECK_STR(s[4].dest, "Serier/Friends/poster.jpg");
    free(s);

    /* "Add episodes" on Friends' page: everything into its folder, the season from the names */
    static const char *const more[] = {"Whatever/Season 3/x.S03E04.mkv", "Whatever/Bonus clip.mkv",
                                       "Some.Film.2001.mkv", "Season 4/Disc 1/t00.mkv"};
    s = sort_these(root, more, 4, media_lib()->series[0].id, NULL, 0);
    CHECK_STR(s[0].dest, "Serier/Friends/Season 3/x.S03E04.mkv");
    CHECK_STR(s[1].dest, "Serier/Friends/Bonus clip.mkv");
    CHECK_STR(s[2].dest, "Serier/Friends/Some.Film.2001.mkv");
    CHECK_STR(s[3].dest, "Serier/Friends/Season 4/Disc 1/t00.mkv");
    free(s);
    th_halt();
}

TEST(upload_sorts_into_a_library_folder_named_like_its_own) {
    th_boot();
    char base[512], series[600], movies[600], empty[600];
    str_copy(base, sizeof base, test_tmpdir());
    snprintf(series, sizeof series, "%s/TV", base);
    snprintf(movies, sizeof movies, "%s/Movies", base);
    snprintf(empty, sizeof empty, "%s/Media", base);
    dir_make(series);
    dir_make(movies);
    dir_make(empty);
    static const char *const rels[] = {"Show S01E01.mkv", "Film (2003).mkv"};
    UpSorted *s = sort_these(series, rels, 2, 0, NULL, 0); /* a library folder that is a series folder ... */
    CHECK_STR(s[0].dest, "Show/Season 1/Show S01E01.mkv");
    CHECK_STR(s[1].dest, "Movies/Film (2003)/Film (2003).mkv");
    free(s);
    s = sort_these(movies, rels, 2, 0, NULL, 0); /* ... or a movies folder */
    CHECK_STR(s[0].dest, "Series/Show/Season 1/Show S01E01.mkv");
    CHECK_STR(s[1].dest, "Film (2003)/Film (2003).mkv");
    free(s);
    s = sort_these(empty, rels, 2, 0, NULL, 0); /* nothing there yet: Series and Movies */
    CHECK_STR(s[0].dest, "Series/Show/Season 1/Show S01E01.mkv");
    CHECK_STR(s[1].dest, "Movies/Film (2003)/Film (2003).mkv");
    free(s);
    /* specials, and a show and its season named by the folders only */
    static const char *const sp[] = {"Show S00E02.mkv", "Doctor Who/Specials/Doctor.Who.S00E01.mkv"};
    s = sort_these(empty, sp, 2, 0, NULL, 0);
    CHECK_STR(s[0].dest, "Series/Show/Specials/Show S00E02.mkv");
    CHECK_STR(s[1].dest, "Series/Doctor Who/Specials/Doctor.Who.S00E01.mkv");
    free(s);
    th_halt();
}

/* A DVD VobSub is an .idx and a .sub: the .sub is subtitles only with its .idx among the chosen files (alone
   it's MicroDVD text), and goes wherever its .idx goes. */
TEST(upload_sorts_a_vobsub_with_its_index) {
    th_boot();
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    static const char *const rels[] = {
        "Downloads/Arrival (2016)/Arrival.2016.1080p.mkv",    /* 0 */
        "Downloads/Arrival (2016)/Arrival.2016.1080p.en.idx", /* 1: with its video */
        "Downloads/Arrival (2016)/Arrival.2016.1080p.en.sub", /* 2: with its .idx */
        "Downloads/Arrival (2016)/notes.sub",                 /* 3: alone - text */
        "Show S01E01.mkv",                                    /* 4 */
        "Show S01E01.sv.IDX",                                 /* 5: the extension in capitals */
        "Show S01E01.sv.sub",                                 /* 6 */
        "Odd/thing.idx",                                      /* 7: no video - as chosen */
        "Odd/thing.sub",                                      /* 8: ... beside it */
        "A/x.sub",                                            /* 9: its .idx is in another folder: text */
        "B/x.idx",                                            /* 10 */
    };
    UpSorted *s = sort_these(root, rels, (int)countof(rels), 0, NULL, 0);
    CHECK_STR(s[1].dest, "Movies/Arrival (2016)/Arrival.2016.1080p.en.idx");
    CHECK_STR(s[2].dest, "Movies/Arrival (2016)/Arrival.2016.1080p.en.sub");
    CHECK_INT(s[2].kind, UPK_SUB);
    CHECK_STR(s[2].title, "Arrival");
    CHECK_INT(s[2].year, 2016);
    CHECK_INT(s[3].kind, UPK_JUNK);
    CHECK_STR(s[5].dest, "Series/Show/Season 1/Show S01E01.sv.IDX");
    CHECK_STR(s[6].dest, "Series/Show/Season 1/Show S01E01.sv.sub");
    CHECK_INT(s[6].kind, UPK_SUB);
    CHECK_INT(s[6].episode, 1);
    CHECK_STR(s[8].dest, "Odd/thing.sub");
    CHECK_INT(s[8].kind, UPK_SUB);
    CHECK(s[8].kept);
    CHECK_INT(s[9].kind, UPK_JUNK);
    CHECK_INT(s[10].kind, UPK_SUB);
    free(s);
    th_halt();
}

/* A show the library keeps in several places (a household's real one, 2026-10-23): a copy at the library
   folder's top level, its seasons in numbered folders under a "COMPLETE" release folder, a later season in
   a folder of its own. New episodes go into a folder named after the show - never the library folder's top
   level, where the first episode the library listed happened to sit (seasons 6 and 7 landed there). */
TEST(upload_sorts_a_show_into_the_folder_named_after_it) {
    th_boot();
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    put_file(root, "Solsidan S01E01.web.mp4");
    put_file(root, "Season 6/Solsidan S06E01.mp4");
    put_file(root, "Series/solsidan/s1/Solsidan COMPLETE S01-S05 720p x265 HEVC-Lund/2/Solsidan S02E01.web.mp4");
    put_file(root, "Series/solsidan/s1/Solsidan COMPLETE S01-S05 720p x265 HEVC-Lund/2/Solsidan S02E02.web.mp4");
    put_file(root, "Solsidan.S08.SWEDiSH.720p.WEBRip.AAC.2.0.AV1-WoKE/Solsidan.S08E01.SWEDiSH.720p.WEBRip.AAC.2.0.AV1-WoKE.mkv");
    put_file(root, "Arthur/Season 1/Arthur - S01E01.web.mp4");
    setting_set("media.roots", root);
    media_scan_sync();
    static const char *const rels[] = {
        "7/Solsidan S07E02 2021 svensk text/Solsidan S07E02 2021 svensk text.mkv",
        "Arthur - S02E01.mkv",
    };
    UpSorted *s = sort_these(root, rels, 2, 0, NULL, 0);
    CHECK(s[0].known);
    CHECK_STR(s[0].dest, "Series/solsidan/Season 7/Solsidan S07E02 2021 svensk text.mkv");
    CHECK_STR(s[1].dest, "Arthur/Season 2/Arthur - S02E01.mkv");
    free(s);
    /* "Add episodes" on the series' page goes there too */
    const MediaSeries *ms = media_series(upload_show_key("Solsidan"));
    CHECK(ms != NULL);
    s = sort_these(root, rels, 1, ms ? ms->id : 0, NULL, 0);
    CHECK_STR(s[0].dest, "Series/solsidan/Season 7/Solsidan S07E02 2021 svensk text.mkv");
    free(s);
    /* only the copy at the top level (no folder names the show): a new folder of its own, not the top level */
    char root2[512];
    str_copy(root2, sizeof root2, test_tmpdir());
    put_file(root2, "Solsidan S01E01.web.mp4");
    put_file(root2, "Series/Arthur/Season 1/Arthur - S01E01.web.mp4");
    setting_set("media.roots", root2);
    media_scan_sync();
    s = sort_these(root2, rels, 1, 0, NULL, 0);
    CHECK(!s[0].known);
    CHECK_STR(s[0].dest, "Series/Solsidan/Season 7/Solsidan S07E02 2021 svensk text.mkv");
    free(s);
    th_halt();
}

/* A whole long-running show with its subtitles (the plan's own limit is 5000 files): every one placed, and
   quick - the preview asks again whenever an option changes, on the event loop. */
TEST(upload_sorts_a_big_folder_quickly) {
    th_boot();
    char root[512];
    str_copy(root, sizeof root, test_tmpdir());
    enum { N = 4000 };
    char **rels = xcalloc(N, sizeof *rels);
    for (int i = 0; i < N; i++) {
        char rel[200];
        int ep = i / 2 + 1, season = (ep - 1) / 100 + 1;
        snprintf(rel, sizeof rel, "Long Show/Season %d/Long.Show.S%02dE%03d.%s", season, season, ep, i % 2 ? "en.srt" : "mkv");
        rels[i] = xstrdup(rel);
    }
    int64_t t0 = now_ms();
    UpSorted *s = sort_these(root, (const char *const *)rels, N, 0, NULL, 0);
    CHECK(now_ms() - t0 < 5000); /* (63 ms on a PC's debug build; a Pi 3 takes about 20 times as long) */
    CHECK_STR(s[0].dest, "Series/Long Show/Season 1/Long.Show.S01E001.mkv");
    CHECK_STR(s[N - 1].dest, "Series/Long Show/Season 20/Long.Show.S20E2000.en.srt");
    CHECK_INT(s[N - 1].kind, UPK_SUB);
    for (int i = 0; i < N; i++) free(rels[i]);
    free(rels);
    free(s);
    th_halt();
}

TEST(upload_names_folders_safely) {
    char out[200];
    CHECK(upload_safe_name("Star Wars: A New Hope (1977)", out, sizeof out));
    CHECK_STR(out, "Star Wars - A New Hope (1977)");
    CHECK(upload_safe_name("  What? / Why*  ", out, sizeof out));
    CHECK_STR(out, "What Why");
    CHECK(upload_safe_name("Mr. Robot...", out, sizeof out));
    CHECK_STR(out, "Mr. Robot");
    CHECK(upload_safe_name("Så <ska> det \"låta\"", out, sizeof out));
    CHECK_STR(out, "Så ska det låta");
    CHECK(!upload_safe_name(" ... ", out, sizeof out));
    CHECK(upload_safe_name("..hidden", out, sizeof out)); /* (a dot first: a folder the library never looks in) */
    CHECK_STR(out, "hidden");
    CHECK(!upload_safe_name("", out, sizeof out));
    CHECK_INT(upload_show_key("70's Show"), upload_show_key("70'S SHOW"));
    CHECK(upload_show_key("Friends") != upload_show_key("Fiends"));
}
