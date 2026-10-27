#include "../../tests/test.h"
#include "mediainfo.h"

typedef struct {
    const char *path;
    MediaKind kind;
    const char *title;
    int year, season, episode, episode_end;
    const char *ep_title;
} NameCase;

#define EP(path, title, year, s, e, ee, ept) {path, MEDIA_EPISODE, title, year, s, e, ee, ept}
#define MOVIE(path, title, year) {path, MEDIA_MOVIE, title, year, 0, 0, 0, ""}
#define OTHER(path, title, year) {path, MEDIA_OTHER, title, year, 0, 0, 0, ""}

static const NameCase CASES[] = {
    /* --- episodes: explicit markers --- */
    EP("Series/The Office (US)/Season 02/The.Office.US.S02E05.Halloween.720p.WEB-DL.x264.mkv", "The Office (US)", 0, 2,
       5, 5, "Halloween"),
    EP("The.Office.US.S02E05.Halloween.720p.WEB-DL.x264.mkv", "The Office US", 0, 2, 5, 5, "Halloween"),
    EP("Game.of.Thrones.S08E06.1080p.WEB.H264-MEMENTO.mkv", "Game of Thrones", 0, 8, 6, 6, ""),
    EP("game.of.thrones.s01e01.winter.is.coming.mkv", "Game of Thrones", 0, 1, 1, 1, "Winter Is Coming"),
    EP("Friends - 1x05 - The One with the East German Laundry Detergent.avi", "Friends", 0, 1, 5, 5,
       "The One with the East German Laundry Detergent"),
    EP("Breaking Bad S05E14 Ozymandias.mkv", "Breaking Bad", 0, 5, 14, 14, "Ozymandias"),
    EP("Doctor.Who.2005.S01E01.Rose.mkv", "Doctor Who", 2005, 1, 1, 1, "Rose"),
    EP("Show.Name.S01E01E02.720p.mkv", "Show Name", 0, 1, 1, 2, ""),
    EP("Show.Name.S01E01-E02.mkv", "Show Name", 0, 1, 1, 2, ""),
    EP("Show.Name.S01E01-02.mkv", "Show Name", 0, 1, 1, 2, ""),
    EP("Show.Name.S01E01.E02.mkv", "Show Name", 0, 1, 1, 2, ""),
    /* --- a whole show in one release: its range of seasons is no part of the title (1.3) --- */
    EP("Series/The Simpsons S01-S31/Season 31/The.Simpsons.S31E01.The.Winter.of.Our.Monetized.Content.1080p.WEB-DL.10bit."
       "x265.HEVC-PHOCiS.mkv",
       "The Simpsons", 0, 31, 1, 1, "The Winter of Our Monetized Content"),
    EP("The.Simpsons.S01-S31.1080p.WEB-DL.BluRay.10bit.x265.HEVC-PHOCiS/Season 13/The.Simpsons.S13E01.Treehouse.of.Horror."
       "XII.1080p.BluRay.10bit.x265.HEVC-PHOCiS.mkv",
       "The Simpsons", 0, 13, 1, 1, "Treehouse of Horror XII"),
    EP("Seinfeld Seasons 1-9/Season 2/02 - The Pony Remark.mkv", "Seinfeld", 0, 2, 2, 2, "The Pony Remark"),
    EP("Frasier.S1-11.DVDRip/Season 3/Frasier.S03E04.mkv", "Frasier", 0, 3, 4, 4, ""),
    EP("Spaced Season 1-2/Season 1/Spaced.S01E01.mkv", "Spaced", 0, 1, 1, 1, ""),
    EP("Show.Name.S01.E03.mkv", "Show Name", 0, 1, 3, 3, ""),
    EP("show.name.s1e2.mkv", "Show Name", 0, 1, 2, 2, ""),
    EP("Show - Season 1 Episode 3 - Title.mkv", "Show", 0, 1, 3, 3, "Title"),
    /* two (or more) episodes in one file, however the name says it */
    EP("Arthur/Arthur - S01E03 & E04 - Arthur's Baby + D.W.'s Baby SDTV.mkv", "Arthur", 0, 1, 3, 4, "Arthur's Baby + D.W.'s Baby"),
    EP("Arthur/Arthur - S01E01+E02 - A.mkv", "Arthur", 0, 1, 1, 2, "A"),
    EP("Show/Show - S01E01 and S01E02.mkv", "Show", 0, 1, 1, 2, ""),
    EP("Show/Season 1/Show - 01-02 - Two Tales.mkv", "Show", 0, 1, 1, 2, "Two Tales"),
    EP("Show/Show - S01E01-E03 - Three.mkv", "Show", 0, 1, 1, 3, "Three"),
    EP("Show/Show - S01E04 - E10 Is Not An Episode.mkv", "Show", 0, 1, 4, 4, "E10 Is Not An Episode"),
    EP("The.Mandalorian.S02E08.Chapter.16.The.Rescue.1080p.DSNP.WEB-DL.DDP5.1.H.264-NTb.mkv", "The Mandalorian", 0,
       2, 8, 8, "Chapter 16 The Rescue"),
    EP("Marvels.Agents.of.S.H.I.E.L.D.S01E01.mkv", "Marvels Agents of S.H.I.E.L.D.", 0, 1, 1, 1, ""),
    EP("Series/Twin Peaks/Specials/Twin.Peaks.S00E01.Pilot.mkv", "Twin Peaks", 0, 0, 1, 1, "Pilot"),
    EP("Series/Friends/Season 1/Friends.1x01.The.One.Where.Monica.Gets.a.Roommate.mkv", "Friends", 0, 1, 1, 1,
       "The One Where Monica Gets a Roommate"),
    EP("Series/Doctor Who (2005)/Season 1/Doctor.Who.S01E01.mkv", "Doctor Who", 2005, 1, 1, 1, ""),
    /* --- episodes: folder context --- */
    EP("Series/Bluey/Season 1/05 - Hospital.mp4", "Bluey", 0, 1, 5, 5, "Hospital"),
    EP("Series/Bluey/Season 2/S02E03.mp4", "Bluey", 0, 2, 3, 3, ""),
    EP("Series/Breaking Bad/Season 1/01.mkv", "Breaking Bad", 0, 1, 1, 1, ""),
    EP("Series/Planet Earth/Season 1/E03 Mountains.mkv", "Planet Earth", 0, 1, 3, 3, "Mountains"),
    EP("Serier/Bron/Säsong 1/Bron.S01E01.mkv", "Bron", 0, 1, 1, 1, ""),
    EP("Serier/Bron/Säsong 2 Avsnitt 4.mkv", "Bron", 0, 2, 4, 4, ""),
    EP("Serier/Pippi Långstrump/Avsnitt 3 - Pippi går i affärer.mp4", "Pippi Långstrump", 0, 1, 3, 3,
       "Pippi går i affärer"),
    EP("[SubsPlease] Frieren - 05 (1080p) [ABCD1234].mkv", "Frieren", 0, 1, 5, 5, ""),
    EP("[Group] Show Name - 12v2 [720p].mkv", "Show Name", 0, 1, 12, 12, ""),
    EP("Anime/Cowboy Bebop/Cowboy Bebop - 05 - Ballad of Fallen Angels.mkv", "Cowboy Bebop", 0, 1, 5, 5,
       "Ballad of Fallen Angels"),
    EP("Series\\Bluey\\Season 1\\05 - Hospital.mp4", "Bluey", 0, 1, 5, 5, "Hospital"), /* Windows separators */
    /* --- scene tags right after the marker never become episode titles --- */
    EP("Serier/Solsidan/Säsong 3/Solsidan.S03E02.SWEDiSH.720p.HDTV.x264.mkv", "Solsidan", 0, 3, 2, 2, ""),
    EP("Solsidan.S03E02.SWEDISH.720p.HDTV.x264.mkv", "Solsidan", 0, 3, 2, 2, ""),
    EP("Show.S01E01.PROPER.720p.mkv", "Show", 0, 1, 1, 1, ""),
    EP("Show.S01E01.WEB.h264-GRP.mkv", "Show", 0, 1, 1, 1, ""),
    EP("Show.S01E01.Pilot.NORDiC.1080p.mkv", "Show", 0, 1, 1, 1, "Pilot"),
    /* --- episodes: which folder names the show --- */
    EP("Downloads/The.Wire.S03E07.Back.Burners.mkv", "The Wire", 0, 3, 7, 7, "Back Burners"),
    EP("Torrent Stuff/The.Wire.S03E07.mkv", "The Wire", 0, 3, 7, 7, ""),
    EP("The.Office.US.S02E05.720p-GRP/abc123.mkv", "The Office US", 0, 2, 5, 5, ""),
    EP("The.Office.US.S02.720p.WEB-DL.x264-GRP/The.Office.US.S02E01.Dundies.mkv", "The Office US", 0, 2, 1, 1,
       "Dundies"),
    EP("Series/S02E05.mkv", "Unknown series", 0, 2, 5, 5, ""),
    /* --- disc rips: the season folder makes it an episode; the library numbers it --- */
    EP("70's show/season 1 70's show/Disc - 1/B1_t01.mkv", "70's Show", 0, 1, 0, 0, ""),
    EP("70's show/season 6 70's show/Disc - 2/E1_t02.mkv", "70's Show", 0, 6, 0, 0, ""), /* title 2, not "E1" */
    EP("70's show/season 4 70's show/That.70s.Show.S04E01.720p.BluRay.ReEnc-DeeJayAhmed.mkv", "70's Show", 0, 4, 1, 1,
       ""),
    EP("Series/Friends/Season 1 Disc 2/title_t01.mkv", "Friends", 0, 1, 0, 0, ""),
    EP("Series/Friends/Disc 2/title_t01.mkv", "Friends", 0, 1, 0, 0, ""),
    EP("Series/Friends/Season 1/Season 1 Disc 2/title_t01.mkv", "Friends", 0, 1, 0, 0, ""),
    EP("Series/Bluey/Season 1/Hospital.mp4", "Bluey", 0, 1, 0, 0, "Hospital"),
    EP("Random Stuff/That.70s.Show.S01.DVDRip-GRP/That.70s.Show.S01E01.mkv", "That 70s Show", 0, 1, 1, 1, ""),
    EP("Random Stuff/That.70s.Show.S01.DVDRip-GRP/title_t03.mkv", "That 70s Show", 0, 1, 0, 0, ""),
    EP("TV/Doctor Who Series 3/Doctor.Who.S03E01.mkv", "Doctor Who", 0, 3, 1, 1, ""),
    /* --- movies --- */
    MOVIE("Movies/Inception (2010)/Inception (2010).mkv", "Inception", 2010),
    MOVIE("Inception.2010.1080p.BluRay.x264-GRP.mkv", "Inception", 2010),
    MOVIE("Movies/Inception (2010)/movie.mkv", "Inception", 2010),
    MOVIE("Movies/Inception (2010)/title_t00.mkv", "Inception", 2010),
    MOVIE("Movies/Inception (2010)/Disc 1/title_t00.mkv", "Inception", 2010), /* a two-disc movie */
    MOVIE("Movies/Inception (2010)/Inception.mkv", "Inception", 2010),
    MOVIE("Inception.2010.1080p.BluRay.x264-GRP/grp-inception.mkv", "Inception", 2010),
    MOVIE("Spirited Away (2001).mkv", "Spirited Away", 2001),
    MOVIE("Movies/Spirited Away.mkv", "Spirited Away", 0),
    MOVIE("1917 (2019).mkv", "1917", 2019),
    MOVIE("2001 A Space Odyssey (1968).mkv", "2001 A Space Odyssey", 1968),
    MOVIE("Blade.Runner.2049.2017.1080p.BluRay.mkv", "Blade Runner 2049", 2017),
    MOVIE("Wonder.Woman.1984.2020.1080p.WEBRip.mkv", "Wonder Woman 1984", 2020),
    MOVIE("Apollo.13.1995.REMASTERED.1080p.mkv", "Apollo 13", 1995),
    MOVIE("Filmer/Så som i himmelen (2004).mkv", "Så som i himmelen", 2004),
    MOVIE("så.som.i.himmelen.2004.mkv", "Så Som I Himmelen", 2004),
    MOVIE("Fucking.Åmål.1998.SWEDiSH.DVDRip.XviD.avi", "Fucking Åmål", 1998),
    MOVIE("THE.MATRIX.1999.1080P.BLURAY.mkv", "The Matrix", 1999),
    MOVIE("Movies/Harry.Potter.and.the.Deathly.Hallows.Part.1.2010.1080p.mkv",
          "Harry Potter and the Deathly Hallows Part 1", 2010),
    MOVIE("A.I.Artificial.Intelligence.2001.mkv", "A.I. Artificial Intelligence", 2001),
    MOVIE("Spider-Man.Into.the.Spider-Verse.2018.mkv", "Spider-Man Into the Spider-Verse", 2018),
    MOVIE("Inception.1080p.BluRay.x264.mkv", "Inception", 0),
    MOVIE("www.UIndex.org - Dune.Part.Two.2024.1080p.mkv", "Dune Part Two", 2024),
    MOVIE("Crouching.Tiger,Hidden.Dragon.2000.mkv", "Crouching Tiger, Hidden Dragon", 2000),
    MOVIE("Dual.2022.1080p.mkv", "Dual", 2022),
    MOVIE("Movies/Toy Story (1995) [1080p]/Toy.Story.mkv", "Toy Story", 1995),
    MOVIE("The Shining (1980) (Extended Cut).mkv", "The Shining", 1980),
    MOVIE("The.Danish.Girl.2015.1080p.mkv", "The Danish Girl", 2015),
    MOVIE("Den.Allvarsamma.Leken.SWEDiSH.2016.DVDRip.mkv", "Den Allvarsamma Leken", 2016),
    MOVIE("Movies/Arrival.LIMITED.2016.1080p.mkv", "Arrival", 2016),
    MOVIE("The.Lord.of.the.Rings.The.Fellowship.of.the.Ring.2001.EXTENDED.2160p.UHD.BluRay.x265-GRP.mkv",
          "The Lord of the Rings The Fellowship of the Ring", 2001),
    MOVIE("Star.Wars.Episode.IV.A.New.Hope.1977.1080p.mkv", "Star Wars Episode IV A New Hope", 1977),
    MOVIE("Movies/Mission Impossible - Fallout (2018).mkv", "Mission Impossible - Fallout", 2018),
    MOVIE("Movies/WALL-E (2008).mkv", "WALL-E", 2008),
    MOVIE("Movies/Amélie (2001).mkv", "Amélie", 2001),
    /* --- other stuff --- */
    OTHER("Home Videos/Christmas 2019.mp4", "Christmas 2019", 2019),
    OTHER("Home Videos/Midsommar/IMG_1234.MOV", "IMG 1234", 0),
    OTHER("Spirited.Away.mkv", "Spirited Away", 0),
    OTHER("random clip.mp4", "Random Clip", 0),
    OTHER("Hemvideor/Semester i Skåne.mp4", "Semester i Skåne", 0),
};

TEST(media_names_table) {
    for (int i = 0; i < countof(CASES); i++) {
        const NameCase *c = &CASES[i];
        MediaName n;
        media_parse_name(c->path, &n);
        if (n.kind != c->kind || strcmp(n.title, c->title) != 0 || n.year != c->year ||
            (c->kind == MEDIA_EPISODE && (n.season != c->season || n.episode != c->episode ||
                                          n.episode_end != c->episode_end || strcmp(n.ep_title, c->ep_title) != 0))) {
            test_fail(__FILE__, __LINE__,
                      "%s\n      got:      kind=%d \"%s\" (%d) S%dE%d-%d \"%s\"\n      expected: kind=%d \"%s\" (%d) "
                      "S%dE%d-%d \"%s\"",
                      c->path, n.kind, n.title, n.year, n.season, n.episode, n.episode_end, n.ep_title, c->kind,
                      c->title, c->year, c->season, c->episode, c->episode_end, c->ep_title);
        }
        CHECK(!n.extra);
    }
}

TEST(media_names_disc_rips_know_their_disc_and_title) {
    MediaName n;
    media_parse_name("70's show/season 1 70's show/Disc - 4/C1_t07.mkv", &n);
    CHECK(n.unnumbered && n.disc == 4 && n.track == 7 && n.season == 1);
    media_parse_name("Show/Season 2/S02E05.mkv", &n);
    CHECK(!n.unnumbered && n.disc == 0 && n.track == -1);
    /* the root folder's own name names the show (or season) when rel doesn't */
    media_parse_name_in("70's show", "season 3 70's show/Disc - 3/J1_t01.mkv", &n);
    CHECK_STR(n.title, "70's Show");
    CHECK(n.kind == MEDIA_EPISODE && n.season == 3 && n.disc == 3 && n.track == 1 && n.unnumbered);
    media_parse_name_in("season 2 70's show", "Disc - 4/C3_t06.mkv", &n);
    CHECK_STR(n.title, "70's Show");
    CHECK(n.season == 2 && n.disc == 4 && n.track == 6);
    media_parse_name_in("Inception (2010)", "movie.mkv", &n);
    CHECK(n.kind == MEDIA_MOVIE && n.year == 2010);
    CHECK_STR(n.title, "Inception");
    media_parse_name_in("Series & movies", "Inception.2010.1080p.mkv", &n); /* a plain parent changes nothing */
    CHECK(n.kind == MEDIA_MOVIE && n.year == 2010);
    media_parse_name_in("Series & movies", "The.Wire.S03E07.mkv", &n);
    CHECK_STR(n.title, "The Wire");
    media_parse_name_in("", "70's show/season 1 70's show/Disc - 1/B1_t01.mkv", &n);
    CHECK_STR(n.title, "70's Show");
    /* MakeMKV title numbers */
    media_parse_name("Show/Season 1/B8_t00.mkv", &n);
    CHECK_INT(n.track, 0);
    media_parse_name("Show/Season 1/title_t12.mkv", &n);
    CHECK_INT(n.track, 12);
    media_parse_name("Show/Season 1/title03.mkv", &n);
    CHECK_INT(n.track, 3);
    media_parse_name("Show/Season 1/THAT_70S_SHOW_S1_D1_t04.mkv", &n);
    CHECK_INT(n.track, 4);
}

TEST(media_names_folder_roles) {
    static const struct {
        const char *name;
        int season, disc;
    } F[] = {
        {"season 1 70's show", 1, -1}, {"Season 01", 1, -1},        {"S02", 2, -1},
        {"Specials", 0, -1},           {"Säsong 3", 3, -1},         {"That.70s.Show.S05.DVDRip-GRP", 5, -1},
        {"Doctor Who Series 3", 3, -1}, {"Season1", 1, -1},         {"Disc - 1", -1, 1},
        {"Disc 2", -1, 2},             {"DVD2", -1, 2},             {"Disk 3 of 4", -1, 3},
        {"D1", -1, 1},                 {"Series & movies", -1, -1}, {"Season of the Witch (2011)", -1, -1},
        {"Discovery", -1, -1},         {"DVDRip", -1, -1},          {"Movies", -1, -1},
        {"S1m0ne (2002)", -1, -1},     {"Show.S01E02.720p", -1, -1}, {"Season 1 Disc 2", 1, -1},
        {"The Simpsons S01-S31", -1, -1}, {"Seinfeld Seasons 1-9", -1, -1}, {"Spaced Season 1-2", -1, -1}, /* (a show's folder) */
    };
    for (int i = 0; i < countof(F); i++) {
        int s = media_season_folder(F[i].name), d = media_disc_folder(F[i].name);
        if (s != F[i].season || d != F[i].disc)
            test_fail(__FILE__, __LINE__, "%s: season %d disc %d, expected %d %d", F[i].name, s, d, F[i].season,
                      F[i].disc);
    }
    CHECK_INT(media_season_folder(NULL), -1);
    CHECK_INT(media_disc_folder(NULL), -1);
}

TEST(media_names_extras_are_flagged) {
    static const char *const extras[] = {
        "Movies/Avatar (2009)/Extras/Behind the scenes.mkv",
        "Inception.2010.1080p.BluRay-sample.mkv",
        "Movies/Inception (2010)/inception-trailer.mp4",
        "Movies/Inception (2010)/Featurettes/Dreams.mkv",
        "Show.S01E01.720p/sample.mkv",
        "Movies/Up (2009)/sample-up.mkv",
    };
    for (int i = 0; i < countof(extras); i++) {
        MediaName n;
        media_parse_name(extras[i], &n);
        if (!n.extra || n.kind != MEDIA_OTHER)
            test_fail(__FILE__, __LINE__, "%s: extra=%d kind=%d", extras[i], n.extra, n.kind);
    }
    MediaName n;
    media_parse_name("Movies/Avatar (2009)/Extras/Behind the scenes.mkv", &n);
    CHECK_STR(n.title, "Behind the scenes");
    CHECK_INT(n.year, 2009); /* lets the scanner attach it to the movie */
    CHECK(!n.named && !n.sample); /* (an Extras folder says so for sure: no length asked) */
    /* by name: the scanner asks how long they run (a long one is a real video) */
    static const char *const named[] = {"Movies/Up (2009)/Up - Trailer.mkv", "Movies/Up (2009)/Up_featurette.mkv",
                                        "Up.2009.sample.mkv", "Movies/Up (2009)/trailer.mp4",
                                        "Movies/Up (2009)/inception-trailer.mp4"};
    for (int i = 0; i < countof(named); i++) {
        media_parse_name(named[i], &n);
        if (!n.extra || !n.named || n.sample)
            test_fail(__FILE__, __LINE__, "%s: extra=%d named=%d sample=%d", named[i], n.extra, n.named, n.sample);
    }
    media_parse_name("Movies/Up (2009)/sample-up.mkv", &n);
    CHECK(n.extra && n.sample && !n.named);
}

/* Issue #94: a plain word at the end made a film or an episode an extra, whatever its length. */
TEST(media_names_titles_that_only_end_like_extras) {
    MediaName n;
    media_parse_name("Movies/The Sample (2019)/The Sample.mkv", &n);
    CHECK(!n.extra);
    CHECK_INT(n.kind, MEDIA_MOVIE);
    CHECK_STR(n.title, "The Sample");
    media_parse_name("Movies/The Interview.mp4", &n);
    CHECK(!n.extra);
    CHECK_STR(n.title, "The Interview");
    media_parse_name("Series/Test Show/Season 1/Test Show S01E04 - Crime Scene.mp4", &n);
    CHECK(!n.extra);
    CHECK_INT(n.kind, MEDIA_EPISODE);
    CHECK_INT(n.episode, 4);
    CHECK_STR(n.ep_title, "Crime Scene");
    media_parse_name("Series/Test Show/Season 1/Test Show S01E05 - Scene.mp4", &n); /* an episode code wins */
    CHECK(!n.extra && n.kind == MEDIA_EPISODE && n.episode == 5);
    media_parse_name("Series/Test Show/Extras/Test Show S01E05 - Scene.mp4", &n); /* ... not over an Extras folder */
    CHECK(n.extra);
    media_parse_name("Movies/Short Term 12 (2013)/Short Term 12.mkv", &n);
    CHECK(!n.extra);
    /* the length rule off (a long one): a real video, named as one */
    media_parse_name_long(NULL, "Movies/Up (2009)/Up - Trailer.mkv", &n);
    CHECK(!n.extra && !n.named);
}

TEST(media_names_degenerate_input) {
    MediaName n;
    media_parse_name("", &n);
    CHECK_STR(n.title, "Untitled");
    media_parse_name(NULL, &n);
    CHECK_STR(n.title, "Untitled");
    media_parse_name("///", &n);
    CHECK_STR(n.title, "Untitled");
    media_parse_name("1080p.mkv", &n);
    CHECK(n.title[0] != 0);
    media_parse_name(".mkv", &n);
    CHECK(n.title[0] != 0);
    /* very long and deep paths must not overflow anything */
    Buf b = {0};
    for (int i = 0; i < 80; i++) buf_puts(&b, "Very Long Folder Name With Many Words/");
    for (int i = 0; i < 60; i++) buf_puts(&b, "word.");
    buf_puts(&b, "S01E02.mkv");
    media_parse_name(buf_cstr(&b), &n);
    CHECK_INT(n.kind, MEDIA_EPISODE);
    CHECK_INT(n.episode, 2);
    CHECK(strlen(n.title) < sizeof n.title);
    buf_free(&b);
}

TEST(media_names_no_false_episodes) {
    static const char *const movies[] = {
        "Se7en.1995.1080p.mkv", "Movies/Ocean's Eleven (2001).mkv", "1920x1080 Demo Reel.mp4",
        "Videos/Concert 2019-05-03.mp4", "S1m0ne.2002.mkv",
    };
    for (int i = 0; i < countof(movies); i++) {
        MediaName n;
        media_parse_name(movies[i], &n);
        if (n.kind == MEDIA_EPISODE) test_fail(__FILE__, __LINE__, "%s parsed as an episode", movies[i]);
    }
}

TEST(media_names_film_parts) {
    /* the title leaves these out, so telling CD1 from CD2 needs the part */
    CHECK_INT(media_name_part("Kill Bill (2003)/Kill Bill (2003) CD1.mkv"), 1);
    CHECK_INT(media_name_part("Kill Bill (2003)/Kill.Bill.2003.cd2.1080p.mkv"), 2);
    CHECK_INT(media_name_part("Film (1999) - Part 2.mp4"), 2);
    CHECK_INT(media_name_part("Family Guy (1999) - S08E20 - Something Something Something Dark Side Pt. 2 [DSNP WEBDL-1080p]"
                              "[AAC 2.0][h264].web.mp4"), 2); /* (an episode in two files too - 1.4) */
    CHECK_INT(media_name_part("Family Guy (1999) - S08E20 - Something Something Something Dark Side [DSNP WEBDL-1080p]"
                              "[AAC 2.0][h264]-playWEB.web.mp4"), 0);
    CHECK_INT(media_name_part("Film.1999.pt1.avi"), 1);
    CHECK_INT(media_name_part("Film (1999) Disc 1.mkv"), 1);
    CHECK_INT(media_name_part("Filmen (1999) del 2.mkv"), 2);
    CHECK_INT(media_name_part("Dune (2021) Part One.mkv"), 1);
    CHECK_INT(media_name_part("Kill Bill (2003) CD1.web.mp4"), 1); /* a copy: the same part */
    CHECK_INT(media_name_part("Film (1999).mkv"), 0);
    CHECK_INT(media_name_part("Film.1999.1080p.x264.DTS-HD.mkv"), 0);
    CHECK_INT(media_name_part("CD 2/Film (1999).mkv"), 0); /* folders don't count, only the file's name */
    CHECK_INT(media_name_part("Partisan (2015).mkv"), 0);
    CHECK_INT(media_name_part("Part (2015).mkv"), 0);
}
